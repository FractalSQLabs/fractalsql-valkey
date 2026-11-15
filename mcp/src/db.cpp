// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#include "db.h"
#include "logging.h"
#include <cstring>
#include <iostream>
#include <cerrno>
#include <chrono>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace fsqlmcp
{
    namespace
    {
        bool startsWith( const std::string& s, const char* p ) { return s.rfind( p, 0 ) == 0; }
        bool containsCi( std::string hay, const char* needle )
        {
            for( char& c : hay ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
            return hay.find( needle ) != std::string::npos;
        }
        long long nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch() ).count();
        }
        std::string encodeCommand( const std::vector<std::string>& args )
        {
            std::string out = "*" + std::to_string( args.size() ) + "\r\n";
            for( const std::string& a : args )
            {
                out += "$" + std::to_string( a.size() ) + "\r\n";
                out += a;
                out += "\r\n";
            }
            return out;
        }
    }

    ToolError classifyReplyError( const std::string& message )
    {
        // The reply text is the engine's own and goes to the caller verbatim, but the class decides what a
        // client should do about it: fix the call, fix the connection, or wait.
        if( startsWith( message, "NOAUTH" ) || startsWith( message, "WRONGPASS" ) )
            return ToolError( ErrorClass::Connectivity, "Valkey refused the credentials: " + message );
        if( startsWith( message, "LOADING" ) || startsWith( message, "BUSY" ) || startsWith( message, "MASTERDOWN" )
            || startsWith( message, "TRYAGAIN" ) || startsWith( message, "CLUSTERDOWN" ) || startsWith( message, "READONLY" ) )
            return ToolError( ErrorClass::Connectivity, "Valkey is not ready to serve this call: " + message );
        if( containsCi( message, "unknown command" ) && containsCi( message, "fractal" ) )
            return ToolError( ErrorClass::Connectivity,
                              "the FractalSQL module is not loaded in this Valkey (loadmodule fractalsql.so): " + message );
        if( containsCi( message, "daemon" ) && ( containsCi( message, "unreachable" ) || containsCi( message, "timed out" )
                                                  || containsCi( message, "connect" ) ) )
            return ToolError( ErrorClass::Connectivity, message );
        // NOPERM (ACL), WRONGTYPE, module validation, "daemon not configured": the call reached the engine.
        return ToolError( ErrorClass::Engine, message );
    }

    std::size_t parseResp( const char* data, std::size_t len, Resp& out, long long maxBulkBytes, int maxDepth )
    {
        if( maxDepth < 0 ) throw ToolError( ErrorClass::ServerInternal, "RESP reply nested too deeply" );
        if( len == 0 ) return 0;
        const char type = data[ 0 ];
        // Validate the type byte BEFORE looking for the end of the line: junk with no newline must be refused at
        // once, not waited on until the reply timeout.
        if( type != '+' && type != '-' && type != ':' && type != '$' && type != '*' )
            throw ToolError( ErrorClass::ServerInternal, std::string( "unexpected RESP type byte 0x" )
                             + "0123456789abcdef"[ ( static_cast<unsigned char>( type ) >> 4 ) & 15 ]
                             + "0123456789abcdef"[ static_cast<unsigned char>( type ) & 15 ] );
        const char* eol = static_cast<const char*>( memchr( data + 1, '\n', len - 1 ) );
        if( !eol )
        {
            if( len > 64 * 1024 ) throw ToolError( ErrorClass::ServerInternal, "RESP line too long" );
            return 0;
        }
        const std::size_t lineEnd = static_cast<std::size_t>( eol - data );          // index of '\n'
        if( lineEnd < 2 || data[ lineEnd - 1 ] != '\r' )
            throw ToolError( ErrorClass::ServerInternal, "RESP line is not CRLF-terminated" );
        const std::string line( data + 1, lineEnd - 2 );
        const std::size_t headerLen = lineEnd + 1;
        auto toLong = [ & ]( const std::string& s ) -> long long
        {
            if( s.empty() || s.size() > 20 ) throw ToolError( ErrorClass::ServerInternal, "RESP length is not a number" );
            char* end = nullptr;
            errno = 0;
            const long long n = std::strtoll( s.c_str(), &end, 10 );
            if( *end != '\0' || errno == ERANGE ) throw ToolError( ErrorClass::ServerInternal, "RESP length is not a number" );
            return n;
        };
        switch( type )
        {
            case '+': out = Resp{}; out.type = Resp::Type::Simple; out.str = line; return headerLen;
            case '-': out = Resp{}; out.type = Resp::Type::Error;  out.str = line; return headerLen;
            case ':': out = Resp{}; out.type = Resp::Type::Integer; out.i = toLong( line ); return headerLen;
            case '$':
            {
                const long long n = toLong( line );
                out = Resp{};
                if( n == -1 ) { out.type = Resp::Type::Nil; return headerLen; }
                if( n < 0 || n > maxBulkBytes )
                    throw ToolError( ErrorClass::ServerInternal, "RESP bulk length " + std::to_string( n ) + " is out of range" );
                const std::size_t need = headerLen + static_cast<std::size_t>( n ) + 2;
                if( len < need ) return 0;
                if( data[ need - 2 ] != '\r' || data[ need - 1 ] != '\n' )
                    throw ToolError( ErrorClass::ServerInternal, "RESP bulk string is not CRLF-terminated" );
                out.type = Resp::Type::Bulk;
                out.str.assign( data + headerLen, static_cast<std::size_t>( n ) );
                return need;
            }
            case '*':
            {
                const long long n = toLong( line );
                out = Resp{};
                if( n == -1 ) { out.type = Resp::Type::Nil; return headerLen; }
                if( n < 0 || n > ( 1 << 20 ) )
                    throw ToolError( ErrorClass::ServerInternal, "RESP array length " + std::to_string( n ) + " is out of range" );
                std::size_t used = headerLen;
                Resp arr; arr.type = Resp::Type::Array;
                arr.items.reserve( static_cast<std::size_t>( n < 4096 ? n : 4096 ) );
                for( long long k = 0; k < n; ++k )
                {
                    Resp item;
                    const std::size_t took = parseResp( data + used, len - used, item, maxBulkBytes, maxDepth - 1 );
                    if( took == 0 ) return 0;
                    used += took;
                    arr.items.push_back( std::move( item ) );
                }
                out = std::move( arr );
                return used;
            }
            default:
                throw ToolError( ErrorClass::ServerInternal, std::string( "unexpected RESP type byte 0x" )
                                 + "0123456789abcdef"[ ( static_cast<unsigned char>( type ) >> 4 ) & 15 ]
                                 + "0123456789abcdef"[ static_cast<unsigned char>( type ) & 15 ] );
        }
    }

    void Db::close() noexcept
    {
        if( ssl_ ) { SSL_set_quiet_shutdown( ssl_, 1 ); SSL_free( ssl_ ); ssl_ = nullptr; }
        if( sslCtx_ ) { SSL_CTX_free( sslCtx_ ); sslCtx_ = nullptr; }
        if( fd_ >= 0 ) { ::close( fd_ ); fd_ = -1; }
        inbuf_.clear();
        clientId_ = 0;
    }

    void Db::openSocket()
    {
        close();
        int fd = -1;
        if( !cfg_.socketPath.empty() )
        {
            sockaddr_un sa{};
            sa.sun_family = AF_UNIX;
            if( cfg_.socketPath.size() >= sizeof( sa.sun_path ) )
                throw ToolError( ErrorClass::Connectivity, "VALKEY_SOCKET path is too long" );
            std::memcpy( sa.sun_path, cfg_.socketPath.c_str(), cfg_.socketPath.size() + 1 );
            fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
            if( fd < 0 ) throw ToolError( ErrorClass::Connectivity, "could not create a socket" );
            if( ::connect( fd, reinterpret_cast<sockaddr*>( &sa ), sizeof( sa ) ) != 0 )
            {
                const std::string why = std::strerror( errno );
                ::close( fd );
                throw ToolError( ErrorClass::Connectivity, "cannot connect to Valkey at unix:" + cfg_.socketPath + ": " + why );
            }
        }
        else
        {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            const int gai = ::getaddrinfo( cfg_.host.c_str(), std::to_string( cfg_.port ).c_str(), &hints, &res );
            if( gai != 0 )
                throw ToolError( ErrorClass::Connectivity, "cannot resolve VALKEY_HOST '" + cfg_.host + "': " + gai_strerror( gai ) );
            std::string lastErr = "no address";
            for( addrinfo* ai = res; ai; ai = ai->ai_next )
            {
                int s = ::socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
                if( s < 0 ) continue;
                // A bounded connect: non-blocking connect, then poll for writability.
                const int fl = fcntl( s, F_GETFL, 0 );
                fcntl( s, F_SETFL, fl | O_NONBLOCK );
                int rc = ::connect( s, ai->ai_addr, ai->ai_addrlen );
                if( rc != 0 && errno == EINPROGRESS )
                {
                    pollfd p{ s, POLLOUT, 0 };
                    const int pr = ::poll( &p, 1, cfg_.connectTimeoutMs );
                    if( pr == 1 )
                    {
                        int soerr = 0; socklen_t sl = sizeof( soerr );
                        getsockopt( s, SOL_SOCKET, SO_ERROR, &soerr, &sl );
                        rc = soerr == 0 ? 0 : -1;
                        if( soerr ) errno = soerr;
                    }
                    else { rc = -1; errno = ETIMEDOUT; }
                }
                if( rc == 0 ) { fcntl( s, F_SETFL, fl ); fd = s; break; }
                lastErr = std::strerror( errno );
                ::close( s );
            }
            ::freeaddrinfo( res );
            if( fd < 0 )
                throw ToolError( ErrorClass::Connectivity, "cannot connect to Valkey at " + cfg_.host + ":"
                                 + std::to_string( cfg_.port ) + ": " + lastErr );
            int one = 1;
            setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
        }
        fd_ = fd;
        if( cfg_.tls && cfg_.socketPath.empty() ) tlsHandshake();
    }

    void Db::tlsHandshake()
    {
        auto fail = [ this ]( const std::string& why )
        {
            ERR_clear_error();
            close();
            throw ToolError( ErrorClass::Connectivity, "TLS handshake with Valkey at " + cfg_.host + ":" + std::to_string( cfg_.port ) + " failed: " + why );
        };
        sslCtx_ = SSL_CTX_new( TLS_client_method() );
        if( !sslCtx_ ) fail( "could not create a TLS context" );
        SSL_CTX_set_min_proto_version( sslCtx_, TLS1_2_VERSION );
        if( cfg_.tlsInsecure ) SSL_CTX_set_verify( sslCtx_, SSL_VERIFY_NONE, nullptr );
        else
        {
            SSL_CTX_set_verify( sslCtx_, SSL_VERIFY_PEER, nullptr );
            if( !cfg_.tlsCaFile.empty() ? SSL_CTX_load_verify_locations( sslCtx_, cfg_.tlsCaFile.c_str(), nullptr ) != 1
                                        : SSL_CTX_set_default_verify_paths( sslCtx_ ) != 1 )
                fail( "could not load the CA certificates" + ( cfg_.tlsCaFile.empty() ? std::string() : " from '" + cfg_.tlsCaFile + "'" ) );
        }
        if( !cfg_.tlsCertFile.empty()
            && ( SSL_CTX_use_certificate_chain_file( sslCtx_, cfg_.tlsCertFile.c_str() ) != 1
                 || SSL_CTX_use_PrivateKey_file( sslCtx_, cfg_.tlsKeyFile.c_str(), SSL_FILETYPE_PEM ) != 1
                 || SSL_CTX_check_private_key( sslCtx_ ) != 1 ) )
            fail( "could not load the client certificate and key" );
        ssl_ = SSL_new( sslCtx_ );
        if( !ssl_ ) fail( "could not create a TLS session" );
        const std::string name = cfg_.tlsServerName.empty() ? cfg_.host : cfg_.tlsServerName;
        unsigned char ipbuf[ sizeof( in6_addr ) ];
        const bool isIp = inet_pton( AF_INET, name.c_str(), ipbuf ) == 1 || inet_pton( AF_INET6, name.c_str(), ipbuf ) == 1;
        if( !isIp ) SSL_set_tlsext_host_name( ssl_, name.c_str() );
        if( !cfg_.tlsInsecure )
        {
            if( isIp ? X509_VERIFY_PARAM_set1_ip_asc( SSL_get0_param( ssl_ ), name.c_str() ) != 1 : SSL_set1_host( ssl_, name.c_str() ) != 1 )
                fail( "could not set the name to verify" );
        }
        SSL_set_fd( ssl_, fd_ );
        fcntl( fd_, F_SETFL, fcntl( fd_, F_GETFL, 0 ) | O_NONBLOCK );
        const long long deadline = nowMs() + cfg_.connectTimeoutMs;
        for( ;; )
        {
            const int r = SSL_connect( ssl_ );
            if( r == 1 ) return;
            const int e = SSL_get_error( ssl_, r );
            if( e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE )
            {
                std::string why;
                const long vr = SSL_get_verify_result( ssl_ );
                if( vr != X509_V_OK ) why = std::string( "certificate verify failed: " ) + X509_verify_cert_error_string( vr );
                else
                {
                    char buf[ 256 ] = { 0 };
                    const unsigned long ec = ERR_peek_last_error();
                    if( ec ) ERR_error_string_n( ec, buf, sizeof buf );
                    why = ec ? buf : ( e == SSL_ERROR_SYSCALL || e == SSL_ERROR_ZERO_RETURN ? "the server closed the connection (is this a TLS port?)" : "protocol error" );
                }
                fail( why );
            }
            pollfd p{ fd_, static_cast<short>( e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT ), 0 };
            const long long left = deadline - nowMs();
            if( left <= 0 || ::poll( &p, 1, static_cast<int>( left > 2147483647 ? 2147483647 : left ) ) <= 0 ) fail( "timed out" );
        }
    }

    void Db::sendAll( const std::string& bytes )
    {
        std::size_t off = 0;
        const long long deadline = nowMs() + cfg_.replyTimeoutMs;
        while( off < bytes.size() )
        {
            pollfd p{ fd_, POLLOUT, 0 };
            const long long left = deadline - nowMs();
            if( left <= 0 || ::poll( &p, 1, static_cast<int>( left > 2147483647 ? 2147483647 : left ) ) <= 0 )
            { close(); throw ToolError( ErrorClass::Connectivity, "timed out sending a command to Valkey" ); }
            if( ssl_ )
            {
                const int w = SSL_write( ssl_, bytes.data() + off, static_cast<int>( bytes.size() - off > 1 << 20 ? 1 << 20 : bytes.size() - off ) );
                if( w > 0 ) { off += static_cast<std::size_t>( w ); continue; }
                const int se = SSL_get_error( ssl_, w );
                if( se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE ) continue;
                ERR_clear_error();
                close();
                throw ToolError( ErrorClass::Connectivity, "lost the connection to Valkey while sending (TLS)" );
            }
            const ssize_t n = ::send( fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL );
            if( n < 0 )
            {
                if( errno == EINTR || errno == EAGAIN ) continue;
                const std::string why = std::strerror( errno );
                close();
                throw ToolError( ErrorClass::Connectivity, "lost the connection to Valkey while sending: " + why );
            }
            off += static_cast<std::size_t>( n );
        }
    }

    Resp Db::readReply()
    {
        const long long deadline = nowMs() + cfg_.replyTimeoutMs;
        for( ;; )
        {
            Resp r;
            std::size_t took = 0;
            try { took = parseResp( inbuf_.data(), inbuf_.size(), r, cfg_.maxReplyBytes ); }
            catch( ... ) { close(); throw; }     // a malformed stream cannot be resynchronised
            if( took > 0 ) { inbuf_.erase( 0, took ); return r; }
            pollfd p{ fd_, POLLIN, 0 };
            const long long left = deadline - nowMs();
            if( left <= 0 ) { close(); throw ToolError( ErrorClass::Connectivity, "timed out waiting for Valkey's reply" ); }
            const int pr = ( ssl_ && SSL_pending( ssl_ ) > 0 ) ? 1 : ::poll( &p, 1, static_cast<int>( left > 2147483647 ? 2147483647 : left ) );
            if( pr == 0 ) { close(); throw ToolError( ErrorClass::Connectivity, "timed out waiting for Valkey's reply" ); }
            if( pr < 0 ) { if( errno == EINTR ) continue; close(); throw ToolError( ErrorClass::Connectivity, "poll failed while waiting for Valkey" ); }
            char buf[ 65536 ];
            ssize_t n;
            if( ssl_ )
            {
                const int rd = SSL_read( ssl_, buf, sizeof( buf ) );
                if( rd > 0 ) n = rd;
                else
                {
                    const int se = SSL_get_error( ssl_, rd );
                    if( se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE ) continue;
                    ERR_clear_error();
                    n = ( se == SSL_ERROR_ZERO_RETURN || ( se == SSL_ERROR_SYSCALL && errno == 0 ) ) ? 0 : -1;
                    if( n < 0 && errno == 0 ) errno = EIO;
                }
            }
            else n = ::recv( fd_, buf, sizeof( buf ), 0 );
            if( n == 0 )
            {
                const bool wasCancelled = cancelled_;
                close();
                if( wasCancelled ) throw ToolError( ErrorClass::Cancelled, "call was cancelled by the client" );
                throw ToolError( ErrorClass::Connectivity, "Valkey closed the connection" );
            }
            if( n < 0 )
            {
                if( errno == EINTR || errno == EAGAIN ) continue;
                const std::string why = std::strerror( errno );
                const bool wasCancelled = cancelled_;
                close();
                if( wasCancelled ) throw ToolError( ErrorClass::Cancelled, "call was cancelled by the client" );
                throw ToolError( ErrorClass::Connectivity, "lost the connection to Valkey: " + why );
            }
            inbuf_.append( buf, static_cast<std::size_t>( n ) );
            if( static_cast<long long>( inbuf_.size() ) > cfg_.maxReplyBytes + ( 1 << 20 ) )
            { close(); throw ToolError( ErrorClass::ServerInternal, "Valkey's reply exceeds the configured limit" ); }
        }
    }

    Resp Db::rawCall( const std::vector<std::string>& args )
    {
        // A connection that died while idle (Valkey restarted, an idle timeout, CLIENT KILL) is detected before
        // anything is sent, so the call reconnects instead of failing once. Bytes waiting on an idle connection
        // are unsolicited and mean it is out of step: drop it as well.
        if( fd_ >= 0 )
        {
            char b;
            const ssize_t n = ::recv( fd_, &b, 1, MSG_PEEK | MSG_DONTWAIT );
            if( n > 0 || n == 0 || ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ) ) close();
        }
        if( fd_ < 0 ) { openSocket(); handshake(); }
        sendAll( encodeCommand( args ) );
        return readReply();
    }

    void Db::handshake()
    {
        if( !cfg_.password.empty() || !cfg_.username.empty() )
        {
            std::vector<std::string> auth{ "AUTH" };
            if( !cfg_.username.empty() ) auth.push_back( cfg_.username );
            auth.push_back( cfg_.password );
            Resp r = rawCall( auth );
            if( r.type == Resp::Type::Error ) { const std::string m = r.str; close(); throw classifyReplyError( m ); }
        }
        if( cfg_.database != 0 )
        {
            Resp r = rawCall( { "SELECT", std::to_string( cfg_.database ) } );
            if( r.type == Resp::Type::Error ) { const std::string m = r.str; close(); throw classifyReplyError( m ); }
        }
        Resp id = rawCall( { "CLIENT", "ID" } );
        clientId_ = id.type == Resp::Type::Integer ? id.i : 0;
        if( cfg_.lockDestructive )
        {
            // Read-only mode: latch the ledger's destructive commands for THIS connection. The latch is keyed by the
            // Valkey client id, so it is lost whenever the connection is replaced -- this runs in handshake(), which
            // both the first connect and every reconnect go through. The module answers OK, or "not configured"
            // when no daemon is attached (nothing to latch, nothing to protect). A failure is a warning unless
            // FSQL_MCP_REQUIRE_LEDGER_LATCH is set, in which case the connection is refused (fail closed).
            Resp r = rawCall( { "FRACTAL.LEDGER.LOCK" } );
            if( r.type == Resp::Type::Error && !containsCi( r.str, "not configured" ) )
            {
                const std::string m = sanitizeSecrets( r.str );
                if( cfg_.requireLedgerLatch )
                {
                    close();
                    throw ToolError( ErrorClass::ServerInternal, "FSQL_MCP_READ_ONLY: the ledger latch could not be applied (FSQL_MCP_REQUIRE_LEDGER_LATCH is set): " + m );
                }
                std::cerr << "WARN: ledger latch not applied: " << m << "\n";
            }
        }
    }

    void Db::connect( const EngineConfig& cfg )
    {
        std::lock_guard<std::mutex> lk( mu_ );
        cfg_ = cfg;
        cancelled_ = false;
        openSocket();
        handshake();
    }

    Resp Db::call( const std::vector<std::string>& args )
    {
        if( args.empty() ) throw ToolError( ErrorClass::ServerInternal, "empty command" );
        std::lock_guard<std::mutex> lk( mu_ );
        cancelled_ = false;
        Resp r = rawCall( args );
        if( r.type == Resp::Type::Error ) throw classifyReplyError( r.str );
        return r;
    }

    bool Db::probeAlive()
    {
        try
        {
            std::lock_guard<std::mutex> lk( mu_ );
            Resp r = rawCall( { "PING" } );
            return r.type == Resp::Type::Simple && r.str == "PONG";
        }
        catch( ... ) { return false; }
    }

    void Db::cancelInflight()
    {
        // Deliberately not under mu_: the call being cancelled holds it. Only the id and config are read.
        const long long id = clientId_;
        if( id <= 0 ) return;
        cancelled_ = true;
        try
        {
            Db control;
            EngineConfig c = cfg_;
            c.lockDestructive = false;
            control.connect( c );
            control.call( { "CLIENT", "KILL", "ID", std::to_string( id ) } );
        }
        catch( ... ) { /* best effort: the call times out on its own */ }
    }

    std::string Db::moduleVersion()
    {
        try
        {
            Resp r = call( { "FRACTAL.VERSION" } );
            return r.isString() ? r.str : std::string();
        }
        catch( const ToolError& ) { return std::string(); }
    }

    void Db::checkVersionSkew( const std::string& expected )
    {
        if( expected.empty() ) return;
        const std::string got = moduleVersion();
        if( got.empty() )
            std::cerr << "WARN: " << ("the FractalSQL module did not answer FRACTAL.VERSION; is it loaded?") << "\n";
        else if( got != expected )
            std::cerr << "WARN: " << ("version skew: module " + got + " vs this server built for " + expected) << "\n";
    }
}
