//! ragc-ffi — C ABI over ragc-core for the sxgc AGC text oracle (Bit 5).
//!
//! Exports (linked from the C++ `agc_text_oracle`):
//!   sxgc_agc_open(path)            -> handle (builds stored-cname -> sample map)
//!   sxgc_agc_len(handle, cname)    -> contig length (0 if unknown)
//!   sxgc_agc_range(h, cname, s, e, out) -> bytes written (-1 on error)
//!   sxgc_agc_close(handle)
//!
//! `cname` is the STORED contig name (e.g. "HG02647#1#CM086560.1") — the
//! handle resolves it to the AGC sample name via metadata at open time.
use ragc_core::{Decompressor, DecompressorConfig, CNV_NUM};
use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

pub struct AgcHandle {
    dec: Decompressor,
    /// stored contig name -> AGC sample name
    cname_sample: HashMap<String, String>,
}

#[no_mangle]
pub extern "C" fn sxgc_agc_open(path: *const c_char) -> *mut AgcHandle {
    unsafe {
        let path = CStr::from_ptr(path).to_string_lossy().to_string();
        let res = (|| -> anyhow::Result<AgcHandle> {
            let mut dec = Decompressor::open(&path, DecompressorConfig::default())?;
            let samples = dec.list_samples();
            let mut cname_sample = HashMap::new();
            for s in &samples {
                for cname in dec.list_contigs(s)? {
                    cname_sample.insert(cname, s.clone());
                }
            }
            Ok(AgcHandle { dec, cname_sample })
        })();
        match res {
            Ok(h) => Box::into_raw(Box::new(h)),
            Err(_) => std::ptr::null_mut(),
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn sxgc_agc_len(h: *mut AgcHandle, cname: *const c_char) -> u64 {
    let h = match h.as_mut() {
        Some(h) => h,
        None => return 0,
    };
    let cname = CStr::from_ptr(cname).to_string_lossy().to_string();
    let sample = match h.cname_sample.get(&cname) {
        Some(s) => s.clone(),
        None => return 0,
    };
    h.dec.get_contig_length(&sample, &cname).unwrap_or(0) as u64
}

/// reads [start, end) of the stored contig into `out` (capacity >= end-start).
/// returns bytes written, or -1 on error.
#[no_mangle]
pub unsafe extern "C" fn sxgc_agc_range(
    h: *mut AgcHandle,
    cname: *const c_char,
    start: u64,
    end: u64,
    out: *mut u8,
) -> c_int {
    let h = match h.as_mut() {
        Some(h) => h,
        None => return -1,
    };
    let cname = CStr::from_ptr(cname).to_string_lossy().to_string();
    let sample = match h.cname_sample.get(&cname) {
        Some(s) => s.clone(),
        None => return -1,
    };
    if end <= start || out.is_null() {
        return -1;
    }
    let res = h.dec.get_contig_range(&sample, &cname, start as usize, end as usize);
    match res {
        Ok(v) => {
            // ragc returns the numeric encoding (0-15 = ACGT+IUPAC per CNV_NUM);
            // convert to ASCII exactly like agc2flat/write_sample_fasta
            let out_slice = std::slice::from_raw_parts_mut(out, v.len());
            for (i, &b) in v.iter().enumerate() {
                out_slice[i] = if b < 16 { CNV_NUM[b as usize] } else { b'N' };
            }
            v.len() as c_int
        }
        Err(_) => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn sxgc_agc_close(h: *mut AgcHandle) {
    if !h.is_null() {
        let mut dec = Box::from_raw(h).dec;
        let _ = dec.close();
    }
}
