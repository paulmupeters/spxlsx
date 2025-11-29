#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Forward declaration
struct SharepointReadBindData;

// Table function for reading SharePoint lists
void RegisterSharepointReadFunction(DatabaseInstance &db);

} // namespace duckdb