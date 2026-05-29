#!/usr/bin/env bash
# Smoke test for harness phases 1-4.
# Exercises STATE / KEY / MOVE / CLICK / QUIT against a gamescope instance.
# Exit 0 on all-pass, 1 with diagnostics on first failure.
#
# Prerequisites:
#   - Run on a system where /tmp/.X11-unix is owned by root or the running user.
#     Inside the gs-build distrobox, /tmp/.X11-unix is owned by 'nobody', which
#     prevents gamescope's embedded XWayland from binding. Run this test on the
#     host Wayland session or inside the live AoE3 gamescope session instead.
#   - WAYLAND_DISPLAY must point to a live Wayland compositor.
#     Example: WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 bash phase1-4_smoke.sh
#   - The gamescope --backend wayland flag is used so gamescope nests inside the
#     running compositor without requiring a DRM session.

set -euo pipefail

GAMESCOPE_BIN="${GAMESCOPE_BIN:-/var/home/jflessenkemper/AOE-3-DE-Harness/build/src/gamescope}"
SOCK="${HARNESS_SOCK:-/tmp/gs-smoke-test.sock}"
WIDTH=1280
HEIGHT=720

if [ ! -x "$GAMESCOPE_BIN" ]; then
    echo "FAIL: gamescope binary not found at $GAMESCOPE_BIN" >&2
    exit 1
fi

# Clean up any leftover socket from a previous run.
rm -f "$SOCK"

start_gamescope() {
    # Use 'wayland' backend (nested inside the host compositor).
    # 'headless' backend also works but still requires XWayland to initialise,
    # so either backend needs a usable /tmp/.X11-unix directory.
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
    rm -f "$SOCK"
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
# VK_F1 = 0x70. Under headless there is no visible output but there must be no crash.
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

# Final STATE check: gamescope must not have crashed.
RESP=$(send_cmd "STATE")
echo "$RESP" | grep -q "^OK pid=" || fail "Final STATE failed after input injection: '$RESP'"
pass "Final STATE OK (gamescope survived input injection)"

echo ""
echo "All phase 1-4 smoke tests PASSED."
exit 0
