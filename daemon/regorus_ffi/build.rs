// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// Renders include/daemon_regorus_ffi.h from the extern "C" surface in src/lib.rs. Generated rather
// than written, because a header a signature change left behind still compiles and fails at run
// time.

fn main() {
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").expect("cargo sets CARGO_MANIFEST_DIR");
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    cbindgen::generate(&crate_dir)
        .expect("cbindgen could not read the extern \"C\" surface")
        .write_to_file(format!("{crate_dir}/src/daemon_regorus_ffi.h"));
}
