# 05 - SharePoint Lists

In this module, you'll implement the table function that reads data from SharePoint Lists. This is the core feature that lets users query SharePoint data with SQL.

## Goals
- Understand DuckDB's table function API
- Implement schema discovery (bind phase)
- Implement data retrieval (execution phase)
- Map SharePoint column types to DuckDB types
- Handle pagination and NULL values

## Overview: Table Function Pattern

DuckDB table functions have two phases:

```
1. BIND PHASE (once per query)
   ├─→ Parse input URL/parameters
   ├─→ Get authentication token
   ├─→ Fetch list schema from SharePoint
   ├─→ Map SharePoint types to DuckDB types
   └─→ Return column names and types

2. EXECUTION PHASE (repeated for data chunks)
   ├─→ Fetch list items from SharePoint
   ├─→ Parse JSON response
   ├─→ Convert values to DuckDB types
   ├─→ Fill output DataChunk
   └─→ Handle pagination if needed
```

## Step 1: Define Data Structures

Update `src/include/sharepoint_read.hpp`:

```cpp
#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Bind data: stores information discovered during bind phase
struct SharepointReadBindData : public TableFunctionData {
    std::string site_id;
    std::string list_id;
    std::string token;
    std::string response_json;  // Cached list items
    bool finished;
    idx_t row_index;

    // Schema information
    vector<LogicalType> return_types;
    vector<std::string> names;

    SharepointReadBindData() : finished(false), row_index(0) {}
};

// Register the read_sharepoint function
void RegisterSharepointReadFunction(DatabaseInstance &db);

} // namespace duckdb
```

## Step 2: Implement Bind Phase

Create the bind function in `src/sharepoint_read.cpp`:

```cpp
#include "sharepoint_read.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "sharepoint_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "third_party/json.hpp"

using json = nlohmann::json;

namespace duckdb {

// Map SharePoint field type to DuckDB LogicalType
static LogicalType MapSharepointTypeToDuckDB(const std::string &sp_type) {
    if (sp_type == "text" || sp_type == "note" || sp_type == "choice") {
        return LogicalType::VARCHAR;
    }
    else if (sp_type == "number") {
        return LogicalType::DOUBLE;
    }
    else if (sp_type == "boolean") {
        return LogicalType::BOOLEAN;
    }
    else if (sp_type == "dateTime") {
        return LogicalType::TIMESTAMP;
    }
    else if (sp_type == "lookup" || sp_type == "user") {
        return LogicalType::VARCHAR;  // Store as text for now
    }
    else if (sp_type == "currency") {
        return LogicalType::DOUBLE;
    }
    else {
        // Default to VARCHAR for unknown types
        return LogicalType::VARCHAR;
    }
}

// Helper: Extract site ID from URL
static std::string GetSiteId(const std::string &url, const std::string &token) {
    // Parse SharePoint URL
    std::string tenant = SharepointUtils::ExtractTenantFromUrl(url);
    std::string site_path = SharepointUtils::ExtractSiteUrl(url);

    if (tenant.empty()) {
        throw InvalidInputException("Invalid SharePoint URL: " + url);
    }

    // Call Graph API to get site ID
    std::ostringstream path;
    if (site_path == "/" || site_path.empty()) {
        // Root site
        path << "/v1.0/sites/" << tenant << ".sharepoint.com";
    } else {
        // Site collection
        path << "/v1.0/sites/" << tenant << ".sharepoint.com:" << site_path;
    }

    std::string response = PerformHttpsRequest(
        "graph.microsoft.com",
        path.str(),
        token,
        HttpMethod::GET
    );

    json site_data = json::parse(response);
    return site_data["id"];
}

// Helper: Get list ID by name
static std::string GetListIdByName(
    const std::string &site_id,
    const std::string &list_name,
    const std::string &token) {

    std::ostringstream path;
    path << "/v1.0/sites/" << site_id << "/lists?$filter=displayName eq '" << list_name << "'";

    std::string response = PerformHttpsRequest(
        "graph.microsoft.com",
        path.str(),
        token,
        HttpMethod::GET
    );

    json lists_data = json::parse(response);

    if (!lists_data.contains("value") || lists_data["value"].empty()) {
        throw InvalidInputException("List not found: " + list_name);
    }

    return lists_data["value"][0]["id"];
}

// Bind phase: Discover schema
static unique_ptr<FunctionData> SharepointReadBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    auto bind_data = make_uniq<SharepointReadBindData>();

    // 1. Parse input URL
    if (input.inputs.empty()) {
        throw InvalidInputException("read_sharepoint requires a SharePoint list URL");
    }

    std::string url = input.inputs[0].ToString();

    // 2. Get authentication token
    bind_data->token = SharepointAuth::GetAccessToken(context);

    // 3. Extract site and list information
    std::string list_name = SharepointUtils::ExtractListName(url);
    if (list_name.empty()) {
        throw InvalidInputException("Could not extract list name from URL: " + url);
    }

    // 4. Get site ID
    bind_data->site_id = GetSiteId(url, bind_data->token);

    // 5. Get list ID
    bind_data->list_id = GetListIdByName(bind_data->site_id, list_name, bind_data->token);

    // 6. Fetch list schema (columns)
    std::string list_metadata = GetListMetadata(
        bind_data->site_id,
        bind_data->list_id,
        bind_data->token
    );

    json metadata = json::parse(list_metadata);

    // 7. Map columns to DuckDB schema
    if (metadata.contains("columns")) {
        for (const auto &column : metadata["columns"]) {
            // Skip hidden/system columns
            if (column.value("hidden", false)) {
                continue;
            }

            std::string col_name = column["name"];
            std::string col_type = column.value("type", "text");

            // Add to schema
            names.push_back(col_name);
            LogicalType duck_type = MapSharepointTypeToDuckDB(col_type);
            return_types.push_back(duck_type);
        }
    }

    // If no columns found, use defaults
    if (names.empty()) {
        throw InvalidInputException("No columns found in list");
    }

    // Store schema in bind data
    bind_data->names = names;
    bind_data->return_types = return_types;

    // 8. Fetch list items (cache for execution phase)
    bind_data->response_json = CallGraphApiListItems(
        bind_data->site_id,
        bind_data->list_id,
        bind_data->token
    );

    return std::move(bind_data);
}

// Execution phase: Retrieve data
static void SharepointReadFunction(
    ClientContext &context,
    TableFunctionInput &data_p,
    DataChunk &output) {

    auto &bind_data = data_p.bind_data->Cast<SharepointReadBindData>();

    // Check if we're done
    if (bind_data.finished) {
        return;
    }

    // Parse JSON response
    json response = json::parse(bind_data.response_json);

    if (!response.contains("value")) {
        bind_data.finished = true;
        return;
    }

    auto items = response["value"];
    idx_t items_count = items.size();

    if (items_count == 0) {
        bind_data.finished = true;
        return;
    }

    // Process items starting from row_index
    idx_t output_idx = 0;
    idx_t max_rows = STANDARD_VECTOR_SIZE;  // DuckDB's standard chunk size

    while (bind_data.row_index < items_count && output_idx < max_rows) {
        auto &item = items[bind_data.row_index];

        // Get the fields object
        if (!item.contains("fields")) {
            bind_data.row_index++;
            continue;
        }

        auto fields = item["fields"];

        // Fill each column
        for (idx_t col_idx = 0; col_idx < bind_data.names.size(); col_idx++) {
            auto &col_name = bind_data.names[col_idx];
            auto &col_type = bind_data.return_types[col_idx];

            // Check if field exists
            if (!fields.contains(col_name)) {
                // NULL value
                FlatVector::SetNull(output.data[col_idx], output_idx, true);
                continue;
            }

            auto field_value = fields[col_name];

            // Handle NULL
            if (field_value.is_null()) {
                FlatVector::SetNull(output.data[col_idx], output_idx, true);
                continue;
            }

            // Convert based on type
            switch (col_type.id()) {
                case LogicalTypeId::VARCHAR: {
                    std::string str_value;
                    if (field_value.is_string()) {
                        str_value = field_value.get<std::string>();
                    } else {
                        str_value = field_value.dump();  // Convert to JSON string
                    }
                    FlatVector::GetData<string_t>(output.data[col_idx])[output_idx] =
                        StringVector::AddString(output.data[col_idx], str_value);
                    break;
                }
                case LogicalTypeId::DOUBLE: {
                    double num_value = field_value.get<double>();
                    FlatVector::GetData<double>(output.data[col_idx])[output_idx] = num_value;
                    break;
                }
                case LogicalTypeId::BOOLEAN: {
                    bool bool_value = field_value.get<bool>();
                    FlatVector::GetData<bool>(output.data[col_idx])[output_idx] = bool_value;
                    break;
                }
                case LogicalTypeId::TIMESTAMP: {
                    // Parse ISO 8601 timestamp
                    std::string timestamp_str = field_value.get<std::string>();
                    // Simplified parsing - in production, use proper timestamp parser
                    // For now, store as string
                    FlatVector::GetData<string_t>(output.data[col_idx])[output_idx] =
                        StringVector::AddString(output.data[col_idx], timestamp_str);
                    break;
                }
                default:
                    // Unsupported type - store as NULL
                    FlatVector::SetNull(output.data[col_idx], output_idx, true);
                    break;
            }
        }

        bind_data.row_index++;
        output_idx++;
    }

    // Set output size
    output.SetCardinality(output_idx);

    // Check if we're finished
    if (bind_data.row_index >= items_count) {
        bind_data.finished = true;
    }
}

// Register the function
void RegisterSharepointReadFunction(DatabaseInstance &db) {
    // Create table function
    TableFunction read_sharepoint_func(
        "read_sharepoint",                     // Function name
        {LogicalType::VARCHAR},                // Input: URL string
        SharepointReadFunction,                // Execution function
        SharepointReadBind                     // Bind function
    );

    // Optional parameters
    read_sharepoint_func.named_parameters["filter"] = LogicalType::VARCHAR;
    read_sharepoint_func.named_parameters["top"] = LogicalType::INTEGER;

    // Register with DuckDB
    ExtensionUtil::RegisterFunction(db, read_sharepoint_func);
}

} // namespace duckdb
```

## Step 3: Update Extension Entry Point

Update `src/sharepoint_extension.cpp` to register the read function:

```cpp
#include "sharepoint_read.hpp"

static void LoadInternal(DatabaseInstance &instance) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Register authentication
    RegisterSharepointAuthFunctions(instance);

    // Register table function
    RegisterSharepointReadFunction(instance);
}
```

## Step 4: Test Reading a SharePoint List

Build and test:

```bash
make

cd duckdb
./build/release/duckdb
```

In DuckDB:

```sql
-- Load extension
LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension';

-- Create authentication secret
CREATE SECRET sharepoint_secret (TYPE sharepoint, PROVIDER oauth);

-- Query a SharePoint list
SELECT *
FROM read_sharepoint('https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees');

-- Filter and aggregate
SELECT Department, COUNT(*) as employee_count
FROM read_sharepoint('https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees')
GROUP BY Department
ORDER BY employee_count DESC;

-- Join with local data
CREATE TABLE departments AS SELECT 'Engineering' as name, 100 as budget UNION ALL SELECT 'Sales', 75;

SELECT d.name, d.budget, COUNT(e.*) as headcount
FROM departments d
LEFT JOIN read_sharepoint('https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees') e
  ON d.name = e.Department
GROUP BY d.name, d.budget;
```

## Step 5: Handle Pagination

SharePoint returns results in pages. Update the bind function to handle `@odata.nextLink`:

```cpp
// In SharepointReadBind, after fetching initial items:
json response = json::parse(bind_data->response_json);

// Check for pagination
while (response.contains("@odata.nextLink")) {
    std::string next_url = response["@odata.nextLink"];

    // Extract path from full URL
    size_t path_start = next_url.find("/v1.0/");
    std::string next_path = next_url.substr(path_start);

    // Fetch next page
    std::string next_response = PerformHttpsRequest(
        "graph.microsoft.com",
        next_path,
        bind_data->token,
        HttpMethod::GET
    );

    json next_page = json::parse(next_response);

    // Append items
    if (next_page.contains("value")) {
        response["value"].insert(
            response["value"].end(),
            next_page["value"].begin(),
            next_page["value"].end()
        );
    }

    response = next_page;
}
```

## Step 6: Type Mapping Reference

| SharePoint Type | DuckDB Type | Notes |
|----------------|-------------|-------|
| `text` | `VARCHAR` | Single line of text |
| `note` | `VARCHAR` | Multiple lines of text |
| `number` | `DOUBLE` | Numeric values |
| `boolean` | `BOOLEAN` | Yes/No |
| `dateTime` | `TIMESTAMP` | Date and time |
| `choice` | `VARCHAR` | Drop-down choices |
| `lookup` | `VARCHAR` | Reference to another list |
| `user` | `VARCHAR` | Person or group |
| `currency` | `DOUBLE` | Money values |
| `calculated` | `VARCHAR` | Computed values |
| `url` | `VARCHAR` | Hyperlink |

## Step 7: Handle Complex Field Types

Some SharePoint fields have complex structures:

### Lookup Fields

```cpp
if (field_value.is_object() && field_value.contains("LookupValue")) {
    str_value = field_value["LookupValue"].get<std::string>();
}
```

### Person Fields

```cpp
if (field_value.is_object() && field_value.contains("Email")) {
    str_value = field_value["Email"].get<std::string>();
}
```

### Multi-value Fields

```cpp
if (field_value.is_array()) {
    std::ostringstream ss;
    for (size_t i = 0; i < field_value.size(); i++) {
        if (i > 0) ss << "; ";
        ss << field_value[i].get<std::string>();
    }
    str_value = ss.str();
}
```

## Step 8: Add Optional Parameters

Support filtering and limiting results:

```cpp
// In SharepointReadBind:
auto filter_param = input.named_parameters.find("filter");
std::string filter = "";
if (filter_param != input.named_parameters.end()) {
    filter = filter_param->second.ToString();
}

auto top_param = input.named_parameters.find("top");
int top = 0;
if (top_param != input.named_parameters.end()) {
    top = top_param->second.GetValue<int>();
}

// Pass to API call
bind_data->response_json = CallGraphApiListItems(
    bind_data->site_id,
    bind_data->list_id,
    bind_data->token,
    "",      // select fields
    filter,  // OData filter
    top      // limit
);
```

Use it:

```sql
-- Get only active employees
SELECT *
FROM read_sharepoint(
    'https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees',
    filter := 'Status eq ''Active'''
);

-- Get top 10 most recent items
SELECT *
FROM read_sharepoint(
    'https://contoso.sharepoint.com/sites/TeamSite/Lists/Orders',
    top := 10
);
```

## Common Issues

### Issue: "List not found"

**Solution**: Check the list name in the URL. SharePoint URLs are case-sensitive.

### Issue: "No columns found"

**Solution**: The list might be empty or have only system columns. Add some data to the list and try again.

### Issue: Type conversion errors

**Solution**: SharePoint's type system doesn't always match expectations. Default to `VARCHAR` for problematic fields.

### Issue: Performance with large lists

**Solution**: Implement server-side filtering and pagination properly to avoid loading all items at once.

## Optimization Tips

1. **Lazy loading**: Only fetch data when needed, not during bind phase
2. **Column selection**: Use `$select` to fetch only requested columns
3. **Server-side filtering**: Push down WHERE clauses to SharePoint
4. **Parallel loading**: Fetch multiple pages concurrently
5. **Caching**: Cache schema information to avoid repeated metadata calls

## What You've Accomplished

You now have a working DuckDB extension that:
- Reads SharePoint Lists
- Discovers schema automatically
- Maps SharePoint types to DuckDB types
- Handles pagination
- Supports filtering and limiting
- Integrates with DuckDB's query engine

Users can now query SharePoint data with SQL, join it with local tables, and use all of DuckDB's powerful analytics features!

---

**Navigation:**
- Previous: [04 - Authentication](./04-authentication.md)
- Next: [06 - Document Libraries](./06-document-libraries.md)
