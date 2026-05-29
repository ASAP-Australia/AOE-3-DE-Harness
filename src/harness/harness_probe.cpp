// harness_probe.cpp — PROBE_TAIL inotify log tail for the harness control socket.
//
// See harness_probe.h for the public API and threading model.

#include "harness_probe.h"
#include "../log.hpp"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static LogScope s_probe_log( "harness_probe" );

namespace gamescope::Harness
{

// ---------------------------------------------------------------------------
// Ring buffer — 512 entries, each capped at PROBE_LINE_MAX bytes.
// ---------------------------------------------------------------------------

static constexpr size_t RING_SIZE     = 512;
static constexpr size_t PROBE_LINE_MAX = 1024;

struct RingBuffer
{
    char    lines[ RING_SIZE ][ PROBE_LINE_MAX ];
    size_t  lens[ RING_SIZE ];
    size_t  head = 0; // next write slot
    size_t  tail = 0; // oldest unread slot
    size_t  count = 0;
    size_t  dropped = 0; // lines dropped due to ring full (pending report)

    void push( const char *line, size_t len )
    {
        if ( len >= PROBE_LINE_MAX )
            len = PROBE_LINE_MAX - 1;
        if ( count == RING_SIZE )
        {
            // Drop oldest to make room.
            tail = ( tail + 1 ) % RING_SIZE;
            --count;
            ++dropped;
        }
        memcpy( lines[ head ], line, len );
        lines[ head ][ len ] = '\0';
        lens[ head ] = len;
        head = ( head + 1 ) % RING_SIZE;
        ++count;
    }

    bool pop( char *out, size_t *out_len )
    {
        if ( count == 0 )
            return false;
        size_t l = lens[ tail ];
        memcpy( out, lines[ tail ], l );
        out[ l ] = '\0';
        *out_len = l;
        tail = ( tail + 1 ) % RING_SIZE;
        --count;
        return true;
    }
};

// ---------------------------------------------------------------------------
// ProbeSubscription
// ---------------------------------------------------------------------------

struct ProbeSubscription
{
    int             conn_fd   = -1;
    std::string     log_path;
    std::atomic<bool> stop   { false };
    pthread_t       thread   {};
    bool            thread_started = false;
};

// ---------------------------------------------------------------------------
// Tail thread
// ---------------------------------------------------------------------------

// Try to write buf[0..len) to fd using MSG_DONTWAIT.
// Returns true on success, false if EAGAIN (client is slow).
static bool try_send( int fd, const char *buf, size_t len )
{
    while ( len > 0 )
    {
        ssize_t n = send( fd, buf, len, MSG_DONTWAIT );
        if ( n > 0 )
        {
            buf += n;
            len -= (size_t)n;
        }
        else if ( n == 0 )
        {
            return false;
        }
        else
        {
            if ( errno == EINTR )
                continue;
            if ( errno == EAGAIN || errno == EWOULDBLOCK )
                return false; // slow client
            return false;     // connection error
        }
    }
    return true;
}

static void *probe_tail_func( void *arg )
{
    ProbeSubscription *ps = static_cast<ProbeSubscription *>( arg );
    pthread_setname_np( pthread_self(), "harness-probe" );

    // Open the log file and seek to EOF so we only see new lines.
    int log_fd = open( ps->log_path.c_str(), O_RDONLY | O_CLOEXEC );
    if ( log_fd < 0 )
    {
        char errbuf[256];
        snprintf( errbuf, sizeof( errbuf ), "ERR PROBE_OPEN_FAILED %s\n", strerror( errno ) );
        send( ps->conn_fd, errbuf, strlen( errbuf ), MSG_DONTWAIT );
        return nullptr;
    }

    // Seek to end so we only tail new content.
    lseek( log_fd, 0, SEEK_END );

    // Set up inotify.
    int ifd = inotify_init1( IN_NONBLOCK | IN_CLOEXEC );
    int iwd = -1;
    if ( ifd >= 0 )
        iwd = inotify_add_watch( ifd, ps->log_path.c_str(), IN_MODIFY );

    if ( ifd < 0 || iwd < 0 )
    {
        s_probe_log.warnf( "inotify setup failed for %s: %s",
                           ps->log_path.c_str(), strerror( errno ) );
        // Fall back to polling — still functional.
    }

    RingBuffer ring;
    char       read_buf[ 65536 ];
    char       line_buf[ PROBE_LINE_MAX ];
    size_t     line_len  = 0;
    char       send_buf[ PROBE_LINE_MAX + 16 ]; // "PROBE: " + line + "\n"

    while ( !ps->stop.load( std::memory_order_relaxed ) )
    {
        // Drain inotify events (or just wait 200 ms via poll).
        {
            struct pollfd pfd[2];
            int nfds = 0;
            if ( ifd >= 0 )
            {
                pfd[ nfds ].fd     = ifd;
                pfd[ nfds ].events = POLLIN;
                ++nfds;
            }
            // Also poll the log fd directly in case inotify isn't available.
            // (But mainly we rely on inotify + the 200 ms timeout as keepalive.)
            pfd[ nfds ].fd      = -1; // unused slot placeholder
            pfd[ nfds ].events  = 0;
            // We just need the timeout behaviour.
            poll( pfd, (nfds > 0) ? nfds : 0, 200 ); // 200 ms

            if ( ifd >= 0 )
            {
                // Drain the inotify fd — we don't care about individual events,
                // just that a modification happened.
                char ibuf[ 512 ];
                while ( read( ifd, ibuf, sizeof( ibuf ) ) > 0 )
                    ; // drain
            }
        }

        if ( ps->stop.load( std::memory_order_relaxed ) )
            break;

        // Read new data from the log file.
        ssize_t nread;
        while ( ( nread = read( log_fd, read_buf, sizeof( read_buf ) ) ) > 0 )
        {
            for ( ssize_t i = 0; i < nread; ++i )
            {
                char c = read_buf[ i ];
                if ( c == '\n' )
                {
                    // Complete line — filter on [LLP v=2].
                    line_buf[ line_len ] = '\0';
                    if ( strstr( line_buf, "[LLP v=2]" ) != nullptr )
                        ring.push( line_buf, line_len );
                    line_len = 0;
                }
                else
                {
                    if ( line_len < PROBE_LINE_MAX - 1 )
                        line_buf[ line_len++ ] = c;
                    // Lines longer than PROBE_LINE_MAX are silently truncated.
                }
            }
        }

        // Flush the ring buffer to the client.
        // Report any accumulated drops first.
        if ( ring.dropped > 0 )
        {
            int n = snprintf( send_buf, sizeof( send_buf ),
                              "PROBE: DROPPED %zu\n", ring.dropped );
            ring.dropped = 0;
            if ( n > 0 )
                try_send( ps->conn_fd, send_buf, (size_t)n );
        }

        char entry[ PROBE_LINE_MAX ];
        size_t entry_len = 0;
        while ( ring.pop( entry, &entry_len ) )
        {
            int n = snprintf( send_buf, sizeof( send_buf ),
                              "PROBE: %.*s\n", (int)entry_len, entry );
            if ( n > 0 )
            {
                if ( !try_send( ps->conn_fd, send_buf, (size_t)n ) )
                {
                    // Client is slow or disconnected; put line back by
                    // incrementing dropped and break — we'll retry next tick.
                    ring.dropped += ring.count + 1;
                    // Clear ring to avoid resending stale data.
                    ring.head = ring.tail = ring.count = 0;
                    break;
                }
            }
        }
    }

    // Cleanup.
    if ( iwd >= 0 )
        inotify_rm_watch( ifd, iwd );
    if ( ifd >= 0 )
        close( ifd );
    close( log_fd );

    return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ProbeSubscription *probe_subscribe( int conn_fd, const std::string &log_path )
{
    // Validate the log path.
    if ( log_path.empty() || log_path[0] != '/' )
    {
        const char *err = "ERR PROBE_INVALID_PATH not_absolute\n";
        send( conn_fd, err, strlen( err ), MSG_DONTWAIT );
        return nullptr;
    }
    if ( log_path.find( ".." ) != std::string::npos )
    {
        const char *err = "ERR PROBE_INVALID_PATH dotdot_not_allowed\n";
        send( conn_fd, err, strlen( err ), MSG_DONTWAIT );
        return nullptr;
    }

    // Check the file exists and is readable.
    struct stat st{};
    if ( stat( log_path.c_str(), &st ) != 0 )
    {
        char errbuf[256];
        snprintf( errbuf, sizeof( errbuf ),
                  "ERR PROBE_FILE_NOT_FOUND %s\n", strerror( errno ) );
        send( conn_fd, errbuf, strlen( errbuf ), MSG_DONTWAIT );
        return nullptr;
    }
    if ( !S_ISREG( st.st_mode ) )
    {
        const char *err = "ERR PROBE_NOT_A_FILE\n";
        send( conn_fd, err, strlen( err ), MSG_DONTWAIT );
        return nullptr;
    }

    // Build subscription.
    ProbeSubscription *ps = new ProbeSubscription();
    ps->conn_fd  = conn_fd;
    ps->log_path = log_path;

    int rc = pthread_create( &ps->thread, nullptr, probe_tail_func, ps );
    if ( rc != 0 )
    {
        char errbuf[128];
        snprintf( errbuf, sizeof( errbuf ),
                  "ERR PROBE_THREAD_FAILED %s\n", strerror( rc ) );
        send( conn_fd, errbuf, strlen( errbuf ), MSG_DONTWAIT );
        delete ps;
        return nullptr;
    }
    ps->thread_started = true;

    const char *ok = "OK SUBSCRIBED\n";
    send( conn_fd, ok, strlen( ok ), MSG_DONTWAIT );
    s_probe_log.infof( "probe subscribed: %s on fd %d", log_path.c_str(), conn_fd );
    return ps;
}

static void probe_stop_internal( ProbeSubscription *ps, bool send_eof )
{
    if ( !ps )
        return;

    ps->stop.store( true, std::memory_order_release );
    if ( ps->thread_started )
        pthread_join( ps->thread, nullptr );

    if ( send_eof )
    {
        const char *eof = "PROBE: EOF\n";
        send( ps->conn_fd, eof, strlen( eof ), MSG_DONTWAIT );
    }
}

void probe_unsubscribe( ProbeSubscription *ps )
{
    if ( !ps )
        return;
    probe_stop_internal( ps, true );
    const char *ok = "OK\n";
    send( ps->conn_fd, ok, strlen( ok ), MSG_DONTWAIT );
    s_probe_log.infof( "probe unsubscribed on fd %d", ps->conn_fd );
    delete ps;
}

void probe_disconnect( ProbeSubscription *ps )
{
    if ( !ps )
        return;
    probe_stop_internal( ps, false ); // don't send to a closing fd
    s_probe_log.infof( "probe disconnected on fd %d", ps->conn_fd );
    delete ps;
}

} // namespace gamescope::Harness
