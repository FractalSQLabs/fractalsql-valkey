#include "transport_http.h"
#include "errors.h"
#include "logging.h"
#include "policy.h"
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <fstream>
#include <mutex>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <optional>
#include <set>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <map>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <algorithm>

  #include <sys/socket.h>
  #include <sys/resource.h>
  #include <sys/time.h>
  #include <poll.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <sys/stat.h>

// Close a socket. A small inline shim rather than a macro: a macro that
// expanded to a bare `close` would be open to a member-function lookup
// shadowing it, while the call here is always unqualified and always
// resolves to the global function.
  inline void fsqlCloseSocket( int fd ) { ::close( fd ); }
  inline void fsqlHalfCloseSend( int fd ) { ::shutdown( fd, SHUT_WR ); }
  inline void fsqlSetRecvTimeoutMs( int fd, int ms )
  { timeval t{ ms / 1000, ( ms % 1000 ) * 1000 }; ::setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof( t ) ); }

// Closes after a refusal reply: half-closes, reads and discards the client's
// pending request for up to lingerMs, then closes. A plain close with unread
// data resets the connection, and the reset discards the reply that was just
// written -- the peer then sees a broken connection instead of the refusal it
// was owed.
inline void fsqlCloseAfterReply( int fd, int lingerMs )
{
    fsqlHalfCloseSend( fd );
    fsqlSetRecvTimeoutMs( fd, 100 );
    char sink[ 4096 ];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( lingerMs );
    while( std::chrono::steady_clock::now() < deadline )
    {
        const int n = static_cast<int>( ::recv( fd, sink, sizeof( sink ), 0 ) );
        if( n == 0 ) break;   // the peer closed: nothing more can arrive
        if( n < 0 )
        {
            // No data yet (or a signal landed mid-read): keep waiting for the
            // rest of the peer's request.
            if( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ) break;
        }
    }
    fsqlCloseSocket( fd );
}

// Minimal threaded HTTP/1.1 listener :
//   GET  /healthz -> 200, no auth, no DB touch
//   GET  /readyz  -> readinessProbe() ? 200 : 503, no auth
//   POST <path>    -> auth required; the body is dispatched as JSON-RPC 2.0
// One request per connection: the reply is written, then the connection is
// closed. The MCP HTTP transport is a request/response protocol, so a
// keep-alive loop bought nothing and cost a class of lifetime bugs.
//
// Two protocol eras are served on the same endpoint by default. The legacy
// era (revisions 2024-11-05 .. 2025-11-25) keeps the initialize handshake
// and the Mcp-Session-Id header; the modern era (2026-07-28) is stateless:
// every request carries its own MCP-Protocol-Version header and per-request
// metadata, and the engine state it touches is named by an explicit context
// handle. FSQL_MCP_PROTOCOL_ERA=modern serves the modern era only.
//
//   DELETE <path>  -> closes the named session's dedicated DB connection
// SIGTERM/SIGINT -> sessions drain up to cfg.shutdownDrainMs, then the
// listening socket is closed. SIGHUP -> reload the credentials ONLY, never
// shutdown. The reload itself runs on the request path, not in the signal
// handler: the handler only raises a flag, because the reload is file I/O
// plus allocation and neither is async-signal-safe.

namespace fsqlmcp
{
    // A header the server acts on that arrives twice is refused: two values for the host, the credential, the origin or the
    // framing are an ambiguity a proxy in front and this parser could resolve differently (SECURITY_AUDIT.md 3.18).
    static bool isSingleValuedHeader( const std::string& name )
    {
        static const char* const kNames[] = { "host", "authorization", "origin", "content-type", "transfer-encoding",
                                              "mcp-session-id", "mcp-protocol-version", "mcp-method", "mcp-name" };
        for( const char* n : kNames )
            if( name == n ) return true;
        return false;
    }

    static constexpr std::size_t kBodyMax = 32 * 1024 * 1024;

    // SIGHUP means "reload the credentials", and that reload is file I/O plus
    // allocation. Doing that work inside the signal handler is not
    // async-signal-safe: it can deadlock against the malloc arena lock the
    // interrupted thread already held (observed as a TSan-reported hang
    // under a fully thread-instrumented build), and it can corrupt the
    // TokenStore's own mutex if the interrupted thread was inside reload().
    //
    // So the handler only sets a flag. The reload itself happens on the
    // ordinary request path (check() -> reload()), which is not signal
    // context and is free to block on I/O.
    static std::atomic<bool> g_sighupReload{ false };

    // Failed-auth backoff: each failure stalls its own connection thread for a growing
    // interval (capped at 2 s), which throttles online token guessing without a lockout
    // an attacker could turn against legitimate clients. A success pays one failure back.
    static std::atomic<int>& authFailureCounter() { static std::atomic<int> n{ 0 }; return n; }
    static void authFailed()
    {
        const int fails = std::min( authFailureCounter().fetch_add( 1 ) + 1, 20 );
        std::this_thread::sleep_for( std::chrono::milliseconds( fails * 100 ) );
    }
    static void authOk()
    {
        int f = authFailureCounter().load();
        while( f > 0 && !authFailureCounter().compare_exchange_weak( f, f - 1 ) ) {}
    }

    // The modification time of a file, as a comparable number. It is what
    // decides whether the token file or the credential policy file has to be
    // read again.
    static bool fileMtime( const std::string& path, unsigned long long& out )
    {
        struct stat st{};
        if( ::stat( path.c_str(), &st ) != 0 ) return false;
        out = static_cast<unsigned long long>( st.st_mtime );
        return true;
    }

    // The credentials this server accepts.
    //
    // One deployment token (a plain value, or a file that is re-read when it
    // changes), and optionally a policy file of per-client tokens. A policy
    // entry may name its client, say when it expires, and limit the client
    // to a subset of the tools:
    //
    //   { "tokens": [ { "sha256": "<64 hex>", "tools": "*" | ["a","b"],
    //                   "name": "<label>", "expires": "YYYY-MM-DD[THH:MM:SSZ]" } ] }
    //
    // "name" and "expires" are optional; "sha256" and "tools" are what a
    // policy file has always carried. The deployment token keeps every tool;
    // a policy entry gets exactly the tools it names.
    class TokenStore
    {
    public:
        // What a presented credential turned out to be.
        struct Credential
        {
            bool ok = false;
            bool expired = false;
            std::string clientName;                        // the policy entry's name, or a token-derived key
            std::optional<std::set<std::string>> allow;    // unset = every tool
        };

        // The config is held BY VALUE: a connection thread can outlive
        // runHttpTransport() (the shutdown path detaches whatever is still
        // in flight), and a reference to the caller's config would dangle
        // by then.
        explicit TokenStore( const HttpConfig& cfg ) : cfg_( cfg ) { reload( true ); }

        Credential check( const std::string& presented )
        {
            // One lock only: the reload reads the credential state, so it
            // runs under the same guard as the compare. A same-thread second
            // lock on this non-recursive mutex deadlocks
            // forever on glibc.
            std::lock_guard<std::mutex> lk( mu_ );
            const bool forced = g_sighupReload.exchange( false, std::memory_order_relaxed );
            if( forced ) reload( true );
            else
            {
                if( !cfg_.authTokenFile.empty() ) reloadToken( false );
                reloadPolicy( false );
            }
            if( presented.empty() ) return Credential{};

            // The deployment token: a timing-safe compare, so a wrong token
            // costs the same time as a right one.
            if( !token_.empty() && presented.size() == token_.size() &&
                CRYPTO_memcmp( presented.data(), token_.data(), token_.size() ) == 0 )
            {
                Credential c;
                c.ok = true;
                // The identity of a deployment-token holder: a short digest
                // of the token, never the token itself.
                c.clientName = "token:" + sha256Hex( presented ).substr( 0, 8 );
                return c;
            }

            if( policies_.empty() ) return Credential{};

            // Per-client credentials: the presented token's SHA-256 against
            // every entry. No early exit: the work per attempt stays the
            // same whether or not the hash matches.
            const std::string hex = sha256Hex( presented );
            Credential out;
            bool known = false;
            for( const PolicyEntry& e : policies_ )
            {
                if( e.sha256.size() != hex.size() ) continue;
                if( CRYPTO_memcmp( e.sha256.data(), hex.data(), hex.size() ) != 0 ) continue;
                known = true;
                if( e.expired() ) { out.expired = true; continue; }
                out.ok = true;
                out.clientName = e.name.empty() ? ( "client:" + hex.substr( 0, 8 ) ) : e.name;
                out.allow = e.allTools ? std::nullopt
                                       : std::optional<std::set<std::string>>( e.tools );
            }
            if( !known ) return Credential{};
            return out;
        }

        static std::string sha256Hex( const std::string& v )
        {
            unsigned char d[ SHA256_DIGEST_LENGTH ];
            SHA256( reinterpret_cast<const unsigned char*>( v.data() ), v.size(), d );
            static const char* const kHex = "0123456789abcdef";
            std::string out;
            for( unsigned char c : d ) { out.push_back( kHex[ c >> 4 ] ); out.push_back( kHex[ c & 0xF ] ); }
            return out;
        }

        // A malformed policy file keeps the last good one and says so; the
        // caller surfaces the message at startup, where it is a fatal
        // configuration error rather than a running server without
        // authentication.
        std::string policyError()
        {
            std::lock_guard<std::mutex> lk( mu_ );
            return policyError_;
        }

    private:
        struct PolicyEntry
        {
            std::string name;
            std::string sha256;
            bool allTools = false;
            std::set<std::string> tools;
            std::time_t expiresAt = 0;      // 0 = never expires
            bool expired() const
            {
                if( expiresAt == 0 ) return false;
                return std::time( nullptr ) >= expiresAt;
            }
        };

        // Parses an ISO-8601 expiry: "YYYY-MM-DD" or
        // "YYYY-MM-DDTHH:MM:SSZ". Returns 0 when the text is not a usable
        // expiry, which the caller treats as an error.
        static std::time_t parseExpiry( const std::string& text )
        {
            if( text.size() < 10 ) return 0;
            std::tm tm{};
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
            if( std::sscanf( text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se ) < 3 &&
                std::sscanf( text.c_str(), "%d-%d-%d", &y, &mo, &d ) < 3 )
                return 0;
            if( y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 ) return 0;
            tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
            tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
            return utcToTimeT( tm );
        }

        // UTC calendar time -> time_t. timegm() is POSIX.
        static std::time_t utcToTimeT( std::tm& tm )
        { return timegm( &tm ); }

        void loadPolicyLocked()
        {
            if( cfg_.tokenPolicyFile.empty() ) return;
            std::ifstream in( cfg_.tokenPolicyFile, std::ios::binary );
            if( !in ) { policyError_ = "cannot read " + cfg_.tokenPolicyFile; return; }
            const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            std::vector<PolicyEntry> next;
            try
            {
                const Json doc = Json::parse( text );
                if( !doc.is_object() || !doc.contains( "tokens" ) || !doc.at( "tokens" ).is_array() )
                    throw std::runtime_error( "expected {\"tokens\":[...]}" );
                for( const Json& t : doc.at( "tokens" ) )
                {
                    PolicyEntry e;
                    if( t.contains( "name" ) && t.at( "name" ).is_string() ) e.name = t.at( "name" ).get<std::string>();
                    e.sha256 = t.at( "sha256" ).get<std::string>();
                    if( e.sha256.size() != 64 ) throw std::runtime_error( "sha256 must be 64 hex digits" );
                    for( char& c : e.sha256 ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
                    const Json& tools = t.at( "tools" );
                    if( tools.is_string() && tools.get<std::string>() == "*" ) e.allTools = true;
                    else if( tools.is_array() )
                        for( const Json& n : tools ) e.tools.insert( n.get<std::string>() );
                    else throw std::runtime_error( "tools must be \"*\" or an array of tool names" );
                    if( t.contains( "expires" ) && t.at( "expires" ).is_string() )
                    {
                        e.expiresAt = parseExpiry( t.at( "expires" ).get<std::string>() );
                        if( e.expiresAt == 0 )
                            throw std::runtime_error( "expires must be an ISO-8601 date/time (YYYY-MM-DD or YYYY-MM-DDTHH:MM:SSZ)" );
                    }
                    next.push_back( std::move( e ) );
                }
            }
            catch( const std::exception& ex )
            {
                policyError_ = std::string( "FSQL_MCP_TOKEN_POLICY_FILE is not valid: " ) + ex.what();
                return;   // keep the last good policy
            }
            policies_ = std::move( next );
            policyError_.clear();
        }

        // reload() runs with mu_ held.
        void reload( bool force )
        {
            reloadToken( force );
            reloadPolicy( force );
        }

        // The deployment token: a plain value, or a file that is read again
        // whenever it changes.
        void reloadToken( bool force )
        {
            if( cfg_.authTokenFile.empty() ) { token_ = cfg_.authToken; return; }   // no hot-reload
            unsigned long long mt = 0;
            if( !fileMtime( cfg_.authTokenFile, mt ) ) return;   // the file vanished: keep serving the last known token
            if( !force && mt == tokenMtime_ ) return;
            std::ifstream in( cfg_.authTokenFile, std::ios::binary );
            if( !in ) return;
            std::string t;
            std::getline( in, t );
            while( !t.empty() && ( t.back() == '\r' || t.back() == '\n' ) ) t.pop_back();
            // An unreadable or empty file never silently disables
            // authentication: the last known token keeps working.
            if( t.empty() ) return;
            token_ = t;
            tokenMtime_ = mt;
        }

        // The credential policy file: re-read when it changes, like the token
        // file. A file that cannot be read is a startup failure (the caller
        // exits non-zero); later it keeps the last good policy.
        void reloadPolicy( bool force )
        {
            if( cfg_.tokenPolicyFile.empty() ) return;
            unsigned long long mt = 0;
            if( fileMtime( cfg_.tokenPolicyFile, mt ) )
            {
                if( !force && mt == policyMtime_ ) return;
                loadPolicyLocked();
                policyMtime_ = mt;
                policyLoaded_ = true;
                return;
            }
            if( force || !policyLoaded_ ) { loadPolicyLocked(); policyLoaded_ = true; }
        }

        HttpConfig cfg_;
        std::mutex mu_;
        std::string token_;
        unsigned long long tokenMtime_ = 0;
        unsigned long long policyMtime_ = 0;
        std::vector<PolicyEntry> policies_;
        std::string policyError_;
        bool policyLoaded_ = false;
    };

    // SIGHUP means "reload the credentials", and that reload is file I/O plus
    // allocation. Doing that work inside the signal handler is not
    // async-signal-safe: it can deadlock against the malloc arena lock the
    // interrupted thread already held (observed as a TSan-reported hang
    // under a fully thread-instrumented build), and it can corrupt the
    // TokenStore's own mutex if the interrupted thread was inside reload().
    //
    // So the handler only sets a flag. The reload itself happens on the
    // ordinary request path (check() -> reload()), which is not signal
    // context and is free to block on I/O.
    static std::atomic<bool> g_httpSignalled{ false };
    static void onHttpTermSignal( int ) { g_httpSignalled.store( true ); }

    // Most connections served at once. Each one holds a thread and a
    // descriptor before it has authenticated, so the count is bounded:
    // FSQL_MCP_MAX_CONNECTIONS overrides the default of 256, and on POSIX
    // the result never exceeds half the process's descriptor limit.
    static std::size_t maxConnections()
    {
        long n = 256;
        if( const char* v = std::getenv( "FSQL_MCP_MAX_CONNECTIONS" ) )
        {
            const long x = std::strtol( v, nullptr, 10 );
            if( x > 0 ) n = x;
        }
        rlimit rl{};
        if( getrlimit( RLIMIT_NOFILE, &rl ) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 8 )
            n = std::min<long>( n, static_cast<long>( rl.rlim_cur / 2 ) );
        return static_cast<std::size_t>( n );
    }

    // A refused connection is closed only after the peer's request has been
    // read and discarded, so the refusal itself is not lost. The window is
    // long enough for a client that is slow to write its request, and short
    // enough that a refused connection does not hold anything for long.
    static constexpr int kRefusalLingerMs = 1000;

    // How many refusals may be delivering themselves at once. A flood of
    // connections past the limit must not turn into a flood of threads.
    static constexpr int kMaxRefusalsInFlight = 64;

    // Bounds how long a read or write on this socket may block.
    static void setSocketTimeoutMs( int fd, int ms )
    {
        timeval tv{ ms / 1000, ( ms % 1000 ) * 1000 };
        setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
        setsockopt( fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
    }

    // Waits up to `ms` for a pending connection. A handler installed with
    // std::signal restarts a blocked accept(), so the accept loop waits here
    // in short slices and re-checks g_httpSignalled between them.
    static bool waitForConnection( int listenFd, int ms )
    {
        pollfd p{ listenFd, POLLIN, 0 };
        return ::poll( &p, 1, ms ) > 0;
    }
#ifdef SIGHUP
    static void onSighup( int ) { g_sighupReload.store( true, std::memory_order_relaxed ); }
#endif

    // ---- the per-client registry -------------------------------------
    //
    // Three kinds of entry live here, and every one of them is a
    // JsonRpcServer built over its own dedicated engine connection:
    //
    //  - a protocol session: the legacy era's Mcp-Session-Id;
    //  - an engine context: the modern era's explicit context_id handle,
    //    which names the state a stateful call works on;
    //  - a credential connection: the modern era's connection for the calls
    //    that keep no state of their own.
    //
    // Every entry belongs to the credential that created it: another token
    // is told the handle does not exist, so a client can neither use nor
    // probe for a handle it does not own. The entries expire when idle
    // (FSQL_MCP_SESSION_TTL_SECS, default 3600) and their number is capped
    // (FSQL_MCP_MAX_SESSIONS, default 64), because each one pins an engine
    // connection.
    class ContextRegistry
    {
    public:
        explicit ContextRegistry( const HttpConfig& cfg ) : cfg_( cfg ) {}

        struct Handle
        {
            std::string id;
            std::shared_ptr<JsonRpcServer> server;
        };

        // Creates a handle with its own dedicated engine connection. Throws
        // through with whatever the factory raised (e.g. Connectivity when
        // the engine refuses a new connection).
        Handle create( const std::string& owner )
        {
            evictIdle();
            {
                std::lock_guard<std::mutex> lk( mu_ );
                if( entries_.size() >= maxEntries() )
                    throw ToolError( ErrorClass::Connectivity, "too many open MCP sessions" );
            }
            std::unique_ptr<JsonRpcServer> srv = cfg_.sessionFactory();
            const std::string id = mintHandleId();
            std::shared_ptr<JsonRpcServer> server;
            {
                std::lock_guard<std::mutex> lk( mu_ );
                Entry e;
                e.server = makeServer( std::move( srv ) );
                e.ownerFp = owner;
                e.lastUse = std::chrono::steady_clock::now();
                server = e.server;
                entries_[ id ] = std::move( e );
            }
            return Handle{ id, server };
        }

        // The server a handle names, when the caller owns it. A SHARED
        // reference, not a raw pointer: a handle removed on another
        // connection thread must not pull the server out from under a
        // request that is still using it. The shared reference keeps the
        // server alive until the in-flight request is done with it; the
        // engine connection (held by statePin) closes on the last release.
        std::shared_ptr<JsonRpcServer> find( const std::string& id, const std::string& owner )
        {
            std::lock_guard<std::mutex> lk( mu_ );
            auto it = entries_.find( id );
            if( it == entries_.end() ) return nullptr;
            if( it->second.ownerFp != owner ) return nullptr;
            it->second.lastUse = std::chrono::steady_clock::now();
            return it->second.server;
        }

        // The connection a credential's stateless calls run on: one per
        // credential, created on first use and kept until it goes idle.
        std::shared_ptr<JsonRpcServer> credentialServer( const std::string& credKey, const std::string& owner )
        {
            const std::string id = "cred:" + credKey;
            {
                std::lock_guard<std::mutex> lk( mu_ );
                auto it = entries_.find( id );
                if( it != entries_.end() && it->second.ownerFp == owner )
                {
                    it->second.lastUse = std::chrono::steady_clock::now();
                    return it->second.server;
                }
            }
            std::unique_ptr<JsonRpcServer> srv = cfg_.sessionFactory();
            std::shared_ptr<JsonRpcServer> server;
            {
                std::lock_guard<std::mutex> lk( mu_ );
                Entry e;
                e.server = makeServer( std::move( srv ) );
                e.ownerFp = owner;
                e.lastUse = std::chrono::steady_clock::now();
                server = e.server;
                entries_[ id ] = std::move( e );
            }
            return server;
        }

        // Removes a handle and closes its engine connection.
        bool erase( const std::string& id, const std::string& owner )
        {
            std::shared_ptr<JsonRpcServer> dead;
            {
                std::lock_guard<std::mutex> lk( mu_ );
                auto it = entries_.find( id );
                if( it == entries_.end() ) return false;
                if( it->second.ownerFp != owner ) return false;
                dead = it->second.server;
                entries_.erase( it );
            }
            dead.reset();    // the connection (captured in its handlers) closes here
            return true;
        }

        static std::size_t maxEntries()
        {
            const char* v = std::getenv( "FSQL_MCP_MAX_SESSIONS" );
            const long n = ( v && *v ) ? std::atol( v ) : 64;
            return n > 0 ? static_cast<std::size_t>( n ) : 64;
        }
        static std::chrono::seconds entryTtl()
        {
            const char* v = std::getenv( "FSQL_MCP_SESSION_TTL_SECS" );
            const long n = ( v && *v ) ? std::atol( v ) : 3600;
            return std::chrono::seconds( n > 0 ? n : 3600 );
        }

        void evictIdle()
        {
            std::vector<std::shared_ptr<JsonRpcServer>> dead;   // destroyed outside the lock
            {
                std::lock_guard<std::mutex> lk( mu_ );
                const auto now = std::chrono::steady_clock::now();
                for( auto it = entries_.begin(); it != entries_.end(); )
                {
                    if( now - it->second.lastUse > entryTtl() )
                    {
                        dead.push_back( it->second.server );
                        it = entries_.erase( it );
                    }
                    else ++it;
                }
            }
        }

        void endAll()
        {
            decltype( entries_ ) dead;
            {
                std::lock_guard<std::mutex> lk( mu_ );
                dead = std::move( entries_ );
                entries_.clear();
            }
            dead.clear();
        }

    private:
        struct Entry
        {
            std::shared_ptr<JsonRpcServer> server;
            std::string ownerFp;      // the fingerprint of the creating credential
            std::chrono::steady_clock::time_point lastUse;
        };

        // A handle: 128 bits of randomness, hex.
        static std::string mintHandleId()
        {
            unsigned char rnd[ 16 ] = {};
            if( RAND_bytes( rnd, sizeof( rnd ) ) != 1 )
            {
                RAND_poll();
                ( void ) RAND_bytes( rnd, sizeof( rnd ) );
            }
            static const char* const kHex = "0123456789abcdef";
            std::string out;
            for( unsigned char c : rnd )
            {
                out.push_back( kHex[ c >> 4 ] );
                out.push_back( kHex[ c & 0xF ] );
            }
            return out;
        }

        // The teardown callback is tied to the SERVER's own destruction, not
        // to the removal of the map slot: that is the only ordering that
        // holds when an in-flight request still holds the server alive, and
        // it is the order the config documents ("after its server is
        // destroyed"). The callback is copied into the deleter so the
        // deleter does not depend on this registry's lifetime.
        std::shared_ptr<JsonRpcServer> makeServer( std::unique_ptr<JsonRpcServer> srv )
        {
            auto teardown = cfg_.sessionTeardown;
            return std::shared_ptr<JsonRpcServer>( srv.release(),
                [ teardown ]( JsonRpcServer* p )
                {
                    delete p;
                    if( teardown ) teardown();
                } );
        }

        HttpConfig cfg_;
        std::mutex mu_;
        std::map<std::string, Entry> entries_;
    };

    // Non-reversible fingerprint of the presented bearer token (never log or return it).
    static std::string tokenFingerprint( const std::string& bearer )
    {
        unsigned char d[ SHA256_DIGEST_LENGTH ];
        SHA256( reinterpret_cast<const unsigned char*>( bearer.data() ), bearer.size(), d );
        static const char* const kHex = "0123456789abcdef";
        std::string out;
        for( int i = 0; i < 16; ++i ) { out.push_back( kHex[ d[ i ] >> 4 ] ); out.push_back( kHex[ d[ i ] & 0xF ] ); }
        return out;
    }

    static std::string clientKeyFor( const std::string& bearer, const std::string& sessionId )
    {
        // Session id when the deployment uses the file-token form, a raw
        // token prefix would collapse per-client fairness to one bucket when
        // the whole team shares one static token.
        return sessionId.empty() ? ( "token:" + bearer.substr( 0, 8 ) ) : ( "session:" + sessionId );
    }

    // ---- wire abstraction: plain socket or OpenSSL TLS BIO ----
    struct Wire
    {
        virtual ~Wire() = default;
        virtual int sendAll( const char* data, std::size_t len ) = 0;
        virtual int recvSome( char* buf, std::size_t max ) = 0;
        virtual void closeWire() = 0;
        virtual void setTimeoutMs( int ms ) = 0;
    };
    using WirePtr = std::unique_ptr<Wire>;

    class PlainWire : public Wire
    {
    public:
        explicit PlainWire( int fd ) : fd_( fd ) {}
        int sendAll( const char* data, std::size_t len ) override
        {
            std::size_t off = 0;
            while( off < len )
            {
                const int n = ::send( fd_, data + off, static_cast<int>( len - off ), 0 );
                if( n <= 0 ) return -1;
                off += static_cast<std::size_t>( n );
            }
            return 0;
        }
        int recvSome( char* buf, std::size_t max ) override
        {
            return static_cast<int>( ::recv( fd_, buf, static_cast<int>( max ), 0 ) );
        }
        void closeWire() override { if( fd_ >= 0 ) { fsqlCloseSocket( fd_ ); fd_ = -1; } }
        void setTimeoutMs( int ms ) override { if( fd_ >= 0 ) setSocketTimeoutMs( fd_, ms ); }
    private:
        int fd_ = -1;
    };

    class TlsWire : public Wire
    {
    public:
        TlsWire( int fd, SSL_CTX* ctx ) : ssl_( SSL_new( ctx ) ), fd_( fd )
        {
            if( ssl_ ) SSL_set_fd( ssl_, fd );
        }
        bool ok() const { return ssl_ != nullptr; }
        bool accept() { return SSL_accept( ssl_ ) == 1; }
        int sendAll( const char* data, std::size_t len ) override
        {
            std::size_t off = 0;
            while( off < len )
            {
                const int n = SSL_write( ssl_, data + off, static_cast<int>( len - off ) );
                if( n <= 0 ) return -1;
                off += static_cast<std::size_t>( n );
            }
            return 0;
        }
        int recvSome( char* buf, std::size_t max ) override
        {
            return SSL_read( ssl_, buf, static_cast<int>( max ) );
        }
        void closeWire() override { if( ssl_ ) { SSL_shutdown( ssl_ ); SSL_free( ssl_ ); ssl_ = nullptr; } }
        void setTimeoutMs( int ms ) override { setSocketTimeoutMs( fd_, ms ); }
    private:
        SSL* ssl_ = nullptr;
        int fd_ = -1;
    };

    struct HttpRequest
    {
        std::string method, target, bearer, sessionId;
        std::string path;       // the target without its query string
        std::string body;
        bool keepAlive = true;
        // The framing of the body, taken from the headers. The body
        // itself is read separately, after the request is authenticated.
        std::size_t contentLength = 0;
        bool chunked = false;
        bool malformed = false;    // framing the server refuses; answered with 400
        // Every header, with its name in lower case. The headers that steer
        // the protocol (the origin, the modern era's protocol version,
        // method and name) are read from here.
        std::map<std::string, std::string> headers;
    };

    static std::string lowerCopy( std::string s )
    {
        for( char& c : s ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        return s;
    }

    static std::string trimCopy( const std::string& s )
    {
        const std::size_t b = s.find_first_not_of( " \t\r\n" );
        if( b == std::string::npos ) return std::string();
        const std::size_t e = s.find_last_not_of( " \t\r\n" );
        return s.substr( b, e - b + 1 );
    }

    // The path of a request target: everything before the query string. A
    // credential in the query string is not a credential at all, so the
    // query string takes no part in the routing either.
    static std::string targetPath( const std::string& target )
    {
        const std::size_t q = target.find( '?' );
        return q == std::string::npos ? target : target.substr( 0, q );
    }

    static std::string headerValue( const HttpRequest& req, const std::string& name )
    {
        auto it = req.headers.find( name );
        return it == req.headers.end() ? std::string() : trimCopy( it->second );
    }

    // True for an IPv4 address in 127.0.0.0/8, parsed as an address: a name that merely starts with "127." (127.evil.example,
    // 127.0.0.1.nip.io) is not one (SECURITY_AUDIT.md 3.17).
    static bool isLoopbackV4( const std::string& host )
    {
        in_addr addr{};
        return inet_pton( AF_INET, host.c_str(), &addr ) == 1 && ( ntohl( addr.s_addr ) >> 24 ) == 127;
    }

    // True when the origin names a loopback host. The host part is what
    // matters: a page served from http://127.0.0.1:3000 is a loopback
    // origin whatever its port.
    static bool originIsLoopback( const std::string& origin )
    {
        const std::size_t sep = origin.find( "://" );
        if( sep == std::string::npos ) return false;
        std::string host = origin.substr( sep + 3 );
        const std::size_t slash = host.find( '/' );
        if( slash != std::string::npos ) host = host.substr( 0, slash );
        if( !host.empty() && host.front() == '[' )
        {
            const std::size_t close = host.find( ']' );
            if( close == std::string::npos ) return false;
            return host.substr( 1, close - 1 ) == "::1";
        }
        const std::size_t colon = host.rfind( ':' );
        if( colon != std::string::npos ) host = host.substr( 0, colon );
        if( host == "localhost" ) return true;
        if( isLoopbackV4( host ) ) return true;
        return false;
    }

    // An Origin header, when present, must be one this deployment allows: a
    // loopback origin by default, plus whatever FSQL_MCP_ALLOWED_ORIGINS
    // lists (browser tools such as the MCP Inspector need their origin
    // there). A request with no Origin header is not a browser request and
    // is allowed through.
    static bool originAllowed( const std::string& origin, const HttpConfig& cfg )
    {
        if( origin.empty() ) return true;
        if( originIsLoopback( origin ) ) return true;
        const std::string o = lowerCopy( trimCopy( origin ) );
        for( const std::string& allowed : cfg.allowedOrigins )
            if( lowerCopy( trimCopy( allowed ) ) == o ) return true;
        return false;
    }

    // base64 decode, for the Mcp-Name header's "=?base64?...?=" form.
    static std::string base64Decode( const std::string& in )
    {
        if( in.empty() ) return std::string();
        // The decoder counts the padding characters as decoded bytes, so the
        // value's length is the decoded length less the padding.
        std::size_t pad = 0;
        while( pad < in.size() && in[ in.size() - 1 - pad ] == '=' ) ++pad;
        std::string out( ( in.size() / 4 + 1 ) * 3 + 3, '\0' );
        const int n = EVP_DecodeBlock( reinterpret_cast<unsigned char*>( &out[ 0 ] ),
                                       reinterpret_cast<const unsigned char*>( in.data() ),
                                       static_cast<int>( in.size() ) );
        if( n < 0 ) return std::string();
        std::size_t len = static_cast<std::size_t>( n );
        if( len >= pad ) len -= pad;   // the padding is not part of the value
        out.resize( len );
        return out;
    }

    // The Mcp-Name header may carry the name in the base64 form the spec
    // allows. Decoded, it is the same string; a value that is not in that
    // form is taken literally.
    static std::string decodeMcpName( const std::string& raw )
    {
        static const std::string kPrefix = "=?base64?";
        const std::string v = trimCopy( raw );
        if( v.rfind( kPrefix, 0 ) != 0 ) return v;
        const std::size_t end = v.rfind( "?=" );
        if( end == std::string::npos || end < kPrefix.size() ) return std::string();
        std::string b64 = v.substr( kPrefix.size(), end - kPrefix.size() );
        for( char& c : b64 ) { if( c == '-' ) c = '+'; else if( c == '_' ) c = '/'; }
        while( b64.size() % 4 != 0 ) b64.push_back( '=' );
        return base64Decode( b64 );
    }

    // The modern era's per-request protocol version, carried in the body's
    // _meta. The spec names it
    // _meta.io.modelcontextprotocol/protocolVersion; the nested spelling is
    // accepted too, because a client that builds _meta as a tree rather
    // than as a namespaced key produces that.
    static std::string modernMetaVersion( const Json& body )
    {
        if( !body.is_object() ) return std::string();
        auto meta = body.find( "_meta" );
        if( meta == body.end() || !meta->is_object() ) return std::string();
        auto it = meta->find( "io.modelcontextprotocol/protocolVersion" );
        if( it != meta->end() && it->is_string() ) return it->get<std::string>();
        auto ns = meta->find( "io.modelcontextprotocol" );
        if( ns != meta->end() && ns->is_object() )
        {
            auto v = ns->find( "protocolVersion" );
            if( v != ns->end() && v->is_string() ) return v->get<std::string>();
        }
        return std::string();
    }

    // A string field of the request's params, or "" when it is absent or
    // not a string. Never throws on a wrongly typed params object.
    static std::string paramsString( const Json& body, const std::string& key )
    {
        auto p = body.find( "params" );
        if( p == body.end() || !p->is_object() ) return std::string();
        auto v = p->find( key );
        if( v == p->end() || !v->is_string() ) return std::string();
        return v->get<std::string>();
    }

    // A string field of the request's tool arguments, or "" when it is
    // absent or not a string.
    static std::string paramsArgumentString( const Json& body, const std::string& key )
    {
        auto p = body.find( "params" );
        if( p == body.end() || !p->is_object() ) return std::string();
        auto a = p->find( "arguments" );
        if( a == p->end() || !a->is_object() ) return std::string();
        auto v = a->find( key );
        if( v == a->end() || !v->is_string() ) return std::string();
        return v->get<std::string>();
    }

    // The revisions the modern protocol era covers. A modern request must
    // name one of them.
    static const std::vector<std::string>& modernProtocolVersions()
    {
        static const std::vector<std::string> v{ "2025-11-25", "2026-07-28" };
        return v;
    }

    // The JSON-RPC methods this server answers. A modern request naming
    // anything else is answered with 404 and -32601; a legacy request keeps
    // the legacy behaviour (the dispatch core answers -32601).
    static bool isKnownRpcMethod( const std::string& m )
    {
        static const char* const kMethods[] = {
            "initialize", "server/discover", "ping", "tools/list", "tools/call",
            "resources/list", "resources/read", "resources/templates/list",
            "prompts/list", "prompts/get", "notifications/initialized",
            "notifications/cancelled", "$/cancel"
        };
        for( const char* k : kMethods ) if( m == k ) return true;
        return false;
    }

    // The outcome of validating a modern-era request's headers against its
    // body.
    struct ModernCheck
    {
        bool ok = true;
        int httpStatus = 200;
        int code = 0;                 // the JSON-RPC error code, 0 = no error
        std::string message;
        Json data = Json();           // the error's data, when it carries any
    };

    // A modern protocol era request must carry MCP-Protocol-Version,
    // Mcp-Method and, where the method addresses something by name,
    // Mcp-Name; and the headers must agree with the body. A disagreement
    // is -32020 (HeaderMismatch), an unsupported revision is -32022
    // (UnsupportedProtocolVersionError).
    static ModernCheck validateModernRequest( const HttpRequest& req, const Json& body,
                                               const std::string& bodyMethod )
    {
        ModernCheck out;

        auto pv = req.headers.find( "mcp-protocol-version" );
        if( pv == req.headers.end() )
        {
            out.ok = false; out.httpStatus = 400; out.code = -32020;
            out.message = "missing MCP-Protocol-Version header: a modern protocol era request "
                          "must name the revision it speaks";
            return out;
        }
        const std::string headerVersion = trimCopy( pv->second );
        const auto& versions = modernProtocolVersions();
        if( std::find( versions.begin(), versions.end(), headerVersion ) == versions.end() )
        {
            out.ok = false; out.httpStatus = 400; out.code = -32022;
            out.message = "unsupported protocol version: " + headerVersion;
            Json supported = Json::array();
            for( const std::string& v : versions ) supported.push_back( v );
            out.data = Json{ { "supported", supported }, { "requested", headerVersion } };
            return out;
        }

        const std::string bodyVersion = modernMetaVersion( body );
        if( bodyVersion.empty() )
        {
            out.ok = false; out.httpStatus = 400; out.code = -32020;
            out.message = "missing _meta.io.modelcontextprotocol/protocolVersion: a modern protocol "
                          "era request carries its revision in the body as well as in the header";
            return out;
        }
        if( bodyVersion != headerVersion )
        {
            out.ok = false; out.httpStatus = 400; out.code = -32020;
            out.message = "header and body disagree about the protocol version: header says "
                          + headerVersion + ", body says " + bodyVersion;
            return out;
        }

        auto mm = req.headers.find( "mcp-method" );
        if( mm == req.headers.end() )
        {
            out.ok = false; out.httpStatus = 400; out.code = -32020;
            out.message = "missing Mcp-Method header: a modern protocol era request must name its method";
            return out;
        }
        if( trimCopy( mm->second ) != bodyMethod )
        {
            out.ok = false; out.httpStatus = 400; out.code = -32020;
            out.message = "header and body disagree about the method: header says '"
                          + trimCopy( mm->second ) + "', body says '" + bodyMethod + "'";
            return out;
        }

        // Mcp-Name, for the methods that address something by name.
        std::string expectedName;
        bool nameRequired = false;
        if( bodyMethod == "tools/call" || bodyMethod == "prompts/get" )
        {
            expectedName = paramsString( body, "name" );
            nameRequired = true;
        }
        else if( bodyMethod == "resources/read" )
        {
            expectedName = paramsString( body, "uri" );
            nameRequired = true;
        }
        if( nameRequired )
        {
            auto mn = req.headers.find( "mcp-name" );
            if( mn == req.headers.end() )
            {
                out.ok = false; out.httpStatus = 400; out.code = -32020;
                out.message = "missing Mcp-Name header: " + bodyMethod + " must name what it addresses";
                return out;
            }
            const std::string headerName = decodeMcpName( mn->second );
            if( headerName != expectedName )
            {
                out.ok = false; out.httpStatus = 400; out.code = -32020;
                out.message = "header and body disagree about the name: header says '" + headerName
                              + "', body says '" + expectedName + "'";
                return out;
            }
        }
        return out;
    }

    // Resolves the configured bind address into addr. Accepts a literal
    // IPv4 dotted quad, the two loopback spellings, and the empty string
    // (which means "the loopback interface" -- the documented default).
    // Anything else is refused rather than silently widened to INADDR_ANY:
    // a bind address an operator mistyped must fail loudly at startup, not
    // quietly listen on every interface.
    static bool resolveBindAddr( const std::string& host, sockaddr_in& addr )
    {
        std::string h = host;
        // Trim surrounding whitespace: a value arriving from a config file
        // or a shell variable routinely carries a stray newline.
        while( !h.empty() && ( h.front() == ' ' || h.front() == '\t' || h.front() == '\r' || h.front() == '\n' ) )
            h.erase( h.begin() );
        while( !h.empty() && ( h.back() == ' ' || h.back() == '\t' || h.back() == '\r' || h.back() == '\n' ) )
            h.pop_back();
        if( h.empty() || h == "localhost" || h == "127.0.0.1" )
        {
            addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
            return true;
        }
        if( h == "*" || h == "0.0.0.0" )
        {
            addr.sin_addr.s_addr = htonl( INADDR_ANY );
            return true;
        }
        in_addr parsed{};
        if( inet_pton( AF_INET, h.c_str(), &parsed ) == 1 )
        {
            addr.sin_addr = parsed;
            return true;
        }
        return false;
    }

    static const char* httpStatusText( int code )
    {
        switch( code )
        {
            case 200: return "OK";
            case 202: return "Accepted";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 411: return "Length Required";
            case 413: return "Payload Too Large";
            case 503: return "Service Unavailable";
            default:  return "OK";
        }
    }

    // One reply: the status line, the body, and any extra headers. Every
    // reply says "Connection: close" -- the server handles one request per
    // connection, and a client that pools connections must see that.
    static void sendResponse( Wire& w, int status, const std::string& body, const std::string& mime,
                              const std::map<std::string, std::string>& extra = {} )
    {
        std::ostringstream h;
        h << "HTTP/1.1 " << status << ' ' << httpStatusText( status ) << "\r\n"
          << "Content-Type: " << mime << "\r\n"
          << "Content-Length: " << body.size() << "\r\n"
          << "Connection: close\r\n";
        for( const auto& kv : extra ) h << kv.first << ": " << kv.second << "\r\n";
        h << "\r\n";
        const std::string head = h.str();
        if( w.sendAll( head.data(), head.size() ) != 0 ) return;
        if( !body.empty() ) w.sendAll( body.data(), body.size() );
    }

    static void sendStatus( Wire& w, int code, const char* text )
    {
        std::ostringstream h;
        h << "HTTP/1.1 " << code << ' ' << text << "\r\nContent-Length: 0\r\n"
          << "Content-Type: application/json\r\nConnection: close\r\n\r\n";
        w.sendAll( h.str().data(), h.str().size() );
    }

    // A refusal that carries a JSON body, so the client can read WHY it
    // was refused instead of only that it was.
    static void sendJsonError( Wire& w, int code, const char* text, const std::string& message,
                               const std::map<std::string, std::string>& extra = {} )
    {
        const std::string body = Json{ { "error", message } }.dump();
        std::ostringstream h;
        h << "HTTP/1.1 " << code << ' ' << text << "\r\nContent-Type: application/json\r\n"
          << "Content-Length: " << body.size() << "\r\nConnection: close\r\n";
        for( const auto& kv : extra ) h << kv.first << ": " << kv.second << "\r\n";
        h << "\r\n";
        const std::string head = h.str();
        if( w.sendAll( head.data(), head.size() ) != 0 ) return;
        w.sendAll( body.data(), body.size() );
    }

    // The RFC 6750 challenge a 401 carries. No scope and no
    // resource_metadata: this server authenticates one bearer token (or a
    // per-client credential from the policy file) and does not implement
    // the OAuth 2.1 resource-server flow.
    static const char* const kAuthRealm = "Bearer realm=\"fractalsql-mcp\"";

    // A JSON-RPC error body for a refusal that happened before the request
    // reached the dispatch core. There is no id to correlate it with, so
    // the body carries none.
    static std::string rpcErrorBody( int code, const std::string& message, const Json& data = Json() )
    {
        Json err{ { "code", code }, { "message", message } };
        if( !data.is_null() ) err[ "data" ] = data;
        return Json{ { "jsonrpc", "2.0" }, { "error", err } }.dump();
    }

    static void sendRpcError( Wire& w, int status, int code, const std::string& message,
                              const Json& data = Json(),
                              const std::map<std::string, std::string>& extra = {} )
    {
        sendResponse( w, status, rpcErrorBody( code, message, data ), "application/json", extra );
    }

    // Extracts the bearer credential from the Authorization header and
    // checks it against the token store (the timing-safe compare lives in
    // TokenStore::check). The presented value is returned via presentedOut
    // but is NEVER logged by any caller.
    //
    // The header is the ONLY place a credential is read from: a token in
    // the URL's query string is not a credential, and the spec forbids
    // putting one there (it ends up in proxy logs and browser history).
    static TokenStore::Credential authenticate( const HttpRequest& req, TokenStore& tokens,
                                               std::string& presentedOut )
    {
        presentedOut.clear();
        const std::string authHeader = headerValue( req, "authorization" );
        if( lowerCopy( authHeader ).rfind( "bearer ", 0 ) == 0 ) presentedOut = trimCopy( authHeader.substr( 7 ) );
        if( presentedOut.empty() ) return TokenStore::Credential{};
        return tokens.check( presentedOut );
    }

    // Reads one HTTP request HEAD (the request line and the headers);
    // returns false on EOF/empty. The body is read separately, AFTER
    // the request has been authenticated -- see readRequestBody.
    //
    // Splitting the two is deliberate. The body is up to 32 MiB, and a
    // client that has not authenticated yet should not be able to make
    // the server hold that much memory per connection: the head is
    // bounded by the per-line caps below, so an unauthenticated
    // connection can only ever cost the server a few tens of KiB.
    static bool readRequestHead( Wire& w, HttpRequest& out )
    {
        // The whole head, not just each line, is bounded: an unauthenticated
        // client must not be able to keep a thread reading headers forever.
        constexpr std::size_t kHeadMax = 64 * 1024;
        constexpr int kMaxHeaders = 100;
        std::size_t headBytes = 0;
        std::string buf;
        auto readLine = [&]( std::string& line, std::size_t cap ) -> bool
        {
            std::size_t nl;
            while( ( nl = buf.find( '\n' ) ) == std::string::npos )
            {
                if( buf.size() > cap || ++headBytes > kHeadMax ) return false;
                char tmp[ 1 ];
                const int n = w.recvSome( tmp, sizeof( tmp ) );
                if( n <= 0 ) return false;
                buf.append( tmp, static_cast<std::size_t>( n ) );
            }
            line = buf.substr( 0, nl );
            if( !line.empty() && line.back() == '\r' ) line.pop_back();
            buf.erase( 0, nl + 1 );
            return true;
        };

        std::string head;
        if( !readLine( head, 16 * 1024 ) ) return false;
        {
            std::istringstream sl( head );
            sl >> out.method >> out.target;
        }
        out.path = targetPath( out.target );
        std::string h;
        int headerCount = 0;
        bool sawLength = false, sawEncoding = false;
        std::set<std::string> seenHeaders;
        while( readLine( h, 64 * 1024 ) && !h.empty() )
        {
            if( ++headerCount > kMaxHeaders ) return false;
            const std::size_t colon = h.find( ':' );
            if( colon == std::string::npos ) continue;    // not a header line: ignored
            const std::string name = lowerCopy( trimCopy( h.substr( 0, colon ) ) );
            const std::string value = trimCopy( h.substr( colon + 1 ) );
            if( name.empty() ) continue;
            if( !seenHeaders.insert( name ).second && isSingleValuedHeader( name ) ) { out.malformed = true; return false; }

            if( name == "content-length" )
            {
                // Exactly one Content-Length, digits only: anything else is a
                // framing error, not a different length.
                const bool digits = !value.empty() && value.size() <= 12 &&
                    std::all_of( value.begin(), value.end(), []( char c ) { return c >= '0' && c <= '9'; } );
                if( sawLength || !digits ) { out.malformed = true; return false; }
                sawLength = true;
                out.contentLength = static_cast<std::size_t>( std::strtoull( value.c_str(), nullptr, 10 ) );
            }
            else if( name == "transfer-encoding" )
            {
                // A chunked body is not accepted: the reply is 411 and the
                // connection closes, so the body is never read.
                if( sawEncoding ) { out.malformed = true; return false; }
                sawEncoding = true;
                out.chunked = true;
            }
            else if( name == "connection" )
            {
                if( lowerCopy( value ) == "close" ) out.keepAlive = false;
            }
            else if( name == "mcp-session-id" )
            {
                out.sessionId = value;
            }
            else
            {
                // Every other header is kept, with its name in lower case.
                // A header sent twice keeps its first value; the ones that
                // steer the protocol are checked for agreement elsewhere.
                out.headers.emplace( name, value );
            }
        }
        // The bearer credential, if the Authorization header carries one.
        // The raw header is kept in the map as well: the credential check
        // reads it from there, so a header that is not a bearer credential
        // is refused instead of being taken for "no credential at all".
        const std::string authHeader = headerValue( out, "authorization" );
        if( lowerCopy( authHeader ).rfind( "bearer ", 0 ) == 0 ) out.bearer = trimCopy( authHeader.substr( 7 ) );
        if( sawLength && sawEncoding ) { out.malformed = true; return false; }
        return !out.method.empty();
    }

    // Reads the request BODY into `out`. Returns false on EOF; throws
    // when the body exceeds the 32 MiB cap.
    static bool readRequestBody( Wire& w, HttpRequest& out )
    {
        if( out.contentLength == 0 ) return true;
        if( out.contentLength > kBodyMax )
            throw ToolError( ErrorClass::Connectivity, "request body exceeds the 32 MiB cap" );
        out.body.resize( out.contentLength );
        std::size_t got = 0;
        while( got < out.contentLength )
        {
            const int n = w.recvSome( &out.body[ got ], out.contentLength - got );
            if( n <= 0 ) return false;
            got += static_cast<std::size_t>( n );
        }
        return true;
    }

    // Reads and DISCARDS `count` bytes of the request body. Used on the
    // paths that answer before the body is read: the client has already
    // sent the body, and a connection that is closed with the body still
    // unread makes the kernel reset the connection, so the client would
    // see a reset instead of the answer the server wrote.
    static void drainBodyBytes( Wire& w, std::size_t count )
    {
        if( count == 0 || count > kBodyMax ) return;
        char buf[ 4096 ];
        std::size_t left = count;
        while( left > 0 )
        {
            const std::size_t want = left < sizeof( buf ) ? left : sizeof( buf );
            const int n = w.recvSome( buf, want );
            if( n <= 0 ) return;
            left -= static_cast<std::size_t>( n );
        }
    }

    // Reads and discards whatever is already waiting in the socket, up to
    // `maxBytes`. The framing refusals answer a request whose body the
    // server never reads (a chunked body has no length to read, and an
    // over-limit body is not read at all), and the answer still has to
    // reach the client: the bytes that are already in the socket are
    // discarded first, so the connection does not end in a reset.
    static void drainPending( Wire& w, std::size_t maxBytes )
    {
        char buf[ 4096 ];
        std::size_t total = 0;
        w.setTimeoutMs( 100 );    // the bytes are already there; this is only a backstop
        while( total < maxBytes )
        {
            const int n = w.recvSome( buf, sizeof( buf ) );
            if( n <= 0 ) break;
            total += static_cast<std::size_t>( n );
        }
    }

    // ---- request handling, split by concern ----
    //
    // Each helper below handles one decision and returns whether the
    // connection may continue. Keeping them separate (rather than one
    // long function with inline `break`s) is what makes the
    // one-request-per-connection rule below obvious: there is exactly
    // one place that decides "reply and stop".

    // The two unauthenticated liveness endpoints. They touch no session
    // state and no DB, so they are answered before the auth check.
    //
    // The return value means "this request is FINISHED -- do not send
    // anything else on this connection". A wrong path is finished: the
    // 404 IS the answer. Returning false there (as an earlier version
    // of this helper did) let the caller fall through to the auth check
    // and then to the dispatch, so an authenticated POST to a path the
    // server does not serve got a 404 AND a 200 with the tool's result
    // -- two HTTP responses on one connection, and the tool really ran.
    // A client that reads one response per connection was then left
    // with the stale 200 in its socket buffer, and the NEXT request on
    // a pooled socket parsed that stale response as its own.
    static bool handleUnauthenticatedGet( Wire& wire, const HttpRequest& req, const std::string& mcpPath,
                                          const std::function<bool()>& readinessProbe )
    {
        if( req.path == "/healthz" ) { sendStatus( wire, 200, "OK" ); return true; }
        if( req.path == "/readyz" )
        {
            // This route is unauthenticated and the probe is a live query: the result is kept for 5 s so a burst of requests
            // costs the engine one probe, not one each (SECURITY_AUDIT.md 3.14).
            static std::mutex probeMu;
            static std::chrono::steady_clock::time_point probedAt{};
            static bool probed = false, lastReady = false;
            bool ready;
            {
                std::lock_guard<std::mutex> lk( probeMu );
                const auto now = std::chrono::steady_clock::now();
                if( !probed || now - probedAt >= std::chrono::seconds( 5 ) )
                {
                    lastReady = readinessProbe();
                    probedAt = now;
                    probed = true;
                }
                ready = lastReady;
            }
            sendStatus( wire, ready ? 200 : 503, ready ? "OK" : "Service Unavailable" );
            return true;
        }
        if( req.path != mcpPath ) { sendStatus( wire, 404, "Not Found" ); return true; }
        return false;   // the MCP path itself: the caller continues
    }

    // DELETE closes the named session's dedicated DB connection.
    static bool handleDelete( Wire& wire, const HttpRequest& req, ContextRegistry& reg,
                              const std::string& ownerFp )
    {
        if( req.sessionId.empty() ) { sendStatus( wire, 400, "Bad Request" ); return false; }
        if( reg.erase( req.sessionId, ownerFp ) ) sendStatus( wire, 200, "OK" );
        else sendStatus( wire, 404, "Not Found" );
        return false;
    }

    // The legacy era: the protocol session supplies the engine context.
    // An initialize without a session id mints one (and its dedicated
    // engine connection); every other request presents a valid one.
    static void handleLegacyDispatch( Wire& wire, const HttpRequest& req, const Json& body,
                                      const std::string& method, bool isRequest,
                                      ContextRegistry& reg, const TokenStore::Credential& cred,
                                      const std::string& ownerFp )
    {
        std::string sessionId = req.sessionId;
        std::shared_ptr<JsonRpcServer> srv;
        std::string mintedId;
        if( sessionId.empty() )
        {
            if( method != "initialize" ) { sendStatus( wire, 400, "Bad Request" ); return; }
            try
            {
                const ContextRegistry::Handle h = reg.create( ownerFp );
                sessionId = h.id;
                srv = h.server;
                mintedId = h.id;
            }
            catch( const ToolError& ) { sendStatus( wire, 503, "Service Unavailable" ); return; }
        }
        else
        {
            srv = reg.find( sessionId, ownerFp );
            if( !srv ) { sendStatus( wire, 404, "Not Found" ); return; }
        }

        // A context handle in the legacy era has to be the session's own
        // context: the protocol session supplies the context there, so a
        // handle that names a different one is refused.
        const std::string requestedContext = paramsArgumentString( body, "context_id" );
        if( !requestedContext.empty() && requestedContext != sessionId )
        {
            sendRpcError( wire, 400, -32602,
                "the context handle does not match this session's context: the legacy era takes "
                "the context from the protocol session" );
            return;
        }

        const std::string clientKey = clientKeyFor( req.bearer, sessionId );
        CallOptions opts;
        opts.era = ProtocolEra::Legacy;
        opts.toolAllow = cred.allow;
        // The protocol session is the context in this era, so a call that
        // does not name the state it acts on acts on the session's own.
        opts.contextKey = sessionId;
        auto respOpt = srv->handleLine( req.body, clientKey, opts );

        std::map<std::string, std::string> extra;
        if( !mintedId.empty() ) extra[ "Mcp-Session-Id" ] = mintedId;
        if( respOpt ) sendResponse( wire, 200, *respOpt, "application/json", extra );
        else
        {
            // A notification gets an acknowledgement and nothing of
            // substance; a request whose result was discarded answers 200.
            sendResponse( wire, isRequest ? 200 : 202, "", "text/plain", extra );
        }
    }

    // The modern era: stateless, no protocol session. The engine context
    // comes from the request's own context handle when the call addresses a
    // stateful tool, and the credential's own connection otherwise.
    static void handleModernDispatch( Wire& wire, const HttpRequest& req, const Json& body,
                                      const std::string& method, bool isRequest,
                                      ContextRegistry& reg, const TokenStore::Credential& cred,
                                      const std::string& ownerFp )
    {
        const ModernCheck check = validateModernRequest( req, body, method );
        if( !check.ok )
        {
            sendRpcError( wire, check.httpStatus, check.code, check.message, check.data );
            return;
        }
        if( !isKnownRpcMethod( method ) )
        {
            sendRpcError( wire, 404, -32601, "method not found: " + method );
            return;
        }

        // The engine context: the request's own handle when it names one
        // and the tool is stateful, a fresh handle when it names none, and
        // the credential's own connection for a tool that keeps no state.
        const std::string toolName = ( method == "tools/call" ) ? paramsString( body, "name" ) : std::string();
        const bool stateful = !toolName.empty() && isContextScopedTool( toolName );

        std::shared_ptr<JsonRpcServer> srv;
        std::string contextId;
        if( stateful )
        {
            const std::string requested = paramsArgumentString( body, "context_id" );
            if( requested.empty() )
            {
                try
                {
                    const ContextRegistry::Handle h = reg.create( ownerFp );
                    contextId = h.id;
                    srv = h.server;
                }
                catch( const ToolError& ) { sendStatus( wire, 503, "Service Unavailable" ); return; }
            }
            else
            {
                srv = reg.find( requested, ownerFp );
                if( !srv )
                {
                    sendJsonError( wire, 404, "Not Found",
                        "unknown context handle: it does not exist, or it belongs to another credential" );
                    return;
                }
                contextId = requested;
            }
        }
        else
        {
            try { srv = reg.credentialServer( cred.clientName, ownerFp ); }
            catch( const ToolError& ) { sendStatus( wire, 503, "Service Unavailable" ); return; }
        }

        CallOptions opts;
        opts.era = ProtocolEra::Modern;
        opts.contextId = stateful ? contextId : std::string();
        // A stateful call runs on a handle of its own, so that handle is the
        // state it acts on when the call does not name the state itself.
        opts.contextKey = stateful ? contextId : std::string();
        opts.toolAllow = cred.allow;
        auto respOpt = srv->handleLine( req.body, cred.clientName, opts );
        if( respOpt ) sendResponse( wire, 200, *respOpt, "application/json" );
        else sendResponse( wire, isRequest ? 200 : 202, "", "text/plain" );
    }

    static void serveConnection( const WirePtr& wire, ContextRegistry& reg,
                                  TokenStore& tokens, const HttpConfig& cfg,
                                  std::function<bool()> readinessProbe )
    {
        // One request per connection. The MCP HTTP transport is a
        // request/response protocol: the client opens a connection,
        // sends one request, and reads one reply. Keeping a keep-alive
        // loop here bought nothing and cost a class of bugs -- a
        // half-sent request left in the socket when the session was
        // torn down, and a reply written to a connection the client had
        // already abandoned. One request, one reply, one close.
        HttpRequest req;
        if( !readRequestHead( *wire, req ) )
        {
            if( req.malformed )
            {
                drainPending( *wire, 64 * 1024 );
                sendStatus( *wire, 400, "Bad Request" );
            }
            return;
        }

        // A chunked body is not accepted: the length of the body has to be
        // known before it is read. The reply is 411 and the connection
        // closes without the body ever being read.
        if( req.chunked )
        {
            drainPending( *wire, 64 * 1024 );
            sendJsonError( *wire, 411, "Length Required",
                "a chunked request body is not accepted; send Content-Length" );
            return;
        }

        wire->setTimeoutMs( 65000 );    // the head arrived; allow a slower body

        if( g_httpSignalled.load() ) return;
        if( handleUnauthenticatedGet( *wire, req, cfg.path, readinessProbe ) ) return;

        // The origin, when the request carries one, has to be one this
        // deployment allows. A browser origin that is not allowed is
        // refused before the request is looked at any further.
        const std::string origin = headerValue( req, "origin" );
        if( !originAllowed( origin, cfg ) )
        {
            drainBodyBytes( *wire, req.contentLength );
            sendRpcError( *wire, 403, -32000,
                "origin not allowed: " + origin + " is not a loopback origin and is not "
                "listed in FSQL_MCP_ALLOWED_ORIGINS" );
            return;
        }

        // The credential. 401 BEFORE the request reaches the adapter layer.
        // The presented value is never logged. The challenge names the
        // realm; a token that WAS presented but is wrong or expired also
        // says why.
        std::string presented;
        const TokenStore::Credential cred = authenticate( req, tokens, presented );
        if( !cred.ok )
        {
            authFailed();
            drainBodyBytes( *wire, req.contentLength );
            std::map<std::string, std::string> extra;
            extra[ "WWW-Authenticate" ] = std::string( kAuthRealm ) +
                ( presented.empty() ? "" : ", error=\"invalid_token\"" );
            sendJsonError( *wire, 401, "Unauthorized",
                presented.empty() ? "missing bearer token" : "invalid bearer token", extra );
            return;
        }
        authOk();
        const std::string ownerFp = tokenFingerprint( presented );

        // A modern era request carries the modern protocol version header
        // (and the matching _meta in its body). The legacy era has neither.
        const bool modernByHeader = req.headers.count( "mcp-protocol-version" ) > 0;

        if( req.method == "GET" || req.method == "DELETE" )
        {
            // The modern era has no GET stream and no session teardown, so
            // both are refused for a modern request.
            if( modernByHeader || modernEraOnly() )
            {
                sendJsonError( *wire, 405, "Method Not Allowed",
                    req.method == "GET"
                        ? "GET is not used by the modern protocol era; POST JSON-RPC 2.0 messages"
                        : "DELETE is not used by the modern protocol era; it has no protocol session to tear down" );
                return;
            }
            if( req.method == "GET" ) { sendStatus( *wire, 405, "Method Not Allowed" ); return; }
            handleDelete( *wire, req, reg, ownerFp );
            return;
        }

        if( req.method != "POST" ) { sendStatus( *wire, 405, "Method Not Allowed" ); return; }

        // The body is read only once the request is known to be
        // authenticated: an unauthenticated client must not be able to
        // make the server buffer a body for it.
        try
        {
            if( !readRequestBody( *wire, req ) ) return;
        }
        catch( const ToolError& )
        {
            drainPending( *wire, 64 * 1024 );
            sendStatus( *wire, 413, "Payload Too Large" );
            return;
        }

        // A body that is not a JSON-RPC message is a parse error, and the
        // JSON-RPC error object says so: the answer is a 400 whose body
        // carries the error code, not an empty refusal.
        Json body;
        bool bodyParsed = false;
        if( !jsonNestsTooDeep( req.body ) )
        {
            try { body = Json::parse( req.body ); bodyParsed = true; }
            catch( ... ) { bodyParsed = false; }
        }
        if( !bodyParsed || !body.is_object() )
        {
            sendRpcError( *wire, 400, -32700,
                "parse error: the request body is not a JSON-RPC 2.0 message" );
            return;
        }

        const std::string method = ( body.contains( "method" ) && body[ "method" ].is_string() )
                                 ? body[ "method" ].get<std::string>() : std::string();
        const bool isRequest = body.contains( "id" );
        const bool isInitialize = ( method == "initialize" );

        // The era of this request. "initialize" is the legacy handshake and
        // always selects the legacy flow; anything else is modern when it
        // carries the modern metadata or the modern version header.
        const bool isModern = !isInitialize &&
            ( modernByHeader || !modernMetaVersion( body ).empty() );

        if( modernEraOnly() && !isModern )
        {
            // A modern-only server refuses the legacy era outright, and
            // says which revisions it speaks.
            sendRpcError( *wire, 400, -32000,
                "this server speaks the modern protocol era only (FSQL_MCP_PROTOCOL_ERA=modern); "
                "the legacy initialize handshake and the legacy session headers are refused. "
                "Supported versions: 2025-11-25, 2026-07-28" );
            return;
        }

        if( isModern )
        {
            handleModernDispatch( *wire, req, body, method, isRequest, reg, cred, ownerFp );
            return;
        }

        handleLegacyDispatch( *wire, req, body, method, isRequest, reg, cred, ownerFp );
    }

    // The last OpenSSL error as a readable string. Every TLS startup
    // failure below reports through it: a bare "cert/key failed to load"
    // tells an operator nothing about WHICH file failed or why.
    static std::string opensslLastError()
    {
        const unsigned long code = ERR_get_error();
        if( code == 0 ) return "unknown OpenSSL error";
        char buf[ 256 ];
        ERR_error_string_n( code, buf, sizeof( buf ) );
        return std::string( buf );
    }

    // A routable bind without TLS sends the bearer token and every tool result in clear
    // text. Warn always; refuse unless FSQL_MCP_ALLOW_PLAINTEXT_NONLOOPBACK is set.
    static bool hostIsLoopback( const std::string& h )
    {
        return h.empty() || h == "localhost" || h == "::1" || isLoopbackV4( h );
    }

    static SSL_CTX* makeSslCtx( const HttpConfig& cfg, std::string& errOut )
    {
        SSL_CTX* ctx = SSL_CTX_new( TLS_server_method() );
        if( !ctx ) { errOut = "SSL_CTX_new failed: " + opensslLastError(); return nullptr; }
        SSL_CTX_set_min_proto_version( ctx, TLS1_2_VERSION );
        if( SSL_CTX_use_certificate_chain_file( ctx, cfg.tlsCertPath.c_str() ) != 1 )
        {
            errOut = "failed to load TLS cert file '" + cfg.tlsCertPath + "': " + opensslLastError();
            SSL_CTX_free( ctx );
            return nullptr;
        }
        if( SSL_CTX_use_PrivateKey_file( ctx, cfg.tlsKeyPath.c_str(), SSL_FILETYPE_PEM ) != 1 )
        {
            errOut = "failed to load TLS key file '" + cfg.tlsKeyPath + "': " + opensslLastError();
            SSL_CTX_free( ctx );
            return nullptr;
        }
        // Both files loaded, but they may simply not belong to each other.
        // Without this check the mismatch surfaces much later, as a
        // handshake failure on every single connection -- an
        // indistinguishable symptom from a broken client.
        if( SSL_CTX_check_private_key( ctx ) != 1 )
        {
            errOut = "TLS certificate and private key do not match: " + opensslLastError();
            SSL_CTX_free( ctx );
            return nullptr;
        }
        return ctx;
    }

    int runHttpTransport( JsonRpcServer& server, const HttpConfig& cfg, std::function<bool()> readinessProbe )
    {
        (void)server;    // dispatch is per-session; this instance only carries the shared registry
        if( !cfg.sessionFactory )
            throw ToolError( ErrorClass::ServerInternal, "runHttpTransport needs a per-session factory" );

#ifdef SIGHUP
        std::signal( SIGHUP, onSighup );
#endif
        std::signal( SIGTERM, onHttpTermSignal );
        std::signal( SIGINT,  onHttpTermSignal );


        // The registry, the token store and the SSL context are held by
        // SHARED pointer and captured BY VALUE by every connection thread.
        // The shutdown path detaches whatever is still in flight (a client
        // that opened a connection and never sent a request must not be
        // able to wedge the server forever), and a detached thread
        // outliving this function would otherwise touch stack objects
        // that are already gone.
        std::shared_ptr<TokenStore> tokens = std::make_shared<TokenStore>( cfg );
        // A credential policy file that cannot be read or parsed is a
        // startup failure, not a server that quietly runs without
        // authentication.
        if( !tokens->policyError().empty() )
        {
            std::cerr << tokens->policyError() << "\n";
            return 2;
        }
        std::shared_ptr<ContextRegistry> reg = std::make_shared<ContextRegistry>( cfg );
        std::shared_ptr<SSL_CTX> sslCtx;
        if( cfg.tlsEnabled )
        {
            std::string tlsErr;
            SSL_CTX* ctx = makeSslCtx( cfg, tlsErr );
            if( !ctx )
                throw ToolError( ErrorClass::ServerInternal, "TLS enabled but cert/key failed to load: " + tlsErr );
            // The deleter is OpenSSL's, not delete's: an SSL_CTX is not a
            // C++ object and must go through SSL_CTX_free.
            sslCtx.reset( ctx, []( SSL_CTX* c ) { if( c ) SSL_CTX_free( c ); } );
        }

        const int listenFd = static_cast<int>( ::socket( AF_INET, SOCK_STREAM, 0 ) );
        if( listenFd < 0 )
            throw ToolError( ErrorClass::ServerInternal, "socket() failed" );
        int one = 1;
        setsockopt( listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>( &one ), sizeof( one ) );
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons( static_cast<unsigned short>( cfg.port ) );
        // The bind address comes from FSQL_MCP_BIND / --bind and defaults to
        // the loopback interface. Binding INADDR_ANY here would expose an
        // authenticated-but-otherwise-open JSON-RPC surface on every
        // interface of the host, which is not what "off by default" has to
        // mean for a server that ships with a Bearer token and no TLS.
        if( !resolveBindAddr( cfg.host, addr ) )
            // The message says what is ACCEPTED, not what was attempted:
            // no name resolution happens here, so a message about
            // "resolving" an address the code never tries to resolve
            // sends the operator looking for a DNS problem that is not
            // there.
            throw ToolError( ErrorClass::ServerInternal,
                             "the HTTP bind address '" + cfg.host + "' is not accepted: "
                             "use an IPv4 dotted quad (127.0.0.1), 'localhost', '*' "
                             "for every interface, or an empty value for the loopback" );
        if( ::bind( listenFd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) != 0 ||
            ::listen( listenFd, 16 ) != 0 )
            throw ToolError( ErrorClass::ServerInternal, "cannot bind the HTTP listener" );
        if( !sslCtx && !hostIsLoopback( cfg.host ) )
        {
            logging::logInfo( "WARNING: the HTTP transport is bound to non-loopback address '" + cfg.host +
                              "' WITHOUT TLS; the bearer token and all results travel in clear text (set FSQL_MCP_HTTP_TLS=true)" );
            const char* allowPlain = std::getenv( "FSQL_MCP_ALLOW_PLAINTEXT_NONLOOPBACK" );
            if( !( allowPlain && *allowPlain && std::string( allowPlain ) != "0" && std::string( allowPlain ) != "false" ) )
                throw ToolError( ErrorClass::ServerInternal, "refusing a non-loopback HTTP bind without TLS (set FSQL_MCP_HTTP_TLS=true, or FSQL_MCP_ALLOW_PLAINTEXT_NONLOOPBACK=true to accept the risk)" );
        }
        logging::logInfo( "http transport listening on " + cfg.host + ":" + std::to_string( cfg.port ) +
                          ( sslCtx ? " (TLS)" : " (plain)" ) +
                          ( modernEraOnly() ? " (modern protocol era only)" : " (legacy and modern protocol eras)" ) );

        // Every connection gets its own thread, and the thread objects are
        // REAPED as they finish. Keeping every thread object for the life
        // of the process (the old behaviour) grew the vector without bound
        // on a long-running server: a std::thread that has been joined
        // still occupies its stack handle, and an unjoined one occupies its
        // whole stack allocation.
        struct Conn
        {
            std::thread t;
            // Shared, not a plain member: a thread that is detached at
            // shutdown may outlive this function, and it still has to be
            // able to set the flag without touching freed memory.
            std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>( false );
        };

        // The bookkeeping lives in a SHARED object rather than in locals,
        // because the reaper thread and the connection threads both reach
        // it, and a connection thread that is detached at shutdown can
        // outlive this function. The stop flag is shared for the same
        // reason: the reaper is joined on every path out of here, but a
        // std::thread constructor that throws would leave it running
        // against a stack object that no longer exists.
        struct ConnState
        {
            std::mutex mu;
            std::condition_variable cv;
            std::vector<std::unique_ptr<Conn>> conns;
        };
        auto state = std::make_shared<ConnState>();
        auto reaperStop = std::make_shared<std::atomic<bool>>( false );
        // Refusals still delivering themselves (past the connection limit).
        auto refusalsInFlight = std::make_shared<std::atomic<int>>( 0 );

        // Joins and drops the connections that have already finished.
        // Called with state->mu held.
        auto reapLocked = [ state ]()
        {
            for( auto it = state->conns.begin(); it != state->conns.end(); )
            {
                if( ( *it )->done->load() )
                {
                    if( ( *it )->t.joinable() ) ( *it )->t.join();
                    it = state->conns.erase( it );
                }
                else ++it;
            }
        };
        auto reap = [ state, reapLocked ]()
        {
            std::lock_guard<std::mutex> lk( state->mu );
            reapLocked();
        };

        // A connection that finishes while the server is IDLE has to be
        // reclaimed too, and the accept loop cannot do it: it only wakes
        // for a new connection or the next poll slice. Without
        // this thread, a burst of N requests followed by a quiet period
        // left N finished-but-unjoined threads (each keeping its stack
        // allocated) and N stale entries in the vector until the next
        // request came in. The reaper waits on a condition variable that
        // a finishing connection signals, and also on a timeout so it
        // cannot miss a signal.
        std::thread reaper( [ state, reaperStop, reapLocked ]()
        {
            std::unique_lock<std::mutex> lk( state->mu );
            while( !reaperStop->load() )
            {
                state->cv.wait_for( lk, std::chrono::milliseconds( 250 ) );
                reapLocked();
            }
        } );

        const std::size_t connLimit = maxConnections();
        logging::logInfo( "http max connections: " + std::to_string( connLimit ) );

        while( !g_httpSignalled.load() )
        {
            if( !waitForConnection( listenFd, 250 ) ) continue;
            sockaddr_in peer{}; socklen_t plen = sizeof( peer );
            const int fd = static_cast<int>( ::accept( listenFd, reinterpret_cast<sockaddr*>( &peer ), &plen ) );
            if( fd < 0 )
            {
                // Out of descriptors, an aborted connection: keep serving.
                if( g_httpSignalled.load() ) break;
                std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
                continue;
            }
            bool overLimit = false;
            {
                std::lock_guard<std::mutex> lk( state->mu );
                reapLocked();
                overLimit = state->conns.size() >= connLimit;
            }
            if( overLimit )
            {
                // Over the limit: refuse before spending a thread on it. A
                // TLS client cannot read a plaintext reply; it just sees the close.
                if( !sslCtx )
                {
                    static const char kBusy[] = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\n"
                                                "Content-Length: 0\r\nConnection: close\r\n\r\n";
                    ::send( fd, kBusy, static_cast<int>( sizeof( kBusy ) - 1 ), 0 );
                    // The refusal is finished on its own short-lived thread:
                    // draining the peer's request takes a moment, and the
                    // accept loop must keep accepting while it does.
                    if( refusalsInFlight->fetch_add( 1 ) < kMaxRefusalsInFlight )
                    {
                        std::thread( [ fd, refusalsInFlight ] {
                            fsqlCloseAfterReply( fd, kRefusalLingerMs );
                            refusalsInFlight->fetch_sub( 1 );
                        } ).detach();
                    }
                    else
                    {
                        refusalsInFlight->fetch_sub( 1 );
                        fsqlCloseSocket( fd );
                    }
                }
                else
                    fsqlCloseSocket( fd );
                continue;
            }
            // Everything the thread touches is captured BY VALUE (shared
            // pointers for the registry, the token store, the SSL context
            // and the connection bookkeeping; the config by copy), so a
            // thread that is still running when this function returns
            // keeps every object it needs alive.
            const HttpConfig cfgCopy = cfg;
            auto conn = std::unique_ptr<Conn>( new Conn );
            auto done = conn->done;
            conn->t = std::thread( [ fd, reg, tokens, sslCtx, cfgCopy, readinessProbe, done, state ]()
            {
                WirePtr wire;
                setSocketTimeoutMs( fd, 15000 );    // TLS handshake and request head
                if( sslCtx )
                {
                    TlsWire* t = new TlsWire( fd, sslCtx.get() );
                    if( !t->ok() || !t->accept() ) { delete t; done->store( true ); state->cv.notify_one(); return; }
                    wire.reset( t );
                }
                else wire.reset( new PlainWire( fd ) );
                serveConnection( wire, *reg, *tokens, cfgCopy, readinessProbe );
                wire->closeWire();
                done->store( true );
                state->cv.notify_one();
            } );
            {
                std::lock_guard<std::mutex> lk( state->mu );
                state->conns.emplace_back( std::move( conn ) );
            }
            reap();
        }

        server.beginShutdown();
        reg->endAll();
        fsqlCloseSocket( listenFd );
        {
            // Stop the reaper before the drain: it shares the bookkeeping
            // with the drain loop, and the drain loop reaps on its own.
            reaperStop->store( true );
            state->cv.notify_all();
            if( reaper.joinable() ) reaper.join();
        }
        const auto deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds( cfg.shutdownDrainMs );
        while( std::chrono::steady_clock::now() < deadline )
        {
            reap();
            bool alive = false;
            { std::lock_guard<std::mutex> lk( state->mu ); alive = !state->conns.empty(); }
            // A refusal that is still delivering itself counts as alive: the
            // process must not exit while a client is owed its 503.
            if( !alive && refusalsInFlight->load() == 0 ) break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }
        {
            // Whatever is still in flight is detached rather than joined:
            // a client that opened a connection and never sent a request
            // would otherwise wedge the shutdown for good. Detaching is
            // safe because the thread owns every object it touches.
            std::lock_guard<std::mutex> lk( state->mu );
            for( auto& c : state->conns ) if( c->t.joinable() ) c->t.detach();
        }
        return 0;
    }
}
