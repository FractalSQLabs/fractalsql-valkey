// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
// fractalsql-valkey-mcp's connection layer: a small RESP2 client over TCP or a Unix socket, one dedicated
// connection per session (over stdio exactly one session), every command under the connection's own mutex
// (no pipelining within a session). The server never links the FractalSQL module or any Valkey client
// library: it is an ordinary client of a Valkey that has the module loaded, and the Valkey ACL of the user
// it connects as is the authorization boundary.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "config.h"
#include "errors.h"
#include "mcp_json.h"

struct ssl_st;
struct ssl_ctx_st;

namespace fsqlmcp
{
    // The plugin's own version, and the module version it was built and tested against.
    static const char* const kMcpPluginName       = "fractalsql-valkey-mcp";
    static const char* const kEngineVersionExpect = "2.0.2";

    // One RESP2 reply. An error reply is returned as a value here; Db::call turns it into a ToolError.
    struct Resp
    {
        enum class Type { Nil, Simple, Error, Integer, Bulk, Array };
        Type                 type = Type::Nil;
        std::string          str;      // Simple, Error and Bulk
        long long            i = 0;    // Integer
        std::vector<Resp>    items;    // Array

        bool isNil() const { return type == Type::Nil; }
        bool isString() const { return type == Type::Simple || type == Type::Bulk; }
    };

    // Maps a RESP error reply (or a transport failure message) onto the error taxonomy. Exposed for the probe.
    ToolError classifyReplyError( const std::string& message );

    // Decodes one reply from a byte buffer; returns the number of bytes consumed, 0 if more bytes are needed,
    // or throws ToolError(ServerInternal) on malformed or over-limit input. Exposed for the probe: this is
    // the parser a hostile or broken server's bytes go through.
    std::size_t parseResp( const char* data, std::size_t len, Resp& out, long long maxBulkBytes, int maxDepth = 8 );

    class Db
    {
    public:
        Db() = default;
        Db( const Db& ) = delete;
        Db& operator=( const Db& ) = delete;
        ~Db() { close(); }

        // Connects, authenticates, selects the database, and (read-only mode) latches the ledger. Raises a
        // Connectivity error with a clean message on any failure.
        void connect( const EngineConfig& cfg );

        // One command; the first element is the command name. Raises ToolError for transport failures
        // (Connectivity) and for error replies (classified).
        Resp call( const std::vector<std::string>& args );

        // The same, as a one-shot convenience.
        Resp call( std::initializer_list<std::string> args ) { return call( std::vector<std::string>( args ) ); }

        void close() noexcept;
        bool connected() const { return fd_ >= 0; }
        const EngineConfig& config() const { return cfg_; }
        bool probeAlive();
        long long clientId() const { return clientId_; }

        // $/cancel: drops THIS session's connection from a one-shot control connection (CLIENT KILL ID), so
        // the waiting call returns at once. The server-side job is not interruptible; it finishes and its
        // reply is discarded. The next call reconnects.
        void cancelInflight();

        // The module's own version string, or empty if the module is not loaded.
        std::string moduleVersion();
        // Warns (never fails) when the module version differs from the one this build was tested against.
        void checkVersionSkew( const std::string& expected );

    private:
        void openSocket();
        void tlsHandshake();                  // wraps fd_ in TLS when cfg_.tls
        void handshake();
        void sendAll( const std::string& bytes );
        Resp readReply();
        Resp rawCall( const std::vector<std::string>& args );   // no error-to-exception mapping; caller holds mu_

        EngineConfig cfg_;
        int          fd_ = -1;
        ssl_st*      ssl_ = nullptr;
        ssl_ctx_st*  sslCtx_ = nullptr;
        std::atomic<long long> clientId_{ 0 };   // read by cancelInflight() from another thread
        std::string  inbuf_;
        std::mutex   mu_;
        std::atomic<bool> cancelled_{ false };       // set by cancelInflight() from another thread
    };
}
