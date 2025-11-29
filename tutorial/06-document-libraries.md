# 06 - Document Libraries

In this module, you'll extend your extension to read file metadata from SharePoint Document Libraries. This complements the list reading functionality with file system capabilities.

## Goals
- Understand SharePoint Document Libraries vs Lists
- Implement a table function for reading file metadata
- Access file properties (name, size, modified date, etc.)
- Handle folders and nested structures
- (Optional) Download file contents

## Overview: Document Libraries

Document Libraries are special types of lists that store files. Key differences:

| Aspect | Lists | Document Libraries |
|--------|-------|-------------------|
| **API** | Lists endpoint | Drives/Files endpoint |
| **Primary data** | List items | Files and folders |
| **Schema** | Custom columns | File properties + custom metadata |
| **Hierarchy** | Flat | Folder structure |

## Step 1: Add Library Function Signature

Update `src/include/sharepoint_read.hpp`:

```cpp
// ... existing code ...

// Bind data for library reads
struct SharepointLibraryBindData : public TableFunctionData {
    std::string site_id;
    std::string drive_id;
    std::string folder_path;
    std::string token;
    std::string response_json;
    bool finished;
    idx_t row_index;

    vector<LogicalType> return_types;
    vector<std::string> names;

    SharepointLibraryBindData() : finished(false), row_index(0) {}
};

// Register library function
void RegisterSharepointLibraryFunction(DatabaseInstance &db);
```

## Step 2: Implement Document Library Reader

Add to `src/sharepoint_read.cpp`:

```cpp
// ... existing includes and code ...

// Helper: Get drive (document library) ID
static std::string GetDriveIdByName(
    const std::string &site_id,
    const std::string &library_name,
    const std::string &token) {

    std::ostringstream path;
    path << "/v1.0/sites/" << site_id << "/drives";

    std::string response = PerformHttpsRequest(
        "graph.microsoft.com",
        path.str(),
        token,
        HttpMethod::GET
    );

    json drives_data = json::parse(response);

    if (!drives_data.contains("value")) {
        throw InvalidInputException("No drives found");
    }

    // Find drive by name
    for (const auto &drive : drives_data["value"]) {
        if (drive["name"] == library_name) {
            return drive["id"];
        }
    }

    throw InvalidInputException("Document library not found: " + library_name);
}

// Bind phase for library reads
static unique_ptr<FunctionData> SharepointLibraryBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    auto bind_data = make_uniq<SharepointLibraryBindData>();

    // 1. Parse input
    if (input.inputs.empty()) {
        throw InvalidInputException("read_sharepoint_library requires a URL");
    }

    std::string url = input.inputs[0].ToString();

    // 2. Get token
    bind_data->token = SharepointAuth::GetAccessToken(context);

    // 3. Extract site and library
    bind_data->site_id = GetSiteId(url, bind_data->token);

    // Extract library name from URL
    // e.g., /sites/TeamSite/Shared%20Documents
    std::string library_name = "Documents";  // Default
    size_t docs_pos = url.find("/Shared%20Documents");
    if (docs_pos == std::string::npos) {
        docs_pos = url.find("/Documents");
    }
    if (docs_pos != std::string::npos) {
        library_name = "Documents";
    }

    // 4. Get drive ID
    bind_data->drive_id = GetDriveIdByName(
        bind_data->site_id,
        library_name,
        bind_data->token
    );

    // 5. Check for folder path parameter
    auto folder_param = input.named_parameters.find("folder");
    if (folder_param != input.named_parameters.end()) {
        bind_data->folder_path = folder_param->second.ToString();
    }

    // 6. Define schema for file metadata
    names.push_back("name");
    return_types.push_back(LogicalType::VARCHAR);

    names.push_back("size");
    return_types.push_back(LogicalType::BIGINT);

    names.push_back("created");
    return_types.push_back(LogicalType::TIMESTAMP);

    names.push_back("modified");
    return_types.push_back(LogicalType::TIMESTAMP);

    names.push_back("created_by");
    return_types.push_back(LogicalType::VARCHAR);

    names.push_back("modified_by");
    return_types.push_back(LogicalType::VARCHAR);

    names.push_back("web_url");
    return_types.push_back(LogicalType::VARCHAR);

    names.push_back("is_folder");
    return_types.push_back(LogicalType::BOOLEAN);

    names.push_back("file_type");
    return_types.push_back(LogicalType::VARCHAR);

    bind_data->names = names;
    bind_data->return_types = return_types;

    // 7. Fetch items
    bind_data->response_json = GetLibraryItems(
        bind_data->site_id,
        bind_data->drive_id,
        bind_data->token,
        bind_data->folder_path
    );

    return std::move(bind_data);
}

// Execution phase for library reads
static void SharepointLibraryFunction(
    ClientContext &context,
    TableFunctionInput &data_p,
    DataChunk &output) {

    auto &bind_data = data_p.bind_data->Cast<SharepointLibraryBindData>();

    if (bind_data.finished) {
        return;
    }

    // Parse response
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

    // Process items
    idx_t output_idx = 0;
    idx_t max_rows = STANDARD_VECTOR_SIZE;

    while (bind_data.row_index < items_count && output_idx < max_rows) {
        auto &item = items[bind_data.row_index];

        // Column 0: Name
        std::string name = item.value("name", "");
        FlatVector::GetData<string_t>(output.data[0])[output_idx] =
            StringVector::AddString(output.data[0], name);

        // Column 1: Size
        int64_t size = item.value("size", 0);
        FlatVector::GetData<int64_t>(output.data[1])[output_idx] = size;

        // Column 2: Created
        std::string created_time = item.value("createdDateTime", "");
        FlatVector::GetData<string_t>(output.data[2])[output_idx] =
            StringVector::AddString(output.data[2], created_time);

        // Column 3: Modified
        std::string modified_time = item.value("lastModifiedDateTime", "");
        FlatVector::GetData<string_t>(output.data[3])[output_idx] =
            StringVector::AddString(output.data[3], modified_time);

        // Column 4: Created by
        std::string created_by = "";
        if (item.contains("createdBy") && item["createdBy"].contains("user")) {
            created_by = item["createdBy"]["user"].value("displayName", "");
        }
        FlatVector::GetData<string_t>(output.data[4])[output_idx] =
            StringVector::AddString(output.data[4], created_by);

        // Column 5: Modified by
        std::string modified_by = "";
        if (item.contains("lastModifiedBy") && item["lastModifiedBy"].contains("user")) {
            modified_by = item["lastModifiedBy"]["user"].value("displayName", "");
        }
        FlatVector::GetData<string_t>(output.data[5])[output_idx] =
            StringVector::AddString(output.data[5], modified_by);

        // Column 6: Web URL
        std::string web_url = item.value("webUrl", "");
        FlatVector::GetData<string_t>(output.data[6])[output_idx] =
            StringVector::AddString(output.data[6], web_url);

        // Column 7: Is folder
        bool is_folder = item.contains("folder");
        FlatVector::GetData<bool>(output.data[7])[output_idx] = is_folder;

        // Column 8: File type
        std::string file_type = "";
        if (item.contains("file") && item["file"].contains("mimeType")) {
            file_type = item["file"]["mimeType"];
        } else if (is_folder) {
            file_type = "folder";
        }
        FlatVector::GetData<string_t>(output.data[8])[output_idx] =
            StringVector::AddString(output.data[8], file_type);

        bind_data.row_index++;
        output_idx++;
    }

    output.SetCardinality(output_idx);

    if (bind_data.row_index >= items_count) {
        bind_data.finished = true;
    }
}

// Register library function
void RegisterSharepointLibraryFunction(DatabaseInstance &db) {
    TableFunction read_library_func(
        "read_sharepoint_library",
        {LogicalType::VARCHAR},
        SharepointLibraryFunction,
        SharepointLibraryBind
    );

    // Optional parameters
    read_library_func.named_parameters["folder"] = LogicalType::VARCHAR;
    read_library_func.named_parameters["recursive"] = LogicalType::BOOLEAN;

    ExtensionUtil::RegisterFunction(db, read_library_func);
}
```

## Step 3: Register Library Function

Update `src/sharepoint_extension.cpp`:

```cpp
#include "sharepoint_read.hpp"

static void LoadInternal(DatabaseInstance &instance) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    RegisterSharepointAuthFunctions(instance);
    RegisterSharepointReadFunction(instance);
    RegisterSharepointLibraryFunction(instance);  // Add this
}
```

## Step 4: Test Document Library Access

```sql
-- Load extension
LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension';

-- Create secret if not already done
CREATE SECRET sharepoint_secret (TYPE sharepoint, PROVIDER oauth);

-- Query document library
SELECT name, size, modified, created_by
FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/TeamSite/Shared%20Documents');

-- Find large files
SELECT name, size / 1024 / 1024 as size_mb
FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/TeamSite/Documents')
WHERE size > 10485760  -- 10 MB
ORDER BY size DESC;

-- Find recently modified files
SELECT name, modified, modified_by
FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/TeamSite/Documents')
WHERE modified > CURRENT_DATE - INTERVAL '7 days'
ORDER BY modified DESC;

-- Count files by type
SELECT file_type, COUNT(*) as count, SUM(size) as total_size
FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/TeamSite/Documents')
GROUP BY file_type
ORDER BY count DESC;

-- Query specific folder
SELECT *
FROM read_sharepoint_library(
    'https://contoso.sharepoint.com/sites/TeamSite/Documents',
    folder := 'Projects/2024'
);
```

## Step 5: Add Recursive Folder Traversal

To scan all files recursively:

```cpp
// In SharepointLibraryBind, add recursive parameter handling:
auto recursive_param = input.named_parameters.find("recursive");
bool recursive = false;
if (recursive_param != input.named_parameters.end()) {
    recursive = recursive_param->second.GetValue<bool>();
}

if (recursive) {
    // Fetch all items recursively
    std::function<void(const std::string&)> fetch_recursive =
        [&](const std::string &path) {
            std::string items_json = GetLibraryItems(
                bind_data->site_id,
                bind_data->drive_id,
                bind_data->token,
                path
            );

            json items = json::parse(items_json);

            // Append to results
            // ... merge logic ...

            // Recurse into folders
            if (items.contains("value")) {
                for (const auto &item : items["value"]) {
                    if (item.contains("folder")) {
                        std::string subfolder = path.empty() ?
                            item["name"] : path + "/" + item["name"];
                        fetch_recursive(subfolder);
                    }
                }
            }
        };

    fetch_recursive(bind_data->folder_path);
}
```

Use it:

```sql
-- Scan entire document library recursively
SELECT *
FROM read_sharepoint_library(
    'https://contoso.sharepoint.com/sites/TeamSite/Documents',
    recursive := true
);
```

## Step 6: Download File Contents (Advanced)

To actually download file contents, add a function:

```cpp
// In sharepoint_requests.cpp:
std::string DownloadFileContent(
    const std::string &site_id,
    const std::string &drive_id,
    const std::string &file_id,
    const std::string &token) {

    std::ostringstream path;
    path << "/v1.0/sites/" << site_id
         << "/drives/" << drive_id
         << "/items/" << file_id
         << "/content";

    return PerformHttpsRequest(
        "graph.microsoft.com",
        path.str(),
        token,
        HttpMethod::GET
    );
}
```

Then create a scalar function:

```cpp
// Scalar function to download file
static void DownloadFileScalar(DataChunk &args, ExpressionState &state, Vector &result) {
    // Extract file ID from args
    // Download content
    // Return as BLOB
}

void RegisterSharepointDownloadFunction(DatabaseInstance &db) {
    ScalarFunction download_func(
        "sharepoint_download",
        {LogicalType::VARCHAR},  // file_id
        LogicalType::BLOB,
        DownloadFileScalar
    );

    ExtensionUtil::RegisterFunction(db, download_func);
}
```

Use it:

```sql
-- Download file content
SELECT name, sharepoint_download(id) as content
FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/TeamSite/Documents')
WHERE name = 'report.pdf';
```

## Step 7: File Metadata Properties

SharePoint files have many additional properties you can expose:

```cpp
// Add more columns:
names.push_back("id");
return_types.push_back(LogicalType::VARCHAR);

names.push_back("parent_path");
return_types.push_back(LogicalType::VARCHAR);

names.push_back("file_extension");
return_types.push_back(LogicalType::VARCHAR);

names.push_back("mime_type");
return_types.push_back(LogicalType::VARCHAR);

names.push_back("download_url");
return_types.push_back(LogicalType::VARCHAR);

// For images/videos:
names.push_back("width");
return_types.push_back(LogicalType::INTEGER);

names.push_back("height");
return_types.push_back(LogicalType::INTEGER);

// For documents:
names.push_back("page_count");
return_types.push_back(LogicalType::INTEGER);

names.push_back("word_count");
return_types.push_back(LogicalType::INTEGER);
```

Extract from response:

```cpp
// ID
std::string id = item.value("id", "");

// Parent path
std::string parent_path = "";
if (item.contains("parentReference") &&
    item["parentReference"].contains("path")) {
    parent_path = item["parentReference"]["path"];
}

// File extension
std::string extension = "";
if (item.contains("file") && item["file"].contains("extension")) {
    extension = item["file"]["extension"];
}

// Download URL
std::string download_url = "";
if (item.contains("@microsoft.graph.downloadUrl")) {
    download_url = item["@microsoft.graph.downloadUrl"];
}

// Image dimensions
int width = 0, height = 0;
if (item.contains("image")) {
    width = item["image"].value("width", 0);
    height = item["image"].value("height", 0);
}
```

## Step 8: Advanced Queries

### Find duplicate files

```sql
SELECT name, COUNT(*) as count, array_agg(web_url) as locations
FROM read_sharepoint_library('...', recursive := true)
GROUP BY name
HAVING COUNT(*) > 1;
```

### Storage analysis by user

```sql
SELECT created_by,
       COUNT(*) as file_count,
       SUM(size) / 1024 / 1024 / 1024 as storage_gb
FROM read_sharepoint_library('...', recursive := true)
WHERE NOT is_folder
GROUP BY created_by
ORDER BY storage_gb DESC;
```

### Find stale files

```sql
SELECT name, modified, size / 1024 / 1024 as size_mb
FROM read_sharepoint_library('...', recursive := true)
WHERE modified < CURRENT_DATE - INTERVAL '365 days'
  AND NOT is_folder
ORDER BY size DESC;
```

### Export file inventory

```sql
COPY (
    SELECT name, size, modified, created_by, file_type, web_url
    FROM read_sharepoint_library('...', recursive := true)
    WHERE NOT is_folder
) TO 'file_inventory.csv' (HEADER, DELIMITER ',');
```

## Common Issues

### Issue: "Drive not found"

**Solution**: Document library names are case-sensitive. Try "Documents" or "Shared Documents".

### Issue: Permission denied

**Solution**: Ensure your Azure AD app has `Files.Read.All` permission granted.

### Issue: Large libraries timeout

**Solution**: Implement pagination and streaming for large libraries.

### Issue: Special characters in file names

**Solution**: URL-encode file paths properly when accessing specific files.

## Performance Tips

1. **Filter at source**: Use SharePoint's `$filter` parameter when possible
2. **Limit columns**: Only fetch metadata you need with `$select`
3. **Parallel downloads**: Download multiple files concurrently
4. **Cache metadata**: Store file metadata locally for repeated queries
5. **Use deltas**: Query only changes since last sync with delta API

## What You've Accomplished

You can now:
- Read file metadata from SharePoint Document Libraries
- Access file properties (size, dates, owners)
- Navigate folder hierarchies
- Query files recursively
- Analyze storage usage
- Find files by criteria
- (Optionally) Download file contents

Combined with list reading, your extension provides comprehensive SharePoint data access through SQL!

---

**Navigation:**
- Previous: [05 - SharePoint Lists](./05-sharepoint-lists.md)
- Next: [07 - Testing & Debugging](./07-testing-debugging.md)
