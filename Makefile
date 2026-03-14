# Makefile for sqlite-objs — SQLite VFS backed by Azure Blob Storage
#
# Targets:
#   all              Build everything: library, shell, benchmarks
#   shell            Build just the shell binary
#   benchmarks       Build speedtest1 benchmarks
#   tpcc             Build TPC-C benchmarks
#   test-unit        Build and run Layer 1 unit tests
#   test-integration Build and run Layer 2 integration tests (requires Azurite)
#   test             Run test-unit + test-integration
#   clean            Remove all build artifacts

# ---------- Directories ----------

SQLITE_DIR    = sqlite-autoconf-3520000
SRC_DIR       = src
TEST_DIR      = test
BENCH_DIR     = benchmark
TPCC_DIR      = benchmark/tpcc
BUILD_DIR     = build

# ---------- Toolchain ----------

CC       ?= cc
AR       ?= ar
CFLAGS    = -Wall -Wextra -Wpedantic -std=c11 -O2
CFLAGS   += -DSQLITE_THREADSAFE=1
CFLAGS   += -DSQLITE_ENABLE_FTS5
CFLAGS   += -DSQLITE_ENABLE_JSON1
# Expose POSIX declarations hidden by -std=c11.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CFLAGS   += -D_DARWIN_C_SOURCE
else
CFLAGS   += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
endif

CFLAGS   += -I$(SRC_DIR) -I$(SQLITE_DIR)

ifeq ($(UNAME_S),Darwin)
    LDFLAGS  = -lpthread -lm
else
    LDFLAGS  = -lpthread -ldl -lm
endif

# External dependencies (libcurl + OpenSSL) via pkg-config
PKG_CONFIG ?= pkg-config
OPENSSL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null || echo "")
OPENSSL_LDFLAGS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo "-lssl -lcrypto")
CURL_CFLAGS     := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null || echo "")
CURL_LDFLAGS    := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || echo "-lcurl")

CFLAGS_ALL   = $(CFLAGS) $(OPENSSL_CFLAGS) $(CURL_CFLAGS)
LDFLAGS_ALL  = $(LDFLAGS) $(CURL_LDFLAGS) $(OPENSSL_LDFLAGS)

# Test-specific CFLAGS
TEST_CFLAGS = $(CFLAGS) $(OPENSSL_CFLAGS) -I$(TEST_DIR)

# ---------- Core objects ----------

SQLITE_OBJ  = $(BUILD_DIR)/sqlite3.o

VFS_OBJS    = $(BUILD_DIR)/sqlite_objs_vfs.o \
              $(BUILD_DIR)/azure_client.o \
              $(BUILD_DIR)/azure_auth.o \
              $(BUILD_DIR)/azure_error.o

LIB_OBJS    = $(SQLITE_OBJ) $(VFS_OBJS)
LIBRARY      = $(BUILD_DIR)/libsqlite_objs.a

SHELL_BIN   = sqlite-objs-shell

# Unit test objects (stub client — no curl/OpenSSL needed for mocks)
MOCK_OBJ    = $(BUILD_DIR)/mock_azure_ops.o
TEST_OBJS   = $(SQLITE_OBJ) $(BUILD_DIR)/sqlite_objs_vfs.o \
              $(BUILD_DIR)/azure_client_stub.o $(MOCK_OBJ) \
              $(BUILD_DIR)/azure_auth.o $(BUILD_DIR)/azure_error.o

# Benchmark binaries
SPEEDTEST1_BIN       = $(BENCH_DIR)/speedtest1
SPEEDTEST1_AZURE_BIN = $(BENCH_DIR)/speedtest1-azure
BENCHMARK_BIN        = $(BENCH_DIR)/benchmark

# TPC-C binaries
TPCC_LOCAL_BIN = $(TPCC_DIR)/tpcc-local
TPCC_AZURE_BIN = $(TPCC_DIR)/tpcc-azure
TPCC_BUILD     = $(TPCC_DIR)/build

# TPC-C objects
TPCC_LOCAL_OBJS = $(TPCC_BUILD)/tpcc_local.o \
                  $(TPCC_BUILD)/tpcc_load_local.o \
                  $(TPCC_BUILD)/tpcc_txn_local.o

TPCC_AZURE_OBJS = $(TPCC_BUILD)/tpcc_azure.o \
                  $(TPCC_BUILD)/tpcc_load_azure.o \
                  $(TPCC_BUILD)/tpcc_txn_azure.o

# ---------- Default target ----------

.PHONY: all shell benchmarks tpcc clean test test-unit test-integration \
        sanitize coverage help

all: $(LIBRARY) shell benchmarks tpcc

# ---------- Build directories ----------

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(TPCC_BUILD):
	@mkdir -p $(TPCC_BUILD)

# ---------- SQLite amalgamation ----------

$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -w -c -o $@ $<

# ---------- VFS + Azure client ----------

$(BUILD_DIR)/sqlite_objs_vfs.o: $(SRC_DIR)/sqlite_objs_vfs.c $(SRC_DIR)/sqlite_objs.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

$(BUILD_DIR)/azure_client.o: $(SRC_DIR)/azure_client.c $(SRC_DIR)/azure_client_impl.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

$(BUILD_DIR)/azure_auth.o: $(SRC_DIR)/azure_auth.c $(SRC_DIR)/azure_client_impl.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

$(BUILD_DIR)/azure_error.o: $(SRC_DIR)/azure_error.c $(SRC_DIR)/azure_client_impl.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) -c -o $@ $<

# Stub client — only used by unit tests
$(BUILD_DIR)/azure_client_stub.o: $(SRC_DIR)/azure_client_stub.c $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------- Static library ----------

$(LIBRARY): $(LIB_OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $(LIB_OBJS)

# ---------- Shell ----------

shell: $(SHELL_BIN)

$(SHELL_BIN): $(SRC_DIR)/sqlite_objs_shell.c $(LIBRARY)
	$(CC) $(CFLAGS_ALL) -w -DSQLITE_THREADSAFE=1 \
		-o $@ $< $(LIB_OBJS) $(LDFLAGS_ALL)

# ---------- Benchmarks (speedtest1) ----------

benchmarks: $(BENCHMARK_BIN) $(SPEEDTEST1_BIN) $(SPEEDTEST1_AZURE_BIN)

$(BENCHMARK_BIN): $(BENCH_DIR)/benchmark.c
	$(CC) $(CFLAGS) -I$(BENCH_DIR) -o $@ $<

$(SPEEDTEST1_BIN): $(BENCH_DIR)/speedtest1.c $(SQLITE_OBJ)
	$(CC) $(CFLAGS) -w -o $@ $< $(SQLITE_OBJ) $(LDFLAGS)

$(SPEEDTEST1_AZURE_BIN): $(BENCH_DIR)/speedtest1_wrapper.c $(BENCH_DIR)/speedtest1.c $(LIBRARY)
	$(CC) $(CFLAGS_ALL) -w -I$(BENCH_DIR) -o $@ $< $(LIB_OBJS) $(LDFLAGS_ALL)

# ---------- TPC-C ----------

tpcc: $(TPCC_LOCAL_BIN) $(TPCC_AZURE_BIN)

$(TPCC_LOCAL_BIN): $(TPCC_LOCAL_OBJS) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) -o $@ $(TPCC_LOCAL_OBJS) $(SQLITE_OBJ) $(LDFLAGS)

$(TPCC_AZURE_BIN): $(TPCC_AZURE_OBJS) $(SQLITE_OBJ) $(VFS_OBJS)
	$(CC) $(CFLAGS_ALL) -DSQLITE_OBJS_VFS_AVAILABLE \
		-o $@ $(TPCC_AZURE_OBJS) $(SQLITE_OBJ) $(VFS_OBJS) $(LDFLAGS_ALL)

$(TPCC_BUILD)/%_local.o: $(TPCC_DIR)/%.c $(TPCC_DIR)/tpcc_schema.h | $(TPCC_BUILD)
	$(CC) $(CFLAGS) -I$(TPCC_DIR) -c -o $@ $<

$(TPCC_BUILD)/%_azure.o: $(TPCC_DIR)/%.c $(TPCC_DIR)/tpcc_schema.h | $(TPCC_BUILD)
	$(CC) $(CFLAGS_ALL) -I$(TPCC_DIR) -DSQLITE_OBJS_VFS_AVAILABLE -c -o $@ $<

# ---------- Mock object (for unit tests) ----------

$(MOCK_OBJ): $(TEST_DIR)/mock_azure_ops.c $(TEST_DIR)/mock_azure_ops.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ---------- Tests ----------

test-unit: $(BUILD_DIR)/test_main
	@echo "=== Running unit tests ==="
	$(BUILD_DIR)/test_main
	@echo "=== All unit tests passed ==="

$(BUILD_DIR)/test_main: $(TEST_DIR)/test_main.c $(TEST_DIR)/test_vfs.c $(TEST_DIR)/test_azure_client.c $(TEST_DIR)/test_coalesce.c $(TEST_DIR)/test_wal.c $(TEST_DIR)/test_uri.c $(TEST_DIR)/mock_azure_ops.h $(TEST_DIR)/test_harness.h $(TEST_OBJS) | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -DENABLE_VFS_INTEGRATION -DENABLE_WAL_TESTS -DENABLE_AZURE_CLIENT_TESTS \
		-o $@ $(TEST_DIR)/test_main.c $(TEST_OBJS) $(LDFLAGS) $(OPENSSL_LDFLAGS)

test-integration: $(BUILD_DIR)/test_integration
	@echo "=== Running integration tests (requires Azurite) ==="
	@./test/run-integration.sh

$(BUILD_DIR)/test_integration: $(TEST_DIR)/test_integration.c $(TEST_DIR)/test_harness.h $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ $< $(LIB_OBJS) $(LDFLAGS_ALL)

test: test-unit test-integration

# ---------- Clean ----------

clean:
	rm -rf $(BUILD_DIR) $(TPCC_BUILD)
	rm -f $(SHELL_BIN)
	rm -f $(BENCHMARK_BIN) $(SPEEDTEST1_BIN) $(SPEEDTEST1_AZURE_BIN)
	rm -f $(TPCC_LOCAL_BIN) $(TPCC_AZURE_BIN)

# ---------- Sanitizer build ----------

SANITIZE_CFLAGS  = -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g \
                   -fno-sanitize-recover=all
SANITIZE_LDFLAGS = -fsanitize=address,undefined

sanitize: clean
	@echo "=== Building with AddressSanitizer + UBSan ==="
	$(MAKE) test-unit \
		CFLAGS="$(CFLAGS) $(SANITIZE_CFLAGS)" \
		LDFLAGS="$(LDFLAGS) $(SANITIZE_LDFLAGS)"
	@echo "=== Sanitizer tests passed ==="

# ---------- Coverage build ----------

coverage: clean
	@echo "=== Building with coverage instrumentation ==="
	$(MAKE) test-unit \
		CFLAGS="$(CFLAGS) --coverage -O0 -g" \
		LDFLAGS="$(LDFLAGS) --coverage"
	@echo "=== Generating coverage report ==="
	lcov --capture --directory $(BUILD_DIR) --output-file $(BUILD_DIR)/coverage.info \
		--rc branch_coverage=1 --quiet
	lcov --remove $(BUILD_DIR)/coverage.info '*/sqlite3.*' '*/test/*' \
		--output-file $(BUILD_DIR)/coverage-filtered.info --rc branch_coverage=1 --quiet
	genhtml $(BUILD_DIR)/coverage-filtered.info \
		--output-directory $(BUILD_DIR)/coverage-report \
		--rc branch_coverage=1 --quiet
	@echo "=== Coverage report: $(BUILD_DIR)/coverage-report/index.html ==="

# ---------- Help ----------

help:
	@echo "sqlite-objs Build Targets:"
	@echo "  all              Build everything (library, shell, benchmarks, tpcc)"
	@echo "  shell            Build the sqlite-objs shell"
	@echo "  benchmarks       Build speedtest1 benchmarks"
	@echo "  tpcc             Build TPC-C benchmarks (local + azure)"
	@echo "  test-unit        Run unit tests"
	@echo "  test-integration Run integration tests (requires Azurite)"
	@echo "  test             Run all tests"
	@echo "  sanitize         Run unit tests with ASan + UBSan"
	@echo "  coverage         Generate code coverage report"
	@echo "  clean            Remove all build artifacts"
