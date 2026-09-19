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
//   agc2flat <archive.agc> [--samples <file>] [--reverse] [--stdout] ...
//       --samples restricts ALL modes to the listed AGC sample names
//       (one per line, '#' starts a comment); unknown names abort loudly;
//       archive order is preserved so a subset stream is a subsequence of
//       the whole-collection stream.
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
    reverse: bool,
    stdout: bool,
    samples: Option<String>,
}

fn parse_args() -> Result<Args> {
    let mut a = Args { archive: String::new(), out: String::new(), upper: false, groups: false, group: None, band: None, reverse: false, stdout: false, samples: None };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "-o" => a.out = it.next().context("-o needs a value")?,
            "--upper" => a.upper = true,
            "--reverse" => a.reverse = true,
            "--stdout" => a.stdout = true,
            "--samples" => a.samples = Some(it.next().context("--samples needs a file of sample names")?),
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
        anyhow::bail!("usage: agc2flat <archive.agc> [-o out.txt] [--upper] [--groups] [--group <contig>] [--band S:E] [--samples <file>] [--reverse] [--stdout]");
    }
    Ok(a)
}

fn main() -> Result<()> {
    let args = parse_args()?;
    let mut dec = Decompressor::open(&args.archive, DecompressorConfig::default())?;
    let all_samples = dec.list_samples();

    // --samples: restrict to the listed AGC sample names (archive order kept,
    // unknown names abort). Applies to every mode.
    let samples: Vec<String> = match &args.samples {
        Some(path) => {
            let text = std::fs::read_to_string(path)
                .with_context(|| format!("--samples: cannot read {path}"))?;
            let wanted: std::collections::BTreeSet<String> = text
                .lines()
                .map(|l| l.trim())
                .filter(|l| !l.is_empty() && !l.starts_with('#')) // '#' ONLY starts a comment; sample names contain '#' (PanSN)
                .map(|l| l.to_string())
                .collect();
            anyhow::ensure!(!wanted.is_empty(), "--samples: file has no names");
            let have: std::collections::BTreeSet<String> = all_samples.iter().cloned().collect();
            for w in &wanted {
                anyhow::ensure!(have.contains(w), "--samples: sample not in archive: {w}");
            }
            let filtered: Vec<String> = all_samples
                .into_iter()
                .filter(|s| wanted.contains(s))
                .collect();
            eprintln!("--samples: {}/{} selected from {path}", filtered.len(), have.len());
            filtered
        }
        None => all_samples,
    };

    // --groups: metadata only (list_contigs + get_contig_length — no decompression)
    if args.groups {
        let mut groups: BTreeMap<String, (u64, u64)> = BTreeMap::new();
        for s in &samples {
            let names = dec.list_contigs(s)?;
            for cname in &names {
                let len = dec.get_contig_length(s, cname)? as u64;
                if std::env::var("SXGC_RAW_NAMES").is_ok() {
                    println!("{s}\t{cname}\t{len}");
                    continue;
                }
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
        let tsv_base = out_path.strip_suffix(".txt").unwrap_or(out_path.as_str());
        let tsv_path = format!("{tsv_base}.names.tsv");
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
    // --reverse --stdout: stream the byte-exact mirror of the forward flat text
    // ('$' + reversed contig, last contig first) so pscan -S can parse the
    // reversed text without any materialization. Sidecar keeps FORWARD offsets.
    if args.reverse {
        // stream the byte-exact mirror of the forward flat text ('$' + reversed
        // contig, last contig first). Sidecar written at EOF from ACTUAL stream
        // positions (metadata lengths can disagree with decompressed bytes).
        let mut out_path = args.out.clone();
        if out_path.is_empty() {
            out_path = Path::new(&args.archive).file_stem().unwrap().to_string_lossy().to_string() + ".txt";
        }
        let tsv_path = format!("{out_path}.names.tsv");
        let mut stdout_out: Option<BufWriter<std::io::Stdout>> = None;
        let mut file_out: Option<BufWriter<File>> = None;
        if args.stdout {
            use std::io::Write as _;
            stdout_out = Some(BufWriter::with_capacity(1 << 22, std::io::stdout()));
        } else {
            file_out = Some(BufWriter::with_capacity(1 << 22, File::create(&out_path)?));
        }
        let mut n_contigs: u64 = 0;
        let mut streamed: u64 = 0;
        let mut rows: Vec<(String, u64, u64)> = Vec::new(); // (cname, stream_off, len)
        let mut samples_rev = samples.clone();
        samples_rev.reverse();
        let total_samples = samples_rev.len();
        for (sidx, s) in samples_rev.iter().enumerate() {
            let t0 = std::time::Instant::now();
            let sample_contigs: u64 = {
                let mut names = dec.list_contigs(s)?;
                names.reverse();
                let mut c = 0u64;
                for cname in &names {
                    let numeric = dec.get_contig(s, cname)?;
                    let mut seq = ascii_of(&numeric, args.upper)?;
                    let len = seq.len() as u64;
                    seq.reverse();
                    { use std::io::Write as _;
                      if let Some(w) = stdout_out.as_mut() { w.write_all(b"$")?; w.write_all(&seq)?; }
                      else if let Some(w) = file_out.as_mut() { w.write_all(b"$")?; w.write_all(&seq)?; }
                    }
                    rows.push((cname.clone(), streamed, len));
                    streamed += len + 1;
                    c += 1;
                }
                c
            };
            n_contigs += sample_contigs;
            eprintln!("[{}/{}] sample {s}: {sample_contigs} contigs, streamed {streamed} bytes ({:.0}s)",
                      total_samples - sidx, total_samples, t0.elapsed().as_secs_f32());
        }
        let total = streamed; // == forward flat length (exact mirror)
        eprintln!("TOTAL {total}");
        { use std::io::Write as _;
          if let Some(w) = stdout_out.as_mut() { w.flush()?; }
          if let Some(w) = file_out.as_mut() { w.flush()?; }
        }
        let mut tsv = BufWriter::with_capacity(1 << 16, File::create(&tsv_path)?);
        for (cname, soff, len) in &rows {
            // stream: [soff]='$', [soff+1 .. soff+len)=rev(contig)
            // forward: [fstart .. fstart+len)=contig, [fstart+len]='$'
            // byte i of stream == byte total-1-i of forward
            let fstart = total - 1 - soff - len;
            writeln!(tsv, "{cname}\t{fstart}\t{len}")?;
        }
        tsv.flush()?;
        eprintln!("reversed flat: {total} bytes, {n_contigs} contigs\nnames: {tsv_path}");
        return Ok(());
    }
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
