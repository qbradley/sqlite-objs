# Makefile for azqlite — SQLite VFS backed by Azure Blob Storage
#
# Targets:
#   all              Build libazqlite.a and azqlite-shell (stub, no external deps)
#   all-production   Build with real Azure client (requires libcurl + OpenSSL)
#   test-unit        Build and run Layer 1 unit tests
#   test-integration Build and run Layer 2 integration tests (requires Azurite)
#   test             Run test-unit + test-integration
#   clean            Remove build artifacts
#   amalgamation     Produce single-file distribution (future)

# ---------- Directories ----------

SQLITE_DIR  = sqlite-autoconf-3520000
SRC_DIR     = src
TEST_DIR    = test
BUILD_DIR   = build

# ---------- Toolchain ----------

CC       ?= cc
AR       ?= ar
CFLAGS    = -Wall -Wextra -Wpedantic -std=c11 -O2
CFLAGS   += -DSQLITE_THREADSAFE=1
CFLAGS   += -DSQLITE_ENABLE_FTS5
CFLAGS   += -DSQLITE_ENABLE_JSON1

# Include paths: our src/ dir + SQLite source dir
CFLAGS   += -I$(SRC_DIR) -I$(SQLITE_DIR)

# Platform-specific linker flags
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS  = -lpthread -lm
else
    LDFLAGS  = -lpthread -ldl -lm
endif

# pkg-config support for production dependencies (OpenSSL + libcurl)
# Falls back to hardcoded values if pkg-config is unavailable
PKG_CONFIG ?= pkg-config
OPENSSL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null || echo "")
OPENSSL_LDFLAGS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo "-lssl -lcrypto")
CURL_CFLAGS     := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null || echo "")
CURL_LDFLAGS    := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || echo "-lcurl")

# Production CFLAGS and LDFLAGS
CFLAGS_PROD  = $(CFLAGS) $(OPENSSL_CFLAGS) $(CURL_CFLAGS)
LDFLAGS_PROD = $(LDFLAGS) $(CURL_LDFLAGS) $(OPENSSL_LDFLAGS)

# Test-specific CFLAGS (suppress GNU extension warning in test harness)
TEST_CFLAGS = $(CFLAGS) -I$(TEST_DIR)

# ---------- Sources ----------

SQLITE_SRC  = $(SQLITE_DIR)/sqlite3.c
SQLITE_OBJ  = $(BUILD_DIR)/sqlite3.o

VFS_SRCS    = $(SRC_DIR)/azqlite_vfs.c \
              $(SRC_DIR)/azure_client_stub.c
VFS_OBJS    = $(BUILD_DIR)/azqlite_vfs.o \
              $(BUILD_DIR)/azure_client_stub.o

PROD_SRCS   = $(SRC_DIR)/azqlite_vfs.c \
              $(SRC_DIR)/azure_client.c \
              $(SRC_DIR)/azure_auth.c \
              $(SRC_DIR)/azure_error.c
PROD_OBJS   = $(BUILD_DIR)/azqlite_vfs.o \
              $(BUILD_DIR)/azure_client.o \
              $(BUILD_DIR)/azure_auth.o \
              $(BUILD_DIR)/azure_error.o

LIB_OBJS    = $(SQLITE_OBJ) $(VFS_OBJS)
LIBRARY      = $(BUILD_DIR)/libazqlite.a

PROD_LIB_OBJS = $(SQLITE_OBJ) $(PROD_OBJS)
PROD_LIBRARY   = $(BUILD_DIR)/libazqlite.a

SHELL_SRC   = $(SRC_DIR)/azqlite_shell.c
SHELL_BIN   = azqlite-shell

# Test objects — tests link against mock + stub (VFS refs production client API)
MOCK_OBJ    = $(BUILD_DIR)/mock_azure_ops.o
TEST_OBJS   = $(SQLITE_OBJ) $(BUILD_DIR)/azqlite_vfs.o $(BUILD_DIR)/azure_client_stub.o $(MOCK_OBJ)

# ---------- Default target ----------

.PHONY: all all-production clean test test-unit test-integration amalgamation

all: $(LIBRARY) $(SHELL_BIN)

# ---------- Build directory ----------

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ---------- SQLite amalgamation ----------

$(SQLITE_OBJ): $(SQLITE_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -w -c -o $@ $<

# ---------- VFS sources ----------

$(BUILD_DIR)/azqlite_vfs.o: $(SRC_DIR)/azqlite_vfs.c $(SRC_DIR)/azqlite.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/azure_client_stub.o: $(SRC_DIR)/azure_client_stub.c $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------- Production client sources ----------

$(BUILD_DIR)/azure_client.o: $(SRC_DIR)/azure_client.c $(SRC_DIR)/azure_client_impl.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_PROD) -c -o $@ $<

$(BUILD_DIR)/azure_auth.o: $(SRC_DIR)/azure_auth.c $(SRC_DIR)/azure_client_impl.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_PROD) -c -o $@ $<

$(BUILD_DIR)/azure_error.o: $(SRC_DIR)/azure_error.c $(SRC_DIR)/azure_client_impl.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_PROD) -c -o $@ $<

# ---------- Static library (stub) ----------

$(LIBRARY): $(LIB_OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $(LIB_OBJS)

# ---------- Production build ----------

all-production: $(PROD_LIB_OBJS) $(SHELL_SRC) | $(BUILD_DIR)
	$(AR) rcs $(PROD_LIBRARY) $(PROD_LIB_OBJS)
	$(CC) $(CFLAGS_PROD) -w -I$(SRC_DIR) -I$(SQLITE_DIR) \
		-DSQLITE_THREADSAFE=1 \
		-o $(SHELL_BIN) $(SRC_DIR)/azqlite_shell.c \
		$(BUILD_DIR)/azqlite_vfs.o \
		$(BUILD_DIR)/azure_client.o \
		$(BUILD_DIR)/azure_auth.o \
		$(BUILD_DIR)/azure_error.o \
		$(SQLITE_OBJ) \
		$(LDFLAGS_PROD)

# ---------- Shell binary ----------
# The shell is compiled as a single translation unit that #includes shell.c.
# We suppress warnings from the SQLite shell code with -w.

$(SHELL_BIN): $(SRC_DIR)/azqlite_shell.c $(LIBRARY)
	$(CC) $(CFLAGS) -w -I$(SRC_DIR) -I$(SQLITE_DIR) \
		-DSQLITE_THREADSAFE=1 \
		-o $@ $(SRC_DIR)/azqlite_shell.c \
		$(BUILD_DIR)/azqlite_vfs.o \
		$(BUILD_DIR)/azure_client_stub.o \
		$(SQLITE_OBJ) \
		$(LDFLAGS)

# ---------- Mock object (for tests) ----------

$(MOCK_OBJ): $(TEST_DIR)/mock_azure_ops.c $(TEST_DIR)/mock_azure_ops.h $(SRC_DIR)/azure_client.h | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

# ---------- Tests ----------

# Layer 1: Unit tests (C mocks, no network)
# test_main.c #includes test_vfs.c and test_azure_client.c directly,
# so we build only test_main and link against mock + VFS (not stub).
test-unit: $(BUILD_DIR)/test_main
	@echo "=== Running unit tests ==="
	$(BUILD_DIR)/test_main
	@echo "=== All unit tests passed ==="

$(BUILD_DIR)/test_main: $(TEST_DIR)/test_main.c $(TEST_DIR)/test_vfs.c $(TEST_DIR)/test_azure_client.c $(TEST_DIR)/mock_azure_ops.h $(TEST_DIR)/test_harness.h $(TEST_OBJS) | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -DENABLE_VFS_INTEGRATION -o $@ $(TEST_DIR)/test_main.c $(TEST_OBJS) $(LDFLAGS)

# Layer 2: Integration tests (requires Azurite)
# These tests link against the REAL azure_client.c (not stubs/mocks)
# and require Azurite to be running.
test-integration: $(BUILD_DIR)/test_integration
	@echo "=== Running integration tests (requires Azurite) ==="
	@./test/run-integration.sh

# Build the integration test binary
# Links against: SQLite + VFS + REAL Azure client (with libcurl + OpenSSL)
$(BUILD_DIR)/test_integration: $(TEST_DIR)/test_integration.c $(TEST_DIR)/test_harness.h $(PROD_LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_integration.c $(PROD_LIB_OBJS) $(LDFLAGS_PROD)

# Both layers
test: test-unit test-integration

# ---------- Clean ----------

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(SHELL_BIN)

# ---------- Amalgamation (future) ----------

amalgamation:
	@echo "Amalgamation target is planned for post-MVP 1."
	@echo "It will produce azqlite.c + azqlite.h as single-file distribution."
