#pragma once

#include <string>

namespace duckdb {

class ClientContext;

// HTTP request methods
enum class HttpMethod { GET, POST, PUT, DELETE_REQUEST };

// Perform HTTPS request to SharePoint/Graph API
std::string PerformHttpsRequest(ClientContext &context, const std::string &host, const std::string &path,
                                const std::string &token, HttpMethod method = HttpMethod::GET,
                                const std::string &body = "", const std::string &content_type = "application/json",
                                const std::string &accept = "application/json");

// SharePoint-specific API wrappers

// Get items from a SharePoint list
std::string CallGraphApiListItems(ClientContext &context, const std::string &site_id, const std::string &list_id,
                                  const std::string &token, const std::string &select_fields = "",
                                  const std::string &filter = "", int top = 0);

// Get list metadata (columns, types, etc.)
std::string GetListMetadata(ClientContext &context, const std::string &site_id, const std::string &list_id,
                            const std::string &token);

// Get site information
std::string GetSiteByUrl(ClientContext &context, const std::string &site_url, const std::string &token);

// Get document library items
std::string GetLibraryItems(ClientContext &context, const std::string &site_id, const std::string &drive_id,
                            const std::string &token, const std::string &folder_path = "");

// Download file content from SharePoint (binary data)
std::string DownloadSharepointFileContent(ClientContext &context, const std::string &site_id,
                                          const std::string &drive_id, const std::string &item_id,
                                          const std::string &token);

} // namespace duckdb
