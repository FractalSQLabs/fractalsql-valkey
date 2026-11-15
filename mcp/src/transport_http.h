#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "jsonrpc_server.h"

namespace fsqlmcp
{
    struct HttpConfig
    {
        std::string host = "127.0.0.1";  // FSQL_MCP_BIND / --bind
        int port = 8765;
        std::string path = "/mcp";        // FSQL_MCP_HTTP_PATH
        std::string authToken;          // FSQL_MCP_AUTH_TOKEN
        std::string authTokenFile;       // FSQL_MCP_AUTH_TOKEN_FILE (hot-reloaded)
        // FSQL_MCP_TOKEN_POLICY_FILE: JSON {"tokens":[{"sha256":"<hex of the bearer token>","tools":["a","b"] | "*"}]}.
        // Each listed token is an additional valid credential limited to its tools. The primary token above keeps every tool.
        // An entry may also carry an optional "name" (the client's label) and an
        // optional "expires" ("YYYY-MM-DD" or "YYYY-MM-DDTHH:MM:SSZ").
        std::string tokenPolicyFile;
        // FSQL_MCP_ALLOWED_ORIGINS: comma-separated origins a browser request
        // may come from. A loopback origin is always allowed; a request with no
        // Origin header is not a browser request and is allowed through.
        std::vector<std::string> allowedOrigins;
        bool tlsEnabled = false;
        std::string tlsCertPath, tlsKeyPath;
        int shutdownDrainMs = 5000;

        // Per-session engine state, supplied by main.cpp: each Mcp-Session-Id
        // gets its own JsonRpcServer built over its own dedicated DB connection
        // (per-session isolation). The same factory builds the server behind a
        // modern-era context handle.
        std::function<std::unique_ptr<JsonRpcServer>()> sessionFactory;
        // Called when a session is torn down (DELETE /mcp or server drain)
        // *after* its server is destroyed, e.g. Db::sessionClose().
        std::function<void()> sessionTeardown = nullptr;
    };

    // Minimal threaded HTTP/1.1 listener, no Boost.Asio/libuv. Every
    // path except GET /healthz and GET /readyz requires a valid Bearer token.
    // One dedicated DB session per Mcp-Session-Id (per-client
    // isolation), created via HttpConfig::sessionFactory.
    int runHttpTransport( JsonRpcServer& server, const HttpConfig& cfg,
                           std::function<bool()> readinessProbe );
}