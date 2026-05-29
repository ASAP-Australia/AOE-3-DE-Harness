#pragma once

// harness_capture.h — synchronous PNG screenshot capture for the harness socket.
//
// Provides capture_to_path(), which validates the requested file path, enqueues
// a GamescopeScreenshotInfo with a std::shared_ptr<std::promise<bool>> completion
// signal, calls nudge_steamcompmgr() (OQ-5: wake an idle compositor), and waits
// on the resulting future with a 5-second timeout (OQ-1: no indefinite block).
//
// Concurrency assumption: the harness socket is single-client-at-a-time (see
// harness_socket.cpp — serial accept loop).  capture_to_path() may therefore be
// called from only one thread at a time.  Internal state (the shutdown flag) is
// still protected by an atomic for correctness across the signal_shutdown() path.

#include <cstddef>
#include <cstdint>
#include <string>

namespace gamescope::Harness
{

enum class ScreenshotResult
{
    Ok,
    Timeout,
    Failed,
    InvalidPath,
    ShuttingDown,
};

struct ScreenshotOutcome
{
    ScreenshotResult result       = ScreenshotResult::Failed;
    std::string      path;           // resolved absolute path (meaningful on Ok)
    size_t           bytes_written   = 0; // only meaningful when Ok
    std::string      error_detail;        // human-readable; populated on errors
};

// Synchronous screenshot capture.
// Validates path, enqueues via CScreenshotManager, nudges the compositor,
// waits up to 5 s, returns the outcome.
// Single-caller assumption — do not call concurrently.
ScreenshotOutcome capture_to_path( const std::string &host_path );

// Synchronous region screenshot capture.
// Same as capture_to_path() but clips to (x, y, w, h) in logical 1920x1080
// coordinates before writing the PNG.  Bounds validation is performed here
// (before enqueueing) so callers receive an immediate ERR on bad input.
ScreenshotOutcome capture_region_to_path( const std::string &host_path,
                                          uint32_t x, uint32_t y,
                                          uint32_t w, uint32_t h );

// Call from harness_stop() to unblock any in-flight capture immediately
// rather than waiting for the 5-second timeout.
void signal_shutdown();

} // namespace gamescope::Harness
