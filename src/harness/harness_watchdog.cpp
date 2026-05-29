// harness_watchdog.cpp — crash-detection + auto-restart of the inner command.
//
// See harness_watchdog.h for the full design rationale.
//
// The poll thread wakes every POLL_INTERVAL_MS and calls kill(pid, 0).
// On process death it saves the exit code (via waitpid), decrements the
// restart budget, broadcasts WATCHDOG: RESTART <n_remaining> to PROBE_TAIL
// subscribers, waits restart_delay_ms, then re-spawns via SpawnProcess().
// When budget is exhausted it broadcasts WATCHDOG: EXHAUSTED and disarms.

#include "harness_watchdog.h"
#include "../log.hpp"
#include "../Utils/Process.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static LogScope s_wdlog( "harness_watchdog" );

// Poll interval for the watchdog thread (milliseconds).
static constexpr int POLL_INTERVAL_MS = 500;

namespace gamescope::Harness
{

// ---------------------------------------------------------------------------
// Internal state — all protected by s_mutex except s_running (atomic).
// ---------------------------------------------------------------------------

static std::mutex  s_mutex;
static pthread_t   s_thread;
static std::atomic<bool> s_running { false };

// Inner command info set by HarnessWatchdogSetInnerCommand.
static char      **s_argv         = nullptr; // NOT owned — points into main argv
static pid_t       s_pid          = -1;

// Watchdog configuration/state.
static bool        s_enabled       = false;
static int         s_max_restarts  = 0;
static int         s_restart_delay_ms = 0;
static int         s_restarts_used   = 0;
static int         s_restarts_remaining = 0;
static int         s_last_exit_code  = -1; // -1 = no exit yet

// ---------------------------------------------------------------------------
// Forward declaration of the broadcast helper (defined in harness_socket.cpp).
// ---------------------------------------------------------------------------
// HarnessWatchdogBroadcast() is declared in harness_watchdog.h and
// defined in harness_socket.cpp so it can reach the probe subscriber list.

// ---------------------------------------------------------------------------
// Poll thread
// ---------------------------------------------------------------------------

static void millisleep( int ms )
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)( ms % 1000 ) * 1000000L;
    nanosleep( &ts, nullptr );
}

static void *watchdog_thread_func( void * )
{
    pthread_setname_np( pthread_self(), "harness-watchdog" );
    s_wdlog.infof( "watchdog poll thread started" );

    while ( s_running.load( std::memory_order_relaxed ) )
    {
        millisleep( POLL_INTERVAL_MS );

        if ( !s_running.load( std::memory_order_relaxed ) )
            break;

        // Only act when enabled and we have a valid PID.
        {
            std::lock_guard<std::mutex> lk( s_mutex );
            if ( !s_enabled || s_pid <= 0 || s_argv == nullptr )
                continue;
        }

        // Check if the process is still alive.
        pid_t pid;
        {
            std::lock_guard<std::mutex> lk( s_mutex );
            pid = s_pid;
        }

        // Use waitpid(WNOHANG) as the primary death detector.  This correctly
        // harvests zombie children that kill(pid,0) would report as alive.
        int wstatus = 0;
        pid_t wr = waitpid( pid, &wstatus, WNOHANG );
        if ( wr == 0 )
            continue; // still running

        // wr < 0 (ECHILD: not our child, already reaped) or wr == pid (exited).
        // In either case the child is gone.
        int exit_code = -1;
        if ( wr == pid )
        {
            if ( WIFEXITED( wstatus ) )
                exit_code = WEXITSTATUS( wstatus );
            else if ( WIFSIGNALED( wstatus ) )
                exit_code = 128 + WTERMSIG( wstatus );
        }

        // Grab state under lock and decide whether to restart.
        std::lock_guard<std::mutex> lk( s_mutex );

        s_last_exit_code = exit_code;
        s_pid = -1; // mark as gone

        if ( !s_enabled )
        {
            s_wdlog.infof( "inner process died (exit=%d); watchdog disabled — not restarting", exit_code );
            continue;
        }

        if ( s_restarts_remaining <= 0 )
        {
            s_wdlog.infof( "inner process died (exit=%d); restart budget exhausted", exit_code );
            HarnessWatchdogBroadcast( "EXHAUSTED" );
            s_enabled = false;
            continue;
        }

        // Consume one restart slot.
        --s_restarts_remaining;
        ++s_restarts_used;

        char msg[128];
        snprintf( msg, sizeof( msg ), "RESTART %d", s_restarts_remaining );
        s_wdlog.infof( "WATCHDOG: %s (exit_code=%d)", msg, exit_code );
        HarnessWatchdogBroadcast( msg );

        // Brief delay before re-spawn (done while holding the lock to keep
        // state consistent; this is acceptable since it is short).
        if ( s_restart_delay_ms > 0 )
        {
            // Release lock during sleep so socket commands remain responsive.
            s_mutex.unlock();
            millisleep( s_restart_delay_ms );
            s_mutex.lock();
        }

        if ( !s_running.load( std::memory_order_relaxed ) )
            break;

        // Re-spawn the inner command.
        if ( s_argv != nullptr )
        {
            pid_t new_pid = gamescope::Process::SpawnProcessInWatchdog( s_argv, false );
            if ( new_pid > 0 )
            {
                s_pid = new_pid;
                s_wdlog.infof( "restarted inner command as PID %d", new_pid );
            }
            else
            {
                s_wdlog.errorf( "failed to respawn inner command" );
                HarnessWatchdogBroadcast( "SPAWN_FAILED" );
                s_enabled = false;
            }
        }
    }

    s_wdlog.infof( "watchdog poll thread exiting" );
    return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void HarnessWatchdogSetInnerCommand( char **argv, pid_t inner_pid )
{
    std::lock_guard<std::mutex> lk( s_mutex );
    s_argv = argv;
    s_pid  = inner_pid;
    s_wdlog.infof( "inner command registered: argv[0]=%s pid=%d",
                   argv ? argv[0] : "(null)", (int)inner_pid );
}

void HarnessWatchdogEnable( int max_restarts, int restart_delay_ms )
{
    std::lock_guard<std::mutex> lk( s_mutex );
    s_enabled             = true;
    s_max_restarts        = max_restarts;
    s_restart_delay_ms    = restart_delay_ms;
    s_restarts_used       = 0;
    s_restarts_remaining  = max_restarts;
    s_wdlog.infof( "watchdog enabled: max_restarts=%d delay_ms=%d",
                   max_restarts, restart_delay_ms );
}

void HarnessWatchdogDisable()
{
    std::lock_guard<std::mutex> lk( s_mutex );
    s_enabled = false;
    s_wdlog.infof( "watchdog disabled" );
}

void HarnessWatchdogStatus( int *out_enabled, int *out_used,
                             int *out_remaining, int *out_last_exit_code )
{
    std::lock_guard<std::mutex> lk( s_mutex );
    *out_enabled       = s_enabled ? 1 : 0;
    *out_used          = s_restarts_used;
    *out_remaining     = s_restarts_remaining;
    *out_last_exit_code = s_last_exit_code;
}

void HarnessWatchdogStart()
{
    if ( s_running.load( std::memory_order_relaxed ) )
        return;
    s_running.store( true, std::memory_order_release );
    int rc = pthread_create( &s_thread, nullptr, watchdog_thread_func, nullptr );
    if ( rc != 0 )
    {
        s_wdlog.errorf( "pthread_create failed: %s", strerror( rc ) );
        s_running.store( false, std::memory_order_release );
    }
}

void HarnessWatchdogStop()
{
    if ( !s_running.load( std::memory_order_relaxed ) )
        return;
    s_running.store( false, std::memory_order_release );
    pthread_join( s_thread, nullptr );
}

} // namespace gamescope::Harness
