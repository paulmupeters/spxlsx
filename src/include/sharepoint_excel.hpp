#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

// Register SharePoint Excel integration functions
void RegisterSharepointExcelFunction(ExtensionLoader &loader);

} // namespace duckdb
