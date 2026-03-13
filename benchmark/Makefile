# Makefile for azqlite benchmark harness
#
# This benchmark compares local SQLite performance against azqlite
# (Azure blob-backed SQLite) using the speedtest1 test harness.
#
# Targets:
#   all              Build benchmark binary (stub Azure client)
#   all-production   Build with real Azure client (requires libcurl + OpenSSL)
#   run              Run both local and Azure benchmarks
#   run-local        Run local benchmark only
#   run-azure        Run Azure benchmark only
#   clean            Remove build artifacts

# ---------- Configuration ----------

# Parent directory paths
PARENT_DIR     = ..
SRC_DIR        = $(PARENT_DIR)/src
BUILD_DIR      = $(PARENT_DIR)/build
SQLITE_DIR     = $(PARENT_DIR)/sqlite-autoconf-3520000

# Benchmark build directory
BENCH_BUILD    = build

# ---------- Toolchain ----------

CC       ?= cc
CFLAGS    = -Wall -Wextra -std=c11 -O2
CFLAGS   += -DSQLITE_THREADSAFE=1
CFLAGS   += -DSQLITE_ENABLE_FTS5
CFLAGS   += -DSQLITE_ENABLE_JSON1
CFLAGS   += -I$(SRC_DIR) -I$(SQLITE_DIR) -I.

# Platform-specific linker flags
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS  = -lpthread -lm
else
    LDFLAGS  = -lpthread -ldl -lm
endif

# pkg-config support for production dependencies
PKG_CONFIG ?= pkg-config
OPENSSL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null || echo "")
OPENSSL_LDFLAGS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo "-lssl -lcrypto")
CURL_CFLAGS     := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null || echo "")
CURL_LDFLAGS    := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || echo "-lcurl")

CFLAGS_PROD  = $(CFLAGS) $(OPENSSL_CFLAGS) $(CURL_CFLAGS)
LDFLAGS_PROD = $(LDFLAGS) $(CURL_LDFLAGS) $(OPENSSL_LDFLAGS)

# ---------- Object files ----------

# SQLite amalgamation
SQLITE_OBJ = $(BUILD_DIR)/sqlite3.o

# VFS objects (stub build)
VFS_STUB_OBJS = $(BUILD_DIR)/azqlite_vfs.o \
                $(BUILD_DIR)/azqlite_cache.o \
                $(BUILD_DIR)/azure_client_stub.o

# VFS objects (production build)
VFS_PROD_OBJS = $(BUILD_DIR)/azqlite_vfs.o \
                $(BUILD_DIR)/azqlite_cache.o \
                $(BUILD_DIR)/azure_client.o \
                $(BUILD_DIR)/azure_auth.o \
                $(BUILD_DIR)/azure_error.o

# Benchmark source
BENCH_SRC = benchmark.c

# Binaries
BENCHMARK_BIN = benchmark
SPEEDTEST1_BIN = speedtest1
SPEEDTEST1_AZURE_BIN = speedtest1-azure

# ---------- Build targets ----------

.PHONY: all all-production run run-local run-azure clean help

all: $(BENCHMARK_BIN) $(SPEEDTEST1_BIN)

# Build simple benchmark harness (just wraps speedtest1 executables)
$(BENCHMARK_BIN): $(BENCH_SRC) | $(BENCH_BUILD)
	$(CC) $(CFLAGS) -o $@ $(BENCH_SRC)

# Build speedtest1 with default SQLite
# -w suppresses warnings from third-party speedtest1.c (SQLite test harness)
$(SPEEDTEST1_BIN): speedtest1.c $(SQLITE_OBJ) | $(BENCH_BUILD)
	$(CC) $(CFLAGS) -w -o $@ speedtest1.c $(SQLITE_OBJ) $(LDFLAGS)

# Build speedtest1 with azqlite VFS (stub)
# -w needed because speedtest1_wrapper.c #includes third-party speedtest1.c
$(SPEEDTEST1_AZURE_BIN): speedtest1_wrapper.c speedtest1.c $(SQLITE_OBJ) $(VFS_STUB_OBJS) | $(BENCH_BUILD)
	$(CC) $(CFLAGS) -w -o $@ speedtest1_wrapper.c \
		$(SQLITE_OBJ) $(VFS_STUB_OBJS) $(LDFLAGS)

# Build all with production Azure client
# -w needed because speedtest1_wrapper.c #includes third-party speedtest1.c
all-production: $(BENCHMARK_BIN) $(SPEEDTEST1_BIN) $(SQLITE_OBJ) $(VFS_PROD_OBJS) | $(BENCH_BUILD)
	$(CC) $(CFLAGS_PROD) -w -o $(SPEEDTEST1_AZURE_BIN) speedtest1_wrapper.c \
		$(SQLITE_OBJ) $(VFS_PROD_OBJS) $(LDFLAGS_PROD)

# Build directory
$(BENCH_BUILD):
	@mkdir -p $(BENCH_BUILD)

# Ensure parent objects are built
$(SQLITE_OBJ):
	@echo "Building SQLite amalgamation..."
	@cd $(PARENT_DIR) && $(MAKE) $(BUILD_DIR)/sqlite3.o

$(BUILD_DIR)/azqlite_vfs.o:
	@echo "Building VFS layer..."
	@cd $(PARENT_DIR) && $(MAKE) $(BUILD_DIR)/azqlite_vfs.o

$(BUILD_DIR)/azure_client_stub.o:
	@echo "Building Azure client stub..."
	@cd $(PARENT_DIR) && $(MAKE) $(BUILD_DIR)/azure_client_stub.o

$(BUILD_DIR)/azure_client.o $(BUILD_DIR)/azure_auth.o $(BUILD_DIR)/azure_error.o:
	@echo "Building Azure client (production)..."
	@cd $(PARENT_DIR) && $(MAKE) all-production

# ---------- Run targets ----------

# Run both benchmarks
run: $(BENCHMARK_BIN)
	@./$(BENCHMARK_BIN)

# Run local benchmark only
run-local: $(BENCHMARK_BIN)
	@./$(BENCHMARK_BIN) --local-only

# Run Azure benchmark only (requires env vars)
run-azure: $(BENCHMARK_BIN)
	@./$(BENCHMARK_BIN) --azure-only

# ---------- Clean ----------

clean:
	rm -rf $(BENCH_BUILD)
	rm -f $(BENCHMARK_BIN) $(SPEEDTEST1_BIN) $(SPEEDTEST1_AZURE_BIN)
	rm -f benchmark_local.db benchmark_local.db-journal
	rm -f benchmark_azure.db benchmark_azure.db-journal
	rm -f benchmark_old.c test_simple test_simple.c

# ---------- Help ----------

help:
	@echo "Benchmark Makefile Targets:"
	@echo "  all              Build benchmark binary (stub Azure client)"
	@echo "  all-production   Build with real Azure client (requires libcurl + OpenSSL)"
	@echo "  run              Run both local and Azure benchmarks"
	@echo "  run-local        Run local benchmark only"
	@echo "  run-azure        Run Azure benchmark only"
	@echo "  clean            Remove build artifacts"
	@echo ""
	@echo "Environment variables for Azure mode:"
	@echo "  AZURE_STORAGE_ACCOUNT   Storage account name"
	@echo "  AZURE_STORAGE_KEY       Shared key (or use SAS)"
	@echo "  AZURE_STORAGE_SAS       SAS token (or use KEY)"
	@echo "  AZURE_STORAGE_CONTAINER Container name"
	@echo ""
	@echo "Example:"
	@echo "  make all-production"
	@echo "  export AZURE_STORAGE_ACCOUNT=myaccount"
	@echo "  export AZURE_STORAGE_KEY=mykey"
	@echo "  export AZURE_STORAGE_CONTAINER=mybenchmarks"
	@echo "  ./benchmark --size 50"
