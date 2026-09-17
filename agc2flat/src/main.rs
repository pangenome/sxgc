// agc2flat: stream an AGC archive into one flat text file for suffixient-array
// construction, emitting a sidecar mapping from flat byte offsets into the
// sample/contig name space.
//
// Output:
//   <out>.txt          flat text: contig sequences separated by '$' (0x24)
//   <out>.names.tsv    name \t start_offset \t length   (per contig)
//
// Flat offset p maps to (name, p - start) for the unique contig with
// start <= p < start + len; offsets falling on a '$' are contig boundaries.
use anyhow::Result;
use ragc_core::{Decompressor, DecompressorConfig, CNV_NUM};
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: agc2flat <archive.agc> [-o out.txt] [--upper]");
        std::process::exit(1);
    }
    let archive = args[1].clone();
    let mut out_path = String::new();
    let mut upper = false;
    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "-o" => { out_path = args[i + 1].clone(); i += 2; }
            "--upper" => { upper = true; i += 1; }
            a => { eprintln!("unknown arg: {a}"); std::process::exit(1); }
        }
    }
    if out_path.is_empty() {
        out_path = Path::new(&archive)
            .file_stem().unwrap().to_string_lossy().to_string() + ".txt";
    }
    let tsv_path = format!("{out_path}.names.tsv");

    let mut dec = Decompressor::open(&archive, DecompressorConfig::default())?;
    let samples = dec.list_samples();
    eprintln!("opened {archive}: {} samples", samples.len());

    let mut text = BufWriter::with_capacity(1 << 22, File::create(&out_path)?);
    let mut tsv = BufWriter::with_capacity(1 << 20, File::create(&tsv_path)?);
    let mut offset: u64 = 0;
    let mut n_contigs: u64 = 0;

    for (si, s) in samples.iter().enumerate() {
        let contigs = dec.get_sample(s)?;
        for (cname, numeric) in contigs {
            // get_sample returns ragc's numeric encoding (0-15 = ACGT + IUPAC
            // per CNV_NUM); convert to ASCII exactly like write_sample_fasta.
            let seq: Vec<u8> = numeric.iter()
                .map(|&b| if b < 16 { CNV_NUM[b as usize] } else { b'N' })
                .collect();
            // normalize to full PanSN-ish name: sample#contig
            let full = if cname.contains('#') { cname.clone() }
                       else { format!("{s}#{cname}") };
            let start = offset;
            let mut buf = Vec::with_capacity(seq.len());
            for &b in &seq {
                let c = if upper && b.is_ascii_lowercase() { b - 32 } else { b };
                if c == 0 || c == 1 || c == 2 {
                    anyhow::bail!("forbidden byte {c} in {full}");
                }
                buf.push(c);
            }
            text.write_all(&buf)?;
            text.write_all(b"$")?; // separator after every contig
            offset += buf.len() as u64 + 1;
            writeln!(tsv, "{}\t{}\t{}", full, start, buf.len())?;
            n_contigs += 1;
        }
        if (si + 1) % 20 == 0 || si + 1 == samples.len() {
            eprintln!("[{}/{}] samples, {} contigs, offset {}", si + 1, samples.len(), n_contigs, offset);
        }
    }
    text.flush()?;
    tsv.flush()?;
    eprintln!("flat text length (incl. {} separators): {}", n_contigs, offset);
    eprintln!("text: {out_path}");
    eprintln!("names: {tsv_path}");
    Ok(())
}
