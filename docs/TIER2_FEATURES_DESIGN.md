# Tier-2 Features Design: AOE3DEHarness

_Design-only document. Date: 2026-05-29._

---

## Feature D: Crash Detection + Auto-Restart (`WATCHDOG_ENABLE`)

### Approach

The inner command is spawned inside `LaunchNestedChildren()` → `SpawnProcessInWatchdog()` (steamcompmgr.cpp:8305), which ultimately calls `execvp()` via `Process::SpawnProcess()`. The returned PID (`nPrimaryChildPid`) lives in a detached thread.

Rather than entangling the watchdog with the upstream reaper/wait machinery, we implement a **self-contained `harness_watchdog` module** (`src/harness/harness_watchdog.{h,cpp}`):

- Stores the inner command argv (`char **`) and its spawned PID.
- A watchdog thread calls `waitpid(pid, WNOHANG)` once per second; on death it saves the exit code, decrements restart budget, emits `WATCHDOG: RESTART <n_remaining>\n` to PROBE_TAIL subscribers via the existing broadcast path, then re-spawns via `Process::SpawnProcess()` after `restart_delay_ms`.
- When budget hits 0 it emits `WATCHDOG: EXHAUSTED\n` and disables itself.

New socket commands:
- `WATCHDOG_ENABLE <max_restarts> <restart_delay_ms>` — arms watchdog; returns `OK\n`.
- `WATCHDOG_DISABLE` — disarms; returns `OK\n`.
- `WATCHDOG_STATUS` — returns `OK enabled=<0|1> restarts_used=N restarts_remaining=N last_exit_code=N\n`.

Emit events to PROBE_TAIL subscribers (simplest, reuses existing broadcast). **Opt-in only** — not auto-enabled.

---

## Feature E: `--harness-headless` Convenience Flag

### Approach

Pure convenience alias in `main.cpp` getopt block. No new binary; no new subsystem.

When `--harness-headless` is parsed:
1. Force `eCurrentBackend = gamescope::GamescopeBackend::Headless`.
2. Set `g_nNestedWidth = g_nNestedHeight = g_nPreferredOutputWidth = g_nPreferredOutputHeight = 1` (1×1 output — minimal composition cost).
3. Log a note that headless+harness mode is active.

Control socket starts normally. Inner command + full Wayland/XWayland stack still run.

**Why not just `--backend headless -W 1 -H 1`?** Convenience for CI scripts that shouldn't need to know the resolution flag names.

---
