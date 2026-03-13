# Security Testing Guide for sqlite-objs

> Modern C security testing tools, integration instructions, and CI recommendations.

---

## Table of Contents

1. [Static Analysis Tools](#1-static-analysis-tools)
2. [Dynamic Analysis (Sanitizers)](#2-dynamic-analysis-sanitizers)
3. [Fuzzing](#3-fuzzing)
4. [Code Coverage](#4-code-coverage)
5. [Security-Specific Checks](#5-security-specific-checks)
6. [CI Pipeline Recommendations](#6-ci-pipeline-recommendations)

---

## 1. Static Analysis Tools

### 1.1 Compiler Warnings (Already Enabled)

**What it catches:** Type mismatches, unused variables, implicit conversions, sign comparison issues.

sqlite-objs already uses `-Wall -Wextra -Wpedantic`. Recommended additions:

```makefile
# Add to CFLAGS for maximum warning coverage
CFLAGS += -Wshadow -Wconversion -Wsign-conversion -Wformat=2 \
          -Wformat-security -Wnull-dereference -Wstack-protector \
          -Wdouble-promotion -Wvla
```

**CI note:** Use `-Werror` in CI to treat warnings as errors. Do *not* enable `-Werror` for development builds.

---

### 1.2 Clang Static Analyzer (scan-build)

**What it catches:** Null pointer dereferences, use-after-free, dead stores, uninitialized reads, logic errors, resource leaks.

**Integration:**

```makefile
# Makefile target
static-analysis:
	@echo "=== Running Clang Static Analyzer ==="
	scan-build --status-bugs -o $(BUILD_DIR)/scan-results \
		$(MAKE) clean all
```

**Usage:**
```bash
# Basic scan
make static-analysis

# With increased verbosity
scan-build -v --status-bugs make clean all
```

**CI considerations:**
- `scan-build` ships with LLVM/Clang (available in all major CI images)
- `--status-bugs` returns non-zero exit code on findings → fails the CI job
- Output is HTML; upload `build/scan-results/` as a CI artifact for review

---

### 1.3 cppcheck

**What it catches:** Buffer overflows, null pointer dereference, memory leaks, uninitialized variables, portability issues, style problems. Excellent at finding issues that compilers miss.

**Integration:**

```makefile
cppcheck:
	@echo "=== Running cppcheck ==="
	cppcheck --enable=all --inconclusive --std=c11 \
		--suppress=missingIncludeSystem \
		-I$(SRC_DIR) -I$(SQLITE_DIR) \
		--error-exitcode=1 \
		--xml --output-file=$(BUILD_DIR)/cppcheck-report.xml \
		$(SRC_DIR)/ $(TEST_DIR)/
```

**Key flags:**
- `--enable=all` — Enable all check categories (style, performance, portability, etc.)
- `--inconclusive` — Report potential issues even when uncertain
- `--error-exitcode=1` — Non-zero exit on error for CI integration
- `--suppress=missingIncludeSystem` — Ignore system header warnings

**CI considerations:**
- Install via `apt-get install cppcheck` or `brew install cppcheck`
- XML output integrates with SARIF converters for GitHub Code Scanning

---

### 1.4 clang-tidy

**What it catches:** Modernization opportunities, bug-prone patterns, CERT C coding standard violations, readability issues. Highly configurable via check profiles.

**Integration:**

```makefile
clang-tidy:
	@echo "=== Running clang-tidy ==="
	clang-tidy $(SRC_DIR)/*.c \
		-checks='bugprone-*,cert-*,clang-analyzer-*,security-*,portability-*' \
		-- -std=c11 -I$(SRC_DIR) -I$(SQLITE_DIR)
```

**Recommended checks for C code:**
- `bugprone-*` — Bug-prone coding patterns
- `cert-*` — CERT C Secure Coding Standard violations
- `clang-analyzer-*` — Clang static analyzer checks
- `security-*` — Security-sensitive patterns
- `portability-*` — Portability issues

**CI considerations:**
- Ships with LLVM/Clang
- Can auto-fix some issues with `--fix` (use cautiously in CI)
- Consider a `.clang-tidy` config file for project-specific settings

---

### 1.5 PVS-Studio (Commercial)

**What it catches:** Deep inter-procedural analysis, 64-bit portability, micro-optimizations, copy-paste errors, MISRA compliance. Often finds issues other tools miss.

**Integration:**
```bash
# Requires license; free for open-source projects
pvs-studio-analyzer trace -- make clean all
pvs-studio-analyzer analyze -o build/pvs-report.log
plog-converter -a GA:1,2 -t sarif -o build/pvs-report.sarif build/pvs-report.log
```

**CI considerations:**
- Free license available for open-source projects
- SARIF output integrates with GitHub Code Scanning
- Recommended as a periodic deep scan rather than on every commit

---

## 2. Dynamic Analysis (Sanitizers)

### 2.1 AddressSanitizer (ASan)

**What it catches:** Heap/stack/global buffer overflows, use-after-free, use-after-return, double-free, memory leaks.

**This is the single highest-value tool to add.** It catches the most dangerous C bugs at runtime with ~2x slowdown.

**Integration:**

```makefile
# Makefile targets (see main Makefile for actual targets)
SANITIZE_CFLAGS = -fsanitize=address -fno-omit-frame-pointer -O1 -g
SANITIZE_LDFLAGS = -fsanitize=address

sanitize: clean
	$(MAKE) test-unit \
		CFLAGS="$(CFLAGS) $(SANITIZE_CFLAGS)" \
		LDFLAGS="$(LDFLAGS) $(SANITIZE_LDFLAGS)"
```

**Key environment variables:**
```bash
# Recommended runtime options
export ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1"
```

**What to watch for in sqlite-objs:**
- Buffer handling in `azure_buffer_t` (growable buffer in azure_client.c)
- `aData` memory buffer in VFS layer (realloc on writes past EOF)
- Dirty page bitmap allocation/deallocation
- `strncpy`/`snprintf` boundary handling in error structs
- Lease ID buffer handling (fixed 37-byte UUID strings)

**CI considerations:**
- Supported on GCC ≥4.8 and Clang ≥3.1 (all modern CI)
- Cannot combine with MemorySanitizer or ThreadSanitizer in same build
- ~2x runtime overhead, 2-3x memory overhead
- Run as a separate CI job alongside normal test run

---

### 2.2 UndefinedBehaviorSanitizer (UBSan)

**What it catches:** Signed integer overflow, null pointer dereference, misaligned pointer access, shift overflow, division by zero, unreachable code, implicit conversions losing data.

**Integration:**

```makefile
ubsan: clean
	$(MAKE) test-unit \
		CFLAGS="$(CFLAGS) -fsanitize=undefined -fno-omit-frame-pointer -O1 -g" \
		LDFLAGS="$(LDFLAGS) -fsanitize=undefined"
```

**What to watch for in sqlite-objs:**
- Integer arithmetic in `dirtyBitmapSize()` — page count calculations
- Size calculations: `offset + len` overflow checks in xRead/xWrite
- Alignment calculations: `(size + 511) & ~511` rounding
- Shift operations in dirty bitmap management

**CI considerations:**
- Very low overhead (~20%) — can run on every commit
- **Can combine with ASan** in same build: `-fsanitize=address,undefined`
- Use `-fno-sanitize-recover=all` to abort on first UB (recommended for CI)

---

### 2.3 MemorySanitizer (MSan)

**What it catches:** Use of uninitialized memory. Catches bugs that ASan and Valgrind miss.

**Integration:**

```makefile
msan: clean
	$(MAKE) test-unit \
		CC=clang \
		CFLAGS="$(CFLAGS) -fsanitize=memory -fno-omit-frame-pointer -O1 -g" \
		LDFLAGS="$(LDFLAGS) -fsanitize=memory"
```

**Important limitations:**
- **Clang-only** (GCC does not support MSan)
- **All linked libraries must be MSan-instrumented** — this means libcurl and OpenSSL cannot be used with MSan unless rebuilt with MSan. Best suited for unit tests using the mock layer.
- Cannot combine with ASan

**What to watch for in sqlite-objs:**
- `azure_buffer_t` — are read buffers fully initialized before use?
- `azure_error_t` — are all fields set on error paths?
- VFS file descriptor (`sqlite-objsFile`) — are all struct fields initialized on xOpen?

**CI considerations:**
- Run only on unit tests (mock layer, no libcurl dependency)
- Separate CI job from ASan
- May produce false positives from uninstrumented system libraries

---

### 2.4 ThreadSanitizer (TSan)

**What it catches:** Data races, deadlocks, thread-unsafe patterns.

**Integration:**

```makefile
tsan: clean
	$(MAKE) test-unit \
		CC=clang \
		CFLAGS="$(CFLAGS) -fsanitize=thread -fno-omit-frame-pointer -O1 -g" \
		LDFLAGS="$(LDFLAGS) -fsanitize=thread"
```

**Relevance to sqlite-objs:**
- SQLite is compiled with `SQLITE_THREADSAFE=1` (serialized mode)
- The VFS layer itself may be called from multiple threads
- Lease renewal timing (`leaseRenewIfNeeded`) could race with lock operations
- Lower priority than ASan/UBSan unless concurrent access is a primary use case

**CI considerations:**
- ~5-15x slowdown, ~5-10x memory overhead
- Cannot combine with ASan or MSan
- Best as a periodic CI job (nightly) rather than per-commit

---

## 3. Fuzzing

### 3.1 libFuzzer (Recommended First Fuzzer)

**What it catches:** Crashes, hangs, memory errors, undefined behavior in input parsing code. Particularly effective for:
- XML error response parsing (`azure_parse_error_xml`)
- URL construction (`build_blob_url`)
- Base64 encode/decode (`azure_base64_encode/decode`)
- SAS token handling

**Integration:**

```c
// test/fuzz_xml_parse.c — Example fuzz target
#include "azure_client_impl.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Null-terminate the input for string functions
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    azure_error_t err = {0};
    azure_parse_error_xml(input, &err);

    free(input);
    return 0;
}
```

```makefile
fuzz-xml-parse:
	clang -g -O1 -fsanitize=fuzzer,address \
		-I$(SRC_DIR) -I$(SQLITE_DIR) \
		test/fuzz_xml_parse.c $(SRC_DIR)/azure_error.c \
		-o $(BUILD_DIR)/fuzz_xml_parse
	@echo "Run: $(BUILD_DIR)/fuzz_xml_parse corpus/xml_parse/"
```

**Recommended fuzz targets for sqlite-objs (priority order):**

| Target | File | Risk |
|--------|------|------|
| XML error parsing | `azure_error.c:azure_parse_error_xml` | High — `strstr()` on untrusted server response |
| Base64 decode | `azure_auth.c:azure_base64_decode` | High — processes account keys |
| URL construction | `azure_client.c:build_blob_url` | Medium — user-provided blob names |
| Header parsing | `azure_client.c:curl_header_cb` | Medium — untrusted server headers |
| Full VFS path | `sqlite_objs_vfs.c:sqlite-objsFullPathname` | Low — path construction |

**CI considerations:**
- Requires Clang (libFuzzer is built into Clang)
- Run with a time limit: `./fuzz_target -max_total_time=300` (5 min per target)
- Store and check in corpus files for regression testing
- Combine with ASan for maximum bug detection

---

### 3.2 AFL++ (Alternative)

**What it catches:** Same class of bugs as libFuzzer, with different mutation strategies. Often finds different bugs than libFuzzer.

**Integration:**

```makefile
fuzz-afl:
	AFL_USE_ASAN=1 afl-cc -g -O1 \
		-I$(SRC_DIR) -I$(SQLITE_DIR) \
		test/fuzz_harness.c $(SRC_DIR)/azure_error.c \
		-o $(BUILD_DIR)/fuzz_afl_harness
	@echo "Run: afl-fuzz -i corpus/ -o findings/ -- $(BUILD_DIR)/fuzz_afl_harness"
```

**CI considerations:**
- Install via `apt-get install afl++` or build from source
- Requires longer run times than libFuzzer for effectiveness (hours, not minutes)
- Best as a nightly/weekly CI job or local development tool
- Use `afl-cmin` to minimize corpus for regression testing

---

### 3.3 Honggfuzz

**What it catches:** Similar to AFL++, with hardware-based feedback on Intel CPUs. Good at finding edge cases in structured data parsing.

**Integration:**
```bash
hfuzz-cc -g -O1 -fsanitize=address \
    -I src/ -I sqlite-autoconf-3520000/ \
    test/fuzz_harness.c src/azure_error.c \
    -o build/fuzz_hongg
honggfuzz -i corpus/ -o findings/ -- ./build/fuzz_hongg
```

**CI considerations:**
- Lower priority than libFuzzer/AFL++ for this project
- Worth evaluating if libFuzzer doesn't find issues
- Good Linux perf counter integration for coverage feedback

---

## 4. Code Coverage

### 4.1 gcov + lcov (GCC)

**What it measures:** Line coverage, branch coverage, function coverage. Shows which lines of code are exercised by tests.

**Integration:**

```makefile
coverage: clean
	$(MAKE) test-unit \
		CFLAGS="$(CFLAGS) --coverage -O0 -g" \
		LDFLAGS="$(LDFLAGS) --coverage"
	lcov --capture --directory $(BUILD_DIR) --output-file $(BUILD_DIR)/coverage.info \
		--rc branch_coverage=1
	lcov --remove $(BUILD_DIR)/coverage.info '*/sqlite3.*' '*/test/*' \
		--output-file $(BUILD_DIR)/coverage-filtered.info --rc branch_coverage=1
	genhtml $(BUILD_DIR)/coverage-filtered.info \
		--output-directory $(BUILD_DIR)/coverage-report \
		--rc branch_coverage=1
	@echo "Coverage report: $(BUILD_DIR)/coverage-report/index.html"
```

**What to look for:**
- Are all error paths in `sqlite_objs_vfs.c` exercised? (lease failures, allocation failures, alignment errors)
- Are all `azure_err_t` codes handled in the VFS error mapping?
- Is the retry logic in `execute_with_retry()` tested for each retry scenario?
- Are all branches of `xOpen` (main db, journal, temp, other) covered?

**CI considerations:**
- `--coverage` adds ~15% overhead
- Use `lcov --remove` to exclude sqlite3.c (external code) and test files
- Upload HTML report as CI artifact
- Consider Codecov or Coveralls integration for PR coverage diffs
- Set minimum thresholds: aim for ≥80% line coverage on src/ files

---

### 4.2 llvm-cov (Clang)

**What it measures:** Same as gcov but with Clang's source-based coverage (more accurate branch coverage).

**Integration:**

```makefile
coverage-llvm: clean
	$(MAKE) test-unit \
		CC=clang \
		CFLAGS="$(CFLAGS) -fprofile-instr-generate -fcoverage-mapping -O0 -g" \
		LDFLAGS="$(LDFLAGS) -fprofile-instr-generate"
	LLVM_PROFILE_FILE="$(BUILD_DIR)/test.profraw" $(BUILD_DIR)/test_main
	llvm-profdata merge -sparse $(BUILD_DIR)/test.profraw \
		-o $(BUILD_DIR)/test.profdata
	llvm-cov show $(BUILD_DIR)/test_main \
		-instr-profile=$(BUILD_DIR)/test.profdata \
		-format=html -output-dir=$(BUILD_DIR)/llvm-coverage \
		-ignore-filename-regex='sqlite3|test/'
	@echo "Coverage report: $(BUILD_DIR)/llvm-coverage/index.html"
```

**CI considerations:**
- Clang-only
- Produces more accurate branch coverage than gcov
- Can export to LCOV format for Codecov/Coveralls

---

## 5. Security-Specific Checks

### 5.1 Format String Vulnerabilities

**Risk in sqlite-objs:** Moderate. Error messages from Azure are placed into fixed-size buffers. If any user-controlled string were passed as a format string argument, it could be exploited.

**Detection:**
- Compiler: `-Wformat=2 -Wformat-security` (already recommended above)
- clang-tidy: `cert-err33-c` check
- cppcheck: detects format string misuse

**Locations to audit:**
- `snprintf` calls in `azure_error.c` (error message formatting)
- `sqlite3_snprintf` calls in `sqlite_objs_vfs.c` (path construction)
- Any `printf`-family call where the format string could be influenced by external data

---

### 5.2 Buffer Overflow Protection

**Risk in sqlite-objs:** High. Fixed-size buffers are used throughout:
- `azure_error_t.error_code[128]`
- `azure_error_t.error_message[256]`
- `azure_error_t.request_id[64]`
- Lease ID buffers (37 bytes for UUID format)

**Detection:**
- ASan: Catches at runtime
- Compiler: `-D_FORTIFY_SOURCE=2` (enables runtime buffer overflow checks in glibc)
- Stack protector: `-fstack-protector-strong`

```makefile
# Add to CFLAGS for hardened builds
HARDEN_CFLAGS = -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
                -fPIE -Wl,-z,relro,-z,now
```

**Locations to audit:**
- `strncpy` into fixed-size error buffers — are lengths correct?
- `curl_header_cb` — does it bounds-check header values before copying?
- `extract_xml_tag` — does it handle arbitrarily long XML values?
- `build_blob_url` — does URL construction handle long account/container/blob names?

---

### 5.3 Integer Overflow Risks

**Risk in sqlite-objs:** Medium. Size calculations involve `int64_t` and `size_t` arithmetic:
- `dirtyBitmapSize()` — page count from file size
- `offset + len` in xRead/xWrite — could overflow on 32-bit
- `(nData + 511) & ~511` alignment rounding
- Page blob size calculations

**Detection:**
- UBSan: `-fsanitize=undefined` catches signed overflow
- `-fsanitize=integer` (Clang) catches unsigned overflow too
- clang-tidy: `bugprone-narrowing-conversions`, `cert-int32-c`
- Manual review of all arithmetic on sizes/offsets

---

### 5.4 Credential Handling

**Risk in sqlite-objs:** High. The client handles Azure Storage account keys and SAS tokens.

**Checks to implement:**
- **No credential logging:** Grep for `printf`/`fprintf`/`sqlite3_log` calls near credential variables
- **Memory zeroing:** Secrets should be zeroed with `explicit_bzero()` or `memset_s()` before free
- **No credentials in error messages:** Ensure `azure_error_t` fields never contain key material

**Detection:**
```bash
# Manual audit command — check for potential credential leaks
grep -n 'printf\|fprintf\|sqlite3_log\|AZURE_LOG' src/*.c | \
    grep -i 'key\|token\|secret\|credential\|password\|auth'
```

---

### 5.5 Hardening Compiler Flags

Summary of all recommended security-related compiler flags:

```makefile
# Full security hardening flags
SECURITY_CFLAGS = \
    -Wall -Wextra -Wpedantic \
    -Wshadow -Wconversion -Wsign-conversion \
    -Wformat=2 -Wformat-security \
    -Wnull-dereference -Wstack-protector \
    -D_FORTIFY_SOURCE=2 \
    -fstack-protector-strong \
    -fno-strict-aliasing \
    -fPIE
```

---

## 6. CI Pipeline Recommendations

### 6.1 Recommended GitHub Actions Matrix

```yaml
# .github/workflows/ci.yml (example structure)
name: CI
on: [push, pull_request]

jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build and test
        run: make test-unit

  sanitizers:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        sanitizer: [address, undefined]
    steps:
      - uses: actions/checkout@v4
      - name: Test with ${{ matrix.sanitizer }} sanitizer
        run: make sanitize-${{ matrix.sanitizer }}

  static-analysis:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: cppcheck
        run: |
          sudo apt-get install -y cppcheck
          make cppcheck
      - name: scan-build
        run: make static-analysis

  coverage:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build with coverage
        run: |
          sudo apt-get install -y lcov
          make coverage
      - uses: actions/upload-artifact@v4
        with:
          name: coverage-report
          path: build/coverage-report/

  integration:
    runs-on: ubuntu-latest
    services:
      azurite:
        image: mcr.microsoft.com/azure-storage/azurite
        ports: ['10000:10000']
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: sudo apt-get install -y libcurl4-openssl-dev libssl-dev
      - name: Integration tests
        run: make test-integration
```

### 6.2 Priority Order for Implementation

| Priority | Tool | Effort | Value | Run Frequency |
|----------|------|--------|-------|---------------|
| **P0** | AddressSanitizer + UBSan | Low | Very High | Every commit |
| **P0** | Compiler warnings (`-Werror` in CI) | Trivial | High | Every commit |
| **P1** | Code coverage (gcov/lcov) | Low | High | Every commit |
| **P1** | scan-build | Low | High | Every commit |
| **P1** | cppcheck | Low | Medium | Every commit |
| **P2** | libFuzzer targets | Medium | High | Nightly |
| **P2** | clang-tidy | Low | Medium | Every commit |
| **P3** | MemorySanitizer | Medium | Medium | Weekly |
| **P3** | ThreadSanitizer | Low | Low-Medium | Weekly |
| **P4** | AFL++ / honggfuzz | High | Medium | Weekly/Manual |
| **P4** | PVS-Studio | Medium | Medium | Monthly |

### 6.3 Quick Start

Add these to your development workflow immediately:

```bash
# 1. Run tests with sanitizers (catches ~80% of memory bugs)
make sanitize

# 2. Generate coverage report (find untested code)
make coverage
open build/coverage-report/index.html

# 3. Static analysis (catch bugs without running code)
make static-analysis
```
