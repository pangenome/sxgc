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
//   agc2flat <archive.agc> [--samples <file>] [--revlines] [-o out.txt]
//       one reversed contig per record, --sep terminated (default 0x1E).
//       Sidecar keeps FORWARD flat offsets (same-pass ground truth).
//       Use --sep 0a explicitly for legacy newline consumers.
//   agc2flat <archive.agc> [--samples <file>] [--reverse] [--stdout] ...
//       --samples restricts ALL modes to the listed AGC sample names
//       (one per line, '#' starts a comment); unknown names abort loudly;
//       archive order is preserved so a subset stream is a subsequence of
//       the whole-collection stream.
//
//   agc2flat <archive.agc> --serve-ranges <out>.names.tsv --revlines --upper
//       audit range service: initial LE u64 length, then stdin LE u64 pairs
//       (offset, length <= 65536), stdout exact range bytes; EOF closes.
// Uses targeted per-contig extraction (get_contig / get_contig_range) —
// never get_sample (which decompresses every contig of a sample at once).
use anyhow::{Context, Result};
use ragc_core::{Decompressor, DecompressorConfig, CNV_NUM};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufRead, BufReader, BufWriter, Read, Write};
use std::path::Path;

fn ascii_of(numeric: &[u8], upper: bool, sep: u8) -> Result<Vec<u8>> {
    let mut v = Vec::with_capacity(numeric.len());
    for &b in numeric {
        anyhow::ensure!(b != 0x1e, "corpus contract: reserved separator 0x1E must never appear in sequence content");
        let c = if b < 16 { CNV_NUM[b as usize] } else { b'N' };
        if c == 0 || c == 1 || c == 2 {
            anyhow::bail!("forbidden byte {c} after conversion");
        }
        let c = if upper && c.is_ascii_lowercase() { c - 32 } else { c };
        anyhow::ensure!(c != 0x1e && c != sep, "corpus contract: reserved separator 0x{sep:02X} must never appear in sequence content");
        v.push(c);
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
    revlines: bool,
    list_samples: bool,
    sep: u8,
    serve_ranges: Option<String>,
}

fn parse_sep(value: &str) -> Result<u8> {
    let digits = value.strip_prefix("0x").or_else(|| value.strip_prefix("0X")).unwrap_or(value);
    anyhow::ensure!(!digits.is_empty() && digits.len() <= 2 && digits.bytes().all(|c| c.is_ascii_hexdigit()),
                    "--sep requires a hex byte, e.g. 1e or 0x1E");
    Ok(u8::from_str_radix(digits, 16)?)
}

fn parse_args() -> Result<Args> {
    let mut a = Args { archive: String::new(), out: String::new(), upper: false, groups: false, group: None, band: None, reverse: false, stdout: false, samples: None, revlines: false, list_samples: false, sep: 0x1e, serve_ranges: None };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "-o" => a.out = it.next().context("-o needs a value")?,
            "--sep" => a.sep = parse_sep(&it.next().context("--sep needs a hex byte (default 0x1E)")?)?,
            "--upper" => a.upper = true,
            "--reverse" => a.reverse = true,
            "--revlines" => a.revlines = true,
            "--stdout" => a.stdout = true,
            "--serve-ranges" => a.serve_ranges = Some(it.next().context("--serve-ranges needs names.tsv")?),
            "--samples" => a.samples = Some(it.next().context("--samples needs a file of sample names")?),
            "--list-samples" => a.list_samples = true,
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
        anyhow::bail!("usage: agc2flat <archive.agc> [-o out.txt] [--upper] [--groups] [--group <contig>] [--band S:E] [--samples <file>] [--reverse] [--revlines] [--stdout] [--sep <hexbyte> (default 0x1E)]\nCorpus contract: 0x1E is reserved; it must never appear in sequence content.");
    }
    Ok(a)
}

// Binary range service for the audit. Requests are two LE u64s (stream
// offset, length <= 64 KiB); replies are exactly length bytes. EOF closes it.
// The names rows disambiguate repeated names by archive sample/contig order.
fn serve_ranges(dec: &mut Decompressor, samples: &[String], args: &Args, path: &str) -> Result<()> {
    anyhow::ensure!(args.revlines && args.upper && args.sep == 30,
                    "range service requires --revlines --upper --sep 1e");
    let mut names = BufReader::new(File::open(path)?).lines();
    let mut rows = Vec::new();
    let mut total = 0u64;
    for sample in samples {
        for contig in dec.list_contigs(sample)? {
            let line = names.next().context("missing names row")??;
            let fields: Vec<_> = line.split('\t').collect();
            anyhow::ensure!(fields.len() == 3 && fields[0] == contig, "archive/names order mismatch");
            let start: u64 = fields[1].parse()?;
            let len: u64 = fields[2].parse()?;
            rows.push((total, len, start, sample.clone(), contig));
            total = total.checked_add(len).and_then(|n| n.checked_add(1)).context("names length overflow")?;
        }
    }
    anyhow::ensure!(names.next().is_none() && total > 0, "extra names rows or empty archive");
    for (offset, len, start, _, _) in &rows {
        anyhow::ensure!(*start == total - 1 - offset - len, "invalid names coordinate");
    }
    let mut input = std::io::stdin().lock();
    let mut output = std::io::stdout().lock();
    output.write_all(&total.to_le_bytes())?;
    output.flush()?;
    loop {
        let mut request = [0u8; 16];
        if input.read(&mut request[..1])? == 0 { break; }
        input.read_exact(&mut request[1..])?;
        let mut offset = u64::from_le_bytes(request[..8].try_into()?);
        let length = u64::from_le_bytes(request[8..].try_into()?);
        anyhow::ensure!(length <= 65536 && offset <= total && length <= total - offset, "invalid audit range");
        let end = offset + length;
        while offset < end {
            let idx = rows.partition_point(|r| r.0 <= offset) - 1;
            let (base, len, _, sample, contig) = &rows[idx];
            let local = offset - base;
            if local == *len {
                output.write_all(&[args.sep])?;
                offset += 1;
            } else {
                let take = (end - offset).min(len - local);
                let numeric = dec.get_contig_range(sample, contig,
                    usize::try_from(len - local - take)?, usize::try_from(len - local)?)?;
                anyhow::ensure!(numeric.len() as u64 == take, "short archive range");
                let mut seq = ascii_of(&numeric, true, args.sep)?;
                seq.reverse();
                output.write_all(&seq)?;
                offset += take;
            }
        }
        output.flush()?;
    }
    Ok(())
}

fn main() -> Result<()> {
    let args = parse_args()?;
    let mut dec = Decompressor::open(&args.archive, DecompressorConfig::default())?;
    let all_samples = dec.list_samples();
    if args.list_samples {
        for s in &all_samples { println!("{s}"); }
        return Ok(());
    }

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

    if let Some(path) = &args.serve_ranges {
        return serve_ranges(&mut dec, &samples, &args, path);
    }

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
                let seq = ascii_of(&numeric, args.upper, args.sep)?;
                if seq.is_empty() { continue; }
                text.write_all(&seq)?;
                text.write_all(&[args.sep])?;
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

    // --revlines: one reversed contig per separator-terminated record.
    // Use --sep 0a for legacy BCR consumers. Contigs in FORWARD archive order (each
    // string is independent in BCR; the sidecar is the coordinate truth).
    if args.revlines {
        let mut out_path = args.out.clone();
        if out_path.is_empty() {
            out_path = Path::new(&args.archive).file_stem().unwrap().to_string_lossy().to_string() + ".revlines.txt";
        }
        let tsv_path = format!("{out_path}.names.tsv");
        let sink: Box<dyn Write> = if args.stdout { Box::new(std::io::stdout()) }
            else { Box::new(File::create(&out_path)?) };
        let mut text = BufWriter::with_capacity(1 << 22, sink);
        let mut tsv = BufWriter::with_capacity(1 << 16, File::create(&tsv_path)?);
        let mut rows: Vec<(String, u64, u64)> = Vec::new(); // (cname, stream_off, len)
        let mut streamed: u64 = 0;
        let mut n_contigs: u64 = 0;
        let total_samples = samples.len();
        for (sidx, s) in samples.iter().enumerate() {
            let names = dec.list_contigs(s)?;
            for cname in &names {
                let numeric = dec.get_contig(s, cname)?;
                let mut seq = ascii_of(&numeric, args.upper, args.sep)?;
                let len = seq.len() as u64;
                seq.reverse();
                rows.push((cname.clone(), streamed, len));
                text.write_all(&seq)?;
                text.write_all(&[args.sep])?;
                streamed += len + 1;
                n_contigs += 1;
            }
            eprintln!("[{}/{}] sample {s}: streamed {streamed} bytes, {n_contigs} contigs",
                      sidx + 1, total_samples);
        }
        text.flush()?;
        let total = streamed; // == forward flat length (incl separators)
        eprintln!("TOTAL {total}");
        for (cname, soff, len) in &rows {
            // stream block: rev(contig) at [soff, soff+len), separator at soff+len.
            // The stream is the separator-mirror rotated per block; the rotation
            // cancels, giving the same forward-start formula as --reverse:
            let fstart = total - 1 - soff - len;
            writeln!(tsv, "{cname}\t{fstart}\t{len}")?;
        }
        tsv.flush()?;
        eprintln!("revlines collection: {total} bytes, {n_contigs} strings\ntext: {out_path}\nnames: {tsv_path}");
        return Ok(());
    }

    // whole-collection mode (validated on yeast235)
    // --reverse --stdout: stream the byte-exact mirror of the forward flat text
    // (separator + reversed contig, last contig first) so pscan -S can parse the
    // reversed text without any materialization. Sidecar keeps FORWARD offsets.
    if args.reverse {
        // stream the byte-exact mirror of the forward flat text (separator + reversed
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
                    let mut seq = ascii_of(&numeric, args.upper, args.sep)?;
                    let len = seq.len() as u64;
                    seq.reverse();
                    { use std::io::Write as _;
                      if let Some(w) = stdout_out.as_mut() { w.write_all(&[args.sep])?; w.write_all(&seq)?; }
                      else if let Some(w) = file_out.as_mut() { w.write_all(&[args.sep])?; w.write_all(&seq)?; }
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
            // stream: [soff]=separator, [soff+1 .. soff+len)=rev(contig)
            // forward: [fstart .. fstart+len)=contig, [fstart+len]=separator
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
            let seq = ascii_of(&numeric, args.upper, args.sep)?;
            let start = offset;
            text.write_all(&seq)?;
            text.write_all(&[args.sep])?;
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

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn separator_bytes_and_collisions() {
        for value in ["1e", "1E", "0x1E", "0X1e"] { assert_eq!(parse_sep(value).unwrap(), 30); }
        for value in ["", "100", "-1", "gg", "0x", "+1"] { assert!(parse_sep(value).is_err()); }
        assert_eq!(ascii_of(&[0, 1, 2, 3], true, 30).unwrap(), b"ACGT");
        let error = ascii_of(&[0, 30, 1], true, 30).unwrap_err().to_string();
        assert!(error.contains("corpus contract: reserved separator 0x1E"));
        assert!(ascii_of(&[0, 1], true, b'A').is_err());
    }
}
