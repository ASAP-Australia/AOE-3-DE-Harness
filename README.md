# AOE3DEHarness

AOE3DEHarness is a fork of [gamescope](https://github.com/ValveSoftware/gamescope) that adds a
Unix-domain control socket for cursor-free automated testing of the
**Age of Empires III: Definitive Edition "A New World"** mod.

## Install via AppImage (recommended)

```bash
wget https://github.com/ASAP-Australia/AOE3-DE-Harness/releases/latest/download/AOE3DEHarness-x86_64.AppImage
chmod +x AOE3DEHarness-x86_64.AppImage
sudo mv AOE3DEHarness-x86_64.AppImage /usr/local/bin/AOE3DEHarness
```

Then in Steam, set the launch options for AoE3 DE to:

```
AOE3DEHarness -- %command%
```

The AppImage bundles all non-standard libraries (`libdisplay-info`, `libeis`,
`libavif`, `librav1e`, `libdav1d`, `libvmaf`, etc.) so it runs on any
x86_64 Linux with glibc 2.17+ (Ubuntu 20.04 LTS, Fedora 38+, SteamOS 3+,
Bazzite).

---

## About gamescope

In an embedded session use case, gamescope does the same thing as steamcompmgr,
but with less extra copies and latency:

 - It gets game frames through Wayland via Xwayland, so there is no copy
   within X itself before it gets the frame.
 - It can use DRM/KMS to directly flip game frames to the screen, even when
   stretching or when notifications are up, removing another copy.
 - When it does need to composite with the GPU, it does so with async Vulkan
   compute, meaning you see your frame quickly even if the game already has
   the GPU busy with the next frame.

It also runs on top of a regular desktop (nested mode).

 - Because the game runs in its own Xwayland sandbox, it cannot interfere with
   your desktop and your desktop cannot interfere with it.
 - You can spoof a virtual screen with a desired resolution and refresh rate as
   the only thing the game sees.

Requires Mesa + AMD or Intel (Mesa 20.3+ / 21.2+ respectively). NVIDIA
proprietary driver 515.43.04+ is supported (`nvidia-drm.modeset=1` required).

## Keyboard shortcuts

* **Super + F** : Toggle fullscreen
* **Super + N** : Toggle nearest-neighbour filtering
* **Super + U** : Toggle FSR upscaling
* **Super + Y** : Toggle NIS upscaling
* **Super + I** : Increase FSR sharpness by 1
* **Super + O** : Decrease FSR sharpness by 1
* **Super + S** : Take screenshot (writes to `/tmp/gamescope_$DATE.png`)
* **Super + G** : Toggle keyboard grab

## Options

See `AOE3DEHarness --help` for the full list.

* `-W`, `-H`: output resolution (window size on desktop; display size in embedded mode)
* `-w`, `-h`: game resolution. Defaults to `-W`/`-H` values.
* `-r`: frame-rate limit (fps). Default: unlimited.
* `-o`: frame-rate limit when unfocused (fps). Default: unlimited.
* `-F fsr`: AMD FidelityFX Super Resolution 1.0 upscaling
* `-F nis`: NVIDIA Image Scaling v1.0.3 upscaling
* `-S integer`: integer scaling
* `-S stretch`: stretch scaling (fills the window)
* `-f`: fullscreen window
* `-b`: borderless window

## Reshade support

AOE3DEHarness supports a subset of Reshade effects via `--reshade-effect [path]`
and `--reshade-technique-idx [idx]`. This enables shader effects (CRT shaders,
film grain, HDR histograms, etc.) on top of whatever is displayed, without
hooking into the game process.

Using Reshade increases latency. For simple transformations prefer the LUT/CTM
path (`--look`), which runs in the DC on AMDGPU at scanout time.

---

## Building from source

For contributions or custom builds — see [packaging/appimage/README.md](packaging/appimage/README.md) for AppImage packaging details.

```bash
git submodule update --init
meson setup build-f44/ \
    -Dharness=enabled \
    -Denable_gamescope_wsi_layer=false \
    --buildtype=release \
    -Db_lto=false
ninja -C build-f44/
# Binary: build-f44/src/AOE3DEHarness
```

Required packages (Fedora 44 / Bazzite):

```
dnf install meson ninja-build gcc-c++ glslang libcap-devel SDL2-devel \
    vulkan-headers vulkan-loader-devel libX11-devel libXmu-devel \
    libXcomposite-devel libXrender-devel libXres-devel libXtst-devel \
    libxkbcommon-devel libdrm-devel libinput-devel \
    wayland-devel wayland-protocols-devel xorg-x11-server-Xwayland \
    pipewire-devel cmake libavif-devel libdecor-devel libeis-devel \
    luajit-devel lua-devel libseat-devel libyuv-devel \
    libdisplay-info-devel aom-devel rav1e-devel dav1d-devel libvmaf-devel
```
