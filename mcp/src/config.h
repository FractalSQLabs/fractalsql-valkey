// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#pragma once
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <fstream>

namespace fsqlmcp
{
    static inline std::string envOr( const char* name, const std::string& def )
    {
        const char* v = std::getenv( name );
        return ( v && *v ) ? std::string( v ) : def;
    }

    // Case-insensitive boolean env reader: true/1/on/yes (any case). Every
    // security-relevant switch (FSQL_MCP_READ_ONLY, HTTP_TLS, ...) goes
    // through this so "True" / "TRUE" / "1" cannot silently leave a
    // protection off.
    static inline bool envBoolOr( const char* name, bool def )
    {
        const char* v = std::getenv( name );
        if( !v || !*v ) return def;
        std::string s( v );
        for( char& c : s ) c = static_cast<char>( ::tolower( static_cast<unsigned char>( c ) ) );
        return s == "true" || s == "1" || s == "on" || s == "yes";
    }
    static inline int envIntOr( const char* name, int def )
    {
        const char* v = std::getenv( name );
        return ( v && *v ) ? std::atoi( v ) : def;
    }

    // A knob that must be a whole number of at least `minValue`. Unset or empty keeps the default; anything
    // else that is not such a number (text, a fraction, a negative, zero where zero would admit nobody) is
    // refused with the variable named. envIntOr's atoi turns "abc" into 0, and a concurrency limit of 0
    // makes every call wait out the whole queue timeout, so those knobs are checked here instead of
    // silently becoming a hang.
    static inline bool envIntAtLeast( const char* name, int def, int minValue, int* out, std::string* err )
    {
        const char* v = std::getenv( name );
        if( !v || !*v ) { *out = def; return true; }
        char* end = nullptr;
        errno = 0;
        const long n = std::strtol( v, &end, 10 );
        if( end == v || *end != '\0' || errno == ERANGE || n < minValue || n > 2147483647L )
        {
            *err = std::string( name ) + " must be a whole number of at least " + std::to_string( minValue )
                   + " (got '" + v + "')";
            return false;
        }
        *out = static_cast<int>( n );
        return true;
    }

    // How the server reaches Valkey. One TCP or Unix-socket connection per MCP session, authenticated with an
    // ACL user, so the Valkey ACL the operator gave that user is the authorization boundary.
    struct EngineConfig
    {
        std::string host = "127.0.0.1";                 // VALKEY_HOST
        int         port = 6379;                        // VALKEY_PORT
        std::string socketPath;                         // VALKEY_SOCKET (a Unix socket wins over host/port)
        std::string username;                           // VALKEY_USERNAME (ACL user; empty = the default user)
        std::string password;                           // VALKEY_PASSWORD or VALKEY_PASSWORD_FILE
        int         database = 0;                       // VALKEY_DB
        // TLS to Valkey (a server started with tls-port). Off by default; applies to TCP, never a Unix socket.
        bool        tls = false;                        // VALKEY_TLS=true
        std::string tlsCaFile;                          // VALKEY_TLS_CA_FILE (default: the system trust store)
        std::string tlsCertFile;                        // VALKEY_TLS_CERT_FILE (client certificate, with the key)
        std::string tlsKeyFile;                         // VALKEY_TLS_KEY_FILE
        std::string tlsServerName;                      // VALKEY_TLS_SERVER_NAME (default: VALKEY_HOST)
        bool        tlsInsecure = false;                // VALKEY_TLS_INSECURE=true: encrypt but do not verify the server
        int         connectTimeoutMs = 10000;           // FSQL_MCP_VALKEY_CONNECT_TIMEOUT_MS
        int         replyTimeoutMs   = 600000;          // FSQL_MCP_VALKEY_TIMEOUT_MS (a model-backed agent can take minutes)
        long long   maxReplyBytes    = 64LL << 20;      // FSQL_MCP_VALKEY_MAX_REPLY_MB

        // FSQL_MCP_READ_ONLY=true: every session also latches the module's destructive ledger commands
        // (FRACTAL.LEDGER.LOCK) on top of the server's refusal of the destructiveHint tools.
        bool lockDestructive = false;
        // FSQL_MCP_REQUIRE_LEDGER_LATCH=true: a failure to apply the latch refuses the connection instead of warning.
        bool requireLedgerLatch = false;

        // Empty on success; otherwise the configuration error to print before exiting 2.
        std::string error;

        static EngineConfig fromEnv()
        {
            EngineConfig c;
            c.host       = envOr( "VALKEY_HOST", "127.0.0.1" );
            c.socketPath = envOr( "VALKEY_SOCKET", "" );
            c.username   = envOr( "VALKEY_USERNAME", "" );
            c.password   = envOr( "VALKEY_PASSWORD", "" );
            const std::string pwFile = envOr( "VALKEY_PASSWORD_FILE", "" );
            if( !pwFile.empty() )
            {
                std::ifstream f( pwFile );
                std::string line;
                if( !f || !std::getline( f, line ) )
                    c.error = "VALKEY_PASSWORD_FILE '" + pwFile + "' could not be read";
                else
                {
                    while( !line.empty() && ( line.back() == '\r' || line.back() == '\n' ) ) line.pop_back();
                    c.password = line;
                }
            }
            {
                const std::string t = envOr( "VALKEY_TLS", "false" ), ins = envOr( "VALKEY_TLS_INSECURE", "false" );
                if( c.error.empty() && t != "true" && t != "false" ) c.error = "VALKEY_TLS must be true or false (got '" + t + "')";
                if( c.error.empty() && ins != "true" && ins != "false" ) c.error = "VALKEY_TLS_INSECURE must be true or false (got '" + ins + "')";
                c.tls = t == "true";
                c.tlsInsecure = ins == "true";
                c.tlsCaFile = envOr( "VALKEY_TLS_CA_FILE", "" );
                c.tlsCertFile = envOr( "VALKEY_TLS_CERT_FILE", "" );
                c.tlsKeyFile = envOr( "VALKEY_TLS_KEY_FILE", "" );
                c.tlsServerName = envOr( "VALKEY_TLS_SERVER_NAME", "" );
                if( c.error.empty() && c.tls && !c.socketPath.empty() ) c.error = "VALKEY_TLS does not apply to VALKEY_SOCKET (a Unix socket is local)";
                if( c.error.empty() && c.tlsCertFile.empty() != c.tlsKeyFile.empty() ) c.error = "VALKEY_TLS_CERT_FILE and VALKEY_TLS_KEY_FILE must be set together";
                if( c.error.empty() && !c.tls && ( !c.tlsCaFile.empty() || !c.tlsCertFile.empty() || c.tlsInsecure ) )
                    c.error = "VALKEY_TLS_* settings need VALKEY_TLS=true";
            }
            std::string e;
            if( c.error.empty() && !envIntAtLeast( "VALKEY_PORT", 6379, 1, &c.port, &e ) ) c.error = e;
            if( c.error.empty() && c.port > 65535 ) c.error = "VALKEY_PORT must be at most 65535";
            if( c.error.empty() )
            {
                const char* d = std::getenv( "VALKEY_DB" );
                if( d && *d )
                {
                    char* end = nullptr;
                    const long n = std::strtol( d, &end, 10 );
                    if( *end != '\0' || n < 0 || n > 65535 ) c.error = std::string( "VALKEY_DB must be a whole number from 0 to 65535 (got '" ) + d + "')";
                    else c.database = static_cast<int>( n );
                }
            }
            if( c.error.empty() && !envIntAtLeast( "FSQL_MCP_VALKEY_CONNECT_TIMEOUT_MS", 10000, 1, &c.connectTimeoutMs, &e ) ) c.error = e;
            if( c.error.empty() && !envIntAtLeast( "FSQL_MCP_VALKEY_TIMEOUT_MS", 600000, 1, &c.replyTimeoutMs, &e ) ) c.error = e;
            int maxMb = 64;
            if( c.error.empty() && !envIntAtLeast( "FSQL_MCP_VALKEY_MAX_REPLY_MB", 64, 1, &maxMb, &e ) ) c.error = e;
            c.maxReplyBytes = static_cast<long long>( maxMb ) << 20;
            c.lockDestructive = envBoolOr( "FSQL_MCP_READ_ONLY", false );   // same predicate as ServerConfig::readOnlyMode
            c.requireLedgerLatch = envBoolOr( "FSQL_MCP_REQUIRE_LEDGER_LATCH", false );
            return c;
        }
    };
}
