# AOE3DEHarness AppImage Packaging

## Files

| File | Purpose |
|------|---------|
| `build_appimage.sh` | Main build driver — runs on the host or in a container |
| `gen_icon.py` | Generates a minimal valid 256x256 placeholder PNG (no ImageMagick dep) |
| `tools/appimagetool-x86_64.AppImage` | Downloaded on first run; gitignored |
| `AppDir/` | Transient staging tree; deleted at the start of each build run |

## Usage

```bash
# From the repo root:
./packaging/appimage/build_appimage.sh

# Override build dir or output dir:
BUILD_DIR=build-custom/ OUTPUT_DIR=/tmp ./packaging/appimage/build_appimage.sh
```

The output `AOE3DEHarness-x86_64.AppImage` lands in `OUTPUT_DIR` (default: repo root).

## How it works

1. Copies `build-f44/src/AOE3DEHarness` into `AppDir/usr/bin/`.
2. Bundles a curated set of shared libraries that are absent from Ubuntu 22.04
   LTS or have unstable SONAMEs across distros (see `BUNDLE_LIBS` array).
   Standard X11 / Wayland / systemd / libdrm / libSDL2 are **not** bundled —
   they are stable enough to be left as runtime deps.
3. Writes an `AppRun` wrapper that prepends `usr/lib` to `LD_LIBRARY_PATH`.
4. Calls `appimagetool` with `APPIMAGE_EXTRACT_AND_RUN=1` (no FUSE required).

## Bundled vs unbundled libraries

### Bundled (not in Ubuntu 22.04 / unstable SONAME)
- `libdisplay-info.so.3` — not in Ubuntu 22.04/24.04
- `libeis.so.1` — libei input emulation (post-2022)
- `libdecor-0.so.0` — missing on some non-GNOME stacks
- `libavif.so.16` — libavif-1.x era; frequent SONAME changes
- `libluajit-5.1.so.2` — not universal
- `liblua-5.4.so` — not in Ubuntu 22.04 default repos
- `libseat.so.1` — not universal
- `libyuv.so.0` — not in Ubuntu 22.04
- `librav1e.so.0` — Rust AV1 encoder; not in Ubuntu 22.04
- `libSvtAv1Enc.so.3` — SVT-AV1; SONAME changes with major versions
- `libdav1d.so.7` — dav1d v7 is newer than Ubuntu 22.04
- `libvmaf.so.3` — not in Ubuntu 22.04

### Left as runtime deps (stable across mainstream distros)
`libX11`, `libwayland-client`, `libwayland-server`, `libSDL2`, `libdrm`,
`libxkbcommon`, `libpipewire-0.3`, `libpixman-1`, `libinput`, `libudev`,
`libcap`, `libXtst`, `libXmu`, `libXext`, `libxcb`, `libsystemd`, `libgudev`,
`libgobject-2.0`, `libglib-2.0`, `libstdc++`, `libm`, `libc`, `libgcc_s`,
`libjpeg`, `libpcre2-8`, `libuuid`, `libffi`, `libmtdev`, `libevdev`,
`libwacom`, `libXcursor`, `libXi`, `libXrender`, `libXdamage`, `libXfixes`,
`libXcomposite`, `libXres`, `libXxf86vm`, `libXt`, `libSM`, `libICE`, `libXau`.

## AppImage size

Expected: ~18 MB (binary is 38 MB unstripped; squashfs compression ~27% of raw).

## Gotchas

### SIGPIPE / exit 141 with `set -o pipefail`
`ldconfig -p | awk '{exit}'` causes SIGPIPE to ldconfig when awk exits early.
The script disables `pipefail` around these calls with `set +o pipefail` /
`set -o pipefail` guards. Do not remove them.

### FUSE not available
AppImageKit normally requires FUSE. Setting `APPIMAGE_EXTRACT_AND_RUN=1` makes
both appimagetool (during build) and the final AppImage extract to a tmpdir
instead. This is set automatically by the build script and is safe for CI.

### The binary is not stripped
The `build-f44/` build has debug info (~38 MB). If size matters, rebuild with
`--buildtype=minsize` or strip the binary before packaging. The AppImage
squashfs compresses it to ~18 MB regardless.

### appimagetool `--no-appstream` warning
appimagetool warns about missing AppStream metadata. This is cosmetic. To
suppress it, add `usr/share/metainfo/AOE3DEHarness.appdata.xml`.

### GitHub Actions: `--privileged` container flag
The Fedora 44 container needs `--privileged` for squashfs/fuse operations
inside the container. `APPIMAGE_EXTRACT_AND_RUN=1` is set in the workflow to
avoid the actual FUSE mount.
