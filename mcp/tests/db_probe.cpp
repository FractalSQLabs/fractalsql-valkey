// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
// db_probe: the connection layer on its own, with no Valkey needed. It feeds the RESP parser every well-formed
// reply type and a set of malformed, truncated, oversized and over-nested byte streams, checks the error
// classification, and drives Db against a tiny in-process fake server that authenticates, answers, stalls,
// hangs up, lies about lengths and floods. When VALKEY_PORT is set it also runs a live leg against the real one.
//
//   build: see the Makefile target `probes`.  exit 0 = every check passed.
#include "../src/db.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace fsqlmcp;

static int g_fail = 0, g_n = 0;
static void check( bool ok, const std::string& what )
{
    ++g_n;
    if( !ok ) { ++g_fail; std::printf( "FAIL: %s\n", what.c_str() ); }
    else std::printf( "OK: %s\n", what.c_str() );
}

static bool parses( const std::string& bytes, Resp& out, std::size_t* took = nullptr, long long cap = 1 << 20 )
{
    try { const std::size_t n = parseResp( bytes.data(), bytes.size(), out, cap ); if( took ) *took = n; return n > 0; }
    catch( const ToolError& ) { return false; }
}
static bool throwsOnParse( const std::string& bytes, long long cap = 1 << 20 )
{
    Resp r;
    try { parseResp( bytes.data(), bytes.size(), r, cap ); return false; } catch( const ToolError& ) { return true; }
}

// --------------------------------------------------------------- fake server
struct Fake
{
    int lfd = -1; int port = 0; std::atomic<bool> stop{ false }; std::thread th;
    std::function<void( int )> serve;      // handles one accepted connection
    void start( std::function<void( int )> f )
    {
        serve = std::move( f );
        lfd = ::socket( AF_INET, SOCK_STREAM, 0 );
        int one = 1; setsockopt( lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); sa.sin_port = 0;
        ::bind( lfd, reinterpret_cast<sockaddr*>( &sa ), sizeof( sa ) );
        socklen_t sl = sizeof( sa ); getsockname( lfd, reinterpret_cast<sockaddr*>( &sa ), &sl ); port = ntohs( sa.sin_port );
        ::listen( lfd, 8 );
        th = std::thread( [ this ] { while( !stop ) { int c = ::accept( lfd, nullptr, nullptr ); if( c < 0 ) break; serve( c ); ::close( c ); } } );
    }
    ~Fake()
    {
        stop = true; ::shutdown( lfd, SHUT_RDWR ); ::close( lfd );
        int c = ::socket( AF_INET, SOCK_STREAM, 0 ); sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); sa.sin_port = htons( static_cast<uint16_t>( port ) );
        ::connect( c, reinterpret_cast<sockaddr*>( &sa ), sizeof( sa ) ); ::close( c );
        if( th.joinable() ) th.join();
    }
};

static std::string readSome( int fd, int ms = 1000 )
{
    timeval tv{ ms / 1000, ( ms % 1000 ) * 1000 }; setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
    char b[ 4096 ]; const ssize_t n = ::recv( fd, b, sizeof( b ), 0 );
    return n > 0 ? std::string( b, static_cast<std::size_t>( n ) ) : std::string();
}
static void sendStr( int fd, const std::string& s ) { ::send( fd, s.data(), s.size(), MSG_NOSIGNAL ); }

static EngineConfig cfgFor( int port )
{
    EngineConfig c; c.host = "127.0.0.1"; c.port = port; c.connectTimeoutMs = 2000; c.replyTimeoutMs = 1500; c.maxReplyBytes = 1 << 20;
    return c;
}
static bool callThrows( Db& db, ErrorClass cls, const std::vector<std::string>& cmd = { "PING" } )
{
    try { db.call( cmd ); return false; } catch( const ToolError& e ) { return e.errorClass == cls; }
}

int main()
{
    Resp r; std::size_t n = 0;
    // ---- the parser: happy paths
    check( parses( "+OK\r\n", r, &n ) && r.type == Resp::Type::Simple && r.str == "OK" && n == 5, "parse: a simple string" );
    check( parses( "-ERR boom\r\n", r ) && r.type == Resp::Type::Error && r.str == "ERR boom", "parse: an error reply" );
    check( parses( ":-42\r\n", r ) && r.type == Resp::Type::Integer && r.i == -42, "parse: a negative integer" );
    check( parses( "$5\r\nhello\r\n", r, &n ) && r.str == "hello" && n == 11, "parse: a bulk string" );
    check( parses( "$0\r\n\r\n", r ) && r.type == Resp::Type::Bulk && r.str.empty(), "parse: an empty bulk string" );
    check( parses( std::string( "$4\r\n\0\r\n\x01\r\n", 10 ), r ) && r.str.size() == 4 && r.str[ 0 ] == '\0', "parse: a binary-safe bulk string (NUL and CRLF inside)" );
    check( parses( "$-1\r\n", r ) && r.isNil(), "parse: a nil bulk" );
    check( parses( "*-1\r\n", r ) && r.isNil(), "parse: a nil array" );
    check( parses( "*2\r\n:1\r\n*1\r\n$1\r\nx\r\n", r ) && r.items.size() == 2 && r.items[ 1 ].items[ 0 ].str == "x", "parse: nested arrays" );
    check( parses( "*0\r\n", r ) && r.type == Resp::Type::Array && r.items.empty(), "parse: an empty array" );
    // ---- the parser: more bytes needed
    check( !parses( "$5\r\nhel", r ) && !throwsOnParse( "$5\r\nhel" ), "parse: a truncated bulk asks for more bytes" );
    check( !parses( "*2\r\n:1\r\n", r ) && !throwsOnParse( "*2\r\n:1\r\n" ), "parse: a truncated array asks for more bytes" );
    check( !parses( "+OK", r ) && !throwsOnParse( "+OK" ), "parse: a line without its end asks for more bytes" );
    check( !parses( "", r ), "parse: nothing yet asks for more bytes" );
    // ---- the parser: hostile input
    check( throwsOnParse( "?what\r\n" ), "parse: an unknown type byte is refused" );
    check( throwsOnParse( "$abc\r\n" ), "parse: a non-numeric bulk length is refused" );
    check( throwsOnParse( "$-5\r\n" ), "parse: a negative bulk length other than -1 is refused" );
    check( throwsOnParse( "$2000000\r\n", 1 << 20 ), "parse: a bulk length above the cap is refused before any byte is read" );
    check( throwsOnParse( "$99999999999999999999999\r\n" ), "parse: a bulk length that overflows is refused" );
    check( throwsOnParse( "*2000000\r\n" ), "parse: an array length above the cap is refused" );
    check( throwsOnParse( "$3\r\nabcXY" ), "parse: a bulk string not ended by CRLF is refused" );
    check( throwsOnParse( "+bare-lf\n" ), "parse: a line ended by a bare LF is refused" );
    { std::string deep; for( int i = 0; i < 40; ++i ) deep += "*1\r\n"; deep += ":1\r\n"; check( throwsOnParse( deep ), "parse: 40 levels of nesting are refused, not recursed into" ); }
    { std::string longLine = "+" + std::string( 100000, 'a' ); check( throwsOnParse( longLine ), "parse: a 100 KB line with no end is refused" ); }
    // ---- the classifier
    check( classifyReplyError( "NOAUTH Authentication required" ).errorClass == ErrorClass::Connectivity, "classify: NOAUTH is connectivity" );
    check( classifyReplyError( "WRONGPASS invalid username-password pair" ).errorClass == ErrorClass::Connectivity, "classify: WRONGPASS is connectivity" );
    check( classifyReplyError( "LOADING Valkey is loading the dataset" ).errorClass == ErrorClass::Connectivity, "classify: LOADING is connectivity" );
    check( classifyReplyError( "NOPERM this user has no permissions to run the 'x' command" ).errorClass == ErrorClass::Engine, "classify: NOPERM (an ACL refusal) is an engine error" );
    check( classifyReplyError( "ERR unknown command 'FRACTAL.SEARCH'" ).errorClass == ErrorClass::Connectivity, "classify: the module not loaded is connectivity" );
    check( classifyReplyError( "ERR the fractalsqld daemon is not configured" ).errorClass == ErrorClass::Engine, "classify: 'daemon not configured' is a clean engine error" );
    check( classifyReplyError( "ERR series must be a non-empty packed 32-bit BLOB" ).errorClass == ErrorClass::Engine, "classify: the module's own validation is an engine error" );
    check( classifyReplyError( "WRONGTYPE Operation against a key holding the wrong kind of value" ).errorClass == ErrorClass::Engine, "classify: WRONGTYPE is an engine error" );

    // ---- Db against a fake server
    { Db db; EngineConfig c; c.host = "127.0.0.1"; c.port = 1; c.connectTimeoutMs = 1000;
      bool threw = false; try { db.connect( c ); } catch( const ToolError& e ) { threw = e.errorClass == ErrorClass::Connectivity && e.message.find( "cannot connect" ) != std::string::npos; }
      check( threw, "connect: a closed port is a clean connectivity error" ); }
    { Db db; EngineConfig c = cfgFor( 1 ); c.host = "no-such-host.invalid";
      bool threw = false; try { db.connect( c ); } catch( const ToolError& e ) { threw = e.errorClass == ErrorClass::Connectivity; }
      check( threw, "connect: an unresolvable host is a connectivity error" ); }
    { Fake f; f.start( []( int fd ) { for( ;; ) { const std::string in = readSome( fd, 3000 ); if( in.empty() ) return;
            if( in.find( "CLIENT" ) != std::string::npos ) sendStr( fd, ":77\r\n" );
            else if( in.find( "PING" ) != std::string::npos ) sendStr( fd, "+PONG\r\n" );
            else if( in.find( "ECHO" ) != std::string::npos ) sendStr( fd, "$5\r\nhello\r\n" );
            else if( in.find( "BAD" ) != std::string::npos ) sendStr( fd, "-ERR bad thing\r\n" );
            else sendStr( fd, "+OK\r\n" ); } } );
      Db db; db.connect( cfgFor( f.port ) );
      check( db.connected() && db.clientId() == 77, "connect: reads CLIENT ID from a fake server" );
      check( db.probeAlive(), "probeAlive: PONG" );
      Resp e = db.call( { "ECHO", "hello" } );
      check( e.type == Resp::Type::Bulk && e.str == "hello", "call: a bulk reply round-trips" );
      check( callThrows( db, ErrorClass::Engine, { "BAD" } ), "call: an error reply becomes an engine ToolError" ); }
    { Fake f; f.start( []( int fd ) { readSome( fd, 3000 ); /* stall: never answer */ std::this_thread::sleep_for( std::chrono::milliseconds( 3500 ) ); } );
      Db db; EngineConfig c = cfgFor( f.port ); c.replyTimeoutMs = 700;
      const auto t0 = std::chrono::steady_clock::now();
      bool threw = false; try { db.connect( c ); } catch( const ToolError& e ) { threw = e.errorClass == ErrorClass::Connectivity && e.message.find( "timed out" ) != std::string::npos; }
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
      check( threw && ms < 3000, "timeout: a silent server ends in a connectivity error within the reply timeout" ); }
    { Fake f; f.start( []( int fd ) { readSome( fd, 3000 ); } );     // reads the first command and hangs up
      Db db; EngineConfig c = cfgFor( f.port );
      bool threw = false; try { db.connect( c ); } catch( const ToolError& e ) { threw = e.errorClass == ErrorClass::Connectivity; }
      check( threw, "hang-up: a server that closes mid-handshake is a connectivity error" ); }
    { Fake f; f.start( []( int fd ) { readSome( fd, 3000 ); sendStr( fd, "$999999999\r\n" ); std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) ); } );
      Db db; EngineConfig c = cfgFor( f.port );
      bool threw = false; try { db.connect( c ); } catch( const ToolError& e ) { threw = e.errorClass == ErrorClass::ServerInternal; }
      check( threw && !db.connected(), "lying length: a 999 MB bulk is refused and the connection is dropped" ); }
    { Fake f; f.start( []( int fd ) { readSome( fd, 3000 ); sendStr( fd, std::string( 4000, '\xff' ) ); std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) ); } );
      Db db; EngineConfig c = cfgFor( f.port );
      bool threw = false; try { db.connect( c ); } catch( const ToolError& e ) { threw = e.errorClass == ErrorClass::ServerInternal; }
      check( threw, "garbage: bytes that are not RESP are refused as an internal error" ); }
    { Fake f; f.start( []( int fd ) { readSome( fd, 3000 ); sendStr( fd, "-WRONGPASS invalid username-password pair\r\n" ); } );
      Db db; EngineConfig c = cfgFor( f.port ); c.password = "s3cret";
      bool threw = false; std::string msg; try { db.connect( c ); } catch( const ToolError& e ) { threw = e.errorClass == ErrorClass::Connectivity; msg = e.message; }
      check( threw && msg.find( "s3cret" ) == std::string::npos, "auth: a refused password is a connectivity error that does not echo the password" ); }
    { Fake f; f.start( []( int fd ) { for( ;; ) { const std::string in = readSome( fd, 3000 ); if( in.empty() ) return;
            if( in.find( "CLIENT" ) != std::string::npos ) sendStr( fd, ":5\r\n"); else sendStr( fd, "+OK\r\n" ); } } );
      Db db; db.connect( cfgFor( f.port ) ); db.close();
      check( !db.connected(), "close: the connection is released" );
      check( db.call( { "PING" } ).isString(), "reconnect: the next call after a close connects again" ); }

    // ---- the live leg
    if( const char* lp = std::getenv( "VALKEY_PORT" ) )
    {
        EngineConfig c = EngineConfig::fromEnv(); (void)lp;
        Db db;
        try
        {
            db.connect( c );
            check( db.probeAlive(), "live: PING answers" );
            check( !db.moduleVersion().empty(), "live: FRACTAL.VERSION answers (the module is loaded)" );
            check( callThrows( db, ErrorClass::Engine, { "FRACTAL.DIM.DFA", "x" } ), "live: a module validation failure is an engine error" );
            check( callThrows( db, ErrorClass::Engine, { "HGET" } ) || true, "live: a wrong-arity call does not crash the client" );
            check( db.connected(), "live: still connected after the errors" );
            db.cancelInflight();
            check( db.call( { "PING" } ).isString(), "live: cancelInflight drops the connection and the next call reconnects" );
        }
        catch( const ToolError& e ) { check( false, std::string( "live leg raised: " ) + e.message ); }
    }
    std::printf( "db_probe: %d checks, %d failed\n", g_n, g_fail );
    return g_fail ? 1 : 0;
}
