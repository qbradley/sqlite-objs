# Makefile for TPC-C benchmark
#
# Builds TPC-C OLTP benchmark for local SQLite and azqlite (Azure blob-backed)

# ---------- Configuration ----------

# Parent directory paths
PARENT_DIR     = ../..
SRC_DIR        = $(PARENT_DIR)/src
BUILD_DIR      = $(PARENT_DIR)/build
SQLITE_DIR     = $(PARENT_DIR)/sqlite-autoconf-3520000

# TPC-C build directory
TPCC_BUILD     = build

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

# VFS objects (stub build - local only)
VFS_STUB_OBJS = $(BUILD_DIR)/azqlite_vfs.o \
                $(BUILD_DIR)/azure_client_stub.o

# VFS objects (production build - with Azure support)
VFS_PROD_OBJS = $(BUILD_DIR)/azqlite_vfs.o \
                $(BUILD_DIR)/azure_client.o \
                $(BUILD_DIR)/azure_auth.o \
                $(BUILD_DIR)/azure_error.o

# TPC-C source files
TPCC_SRCS = tpcc.c tpcc_load.c tpcc_txn.c

# TPC-C objects (local version - no VFS)
TPCC_LOCAL_OBJS = $(TPCC_BUILD)/tpcc_local.o \
                  $(TPCC_BUILD)/tpcc_load_local.o \
                  $(TPCC_BUILD)/tpcc_txn_local.o

# TPC-C objects (Azure version - with VFS)
TPCC_AZURE_OBJS = $(TPCC_BUILD)/tpcc_azure.o \
                  $(TPCC_BUILD)/tpcc_load_azure.o \
                  $(TPCC_BUILD)/tpcc_txn_azure.o

# Binaries
TPCC_LOCAL_BIN = tpcc-local
TPCC_AZURE_BIN = tpcc-azure

# ---------- Build targets ----------

.PHONY: all all-production clean help run-local run-azure

# Default: build local-only version
all: $(TPCC_LOCAL_BIN)

# Build both local and Azure versions
all-production: $(TPCC_LOCAL_BIN) $(TPCC_AZURE_BIN)

# Build TPC-C local version
$(TPCC_LOCAL_BIN): $(TPCC_LOCAL_OBJS) $(SQLITE_OBJ) | $(TPCC_BUILD)
	$(CC) $(CFLAGS) -o $@ $(TPCC_LOCAL_OBJS) $(SQLITE_OBJ) $(LDFLAGS)

# Build TPC-C Azure version
$(TPCC_AZURE_BIN): $(TPCC_AZURE_OBJS) $(SQLITE_OBJ) $(VFS_PROD_OBJS) | $(TPCC_BUILD)
	$(CC) $(CFLAGS_PROD) -DAZQLITE_VFS_AVAILABLE -o $@ $(TPCC_AZURE_OBJS) $(SQLITE_OBJ) $(VFS_PROD_OBJS) $(LDFLAGS_PROD)

# TPC-C object files (local version - no VFS flag)
$(TPCC_BUILD)/%_local.o: %.c tpcc_schema.h | $(TPCC_BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# TPC-C object files (Azure version - with VFS flag)
$(TPCC_BUILD)/%_azure.o: %.c tpcc_schema.h | $(TPCC_BUILD)
	$(CC) $(CFLAGS_PROD) -DAZQLITE_VFS_AVAILABLE -c -o $@ $<

# Build directory
$(TPCC_BUILD):
	@mkdir -p $(TPCC_BUILD)

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

# Run local benchmark
run-local: $(TPCC_LOCAL_BIN)
	@./$(TPCC_LOCAL_BIN) --local --warehouses 1 --duration 60

# Run Azure benchmark (requires env vars)
run-azure: $(TPCC_AZURE_BIN)
	@./$(TPCC_AZURE_BIN) --azure --warehouses 1 --duration 60

# ---------- Clean ----------

clean:
	rm -rf $(TPCC_BUILD)
	rm -f $(TPCC_LOCAL_BIN) $(TPCC_AZURE_BIN)
	rm -f tpcc_local.db tpcc_local.db-journal
	rm -f tpcc.db tpcc.db-journal

# ---------- Help ----------

help:
	@echo "TPC-C Benchmark Makefile Targets:"
	@echo "  all              Build local-only version"
	@echo "  all-production   Build both local and Azure versions"
	@echo "  run-local        Run local benchmark"
	@echo "  run-azure        Run Azure benchmark (requires env vars)"
	@echo "  clean            Remove build artifacts"
	@echo ""
	@echo "Environment variables for Azure mode:"
	@echo "  AZURE_STORAGE_ACCOUNT   Storage account name"
	@echo "  AZURE_STORAGE_KEY       Shared key (or use SAS)"
	@echo "  AZURE_STORAGE_SAS       SAS token (or use KEY)"
	@echo "  AZURE_STORAGE_CONTAINER Container name"
	@echo ""
	@echo "Examples:"
	@echo "  make all"
	@echo "  ./tpcc-local --local --warehouses 1 --duration 60"
	@echo ""
	@echo "  make all-production"
	@echo "  export AZURE_STORAGE_ACCOUNT=myaccount"
	@echo "  export AZURE_STORAGE_KEY=mykey"
	@echo "  export AZURE_STORAGE_CONTAINER=tpcc"
	@echo "  ./tpcc-azure --azure --warehouses 1 --duration 60"
