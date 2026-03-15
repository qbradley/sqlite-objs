#!/bin/bash
#
# build-testfixture.sh — Build testfixture-objs with sqlite-objs VFS
#
# Produces build/testfixture-objs: a standard SQLite testfixture binary
# with our VFS registered as the default, backed by mock in-memory Azure ops.
#
# Prerequisites:
#   - test/sqlite-src/ must contain the full SQLite source (with test/)
#   - TCL 8.5+ development headers (macOS: system Tcl.framework)
#   - OpenSSL (for azure_auth.c)
#
# Usage: ./test/tcl/build-testfixture.sh
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SQLITE_SRC="$REPO_ROOT/test/sqlite-src"
BLD_DIR="$SQLITE_SRC/bld"
OUTPUT_DIR="$REPO_ROOT/build"

mkdir -p "$OUTPUT_DIR"

# ── Step 1: Ensure SQLite source is configured and stock testfixture built ──
if [ ! -d "$SQLITE_SRC/test" ]; then
    echo "ERROR: SQLite full source not found at $SQLITE_SRC"
    echo "Clone it: git clone --depth=1 --branch version-3.52.0 https://github.com/sqlite/sqlite.git $SQLITE_SRC"
    exit 1
fi

# Find tclConfig.sh
TCL_CONFIG=""
for d in \
    /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks/Tcl.framework/Versions/8.5 \
    /usr/lib/tcl8.6 \
    /usr/lib/tcl8.5; do
    if [ -f "$d/tclConfig.sh" ]; then
        TCL_CONFIG="$d"
        break
    fi
done
if [ -z "$TCL_CONFIG" ]; then
    echo "ERROR: Cannot find tclConfig.sh. Install TCL development headers."
    exit 1
fi

if [ ! -f "$BLD_DIR/Makefile" ]; then
    echo "==> Configuring SQLite build..."
    mkdir -p "$BLD_DIR"
    (cd "$BLD_DIR" && ../configure --with-tclsh=/usr/bin/tclsh --with-tcl="$TCL_CONFIG" 2>&1 | tail -5)
fi

if [ ! -f "$BLD_DIR/testfixture" ]; then
    echo "==> Building stock testfixture..."
    (cd "$BLD_DIR" && make testfixture -j4 2>&1 | tail -3)
fi

# ── Step 2: Compile our VFS objects ─────────────────────────────────
echo "==> Compiling sqlite-objs VFS components..."

CC="${CC:-cc}"
VFS_CFLAGS="-O2 -g -D_DARWIN_C_SOURCE -DSQLITE_THREADSAFE=1"
VFS_CFLAGS="$VFS_CFLAGS -I$REPO_ROOT/src -I$REPO_ROOT/sqlite-autoconf-3520000 -I$REPO_ROOT/test"
OPENSSL_CFLAGS="$(pkg-config --cflags openssl 2>/dev/null || echo '')"
OPENSSL_LIBS="$(pkg-config --libs openssl 2>/dev/null || echo '-lssl -lcrypto')"

$CC $VFS_CFLAGS -c -o "$OUTPUT_DIR/vfs_for_tcl.o" "$REPO_ROOT/src/sqlite_objs_vfs.c"
$CC $VFS_CFLAGS -c -o "$OUTPUT_DIR/stub_for_tcl.o" "$REPO_ROOT/src/azure_client_stub.c"
$CC $VFS_CFLAGS $OPENSSL_CFLAGS -c -o "$OUTPUT_DIR/auth_for_tcl.o" "$REPO_ROOT/src/azure_auth.c"
$CC $VFS_CFLAGS -c -o "$OUTPUT_DIR/error_for_tcl.o" "$REPO_ROOT/src/azure_error.c"
$CC $VFS_CFLAGS -c -o "$OUTPUT_DIR/mock_for_tcl.o" "$REPO_ROOT/test/mock_azure_ops.c"
$CC $VFS_CFLAGS -c -o "$OUTPUT_DIR/testfixture_vfs_init.o" "$REPO_ROOT/test/tcl/testfixture_vfs_init.c"

VFS_OBJS="$OUTPUT_DIR/vfs_for_tcl.o $OUTPUT_DIR/stub_for_tcl.o $OUTPUT_DIR/auth_for_tcl.o $OUTPUT_DIR/error_for_tcl.o $OUTPUT_DIR/mock_for_tcl.o $OUTPUT_DIR/testfixture_vfs_init.o"

# ── Step 3: Build testfixture-objs ──────────────────────────────────
# Strategy: delete the stock testfixture so `make -n testfixture`
# prints the full build command, then modify it to add our VFS objects.

echo "==> Building testfixture-objs..."
cd "$BLD_DIR"

# Get the build command from make dry-run (join continuation lines first)
rm -f testfixture
BUILD_CMD=$(make -n testfixture 2>/dev/null | sed -e ':a' -e '/\\$/N; s/\\\n//; ta' | tr -s '[:space:]' ' ' | grep '\-o testfixture' | head -1)
# Restore stock testfixture
make testfixture -j4 2>&1 | tail -1

if [ -z "$BUILD_CMD" ]; then
    echo "ERROR: Could not extract testfixture build command from Makefile"
    exit 1
fi

# Add our include paths, VFS objects, and OpenSSL to the build command
MODIFIED_CMD="$BUILD_CMD"
# Change output binary name
MODIFIED_CMD=$(echo "$MODIFIED_CMD" | sed "s|-o testfixture |-o $OUTPUT_DIR/testfixture-objs |")
# Add our include paths
MODIFIED_CMD=$(echo "$MODIFIED_CMD" | sed "s| -DSQLITE_NO_SYNC| -I$REPO_ROOT/src -I$REPO_ROOT/sqlite-autoconf-3520000 -I$REPO_ROOT/test -DSQLITE_NO_SYNC|")
# Append our VFS objects and OpenSSL before TCL libs
MODIFIED_CMD="$MODIFIED_CMD $VFS_OBJS $OPENSSL_LIBS"

eval "$MODIFIED_CMD" 2>&1 | grep -v '^$' | tail -5

echo "==> testfixture-objs ready: $OUTPUT_DIR/testfixture-objs"

# ── Step 4: Smoke test ──────────────────────────────────────────────
echo ""
echo "==> Smoke test:"
result=$(echo 'sqlite3 db test_smoke.db; db eval {CREATE TABLE t(x); INSERT INTO t VALUES(42)}; set r [db eval {SELECT * FROM t}]; db close; file delete test_smoke.db; puts "result=$r"; exit' | "$OUTPUT_DIR/testfixture-objs" 2>&1 | grep -o 'result=[0-9]*')
echo "  $result"

if [ "$result" = "result=42" ]; then
    echo "  ✓ testfixture-objs working correctly"
else
    echo "  ✗ Smoke test FAILED"
    exit 1
fi

echo ""
echo "==> Build complete! Run tests with: make test-tcl"
