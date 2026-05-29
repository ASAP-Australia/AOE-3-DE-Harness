// Harness control socket — Unix-domain line-based command server.
//
// Protocol: ASCII, '\n'-delimited. Max line length: HARNESS_MAX_LINE.
// Request:  VERB [ARG...]\n
// Response: OK [data]\n  |  ERR REASON\n
//
// Commands handled here:
//   STATE                         — returns pid, uptime_ms, internal_w, internal_h, ready
//   QUIT                          — closes the current client connection cleanly
//   KEY_DOWN VK                   — press key (Win32 VK hex, e.g. 0x70)
//   KEY_UP VK                     — release key
//   KEY VK                        — press+release
//   MOVE X Y                      — absolute pointer warp
//   CLICK X Y                     — move + left button press+release
//   SCREENSHOT PATH               — capture composite framebuffer to PNG at PATH
//   SCREENSHOT_REGION X Y W H PATH — capture sub-region to PNG (logical 1920x1080 coords)
//   PROBE_TAIL PATH               — subscribe: stream [LLP v=2] lines as "PROBE: <line>\n"
//   PROBE_UNSUB                   — unsubscribe: harness emits "PROBE: EOF\n" then "OK\n"
//   WATCHDOG_ENABLE <max> <delay> — arm auto-restart; max=max_restarts, delay=ms between restarts
//   WATCHDOG_DISABLE              — disarm watchdog
//   WATCHDOG_STATUS               — query current watchdog state

#include "harness_socket.h"
#include "harness_capture.h"
#include "harness_input.h"
#include "harness_probe.h"
#include "harness_watchdog.h"

#include "../main.hpp"
#include "../log.hpp"
#include "../steamcompmgr.hpp" // get_time_in_milliseconds

#include <atomic>
#include <cerrno>
#include <climits> // PATH_MAX — GCC 16+ no longer transitively pulls it in
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

// Maximum bytes in a single command line (including the terminating '\n').
// Commands longer than this are rejected rather than buffered unboundedly.
static constexpr size_t HARNESS_MAX_LINE = 1024;

// Match the DLL convention for key-press hold time.
static constexpr int KEY_PRESS_DELAY_MS = 30;

static LogScope s_harness_log( "harness" );

namespace gamescope::Harness
{

static std::atomic<bool> s_bRunning{ false };
static int               s_nListenFd = -1;
static pthread_t         s_thread;
static std::string       s_socketPath;

// ---------------------------------------------------------------------------
// PROBE_TAIL subscriber list — used by HarnessWatchdogBroadcast()
// Each entry is an open client fd that has an active PROBE_TAIL subscription.
// We piggyback on the subscriber fd to deliver WATCHDOG: events.
// ---------------------------------------------------------------------------

static std::mutex          s_probe_fd_mutex;
static std::vector<int>    s_probe_fds; // fds with active probe subscriptions

// Called from harness_probe.cpp indirectly — we track the fd separately.
// Register/unregister are called from cmd_probe_tail / cmd_probe_unsub.
static void probe_fd_register( int fd )
{
    std::lock_guard<std::mutex> lk( s_probe_fd_mutex );
    s_probe_fds.push_back( fd );
}

static void probe_fd_unregister( int fd )
{
    std::lock_guard<std::mutex> lk( s_probe_fd_mutex );
    s_probe_fds.erase(
        std::remove( s_probe_fds.begin(), s_probe_fds.end(), fd ),
        s_probe_fds.end() );
}

// Broadcast a WATCHDOG: line to all PROBE_TAIL subscribers.
// Callers pass the suffix only (e.g. "RESTART 2", "EXHAUSTED") — this function
// prepends "WATCHDOG: " and appends "\n".
// Defined here (and declared in harness_watchdog.h) so it can reach s_probe_fds.
void HarnessWatchdogBroadcast( const char *line )
{
    char buf[256];
    int n = snprintf( buf, sizeof( buf ), "WATCHDOG: %s\n", line );
    if ( n <= 0 )
        return;

    std::lock_guard<std::mutex> lk( s_probe_fd_mutex );
    for ( int fd : s_probe_fds )
    {
        if ( fd >= 0 )
            send( fd, buf, (size_t)n, MSG_DONTWAIT );
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void send_response( int fd, const char *msg )
{
    size_t len = strlen( msg );
    ssize_t wrote = write( fd, msg, len );
    if ( wrote < 0 )
        s_harness_log.errorf_errno( "write response to client fd %d failed", fd );
}

// Split a null-terminated line (without the trailing '\n') into verb + args.
// Returns verb. *args_out points to the first character after the verb
// (possibly pointing to '\0' if there are no args).
static const char *split_verb( char *line, const char **args_out )
{
    char *p = line;
    while ( *p && *p != ' ' )
        ++p;
    if ( *p == ' ' )
    {
        *p = '\0';
        *args_out = p + 1;
    }
    else
    {
        *args_out = p; // empty string
    }
    return line;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void cmd_state( int fd )
{
    unsigned int uptime_ms = get_time_in_milliseconds();
    // focusWindow_pid is set (non-zero) by steamcompmgr once the inner game
    // window has been mapped and has committed its first composited frame.
    // ready=1 means "past splash / on the main menu"; ready=0 means no client
    // window is compositing yet.
    int ready = ( focusWindow_pid != 0 ) ? 1 : 0;
    char buf[256];
    snprintf( buf, sizeof( buf ),
              "OK pid=%d uptime_ms=%u internal_w=%u internal_h=%u ready=%d\n",
              (int)getpid(), uptime_ms,
              g_nOutputWidth, g_nOutputHeight, ready );
    send_response( fd, buf );
}

static void cmd_key_down( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARG VK\n" );
        return;
    }

    char *end = nullptr;
    unsigned long vk = strtoul( args, &end, 16 );
    if ( end == args || *end != '\0' )
    {
        send_response( fd, "ERR BAD_VK_FORMAT\n" );
        return;
    }

    if ( !HarnessKeyDown( (uint32_t)vk ) )
    {
        char buf[64];
        snprintf( buf, sizeof( buf ), "ERR INVALID_VK %s\n", args );
        send_response( fd, buf );
        return;
    }

    send_response( fd, "OK\n" );
}

static void cmd_key_up( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARG VK\n" );
        return;
    }

    char *end = nullptr;
    unsigned long vk = strtoul( args, &end, 16 );
    if ( end == args || *end != '\0' )
    {
        send_response( fd, "ERR BAD_VK_FORMAT\n" );
        return;
    }

    if ( !HarnessKeyUp( (uint32_t)vk ) )
    {
        char buf[64];
        snprintf( buf, sizeof( buf ), "ERR INVALID_VK %s\n", args );
        send_response( fd, buf );
        return;
    }

    send_response( fd, "OK\n" );
}

static void cmd_key( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARG VK\n" );
        return;
    }

    char *end = nullptr;
    unsigned long vk = strtoul( args, &end, 16 );
    if ( end == args || *end != '\0' )
    {
        send_response( fd, "ERR BAD_VK_FORMAT\n" );
        return;
    }

    if ( !HarnessKey( (uint32_t)vk ) )
    {
        char buf[64];
        snprintf( buf, sizeof( buf ), "ERR INVALID_VK %s\n", args );
        send_response( fd, buf );
        return;
    }

    send_response( fd, "OK\n" );
}

static void cmd_move( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARGS X Y\n" );
        return;
    }

    int x = 0, y = 0;
    if ( sscanf( args, "%d %d", &x, &y ) != 2 )
    {
        send_response( fd, "ERR BAD_COORD_FORMAT\n" );
        return;
    }

    // Validate against the current output dimensions.
    if ( x < 0 || y < 0 ||
         (uint32_t)x >= g_nOutputWidth ||
         (uint32_t)y >= g_nOutputHeight )
    {
        send_response( fd, "ERR COORDS_OUT_OF_RANGE\n" );
        return;
    }

    if ( !HarnessMove( (double)x, (double)y ) )
    {
        send_response( fd, "ERR MOVE_FAILED\n" );
        return;
    }

    send_response( fd, "OK\n" );
}

static void cmd_click( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARGS X Y\n" );
        return;
    }

    int x = 0, y = 0;
    if ( sscanf( args, "%d %d", &x, &y ) != 2 )
    {
        send_response( fd, "ERR BAD_COORD_FORMAT\n" );
        return;
    }

    if ( x < 0 || y < 0 ||
         (uint32_t)x >= g_nOutputWidth ||
         (uint32_t)y >= g_nOutputHeight )
    {
        send_response( fd, "ERR COORDS_OUT_OF_RANGE\n" );
        return;
    }

    if ( !HarnessClick( (double)x, (double)y ) )
    {
        send_response( fd, "ERR CLICK_FAILED\n" );
        return;
    }

    send_response( fd, "OK\n" );
}

static void cmd_screenshot_region( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARGS x y w h path\n" );
        return;
    }

    int x = 0, y = 0, w = 0, h = 0;
    char path[PATH_MAX];
    path[0] = '\0';

    // Parse: "<x> <y> <w> <h> <path>"
    if ( sscanf( args, "%d %d %d %d %4095s", &x, &y, &w, &h, path ) != 5 )
    {
        send_response( fd, "ERR BAD_FORMAT expected: x y w h path\n" );
        return;
    }

    if ( x < 0 || y < 0 || w <= 0 || h <= 0 )
    {
        send_response( fd, "ERR INVALID_REGION negative_or_zero_dimension\n" );
        return;
    }

    // Bounds checked further inside capture_region_to_path against g_nOutputWidth/Height.
    ScreenshotOutcome out = capture_region_to_path(
        std::string( path ),
        (uint32_t)x, (uint32_t)y,
        (uint32_t)w, (uint32_t)h );

    char buf[PATH_MAX + 128];
    switch ( out.result )
    {
    case ScreenshotResult::Ok:
        snprintf( buf, sizeof( buf ), "OK %s %zu\n",
                  out.path.c_str(), out.bytes_written );
        break;
    case ScreenshotResult::Timeout:
        snprintf( buf, sizeof( buf ), "ERR SCREENSHOT_TIMEOUT\n" );
        break;
    case ScreenshotResult::Failed:
        snprintf( buf, sizeof( buf ), "ERR SCREENSHOT_FAILED\n" );
        break;
    case ScreenshotResult::InvalidPath:
        snprintf( buf, sizeof( buf ), "ERR INVALID_REGION %s\n",
                  out.error_detail.c_str() );
        break;
    case ScreenshotResult::ShuttingDown:
        snprintf( buf, sizeof( buf ), "ERR HARNESS_SHUTTING_DOWN\n" );
        break;
    }
    send_response( fd, buf );
}

static void cmd_screenshot( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR INVALID_PATH missing_argument\n" );
        return;
    }

    // Trim leading whitespace from the path argument.
    while ( *args == ' ' ) ++args;

    if ( !*args )
    {
        send_response( fd, "ERR INVALID_PATH empty_path\n" );
        return;
    }

    ScreenshotOutcome out = capture_to_path( std::string( args ) );

    char buf[PATH_MAX + 128];
    switch ( out.result )
    {
    case ScreenshotResult::Ok:
        snprintf( buf, sizeof( buf ), "OK %s %zu\n",
                  out.path.c_str(), out.bytes_written );
        break;
    case ScreenshotResult::Timeout:
        snprintf( buf, sizeof( buf ), "ERR SCREENSHOT_TIMEOUT\n" );
        break;
    case ScreenshotResult::Failed:
        snprintf( buf, sizeof( buf ), "ERR SCREENSHOT_FAILED\n" );
        break;
    case ScreenshotResult::InvalidPath:
        snprintf( buf, sizeof( buf ), "ERR INVALID_PATH %s\n",
                  out.error_detail.c_str() );
        break;
    case ScreenshotResult::ShuttingDown:
        snprintf( buf, sizeof( buf ), "ERR HARNESS_SHUTTING_DOWN\n" );
        break;
    }
    send_response( fd, buf );
}

static void cmd_probe_tail( int fd, const char *args,
                             ProbeSubscription **pp_sub )
{
    // Trim leading whitespace.
    while ( args && *args == ' ' ) ++args;

    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARG path\n" );
        return;
    }

    // Stop any existing subscription on this connection first.
    if ( *pp_sub )
    {
        probe_fd_unregister( fd );
        probe_unsubscribe( *pp_sub );
        *pp_sub = nullptr;
    }

    // Start new subscription — probe_subscribe sends OK SUBSCRIBED\n or ERR.
    *pp_sub = probe_subscribe( fd, std::string( args ) );
    if ( *pp_sub )
        probe_fd_register( fd );
}

static void cmd_probe_unsub( int fd, ProbeSubscription **pp_sub )
{
    if ( !*pp_sub )
    {
        send_response( fd, "ERR NO_ACTIVE_SUBSCRIPTION\n" );
        return;
    }
    probe_fd_unregister( fd );
    probe_unsubscribe( *pp_sub ); // sends PROBE: EOF\n then OK\n
    *pp_sub = nullptr;
}

static void cmd_watchdog_enable( int fd, const char *args )
{
    if ( !args || !*args )
    {
        send_response( fd, "ERR MISSING_ARGS max_restarts restart_delay_ms\n" );
        return;
    }
    int max_restarts = 0, delay_ms = 0;
    if ( sscanf( args, "%d %d", &max_restarts, &delay_ms ) != 2 )
    {
        send_response( fd, "ERR BAD_FORMAT expected: <max_restarts> <restart_delay_ms>\n" );
        return;
    }
    if ( max_restarts < 0 || delay_ms < 0 )
    {
        send_response( fd, "ERR NEGATIVE_ARG\n" );
        return;
    }
    HarnessWatchdogEnable( max_restarts, delay_ms );
    send_response( fd, "OK\n" );
}

static void cmd_watchdog_disable( int fd )
{
    HarnessWatchdogDisable();
    send_response( fd, "OK\n" );
}

static void cmd_watchdog_status( int fd )
{
    int enabled = 0, used = 0, remaining = 0, last_exit = -1;
    HarnessWatchdogStatus( &enabled, &used, &remaining, &last_exit );
    char buf[256];
    snprintf( buf, sizeof( buf ),
              "OK enabled=%d restarts_used=%d restarts_remaining=%d last_exit_code=%d\n",
              enabled, used, remaining, last_exit );
    send_response( fd, buf );
}

// ---------------------------------------------------------------------------
// Per-connection handler — called synchronously from the accept loop.
// Returns when the client disconnects or sends QUIT.
// ---------------------------------------------------------------------------

static void handle_client( int conn_fd )
{
    // Log the connecting peer's credentials for defense-in-depth.
    {
        struct ucred cred{};
        socklen_t cred_len = sizeof( cred );
        if ( getsockopt( conn_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len ) == 0 )
            s_harness_log.infof( "client connected: pid=%d uid=%d", cred.pid, cred.uid );
        else
            s_harness_log.warnf( "SO_PEERCRED failed: %s", strerror( errno ) );
    }

    // Active PROBE_TAIL subscription for this connection (if any).
    ProbeSubscription *p_probe = nullptr;

    // Read lines from the client into a fixed-size buffer.
    // We do not use getline() to avoid unbounded heap allocation.
    char buf[ HARNESS_MAX_LINE + 1 ];
    size_t buf_used = 0;

    while ( s_bRunning.load( std::memory_order_relaxed ) )
    {
        ssize_t n = read( conn_fd, buf + buf_used,
                          sizeof( buf ) - 1 - buf_used );
        if ( n == 0 )
        {
            s_harness_log.infof( "client disconnected" );
            break;
        }
        if ( n < 0 )
        {
            if ( errno == EINTR )
                continue;
            s_harness_log.errorf_errno( "read from client failed" );
            break;
        }

        buf_used += (size_t)n;

        // Process all complete lines in the buffer.
        char *line_start = buf;
        bool close_conn = false;

        while ( true )
        {
            char *nl = (char *)memchr( line_start, '\n',
                                       buf_used - (size_t)(line_start - buf) );
            if ( !nl )
                break;

            size_t line_len = (size_t)(nl - line_start);

            if ( line_len >= HARNESS_MAX_LINE )
            {
                send_response( conn_fd, "ERR LINE_TOO_LONG\n" );
                // Discard up to and including this newline.
            }
            else
            {
                // Null-terminate the line (overwriting '\n').
                line_start[ line_len ] = '\0';

                // Strip a trailing '\r' if present (Windows clients).
                if ( line_len > 0 && line_start[ line_len - 1 ] == '\r' )
                    line_start[ --line_len ] = '\0';

                if ( line_len == 0 )
                {
                    // Blank line — ignore.
                }
                else
                {
                    const char *args = nullptr;
                    const char *verb = split_verb( line_start, &args );

                    if ( strcmp( verb, "STATE" ) == 0 )
                    {
                        cmd_state( conn_fd );
                    }
                    else if ( strcmp( verb, "QUIT" ) == 0 )
                    {
                        send_response( conn_fd, "OK\n" );
                        close_conn = true;
                    }
                    else if ( strcmp( verb, "KEY_DOWN" ) == 0 )
                    {
                        cmd_key_down( conn_fd, args );
                    }
                    else if ( strcmp( verb, "KEY_UP" ) == 0 )
                    {
                        cmd_key_up( conn_fd, args );
                    }
                    else if ( strcmp( verb, "KEY" ) == 0 )
                    {
                        cmd_key( conn_fd, args );
                    }
                    else if ( strcmp( verb, "MOVE" ) == 0 )
                    {
                        cmd_move( conn_fd, args );
                    }
                    else if ( strcmp( verb, "CLICK" ) == 0 )
                    {
                        cmd_click( conn_fd, args );
                    }
                    else if ( strcmp( verb, "SCREENSHOT" ) == 0 )
                    {
                        cmd_screenshot( conn_fd, args );
                    }
                    else if ( strcmp( verb, "SCREENSHOT_REGION" ) == 0 )
                    {
                        cmd_screenshot_region( conn_fd, args );
                    }
                    else if ( strcmp( verb, "PROBE_TAIL" ) == 0 )
                    {
                        cmd_probe_tail( conn_fd, args, &p_probe );
                    }
                    else if ( strcmp( verb, "PROBE_UNSUB" ) == 0 )
                    {
                        cmd_probe_unsub( conn_fd, &p_probe );
                    }
                    else if ( strcmp( verb, "WATCHDOG_ENABLE" ) == 0 )
                    {
                        cmd_watchdog_enable( conn_fd, args );
                    }
                    else if ( strcmp( verb, "WATCHDOG_DISABLE" ) == 0 )
                    {
                        cmd_watchdog_disable( conn_fd );
                    }
                    else if ( strcmp( verb, "WATCHDOG_STATUS" ) == 0 )
                    {
                        cmd_watchdog_status( conn_fd );
                    }
                    else
                    {
                        char errbuf[256];
                        snprintf( errbuf, sizeof( errbuf ),
                                  "ERR UNKNOWN_COMMAND %s\n", verb );
                        send_response( conn_fd, errbuf );
                    }
                }
            }

            line_start = nl + 1;

            if ( close_conn )
                goto disconnect;
        }

        // Compact the buffer: move any partial line back to the front.
        buf_used -= (size_t)( line_start - buf );
        if ( buf_used > 0 )
            memmove( buf, line_start, buf_used );

        // If the buffer is full and there is still no newline, the client is
        // sending an overlong line without a terminator. Reject and reset.
        if ( buf_used >= sizeof( buf ) - 1 )
        {
            send_response( conn_fd, "ERR LINE_TOO_LONG\n" );
            buf_used = 0;
        }
    }

disconnect:
    // Stop any active probe tail thread before closing the fd.
    probe_fd_unregister( conn_fd );
    probe_disconnect( p_probe );
    p_probe = nullptr;
    close( conn_fd );
    s_harness_log.infof( "client fd closed" );
}

// ---------------------------------------------------------------------------
// Accept loop — runs on the aoe3de-harness thread.
// ---------------------------------------------------------------------------

static void *harness_thread_func( void * )
{
    pthread_setname_np( pthread_self(), "aoe3de-harness" );

    s_harness_log.infof( "listening on %s", s_socketPath.c_str() );

    while ( s_bRunning.load( std::memory_order_relaxed ) )
    {
        // accept4: SOCK_CLOEXEC so child processes do not inherit the fd;
        // SOCK_NONBLOCK deferred — we actually want blocking reads on the
        // client fd. Use plain accept4 with SOCK_CLOEXEC only.
        int conn_fd = accept4( s_nListenFd, nullptr, nullptr, SOCK_CLOEXEC );
        if ( conn_fd < 0 )
        {
            if ( errno == EINTR || errno == EAGAIN )
                continue;
            if ( !s_bRunning.load( std::memory_order_relaxed ) )
                break; // StopHarnessSocket closed the listen fd
            s_harness_log.errorf_errno( "accept4 failed" );
            break;
        }

        // Serial: handle one client fully before accepting the next.
        // This is sufficient for the single-AI-client use case and avoids
        // all the complexity of a multi-threaded dispatch.
        handle_client( conn_fd );
    }

    // Clean up the socket file on orderly exit.
    unlink( s_socketPath.c_str() );
    s_harness_log.infof( "thread exiting" );
    return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void StartHarnessSocket( const std::string &socketPath )
{
    if ( s_bRunning.load( std::memory_order_relaxed ) )
    {
        s_harness_log.warnf( "StartHarnessSocket called twice — ignoring" );
        return;
    }

    s_socketPath = socketPath;

    // OQ-7: unlink any stale socket file left by a previous crash.
    if ( unlink( socketPath.c_str() ) < 0 && errno != ENOENT )
        s_harness_log.warnf( "unlink(%s): %s", socketPath.c_str(), strerror( errno ) );

    int fd = socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 );
    if ( fd < 0 )
    {
        s_harness_log.errorf_errno( "socket() failed" );
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if ( socketPath.size() >= sizeof( addr.sun_path ) )
    {
        s_harness_log.errorf( "socket path too long (%zu >= %zu)",
                              socketPath.size(), sizeof( addr.sun_path ) );
        close( fd );
        return;
    }
    strncpy( addr.sun_path, socketPath.c_str(), sizeof( addr.sun_path ) - 1 );

    if ( bind( fd, (struct sockaddr *)&addr, sizeof( addr ) ) < 0 )
    {
        s_harness_log.errorf_errno( "bind(%s) failed", socketPath.c_str() );
        close( fd );
        return;
    }

    // Mode 0600 — only the owning user may connect. Defense in depth.
    chmod( socketPath.c_str(), 0600 );

    if ( listen( fd, 4 ) < 0 )
    {
        s_harness_log.errorf_errno( "listen() failed" );
        close( fd );
        unlink( socketPath.c_str() );
        return;
    }

    s_nListenFd = fd;
    s_bRunning.store( true, std::memory_order_release );

    int rc = pthread_create( &s_thread, nullptr, harness_thread_func, nullptr );
    if ( rc != 0 )
    {
        s_harness_log.errorf( "pthread_create failed: %s", strerror( rc ) );
        s_bRunning.store( false, std::memory_order_release );
        close( fd );
        s_nListenFd = -1;
        unlink( socketPath.c_str() );
        return;
    }

    // Start the watchdog poll thread (opt-in via WATCHDOG_ENABLE socket command).
    HarnessWatchdogStart();
}

void StopHarnessSocket()
{
    if ( !s_bRunning.load( std::memory_order_relaxed ) )
        return;

    s_bRunning.store( false, std::memory_order_release );

    // Stop the watchdog poll thread before closing fds.
    HarnessWatchdogStop();

    // Unblock any in-flight capture_to_path() call so the harness thread
    // is not stuck waiting on the screenshot future when we try to join it.
    signal_shutdown();

    // Closing the listen fd causes accept4() to return EBADF / error,
    // unblocking the thread.
    if ( s_nListenFd >= 0 )
    {
        close( s_nListenFd );
        s_nListenFd = -1;
    }

    pthread_join( s_thread, nullptr );
}

} // namespace gamescope::Harness
