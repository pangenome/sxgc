// agc2flat: stream an AGC archive into flat text for suffixient-array (sxgc)
// construction, emitting a sidecar mapping from flat byte offsets into the
// sample/contig name space.
//
// Modes:
//   agc2flat <archive.agc> [-o out.txt] [--upper]
//       whole-collection flat text + <out>.names.tsv
//   agc2flat <archive.agc> --groups
//       list contig groups (contig \t #copies \t total bytes) — METADATA ONLY
//   agc2flat <archive.agc> --group <contig> [-o out.txt] [--band S:E] [--upper]
//       stream ONE contig group (all samples' copies, sample order) to
//       out.txt + out.names.tsv; --band S:E slices [S,E) of every copy
//       (AGC-native sharded construction: extract -> build -> delete)
//
// Uses targeted per-contig extraction (get_contig / get_contig_range) —
// never get_sample (which decompresses every contig of a sample at once).
use anyhow::{Context, Result};
use ragc_core::{Decompressor, DecompressorConfig, CNV_NUM};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

fn ascii_of(numeric: &[u8], upper: bool) -> Result<Vec<u8>> {
    let mut v = Vec::with_capacity(numeric.len());
    for &b in numeric {
        let c = if b < 16 { CNV_NUM[b as usize] } else { b'N' };
        if c == 0 || c == 1 || c == 2 {
            anyhow::bail!("forbidden byte {c} after conversion");
        }
        v.push(if upper && c.is_ascii_lowercase() { c - 32 } else { c });
    }
    Ok(v)
}

/// contig group key: the part after the LAST '#' of the stored contig name
/// (PanSN names may be sample#contig or sample#hap#contig).
fn group_key(cname: &str) -> &str {
    match cname.rfind('#') {
        Some(i) => &cname[i + 1..],
        None => cname,
    }
}

struct Args {
    archive: String,
    out: String,
    upper: bool,
    groups: bool,
    group: Option<String>,
    band: Option<(u64, u64)>,
}

fn parse_args() -> Result<Args> {
    let mut a = Args { archive: String::new(), out: String::new(), upper: false, groups: false, group: None, band: None };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "-o" => a.out = it.next().context("-o needs a value")?,
            "--upper" => a.upper = true,
            "--groups" => a.groups = true,
            "--group" => a.group = Some(it.next().context("--group needs a contig name")?),
            "--band" => {
                let v = it.next().context("--band needs S:E")?;
                let (s, e) = v.split_once(':').context("--band format S:E")?;
                a.band = Some((s.parse()?, e.parse()?));
            }
            other if a.archive.is_empty() => a.archive = other.to_string(),
            other => anyhow::bail!("unknown arg: {other}"),
        }
    }
    if a.archive.is_empty() {
        anyhow::bail!("usage: agc2flat <archive.agc> [-o out.txt] [--upper] [--groups] [--group <contig>] [--band S:E]");
    }
    Ok(a)
}

fn main() -> Result<()> {
    let args = parse_args()?;
    let mut dec = Decompressor::open(&args.archive, DecompressorConfig::default())?;
    let samples = dec.list_samples();

    // --groups: metadata only (list_contigs + get_contig_length — no decompression)
    if args.groups {
        let mut groups: BTreeMap<String, (u64, u64)> = BTreeMap::new();
        for s in &samples {
            let names = dec.list_contigs(s)?;
            for cname in &names {
                let len = dec.get_contig_length(s, cname)? as u64;
                let e = groups.entry(group_key(&cname).to_string()).or_insert((0, 0));
                e.0 += 1;
                e.1 += len;
            }
        }
        for (g, (copies, bytes)) in groups {
            println!("{g}\t{copies}\t{bytes}");
        }
        return Ok(());
    }

    if let Some(contig) = &args.group {
        let mut out_path = args.out.clone();
        if out_path.is_empty() { out_path = format!("{contig}.txt"); }
        let tsv_path = format!("{contig}.names.tsv");
        let mut text = BufWriter::with_capacity(1 << 22, File::create(&out_path)?);
        let mut tsv = BufWriter::with_capacity(1 << 16, File::create(&tsv_path)?);
        let mut offset: u64 = 0;
        let mut copies: u64 = 0;
        let (bs, be) = args.band.unwrap_or((0, u64::MAX));
        anyhow::ensure!(bs <= be, "--band S:E with S > E");
        for s in &samples {
            let names = dec.list_contigs(s)?;
            for cname in &names {
                if group_key(cname) != contig.as_str() { continue; }
                let numeric = if (bs, be) == (0, u64::MAX) {
                    dec.get_contig(s, cname)?
                } else {
                    let s0 = (bs as usize).min(be as usize);
                    let clen = dec.get_contig_length(s, cname)?;
                    dec.get_contig_range(s, cname, s0, (be as usize).min(clen))?
                };
                let seq = ascii_of(&numeric, args.upper)?;
                if seq.is_empty() { continue; }
                text.write_all(&seq)?;
                text.write_all(b"$")?;
                writeln!(tsv, "{}\t{}\t{}", cname, offset, seq.len())?;
                offset += seq.len() as u64 + 1;
                copies += 1;
            }
        }
        text.flush()?; tsv.flush()?;
        eprintln!("group {contig}: {copies} copies, shard length {offset} (band {:?})", args.band);
        eprintln!("text: {out_path}\nnames: {tsv_path}");
        return Ok(());
    }

    // whole-collection mode (validated on yeast235)
    let mut out_path = args.out.clone();
    if out_path.is_empty() {
        out_path = Path::new(&args.archive).file_stem().unwrap().to_string_lossy().to_string() + ".txt";
    }
    let tsv_path = format!("{out_path}.names.tsv");
    let mut text = BufWriter::with_capacity(1 << 22, File::create(&out_path)?);
    let mut tsv = BufWriter::with_capacity(1 << 16, File::create(&tsv_path)?);
    let mut offset: u64 = 0;
    let mut n_contigs: u64 = 0;
    for (si, s) in samples.iter().enumerate() {
        let names = dec.list_contigs(s)?;
        for cname in &names {
            let numeric = dec.get_contig(s, cname)?;
            let seq = ascii_of(&numeric, args.upper)?;
            let start = offset;
            text.write_all(&seq)?;
            text.write_all(b"$")?;
            offset += seq.len() as u64 + 1;
            writeln!(tsv, "{cname}\t{start}\t{}", seq.len())?;
            n_contigs += 1;
        }
        if (si + 1) % 20 == 0 || si + 1 == samples.len() {
            eprintln!("[{}/{}] samples, {n_contigs} contigs, offset {offset}", si + 1, samples.len());
        }
    }
    text.flush()?; tsv.flush()?;
    eprintln!("flat text length (incl. {n_contigs} separators): {offset}\ntext: {out_path}\nnames: {tsv_path}");
    Ok(())
}
