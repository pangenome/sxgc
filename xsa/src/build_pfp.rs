//! `xsa build --pfp`: fresh construction, orchestrated natively in this process.
//! Rows flow through bounded Unix pipes; the only bytes following the rows are
//! the two metadata members. The stages are the bundled binaries; nothing in
//! this path is interpreted (no Python orchestrator, no script in the bundle).
use crate::{ctx, die, docstarts};
use std::ffi::OsString;
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::fd::{FromRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdout, Command as Process, Stdio};
use std::sync::atomic::{AtomicI32, Ordering};
use std::time::Instant;

pub const USAGE: &str = "xsa build --pfp RAW --docs-map DIR -o INDEX [-T TMPDIR] [-j THREADS] [-m SORT_MEMORY]";

const HELP: &str = "\
usage: xsa build --pfp [-h] [--input RAW] --docs-map DIR -o INDEX [-T DIR] [-j N]
                       [-m SIZE] [-v] [RAW]

Build a document-aware v10 suffix index by prefix-free parsing.

positional arguments:
  RAW                   raw corpus

options:
  -h, --help            show this help message and exit
  --pfp                 selects this route (already in the command)
  --input RAW           raw corpus instead of positional RAW
  --docs-map DIR        XSEP map of the corpus (or of the same stream before
                        prefix truncation)
  -o, --output INDEX    new index; never overwrites
  -T, --tmpdir, --scratch DIR
                        scratch parent (default: TMPDIR or output directory)
  -j, --threads N       sort workers (default: up to 4)
  -m, --sort-memory SIZE
                        sort array budget, not total RSS (default: max(256M, 2 x
                        corpus bytes))
  -v, --verbose         print stage diagnostics and phase times to stderr
";

/// A construction failure, kept distinct so the exit code and the wording stay
/// the ones the previous orchestrator produced.
enum Fail {
    Help,
    Usage(String), // command line: exit 2 after printing usage
    Run(String),   // runtime: `build --pfp: <message>`, exit 1
    Signal(i32),   // SIGINT/SIGTERM/SIGHUP: exit 128+signal
}
impl From<String> for Fail {
    fn from(m: String) -> Self {
        Fail::Run(m)
    }
}

// SIGINT/SIGTERM/SIGHUP record the signal and return: the blocking stage wait is
// interrupted (no SA_RESTART), the stage group is cancelled, the scratch
// directories drop, and the process exits 128+signal.
static TERM_SIGNAL: AtomicI32 = AtomicI32::new(0);

extern "C" fn on_terminate(sig: libc::c_int) {
    TERM_SIGNAL.store(sig, Ordering::SeqCst);
}

fn install_signal_handlers() {
    unsafe {
        for sig in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
            let mut action: libc::sigaction = std::mem::zeroed();
            action.sa_sigaction = on_terminate as *const () as usize;
            action.sa_flags = 0;
            libc::sigemptyset(&mut action.sa_mask);
            libc::sigaction(sig, &action, std::ptr::null_mut());
        }
    }
}

fn check_signal() -> Result<(), Fail> {
    match TERM_SIGNAL.load(Ordering::SeqCst) {
        0 => Ok(()),
        sig => Err(Fail::Signal(sig)),
    }
}

pub fn command(args: &[OsString]) -> ! {
    install_signal_handlers();
    match run(args) {
        Ok(()) => std::process::exit(0),
        Err(Fail::Help) => {
            println!("{HELP}");
            std::process::exit(0)
        }
        Err(Fail::Usage(msg)) => {
            eprintln!("usage: {USAGE}");
            eprintln!("xsa build --pfp: error: {msg}");
            std::process::exit(2)
        }
        Err(Fail::Run(msg)) => die(&format!("build --pfp: {msg}")),
        Err(Fail::Signal(sig)) => std::process::exit(128 + sig),
    }
}

struct Options {
    corpus: PathBuf,
    docs_map: PathBuf,
    output: PathBuf,
    tmpdir: Option<PathBuf>,
    threads: usize,
    sort_memory: Option<u64>,
    verbose: bool,
}

/// CPUs this process may run on, the quantity `-j` defaults from and is capped by.
fn affinity() -> usize {
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        if libc::sched_getaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &mut set) == 0 {
            let count = libc::CPU_COUNT_S(std::mem::size_of::<libc::cpu_set_t>(), &set);
            if count > 0 {
                return count as usize;
            }
        }
    }
    1
}

/// `1K`..`1T` are binary; the value is a sort-array budget, never a file offset.
fn byte_size(value: &str) -> Result<u64, String> {
    let bad = || "expected bytes or a K/M/G/T/P binary suffix".to_string();
    let upper = value.to_ascii_uppercase();
    let (digits, shift) = match upper.strip_suffix(|c: char| "KMGTP".contains(c)) {
        Some(rest) => {
            let suffix = upper.as_bytes()[rest.len()] as char;
            (rest, 10 * (" KMGTP".find(suffix).unwrap() as u32))
        }
        None => (upper.as_str(), 0),
    };
    if digits.is_empty() || digits.starts_with('0') || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return Err(bad());
    }
    let n = digits.parse::<u128>().map_err(|_| bad())? << shift;
    if n >= 1 << 63 {
        return Err("size exceeds 64-bit file offsets".into());
    }
    Ok(n as u64)
}

fn parse(args: &[OsString]) -> Result<Options, Fail> {
    let usage = |m: String| Fail::Usage(m);
    let mut pfp = false;
    let mut corpus: Option<PathBuf> = None;
    let mut input: Option<PathBuf> = None;
    let mut docs_map = None;
    let mut output = None;
    let mut tmpdir = None;
    let mut threads = None;
    let mut sort_memory = None;
    let mut verbose = false;
    let mut i = 0;
    while i < args.len() {
        let arg = &args[i];
        // A non-UTF-8 argument can only be a path value, never a flag.
        let text = arg.to_str().unwrap_or("");
        if !text.starts_with('-') || text == "-" {
            if corpus.replace(PathBuf::from(arg)).is_some() {
                return Err(usage(format!("unrecognized arguments: {text}")));
            }
            if input.is_some() {
                return Err(usage("argument RAW: not allowed with argument --input".into()));
            }
            i += 1;
            continue;
        }
        let (name, inline) = if let Some(rest) = text.strip_prefix("--") {
            match rest.split_once('=') {
                Some((n, v)) => (format!("--{n}"), Some(OsString::from(v))),
                None => (text.to_string(), None),
            }
        } else if text.len() > 2 {
            (text[..2].to_string(), Some(OsString::from(&text[2..])))
        } else {
            (text.to_string(), None)
        };
        let mut value = |flag: &str| -> Result<OsString, Fail> {
            if let Some(v) = inline.clone() {
                return Ok(v);
            }
            i += 1;
            args.get(i).cloned().ok_or_else(|| usage(format!("argument {flag}: expected one argument")))
        };
        match name.as_str() {
            "-h" | "--help" => return Err(Fail::Help),
            "--pfp" => pfp = true,
            "--input" => {
                let v = value("--input")?;
                if corpus.is_some() {
                    return Err(usage("argument --input: not allowed with argument RAW".into()));
                }
                input = Some(PathBuf::from(v));
            }
            "--docs-map" => docs_map = Some(PathBuf::from(value("--docs-map")?)),
            "-o" | "--output" => output = Some(PathBuf::from(value("-o/--output")?)),
            "-T" | "--tmpdir" | "--scratch" => tmpdir = Some(PathBuf::from(value(&name)?)),
            "-j" | "--threads" => {
                let v = value(&name)?;
                let v = v.to_string_lossy();
                let n = v.trim().parse::<i64>().map_err(|_| usage(format!("argument -j/--threads: invalid int value: '{v}'")))?;
                threads = Some(n);
            }
            "-m" | "--sort-memory" => {
                let v = value(&name)?;
                let v = v.to_string_lossy();
                sort_memory = Some(byte_size(&v).map_err(|m| usage(format!("argument -m/--sort-memory: {m}")))?);
            }
            "-v" | "--verbose" => verbose = true,
            _ => return Err(usage(format!("unrecognized arguments: {text}"))),
        }
        i += 1;
    }
    let mut missing: Vec<&str> = Vec::new();
    if !pfp {
        missing.push("--pfp");
    }
    if docs_map.is_none() {
        missing.push("--docs-map");
    }
    if output.is_none() {
        missing.push("-o/--output");
    }
    if !missing.is_empty() {
        return Err(usage(format!("the following arguments are required: {}", missing.join(", "))));
    }
    let corpus = match (input, corpus) {
        (Some(i), _) => i,
        (None, Some(c)) => c,
        (None, None) => return Err(usage("one of the arguments RAW --input is required".into())),
    };
    let cpus = affinity();
    let threads = match threads {
        Some(n) if n >= 1 && n <= (64usize.min(cpus)) as i64 => n as usize,
        Some(_) => return Err(usage("thread count exceeds available CPUs or the 64-worker limit".into())),
        None => 4.min(cpus),
    };
    Ok(Options {
        corpus,
        docs_map: docs_map.unwrap(),
        output: output.unwrap(),
        tmpdir,
        threads,
        sort_memory,
        verbose,
    })
}
/// Where the stages write: the terminal stage's stdout and every stage's stderr
/// go to the parent's stderr with `-v`, otherwise to one unlinked-by-cleanup log
/// whose tail is forwarded when a stage fails.
enum Log {
    Inherit,
    File(File),
}

fn dup_stderr() -> Result<Stdio, Fail> {
    let fd = unsafe { libc::dup(libc::STDERR_FILENO) };
    if fd < 0 {
        return Err(Fail::Run(format!("duplicate stderr: {}", io::Error::last_os_error())));
    }
    Ok(Stdio::from(unsafe { OwnedFd::from_raw_fd(fd) }))
}

impl Log {
    fn stderr(&self) -> Result<Stdio, Fail> {
        match self {
            Log::Inherit => Ok(Stdio::inherit()),
            Log::File(f) => Ok(Stdio::from(f.try_clone().map_err(|e| format!("duplicate log: {e}"))?)),
        }
    }
    // The last stage's stdout goes where its stderr goes, so a verbose run keeps
    // the stage output on stderr and a silent run leaves stdout untouched.
    fn stdout(&self) -> Result<Stdio, Fail> {
        match self {
            Log::Inherit => dup_stderr(),
            Log::File(f) => Ok(Stdio::from(f.try_clone().map_err(|e| format!("duplicate log: {e}"))?)),
        }
    }
}

fn exit_code(status: libc::c_int) -> i32 {
    if libc::WIFEXITED(status) {
        libc::WEXITSTATUS(status)
    } else if libc::WIFSIGNALED(status) {
        -libc::WTERMSIG(status)
    } else {
        -1
    }
}

/// Pipe bounded kernel buffers only: each stage's stdout is handed to the next
/// stage as stdin, so the parent holds no upstream read end and a failing
/// downstream stage reaches its writer as EPIPE. Any failed stage cancels peers.
fn pipeline(commands: &[Vec<OsString>], log: &Log) -> Result<(), Fail> {
    let mut children: Vec<Option<Child>> = commands.iter().map(|_| None).collect();
    let mut previous: Option<ChildStdout> = None;
    let mut result = spawn_stages(commands, log, &mut children, &mut previous);
    if result.is_ok() {
        result = wait_stages(commands, &mut children);
    }
    drop(previous);
    for slot in children.iter_mut() {
        if let Some(mut child) = slot.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
    result
}

fn spawn_stages(
    commands: &[Vec<OsString>],
    log: &Log,
    children: &mut [Option<Child>],
    previous: &mut Option<ChildStdout>,
) -> Result<(), Fail> {
    for (i, argv) in commands.iter().enumerate() {
        let mut c = Process::new(&argv[0]);
        c.args(&argv[1..]);
        match previous.take() {
            Some(out) => c.stdin(Stdio::from(out)),
            None => c.stdin(Stdio::null()),
        };
        if i + 1 < commands.len() {
            c.stdout(Stdio::piped());
        } else {
            c.stdout(log.stdout()?);
        }
        c.stderr(log.stderr()?);
        let mut child = c.spawn().map_err(|e| Fail::Run(format!("{}: {e}", argv[0].to_string_lossy())))?;
        if i + 1 < commands.len() {
            *previous = child.stdout.take();
        }
        children[i] = Some(child);
    }
    Ok(())
}

fn wait_stages(commands: &[Vec<OsString>], children: &mut [Option<Child>]) -> Result<(), Fail> {
    loop {
        check_signal()?;
        if children.iter().all(|c| c.is_none()) {
            return Ok(());
        }
        let mut status: libc::c_int = 0;
        let pid = unsafe { libc::waitpid(-1, &mut status, 0) };
        if pid < 0 {
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::Interrupted {
                continue; // check_signal at the top of the loop
            }
            return Err(Fail::Run(format!("wait for stages: {e}")));
        }
        let index = children.iter().position(|c| c.as_ref().is_some_and(|c| c.id() as libc::pid_t == pid));
        let Some(index) = index else { continue };
        children[index] = None;
        let code = exit_code(status);
        if code != 0 {
            // Same shape as before: the stage binary's name and its first argument.
            let name = Path::new(&commands[index][0]).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            let arg = commands[index].get(1).map(|a| a.to_string_lossy().into_owned()).unwrap_or_default();
            return Err(Fail::Run(format!("{name} {arg} exited {code}")));
        }
    }
}

/// Phase driver: times each stage group, journals the phase lines, and forwards
/// the tail of the stage log when a group fails without `-v`.
struct Phases {
    verbose: bool,
    log: Log,
    errors: Option<File>,
}

impl Phases {
    fn note(&self, message: &str) {
        if self.verbose {
            eprintln!("{message}");
        }
    }
    fn run(&mut self, name: &str, commands: &[Vec<OsString>]) -> Result<(), Fail> {
        check_signal()?;
        let start = Instant::now();
        if let Err(fail) = pipeline(commands, &self.log) {
            if let (Some(errors), Fail::Run(_)) = (self.errors.as_mut(), &fail) {
                let _ = forward_log_tail(errors);
            }
            return Err(fail);
        }
        let elapsed = start.elapsed().as_secs_f64();
        self.note(&format!("build --pfp: {name} {elapsed:.3}s"));
        Ok(())
    }
}

fn forward_log_tail(errors: &mut File) -> io::Result<()> {
    let end = errors.stream_position()?;
    errors.seek(SeekFrom::Start(end.saturating_sub(4096)))?;
    let mut tail = Vec::new();
    errors.read_to_end(&mut tail)?;
    let mut stderr = io::stderr().lock();
    stderr.write_all(&tail)?;
    stderr.flush()
}

fn tool(tools: &Path, name: &str) -> OsString {
    tools.join(name).into_os_string()
}

fn path_arg(path: &Path) -> OsString {
    path.as_os_str().to_owned()
}

fn file_len(path: &Path) -> Result<u64, Fail> {
    Ok(fs::metadata(path).map_err(|e| format!("{}: {e}", path.display()))?.len())
}

/// One seam: P (LE u64 IDs) -> one-based inverse(P), END rank zero.
///
/// `free` excludes *all* live ancestor columns. The child directory lives until
/// the lift is over: references point into P, never into copied text.
#[allow(clippy::too_many_arguments)]
fn rank_tape(
    phases: &mut Phases,
    tools: &Path,
    tape: &Path,
    alphabet: u64,
    inverse: &Path,
    workers: &str,
    memory: u64,
    free: u64,
    level: u32,
) -> Result<(), Fail> {
    let size = file_len(tape)?;
    let n = size / 8;
    if alphabet == 0 || alphabet > n || size != 8 * n {
        return Err(Fail::Run("invalid lexical ID tape".into()));
    }
    if size > free {
        return Err(Fail::Run(format!("P{level} inverse needs {size} bytes, {free} free")));
    }
    let ranker = tools.join("stream_rank");
    let plan = Process::new(&ranker)
        .arg("plan")
        .arg(n.to_string())
        .arg(workers)
        .output()
        .map_err(|e| Fail::Run(format!("{} plan: {e}", ranker.display())))?;
    let required = match parse_plan(&plan.stdout) {
        Some(bound) if plan.status.success() => bound,
        _ => {
            let tail = &plan.stderr[plan.stderr.len().saturating_sub(512)..];
            return Err(Fail::Run(format!("invalid P{level} kernel plan: {:?}", String::from_utf8_lossy(tail))));
        }
    };
    let kernel_fits = required <= memory;

    // No repeated first symbol means no repeated block, and tiny tapes are
    // cheaper as leaves. Otherwise measure the ACTUAL next dictionary. A
    // shorter Q alone is not a work saving: count owned offsets as well.
    if n >= 16 && (!kernel_fits || alphabet < n) {
        let child = tempfile::Builder::new()
            .prefix("level-")
            .tempdir_in(inverse.parent().unwrap_or_else(|| Path::new(".")))
            .map_err(|e| format!("create parse level: {e}"))?;
        let dir = child.path();
        let q = dir.join("parse.u64");
        let qi = dir.join("inverse");
        phases.run(
            &format!("parse P{level}"),
            &[vec![
                tool(tools, "pfp_parse"),
                "ids".into(),
                path_arg(tape),
                path_arg(dir),
                workers.into(),
                memory.to_string().into(),
                free.to_string().into(),
            ]],
        )?;
        let census = fs::read(dir.join("census")).map_err(|e| format!("census: {e}"))?;
        if census.len() != 40 {
            return Err(Fail::Run("bad recursive parse census".into()));
        }
        let word = |i: usize| u64::from_le_bytes(census[8 * i..8 * i + 8].try_into().unwrap());
        let (lower, m, k, owned) = (word(0), word(1), word(2), word(3));
        let child_bytes = 8 * m + 24 * k + 40;
        if !(lower == n
            && 0 < k
            && k <= m
            && m <= n
            && k <= owned
            && owned <= n
            && file_len(&q)? == 8 * m
            && file_len(&dir.join("phrases.ref"))? == 24 * k)
        {
            return Err(Fail::Run("bad recursive parse census".into()));
        }
        let lift_bytes = child_bytes + 8 * m + size + 16 * owned;
        let use_lift = m >= 2 && 2 * m <= n && (!kernel_fits || owned + m < n) && lift_bytes <= free;
        phases.note(&format!(
            "PFP_CENSUS level={level} lower={n} upper={m} words={k} owned={owned} \
             direct_bytes={required} lift_scratch={lift_bytes} free={free} descend={}",
            u8::from(use_lift)
        ));
        if use_lift {
            rank_tape(phases, tools, &q, k, &qi, workers, memory, free - child_bytes, level + 1)?;
            phases.run(
                &format!("lift P{}->P{level}", level + 1),
                &[vec![
                    tool(tools, "pfp_lift"),
                    path_arg(tape),
                    path_arg(dir),
                    path_arg(inverse),
                    workers.into(),
                    memory.to_string().into(),
                    (free - child_bytes - 8 * m).to_string().into(),
                ]],
            )?;
            return Ok(());
        }
        if !kernel_fits {
            return Err(Fail::Run(format!(
                "P{level} direct rank needs {required} array bytes; next census {n}->{m}, \
                 lift needs {lift_bytes} scratch bytes ({free} free)"
            )));
        }
        // Unselected trial retired BEFORE allocating the direct inverse.
    }
    if !kernel_fits {
        return Err(Fail::Run(format!("P{level} direct rank needs {required} array bytes, grant {memory}")));
    }
    phases.note(&format!("PFP_LEAF level={level} symbols={n} alphabet={alphabet} array_bound={required}"));
    phases.run(
        &format!("rank P{level}"),
        &[vec![
            ranker.into_os_string(),
            "inverse".into(),
            path_arg(tape),
            path_arg(inverse),
            workers.into(),
            memory.to_string().into(),
        ]],
    )
}

/// `stream_rank plan` prints exactly one decimal line and nothing else.
fn parse_plan(stdout: &[u8]) -> Option<u64> {
    let digits = stdout.strip_suffix(b"\n")?.strip_prefix(b"parse64_array_bound=")?;
    if digits.is_empty() || !digits.iter().all(|b| b.is_ascii_digit()) {
        return None;
    }
    std::str::from_utf8(digits).ok()?.parse().ok()
}

fn fsync_dir(dir: &Path) -> Result<(), Fail> {
    let name = std::ffi::CString::new(dir.as_os_str().as_bytes()).map_err(|e| format!("{}: {e}", dir.display()))?;
    let fd = unsafe { libc::open(name.as_ptr(), libc::O_RDONLY | libc::O_DIRECTORY) };
    if fd < 0 {
        return Err(Fail::Run(format!("open {}: {}", dir.display(), io::Error::last_os_error())));
    }
    let rc = unsafe { libc::fsync(fd) };
    unsafe { libc::close(fd) };
    if rc != 0 {
        return Err(Fail::Run(format!("fsync {}: {}", dir.display(), io::Error::last_os_error())));
    }
    Ok(())
}

fn run(args: &[OsString]) -> Result<(), Fail> {
    let a = parse(args)?;
    let raw = fs::canonicalize(&a.corpus).map_err(|e| format!("{}: {e}", a.corpus.display()))?;
    let metadata = fs::metadata(&raw).map_err(|e| format!("{}: {e}", raw.display()))?;
    let n = metadata.len();
    if !metadata.is_file() || n == 0 || n >= 1 << 43 {
        return Err(Fail::Usage("corpus must be a nonempty regular file shorter than 2^43 bytes".into()));
    }
    if !a.docs_map.join("seps.bin").is_file() {
        return Err(Fail::Usage("--docs-map must contain seps.bin".into()));
    }
    if fs::symlink_metadata(&a.output).is_ok() {
        return Err(Fail::Usage(format!("output exists: {}", a.output.display())));
    }
    let mem = a.sort_memory.unwrap_or_else(|| (256 << 20).max(2 * n));
    let workers = a.threads.to_string();
    // Logical planning, NOT a filesystem quota. Include the open key/inverse
    // pair, bitmap, histogram and room for allocation rounding on native files.
    let budget = 65536.max(2 * n);
    let reserve = 16384.max(n / 8);
    let tmp_parent = a
        .tmpdir
        .clone()
        .or_else(|| std::env::var_os("TMPDIR").map(PathBuf::from).filter(|p| !p.as_os_str().is_empty()))
        .unwrap_or_else(|| match a.output.parent() {
            Some(p) if !p.as_os_str().is_empty() => p.to_path_buf(),
            _ => PathBuf::from("."),
        });
    let publish_parent = match a.output.parent() {
        Some(p) if !p.as_os_str().is_empty() => p.to_path_buf(),
        _ => PathBuf::from("."),
    };
    // This path never resumes or imports experimental finish settings.
    std::env::remove_var("XSA_FINISH_SPOOL");
    let tools = match std::env::var_os("XSA_PFP_TOOLS") {
        Some(dir) => PathBuf::from(dir),
        None => crate::bundle::resolve()?,
    };
    let exe = std::env::current_exe().map_err(|e| format!("current executable: {e}"))?;

    let scratch = tempfile::Builder::new()
        .prefix(".xsa-pfp-")
        .tempdir_in(&tmp_parent)
        .map_err(|e| format!("scratch in {}: {e}", tmp_parent.display()))?;
    let stage = tempfile::Builder::new()
        .prefix(".xsa-publish-")
        .tempdir_in(&publish_parent)
        .map_err(|e| format!("staging in {}: {e}", publish_parent.display()))?;
    let tmp = scratch.path();
    let cols = tmp.join("columns");
    fs::create_dir(&cols).map_err(|e| format!("{}: {e}", cols.display()))?;
    let tape = cols.join("parse.u64");
    let refs = cols.join("phrases.ref");
    let hist = cols.join("histogram");
    let inv = cols.join("inverse");
    let index = stage.path().join("index");

    let errors = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(tmp.join("errors.log"))
        .map_err(|e| format!("create stage log: {e}"))?;
    let log = match a.verbose {
        true => Log::Inherit,
        false => Log::File(errors.try_clone().map_err(|e| format!("duplicate stage log: {e}"))?),
    };
    let mut phases = Phases { verbose: a.verbose, log, errors: if a.verbose { None } else { Some(errors) } };

    let start = Instant::now();
    phases.run(
        "parse",
        &[vec![
            tool(&tools, "pfp_parse"),
            path_arg(&raw),
            path_arg(&cols),
            workers.clone().into(),
            mem.to_string().into(),
            (budget - reserve).to_string().into(),
        ]],
    )?;
    // The bitmap can open before the row producer retires its inputs.
    let bitmap = (n + 8) / 8;
    let tape_len = file_len(&tape)?;
    let refs_len = file_len(&refs)?;
    if 2 * tape_len + refs_len + 2048 + bitmap > budget - reserve {
        return Err(Fail::Run("PFP columns, inverse and v10 bitmap exceed scratch budget".into()));
    }
    let free = budget - reserve - tape_len - refs_len - file_len(&hist)?;
    rank_tape(&mut phases, &tools, &tape, refs_len / 24, &inv, &workers, mem, free, 1)?;
    let prepared = start.elapsed().as_secs_f64();
    // pfp_rows closes AND unlinks tape, inverse and refs before opening its
    // posting file. Count the histogram and simultaneous chi bitmap.
    let cap = budget - reserve - 2048 - bitmap;
    phases.run(
        "stream+finish",
        &[
            vec![
                tool(&tools, "pfp_rows"),
                path_arg(&raw),
                path_arg(&cols),
                workers.clone().into(),
                mem.to_string().into(),
                cap.to_string().into(),
            ],
            vec![exe.into_os_string(), "stream-context".into(), path_arg(&raw), path_arg(&a.docs_map)],
            vec![
                tool(&tools, "xsa_finish"),
                "--from-rows".into(),
                path_arg(&hist),
                path_arg(&tmp.join("stream")),
                path_arg(&index),
            ],
        ],
    )?;
    drop(phases);
    // Same filesystem, atomic no-replace. No partial product at the public name,
    // even if an upstream worker fails after emitting its last packet.
    fs::hard_link(&index, &a.output).map_err(|e| format!("publish {}: {e}", a.output.display()))?;
    fsync_dir(&publish_parent)?;
    if a.verbose {
        let size = file_len(&a.output)?;
        eprintln!(
            "build --pfp: {} ({size} bytes); raw-to-prepared {prepared:.3}s",
            a.output.display()
        );
    }
    Ok(())
}

// One coarse entry per 16 KiB keeps the random doc lookup to a small local
// bisect. Absolute starts are authoritative; never interpret a byte as a cut.
struct Docs {
    starts: Vec<u64>, // includes the exclusive text end
    pages: Vec<u32>,
}
impl Docs {
    const PAGE: usize = 16384;
    fn load(path: &str, n: usize) -> Self {
        let source = docstarts::Source::DocsMap(path.to_owned());
        let mut starts: Vec<_> = docstarts::StartsIter::open(&source, Some(n as u64)).collect();
        if starts.first() != Some(&0) {
            die("document map must start at byte zero");
        }
        let mut pages = Vec::with_capacity(n.div_ceil(Self::PAGE) + 1);
        let mut doc = 0;
        for base in (0..n).step_by(Self::PAGE) {
            while doc + 1 < starts.len() && starts[doc + 1] <= base as u64 {
                doc += 1;
            }
            pages.push(doc as u32);
        }
        pages.push((starts.len() - 1) as u32);
        starts.push(n as u64);
        Self { starts, pages }
    }
    fn at(&self, p: usize) -> (u32, u64) {
        let page = p / Self::PAGE;
        let (lo, hi) = (self.pages[page] as usize, self.pages[page + 1] as usize);
        let doc = lo + self.starts[lo + 1..hi + 1].partition_point(|&s| s <= p as u64);
        (doc as u32, self.starts[doc + 1] - p as u64)
    }
}

pub fn context(raw: &str, map: &str) -> io::Result<()> {
    let file = File::open(raw)?;
    let text = unsafe { memmap2::Mmap::map(&file)? }; // writer has closed the corpus
    let n = text.len();
    let docs = Docs::load(map, n);
    let mut trie = ctx::Builder::new(8, 1000, 1000);
    trie.set_source(ctx::SOURCE_IN_BUILD, 0);
    trie.set_boundary_aware();
    // A group of a split row stream carries its own trailer, so the count it
    // should expect is the group's row count, not the whole stream's. Default
    // unchanged: the whole stream is n+1 rows including the END row.
    //   XSA_CTX_ROWS=<rows>       the group's row count
    //   XSA_CTX_ROWS=<A>:<B>      the global rank range it covers
    // A range build retains the trie and writes a MERGEABLE PARTIAL (I1); with
    // XSA_CTX_MERGE_IN=<partial> the adjacent lower range's partial is merged in
    // first, so the two ranges together yield the payload of the whole range.
    let range = std::env::var("XSA_CTX_ROWS").ok();
    let bad = |v: &str| io::Error::new(io::ErrorKind::InvalidData, format!("XSA_CTX_ROWS={v}"));
    let (first_row, left_hint) = match range.as_deref() {
        Some(v) => match v.split_once(':') {
            Some((a, b)) => {
                let (a, b) = (a.parse::<u64>().map_err(|_| bad(v))?, b.parse::<u64>().map_err(|_| bad(v))?);
                if b < a {
                    return Err(bad(v));
                }
                (a, b - a)
            }
            None => (0, v.parse::<u64>().map_err(|_| bad(v))?),
        },
        None => (0, n as u64 + 1),
    };
    if range.is_some() {
        trie.set_first_row(first_row);
        trie.set_slice();
    }
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    let mut packet = vec![0u8; 32768 * 12];
    let mut left: usize = left_hint as usize;
    let entry = std::time::Instant::now();
    let (mut read_s, mut feed_s, mut write_s) = (0.0f64, 0.0f64, 0.0f64);
    while left > 0 {
        let take = left.min(packet.len() / 12);
        let bytes = &mut packet[..12 * take];
        let t = std::time::Instant::now();
        input.read_exact(bytes)?;
        read_s += t.elapsed().as_secs_f64();
        let t = std::time::Instant::now();
        for r in bytes.chunks_exact(12) {
            let tag = u32::from_le_bytes(r[8..12].try_into().unwrap());
            let p = u32::from_le_bytes(r[..4].try_into().unwrap()) as usize
                | (((tag as usize >> 9) & 2047) << 32);
            if p > n {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "row position exceeds corpus",
                ));
            }
            let rem = n - p;
            let (doc, avail) = if rem == 0 { (0, u64::MAX) } else { docs.at(p) };
            trie.feed(&text[p..p + rem.min(9)], rem as u64, avail, doc);
        }
        feed_s += t.elapsed().as_secs_f64();
        let t = std::time::Instant::now();
        output.write_all(bytes)?;
        write_s += t.elapsed().as_secs_f64();
        left -= take;
    }
    if input.read(&mut [0])? != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "extra ordered rows",
        ));
    }
    let payload = if range.is_some() {
        let part = trie.finish_slice(left_hint, n as u64);
        match std::env::var_os("XSA_CTX_MERGE_IN") {
            Some(path) => ctx::merge_slices(&std::fs::read(&path)?, &part),
            None => part,
        }
    } else {
        trie.finish(left_hint, n as u64).0
    };
    output.write_all(&(payload.len() as u64).to_le_bytes())?;
    output.write_all(&payload)?;
    drop(payload);
    let count = docs.starts.len() - 1;
    let m15 = docstarts::encode(
        &mut docs.starts[..count].iter().copied(),
        n as u64,
        docstarts::SAMPLE_STRIDE,
        Some(count as u64),
    );
    output.write_all(&m15.docs.to_le_bytes())?;
    output.write_all(&(m15.bytes.len() as u64).to_le_bytes())?;
    output.write_all(&m15.bytes)?;
    eprintln!(
        "STREAM_CONTEXT_TIME total={:.6} read={:.6} feed={:.6} write={:.6} tail={:.6} rows={} first={}",
        entry.elapsed().as_secs_f64(), read_s, feed_s, write_s,
        entry.elapsed().as_secs_f64() - read_s - feed_s - write_s, left_hint, first_row
    );
    output.flush()
}
