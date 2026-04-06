#include "sharepoint_read.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "sharepoint_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <ctime>
#include <iomanip>

using json = nlohmann::json;

namespace duckdb {

// ============================================================================
// Type Mapping: SharePoint -> DuckDB
// ============================================================================

static LogicalType MapSharepointTypeToDuckDB(const std::string &sp_type) {
	if (sp_type == "text" || sp_type == "note" || sp_type == "choice") {
		return LogicalType::VARCHAR;
	} else if (sp_type == "number") {
		return LogicalType::DOUBLE;
	} else if (sp_type == "boolean") {
		return LogicalType::BOOLEAN;
	} else if (sp_type == "dateTime") {
		return LogicalType::TIMESTAMP;
	} else if (sp_type == "lookup" || sp_type == "user") {
		return LogicalType::VARCHAR; // Basic string extraction
	} else if (sp_type == "currency") {
		return LogicalType::DOUBLE;
	} else {
		// Default to VARCHAR for unknown types
		return LogicalType::VARCHAR;
	}
}

// ============================================================================
// Helper: Extract value from complex field types as string
// ============================================================================

static std::string ExtractFieldValueAsString(const json &field_value) {
	if (field_value.is_string()) {
		return field_value.get<std::string>();
	} else if (field_value.is_number()) {
		return std::to_string(field_value.get<double>());
	} else if (field_value.is_boolean()) {
		return field_value.get<bool>() ? "true" : "false";
	} else if (field_value.is_object()) {
		// Lookup field
		if (field_value.contains("LookupValue")) {
			return field_value["LookupValue"].get<std::string>();
		}
		// Person field
		if (field_value.contains("Email")) {
			return field_value["Email"].get<std::string>();
		}
		if (field_value.contains("Title")) {
			return field_value["Title"].get<std::string>();
		}
		// Fallback: dump as JSON
		return field_value.dump();
	} else if (field_value.is_array()) {
		// Multi-value field
		std::ostringstream ss;
		for (size_t i = 0; i < field_value.size(); i++) {
			if (i > 0)
				ss << "; ";
			ss << ExtractFieldValueAsString(field_value[i]);
		}
		return ss.str();
	}
	return field_value.dump();
}

// ============================================================================
// Helper: Get Site ID from SharePoint URL
// ============================================================================

static std::string GetSiteId(const std::string &url, const std::string &token) {
	std::string tenant = SharepointUtils::ExtractTenantFromUrl(url);
	std::string site_path = SharepointUtils::ExtractSiteUrl(url);

	if (tenant.empty()) {
		throw InvalidInputException("Invalid SharePoint URL - could not extract tenant: " + url);
	}

	// Build Graph API path
	std::ostringstream path;
	if (site_path.empty() || site_path == "/") {
		// Root site
		path << "/v1.0/sites/" << tenant << ".sharepoint.com";
	} else {
		// Site collection - format: /v1.0/sites/{hostname}:/{site-path}
		path << "/v1.0/sites/" << tenant << ".sharepoint.com:" << site_path;
	}

	std::string response = PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);

	json site_data = json::parse(response);

	if (!site_data.contains("id")) {
		throw InvalidInputException("Could not get site ID from SharePoint. Response: " + response);
	}

	return site_data["id"].get<std::string>();
}

// ============================================================================
// Helper: Get List ID by name
// ============================================================================

static std::string GetListIdByName(const std::string &site_id, const std::string &list_name, const std::string &token) {
	// Build the filter expression: displayName eq 'listname'
	// The single quotes and the list name need to be URL-encoded in the query string
	std::ostringstream filter;
	filter << "displayName eq '" << list_name << "'";

	std::ostringstream path;
	path << "/v1.0/sites/" << site_id << "/lists?$filter=" << SharepointUtils::UrlEncode(filter.str());

	std::string response = PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);

	json lists_data = json::parse(response);

	if (!lists_data.contains("value") || lists_data["value"].empty()) {
		throw InvalidInputException("List not found: '" + list_name +
		                            "'. Check that the list name is correct and you have access.");
	}

	return lists_data["value"][0]["id"].get<std::string>();
}

// ============================================================================
// Bind Phase: Discover schema
// ============================================================================

static unique_ptr<FunctionData> SharepointReadBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<SharepointReadBindData>();

	// 1. Parse input URL
	if (input.inputs.empty()) {
		throw InvalidInputException("read_sharepoint_list requires a SharePoint list URL");
	}
	std::string url = input.inputs[0].ToString();

	// 2. Get optional parameters
	auto filter_param = input.named_parameters.find("filter");
	if (filter_param != input.named_parameters.end()) {
		bind_data->filter = filter_param->second.ToString();
	}

	auto top_param = input.named_parameters.find("top");
	if (top_param != input.named_parameters.end()) {
		bind_data->top = top_param->second.GetValue<int>();
	}

	// 3. Get authentication token
	bind_data->token = SharepointAuth::GetAccessToken(context);

	// 4. Extract list name from URL
	std::string list_name = SharepointUtils::ExtractListName(url);
	if (list_name.empty()) {
		throw InvalidInputException("Could not extract list name from URL: " + url +
		                            "\nExpected format: https://tenant.sharepoint.com/sites/SiteName/Lists/ListName");
	}

	// 5. Get site ID
	bind_data->site_id = GetSiteId(url, bind_data->token);

	// 6. Get list ID
	bind_data->list_id = GetListIdByName(bind_data->site_id, list_name, bind_data->token);

	// 7. Fetch list schema (columns)
	std::string list_metadata = GetListMetadata(bind_data->site_id, bind_data->list_id, bind_data->token);

	json metadata = json::parse(list_metadata);

	// 8. Map columns to DuckDB schema
	if (metadata.contains("columns")) {
		for (const auto &column : metadata["columns"]) {
			// Skip hidden/read-only system columns
			if (column.value("hidden", false)) {
				continue;
			}
			if (column.value("readOnly", false)) {
				// Skip some read-only columns but keep useful ones
				std::string col_name = column.value("name", "");
				if (col_name != "Title" && col_name != "Created" && col_name != "Modified") {
					continue;
				}
			}

			std::string col_name = column.value("name", "");
			if (col_name.empty())
				continue;

			// Get column type from SharePoint
			std::string col_type = "text";
			if (column.contains("text")) {
				col_type = "text";
			} else if (column.contains("number")) {
				col_type = "number";
			} else if (column.contains("boolean")) {
				col_type = "boolean";
			} else if (column.contains("dateTime")) {
				col_type = "dateTime";
			} else if (column.contains("lookup")) {
				col_type = "lookup";
			} else if (column.contains("personOrGroup")) {
				col_type = "user";
			} else if (column.contains("currency")) {
				col_type = "currency";
			} else if (column.contains("choice")) {
				col_type = "choice";
			}

			names.push_back(col_name);
			return_types.push_back(MapSharepointTypeToDuckDB(col_type));
		}
	}

	// If no columns found, add at least a Title column
	if (names.empty()) {
		names.push_back("Title");
		return_types.push_back(LogicalType::VARCHAR);
	}

	// Store schema in bind data
	bind_data->names = names;
	bind_data->return_types = return_types;

	return std::move(bind_data);
}

// ============================================================================
// Init Phase: Create global state for execution
// ============================================================================

static unique_ptr<GlobalTableFunctionState> SharepointReadInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<SharepointReadGlobalState>();
}

// ============================================================================
// Execution Phase: Retrieve data with lazy pagination
// ============================================================================

static void SharepointReadFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<SharepointReadBindData>();
	auto &state = data_p.global_state->Cast<SharepointReadGlobalState>();

	// Check if we're done
	if (state.finished) {
		return;
	}

	// Lazy load: fetch data on first execution or when we need next page
	if (state.response_json.empty()) {
		std::cerr << "[DEBUG] Fetching list items..." << std::endl;
		state.response_json = CallGraphApiListItems(bind_data.site_id, bind_data.list_id, bind_data.token,
		                                            "", // select all fields
		                                            bind_data.filter, bind_data.top);
		std::cerr << "[DEBUG] List items response (first 500 chars): " << state.response_json.substr(0, 500)
		          << std::endl;
		state.row_index = 0;
	}

	// Parse JSON response
	json response = json::parse(state.response_json);

	if (!response.contains("value")) {
		state.finished = true;
		return;
	}

	auto items = response["value"];
	idx_t items_count = items.size();

	if (items_count == 0) {
		state.finished = true;
		return;
	}

	// Process items starting from row_index
	idx_t output_idx = 0;
	idx_t max_rows = STANDARD_VECTOR_SIZE;

	while (state.row_index < items_count && output_idx < max_rows) {
		auto &item = items[state.row_index];

		// Get the fields object
		if (!item.contains("fields")) {
			state.row_index++;
			continue;
		}

		auto fields = item["fields"];

		// Fill each column
		for (idx_t col_idx = 0; col_idx < bind_data.names.size(); col_idx++) {
			auto &col_name = bind_data.names[col_idx];
			auto &col_type = bind_data.return_types[col_idx];

			// Check if field exists
			if (!fields.contains(col_name) || fields[col_name].is_null()) {
				FlatVector::SetNull(output.data[col_idx], output_idx, true);
				continue;
			}

			auto field_value = fields[col_name];

			// Convert based on type
			switch (col_type.id()) {
			case LogicalTypeId::VARCHAR: {
				std::string str_value = ExtractFieldValueAsString(field_value);
				FlatVector::GetData<string_t>(output.data[col_idx])[output_idx] =
				    StringVector::AddString(output.data[col_idx], str_value);
				break;
			}
			case LogicalTypeId::DOUBLE: {
				if (field_value.is_number()) {
					double num_value = field_value.get<double>();
					FlatVector::GetData<double>(output.data[col_idx])[output_idx] = num_value;
				} else {
					FlatVector::SetNull(output.data[col_idx], output_idx, true);
				}
				break;
			}
			case LogicalTypeId::BOOLEAN: {
				if (field_value.is_boolean()) {
					bool bool_value = field_value.get<bool>();
					FlatVector::GetData<bool>(output.data[col_idx])[output_idx] = bool_value;
				} else {
					FlatVector::SetNull(output.data[col_idx], output_idx, true);
				}
				break;
			}
			case LogicalTypeId::TIMESTAMP: {
				// Parse ISO 8601 timestamp (e.g., "2025-12-05T11:08:41Z")
				if (field_value.is_string()) {
					std::string timestamp_str = field_value.get<std::string>();
					try {
						// Try to parse as ISO 8601 timestamp
						// Second parameter: use_offset = true to handle timezone info
						timestamp_t ts = Timestamp::FromString(timestamp_str, true);
						FlatVector::GetData<timestamp_t>(output.data[col_idx])[output_idx] = ts;
					} catch (...) {
						FlatVector::SetNull(output.data[col_idx], output_idx, true);
					}
				} else {
					FlatVector::SetNull(output.data[col_idx], output_idx, true);
				}
				break;
			}
			default:
				FlatVector::SetNull(output.data[col_idx], output_idx, true);
				break;
			}
		}

		state.row_index++;
		output_idx++;
	}

	// Set output size
	output.SetCardinality(output_idx);

	// Check if we need to paginate or we're finished
	if (state.row_index >= items_count) {
		// Check for next page
		if (response.contains("@odata.nextLink")) {
			std::string next_url = response["@odata.nextLink"].get<std::string>();

			// Extract path from full URL
			size_t path_start = next_url.find("/v1.0/");
			if (path_start != std::string::npos) {
				std::string next_path = next_url.substr(path_start);

				// Fetch next page
				state.response_json =
				    PerformHttpsRequest("graph.microsoft.com", next_path, bind_data.token, HttpMethod::GET);
				state.row_index = 0;
			} else {
				state.finished = true;
			}
		} else {
			state.finished = true;
		}
	}
}

// ============================================================================
// Register Function
// ============================================================================

static TableFunction CreateSharepointListFunction(const std::string &function_name) {
	TableFunction sharepoint_list_func(function_name,          // Function name
	                                   {LogicalType::VARCHAR}, // Input: URL string
	                                   SharepointReadFunction, // Execution function
	                                   SharepointReadBind,     // Bind function
	                                   SharepointReadInit      // Init function (creates global state)
	);

	sharepoint_list_func.named_parameters["filter"] = LogicalType::VARCHAR;
	sharepoint_list_func.named_parameters["top"] = LogicalType::INTEGER;

	return sharepoint_list_func;
}

void RegisterSharepointReadFunction(ExtensionLoader &loader) {
	// Register the canonical name first, then keep the legacy alias for compatibility.
	loader.RegisterFunction(CreateSharepointListFunction("read_sharepoint_list"));
	loader.RegisterFunction(CreateSharepointListFunction("read_sharepoint"));
}

} // namespace duckdb
