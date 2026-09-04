use std::env;
use std::path::{Path, PathBuf};

fn track_tree(path: &Path) {
    for entry in std::fs::read_dir(path).expect("read native include directory") {
        let path = entry.expect("read native include entry").path();
        if path.is_dir() {
            track_tree(&path);
        } else {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }
}

fn main() {
    let crate_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("manifest directory"));
    let root = crate_dir.join("../..");
    let sources = [
        "src/common.cpp",
        "src/predictor.cpp",
        "src/ged_estimator.cpp",
        "src/eclm_quantizer.cpp",
        "src/vrans_codec.cpp",
        "src/stream_encoder.cpp",
        "src/table.cpp",
        "src/c_api.cpp",
    ];

    let mut build = cc::Build::new();
    build
        .cpp(true)
        .include(root.join("include"))
        .define("AETHER_C_STATIC", None)
        .opt_level(1)
        .flag_if_supported("-std=c++20")
        .flag_if_supported("/std:c++20")
        .flag_if_supported("-fno-lto")
        .warnings(true);
    for source in sources {
        let path = root.join(source);
        println!("cargo:rerun-if-changed={}", path.display());
        build.file(path);
    }
    track_tree(&root.join("include/aether"));
    build.compile("aether_c_rust");
}
