// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#pragma once
// Deployment policy switches that are read from the environment and
// consulted by the MCP layer. They live here (header-only) so the transport,
// the dispatch core and the adapter tables all read the same value through
// the same code, and so no new translation unit has to be added to the
// build.
//
// Three switches, all operator-set:
//
//   FSQL_MCP_ALLOW_AUTO_EXECUTE  off (default) | on
//       Whether a model may ask for auto_execute=true on the tools that
//       take it. The parameter is controlled by the same actor the control
//       protects against, so the operator decides, not the model.
//
//   FSQL_MCP_CONFIG_WRITES       off | safe (default) | all
//       Whether a config-writing admin tool may write, and which keys.
//       This engine has no config-writing admin tool: its tools are the
//       Valkey module commands, and the module's own configuration is set
//       through the server's configuration file or the command line, not
//       through a tool call. The switch is therefore defined but has no
//       call site here; the safe set is empty, so if such a tool is ever
//       added it refuses every key until an operator opts in.
//
//   FSQL_MCP_PROTOCOL_ERA        dual (default) | modern
//       Whether the server also serves the legacy protocol era. modern
//       refuses the legacy handshake and the legacy session headers.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace fsqlmcp
{
    inline std::string policyLower( std::string s )
    {
        std::transform( s.begin(), s.end(), s.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return s;
    }

    // The env value, trimmed and lower-cased; empty when unset.
    inline std::string policyEnv( const char* name )
    {
        const char* v = std::getenv( name );
        if( !v || !*v ) return std::string();
        std::string s( v );
        const std::size_t b = s.find_first_not_of( " \t" );
        if( b == std::string::npos ) return std::string();
        const std::size_t e = s.find_last_not_of( " \t" );
        return policyLower( s.substr( b, e - b + 1 ) );
    }

    inline bool policyEnvTrue( const char* name )
    {
        const std::string s = policyEnv( name );
        return s == "on" || s == "1" || s == "true" || s == "yes";
    }

    enum class ConfigWritesMode { Off, Safe, All };

    // off | safe | all. An unrecognized value is treated as safe: a typo
    // must not silently widen what the admin tools may write.
    inline ConfigWritesMode configWritesMode()
    {
        const std::string s = policyEnv( "FSQL_MCP_CONFIG_WRITES" );
        if( s == "off" || s == "0" || s == "false" || s == "no" ) return ConfigWritesMode::Off;
        if( s == "all" || s == "on" || s == "1" || s == "true" || s == "yes" ) return ConfigWritesMode::All;
        return ConfigWritesMode::Safe;
    }

    inline const char* configWritesModeName()
    {
        switch( configWritesMode() )
        {
            case ConfigWritesMode::Off: return "off";
            case ConfigWritesMode::All: return "all";
            default: return "safe";
        }
    }

    // The model may ask for auto_execute only when the operator turned it on.
    inline bool autoExecuteAllowed()
    {
        return policyEnvTrue( "FSQL_MCP_ALLOW_AUTO_EXECUTE" );
    }

    // true when the server serves the modern era ONLY.
    inline bool modernEraOnly()
    {
        return policyEnv( "FSQL_MCP_PROTOCOL_ERA" ) == "modern";
    }

    // The keys a config-writing admin tool may change in safe mode. This
    // engine exposes no per-connection config surface through the MCP
    // boundary, so the safe set is empty: a key that is not in it is
    // refused, and an unknown key is refused too.
    inline bool isSafeConfigKey( const std::string& )
    {
        return false;
    }

    // Whether a config key may be written through the MCP boundary under
    // the current FSQL_MCP_CONFIG_WRITES setting. off: nothing may be;
    // safe: only the low-risk keys; all: anything.
    inline bool configWriteAllowed( const std::string& key )
    {
        switch( configWritesMode() )
        {
            case ConfigWritesMode::Off: return false;
            case ConfigWritesMode::All: return true;
            default: return isSafeConfigKey( key );
        }
    }
}
