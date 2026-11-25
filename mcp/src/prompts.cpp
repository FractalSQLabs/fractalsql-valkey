#include "prompts.h"
#include "errors.h"
#include <vector>
#include <string>

namespace fsqlmcp
{
    struct PromptArg { const char* name; const char* desc; bool required; };
    struct PromptDef { const char* name; const char* desc; std::vector<PromptArg> args; const char* templateStr; };

    static const char* const kLeastPriv =
        "\n\nSafety: connect this server as a least-privilege Valkey ACL user (key patterns and command categories). "
        "This server enforces nothing beyond that user's ACL, and an agent that sends data to a model is in the "
        "@dangerous category.";

    static const std::vector<PromptDef> kPrompts{
        { "incident_triage", "DevOps/SRE: triage drift on one entity's metric series.",
          { {"series_key","Key holding the series (packed float32, oldest first).",true}, {"baseline_window","Baseline window (default 32).",false} },
          "Call fractal_agent_anomaly_triage(series_key={series_key}, baseline_window={baseline_window}). If it flags an anomaly, "
          "consider fractal_agent_outlier_intercept against a corpus of known-bad states." },
        { "portfolio_rebalance", "FinTech: rebalance a portfolio against its own history.",
          { {"mu","Expected returns.",true}, {"cov","Covariance matrix.",true}, {"cardinality","Max nonzero assets.",true}, {"alloc_key","Key of past allocations.",true} },
          "Call fractal_agent_rebalance_sibling(alloc_key={alloc_key}, mu={mu}, cov={cov}, cardinality={cardinality}, baseline=<current weights>). "
          "For several candidate portfolios also try fractal_agent_diverse_portfolios (Enterprise)." },
        { "patient_triage", "MedTech: cohort-restricted patient deterioration triage.",
          { {"corpus_key","Cohort corpus key.",true}, {"query","Query vector.",true}, {"baseline","Intake vector.",true}, {"current","Present vector.",true} },
          "Call fractal_agent_patient_deterioration_triage(corpus_key={corpus_key}, query={query}, baseline={baseline}, current={current})." },
        { "fleet_detour_check", "Fleet logistics: classify a vehicle detour.",
          { {"corpus_key","Fleet deviation corpus key.",true}, {"baseline","Baseline route vector.",true}, {"current","Current route vector.",true}, {"gps_trace_key","Key of the flattened GPS trace.",true} },
          "Call fractal_agent_detour_classify(corpus_key={corpus_key}, baseline={baseline}, current={current}, gps_trace_key={gps_trace_key})." },
        { "track_anomaly_check", "Maritime/cybersecurity: track-deviation or beaconing check.",
          { {"corpus_key","Fleet signature corpus key.",true}, {"baseline","Baseline state.",true}, {"current","Current state.",true}, {"heading_series_key","Key of the heading-change series.",true} },
          "Call fractal_agent_track_anomaly(corpus_key={corpus_key}, baseline={baseline}, current={current}, heading_series_key={heading_series_key})." },
        { "sensor_grid_health", "Smart cities: sensor-grid coverage alert.",
          { {"point_cloud_key","Key of the sensor coordinates.",true}, {"drift_series_key","Key of the telemetry series.",true} },
          "Call fractal_agent_network_coverage_alert(point_cloud_key={point_cloud_key}, drift_series_key={drift_series_key})." },
        { "churn_recovery", "Customer support: recall plus diverse recommendations for a churn-risk customer.",
          { {"memory_key","Customer-memory corpus key.",true}, {"catalog_key","Catalog corpus key.",true}, {"query","The customer's profile vector.",true} },
          "Call fractal_agent_recall_hybrid(corpus_key={memory_key}, query={query}) for the history, then "
          "fractal_agent_recommend_diverse(catalog_key={catalog_key}, query={query})." },
        { "ask_my_data", "Ask a natural-language question of your hashes (a valkey-search index, or a key prefix when valkey-search is not loaded).",
          { {"index","The valkey-search index name, or the key prefix to scan.",true}, {"question","Natural-language question.",true} },
          "Call fractal_agent_data_analyst(index={index}, question={question}). It uses valkey-search when loaded, otherwise scans the keys under the prefix; it needs a model." },
        { "diversity_audit", "Self-contained diversify health-check cycle.",
          { {"catalog_key","Catalog corpus key.",true}, {"warmup_key","Warmup corpus key.",true}, {"query","The audited query vector.",true} },
          "Call fractal_agent_feedback_audit(catalog_key={catalog_key}, warmup_key={warmup_key}, query={query})." },
        { "ciso_ledger_check", "Enterprise: periodic ledger-chain integrity audit.",
          { {"kind","1 (QTL) or 2 (decision audit).",true} },
          "Call fractal_ledger_verify(kind={kind}) and report ok / rows_verified, or the first failure and its reason." }
    };

    Json Prompts::list()
    {
        Json arr = Json::array();
        for( const PromptDef& p : kPrompts )
        {
            Json args = Json::array();
            for( const PromptArg& a : p.args )
                args.push_back( Json{ {"name",a.name}, {"description",a.desc}, {"required",a.required} } );
            arr.push_back( Json{ {"name",p.name}, {"description",p.desc}, {"arguments",args} } );
        }
        return Json{ { "prompts", arr } };
    }

    Json Prompts::get( const std::string& name, const Json& args )
    {
        for( const PromptDef& p : kPrompts )
        {
            if( p.name != name ) continue;
            std::string filled = p.templateStr;
            for( const PromptArg& a : p.args )
            {
                const std::string placeholder = "{" + std::string( a.name ) + "}";
                std::string value;
                if( args.contains( a.name ) && !args.at( a.name ).is_null() )
                    value = args.at( a.name ).is_string()
                        ? args.at( a.name ).get<std::string>()
                        : args.at( a.name ).dump();
                else if( a.required )
                    throw ToolError( ErrorClass::Schema, std::string( "missing required prompt argument: " ) + a.name );
                else
                    value = "(not provided)";
                // Caller-supplied text is DATA, not instructions: it is rendered as a JSON string literal
                // (quoted, with newlines/quotes/control characters escaped) so it cannot break out of its
                // slot or pose as a new instruction line inside the user message.
                if( args.contains( a.name ) && !args.at( a.name ).is_null() )
                    value = Json( value ).dump( -1, ' ', false, Json::error_handler_t::replace );
                // Substitute in ONE left-to-right pass over the original
                // template. The obvious "replace every occurrence, then
                // look again" loop never terminates when the argument's
                // own value contains the placeholder -- a caller that
                // passes "{sql}" as the value of the "sql" argument makes
                // the server spin forever on a request that is supposed
                // to be cheap. A single pass cannot re-scan what it just
                // inserted, so the inserted value is never a candidate.
                std::string out;
                std::size_t pos = 0;
                while( true )
                {
                    const std::size_t hit = filled.find( placeholder, pos );
                    if( hit == std::string::npos ) break;
                    out.append( filled, pos, hit - pos );
                    out += value;
                    pos = hit + placeholder.size();
                }
                out.append( filled, pos, std::string::npos );
                filled.swap( out );
            }
            if( name == "incident_triage" || name == "ask_my_data" )
                filled += kLeastPriv;
            return Json{ { "description", p.desc },
                { "messages", Json::array( { Json{ { "role", "user" }, { "content", { { "type", "text" }, { "text", filled } } } } } ) } };
        }
        throw ToolError( ErrorClass::Schema, "unknown prompt: " + name );
    }
}