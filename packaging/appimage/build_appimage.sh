#!/usr/bin/env bash
# build_appimage.sh — build AOE3DEHarness-x86_64.AppImage
#
# Usage: run from the harness repo root, or pass REPO_ROOT= env var.
#   ./packaging/appimage/build_appimage.sh
#
# Prerequisites (Fedora 44 / Bazzite):
#   sudo dnf install squashfs-tools fuse-libs
#   (meson/ninja needed only if you haven't built yet)
#
# Downloads appimagetool to packaging/appimage/tools/ if not already present.
# Does NOT require FUSE — uses APPIMAGE_EXTRACT_AND_RUN=1.
#
# Output: AOE3DEHarness-x86_64.AppImage in the repo root (or $OUTPUT_DIR).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-f44}"
OUTPUT_DIR="${OUTPUT_DIR:-$REPO_ROOT}"
TOOLS_DIR="$SCRIPT_DIR/tools"
APPDIR="$SCRIPT_DIR/AppDir"

BINARY_NAME="AOE3DEHarness"
APPIMAGE_NAME="${BINARY_NAME}-x86_64.AppImage"

# ── helpers ───────────────────────────────────────────────────────────────────
log() { echo "[appimage] $*"; }
die() { echo "[appimage] ERROR: $*" >&2; exit 1; }

# ── locate / download appimagetool ───────────────────────────────────────────
APPIMAGETOOL="${TOOLS_DIR}/appimagetool-x86_64.AppImage"

if [[ ! -x "$APPIMAGETOOL" ]]; then
    log "Downloading appimagetool ..."
    mkdir -p "$TOOLS_DIR"
    TOOL_URL="https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
    curl -fsSL -o "$APPIMAGETOOL" "$TOOL_URL"
    chmod +x "$APPIMAGETOOL"
    log "appimagetool saved to $APPIMAGETOOL"
fi

# Run appimagetool without FUSE (works inside containers and overlayfs)
export APPIMAGE_EXTRACT_AND_RUN=1

# ── verify the harness binary exists ─────────────────────────────────────────
HARNESS_BIN="$BUILD_DIR/src/$BINARY_NAME"
if [[ ! -f "$HARNESS_BIN" ]]; then
    die "Binary not found: $HARNESS_BIN
Build it first:
  cd $REPO_ROOT
  meson setup build-f44/ -Dharness=enabled -Denable_gamescope_wsi_layer=false \\
      --buildtype=release -Db_lto=false
  ninja -C build-f44/"
fi

# ── (re)create AppDir skeleton ───────────────────────────────────────────────
log "Preparing AppDir at $APPDIR ..."
rm -rf "$APPDIR"
mkdir -p \
    "$APPDIR/usr/bin" \
    "$APPDIR/usr/lib" \
    "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
    "$APPDIR/usr/share/applications"

# ── copy the harness binary ──────────────────────────────────────────────────
log "Copying $BINARY_NAME ..."
cp "$HARNESS_BIN" "$APPDIR/usr/bin/$BINARY_NAME"
chmod +x "$APPDIR/usr/bin/$BINARY_NAME"

# ── bundle non-stable shared libraries ───────────────────────────────────────
# Bundled because they are absent from Ubuntu 22.04 LTS / older SteamOS, or
# have SONAME / ABI churn across Fedora releases.
# Stable libs (libX11, libwayland-client, libSDL2, libstdc++, libm, libc,
# libdrm, libxkbcommon, libpipewire, libinput, libudev, libcap, libXtst, …)
# are intentionally left as runtime deps.
BUNDLE_LIBS=(
    "libdisplay-info.so.3"   # not in Ubuntu 22.04/24.04 (added ~2023)
    "libeis.so.1"             # libei input emulation — not in Ubuntu 22.04
    "libdecor-0.so.0"         # sometimes missing on non-GNOME stacks
    "libavif.so.16"           # SONAME changes frequently; .16 = libavif-1.x
    "libluajit-5.1.so.2"      # luajit, not universal
    "liblua-5.4.so"           # lua 5.4 not in Ubuntu 22.04 default repos
    "libseat.so.1"            # seatd/libseat — not universal
    "libyuv.so.0"             # not in Ubuntu 22.04
    "librav1e.so.0"           # rav1e Rust AV1 encoder — not in Ubuntu 22.04
    "libSvtAv1Enc.so.3"       # SVT-AV1, SONAME changes with major versions
    "libdav1d.so.7"           # dav1d AV1 SONAME v7 newer than Ubuntu 22.04
    "libvmaf.so.3"            # not in Ubuntu 22.04
)

log "Bundling non-stable shared libraries ..."
for lib in "${BUNDLE_LIBS[@]}"; do
    # Disable pipefail around ldconfig | awk: awk exits early (first match) which
    # sends SIGPIPE to ldconfig while it's still writing.  With pipefail that
    # propagates as exit 141 even though the command succeeded.
    set +o pipefail
    so_path=$(ldconfig -p 2>/dev/null | awk -v lib="$lib" '$1 == lib {print $NF; exit}') || true
    set -o pipefail
    if [[ -z "$so_path" ]]; then
        set +o pipefail
        so_path=$(ldd "$HARNESS_BIN" 2>/dev/null | awk -v lib="$lib" '$1 == lib {print $3}') || true
        set -o pipefail
    fi
    if [[ -z "$so_path" || ! -f "$so_path" ]]; then
        log "  WARNING: $lib not found on host — skipping"
        continue
    fi
    log "  $lib  ->  $so_path"
    cp --preserve=timestamps "$so_path" "$APPDIR/usr/lib/"
    real_path="$(realpath "$so_path")"
    if [[ "$real_path" != "$so_path" && -f "$real_path" ]]; then
        cp --preserve=timestamps "$real_path" "$APPDIR/usr/lib/"
    fi
done

# ── AppRun wrapper ────────────────────────────────────────────────────────────
log "Writing AppRun ..."
cat > "$APPDIR/AppRun" << 'APPRUN_EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH:-}"
exec "${HERE}/usr/bin/AOE3DEHarness" "$@"
APPRUN_EOF
chmod +x "$APPDIR/AppRun"

# ── .desktop file ─────────────────────────────────────────────────────────────
log "Writing .desktop file ..."
cat > "$APPDIR/AOE3DEHarness.desktop" << 'DESKTOP_EOF'
[Desktop Entry]
Type=Application
Name=AOE3DEHarness
Exec=AOE3DEHarness
Icon=AOE3DEHarness
Comment=AOE3 DE testing harness (gamescope fork with control socket)
Categories=Utility;
DESKTOP_EOF
cp "$APPDIR/AOE3DEHarness.desktop" "$APPDIR/usr/share/applications/"

# ── placeholder icon ──────────────────────────────────────────────────────────
log "Generating placeholder icon ..."
python3 "$SCRIPT_DIR/gen_icon.py" "$APPDIR"

# ── build the AppImage ────────────────────────────────────────────────────────
OUTPUT_APPIMAGE="$OUTPUT_DIR/$APPIMAGE_NAME"
log "Running appimagetool ..."
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT_APPIMAGE"

log ""
log "SUCCESS"
log "  Path : $OUTPUT_APPIMAGE"
log "  Size : $(du -sh "$OUTPUT_APPIMAGE" | cut -f1)"
log ""
log "Quick smoke test:"
log "  $OUTPUT_APPIMAGE --help | head -3"
