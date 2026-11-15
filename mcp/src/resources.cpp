// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#include "resources.h"
#include "db.h"
#include "config.h"
#include "errors.h"
#include "version.h"
#include <string>

namespace fsqlmcp
{
    static Db* g_db = nullptr;
    static thread_local Db* t_db = nullptr;    // per-HTTP-session binding
    static EngineConfig g_cfg;

    static Db* currentDb() { return t_db ? t_db : g_db; }

    void Resources::setEngineDb( Db* db ) { g_db = db; }
    void Resources::setEngineConfig( const EngineConfig& cfg ) { g_cfg = cfg; }
    void Resources::bindThreadDb( Db* db ) { t_db = db; }

    Json Resources::list()
    {
        return Json{ { "resources", Json::array( {
            Json{ { "uri", "fractalsql://agents" },
                  { "name", "FractalSQL agents catalog" },
                  { "description", "Static catalog: which agent to use for which problem (the sixteen recipes and five building blocks, "
                                   "with their one-line purpose and whether they need a model), so a client can pull the whole picture in one read "
                                   "instead of reconstructing it from twenty-one tool descriptions." },
                  { "mimeType", "application/json" } },
            Json{ { "uri", "fractalsql://config" },
                  { "name", "Resolved server state (non-secret)" },
                  { "description", "The module version and edition, the Valkey server version, whether a fractalsqld daemon is attached, and "
                                   "whether it reports the enterprise tier. Never a credential, token or key." },
                  { "mimeType", "application/json" } },
        } ) } };
    }

    Json Resources::templates()
    {
        return Json{ { "resourceTemplates", Json::array( {
            Json{ { "uriTemplate", "fractalsql://ledger/{kind}" },
                  { "name", "Ledger audit report" },
                  { "description", "fractal_ledger_verify(kind)'s own JSON audit report as a resource. kind=1 is the QTL chain, kind=2 the "
                                   "decision-audit chain. A read-only forensic query; needs the fractalsqld daemon." },
                  { "mimeType", "application/json" } },
            Json{ { "uriTemplate", "fractalsql://key/{key}" },
                  { "name", "Key description" },
                  { "description", "What a key holds: its type and, for a corpus or series, how many float32 values (fractal_key_info)." },
                  { "mimeType", "application/json" } },
        } ) } };
    }

    static Json wrapRead( const std::string& uri, const char* mime, const std::string& text )
    {
        return Json{ { "contents", Json::array( { Json{ { "uri", uri }, { "mimeType", mime }, { "text", text } } } ) } };
    }

    static const char* const kAgentsJson =
        "{\n"
        "  \"recipes\": [\n"
        "    {\"tool\": \"fractal_agent_anomaly_triage\", \"use\": \"Is this entity's metric series drifting? Drift plus a written triage.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_allocate\", \"use\": \"Pick a cardinality-constrained portfolio and explain it.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_route_task\", \"use\": \"Route a task to the nearest capability under a token budget.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_outlier_intercept\", \"use\": \"Block a proposed state that is too close to a known-bad one.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_recall_hybrid\", \"use\": \"Retrieve the nearest memories, optionally within a cohort.\", \"model\": false},\n"
        "    {\"tool\": \"fractal_agent_recommend_diverse\", \"use\": \"Recommend items that are relevant and mutually diverse.\", \"model\": false},\n"
        "    {\"tool\": \"fractal_agent_data_analyst\", \"use\": \"Ask a natural-language question of a valkey-search index.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_patient_deterioration_triage\", \"use\": \"Cohort search plus the drift from baseline to now.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_feedback_audit\", \"use\": \"Check that diversification is actually working.\", \"model\": false},\n"
        "    {\"tool\": \"fractal_agent_schedule_workload\", \"use\": \"Place a workload on the nearest-capability node.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_rebalance_sibling\", \"use\": \"Rebalance a portfolio against the most similar past allocation.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_detour_classify\", \"use\": \"Did this vehicle detour, and how complex is its GPS trace?\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_track_anomaly\", \"use\": \"Is this track deviating, and is its heading persistent?\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_network_coverage_alert\", \"use\": \"Sensor-grid coverage and telemetry drift.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_regime_triage\", \"use\": \"Has the regime of any series changed?\", \"model\": true},\n"
        "    {\"tool\": \"fractal_agent_diverse_portfolios\", \"use\": \"Several different near-optimal portfolios (enterprise).\", \"model\": true}\n"
        "  ],\n"
        "  \"building_blocks\": [\n"
        "    {\"tool\": \"fractal_agent_plan_explore\", \"use\": \"Branch a seed state into the nearest strategies.\", \"model\": \"embedding\"},\n"
        "    {\"tool\": \"fractal_agent_trajectory_predict\", \"use\": \"Project the next state from a state log.\", \"model\": false},\n"
        "    {\"tool\": \"fractal_agent_detect_loop\", \"use\": \"Is an agent stuck repeating itself?\", \"model\": false},\n"
        "    {\"tool\": \"fractal_search_agent\", \"use\": \"Embed, search, answer from the hits.\", \"model\": true},\n"
        "    {\"tool\": \"fractal_rag_agent\", \"use\": \"search_agent with a fixed search.\", \"model\": true}\n"
        "  ],\n"
        "  \"note\": \"Agents that use a model return the computed half plus reasoning_step_failed when the model does not answer.\"\n"
        "}\n";

    static std::string urlDecode( const std::string& s )
    {
        std::string out;
        for( std::size_t i = 0; i < s.size(); ++i )
        {
            if( s[ i ] == '%' && i + 2 < s.size() + 0 && std::isxdigit( static_cast<unsigned char>( s[ i + 1 ] ) ) && std::isxdigit( static_cast<unsigned char>( s[ i + 2 ] ) ) )
            {
                out.push_back( static_cast<char>( std::stoi( s.substr( i + 1, 2 ), nullptr, 16 ) ) );
                i += 2;
            }
            else out.push_back( s[ i ] );
        }
        return out;
    }

    Json Resources::read( const std::string& uri )
    {
        if( uri == "fractalsql://agents" ) return wrapRead( uri, "application/json", kAgentsJson );

        Db* db = currentDb();
        if( !db ) throw ToolError( ErrorClass::ServerInternal, "no engine connection bound for resources" );

        if( uri == "fractalsql://config" )
        {
            Json out{ { "plugin", kMcpPluginName }, { "plugin_version", FSQL_MCP_VERSION } };
            out[ "module_version" ] = db->moduleVersion();
            try { Resp e = db->call( { "FRACTAL.EDITION" } ); out[ "edition" ] = e.str; } catch( const ToolError& ) { out[ "edition" ] = nullptr; }
            try
            {
                Resp info = db->call( { "INFO", "server" } );
                std::size_t pos = info.str.find( "valkey_version:" );
                if( pos != std::string::npos )
                {
                    const std::size_t s = info.str.find( ':', pos ) + 1, e = info.str.find_first_of( "\r\n", s );
                    out[ "server_version" ] = info.str.substr( s, e - s );
                }
            }
            catch( const ToolError& ) {}
            // Is a daemon attached, and is the enterprise tier behind it? TRUTH.COUNT answers either way, cheaply.
            bool daemon = true, enterprise = false;
            try { db->call( { "FRACTAL.LEDGER.TRUTH.COUNT" } ); enterprise = true; }
            catch( const ToolError& e )
            {
                std::string m = e.message;
                for( char& c : m ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
                if( m.find( "not configured" ) != std::string::npos ) daemon = false;
            }
            out[ "daemon_attached" ] = daemon;
            out[ "enterprise_tier" ] = enterprise;
            out[ "server_endpoint" ] = g_cfg.socketPath.empty() ? ( g_cfg.host + ":" + std::to_string( g_cfg.port ) ) : ( "unix:" + g_cfg.socketPath );
            out[ "acl_user" ] = g_cfg.username.empty() ? Json( "default" ) : Json( g_cfg.username );
            return wrapRead( uri, "application/json", out.dump( 2 ) );
        }
        const std::string kLedger = "fractalsql://ledger/";
        if( uri.rfind( kLedger, 0 ) == 0 )
        {
            const std::string kind = uri.substr( kLedger.size() );
            if( kind != "1" && kind != "2" ) throw ToolError( ErrorClass::Schema, "ledger kind must be 1 or 2" );
            Resp r = db->call( { "FRACTAL.LEDGER.VERIFY", kind } );
            return wrapRead( uri, "application/json", r.str );
        }
        const std::string kKey = "fractalsql://key/";
        if( uri.rfind( kKey, 0 ) == 0 )
        {
            const std::string key = urlDecode( uri.substr( kKey.size() ) );
            if( key.empty() ) throw ToolError( ErrorClass::Schema, "key must not be empty" );
            Resp ty = db->call( { "TYPE", key } );
            Json out{ { "key", key }, { "type", ty.str } };
            if( ty.str == "string" )
            {
                const long long n = db->call( { "STRLEN", key } ).i;
                out[ "bytes" ] = n; out[ "float32_values" ] = n / 4;
            }
            return wrapRead( uri, "application/json", out.dump( 2 ) );
        }
        throw ToolError( ErrorClass::Schema, "unknown resource: " + uri );
    }
}
