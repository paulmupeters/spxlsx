#include "sharepoint_utils.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace duckdb {
namespace SharepointUtils {

std::string UrlEncode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        // Keep alphanumeric and other safe characters
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        }
        // Space becomes '+'
        else if (c == ' ') {
            escaped << '+';
        }
        // Encode other characters
        else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char) c);
            escaped << std::nouppercase;
        }
    }

    return escaped.str();
}

std::string ExtractSiteUrl(const std::string &url) {
    // Extract site from URL like:
    // https://contoso.sharepoint.com/sites/TeamSite/Lists/MyList
    // Returns: /sites/TeamSite

    size_t sites_pos = url.find("/sites/");
    if (sites_pos == std::string::npos) {
        // Try for root site
        size_t protocol_end = url.find("://");
        if (protocol_end != std::string::npos) {
            size_t domain_end = url.find("/", protocol_end + 3);
            if (domain_end == std::string::npos) {
                return "/";  // Root site
            }
        }
        return "/";
    }

    // Find the next segment after /sites/SiteName
    size_t site_name_start = sites_pos + 7;  // Length of "/sites/"
    size_t site_name_end = url.find("/", site_name_start);

    if (site_name_end == std::string::npos) {
        return url.substr(sites_pos);
    }

    return url.substr(sites_pos, site_name_end - sites_pos);
}

std::string ExtractListName(const std::string &url) {
    // Extract list name from URL like:
    // https://contoso.sharepoint.com/sites/TeamSite/Lists/MyList
    // Returns: MyList

    size_t lists_pos = url.find("/Lists/");
    if (lists_pos == std::string::npos) {
        lists_pos = url.find("/lists/");  // Try lowercase
    }

    if (lists_pos == std::string::npos) {
        return "";
    }

    size_t list_name_start = lists_pos + 7;  // Length of "/Lists/"
    size_t list_name_end = url.find("/", list_name_start);

    if (list_name_end == std::string::npos) {
        // No trailing slash, take rest of URL
        return url.substr(list_name_start);
    }

    return url.substr(list_name_start, list_name_end - list_name_start);
}

std::string ExtractTenantFromUrl(const std::string &url) {
    // Extract tenant from URL like:
    // https://contoso.sharepoint.com/...
    // Returns: contoso

    size_t protocol_end = url.find("://");
    if (protocol_end == std::string::npos) {
        return "";
    }

    size_t domain_start = protocol_end + 3;
    size_t domain_end = url.find(".sharepoint.com", domain_start);

    if (domain_end == std::string::npos) {
        return "";
    }

    return url.substr(domain_start, domain_end - domain_start);
}

} // namespace SharepointUtils
} // namespace duckdb