#pragma once

#include <string>

namespace duckdb {

// HTTP request methods
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE
};

// Perform HTTPS request to SharePoint/Graph API
std::string PerformHttpsRequest(
    const std::string &host,
    const std::string &path,
    const std::string &token,
    HttpMethod method = HttpMethod::GET,
    const std::string &body = "",
    const std::string &content_type = "application/json"
);

// SharePoint-specific API wrappers

// Get items from a SharePoint list
std::string CallGraphApiListItems(
    const std::string &site_id,
    const std::string &list_id,
    const std::string &token,
    const std::string &select_fields = "",
    const std::string &filter = "",
    int top = 0
);

// Get list metadata (columns, types, etc.)
std::string GetListMetadata(
    const std::string &site_id,
    const std::string &list_id,
    const std::string &token
);

// Get site information
std::string GetSiteByUrl(
    const std::string &site_url,
    const std::string &token
);

// Get document library items
std::string GetLibraryItems(
    const std::string &site_id,
    const std::string &drive_id,
    const std::string &token,
    const std::string &folder_path = ""
);

// Download file content from SharePoint (binary data)
std::string DownloadSharepointFileContent(
    const std::string &site_id,
    const std::string &drive_id,
    const std::string &item_id,
    const std::string &token
);

} // namespace duckdb