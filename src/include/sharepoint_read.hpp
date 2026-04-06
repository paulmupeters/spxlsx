#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <string>
#include <vector>

namespace duckdb {

// Bind data: stores immutable information discovered during bind phase
struct SharepointReadBindData : public TableFunctionData {
	// SharePoint identifiers
	std::string site_id;
	std::string list_id;
	std::string token;

	// Schema information
	vector<LogicalType> return_types;
	vector<std::string> names;

	// Optional query parameters
	std::string filter;
	int top;

	SharepointReadBindData() : top(0) {
	}
};

// Global state: stores mutable execution state
struct SharepointReadGlobalState : public GlobalTableFunctionState {
	// Cached response and pagination
	std::string response_json;
	std::string next_link; // For lazy pagination

	// Execution state
	bool finished;
	idx_t row_index;

	SharepointReadGlobalState() : finished(false), row_index(0) {
	}
};

// Table function for reading SharePoint lists
void RegisterSharepointReadFunction(ExtensionLoader &loader);

} // namespace duckdb
