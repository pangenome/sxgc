// AMENDMENT R: the deepest level's SA kernel via FFI - libsais
// (vendored at runtime/bit6/third_party/libsais, Apache-2.0).
// libsais64 (the L4 residual, expected never to run on real text)
// is NOT vendored yet - the census arithmetic dies loudly if it
// would be needed.
fn main() {
    let src = env!("CARGO_MANIFEST_DIR").to_string() + "/../runtime/bit6/third_party/libsais/token-libsais.c";
    println!("cargo:rerun-if-changed={}", src);
    let mut b = cc::Build::new();
    b.file(src).opt_level(3);
    // the OPENMP variant is not built (the sequential libsais_int is
    // the certified kernel; the parallel guard is upstream's own flag)
    b.compile("libsais_r");
}
