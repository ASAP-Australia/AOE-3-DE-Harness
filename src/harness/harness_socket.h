#pragma once

#include <string>

// Control socket for the ANW harness. Provides a line-based Unix-domain
// socket that accepts KEY_DOWN/KEY_UP/KEY/MOVE/CLICK/STATE/QUIT commands
// from a Python client, enabling input injection and state queries without
// any in-Wine instrumentation.
//
// All public functions are safe to call from any thread. The implementation
// serialises all command handling on the gs-harness thread.

namespace gamescope::Harness
{

// Spawn the gs-harness thread, unlink any stale socket file, bind, and start
// accepting connections. No-op if called twice.
void StartHarnessSocket( const std::string &socketPath );

// Signal the gs-harness thread to stop and wait for it to exit. Closes the
// listen socket and removes the socket file.
void StopHarnessSocket();

} // namespace gamescope::Harness
