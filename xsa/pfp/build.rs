// AMENDMENT R: the deepest level's SA kernel via FFI - libsais
// (vendored at runtime/bit6/third_party/libsais, Apache-2.0).
// The opt-in wide *byte dictionary* API is a separate object in the archive;
// it does not promote the uncertified recursive integer residual.
fn main() {
    let src = env!("CARGO_MANIFEST_DIR").to_string() + "/../runtime/bit6/third_party/libsais/token-libsais.c";
    println!("cargo:rerun-if-changed={}", src);
    let mut b = cc::Build::new();
    println!("cargo:rerun-if-changed=vendor/libsais64/libsais64.c");
    println!("cargo:rerun-if-changed=vendor/libsais64/libsais64.h");
    b.file(src).file("vendor/libsais64/libsais64.c")
        .include("../runtime/bit6/third_party/libsais").opt_level(3);
    // the OPENMP variant is not built (the sequential libsais_int is
    // the certified kernel; the parallel guard is upstream's own flag)
    b.compile("libsais_r");
}
