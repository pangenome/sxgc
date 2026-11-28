//! `xsa build` dispatch: the ONE core (`--pfp`) and the one construction chain
//! (`--input` / `--tape` / `--bin`). Both run natively; this binary spawns no
//! interpreter and no shell.
use std::ffi::OsString;

/// The genomic source formats this build does NOT read. Their adapters are
/// being ported to Rust; until they land the refusal names the working route.
const ADAPTER_REFUSAL: &str = "\
1. `xsa build --agc|--fasta|--fastq` was REMOVED: the genomic source pipeline\n   \
   (the bundled bit6/sxi_pipeline.py and the shell sequence readers it drove)\n   \
   is gone from this build, which runs no interpreter and no shell.\n\
2. The AGC / FASTA / FASTQ -> raw-tape adapters are being ported to Rust. When\n   \
   they land they will emit the same raw input the chain already consumes.\n\
3. NOTHING WAS WRITTEN: no output file, no scratch tree, no log. Until the\n   \
   adapters land, feed the chain directly:\n   \
   xsa build --input <raw-file> --scratch <dir> [--docs-map <dir>]\n   \
   xsa build --pfp <raw-file> --docs-map <dir> -T <scratch> -o <index>";

pub fn command(raw: &[OsString]) {
    // The selector is an option (in any position before `--`), not a new verb.
    // Forward the original argv so the PFP parser keeps normal option/value semantics.
    let mut flags = raw.iter().take_while(|a| *a != "--").filter_map(|a| a.to_str());
    if flags.clone().any(|a| a == "--pfp") {
        if let Some(other) = flags.find(|a| matches!(*a, "--tape" | "--bin" | "--agc" | "--fasta" | "--fastq" | "--selftest")) {
            super::die(&format!("build: --pfp and {other} are mutually exclusive"));
        }
        super::build_pfp::command(raw);
    }
    let args: Vec<_> = raw.iter().map(|a| a.to_str().unwrap_or_else(|| super::die("arguments must be UTF-8")).to_owned()).collect();
    if args.iter().any(|a| a == "--text") {
        super::die("build: LEGACY_TEXT_REFUSED: --text is retired; use xsa build --tape --input <file> --scratch <dir> (virtual END)");
    }
    if args.iter().any(|a| matches!(a.as_str(), "--input" | "--selftest")) {
        super::chain::command(&args); // the consolidated construction chain route
        return;
    }
    if args.len() == 1 && matches!(args[0].as_str(), "--help" | "-h") {
        println!("usage: {}\n       See `xsa build --pfp --help` for PFP options.\n\n{}", super::build_pfp::USAGE, super::chain::USAGE);
        return;
    }
    if args.iter().any(|a| matches!(a.as_str(), "--agc" | "--fasta" | "--fastq")) {
        super::die(&format!("build: INPUT_ADAPTER_REFUSED\n{ADAPTER_REFUSAL}"));
    }
    super::die(&format!("build: unknown option {} (try --help)", args.first().map(String::as_str).unwrap_or("")));
}
