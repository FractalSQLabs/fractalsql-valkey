// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
// buildAllTools: the full catalog assembly. The order is stable, for reproducible schema hashes.
#include "db.h"
#include "config.h"
#include "adapters/commands.h"

namespace fsqlmcp
{
    std::vector<ToolSpec> buildAllTools( Db& db, const EngineConfig&, const RowLimitConfig& rowCfg )
    {
        std::vector<ToolSpec> out = buildCommandTools( db, rowCfg );
        classifyStateMutatingTools( out );
        // The context-scoped tools take an optional context_id: the modern
        // protocol era has no protocol session, so the handle that names the
        // engine state is passed explicitly. It is added here, in one place,
        // so the schema, the manifest and the locks all describe the same
        // argument.
        for( ToolSpec& t : out )
            if( isContextScopedTool( t.name ) )
                t.params.push_back( contextIdParam() );
        return out;
    }
}
