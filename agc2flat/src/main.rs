// agc2flat: stream an AGC archive into flat text for suffixient-array (sxgc)
// construction, emitting a sidecar mapping from flat byte offsets into the
// sample/contig name space.
//
// Modes:
//   agc2flat <archive.agc> [-o out.txt] [--upper]
//       whole-collection flat text + <out>.names.tsv
//   agc2flat <archive.agc> --groups
//       list contig groups (contig name \t #copies \t total bytes) — no text
//   agc2flat <archive.agc> --group <contig> [-o out.txt] [--band S:E] [--upper]
//       stream ONE contig group (all samples' copies, sample order) to
//       out.txt + out.names.tsv; --band S:E slices [S,E) of every copy
//       (AGC-native sharded construction: extract -> build -> delete)
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

    // group contigs by contig part across samples; sizes come from the
    // collection metadata (no text pass)
    if args.groups {
        let mut groups: BTreeMap<String, (u64, u64)> = BTreeMap::new();
        for s in &samples {
            for (cname, seq) in dec.get_sample(s)? {
                let e = groups.entry(contig_part(s, &cname)).or_insert((0, 0));
                e.0 += 1;
                e.1 += seq.len() as u64;
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
            for (cname, numeric) in dec.get_sample(s)? {
                if contig_part(s, &cname) != *contig { continue; }
                let seq = ascii_of(&numeric, args.upper)?;
                let (start_in_seq, take) = match (bs, be) {
                    (0, u64::MAX) => (0usize, seq.len()),
                    _ => {
                        let s0 = (bs as usize).min(seq.len());
                        let e0 = (be as usize).min(seq.len());
                        (s0, e0.saturating_sub(s0))
                    }
                };
                if take == 0 { continue; }
                let slice = &seq[start_in_seq..start_in_seq + take];
                text.write_all(slice)?;
                text.write_all(b"$")?;
                writeln!(tsv, "{}\t{}\t{}", full_name(s, &cname), offset, slice.len())?;
                offset += slice.len() as u64 + 1;
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
        for (cname, numeric) in dec.get_sample(s)? {
            let seq = ascii_of(&numeric, args.upper)?;
            let full = full_name(s, &cname);
            let start = offset;
            text.write_all(&seq)?;
            text.write_all(b"$")?;
            offset += seq.len() as u64 + 1;
            writeln!(tsv, "{full}\t{start}\t{}", seq.len())?;
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

fn contig_part(sample: &str, cname: &str) -> String {
    // collection contig names may already embed the sample (PanSN); normalize
    // to the group key: the contig part after any embedded sample prefix
    if cname.starts_with(sample) { cname[sample.len()..].trim_start_matches('#').to_string() }
    else { cname.to_string() }
}

fn full_name(sample: &str, cname: &str) -> String {
    if cname.contains('#') && cname.starts_with(sample) { cname.to_string() }
    else if cname.contains('#') { cname.to_string() }
    else { format!("{sample}#{cname}") }
}
