#include "sharepoint_requests.hpp"
#include "duckdb/common/exception.hpp"
#include "yyjson.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <sstream>
#include <thread>
#include <chrono>

namespace duckdb {

// Helper: Extract HTTP status code from response
static int ExtractStatusCode(const std::string &response) {
    size_t status_pos = response.find("HTTP/");
    if (status_pos == std::string::npos) {
        return 0;
    }

    size_t code_start = response.find(" ", status_pos) + 1;
    size_t code_end = response.find(" ", code_start);

    if (code_start == std::string::npos || code_end == std::string::npos) {
        return 0;
    }

    std::string code_str = response.substr(code_start, code_end - code_start);
    return std::stoi(code_str);
}

// Helper: Extract body from HTTP response
static std::string ExtractBody(const std::string &response) {
    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return "";
    }
    return response.substr(header_end + 4);
}

std::string PerformHttpsRequest(
    const std::string &host,
    const std::string &path,
    const std::string &token,
    HttpMethod method,
    const std::string &body,
    const std::string &content_type) {

    // Retry configuration
    const int MAX_RETRIES = 3;
    int retry_count = 0;
    int backoff_seconds = 1;

    while (retry_count <= MAX_RETRIES) {
        try {
            // 1. Create SSL context
            const SSL_METHOD *ssl_method = SSLv23_client_method();
            SSL_CTX *ctx = SSL_CTX_new(ssl_method);
            if (!ctx) {
                throw IOException("Failed to create SSL context");
            }

            // 2. Create BIO connection
            BIO *bio = BIO_new_ssl_connect(ctx);
            if (!bio) {
                SSL_CTX_free(ctx);
                throw IOException("Failed to create BIO");
            }

            // 3. Set hostname (with port 443 for HTTPS)
            std::string host_with_port = host + ":443";
            BIO_set_conn_hostname(bio, host_with_port.c_str());

            // 4. Get SSL pointer and configure
            SSL *ssl;
            BIO_get_ssl(bio, &ssl);
            if (!ssl) {
                BIO_free_all(bio);
                SSL_CTX_free(ctx);
                throw IOException("Failed to get SSL pointer");
            }

            SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

            // 5. Set SNI (Server Name Indication)
            SSL_set_tlsext_host_name(ssl, host.c_str());

            // 6. Establish connection
            if (BIO_do_connect(bio) <= 0) {
                BIO_free_all(bio);
                SSL_CTX_free(ctx);
                throw IOException("Failed to connect to " + host);
            }

            // 7. Verify SSL handshake
            if (BIO_do_handshake(bio) <= 0) {
                BIO_free_all(bio);
                SSL_CTX_free(ctx);
                throw IOException("SSL handshake failed");
            }

            // 8. Build HTTP request
            std::ostringstream request;

            // Method and path
            switch (method) {
                case HttpMethod::GET:
                    request << "GET ";
                    break;
                case HttpMethod::POST:
                    request << "POST ";
                    break;
                case HttpMethod::PUT:
                    request << "PUT ";
                    break;
                case HttpMethod::DELETE:
                    request << "DELETE ";
                    break;
            }
            request << path << " HTTP/1.1\r\n";

            // Headers
            request << "Host: " << host << "\r\n";
            if (!token.empty()) {
                request << "Authorization: Bearer " << token << "\r\n";
            }
            request << "Accept: application/json\r\n";
            request << "User-Agent: DuckDB-SharePoint-Extension/1.0\r\n";
            request << "Connection: close\r\n";

            // Body (for POST/PUT)
            if (!body.empty()) {
                request << "Content-Type: " << content_type << "\r\n";
                request << "Content-Length: " << body.length() << "\r\n";
                request << "\r\n";
                request << body;
            } else {
                request << "\r\n";
            }

            // 9. Send request
            std::string request_str = request.str();
            int written = BIO_write(bio, request_str.c_str(), request_str.length());
            if (written <= 0) {
                BIO_free_all(bio);
                SSL_CTX_free(ctx);
                throw IOException("Failed to send request");
            }

            // 10. Read response
            std::string response;
            char buffer[4096];
            int read_bytes;

            while ((read_bytes = BIO_read(bio, buffer, sizeof(buffer) - 1)) > 0) {
                buffer[read_bytes] = '\0';
                response += buffer;
            }

            // 11. Cleanup
            BIO_free_all(bio);
            SSL_CTX_free(ctx);

            // 12. Check for empty response
            if (response.empty()) {
                throw IOException("Empty response from server");
            }

            // 13. Extract status code
            int status_code = ExtractStatusCode(response);

            // 14. Handle rate limiting (429) with retry
            if (status_code == 429) {
                if (retry_count < MAX_RETRIES) {
retry_count++;
                    std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
                    backoff_seconds *= 2;  // Exponential backoff
                    continue;  // Retry
                } else {
                    throw IOException("Rate limit exceeded (429) - too many requests");
                }
            }

            // 15. Handle other HTTP errors
            if (status_code >= 400) {
                std::string body = ExtractBody(response);
                throw IOException("HTTP " + std::to_string(status_code) + ": " + body);
            }

            // 16. Return response body
            return ExtractBody(response);

        } catch (const std::exception &e) {
            // If we've exhausted retries, re-throw
            if (retry_count >= MAX_RETRIES) {
                throw;
            }
            // Otherwise, retry
            retry_count++;
            std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
            backoff_seconds *= 2;
        }
    }

    throw IOException("Failed after maximum retries");
}

// Call Microsoft Graph API to get list items
std::string CallGraphApiListItems(
    const std::string &site_id,
    const std::string &list_id,
    const std::string &token,
    const std::string &select_fields,
    const std::string &filter,
    int top) {

    std::ostringstream path;
    path << "/v1.0/sites/" << site_id << "/lists/" << list_id << "/items";
    path << "?expand=fields";

    if (!select_fields.empty()) {
        path << "&$select=" << select_fields;
    }

    if (!filter.empty()) {
        path << "&$filter=" << filter;
    }

    if (top > 0) {
        path << "&$top=" << top;
    }

    return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);
}

// Get list schema/metadata
std::string GetListMetadata(
    const std::string &site_id,
    const std::string &list_id,
    const std::string &token) {

    std::ostringstream path;
    path << "/v1.0/sites/" << site_id << "/lists/" << list_id;
    path << "?expand=columns";

    return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);
}

// Get site information from URL
std::string GetSiteByUrl(
    const std::string &site_url,
    const std::string &token) {

    // URL encode the site URL
    // For simplicity, we'll handle this in utils module
    std::ostringstream path;
    path << "/v1.0/sites/root:/" << site_url;

    return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);
}

// Get document library items
std::string GetLibraryItems(
    const std::string &site_id,
    const std::string &drive_id,
    const std::string &token,
    const std::string &folder_path) {

    std::ostringstream path;
    path << "/v1.0/sites/" << site_id << "/drives/" << drive_id;

    if (folder_path.empty()) {
        path << "/root/children";
    } else {
        path << "/root:/" << folder_path << ":/children";
    }

    return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);
}

} // namespace duckdb