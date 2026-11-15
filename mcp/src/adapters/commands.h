// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#pragma once
#include <vector>
#include <functional>
#include <map>
#include "../tool_registry.h"
#include "../rows.h"
#include "../db.h"
#include "../config.h"

namespace fsqlmcp
{
    // The whole tool catalog: every FractalSQL command, five keyspace helpers and the version tool.
    std::vector<ToolSpec> buildCommandTools( Db& db, const RowLimitConfig& rowCfg );
}
