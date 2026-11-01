// Compiles the functions Rust cannot express, cpp/ppc_fallback.cpp, into this
// crate. A project that builds that file itself, for its own flags or headers,
// sets PPC_SKIP_CPP_FALLBACK. CXX and AR name the tools; PPC_SIMDE_INCLUDE
// overrides where simde is, which is where the generator found it.

use std::env;
use std::path::PathBuf;
use std::process::Command;

fn run(command: &mut Command) {
    let status = command.status().unwrap_or_else(|e| panic!("{command:?}: {e}"));
    assert!(status.success(), "{command:?} failed");
}

fn main() {
    println!("cargo:rerun-if-env-changed=PPC_SKIP_CPP_FALLBACK");
    println!("cargo:rerun-if-env-changed=PPC_SIMDE_INCLUDE");
    println!("cargo:rerun-if-env-changed=CXX");
    println!("cargo:rerun-if-env-changed=AR");
    println!("cargo:rerun-if-changed=../cpp/ppc_fallback.cpp");

    if env::var_os("PPC_SKIP_CPP_FALLBACK").is_some() {
        return;
    }

    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let object = out.join("ppc_fallback.o");
    let simde = env::var("PPC_SIMDE_INCLUDE").unwrap_or_else(|_| "@SIMDE@".to_string());
    let cxx = env::var("CXX").unwrap_or_else(|_| "clang++".to_string());
    let ar = env::var("AR").unwrap_or_else(|_| "ar".to_string());

    run(Command::new(cxx)
        .args(["-std=c++20", "-O3", "-march=x86-64-v2", "-w", "-fno-exceptions", "-fno-rtti", "-c"])
        .arg("-I../cpp/include")
        .arg(format!("-I{simde}"))
        .arg("../cpp/ppc_fallback.cpp")
        .arg("-o")
        .arg(&object));
    run(Command::new(ar).arg("crs").arg(out.join("libppc_fallback.a")).arg(&object));

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=ppc_fallback");
}
