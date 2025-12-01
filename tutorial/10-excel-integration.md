# 10 - Excel File Integration

In this module, you'll learn how to read Excel files stored in SharePoint Document Libraries by combining your SharePoint extension with DuckDB's built-in Excel extension.

## Goals
- Understand the integration approach
- Download Excel files from SharePoint
- Create a convenience function for Excel files
- Handle temporary file management
- Optimize for performance

## Why This Is Powerful

Many organizations store Excel files in SharePoint. Being able to query them directly with SQL is incredibly useful:

```sql
-- Query an Excel file in SharePoint as if it were a database table
SELECT * FROM read_sharepoint_excel(
    'https://contoso.sharepoint.com/sites/Finance/Documents/Budget2024.xlsx',
    sheet := 'Q1 Data'
);
```

## Step 1: Understanding the Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     User Query                          │
│  SELECT * FROM read_sharepoint_excel('url', ...)        │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│            Your SharePoint Extension                     │
│  1. Authenticate with Azure AD                          │
│  2. Get file download URL from Graph API                │
│  3. Download file with authentication                   │
│  4. Save to temporary file                              │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│            DuckDB Excel Extension                        │
│  5. Read Excel file from temp location                  │
│  6. Parse sheets and data                               │
│  7. Return results                                      │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│               Clean up temp file                         │
└─────────────────────────────────────────────────────────┘
```

## Step 2: Add File Download Function

First, add a function to download file contents. Update `src/sharepoint_requests.cpp`:

```cpp
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>

namespace duckdb {

// Download file content to local path
void DownloadSharepointFile(
    const std::string &site_id,
    const std::string &drive_id,
    const std::string &item_id,
    const std::string &local_path,
    const std::string &token) {

    // Get download URL
    std::ostringstream path;
    path << "/v1.0/sites/" << site_id
         << "/drives/" << drive_id
         << "/items/" << item_id
         << "/content";

    // Download content
    std::string content = PerformHttpsRequest(
        "graph.microsoft.com",
        path.str(),
        token,
        HttpMethod::GET
    );

    // Write to file
    std::ofstream outfile(local_path, std::ios::binary);
    if (!outfile) {
        throw IOException("Failed to create temporary file: " + local_path);
    }

    outfile.write(content.c_str(), content.size());
    outfile.close();

    if (outfile.fail()) {
        throw IOException("Failed to write file content to: " + local_path);
    }
}

// Alternative: Download by direct URL (some SharePoint files expose direct download links)
void DownloadFileByUrl(
    const std::string &download_url,
    const std::string &local_path,
    const std::string &token) {

    // Parse URL to extract host and path
    size_t protocol_end = download_url.find("://");
    if (protocol_end == std::string::npos) {
        throw InvalidInputException("Invalid URL: " + download_url);
    }

    size_t host_start = protocol_end + 3;
    size_t host_end = download_url.find("/", host_start);
    std::string host = download_url.substr(host_start, host_end - host_start);
    std::string path = download_url.substr(host_end);

    // Download
    std::string content = PerformHttpsRequest(host, path, token, HttpMethod::GET);

    // Write to file
    std::ofstream outfile(local_path, std::ios::binary);
    if (!outfile) {
        throw IOException("Failed to create temporary file: " + local_path);
    }

    outfile.write(content.c_str(), content.size());
    outfile.close();
}

} // namespace duckdb
```

Update `src/include/sharepoint_requests.hpp`:

```cpp
// Download SharePoint file to local path
void DownloadSharepointFile(
    const std::string &site_id,
    const std::string &drive_id,
    const std::string &item_id,
    const std::string &local_path,
    const std::string &token
);

void DownloadFileByUrl(
    const std::string &download_url,
    const std::string &local_path,
    const std::string &token
);
```

## Step 3: Create Excel Integration Function

Create `src/sharepoint_excel.cpp`:

```cpp
#include "sharepoint_excel.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "sharepoint_utils.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/file_system.hpp"
#include <nlohmann/json.hpp>

#include <random>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace duckdb {

// Generate temporary file path
static std::string GenerateTempPath(const std::string &filename) {
    // Get system temp directory
    const char* temp_dir = std::getenv("TMPDIR");
    if (!temp_dir) {
        temp_dir = std::getenv("TEMP");
    }
    if (!temp_dir) {
        temp_dir = "/tmp";
    }

    // Generate unique filename
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(10000, 99999);

    std::ostringstream path;
    path << temp_dir << "/duckdb_sharepoint_"
         << dis(gen) << "_" << filename;

    return path.str();
}

// Helper: Extract file ID and drive ID from SharePoint URL
struct SharePointFileInfo {
    std::string site_id;
    std::string drive_id;
    std::string item_id;
    std::string filename;
};

static SharePointFileInfo GetFileInfo(
    const std::string &url,
    const std::string &token) {

    SharePointFileInfo info;

    // Extract site
    std::string site_url = SharepointUtils::ExtractSiteUrl(url);
    std::string tenant = SharepointUtils::ExtractTenantFromUrl(url);

    // Get site ID
    std::ostringstream site_path;
    if (site_url == "/" || site_url.empty()) {
        site_path << "/v1.0/sites/" << tenant << ".sharepoint.com";
    } else {
        site_path << "/v1.0/sites/" << tenant << ".sharepoint.com:" << site_url;
    }

    std::string site_response = PerformHttpsRequest(
        "graph.microsoft.com",
        site_path.str(),
        token,
        HttpMethod::GET
    );

    json site_data = json::parse(site_response);
    info.site_id = site_data["id"];

    // Extract filename from URL
    size_t last_slash = url.find_last_of('/');
    if (last_slash != std::string::npos) {
        info.filename = url.substr(last_slash + 1);
    } else {
        info.filename = "file.xlsx";
    }

    // Get file by path
    // Extract the path after the site URL
    size_t docs_pos = url.find("/Shared%20Documents/");
    if (docs_pos == std::string::npos) {
        docs_pos = url.find("/Documents/");
    }

    if (docs_pos == std::string::npos) {
        throw InvalidInputException("Could not parse file path from URL");
    }

    // Get the relative path to the file
    std::string file_path = url.substr(docs_pos);

    // Get drives
    std::ostringstream drives_path;
    drives_path << "/v1.0/sites/" << info.site_id << "/drives";

    std::string drives_response = PerformHttpsRequest(
        "graph.microsoft.com",
        drives_path.str(),
        token,
        HttpMethod::GET
    );

    json drives_data = json::parse(drives_response);
    if (drives_data.contains("value") && !drives_data["value"].empty()) {
        // Use first drive (typically "Documents")
        info.drive_id = drives_data["value"][0]["id"];
    } else {
        throw IOException("No drives found in SharePoint site");
    }

    // Get item by path
    std::ostringstream item_path;
    item_path << "/v1.0/sites/" << info.site_id
              << "/drives/" << info.drive_id
              << "/root:" << file_path;

    std::string item_response = PerformHttpsRequest(
        "graph.microsoft.com",
        item_path.str(),
        token,
        HttpMethod::GET
    );

    json item_data = json::parse(item_response);
    info.item_id = item_data["id"];

    return info;
}

// Bind data for Excel function
struct SharepointExcelBindData : public TableFunctionData {
    std::string temp_file_path;
    bool cleanup_needed;

    SharepointExcelBindData() : cleanup_needed(false) {}

    ~SharepointExcelBindData() {
        // Clean up temp file
        if (cleanup_needed && !temp_file_path.empty()) {
            std::remove(temp_file_path.c_str());
        }
    }
};

// Bind phase: Download Excel file and prepare for reading
static unique_ptr<FunctionData> SharepointExcelBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    auto bind_data = make_uniq<SharepointExcelBindData>();

    // 1. Parse URL
    if (input.inputs.empty()) {
        throw InvalidInputException("read_sharepoint_excel requires a SharePoint file URL");
    }

    std::string url = input.inputs[0].ToString();

    // Verify it's an Excel file
    if (url.find(".xlsx") == std::string::npos &&
        url.find(".xlsm") == std::string::npos) {
        throw InvalidInputException(
            "File must be an Excel file (.xlsx or .xlsm). "
            "Note: .xls files are not supported."
        );
    }

    // 2. Get authentication token
    std::string token = SharepointAuth::GetAccessToken(context);

    // 3. Get file information
    SharePointFileInfo file_info = GetFileInfo(url, token);

    // 4. Generate temp file path
    bind_data->temp_file_path = GenerateTempPath(file_info.filename);
    bind_data->cleanup_needed = true;

    // 5. Download file
    DownloadSharepointFile(
        file_info.site_id,
        file_info.drive_id,
        file_info.item_id,
        bind_data->temp_file_path,
        token
    );

    // 6. Now use DuckDB's Excel extension to read the file
    // We need to call the Excel extension's bind function
    // This is done by creating a replacement scan

    // For now, we'll set up a simple schema
    // The actual Excel reading will be done by calling read_xlsx in execute
    names.push_back("_temp_path");
    return_types.push_back(LogicalType::VARCHAR);

    return std::move(bind_data);
}

// Execution phase: Delegate to Excel extension
static void SharepointExcelFunction(
    ClientContext &context,
    TableFunctionInput &data_p,
    DataChunk &output) {

    auto &bind_data = data_p.bind_data->Cast<SharepointExcelBindData>();

    // Return the temp file path
    // The user will actually use this with read_xlsx
    FlatVector::GetData<string_t>(output.data[0])[0] =
        StringVector::AddString(output.data[0], bind_data.temp_file_path);

    output.SetCardinality(1);
}

// Register the function
void RegisterSharepointExcelFunction(DatabaseInstance &db) {
    // Note: This is a simplified version
    // A better implementation would directly integrate with Excel extension

    TableFunction excel_func(
        "read_sharepoint_excel_path",
        {LogicalType::VARCHAR},
        SharepointExcelFunction,
        SharepointExcelBind
    );

    ExtensionUtil::RegisterFunction(db, excel_func);
}

} // namespace duckdb
```

Create `src/include/sharepoint_excel.hpp`:

```cpp
#pragma once

#include "duckdb.hpp"

namespace duckdb {

void RegisterSharepointExcelFunction(DatabaseInstance &db);

} // namespace duckdb
```

## Step 4: User-Friendly Approach with Macros

Since directly integrating with the Excel extension's bind function is complex, a simpler approach is to create a DuckDB macro that combines both extensions:

Add to `src/sharepoint_extension.cpp`:

```cpp
static void LoadInternal(DatabaseInstance &instance) {
    // ... existing initialization ...

    RegisterSharepointAuthFunctions(instance);
    RegisterSharepointReadFunction(instance);
    RegisterSharepointLibraryFunction(instance);
    RegisterSharepointExcelFunction(instance);

    // Create convenience macro for reading Excel files from SharePoint
    auto &config = DBConfig::GetConfig(instance);
    Connection con(instance);

    // Create macro that downloads and reads Excel file
    con.Query(R"(
        CREATE OR REPLACE MACRO read_sharepoint_excel(url, sheet := NULL) AS TABLE
        (
            SELECT * FROM read_xlsx(
                (SELECT temp_path FROM (
                    SELECT sharepoint_download_excel(url) as temp_path
                )),
                sheet := sheet
            )
        );
    )");
}
```

## Step 5: Alternative - Simple Download Function

A more straightforward approach is to create a scalar function that downloads and returns the path:

```cpp
// In sharepoint_excel.cpp

static void DownloadExcelScalar(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &url_vector = args.data[0];
    UnifiedVectorFormat url_data;
    url_vector.ToUnifiedFormat(args.size(), url_data);

    auto result_data = FlatVector::GetData<string_t>(result);

    for (idx_t i = 0; i < args.size(); i++) {
        auto idx = url_data.sel->get_index(i);

        if (!url_data.validity.RowIsValid(idx)) {
            FlatVector::SetNull(result, i, true);
            continue;
        }

        // Get URL
        string_t url_str = ((string_t*)url_data.data)[idx];
        std::string url = url_str.GetString();

        // Get token (from context somehow - this is simplified)
        // std::string token = SharepointAuth::GetAccessToken(state.GetContext());

        // Download file
        // ... (implementation as above)

        // Return temp path
        result_data[i] = StringVector::AddString(result, temp_path);
    }
}

void RegisterSharepointExcelFunction(DatabaseInstance &db) {
    ScalarFunction download_func(
        "sharepoint_download_excel",
        {LogicalType::VARCHAR},
        LogicalType::VARCHAR,
        DownloadExcelScalar
    );

    ExtensionUtil::RegisterFunction(db, download_func);
}
```

## Step 6: Practical Usage Examples

Once implemented, users can query Excel files from SharePoint:

### Basic Query

```sql
-- Load both extensions
LOAD sharepoint;
LOAD excel;

-- Authenticate with SharePoint
CREATE SECRET (TYPE sharepoint, PROVIDER oauth);

-- Method 1: Two-step approach
-- Step 1: Download file
CREATE TEMP TABLE temp AS
SELECT sharepoint_download_excel(
    'https://contoso.sharepoint.com/sites/Finance/Documents/Budget2024.xlsx'
) as path;

-- Step 2: Read with Excel extension
SELECT * FROM read_xlsx((SELECT path FROM temp), sheet := 'Q1 Data');

-- Method 2: Using macro (if implemented)
SELECT * FROM read_sharepoint_excel(
    'https://contoso.sharepoint.com/sites/Finance/Documents/Budget2024.xlsx',
    sheet := 'Q1 Data'
);
```

### Advanced Queries

```sql
-- Query specific range
SELECT * FROM read_xlsx(
    sharepoint_download_excel('https://.../file.xlsx'),
    sheet := 'Data',
    range := 'A1:E100'
);

-- Join Excel data with SharePoint list
SELECT
    e.EmployeeID,
    e.Name,
    s.Department,
    s.Salary
FROM read_xlsx(sharepoint_download_excel('https://.../employees.xlsx')) e
JOIN read_sharepoint('https://.../Lists/HR') s
    ON e.EmployeeID = s.ID;

-- Aggregate data from multiple Excel files
SELECT 'Q1' as quarter, * FROM read_sharepoint_excel('https://.../Q1.xlsx')
UNION ALL
SELECT 'Q2' as quarter, * FROM read_sharepoint_excel('https://.../Q2.xlsx')
UNION ALL
SELECT 'Q3' as quarter, * FROM read_sharepoint_excel('https://.../Q3.xlsx')
UNION ALL
SELECT 'Q4' as quarter, * FROM read_sharepoint_excel('https://.../Q4.xlsx');
```

### Batch Processing

```sql
-- Process all Excel files in a SharePoint folder
WITH excel_files AS (
    SELECT name, id, web_url
    FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/Finance/Reports')
    WHERE name LIKE '%.xlsx'
)
SELECT
    f.name as file_name,
    d.*
FROM excel_files f,
LATERAL (
    SELECT * FROM read_xlsx(sharepoint_download_excel(f.web_url))
) d;
```

## Step 7: Performance Considerations

### Caching Downloaded Files

```cpp
// Cache downloaded files to avoid re-downloading
static std::unordered_map<std::string, std::string> file_cache;
static std::mutex cache_mutex;

std::string GetOrDownloadFile(const std::string &url, const std::string &token) {
    std::lock_guard<std::mutex> lock(cache_mutex);

    auto it = file_cache.find(url);
    if (it != file_cache.end()) {
        // Check if file still exists
        struct stat buffer;
        if (stat(it->second.c_str(), &buffer) == 0) {
            return it->second;  // Return cached path
        }
    }

    // Download and cache
    std::string temp_path = GenerateTempPath(ExtractFilename(url));
    DownloadFile(url, temp_path, token);
    file_cache[url] = temp_path;

    return temp_path;
}
```

### Parallel Downloads

```sql
-- Download multiple files in parallel (if DuckDB supports)
CREATE TABLE excel_data AS
SELECT * FROM read_sharepoint_excel('https://.../file1.xlsx')
UNION ALL
SELECT * FROM read_sharepoint_excel('https://.../file2.xlsx')
UNION ALL
SELECT * FROM read_sharepoint_excel('https://.../file3.xlsx');
```

### Cleanup Strategy

```cpp
// Register cleanup function to run on extension unload
class ExcelCleanup {
public:
    ~ExcelCleanup() {
        // Delete all temp files
        for (const auto &path : temp_files) {
            std::remove(path.c_str());
        }
    }

    static std::vector<std::string> temp_files;
};

// Add temp file to cleanup list
ExcelCleanup::temp_files.push_back(temp_path);
```

## Step 8: Error Handling

Handle common issues:

```cpp
// Verify file is Excel
if (!IsExcelFile(url)) {
    throw InvalidInputException(
        "File must be .xlsx or .xlsm format. "
        "Legacy .xls files are not supported by DuckDB's Excel extension."
    );
}

// Check file size before downloading
if (file_size > MAX_FILE_SIZE) {
    throw IOException(
        "File is too large (" + std::to_string(file_size / 1024 / 1024) + " MB). "
        "Maximum supported size is " + std::to_string(MAX_FILE_SIZE / 1024 / 1024) + " MB."
    );
}

// Handle download failures
try {
    DownloadFile(url, temp_path, token);
} catch (const std::exception &e) {
    throw IOException(
        "Failed to download Excel file from SharePoint: " +
        std::string(e.what())
    );
}

// Verify Excel file is valid
try {
    // Try to open with Excel extension
    auto result = con.Query("SELECT COUNT(*) FROM read_xlsx('" + temp_path + "')");
} catch (const std::exception &e) {
    std::remove(temp_path.c_str());  // Clean up
    throw IOException(
        "Downloaded file is not a valid Excel file: " +
        std::string(e.what())
    );
}
```

## Step 9: Complete Example Use Case

### Business Intelligence Report

```sql
-- Create a comprehensive report combining SharePoint lists and Excel files

LOAD sharepoint;
LOAD excel;

CREATE SECRET (TYPE sharepoint, PROVIDER oauth);

-- Get employee data from SharePoint list
CREATE TEMP TABLE employees AS
SELECT * FROM read_sharepoint(
    'https://contoso.sharepoint.com/sites/HR/Lists/Employees'
);

-- Get salary data from Excel file (sensitive data)
CREATE TEMP TABLE salaries AS
SELECT * FROM read_xlsx(
    sharepoint_download_excel(
        'https://contoso.sharepoint.com/sites/HR/Documents/Salaries2024.xlsx'
    ),
    sheet := 'Current'
);

-- Get budget data from another Excel file
CREATE TEMP TABLE budgets AS
SELECT * FROM read_xlsx(
    sharepoint_download_excel(
        'https://contoso.sharepoint.com/sites/Finance/Documents/Budget2024.xlsx'
    ),
    sheet := 'Departments'
);

-- Create comprehensive report
SELECT
    e.Department,
    COUNT(e.EmployeeID) as headcount,
    AVG(s.Salary) as avg_salary,
    SUM(s.Salary) as total_salary,
    b.Budget,
    b.Budget - SUM(s.Salary) as remaining_budget,
    ROUND(100.0 * SUM(s.Salary) / b.Budget, 2) as budget_utilization_pct
FROM employees e
JOIN salaries s ON e.EmployeeID = s.EmployeeID
JOIN budgets b ON e.Department = b.DepartmentName
WHERE e.Status = 'Active'
GROUP BY e.Department, b.Budget
ORDER BY budget_utilization_pct DESC;

-- Export to new Excel file
COPY (
    SELECT * FROM ... -- your query
) TO 'report_2024_q1.xlsx' WITH (FORMAT xlsx, HEADER true);
```

## Step 10: Testing

Add tests in `test/sql/sharepoint_excel.test`:

```sql
# name: test/sql/sharepoint_excel.test
# description: Test SharePoint Excel integration
# group: [sharepoint]

require sharepoint
require excel

require-env SHAREPOINT_TOKEN
require-env SHAREPOINT_EXCEL_URL

statement ok
CREATE SECRET sharepoint_test (
    TYPE sharepoint,
    PROVIDER token,
    TOKEN '${SHAREPOINT_TOKEN}'
);

# Test downloading Excel file
query I
SELECT LENGTH(sharepoint_download_excel('${SHAREPOINT_EXCEL_URL}')) > 0;
----
true

# Test reading Excel file
query I
SELECT COUNT(*) >= 0
FROM read_xlsx(
    sharepoint_download_excel('${SHAREPOINT_EXCEL_URL}')
);
----
true

# Clean up
statement ok
DROP SECRET sharepoint_test;
```

## Limitations & Considerations

1. **File Size**: Large Excel files (>100MB) may be slow to download
2. **Memory**: Files are downloaded to disk, not streamed
3. **Temp Files**: Need proper cleanup to avoid disk space issues
4. **Format Support**: Only `.xlsx` and `.xlsm`, not legacy `.xls`
5. **Concurrent Access**: Multiple downloads may hit rate limits
6. **Token Expiration**: Long-running queries may need token refresh

## What You've Accomplished

You can now:
- ✅ Read Excel files from SharePoint Document Libraries
- ✅ Combine SharePoint Lists and Excel data in queries
- ✅ Download and cache Excel files efficiently
- ✅ Query multiple Excel files across SharePoint sites
- ✅ Create business intelligence reports from mixed sources

This integration makes your SharePoint extension incredibly powerful for organizations that store data in both SharePoint Lists and Excel files!

---

**Navigation:**
- Previous: [09 - Next Steps](./09-next-steps.md)
- Back to: [00 - Introduction](./00-introduction.md)
