//! Timed, fail-closed source pipeline with endpoint samples from the vendored tap.
use std::path::PathBuf;

const USAGE: &str = "usage:
  xsa build --text <collection.txt> -o <out.sxi> [--threads N] [--address-space-gb N] [--scratch DIR] [--expect-chi N] [--verify-text-sample N] [--mode auto|dna|text] [--verbose]
  xsa build --agc <archive.agc> -o <out.sxi> [same options]
  xsa build --fasta <records.fa> -o <out.sxi> [same options]
  xsa build --fastq <reads.fq> -o <out.sxi> [same options]

Corpus contract: T = s1 0x1E s2 0x1E ... sk 0x1E, ONE cyclic byte string.
0x1E (ASCII record separator) is reserved; never include it in sequence content.
--text passes raw bytes AS-IS, without a content scan; the caller owns this contract.
FASTA (wrapped lines allowed) and FASTQ (four-line records) extract sequences
verbatim in input order, with 0x1E between records and terminally.
Supply already oriented sequences; embedded 0x1E fails during input preparation.
Record identifiers populate the SXI names member. Malformed records fail closed.
AGC uses a seekable archive-backed FUSE ghost file (revlines, upper, sep 1e).
--fifo selects the sequential streaming fallback.
--materialize retains the legacy AGC text-file path.
--manifest PATH selects a tool manifest; --allow-drift is DEBUG ONLY and journals hash mismatches.
Stages: PFP (w1=10,p1=100; w2=5,p2=11), endpoint-tap rpfbwt,
slim streaming aggregates, streamed chi sweep/gate, checked SXI publication.
Scratch must share the source filesystem. Timings and peak RSS are logged.
--address-space-gb sets the child virtual-memory ceiling in decimal GB (default 149), bounded by an inherited hard limit.
Legacy newline-terminated multi-string inputs require --expect-heads RAW --expect-ri4 FILE byte gates
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
            "--manifest" | "--mode" | "--verify-text-sample" | "--threads" | "--address-space-gb" | "--scratch" | "--log-dir" | "--expect-chi" | "--expect-heads" | "--expect-ri4" => {
                i += 1;
                let value = args.get(i).filter(|s| !s.is_empty() && !s.starts_with('-'))
                    .ok_or_else(|| format!("{flag} requires a value"))?;
                extra.extend([flag.clone(), value.clone()]);
            }
            "--allow-drift" | "--materialize" | "--fifo" => extra.push(flag.clone()),
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
    let bundled = super::bundle::resolve()?;
    let script = if let Some(script) = std::env::var_os("XSA_PIPELINE") {
        PathBuf::from(script)
    } else if std::env::var_os("XSA_TOOLS").is_some_and(|prefix| {
        std::fs::read(PathBuf::from(prefix).join("MANIFEST.sha256"))
            .is_ok_and(|bytes| bytes.starts_with(b"# SXI toolset v1"))
    }) {
        // Legacy development manifests also bind checkout sources. Locate that
        // checkout when only XSA_TOOLS was supplied; installed JSON manifests
        // continue using the embedded pipeline in any working directory.
        let mut roots = vec![PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..")];
        if let Ok(cwd) = std::env::current_dir() {
            roots.extend(cwd.ancestors().map(PathBuf::from));
        }
        roots.into_iter().map(|root| root.join("bit6/sxi_pipeline.py"))
            .find(|path| path.is_file()).ok_or(
                "legacy XSA_TOOLS manifest needs its source checkout; run from that checkout or set XSA_PIPELINE")?
    } else {
        bundled.join("bit6/sxi_pipeline.py")
    };
    let mut command = std::process::Command::new("python3");
    command.env("XSA_INSTALL_MANIFEST_SHA256", super::bundle::ID)
        .env("XSA_PACKAGE_VERSION", env!("CARGO_PKG_VERSION"));
    if std::env::var_os("XSA_TOOLS").is_none() && std::env::var_os("XSA_PIPELINE").is_none() {
        command.env("XSA_TOOLS", &bundled);
    }
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
