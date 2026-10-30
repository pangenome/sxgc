use std::{env, path::PathBuf, process::Command};
fn main() {
    assert_eq!(env::var("HOST").unwrap(), env::var("TARGET").unwrap(),
        "xsa bundled native stages currently require a native Linux build");
    assert_eq!(env::var("CARGO_CFG_TARGET_OS").unwrap(), "linux",
        "xsa bundled native stages currently support Linux");
    assert_eq!(env::var("CARGO_CFG_TARGET_ARCH").unwrap(), "x86_64",
        "xsa vendored dependency closure currently supports Linux x86_64");
    // No source digest file is tracked: provenance of the sources is git's
    // (the run journal records the commit), so editing a covered source can
    // never require a manifest regeneration step before the build.
    for path in ["build.rs", "build_tools.py", "runtime", "vendor"] {
        println!("cargo:rerun-if-changed={path}");
    }
    println!("cargo:rerun-if-env-changed=BUILD_JOBS");
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let status = Command::new("python3").arg("-I").arg("build_tools.py")
        .arg(&out).arg(env::var("CARGO_PKG_VERSION").unwrap())
        .status().expect("xsa requires Python 3 to build its bundled stages");
    assert!(status.success(), "xsa native stage build failed; inspect Cargo build output");
}
