#pragma once

// harness_watchdog.h — Socket-controlled auto-restart of the inner command.
//
// The watchdog is OFF by default. It is enabled via the WATCHDOG_ENABLE socket
// command and operates independently of the upstream gamescopereaper process.
//
// Public API (all functions are thread-safe):
//
//   HarnessWatchdogSetInnerCommand(argv, pid)
//       Called once from LaunchNestedChildren() after the inner process is
//       spawned.  Stores the argv for re-spawning and the current PID for
//       death detection.  argv must remain valid for the harness lifetime
//       (it points into the original main() argv array).
//
//   HarnessWatchdogEnable(max_restarts, restart_delay_ms)
//       Arms the watchdog.  The poll thread wakes up and starts monitoring.
//
//   HarnessWatchdogDisable()
//       Disarms the watchdog without stopping the poll thread.
//
//   HarnessWatchdogStatus(out_enabled, out_used, out_remaining, out_exit_code)
//       Fills caller-supplied ints with current state.
//
//   HarnessWatchdogStop()
//       Joins the poll thread; called on harness shutdown.

#include <sys/types.h>

namespace gamescope::Harness
{

// Called from steamcompmgr LaunchNestedChildren after SpawnProcessInWatchdog.
// inner_pid  = PID of the reaper (or direct child) that wraps the inner command.
// argv       = null-terminated list; caller retains ownership (points into main argv).
void HarnessWatchdogSetInnerCommand( char **argv, pid_t inner_pid );

// Socket command implementations — return strings sent as OK/ERR responses.
void HarnessWatchdogEnable( int max_restarts, int restart_delay_ms );
void HarnessWatchdogDisable();
void HarnessWatchdogStatus( int *out_enabled, int *out_used,
                             int *out_remaining, int *out_last_exit_code );

// Broadcast a watchdog event line to all PROBE_TAIL subscribers.
// line is a null-terminated string without a trailing newline.
// (Implemented in harness_socket.cpp — declared here for watchdog.cpp to call.)
void HarnessWatchdogBroadcast( const char *line );

// Start/stop the background poll thread.  Start is called from
// StartHarnessSocket(); Stop from StopHarnessSocket().
void HarnessWatchdogStart();
void HarnessWatchdogStop();

} // namespace gamescope::Harness
