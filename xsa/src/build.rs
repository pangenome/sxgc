//! Timed, fail-closed source pipeline with endpoint samples from the vendored tap.
use std::path::PathBuf;

const USAGE: &str = "usage:
  xsa build --text <collection.txt> -o <out.sxi> [--threads N] [--scratch DIR] [--expect-chi N] [--verbose]
  xsa build --agc <archive.agc> -o <out.sxi> [same options]
  xsa build --fasta <records.fa> -o <out.sxi> [same options]
  xsa build --fastq <reads.fq> -o <out.sxi> [same options]

Text uses newline-terminated pilot collection conventions, already oriented.
FASTA (wrapped lines allowed) and FASTQ (four-line records) extract sequences
verbatim in input order, one per line; supply already oriented sequences.
Record identifiers populate the SXI names member. Malformed records fail closed.
AGC uses agc2flat --revlines --upper, materialized beside its source.
Stages: PFP (w1=10,p1=100; w2=5,p2=11), endpoint-tap rpfbwt,
slim streaming aggregates, streamed chi sweep/gate, checked SXI publication.
Scratch must share the source filesystem. Timings and peak RSS are logged.
Multi-string inputs require --expect-heads RAW --expect-ri4 FILE byte gates
because linear PFP ordering is not established as BCR collection ordering.";

#[derive(Debug)]
struct Options {
    kind: String,
    input: PathBuf,
    output: PathBuf,
    verbose: bool,
    extra: Vec<String>,
}

fn parse(args: &[String]) -> Result<Options, String> {
    let mut source = None;
    let mut output = None;
    let mut verbose = false;
    let mut extra = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let flag = &args[i];
        match flag.as_str() {
            "--agc" | "--fasta" | "--fastq" | "--text" | "-o" | "--output" => {
                i += 1;
                let value = args.get(i).filter(|s| !s.is_empty() && !s.starts_with('-'))
                    .ok_or_else(|| format!("{flag} requires a path"))?;
                if flag == "-o" || flag == "--output" {
                    if output.replace(PathBuf::from(value)).is_some() {
                        return Err("specify the output only once".into());
                    }
                } else if source.replace((flag[2..].to_string(), PathBuf::from(value))).is_some() {
                    return Err("choose exactly one of --agc, --fasta, --fastq, --text".into());
                }
            }
            "--threads" | "--scratch" | "--log-dir" | "--expect-chi" | "--expect-heads" | "--expect-ri4" => {
                i += 1;
                let value = args.get(i).filter(|s| !s.is_empty() && !s.starts_with('-'))
                    .ok_or_else(|| format!("{flag} requires a value"))?;
                extra.extend([flag.clone(), value.clone()]);
            }
            "--verbose" => verbose = true,
            _ => return Err(format!("unknown option {flag}")),
        }
        i += 1;
    }
    let (kind, input) = source.ok_or("need one of --agc, --fasta, --fastq, --text")?;
    let output = output.ok_or("need -o <out.sxi>")?;
    Ok(Options { kind, input, output, verbose, extra })
}

fn run(args: &[String]) -> Result<(), String> {
    let options = parse(args)?;
    // symlink_metadata also catches dangling symlinks. Never overwrite an
    // existing output, even when construction cannot proceed.
    match std::fs::symlink_metadata(&options.output) {
        Ok(_) => return Err(format!("output exists: {}", options.output.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(format!("inspect output: {e}")),
    }
    let input = std::fs::File::open(&options.input)
        .map_err(|e| format!("open {}: {e}", options.input.display()))?;
    if !input.metadata().map_err(|e| format!("inspect input: {e}"))?.is_file() {
        return Err("input must be a regular file".into());
    }
    let script = std::env::var_os("XSA_PIPELINE").map(PathBuf::from).unwrap_or_else(||
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../bit6/sxi_pipeline.py"));
    let mut command = std::process::Command::new("python3");
    command.arg(script).arg(format!("--{}", options.kind)).arg(options.input)
        .arg("--output").arg(options.output).args(options.extra)
        .arg("--xsa").arg(std::env::current_exe().map_err(|e| e.to_string())?);
    if options.verbose { command.arg("--verbose"); }
    let status = command.status().map_err(|e| format!("launch pipeline: {e}"))?;
    if status.success() { Ok(()) } else { Err(format!("pipeline failed: {status}")) }
}

pub fn command(args: &[String]) {
    if args.len() == 1 && matches!(args[0].as_str(), "--help" | "-h") {
        println!("{USAGE}");
        return;
    }
    if let Err(e) = run(args) {
        super::die(&format!("build: {e}"));
    }
}
