#pragma once

#include <string>

namespace duckdb {

// Utility functions for URL parsing, etc.
namespace SharepointUtils {
    // Extract site URL from full SharePoint URL
    std::string ExtractSiteUrl(const std::string &url);

    // Extract list name from URL
    std::string ExtractListName(const std::string &url);

    // URL encode a string
    std::string UrlEncode(const std::string &value);
}

} // namespace duckdb