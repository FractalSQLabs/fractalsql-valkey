// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
//
// libFuzzer entry point for the MCP server's RESP reply parser, fsqlmcp::parseResp in mcp/src/db.cpp: the
// function every byte a (hostile or broken) Valkey sends goes through. The first byte picks the bulk-size
// limit and nesting depth so both the over-limit and the over-deep paths are reached; the rest is the reply.
// A ToolError is the documented rejection; a crash, hang or sanitizer report is the finding.
//
// Built and run by build_test.sh's MCP fuzz gate (gate 29).
#include "db.h"

#include <cstddef>
#include <cstdint>

using namespace fsqlmcp;

static void walk( const Resp& r, std::size_t& n )
{
    ++n;
    for( const Resp& c : r.items ) walk( c, n );
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t* data, size_t size )
{
    if( size == 0 ) return 0;
    const long long maxBulk = ( data[ 0 ] & 1 ) ? 64 : 1 << 20;
    const int depth = 1 + ( data[ 0 ] >> 1 ) % 10;
    try
    {
        Resp out;
        const std::size_t took = parseResp( reinterpret_cast<const char*>( data ) + 1, size - 1, out, maxBulk, depth );
        if( took > size - 1 ) __builtin_trap();   // consumed more than it was given
        std::size_t n = 0;
        walk( out, n );
    }
    catch( const ToolError& ) {}
    catch( const std::exception& ) {}
    return 0;
}
