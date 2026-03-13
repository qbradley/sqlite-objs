# Skill: Rust FFI Crate Creation

## Pattern Name
Two-Crate Workspace for C Library FFI Bindings

## When to Use
Creating Rust bindings for an existing C library with:
- C source code that needs compilation
- System library dependencies (OpenSSL, libcurl, etc.)
- Need for both unsafe FFI layer and safe wrapper API

## Structure

```
rust/                           # Workspace root
├── Cargo.toml                  # Workspace manifest
├── {library}-sys/              # FFI layer (-sys convention)
│   ├── Cargo.toml              # links = "library"
│   ├── build.rs                # C compilation via cc crate
│   └── src/lib.rs              # Raw FFI declarations
└── {library}/                  # Safe wrapper
    ├── Cargo.toml              # Depends on {library}-sys
    ├── src/lib.rs              # Safe Rust API
    └── examples/               # Usage examples
```

## Implementation Steps

### 1. Workspace Manifest (`Cargo.toml`)
```toml
[workspace]
members = ["library-sys", "library"]
resolver = "2"

[workspace.package]
version = "0.1.0"
authors = ["Your Team"]
edition = "2021"
license = "MIT"
rust-version = "1.70"

[workspace.dependencies]
libc = "0.2"
```

### 2. FFI Crate (`library-sys/Cargo.toml`)
```toml
[package]
name = "library-sys"
links = "library"           # Prevents duplicate compilation
build = "build.rs"

version.workspace = true
edition.workspace = true
license.workspace = true

[dependencies]
libc.workspace = true

[build-dependencies]
cc = "1.0"                  # C compiler wrapper
```

### 3. Build Script (`library-sys/build.rs`)

Key patterns:
- Separate SQLite/third-party compilation (warnings off) from library code (warnings on)
- Platform-specific library discovery (pkg-config, Homebrew)
- Proper rerun-if-changed directives

```rust
use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let repo_root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap())
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf();

    let src_dir = repo_root.join("src");

    // System library discovery (OpenSSL example)
    let openssl_include = if let Ok(output) = Command::new("pkg-config")
        .args(&["--cflags-only-I", "openssl"])
        .output()
    {
        String::from_utf8_lossy(&output.stdout)
            .trim()
            .strip_prefix("-I")
            .map(String::from)
    } else if let Ok(output) = Command::new("brew")
        .args(&["--prefix", "openssl"])
        .output()
    {
        let prefix = String::from_utf8_lossy(&output.stdout).trim().to_string();
        Some(format!("{}/include", prefix))
    } else {
        None
    };

    // Compile C library
    let mut builder = cc::Build::new();
    builder
        .file(src_dir.join("library.c"))
        .include(&src_dir)
        .define("FEATURE_FLAG", None)
        .warnings(true)
        .extra_warnings(true)
        .flag("-std=c11");

    if let Some(inc) = openssl_include {
        builder.include(inc);
    }

    builder.compile("library");

    // Link system libraries
    if let Ok(output) = Command::new("pkg-config")
        .args(&["--libs-only-L", "openssl"])
        .output()
    {
        let libs = String::from_utf8_lossy(&output.stdout);
        for flag in libs.trim().split_whitespace() {
            if let Some(path) = flag.strip_prefix("-L") {
                println!("cargo:rustc-link-search=native={}", path);
            }
        }
    } else if let Ok(output) = Command::new("brew")
        .args(&["--prefix", "openssl"])
        .output()
    {
        let prefix = String::from_utf8_lossy(&output.stdout).trim().to_string();
        println!("cargo:rustc-link-search=native={}/lib", prefix);
    }

    println!("cargo:rustc-link-lib=curl");
    println!("cargo:rustc-link-lib=ssl");

    // Rerun triggers
    println!("cargo:rerun-if-changed={}", src_dir.join("library.c").display());
    println!("cargo:rerun-if-changed={}", src_dir.join("library.h").display());
}
```

### 4. FFI Declarations (`library-sys/src/lib.rs`)

```rust
#![allow(non_camel_case_types)]

use libc::{c_char, c_int, c_void};

// Opaque types
#[repr(C)]
pub struct opaque_type_t {
    _private: [u8; 0],
}

// Config struct
#[repr(C)]
#[derive(Debug)]
pub struct library_config_t {
    pub field1: *const c_char,
    pub field2: c_int,
    pub opaque: *mut c_void,
}

// Function bindings
extern "C" {
    pub fn library_init(config: *const library_config_t) -> c_int;
    pub fn library_cleanup() -> c_int;
}

// Constants
pub const LIBRARY_OK: c_int = 0;
pub const LIBRARY_ERROR: c_int = 1;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_linkage() {
        // Basic FFI call to verify linking works
        unsafe {
            let result = library_cleanup();
            assert!(result >= 0);
        }
    }
}
```

### 5. Safe Wrapper (`library/Cargo.toml`)

```toml
[package]
name = "library"
version.workspace = true
edition.workspace = true
license.workspace = true

[dependencies]
library-sys = { path = "../library-sys" }
thiserror = "2.0"

[dev-dependencies]
tempfile = "3.8"
```

### 6. Safe API (`library/src/lib.rs`)

```rust
use std::ffi::CString;
use std::ptr;
use thiserror::Error;

#[derive(Error, Debug)]
pub enum LibraryError {
    #[error("Configuration error: {0}")]
    InvalidConfig(String),
    
    #[error("Operation failed: {0}")]
    OperationFailed(i32),
}

pub type Result<T> = std::result::Result<T, LibraryError>;

#[derive(Debug, Clone)]
pub struct Config {
    pub field1: String,
    pub field2: i32,
}

pub struct Library;

impl Library {
    pub fn init(config: &Config) -> Result<()> {
        let field1_cstr = CString::new(config.field1.as_str())
            .map_err(|_| LibraryError::InvalidConfig("null byte in field1".into()))?;

        let c_config = library_sys::library_config_t {
            field1: field1_cstr.as_ptr(),
            field2: config.field2,
            opaque: ptr::null_mut(),
        };

        let rc = unsafe { library_sys::library_init(&c_config) };
        
        if rc == library_sys::LIBRARY_OK {
            Ok(())
        } else {
            Err(LibraryError::OperationFailed(rc))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_config_validation() {
        let config = Config {
            field1: "test\0invalid".to_string(),
            field2: 42,
        };
        
        let result = Library::init(&config);
        assert!(matches!(result, Err(LibraryError::InvalidConfig(_))));
    }
}
```

## Key Design Decisions

### 1. Two-Crate Split
- **`library-sys`**: Raw FFI, mirrors C API exactly, unsafe
- **`library`**: Safe wrapper, idiomatic Rust, user-facing

**Rationale**: Separation enables future bindgen integration, clear unsafe boundaries

### 2. Build Script Responsibilities
- Compile C sources (no external build system needed)
- Discover system libraries (pkg-config → Homebrew → fallback)
- Platform-specific configuration

**Rationale**: Users only need `cargo build`, no configure scripts

### 3. Owned Types in Safe Wrapper
- Config structs use `String` not `*const c_char`
- Validate null bytes before FFI call
- Convert to C types at FFI boundary only

**Rationale**: Prevents use-after-free, null-byte injection, dangling pointers

### 4. Error Handling
- FFI layer: Return raw C error codes
- Safe wrapper: Convert to Rust `Result<T, Error>`
- Use `thiserror` for ergonomic error types

## Testing Strategy

**Unit Tests (FFI layer):**
- Verify linkage with simple FFI calls
- No business logic needed

**Unit Tests (Safe wrapper):**
- Config validation (null bytes, invalid values)
- Error conversion
- Can use mocking if needed

**Integration Tests:**
- Require actual library functionality
- May need external services (database, API)

## Documentation

### Crate-Level Docs
```rust
//! # library - Safe bindings to C library
//!
//! This crate provides safe, idiomatic Rust bindings to the C library.
//!
//! ## Usage
//!
//! ```no_run
//! use library::{Library, Config};
//!
//! let config = Config {
//!     field1: "value".to_string(),
//!     field2: 42,
//! };
//!
//! Library::init(&config)?;
//! # Ok::<(), library::LibraryError>(())
//! ```
```

### README.md
- Installation instructions
- System dependencies
- Quick start examples
- Link to full documentation

## Common Pitfalls

❌ **Don't**: Put C compilation in the safe wrapper crate  
✅ **Do**: Keep all C code in `-sys` crate

❌ **Don't**: Use `CString` in public API  
✅ **Do**: Use `String` and convert at FFI boundary

❌ **Don't**: Assume OpenSSL/libcurl locations  
✅ **Do**: Use pkg-config with Homebrew fallback

❌ **Don't**: Ignore rerun-if-changed directives  
✅ **Do**: Add all C source files to trigger rebuilds

❌ **Don't**: Mix warnings-off/on in single cc::Build  
✅ **Do**: Use separate builders for third-party vs your code

## Platform Coverage

**Tier 1 (Full Support):**
- macOS (Homebrew OpenSSL)
- Linux (apt/yum packages)

**Tier 2 (Best Effort):**
- BSD (pkg-config)

**Not Supported Initially:**
- Windows (different toolchain, consider later)

## Maintenance

**When C API Changes:**
1. Update FFI declarations in `library-sys/src/lib.rs`
2. Update safe wrapper to match
3. Bump version per semver
4. Update CHANGELOG.md

**Adding New C Sources:**
1. Add `.file()` call in `build.rs`
2. Add `rerun-if-changed` directive
3. Declare new functions in FFI layer

## Related Patterns

- **Pure bindgen**: Auto-generate FFI from headers (works if no custom build)
- **Vendored sources**: Bundle C source in crate (use for portability)
- **Dynamic linking**: Skip C compilation, link system library (use if widely available)

## Example Projects Using This Pattern

- `rusqlite` / `libsqlite3-sys`
- `openssl` / `openssl-sys`
- `curl` / `curl-sys`
- `sqlite-objs` / `sqlite-objs-sys` (this project)

## Checklist

Before publishing:
- [ ] All tests pass (`cargo test`)
- [ ] Compiles on macOS and Linux
- [ ] Documentation builds (`cargo doc`)
- [ ] Examples run successfully
- [ ] README has installation instructions
- [ ] CHANGELOG.md exists
- [ ] License file included
- [ ] Crate metadata complete (description, keywords, categories)
