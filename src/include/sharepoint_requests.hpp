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
    const std::string &body = ""
);

} // namespace duckdb