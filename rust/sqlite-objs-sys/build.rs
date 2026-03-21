use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let src_dir = manifest_dir.join("csrc");

    // Get SQLite include path from libsqlite3-sys (set via its `links` metadata)
    let sqlite_include = env::var("DEP_SQLITE3_INCLUDE")
        .expect("DEP_SQLITE3_INCLUDE should be set by libsqlite3-sys");

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();

    // Find OpenSSL using pkg-config; fall back to Homebrew on macOS only
    let openssl_include = if let Ok(output) = Command::new("pkg-config")
        .args(["--cflags-only-I", "openssl"])
        .output()
    {
        let s = String::from_utf8_lossy(&output.stdout).trim().to_string();
        if s.is_empty() {
            None
        } else {
            s.strip_prefix("-I").map(String::from)
        }
    } else if target_os == "macos" {
        if let Ok(output) = Command::new("brew").args(["--prefix", "openssl"]).output() {
            let prefix = String::from_utf8_lossy(&output.stdout).trim().to_string();
            Some(format!("{}/include", prefix))
        } else {
            None
        }
    } else {
        None
    };

    // Compile sqlite-objs VFS and Azure client (SQLite comes from libsqlite3-sys)
    let mut builder = cc::Build::new();
    builder
        .file(src_dir.join("sqlite_objs_vfs.c"))
        .file(src_dir.join("azure_client.c"))
        .file(src_dir.join("azure_auth.c"))
        .file(src_dir.join("azure_error.c"))
        .include(&src_dir)
        .include(&sqlite_include)
        .define("SQLITE_THREADSAFE", "1")
        .define("SQLITE_ENABLE_FTS5", None)
        .define("SQLITE_ENABLE_JSON1", None)
        .warnings(true)
        .extra_warnings(true)
        .flag("-std=c11");

    // Platform-specific feature macros (mirrors Makefile logic)
    if target_os == "macos" {
        builder.define("_DARWIN_C_SOURCE", None);
    } else {
        builder.define("_GNU_SOURCE", None);
    }

    if let Some(inc) = openssl_include {
        builder.include(inc);
    }

    builder.compile("sqlite_objs");

    // Find and link OpenSSL using pkg-config; fall back to Homebrew on macOS
    if let Ok(output) = Command::new("pkg-config")
        .args(["--libs-only-L", "openssl"])
        .output()
    {
        let libs = String::from_utf8_lossy(&output.stdout);
        for flag in libs.split_whitespace() {
            if let Some(path) = flag.strip_prefix("-L") {
                println!("cargo:rustc-link-search=native={}", path);
            }
        }
    } else if target_os == "macos" {
        if let Ok(output) = Command::new("brew").args(["--prefix", "openssl"]).output() {
            let prefix = String::from_utf8_lossy(&output.stdout).trim().to_string();
            println!("cargo:rustc-link-search=native={}/lib", prefix);
        }
    }

    // Link sqlite3 from libsqlite3-sys (needed for sqlite-objs C code's sqlite3_* calls)
    if let Ok(lib_dir) = env::var("DEP_SQLITE3_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", lib_dir);
    }
    println!("cargo:rustc-link-lib=static=sqlite3");

    // Link system libraries
    println!("cargo:rustc-link-lib=curl");
    println!("cargo:rustc-link-lib=ssl");
    println!("cargo:rustc-link-lib=crypto");
    println!("cargo:rustc-link-lib=pthread");
    println!("cargo:rustc-link-lib=m");
    if target_os == "linux" {
        println!("cargo:rustc-link-lib=dl");
    }

    // Tell cargo to recompile if C sources change
    println!(
        "cargo:rerun-if-changed={}",
        src_dir.join("sqlite_objs_vfs.c").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        src_dir.join("azure_client.c").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        src_dir.join("azure_auth.c").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        src_dir.join("azure_error.c").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        src_dir.join("sqlite_objs.h").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        src_dir.join("azure_client.h").display()
    );
}
