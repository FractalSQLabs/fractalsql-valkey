// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
//
// libFuzzer entry point for the MCP server's JSON-RPC front door:
// JsonRpcServer::handleLine, the function both transports hand every
// client line to. The server is built once with two stub tools (one cheap,
// one expensive) so initialize, tools/list, tools/call argument validation,
// resources, prompts and cancellation are all reachable without a database.
//
// The input is one raw protocol line. Any reply, including an error
// response, is fine; a crash, hang or sanitizer report is the finding.
//
// Built and run by build_test.sh's MCP fuzz gate (gate 29).
#include "jsonrpc_server.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace fsqlmcp;

static JsonRpcServer& server()
{
    static JsonRpcServer* s = []
    {
        // Keep stderr quiet: the server logs every call.
        logging::init( { logging::Level::Info } );
        std::vector<ToolSpec> tools;
        ToolSpec cheap;
        cheap.name = "fuzz_cheap";
        cheap.category = "fuzz";
        cheap.toolClass = ToolClass::Cheap;
        cheap.params = {
            { "text", JsonType::String, true, nullptr, "a string" },
            { "n", JsonType::Integer, false, 3, "an integer" },
            { "v", JsonType::Array, false, nullptr, "an array" },
        };
        cheap.readOnlyHint = true;
        cheap.destructiveHint = false;
        cheap.description = "fuzz stub";
        cheap.handler = []( const Json& args, CallCtx& ) { return Json{ { "echo", args } }; };
        tools.push_back( cheap );
        ToolSpec costly = cheap;
        costly.name = "fuzz_expensive";
        costly.toolClass = ToolClass::Expensive;
        costly.destructiveHint = true;
        costly.readOnlyHint = false;
        tools.push_back( costly );
        return new JsonRpcServer( ServerConfig{}, std::move( tools ), ConcurrencyConfig{} );
    }();
    return *s;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t* data, size_t size )
{
    const std::string line( reinterpret_cast<const char*>( data ), size );
    try
    {
        ( void ) server().handleLine( line, "fuzz" );
    }
    catch( const std::exception& ) {}
    return 0;
}
