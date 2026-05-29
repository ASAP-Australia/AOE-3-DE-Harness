#pragma once

// harness_probe.h — PROBE_TAIL inotify-based log tail for the harness socket.
//
// Usage:
//   ProbeSubscription *ps = probe_subscribe( conn_fd, "/path/to/Age3Log.txt" );
//   // returns nullptr + sends ERR on failure
//   ...
//   probe_unsubscribe( ps );  // stops the tail thread, sends PROBE: EOF\n, joins
//
// The tail thread:
//   - Opens the log file read-only and seeks to EOF on subscribe.
//   - Waits for IN_MODIFY via inotify with a 200 ms timeout for clean shutdown.
//   - Filters lines to those containing "[LLP v=2]".
//   - Uses a 512-entry ring buffer (lines capped at 1024 B) for backpressure.
//   - Writes "PROBE: <line>\n" to conn_fd using MSG_DONTWAIT; drops+counts on EAGAIN.
//   - Emits "PROBE: DROPPED <n>\n" when dropping.
//
// Thread safety:
//   probe_subscribe / probe_unsubscribe are called from the serial handle_client()
//   loop — never concurrently.  The tail thread is the only other accessor of the
//   ProbeSubscription internals after subscribe returns.

#include <string>

namespace gamescope::Harness
{

// Opaque subscription handle.
struct ProbeSubscription;

// Start a tail subscription on log_path for client fd conn_fd.
// On success, returns a non-null handle and has already sent "OK SUBSCRIBED\n".
// On failure, sends "ERR <reason>\n" to conn_fd and returns nullptr.
ProbeSubscription *probe_subscribe( int conn_fd, const std::string &log_path );

// Stop the subscription: signals the tail thread, joins it, sends "PROBE: EOF\n"
// to conn_fd, then sends "OK\n" (the PROBE_UNSUB response).
// Safe to call on nullptr (no-op).
void probe_unsubscribe( ProbeSubscription *ps );

// Called on client disconnect — like probe_unsubscribe but does NOT send OK\n
// (the fd is already closing).  Safe on nullptr.
void probe_disconnect( ProbeSubscription *ps );

} // namespace gamescope::Harness
