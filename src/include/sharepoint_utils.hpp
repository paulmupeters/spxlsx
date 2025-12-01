#pragma once

#include <string>

namespace duckdb {

// Utility functions for URL parsing, etc.
namespace SharepointUtils {
    // URL encode a string for use in HTTP requests
    std::string UrlEncode(const std::string &value);

    // Extract site path from SharePoint URL
    std::string ExtractSiteUrl(const std::string &url);

    // Extract list name from SharePoint URL
    std::string ExtractListName(const std::string &url);

    // Extract tenant name from SharePoint URL
    std::string ExtractTenantFromUrl(const std::string &url);
}

} // namespace duckdb