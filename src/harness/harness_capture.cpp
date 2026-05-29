// harness_capture.cpp — synchronous PNG screenshot for the harness control socket.
//
// See harness_capture.h for the public API contract and concurrency notes.

#include "harness_capture.h"

#include "../steamcompmgr_shared.hpp"
#include "../steamcompmgr.hpp"   // nudge_steamcompmgr()
#include "../log.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static LogScope s_cap_log( "harness_capture" );

namespace gamescope::Harness
{

// ---------------------------------------------------------------------------
// Shutdown flag
// ---------------------------------------------------------------------------

static std::atomic<bool> s_bShuttingDown{ false };

// ---------------------------------------------------------------------------
// Path validation
// ---------------------------------------------------------------------------

// Returns an empty string on success, or a short reason phrase on failure.
static std::string validate_path( const std::string &path )
{
    // 1. Must be absolute.
    if ( path.empty() || path[0] != '/' )
        return "not_absolute";

    // 2. Must not contain '..'.
    if ( path.find("..") != std::string::npos )
        return "dotdot_not_allowed";

    // 3. Must not exceed PATH_MAX - 1.
    if ( path.size() >= static_cast<size_t>(PATH_MAX) )
        return "path_too_long";

    // 4. Must have a .png extension (CScreenshotManager writes PNG for .png).
    {
        std::filesystem::path fsp( path );
        if ( fsp.extension() != ".png" )
            return "must_have_png_extension";
    }

    // 5. Parent directory must exist and be writable by the current process.
    {
        std::filesystem::path parent = std::filesystem::path( path ).parent_path();
        struct stat pst{};
        if ( stat( parent.c_str(), &pst ) != 0 )
            return "parent_dir_not_found";
        if ( !S_ISDIR( pst.st_mode ) )
            return "parent_not_a_directory";
        if ( access( parent.c_str(), W_OK ) != 0 )
            return "parent_not_writable";
    }

    return {}; // success
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ScreenshotOutcome capture_to_path( const std::string &host_path )
{
    // Fast path: already shutting down.
    if ( s_bShuttingDown.load( std::memory_order_acquire ) )
    {
        return { ScreenshotResult::ShuttingDown, {}, 0,
                 "harness is shutting down" };
    }

    // Validate path before touching CScreenshotManager.
    std::string path_err = validate_path( host_path );
    if ( !path_err.empty() )
    {
        return { ScreenshotResult::InvalidPath, {}, 0, path_err };
    }

    // Build the promise/future pair.  The shared_ptr keeps the promise alive
    // across the screenshotThread.detach() in steamcompmgr.cpp.
    auto pPromise = std::make_shared<std::promise<bool>>();
    std::future<bool> future = pPromise->get_future();

    // Enqueue the screenshot request.
    gamescope::CScreenshotManager::Get().TakeScreenshot(
        gamescope::GamescopeScreenshotInfo{
            .szScreenshotPath      = host_path,
            .eScreenshotType       = GAMESCOPE_CONTROL_SCREENSHOT_TYPE_FULL_COMPOSITION,
            .uScreenshotFlags      = 0,
            .bX11PropertyRequested = false,
            .bWaylandRequested     = false,
            .pCompletionPromise    = pPromise,
        }
    );

    // OQ-5: nudge the compositor so it wakes from PollEvents() even when idle.
    nudge_steamcompmgr();

    // OQ-1: wait with a 5-second timeout to avoid blocking the harness thread
    // indefinitely if gamescope is shutting down or the compositor hangs.
    auto status = future.wait_for( std::chrono::seconds( 5 ) );

    if ( s_bShuttingDown.load( std::memory_order_acquire ) )
    {
        return { ScreenshotResult::ShuttingDown, {}, 0,
                 "harness shut down during capture" };
    }

    if ( status == std::future_status::timeout )
    {
        s_cap_log.warnf( "SCREENSHOT timed out waiting for compositor: %s", host_path.c_str() );
        return { ScreenshotResult::Timeout, {}, 0, "compositor did not respond within 5s" };
    }

    // gamescope is built with -fno-exceptions; future.get() cannot throw here.
    // The promise is always set exactly once (by PromiseGuard or the early-exit
    // paths in steamcompmgr.cpp), so get() is safe to call without try/catch.
    bool bOk = future.get();

    if ( !bOk )
    {
        s_cap_log.errorf( "CScreenshotManager reported failure for: %s", host_path.c_str() );
        return { ScreenshotResult::Failed, host_path, 0, "compositor write failed" };
    }

    // Confirm the file was actually written and is non-empty.
    struct stat fst{};
    if ( stat( host_path.c_str(), &fst ) != 0 || fst.st_size <= 0 )
    {
        s_cap_log.errorf( "screenshot file missing or zero-size: %s", host_path.c_str() );
        return { ScreenshotResult::Failed, host_path, 0, "file missing or zero-size after write" };
    }

    return { ScreenshotResult::Ok, host_path, static_cast<size_t>( fst.st_size ), {} };
}

void signal_shutdown()
{
    s_bShuttingDown.store( true, std::memory_order_release );
}

} // namespace gamescope::Harness
