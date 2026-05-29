# Gamescope Harness Control Socket — Architecture Design Audit

**Repository:** `/var/home/jflessenkemper/AOE-3-DE-Harness/`
**Audit date:** 2026-05-29
**HEAD:** `cfd783e` (FORK-NOTICE.md only; all source is stock upstream)
**Purpose:** Design a Unix-domain control socket (`--harness-mode`) that lets a Python client inject input and capture frames from gamescope without any in-Wine instrumentation.

---

## Section 1 — Audit of stock gamescope (zero proposed changes)

### 1.1 CLI argument parsing

**Entry point:** `src/main.cpp:706` — `int main(int argc, char **argv)`.

**Parser mechanism:** Standard POSIX `getopt_long`. The long-option table is declared as a file-scope constant at `src/main.cpp:59-162`:

```cpp
const struct option *gamescope_options = (struct option[]){
    { "help", no_argument, nullptr, 0 },
    { "nested-width", required_argument, nullptr, 'w' },
    ...
    { "allow-deferred-backend", no_argument, nullptr, 0 },
    { "keep-alive", no_argument, nullptr, 0 },
    {} // sentinel
};
```

The short-option string is generated dynamically by `build_optstring()` (`src/main.cpp:332-348`) from entries that have a non-zero `val` field. Long-only flags (val=0) are dispatched in the `case 0:` branch (`src/main.cpp:774-841`) by comparing `opt_name` against the long option's `name` string with `strcmp`.

**Option globals:** Each parsed flag is stored in a translation-unit-global variable (or a `gamescope::ConVar<T>`) declared near the top of `main.cpp`. Examples:

- `src/main.cpp:290`: `std::atomic<bool> g_bRun{true};`
- `src/main.cpp:301`: `bool g_bFullscreen = false;`
- `src/main.cpp:322`: `int g_nXWaylandCount = 1;`
- `src/main.hpp:10-19`: `extern` declarations consumed by other translation units.

**Clean insertion point for `--harness-mode`:** Add `{ "harness-mode", no_argument, nullptr, 0 }` and `{ "harness-socket", required_argument, nullptr, 0 }` to `gamescope_options` before the sentinel at `src/main.cpp:161`. Handle them in the `case 0:` dispatch at `src/main.cpp:841`. Store results in two globals in `main.cpp` / declared in `main.hpp` following the existing pattern (e.g. `bool g_bHarnessMode = false;` and `std::string g_sHarnessSocketPath;`).

**Conditional compilation gate:** There is no harness-specific `#if` yet. The pattern used by existing optional features (e.g. `#if HAVE_PIPEWIRE`, `#if HAVE_LIBEIS`, `#if HAVE_OPENVR`) is a C preprocessor define set by meson: `gamescope_cpp_args += '-DHAVE_LIBEIS=@0@'.format(eis_dep.found().to_int())` (`src/meson.build:153`). A `HAVE_HARNESS` define following this pattern would be clean.

---

### 1.2 Input pipeline (keyboard + pointer)

**Host input ingestion** comes from two paths depending on backend:

1. **Session-based (DRM/embedded):** wlroots creates a libinput backend (`src/wlserver.cpp:1954`) attached to `wlserver.wlr.multi_backend`. wlroots' libinput integration delivers events to the wlroots seat directly. A separate `gamescope::CLibInputHandler` (`src/LibInputHandler.cpp`) is used in non-session contexts (e.g. OpenVR) where direct libinput ownership is needed without a session.

2. **Nested/Wayland backend:** Input comes from the parent Wayland compositor via the wlroots Wayland backend.

**Downstream delivery to game (XWayland/Wayland clients):** All paths ultimately call through these `wlserver.cpp` functions, all of which require the `waylock` mutex to be held:
- Keyboard: `wlserver_key(uint32_t key, bool press, uint32_t time)` at `src/wlserver.cpp:2352`
- Mouse motion (relative): `wlserver_mousemotion(double dx, double dy, uint32_t time)` at `src/wlserver.cpp:2680`
- Mouse warp (absolute): `wlserver_mousewarp(double x, double y, uint32_t time, bool bSynthetic)` at `src/wlserver.cpp:2709`
- Mouse button: `wlserver_mousebutton(int button, bool press, uint32_t time)` at `src/wlserver.cpp:2735`

Inside `wlserver_key`, the key code is passed directly to wlroots: `wlr_seat_keyboard_notify_key(wlserver.wlr.seat, time, key, press)` at `src/wlserver.cpp:2362`. The `key` parameter is an **evdev scancode** (not an XKB keysym), exactly as produced by `linux/input-event-codes.h`.

**Existing libei output:** Yes, gamescope implements a full `EISSOCK` input server. Initialization at `src/wlserver.cpp:2097-2115`:

```cpp
#if HAVE_LIBEIS
char szEISocket[64];
snprintf(szEISocket, sizeof(szEISocket), "%s-ei", wlserver.wl_display_name);
std::unique_ptr<gamescope::GamescopeInputServer> pInputServer = ...;
if (pInputServer->Init(szEISocket)) {
    g_InputServer = std::move(pInputServer);
    setenv("LIBEI_SOCKET", szEISocket, 1);
    g_LibEisWaiter.AddWaitable(g_InputServer.get());
}
```

The socket name is `gamescope-N-ei` (where N is the display slot, e.g. `gamescope-0-ei`). The `GamescopeInputServer` runs on a `CAsyncWaiter` thread named `gamescope-eis` (`src/wlserver.cpp:1934`). When the libei client sends events, `GamescopeInputServer::OnPollIn()` (`src/InputEmulation.cpp:64`) processes them and calls the same `wlserver_*` functions after taking `waylock`.

**Pointer absolute moves vs. relative:** `wlserver_mousewarp` (`src/wlserver.cpp:2709`) performs an absolute position set. It stores the coordinates into `wlserver.mouse_surface_cursorx` / `cursory`, clamps them to surface bounds, then calls `wlr_seat_pointer_notify_motion`. This is the correct function for `CLICK`/`MOVE` commands; it does not require any additional transform.

**Key code translation (critical detail):** `wlserver_key` takes a raw **evdev scancode** (`uint32_t key`). The XKB keymap (`src/wlserver.cpp:1978-1982`) is applied by wlroots when it notifies the XWayland client, translating scancode → keysym → X11 keycode. Our harness must map Win32 VK codes to evdev scancodes — not to XKB keysyms. The relevant include is `<linux/input-event-codes.h>` (`src/wlserver.cpp:14`). For example, `KEY_F1 = 59`, `KEY_A = 30`, `KEY_SPACE = 57`.

---

### 1.3 Composite + present path

**Per-frame compositor loop:** `paint_all(global_focus_t *pFocus, bool async)` at `src/steamcompmgr.cpp:2492`. This runs on the `gamescope-xwm` thread (launched at `src/main.cpp:1075,1093`). The frame loop polls for vblank and events at `src/steamcompmgr.cpp:8490-8510`.

**Final composite framebuffer production:** Frames are composited into a `CVulkanTexture` via:
- `vulkan_composite(FrameInfo_t *frameInfo, ...)` — composites layers into Vulkan image
- `vulkan_screenshot(const FrameInfo_t *frameInfo, Rc<CVulkanTexture> pScreenshotTexture, ...)` — a variant specifically for capturing

The composite image is a `CVulkanTexture` with CPU-mapped memory (`pScreenshotTexture->mappedData()` at `src/steamcompmgr.cpp:2971`).

**Safe pixel readback point:** After `connector->Present(&frameInfo, async)` at `src/steamcompmgr.cpp:2833` and the `ProcessPendingScreenshot()` call at line `2838`. The existing screenshot path is exactly this: it re-runs composite into a staging texture, waits (`vulkan_wait(*oScreenshotSeq, false)` at line `2936`), then reads `mappedData()`. This is already synchronous from the frame loop's perspective (the `screenshotThread` is detached but the GPU wait happens on the compositor thread before detach).

**Pixel format for PNG output:** For `.png` screenshots, the capture format is `DRM_FORMAT_XRGB8888` → `VK_FORMAT_B8G8R8A8_UNORM` (`src/steamcompmgr.cpp:2850,2975,3070`). The write path at `src/steamcompmgr.cpp:3073-3087` manually swaps BGR→RGB and writes via `stbi_write_png`. So the internal format is **BGRA8 (VK_FORMAT_B8G8R8A8_UNORM)**, and the conversion to RGB PNG is already implemented.

**Existing screenshot facility:** Yes. `gamescope::CScreenshotManager` (`src/steamcompmgr_shared.hpp:284-317`) provides:

```cpp
void TakeScreenshot(GamescopeScreenshotInfo info = GamescopeScreenshotInfo{});
std::optional<GamescopeScreenshotInfo> ProcessPendingScreenshot();
static CScreenshotManager &Get();
```

`GamescopeScreenshotInfo` (`src/steamcompmgr_shared.hpp:275-282`) holds path, type, flags, and two bool fields for notification routing. Callers set the info and trigger `hasRepaint = true`; the frame loop picks it up via `ProcessPendingScreenshot()` on the next paint. This is **the correct integration point** for our `SCREENSHOT` command — just call `CScreenshotManager::Get().TakeScreenshot(info)`.

The screenshot type `GAMESCOPE_CONTROL_SCREENSHOT_TYPE_FULL_COMPOSITION` (value 3, `protocol/gamescope-control.xml:86`) captures all layers with no color management applied, which is the right choice for AI pixel inspection.

**stb_image_write:** Vendored at `subprojects/stb/stb_image_write.h`. Used directly in `steamcompmgr.cpp:119` as `#include <stb_image_write.h>`. The dep is `stb_dep` (`meson.build:56`, `src/meson.build:200`).

---

### 1.4 Existing IPC surfaces

**Gamescope's own Wayland server:** Socket name pattern `gamescope-N` where N is an integer slot (0-127) chosen at startup (`src/wlserver.cpp:2064-2068`). Exported via `GAMESCOPE_WAYLAND_DISPLAY` env var (`src/main.cpp:1064`). The libei socket is `gamescope-N-ei`.

**gamescope-control protocol:** A private Wayland protocol (`protocol/gamescope-control.xml`) exposing screenshot, display info, refresh cycle, look (3D LUT), and perf-stats requests. Clients bind `gamescope_control` via the display's global registry. The protocol includes `take_screenshot` (since version 3) which is exactly what triggers `CScreenshotManager::Get().TakeScreenshot(...)` in wlserver (`src/wlserver.cpp:1082-1088`). This protocol is what `gamescopectl` uses.

**MangoApp / Steam integration:** `src/mangoapp.cpp` receives stats from gamescope via a named pipe / Unix socket (GAMESCOPE_STATS). Steam integration is handled through X11 properties on the root window and the `gamescope_control` Wayland protocol. There is a `--steam` / `-e` flag that puts gamescope in Steam mode with additional env var exports (`src/main.cpp:769-773`, `src/main.cpp:596-688`).

**PipeWire:** Optional (`#if HAVE_PIPEWIRE`) for streaming/screen capture via the `gamescope_pipewire` Wayland protocol. Socket is a standard PipeWire socket.

**Our control socket coexistence:** None of the existing sockets use a path like `$XDG_RUNTIME_DIR/gamescope-anw.sock`. The existing `gamescope-N` sockets are inside `$XDG_RUNTIME_DIR` but use the display-slot naming. Our socket path can be anything not matching `gamescope-*` or `gamescope-*-ei`. The `TempFiles.cpp` utility (`src/Utils/TempFiles.cpp:45`) shows the XDG_RUNTIME_DIR pattern already in use.

---

### 1.5 Build system

**Top-level `meson.build`:** At `meson.build:101-103`:
```meson
if get_option('enable_gamescope')
  subdir('src')
endif
```

A `subdir('harness')` call would fit cleanly between the protocol subdir (`meson.build:95`) and the `src` subdir, or after the `src` subdir if it depends on symbols from `src`. Since `harness/` will be compiled into the `gamescope` executable (not a separate binary), it will be a sources-and-include contribution to the `src/meson.build` build rather than an independent `subdir`. The cleanest approach is to add harness sources directly to the `src` array in `src/meson.build` or to `subdir('harness')` from `src/meson.build`.

**`libei` dependency:** `src/meson.build:14`:
```meson
eis_dep = dependency('libeis-1.0', required: get_option('input_emulation'))
```
Option `input_emulation` is in `meson_options.txt:6`. The harness does NOT need libei; it calls `wlserver_*` directly. No additional dependency for input injection.

**Conditional compilation pattern in `src/meson.build`:**
```meson
gamescope_cpp_args += '-DHAVE_LIBEIS=@0@'.format(eis_dep.found().to_int())
gamescope_cpp_args += '-DHAVE_SDL2=@0@'.format(sdl2_dep.found().to_int())
```
Following this: add `option('harness', type: 'feature', description: 'Build harness control socket')` to `meson_options.txt`, then `gamescope_cpp_args += '-DHAVE_HARNESS=@0@'.format(get_option('harness').enabled().to_int())`.

**New deps:** Possibly `libpng`. However, since `stb_image_write.h` is already vendored and used for PNG output in `steamcompmgr.cpp`, there is no need to add libpng — the `stb_dep` already covers PNG writes.

---

### 1.6 Threading model

**Three main threads:**

| Thread name | How spawned | File |
|---|---|---|
| `gamescope-wl` | `wlserver_run()` called from main(), main thread blocks here | `src/wlserver.cpp:2174` |
| `gamescope-xwm` | `std::thread steamCompMgrThread(steamCompMgrThreadRun, ...)` at `main.cpp:1075` | `src/main.cpp:1093-1099` |
| `gamescope-eis` | `CAsyncWaiter` epoll loop, launched by `g_LibEisWaiter` | `src/wlserver.cpp:1934` |

**`gamescope-wl` thread** owns the Wayland event loop (`wl_event_loop_dispatch`) and holds `waylock` while processing Wayland protocol events (`src/wlserver.cpp:2212-2223`).

**`gamescope-xwm` thread** runs `steamcompmgr_main` — all X11, compositing, and screenshot work. It calls `wlserver_lock()` / `wlserver_unlock()` extensively around every `wlserver_*` call.

**`gamescope-eis` thread** runs the epoll loop for libei. It calls `wlserver_lock()` / `wlserver_unlock()` around each input delivery (see `src/InputEmulation.cpp:173-185`).

**Mutex pattern:** A single `pthread_mutex_t waylock = PTHREAD_MUTEX_INITIALIZER` at `src/wlserver.cpp:2143`. All threads must hold this before calling any `wlserver_*` function. The debug assertion `wlserver_is_lock_held()` at `src/wlserver.cpp:2145-2153` confirms this.

**Which thread should host our control socket listener?** A **fourth dedicated thread** — matching the `gamescope-eis` pattern. The socket listener blocks on `accept()` / `read()`, so it must not be on the wl or xwm threads. To dispatch commands, it takes `waylock` for input delivery and posts to `CScreenshotManager` (which uses its own internal mutex) for screenshots.

---

## Section 2 — Proposed modifications

### 2.1 CLI: add `--harness-mode` and `--harness-socket PATH`

**File:** `src/main.cpp`

**New globals** (add after `src/main.cpp:161` option table definition, around line 290 with other globals):
```cpp
bool g_bHarnessMode = false;
std::string g_sHarnessSocketPath;
```

**New extern declarations** in `src/main.hpp` after line 19:
```cpp
extern bool g_bHarnessMode;
extern std::string g_sHarnessSocketPath;
```

**Option table insertion** — add before the `{}` sentinel at `src/main.cpp:161`:
```cpp
{ "harness-mode",   no_argument,       nullptr, 0 },
{ "harness-socket", required_argument, nullptr, 0 },
```

**Dispatch in `case 0:` block** — add after `src/main.cpp:841` (before the `break;`):
```cpp
} else if (strcmp(opt_name, "harness-mode") == 0) {
    g_bHarnessMode = true;
} else if (strcmp(opt_name, "harness-socket") == 0) {
    g_sHarnessSocketPath = optarg;
}
```

**Default socket path** — set after option parsing is complete, before `wlserver_init()` at `src/main.cpp:1036`:
```cpp
if (g_bHarnessMode && g_sHarnessSocketPath.empty()) {
    const char *pXDG = getenv("XDG_RUNTIME_DIR");
    g_sHarnessSocketPath = pXDG ? std::string(pXDG) + "/gamescope-anw.sock"
                                : "/tmp/gamescope-anw.sock";
}
```

---

### 2.2 New file: `src/harness/harness_socket.cpp` + `harness_socket.h`

**Socket model:** Unix-domain `SOCK_STREAM` listener. Line-protocol: one UTF-8 command per line terminated by `\n`; one UTF-8 response line back. The thread created here is the **harness control thread**.

**`harness_socket.h`:**
```cpp
#pragma once
#include <string>

namespace gamescope::Harness {

void StartHarnessSocket(const std::string &socketPath);
void StopHarnessSocket();

} // namespace gamescope::Harness
```

**`harness_socket.cpp` lifecycle:**
- `StartHarnessSocket` creates a `SOCK_STREAM | SOCK_CLOEXEC` socket, binds to `socketPath` (unlinking any stale file first), calls `listen(fd, 4)`, and spawns a thread (`pthread_setname_np(self, "gamescope-harness")`).
- The thread runs `accept()` in a loop. For each connection, processes lines one at a time. On `g_bRun == false`, breaks out and closes the listen socket.
- On each accepted connection, reads lines and dispatches to:
  - `harness_handle_state(conn_fd)` → writes JSON status
  - `harness_handle_screenshot(path, conn_fd)` → calls capture, blocks, then responds
  - `harness_handle_key_down(vk_hex, conn_fd)`, `harness_handle_key_up(vk_hex, conn_fd)`, `harness_handle_key(vk_hex, conn_fd)` → input injection
  - `harness_handle_click(x, y, conn_fd)`, `harness_handle_move(x, y, conn_fd)` → pointer injection
  - `harness_handle_quit(conn_fd)` → writes `OK\n`, closes connection

**Response protocol:**
- Success: `OK\n` or `OK <data>\n`
- Error: `ERR <message>\n`

**Wiring to main.cpp:** Add after `wlserver_init()` at `src/main.cpp:1036` and before `std::thread steamCompMgrThread(...)` at line 1075:
```cpp
#if HAVE_HARNESS
if (g_bHarnessMode) {
    gamescope::Harness::StartHarnessSocket(g_sHarnessSocketPath);
}
#endif
```

Add before `steamCompMgrThread.join()` at `src/main.cpp:1087`:
```cpp
#if HAVE_HARNESS
if (g_bHarnessMode) {
    gamescope::Harness::StopHarnessSocket();
}
#endif
```

---

### 2.3 New file: `src/harness/harness_input.cpp` + `harness_input.h`

**Win32 VK → evdev mapping table:**

The mapping must be a lookup table from Win32 virtual-key codes (e.g., `0x70` = VK_F1) to `<linux/input-event-codes.h>` KEY_* scancodes. `wlserver_key()` accepts these scancodes directly.

Key entries (non-exhaustive sample):
| VK hex | Win32 name | evdev code | linux constant |
|---|---|---|---|
| 0x08 | VK_BACK | 14 | KEY_BACKSPACE |
| 0x09 | VK_TAB | 15 | KEY_TAB |
| 0x0D | VK_RETURN | 28 | KEY_ENTER |
| 0x1B | VK_ESCAPE | 1 | KEY_ESC |
| 0x20 | VK_SPACE | 57 | KEY_SPACE |
| 0x25 | VK_LEFT | 105 | KEY_LEFT |
| 0x26 | VK_UP | 103 | KEY_UP |
| 0x27 | VK_RIGHT | 106 | KEY_RIGHT |
| 0x28 | VK_DOWN | 108 | KEY_DOWN |
| 0x41-0x5A | VK_A..VK_Z | 30,48,46,...  | KEY_A..KEY_Z |
| 0x70-0x7B | VK_F1..VK_F12 | 59-68,87,88 | KEY_F1..KEY_F12 |

**`harness_input.h`:**
```cpp
#pragma once
#include <cstdint>

namespace gamescope::Harness {

// Returns false if vk_code has no known mapping.
bool HarnessKeyDown(uint32_t vk_code);
bool HarnessKeyUp(uint32_t vk_code);
bool HarnessKey(uint32_t vk_code);     // down + up

bool HarnessMove(double x, double y);
bool HarnessClick(double x, double y); // move + left down + left up

} // namespace gamescope::Harness
```

**Implementation:** Each function calls `wlserver_lock()`, calls the appropriate `wlserver_*()` function, then `wlserver_unlock()`. For `HarnessKey`, call key-down then key-up with `time = get_time_in_milliseconds()` (same pattern as `LibInputHandler.cpp` which uses a local `s_uSequence` counter). For `HarnessClick`, call `wlserver_mousewarp(x, y, time, true)` then `wlserver_mousebutton(BTN_LEFT, true, time)` then `wlserver_mousebutton(BTN_LEFT, false, time)`.

**BTN_LEFT:** Defined as `0x110` in `<linux/input-event-codes.h>`. Already used in gamescope at `src/Backends/OpenVRBackend.cpp:1370`.

**Focus requirement:** `wlserver_key` calls `wlr_seat_keyboard_notify_key` which delivers to the currently focused surface regardless of pointer position. No explicit focus-setting is needed for the game since it should already hold keyboard focus. If the game is not focused, the harness should call `wlserver_keyboardfocus(wlserver.kb_focus_surface, false)` — but this is an edge case.

---

### 2.4 New file: `src/harness/harness_capture.cpp` + `harness_capture.h`

**Strategy:** Reuse `gamescope::CScreenshotManager` exactly as the existing `gamescope-control` Wayland protocol and the `SIGUSR2` handler use it. No new Vulkan or frame-loop code.

**Synchronous blocking mechanism:** `CScreenshotManager::TakeScreenshot()` sets `hasRepaint = true` and stores the pending info. The compositor picks it up on its next paint cycle and writes the file. We need to block the harness command handler until the file is written.

The cleanest approach: add a completion notification to `GamescopeScreenshotInfo`. Since `GamescopeScreenshotInfo` is in `src/steamcompmgr_shared.hpp` and the screenshot thread in `steamcompmgr.cpp:2968` is already detached, we need to either (a) add a `std::promise`/`std::future` pair to the info struct, or (b) poll for file existence.

Option (a) is cleaner: add `std::shared_ptr<std::promise<bool>> pCompletionPromise` to `GamescopeScreenshotInfo`, set it in the screenshot thread at `src/steamcompmgr.cpp:3087` (after `stbi_write_png` returns), and `get()` the future in the harness thread. The `screenshotThread.detach()` at line 3146 means the promise can outlive the struct by being `shared_ptr`.

**`harness_capture.h`:**
```cpp
#pragma once
#include <string>

namespace gamescope::Harness {

// Synchronously captures the composite framebuffer (FULL_COMPOSITION type)
// and writes a PNG to path. Returns true on success.
bool HarnessScreenshot(const std::string &path);

} // namespace gamescope::Harness
```

**`harness_capture.cpp` implementation:**
```cpp
bool HarnessScreenshot(const std::string &path) {
    auto pPromise = std::make_shared<std::promise<bool>>();
    auto future = pPromise->get_future();

    gamescope::CScreenshotManager::Get().TakeScreenshot(
        gamescope::GamescopeScreenshotInfo{
            .szScreenshotPath  = path,
            .eScreenshotType   = GAMESCOPE_CONTROL_SCREENSHOT_TYPE_FULL_COMPOSITION,
            .uScreenshotFlags  = 0,
            .bX11PropertyRequested = false,
            .bWaylandRequested = false,
            .pCompletionPromise = pPromise,
        }
    );

    // Block until compositor thread writes the file (or fails).
    return future.get();
}
```

The `GamescopeScreenshotInfo` struct modification and the corresponding `pPromise->set_value(bScreenshotSuccess)` call in `steamcompmgr.cpp` are both small and localized changes.

**Pixel format:** Already confirmed BGRA8 → RGB PNG via stb_image_write. No format conversion needed in the harness layer.

---

### 2.5 `main.cpp` wiring summary

**Insert after `wlserver_init()` at `src/main.cpp:1036`, before line 1075:**
```cpp
#if HAVE_HARNESS
if (g_bHarnessMode) {
    gamescope::Harness::StartHarnessSocket(g_sHarnessSocketPath);
}
#endif
```

**Insert before `steamCompMgrThread.join()` at `src/main.cpp:1087`:**
```cpp
#if HAVE_HARNESS
gamescope::Harness::StopHarnessSocket();
#endif
```

**No changes needed to the frame loop in steamcompmgr.cpp** beyond adding `pCompletionPromise` to `GamescopeScreenshotInfo` and setting it in the screenshot thread. The harness socket thread dispatches input directly via `wlserver_lock()` + `wlserver_*()` without touching `paint_all`.

**Guard:** All harness code paths guarded with `#if HAVE_HARNESS` or runtime `if (g_bHarnessMode)` checks. When `--harness-mode` is absent, `StartHarnessSocket` is never called, no thread is created, and no socket file is written.

---

### 2.6 `meson.build` integration

**`meson_options.txt`** — add after line 11:
```meson
option('harness', type: 'feature', value: 'disabled', description: 'Build harness control socket for ANW AI agent')
```

**`src/meson.build`** — add after line 153 (`HAVE_LIBEIS` line):
```meson
gamescope_cpp_args += '-DHAVE_HARNESS=@0@'.format(get_option('harness').enabled().to_int())
```

**`src/meson.build`** — add harness sources to the `src` list (after `'InputEmulation.cpp',` at line 123):
```meson
if get_option('harness').enabled()
  src += 'harness/harness_socket.cpp'
  src += 'harness/harness_input.cpp'
  src += 'harness/harness_capture.cpp'
endif
```

The harness files live at `src/harness/` (not the repo-root `harness/` which contains this design doc). This keeps the new code adjacent to the gamescope source it integrates with, and avoids a separate `subdir('harness')` in the top-level meson (which would create linking complexity).

**No new `dependency()` calls required.** The harness uses:
- `stb_image_write.h` — already in `stb_dep`
- `wlserver_*` functions — already in gamescope core
- `CScreenshotManager` — already in gamescope core
- Standard POSIX socket APIs — no extra dep needed

To build with harness enabled:
```
meson setup build -Dharness=enabled
```

---

## Section 3 — Open questions and risks

**OQ-1: `GamescopeScreenshotInfo` struct modification and future/promise thread safety.**
Adding `std::shared_ptr<std::promise<bool>>` to the struct requires the harness thread to keep its `std::future` alive across the `TakeScreenshot` call. The screenshot runs in a detached thread (`steamcompmgr.cpp:3146`). The shared_ptr keeps the promise alive, but there is a subtle race: if gamescope shuts down (`g_bRun = false`) while a screenshot is pending, the compositor may never process `ProcessPendingScreenshot()`, leaving the future blocked forever. Mitigation: add a timeout on `future.wait_for()` (e.g. 5 seconds) and return an error if it expires.

*Verification:* Code review of the `screenshotThread.detach()` path in `steamcompmgr.cpp:2968-3146` to confirm the promise is fulfilled in all exit branches including the `!oScreenshotSeq` error path at line 2930.

**OQ-2: HDR / 10-bit frame formats.**
AoE3 DE on Steam Deck hardware could in principle output HDR content. The screenshot path in `steamcompmgr.cpp:2848-2851` chooses `DRM_FORMAT_XRGB2101010` for `.avif` and `DRM_FORMAT_XRGB8888` for `.png`. If we request `FULL_COMPOSITION` type with `.png`, we always get the 8-bit BGRA path at `steamcompmgr.cpp:3070`. This should be fine for AI state detection. Verify that AoE3 DE does not run in HDR mode in our Proton/Headless setup: check `g_bOutputHDREnabled` value at runtime. If HDR is active, the game's content will be tone-mapped before the 8-bit screenshot, which may affect pixel-level accuracy but is acceptable for AI inference.

**OQ-3: XKB layout sensitivity for keyboard injection.**
`wlserver_key()` delivers raw evdev scancodes; the XKB keymap (loaded from `XKB_DEFAULT_*` env vars at `src/wlserver.cpp:1973-1978`) determines how the game receives them as X11 key events. Our Win32 VK → evdev map assumes a US-ANSI keyboard layout. If gamescope is run on a system with a different XKB layout (e.g. AZERTY), the evdev code for VK_Q (`0x51`) would be `KEY_A = 30` in our table (because Q occupies the A position on ANSI), but the host system's XKB layout may emit a different keysym. Since the harness is a dedicated tool and we control the gamescope invocation, we can enforce `XKB_DEFAULT_LAYOUT=us` in the harness launch environment.

*Verification:* Check `XKB_DEFAULT_LAYOUT` in the gamescope distrobox environment; enforce it in the launch wrapper if not set.

**OQ-4: Pointer grab / cursor constraint interaction.**
`wlserver_mousewarp()` applies cursor constraint clamping via `wlserver_clampcursor()` (line 2716). If the game has set a pointer constraint (common in games during pointer-locked / mouse-look modes), the warp target will be clamped to the constraint region. For AoE3 DE, pointer lock is not typical during UI interaction, but if it occurs during game-play clicks, our `CLICK` command will appear to have no effect. The cursor constraint is stored in `wlserver.mouse_constraint` (atomic, `src/wlserver.hpp:139`). Mitigation: add a temporary constraint unlock in `HarnessClick` if a constraint is active, then re-apply.

*Verification:* Test `CLICK` command during active game window and observe cursor behavior with `wlserver.GetCursorConstraint() != nullptr`.

**OQ-5: Synchronous SCREENSHOT blocking the harness thread.**
The `future.get()` call in `HarnessScreenshot` blocks the harness connection handler thread indefinitely until the compositor paints a frame and the screenshot thread finishes writing. If the compositor is idle (no new frames coming in), `hasRepaint = true` must be enough to wake it up. Confirm that `hasRepaint = true` (`steamcompmgr_shared.hpp:271`) is sufficient to trigger a `paint_all` pass even when there are no surface commits. Looking at `src/steamcompmgr.cpp:8501` — `g_SteamCompMgrWaiter.PollEvents()` — the waiter is nudged by `nudge_steamcompmgr()` (`steamcompmgr.cpp:7141`). The screenshot manager sets `hasRepaint = true` but does not call `nudge_steamcompmgr()`. If the compositor is blocked in `PollEvents()` with no pending events, the screenshot may stall. Mitigation: call `nudge_steamcompmgr()` after setting the pending screenshot.

*Verification:* Call `nudge_steamcompmgr()` inside `TakeScreenshot()` (or do it from `HarnessScreenshot` after the `TakeScreenshot` call).

**OQ-6: Thread safety of `currentOutputWidth` / `currentOutputHeight` for STATE response.**
These globals (`src/steamcompmgr.cpp:893`) are written only by the `gamescope-xwm` thread and read with no synchronization. For the `STATE` response, we only need approximate values and a small data race here is acceptable. Alternatively, use `g_nOutputWidth` / `g_nOutputHeight` from `main.hpp` which are set before any threads start.

**OQ-7: Socket file cleanup on abnormal exit.**
If gamescope crashes, the Unix socket file at `$XDG_RUNTIME_DIR/gamescope-anw.sock` is not removed. Subsequent runs will find it and `bind()` will fail with `EADDRINUSE`. Mitigation: always `unlink(socketPath)` before `bind()`. The Python client should also handle ENOENT gracefully.

**OQ-8: Steam gamescope-control protocol conflict.**
If gamescope is started with `--steam` (Steam mode), Steam's overlay process will bind to `gamescope_control` on the Wayland socket. Our harness socket is independent (a separate Unix socket, not a Wayland protocol), so there is no conflict. The `gamescope_control` take_screenshot facility could theoretically be used directly by a Wayland client, but our Python harness is outside the Wayland session and using a direct socket is simpler.

---

## Section 4 — Implementation phases

```
Phase 1 — CLI flag + no-op socket listener
  Files:
    src/main.cpp (add --harness-mode, --harness-socket, globals)
    src/main.hpp (extern declarations)
    src/harness/harness_socket.cpp
    src/harness/harness_socket.h
    src/meson.build (add harness sources + HAVE_HARNESS define)
    meson_options.txt (add 'harness' feature option)
  Acceptance: gamescope starts with --harness-mode, creates socket at
    XDG_RUNTIME_DIR/gamescope-anw.sock, accepts connections, QUIT command
    disconnects cleanly, stock behavior unchanged without flag.
  Verification:
    meson setup build -Dharness=enabled && ninja -C build
    gamescope --backend headless --harness-mode -- sleep 60 &
    echo "QUIT" | nc -U $XDG_RUNTIME_DIR/gamescope-anw.sock
    # expect: OK\n then disconnect
  Estimated agent time: 25 min

Phase 2 — STATE command
  Files:
    src/harness/harness_socket.cpp (add STATE handler)
  Acceptance: STATE returns JSON with pid, frame counter, surface size,
    socket uptime. No crashes, correct values.
  Verification:
    echo "STATE" | nc -U $XDG_RUNTIME_DIR/gamescope-anw.sock
    # expect: OK {"pid":N,"width":W,"height":H,"uptime_s":T}\n
  Estimated agent time: 15 min

Phase 3 — Keyboard and pointer injection
  Files:
    src/harness/harness_input.cpp
    src/harness/harness_input.h
    src/harness/harness_socket.cpp (wire KEY_DOWN/UP/KEY/CLICK/MOVE)
  Acceptance: KEY 0x70 (F1) causes F1 to be received by a running xev
    or xdotool listening on the nested DISPLAY. CLICK 100 100 moves cursor
    and produces a left-click event. MOVE 200 200 moves cursor only.
  Verification:
    DISPLAY=$(gamescope nested display) xev &
    echo "KEY 0x70" | nc -U $XDG_RUNTIME_DIR/gamescope-anw.sock
    # xev should log KeyPress KeyF1 then KeyRelease KeyF1
    echo "CLICK 100 100" | nc -U $XDG_RUNTIME_DIR/gamescope-anw.sock
    # xev should log MotionNotify then ButtonPress/ButtonRelease button=1
  Estimated agent time: 40 min

Phase 4 — SCREENSHOT command
  Files:
    src/steamcompmgr_shared.hpp (add pCompletionPromise field to GamescopeScreenshotInfo)
    src/steamcompmgr.cpp (set promise value after stbi_write_png, and in error paths)
    src/harness/harness_capture.cpp
    src/harness/harness_capture.h
    src/harness/harness_socket.cpp (wire SCREENSHOT)
  Acceptance: SCREENSHOT /tmp/test.png returns OK after the file is written.
    File opens as a valid RGB PNG of the correct dimensions.
  Verification:
    echo "SCREENSHOT /tmp/harness_test.png" | nc -U $XDG_RUNTIME_DIR/gamescope-anw.sock
    # expect: OK\n (after ~33ms for one frame)
    python3 -c "from PIL import Image; img=Image.open('/tmp/harness_test.png'); print(img.size, img.mode)"
    # expect: (W, H) RGB
  Estimated agent time: 40 min

Phase 5 — Integration test with AoE3 DE
  Files: no new source changes; test scripts in harness/tests/
  Acceptance: Python client can issue STATE, KEY 0x46 (F hotkey), and
    SCREENSHOT in sequence while AoE3 DE is running under gamescope
    --harness-mode. Screenshot shows correct game frame.
  Verification:
    Run full ANW launch wrapper with --harness-mode, exercise Python client
    against live game, verify screenshot pixel matches known reference.
  Estimated agent time: 60 min (requires live game session)

Phase 6 — Robustness and shutdown
  Files:
    src/harness/harness_socket.cpp (timeout on SCREENSHOT, socket unlink on init,
      graceful thread join on StopHarnessSocket)
  Acceptance: No stale socket file on clean or crash exit. SCREENSHOT times out
    after 5s if compositor hangs. Repeated connect/disconnect cycles do not leak.
  Verification:
    Start/stop gamescope with --harness-mode N times; verify no socket files remain.
    Kill gamescope -9 mid-screenshot; verify next launch binds successfully.
  Estimated agent time: 30 min
```

---

## Appendix: Key file and line reference summary

| Area | File | Lines |
|---|---|---|
| `main()` entry point | `src/main.cpp` | 706 |
| `gamescope_options` table | `src/main.cpp` | 59-162 |
| Option parsing loop | `src/main.cpp` | 723-846 |
| Option globals | `src/main.cpp` | 290-328 |
| Option extern decls | `src/main.hpp` | 10-19 |
| `wlserver_init()` | `src/wlserver.cpp` | 1939 |
| Thread launch | `src/main.cpp` | 1075 |
| `wlserver_run()` (main thread blocks) | `src/main.cpp` | 1085 |
| `waylock` mutex | `src/wlserver.cpp` | 2143 |
| `wlserver_lock` / `wlserver_unlock` | `src/wlserver.cpp` | 2156-2165 |
| `wlserver_key()` | `src/wlserver.cpp` | 2352 |
| `wlserver_mousewarp()` | `src/wlserver.cpp` | 2709 |
| `wlserver_mousebutton()` | `src/wlserver.cpp` | 2735 |
| Libei EIS init | `src/wlserver.cpp` | 2097-2115 |
| `GamescopeInputServer::OnPollIn()` | `src/InputEmulation.cpp` | 64 |
| `GamescopeScreenshotInfo` struct | `src/steamcompmgr_shared.hpp` | 275-282 |
| `CScreenshotManager::TakeScreenshot` | `src/steamcompmgr_shared.hpp` | 287 |
| `paint_all()` | `src/steamcompmgr.cpp` | 2492 |
| Screenshot invocation in frame loop | `src/steamcompmgr.cpp` | 2838-2839 |
| Screenshot file write (PNG/BGRA) | `src/steamcompmgr.cpp` | 3070-3095 |
| `screenshotThread.detach()` | `src/steamcompmgr.cpp` | 3146 |
| `gamescope-control` protocol XML | `protocol/gamescope-control.xml` | 83-97 |
| `stb_image_write` include | `src/steamcompmgr.cpp` | 119 |
| stb subproject | `subprojects/stb/stb_image_write.h` | — |
| `eis_dep` meson dep | `src/meson.build` | 14 |
| `HAVE_LIBEIS` compile define | `src/meson.build` | 153 |
| `meson_options.txt` | `meson_options.txt` | 1-11 |
| Wayland display name pattern | `src/wlserver.cpp` | 2064-2068 |
| `wlserver.wl_display_name` | `src/wlserver.hpp` | 112 |
| XKB keymap init | `src/wlserver.cpp` | 1971-1982 |
| `BTN_LEFT` usage | `src/Backends/OpenVRBackend.cpp` | 1370 |
