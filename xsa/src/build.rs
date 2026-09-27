//! Source-build CLI boundary. Construction is fail-closed until the missing
//! front-end endpoint/Phi contract is implemented; see SXI_CONSTRUCTION_BLOCKER.md.
//! This module deliberately does not disguise pilot-artifact conversion as a
//! fresh build, or launch a large parse whose next required stage cannot run.
use std::path::PathBuf;

const USAGE: &str = "usage:
  xsa build --agc   <archive.agc> -o <out.sxi> [--verbose]
  xsa build --fasta <refs.fa>     -o <out.sxi> [--verbose]
  xsa build --text  <file.txt>    -o <out.sxi> [--verbose]

Source construction is currently unavailable: the unchanged r-pfbwt emits
head samples, not the tail samples required by the proposed Phi post-step.
No source build is published until that dependency and the chi gate pass.
See bit6/SXI_CONSTRUCTION_BLOCKER.md.";

#[derive(Debug)]
struct Options {
    kind: String,
    input: PathBuf,
    output: PathBuf,
    verbose: bool,
}

fn parse(args: &[String]) -> Result<Options, String> {
    let mut source = None;
    let mut output = None;
    let mut verbose = false;
    let mut i = 0;
    while i < args.len() {
        let flag = &args[i];
        match flag.as_str() {
            "--agc" | "--fasta" | "--text" | "-o" | "--output" => {
                i += 1;
                let value = args.get(i).filter(|s| !s.is_empty() && !s.starts_with('-'))
                    .ok_or_else(|| format!("{flag} requires a path"))?;
                if flag == "-o" || flag == "--output" {
                    if output.replace(PathBuf::from(value)).is_some() {
                        return Err("specify the output only once".into());
                    }
                } else if source.replace((flag[2..].to_string(), PathBuf::from(value))).is_some() {
                    return Err("choose exactly one of --agc, --fasta, --text".into());
                }
            }
            "--verbose" => verbose = true,
            _ => return Err(format!("unknown option {flag}")),
        }
        i += 1;
    }
    let (kind, input) = source.ok_or("need one of --agc, --fasta, --text")?;
    let output = output.ok_or("need -o <out.sxi>")?;
    Ok(Options { kind, input, output, verbose })
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
    eprintln!("xsa build: preflight ({})", options.kind);
    if options.verbose {
        eprintln!("  input: {}", options.input.display());
        eprintln!("  output: {}", options.output.display());
        eprintln!("  required stages: parse -> front-end/RLBWT -> Phi^-1 heads -> slim aggregates -> sweep -> chi gate -> SXI write");
        eprintln!("  no stage subprocess launched: required endpoint construction is unavailable");
    }
    Err("source construction blocked: the unchanged r-pfbwt produces run-head .ssa samples, not tails; no validated r-space tail-only Phi constructor is implemented. headFromTail assumes a sound Phi index. No .sxi was written. See bit6/SXI_CONSTRUCTION_BLOCKER.md".into())
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
