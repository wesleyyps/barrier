#!/bin/bash
# coverity-scan.sh — build and upload Barrier to Coverity Scan
# Credentials are loaded from .env in the project root (never committed).
# Usage: ./coverity-scan.sh [--skip-build]

set -euo pipefail

# Ensure the local Coverity bin is on PATH
export PATH="/home/wesley/coverity/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"
BUILD_DIR="$SCRIPT_DIR/build"
COV_INT_DIR="$SCRIPT_DIR/cov-int"
TARBALL="$SCRIPT_DIR/barrier-coverity.tgz"

# ── Helpers ──────────────────────────────────────────────────────────────────
log()  { echo -e "\033[1;34m[coverity]\033[0m $*"; }
ok()   { echo -e "\033[1;32m[coverity]\033[0m $*"; }
err()  { echo -e "\033[1;31m[coverity]\033[0m $*" >&2; exit 1; }

# ── Load credentials ─────────────────────────────────────────────────────────
[[ -f "$ENV_FILE" ]] || err ".env not found at $ENV_FILE"
# shellcheck disable=SC1090
source "$ENV_FILE"

[[ -n "${COVERITY_TOKEN:-}"   ]] || err "COVERITY_TOKEN is not set in .env"
[[ -n "${COVERITY_EMAIL:-}"   ]] || err "COVERITY_EMAIL is not set in .env"
[[ -n "${COVERITY_PROJECT:-}" ]] || err "COVERITY_PROJECT is not set in .env"

# ── Check cov-build is available ─────────────────────────────────────────────
if ! command -v cov-build &>/dev/null; then
    err "cov-build not found. Download the Coverity Build Tool from your dashboard at
  https://scan.coverity.com/download?tab=cxx
and add its bin/ directory to your PATH, e.g.:
  echo 'export PATH=\"\$HOME/coverity/cov-analysis-linux64-*/bin:\$PATH\"' >> ~/.bashrc"
fi

# ── Parse arguments ───────────────────────────────────────────────────────────
SKIP_BUILD=false
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        *) err "Unknown argument: $arg" ;;
    esac
done

# ── Build phase ───────────────────────────────────────────────────────────────
if [[ "$SKIP_BUILD" == false ]]; then
    log "Cleaning previous build and interception data..."
    rm -rf "$BUILD_DIR" "$COV_INT_DIR" "$TARBALL"
    mkdir -p "$BUILD_DIR"

    log "Configuring with CMake..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBARRIER_BUILD_TESTS=OFF

    log "Intercepting build with cov-build (this may take a while)..."
    cov-build --dir "$COV_INT_DIR" cmake --build "$BUILD_DIR" -j"$(nproc)"

    # Sanity check: make sure cov-build captured something
    EMIT_COUNT=$(find "$COV_INT_DIR/emit" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    if [[ "$EMIT_COUNT" -eq 0 ]]; then
        err "cov-build produced no emission units. The build may have been fully cached.
Run without --skip-build to force a clean rebuild."
    fi
    ok "Build intercepted — $EMIT_COUNT compiler(s) with emission data captured."
else
    log "--skip-build specified, skipping build phase."
    [[ -d "$COV_INT_DIR" ]] || err "No cov-int/ directory found. Run without --skip-build first."
fi

# ── Package ───────────────────────────────────────────────────────────────────
log "Packaging $COV_INT_DIR → $TARBALL ..."
tar czvf "$TARBALL" -C "$SCRIPT_DIR" cov-int
TARBALL_SIZE=$(du -sh "$TARBALL" | cut -f1)
ok "Tarball created: $TARBALL ($TARBALL_SIZE)"

# ── Upload ────────────────────────────────────────────────────────────────────
VERSION="$(git -C "$SCRIPT_DIR" describe --tags --always 2>/dev/null || echo "unknown")"
DESCRIPTION="$(git -C "$SCRIPT_DIR" log -1 --pretty=%s 2>/dev/null || echo "manual upload")"
BRANCH="$(git -C "$SCRIPT_DIR" branch --show-current 2>/dev/null || echo "unknown")"

log "Uploading to Coverity Scan..."
log "  Project : $COVERITY_PROJECT"
log "  Version : $VERSION ($BRANCH)"
log "  Desc    : $DESCRIPTION"

HTTP_STATUS=$(curl --silent --output /tmp/coverity-upload.log --write-out "%{http_code}" \
    --form token="$COVERITY_TOKEN" \
    --form email="$COVERITY_EMAIL" \
    --form file=@"$TARBALL" \
    --form version="$VERSION" \
    --form description="$DESCRIPTION" \
    "https://scan.coverity.com/builds?project=$COVERITY_PROJECT")

if [[ "$HTTP_STATUS" == "200" || "$HTTP_STATUS" == "201" ]]; then
    ok "Upload successful (HTTP $HTTP_STATUS)."
    ok "Results will be available at: https://scan.coverity.com/projects/$COVERITY_PROJECT"
else
    log "Server response:"
    cat /tmp/coverity-upload.log
    err "Upload failed with HTTP $HTTP_STATUS. Check the response above."
fi
