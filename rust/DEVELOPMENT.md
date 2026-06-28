# sqlite-objs Rust Development Guide

## Project Structure

```
rust/
├── Cargo.toml                      # Workspace manifest
├── README.md                       # User documentation
├── QUICKSTART.md                   # Quick reference
├── DEVELOPMENT.md                  # This file
├── .gitignore                      # Ignore target/ and artifacts
├── sqlite-objs-sys/                # FFI bindings (unsafe)
│   ├── Cargo.toml                  # Links="sqlite-objs", build-deps: cc
│   ├── build.rs                    # Compiles C sources via cc crate
│   └── src/lib.rs                  # Raw FFI declarations
└── sqlite-objs/                    # Safe wrapper (idiomatic Rust)
    ├── Cargo.toml                  # Depends on sqlite-objs-sys, thiserror
    ├── src/lib.rs                  # Public API
    └── examples/basic.rs           # Usage demonstration
```

## Build System

### How build.rs Works

1. Compiles SQLite amalgamation (`sqlite3.c`) as static library
2. Compiles sqlite-objs C sources (`sqlite_objs_vfs.c`, `azure_client.c`, `azure_auth.c`, `azure_error.c`)
3. Discovers OpenSSL paths via pkg-config or Homebrew
4. Links system libraries: curl, ssl, crypto, pthread, m
5. Generates rerun-if-changed directives for incremental builds

### Platform-Specific Notes

**macOS:**
- OpenSSL from Homebrew: `brew install openssl`
- Uses `pkg-config --cflags/--libs openssl` or `brew --prefix openssl`
- Requires Xcode Command Line Tools for cc compiler

**Linux:**
- Install dev packages: `apt-get install build-essential libcurl4-openssl-dev libssl-dev pkg-config`
- Standard pkg-config discovery

## Development Workflow

### Initial Setup

```bash
cd rust
cargo build          # Build all crates
cargo test           # Run tests (FFI only, no Azure needed)
cargo doc --open     # Generate and view documentation
```

### Making Changes

**To FFI bindings (sqlite-objs-sys):**
1. Update `src/lib.rs` with new extern "C" declarations
2. Update `build.rs` if new C sources added
3. Run `cargo build` to verify compilation
4. Run `cargo test` (basic linkage tests)

**To safe wrapper (sqlite-objs):**
1. Update `src/lib.rs` with safe Rust API
2. Add tests in `#[cfg(test)] mod tests`
3. Update examples if API changes
4. Run `cargo test --doc` for doctests

## Development Workflow

### Initial Setup

```bash
cd rust
cargo build          # Build all crates
cargo test           # Run tests (FFI only, no Azure needed)
cargo doc --open     # Generate and view documentation
```

### Making Changes

**To FFI bindings (sqlite-objs-sys):**
1. Update `src/lib.rs` with new extern "C" declarations
2. Update `build.rs` if new C sources added
3. Run `cargo build` to verify compilation
4. Run `cargo test` (basic linkage tests)

**To safe wrapper (sqlite-objs):**
1. Update `src/lib.rs` with safe Rust API
2. Add tests in `#[cfg(test)] mod tests`
3. Update examples if API changes
4. Run `cargo test --doc` for doctests

### Testing Strategy

**Fast validation (pre-commit):**
```bash
cd rust && cargo test
```

**Full C + Rust validation:**
```bash
# From project root
make test-unit        # C unit tests
make sanitize         # Memory safety checks
cd rust && cargo test -p sqlite-objs-sys  # Rust FFI tests
cd rust && cargo test -p sqlite-objs      # Rust API tests
```

**Integration testing:**
- For Layer 2 tests (Azurite), see main project docs: `../TEST_DOCS_INDEX.md`
- Requires: `npm install -g azurite` and `make test-integration`

**Code quality checks:**
```bash
cargo fmt              # Format code
cargo clippy           # Lint warnings
cargo doc              # Check documentation builds
cargo build --release  # Check optimized build
```

**Running examples:**
```bash
cargo run --example basic      # Smoke test (should succeed)
```

**Contributing requirement:**
Before submitting changes:
1. Run `cargo fmt` to format code
2. Run `cargo clippy` and fix warnings
3. Run `cargo test` (all tests must pass)
4. Run `cargo test --doc` (doctests pass)
5. Run from project root: `make test-unit && make sanitize`
6. Update documentation if API changes
7. Add tests for new functionality

See `../TEST_DOCS_INDEX.md` for complete testing documentation including stress tests, TCL suite, and known gaps.

## API Stability

**Current Status:** v0.1.0 (pre-release)

**Stability Promise:**
- FFI layer (`sqlite-objs-sys`): Follows C API stability
- Safe wrapper (`sqlite-objs`): May change before 1.0
- Semantic versioning after 1.0 release

## Future Work

**Potential Enhancements:**
- [ ] Integration test suite with Azurite
- [ ] Async API support (tokio runtime)
- [ ] Connection pooling for rusqlite
- [ ] Builder pattern for config
- [ ] Tracing/logging integration
- [ ] Benchmark suite
- [ ] CI/CD pipeline (GitHub Actions)
- [ ] Publish to crates.io

**Non-Goals (out of scope):**
- Pure-Rust Azure client (use C client from sqlite-objs)
- Custom SQLite features (use upstream SQLite)
- Windows support initially (focus on Unix-like first)

## Debugging

**Build failures:**
```bash
cargo clean          # Clear cached builds
cargo build -vv      # Verbose compilation output
```

**Linker errors:**
```bash
# macOS: Check OpenSSL installation
brew list openssl
pkg-config --cflags openssl
pkg-config --libs openssl

# Linux: Check dev packages
dpkg -l | grep libssl
dpkg -l | grep libcurl
```

**Runtime errors:**
```bash
RUST_BACKTRACE=1 cargo run --example basic
RUST_LOG=debug cargo test
```

## Contributing

Before submitting changes:
1. Run `cargo fmt` to format code
2. Run `cargo clippy` and fix warnings
3. Run `cargo test` (all tests must pass)
4. Update documentation if API changes
5. Add tests for new functionality

## License

MIT (matches parent project)
