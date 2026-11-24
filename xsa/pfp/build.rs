fn main() {
    let narrow = "../runtime/bit6/third_party/libsais";
    let wide = "vendor/libsais64";
    for file in ["token-libsais.c", "token-libsais.h", "libsais.h"] {
        println!("cargo:rerun-if-changed={narrow}/{file}");
    }
    println!("cargo:rerun-if-changed={wide}/libsais64.c");
    println!("cargo:rerun-if-changed={wide}/libsais64.h");
    println!("cargo:rerun-if-env-changed=XSA_BUILD_DICT_OPENMP");
    println!("cargo:rustc-check-cfg=cfg(dict_openmp)");
    let mut b = cc::Build::new();
    b.file(format!("{narrow}/token-libsais.c"))
        .file(format!("{wide}/libsais64.c"))
        .include(narrow)
        .opt_level(3);
    let omp = match std::env::var("XSA_BUILD_DICT_OPENMP").as_deref() {
        Ok("1") => true,
        Ok("0") | Err(std::env::VarError::NotPresent) => false,
        _ => panic!("XSA_BUILD_DICT_OPENMP must be 0 or 1"),
    };
    if omp {
        assert!(
            std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux")
                && b.get_compiler().is_like_gnu(),
            "dictionary OpenMP build requires Linux/GCC/libgomp"
        );
        b.define("LIBSAIS_OPENMP", None).flag("-fopenmp");
        println!("cargo:rustc-cfg=dict_openmp");
    }
    b.compile("libsais_r");
    if omp {
        println!("cargo:rustc-link-lib=gomp");
    }
}
