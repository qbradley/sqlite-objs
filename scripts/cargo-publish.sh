#!/usr/bin/env bash
# Publish sqlite-objs Rust crates to crates.io.
#
# cargo publish requires all source files inside the crate directory.
# During normal development, build.rs reads from ../../src/ (the canonical
# location). This script copies the C sources into csrc/ for packaging,
# publishes, then cleans up.
#
# Usage:
#   ./scripts/cargo-publish.sh [--dry-run]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYS_CRATE="$REPO_ROOT/rust/sqlite-objs-sys"
SRC_DIR="$REPO_ROOT/src"
CSRC_DIR="$SYS_CRATE/csrc"
DRY_RUN=""

if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN="--dry-run"
    echo "🧪 Dry run mode"
fi

# Step 1: Copy C sources for packaging
echo "📦 Copying C sources to $CSRC_DIR..."
mkdir -p "$CSRC_DIR"
cp "$SRC_DIR"/sqlite_objs_vfs.c "$CSRC_DIR/"
cp "$SRC_DIR"/azure_client.c    "$CSRC_DIR/"
cp "$SRC_DIR"/azure_auth.c      "$CSRC_DIR/"
cp "$SRC_DIR"/azure_error.c     "$CSRC_DIR/"
cp "$SRC_DIR"/sqlite_objs.h     "$CSRC_DIR/"
cp "$SRC_DIR"/azure_client.h    "$CSRC_DIR/"
cp "$SRC_DIR"/azure_client_impl.h "$CSRC_DIR/"

cleanup() {
    echo "🧹 Cleaning up bundled C sources..."
    rm -rf "$CSRC_DIR"
}
trap cleanup EXIT

# Step 2: Publish sys crate first (the wrapper crate depends on it)
# --allow-dirty is needed because the temporary csrc/ copy isn't committed
echo "🚀 Publishing sqlite-objs-sys..."
(cd "$SYS_CRATE" && cargo publish $DRY_RUN --allow-dirty)

# Step 3: Publish the wrapper crate
echo "🚀 Publishing sqlite-objs..."
(cd "$REPO_ROOT/rust/sqlite-objs" && cargo publish $DRY_RUN)

echo "✅ Done."
