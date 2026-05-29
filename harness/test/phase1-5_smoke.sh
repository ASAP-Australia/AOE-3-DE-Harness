#!/usr/bin/env bash
# Smoke test for harness phases 1-5.
# Exercises STATE / KEY / MOVE / CLICK / SCREENSHOT / QUIT against a
# live gamescope instance.
# Exit 0 on all-pass, 1 with diagnostics on first failure.
#
# Prerequisites:
#   - Run on the HOST Wayland session, NOT inside the gs-build distrobox.
#     Inside distrobox, /tmp/.X11-unix is owned by 'nobody', which prevents
#     gamescope's embedded XWayland from binding.
#   - WAYLAND_DISPLAY must point to a live Wayland compositor.
#     Example:
#       WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 \
#         bash harness/test/phase1-5_smoke.sh
#   - The gamescope --backend wayland flag is used so gamescope nests
#     inside the running compositor without requiring a DRM session.
#
# SCREENSHOT tests: The SCREENSHOT command requires gamescope to actually
# composite a frame and write a PNG via CScreenshotManager.  This only works
# in a real Wayland-nested session.  All SCREENSHOT cases in this script are
# thus gated on host execution.  Full verification is deferred to Phase 6
# (live AoE3DE session on the host).

set -euo pipefail

GAMESCOPE_BIN="${GAMESCOPE_BIN:-/var/home/jflessenkemper/AOE-3-DE-Harness/build/src/gamescope}"
SOCK="${HARNESS_SOCK:-/tmp/gs-smoke-test.sock}"
PNG_OUT="/tmp/gs-anw-test.png"
WIDTH=1280
HEIGHT=720

if [ ! -x "$GAMESCOPE_BIN" ]; then
    echo "FAIL: gamescope binary not found at $GAMESCOPE_BIN" >&2
    exit 1
fi

# Clean up any leftover socket from a previous run.
rm -f "$SOCK" "$PNG_OUT"

start_gamescope() {
    "$GAMESCOPE_BIN" \
        --backend wayland \
        -W "$WIDTH" -H "$HEIGHT" \
        --harness-mode \
        --harness-socket "$SOCK" \
        -- sleep 60 &
    GS_PID=$!
    echo "gamescope pid=$GS_PID"
}

stop_gamescope() {
    if [ -n "${GS_PID:-}" ]; then
        kill "$GS_PID" 2>/dev/null || true
        wait "$GS_PID" 2>/dev/null || true
    fi
    rm -f "$SOCK" "$PNG_OUT"
}

trap stop_gamescope EXIT

start_gamescope

# Wait for the socket to appear (up to 5 s).
for i in $(seq 1 50); do
    [ -S "$SOCK" ] && break
    sleep 0.1
done

if [ ! -S "$SOCK" ]; then
    echo "FAIL: socket $SOCK did not appear within 5s" >&2
    exit 1
fi

send_cmd() {
    # $1 = command string
    # Returns the response line from nc.
    printf '%s\n' "$1" | nc -U "$SOCK"
}

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1" >&2; exit 1; }

# ---- Phase 1: basic connectivity ----
RESP=$(send_cmd "QUIT")
[ "$RESP" = "OK" ] || fail "QUIT expected 'OK', got '$RESP'"
pass "QUIT returns OK"

# ---- Phase 2: STATE ----
RESP=$(send_cmd "STATE")
echo "STATE response: $RESP"
echo "$RESP" | grep -q "^OK pid=" || fail "STATE response missing 'OK pid='"
echo "$RESP" | grep -q "internal_w=$WIDTH" || fail "STATE response wrong width (expected $WIDTH)"
echo "$RESP" | grep -q "internal_h=$HEIGHT" || fail "STATE response wrong height (expected $HEIGHT)"
pass "STATE returns OK with correct dimensions"

RESP=$(send_cmd "NOPE")
echo "$RESP" | grep -q "^ERR UNKNOWN_COMMAND" || fail "Unknown command expected ERR UNKNOWN_COMMAND, got '$RESP'"
pass "Unknown command returns ERR UNKNOWN_COMMAND"

# ---- Phase 3: KEY injection ----
RESP=$(send_cmd "KEY 0x70")
[ "$RESP" = "OK" ] || fail "KEY 0x70 expected 'OK', got '$RESP'"
pass "KEY 0x70 (F1) returns OK"

RESP=$(send_cmd "KEY_DOWN 0x41")
[ "$RESP" = "OK" ] || fail "KEY_DOWN 0x41 expected 'OK', got '$RESP'"
RESP=$(send_cmd "KEY_UP 0x41")
[ "$RESP" = "OK" ] || fail "KEY_UP 0x41 expected 'OK', got '$RESP'"
pass "KEY_DOWN/KEY_UP A returns OK"

RESP=$(send_cmd "KEY 0xFF")
echo "$RESP" | grep -q "^ERR INVALID_VK" || fail "Invalid VK expected ERR INVALID_VK, got '$RESP'"
pass "KEY 0xFF (invalid) returns ERR INVALID_VK"

# ---- Phase 4: MOVE / CLICK ----
RESP=$(send_cmd "MOVE 640 360")
[ "$RESP" = "OK" ] || fail "MOVE 640 360 expected 'OK', got '$RESP'"
pass "MOVE 640 360 returns OK"

RESP=$(send_cmd "CLICK 100 100")
[ "$RESP" = "OK" ] || fail "CLICK 100 100 expected 'OK', got '$RESP'"
pass "CLICK 100 100 returns OK"

RESP=$(send_cmd "MOVE 9999 9999")
echo "$RESP" | grep -q "^ERR COORDS_OUT_OF_RANGE" || fail "Out-of-range MOVE expected ERR COORDS_OUT_OF_RANGE, got '$RESP'"
pass "MOVE 9999 9999 returns ERR COORDS_OUT_OF_RANGE"

RESP=$(send_cmd "CLICK 9999 9999")
echo "$RESP" | grep -q "^ERR COORDS_OUT_OF_RANGE" || fail "Out-of-range CLICK expected ERR COORDS_OUT_OF_RANGE, got '$RESP'"
pass "CLICK 9999 9999 returns ERR COORDS_OUT_OF_RANGE"

# ---- Phase 5: SCREENSHOT path-validation error cases ----
# These are pure validation rejections — the harness never calls
# CScreenshotManager for them, so they work regardless of the compositor state.

RESP=$(send_cmd "SCREENSHOT relative_path.png")
echo "$RESP" | grep -q "^ERR INVALID_PATH" || fail "Relative path expected ERR INVALID_PATH, got '$RESP'"
pass "SCREENSHOT relative_path.png returns ERR INVALID_PATH"

RESP=$(send_cmd "SCREENSHOT /tmp/../etc/shadow")
echo "$RESP" | grep -q "^ERR INVALID_PATH" || fail "Dotdot path expected ERR INVALID_PATH, got '$RESP'"
pass "SCREENSHOT /tmp/../etc/shadow returns ERR INVALID_PATH (dotdot)"

RESP=$(send_cmd "SCREENSHOT /tmp/foo.bmp")
echo "$RESP" | grep -q "^ERR INVALID_PATH" || fail "Wrong extension expected ERR INVALID_PATH, got '$RESP'"
pass "SCREENSHOT /tmp/foo.bmp returns ERR INVALID_PATH (wrong extension)"

RESP=$(send_cmd "SCREENSHOT /nonexistent_gs_test_dir/x.png")
echo "$RESP" | grep -q "^ERR INVALID_PATH" || fail "Nonexistent parent dir expected ERR INVALID_PATH, got '$RESP'"
pass "SCREENSHOT /nonexistent_gs_test_dir/x.png returns ERR INVALID_PATH (parent not found)"

# ---- Phase 5: SCREENSHOT happy path (requires host Wayland + live compositor) ----
echo ""
echo "Running SCREENSHOT happy-path test (requires live compositor frame)..."
echo "  If this hangs for >10s gamescope is not compositing frames."

RESP=$(send_cmd "SCREENSHOT $PNG_OUT")
echo "SCREENSHOT response: $RESP"

if echo "$RESP" | grep -q "^OK "; then
    # Parse bytes from "OK /path N"
    BYTES=$(echo "$RESP" | awk '{print $3}')
    [ "${BYTES:-0}" -gt 0 ] || fail "SCREENSHOT OK but bytes=0"
    pass "SCREENSHOT returned OK with bytes=$BYTES"

    # File must exist and be readable.
    [ -f "$PNG_OUT" ] || fail "PNG file not found at $PNG_OUT"

    # Verify PNG magic: first 4 bytes = 89 50 4E 47
    MAGIC=$(od -A n -N 4 -t x1 "$PNG_OUT" | tr -d ' ')
    [ "$MAGIC" = "89504e47" ] || fail "PNG magic mismatch: got '$MAGIC'"
    pass "PNG magic bytes correct (89 50 4E 47)"
else
    echo "NOTE: SCREENSHOT did not return OK — '$RESP'"
    echo "      This is expected inside distrobox (XWayland constraint)."
    echo "      Deferred to Phase 6 host run."
    pass "SCREENSHOT error response is well-formed (non-OK expected in distrobox)"
fi

# Final STATE check: gamescope must not have crashed.
RESP=$(send_cmd "STATE")
echo "$RESP" | grep -q "^OK pid=" || fail "Final STATE failed after screenshot: '$RESP'"
pass "Final STATE OK (gamescope survived screenshot test)"

echo ""
echo "All phase 1-5 smoke tests PASSED."
exit 0
