# 03 - HTTP Layer

In this module, you'll implement the HTTP/HTTPS client that communicates with SharePoint's APIs. This is the foundation for all data operations.

## Goals
- Understand OpenSSL basics for HTTPS
- Implement a generic HTTPS request function
- Handle errors and retries
- Create SharePoint-specific API wrappers
- Test API calls

## Overview

SharePoint Online exposes data through REST APIs:
- **Microsoft Graph API**: Modern, recommended (`https://graph.microsoft.com/v1.0/...`)
- **SharePoint REST API**: Legacy but sometimes necessary (`https://{tenant}.sharepoint.com/_api/...`)

We'll use Microsoft Graph API as it's more feature-rich and easier to work with.

## Step 1: Understanding the Request Flow

```
Your Extension
     │
     ├─→ PerformHttpsRequest()
     │       │
     │       ├─→ Create SSL connection
     │       ├─→ Send HTTP request with Bearer token
     │       ├─→ Read response
     │       └─→ Parse result
     │
     └─→ Graph API returns JSON
```

## Step 2: Implement Core HTTPS Function

Update `src/sharepoint_requests.cpp` with the complete implementation:

```cpp
#include "sharepoint_requests.hpp"
#include "duckdb/common/exception.hpp"

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
    const std::string &body) {

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
            request << "Authorization: Bearer " << token << "\r\n";
            request << "Accept: application/json\r\n";
            request << "User-Agent: DuckDB-SharePoint-Extension/1.0\r\n";

            // Body (for POST/PUT)
            if (!body.empty()) {
                request << "Content-Type: application/json\r\n";
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

} // namespace duckdb
```

### Understanding the Code

1. **SSL Context**: Creates a secure connection context
2. **BIO (Basic I/O)**: OpenSSL's abstraction for I/O operations
3. **SNI (Server Name Indication)**: Required for HTTPS to work with virtual hosts
4. **HTTP/1.1**: We use HTTP/1.1 protocol (simpler than HTTP/2)
5. **Bearer Token**: Authorization header with OAuth token
6. **Retry Logic**: Handles rate limiting (429) with exponential backoff
7. **Error Handling**: Parses HTTP status codes and throws meaningful errors

## Step 3: Add SharePoint-Specific API Wrappers

Add these helper functions to `src/sharepoint_requests.cpp`:

```cpp
// Call Microsoft Graph API to get list items
std::string CallGraphApiListItems(
    const std::string &site_id,
    const std::string &list_id,
    const std::string &token,
    const std::string &select_fields = "",
    const std::string &filter = "",
    int top = 0) {

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
    const std::string &folder_path = "") {

    std::ostringstream path;
    path << "/v1.0/sites/" << site_id << "/drives/" << drive_id;

    if (folder_path.empty()) {
        path << "/root/children";
    } else {
        path << "/root:/" << folder_path << ":/children";
    }

    return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);
}
```

Update the header file `src/include/sharepoint_requests.hpp`:

```cpp
#pragma once

#include <string>

namespace duckdb {

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE
};

// Core HTTPS request function
std::string PerformHttpsRequest(
    const std::string &host,
    const std::string &path,
    const std::string &token,
    HttpMethod method = HttpMethod::GET,
    const std::string &body = ""
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

} // namespace duckdb
```

## Step 4: Implement Utility Functions

Update `src/sharepoint_utils.cpp` with URL parsing helpers:

```cpp
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
```

Update the header:

```cpp
#pragma once

#include <string>

namespace duckdb {
namespace SharepointUtils {

    // URL encode a string for use in HTTP requests
    std::string UrlEncode(const std::string &value);

    // Extract site path from SharePoint URL
    std::string ExtractSiteUrl(const std::string &url);

    // Extract list name from SharePoint URL
    std::string ExtractListName(const std::string &url);

    // Extract tenant name from SharePoint URL
    std::string ExtractTenantFromUrl(const std::string &url);

} // namespace SharepointUtils
} // namespace duckdb
```

## Step 5: Test the HTTP Layer

Create a simple test to verify HTTP requests work. Add to `src/sharepoint_requests.cpp`:

```cpp
// Test function (for debugging only - remove in production)
std::string TestConnection(const std::string &token) {
    // Simple test: Get Microsoft Graph service root
    return PerformHttpsRequest(
        "graph.microsoft.com",
        "/v1.0/",
        token,
        HttpMethod::GET
    );
}
```

Build and test:

```bash
make

# Test with a real token (get one from https://developer.microsoft.com/en-us/graph/graph-explorer)
# This is just for testing the HTTP layer!
```

## Step 6: Error Handling Patterns

Your HTTP layer should handle these common scenarios:

### Rate Limiting (429)
```cpp
if (status_code == 429) {
    // Retry with exponential backoff
    std::this_thread::sleep_for(std::chrono::seconds(backoff));
}
```

### Authentication Errors (401)
```cpp
if (status_code == 401) {
    throw IOException("Authentication failed - token may be expired");
}
```

### Not Found (404)
```cpp
if (status_code == 404) {
    throw IOException("Resource not found - check site/list URL");
}
```

### Throttling (503)
```cpp
if (status_code == 503) {
    throw IOException("Service unavailable - SharePoint may be throttling requests");
}
```

## Step 7: Testing Tips

### Use Graph Explorer
Test API endpoints manually: https://developer.microsoft.com/en-us/graph/graph-explorer

### Common Graph API Endpoints

```bash
# Get your sites
GET https://graph.microsoft.com/v1.0/sites?search=*

# Get lists in a site
GET https://graph.microsoft.com/v1.0/sites/{site-id}/lists

# Get list items
GET https://graph.microsoft.com/v1.0/sites/{site-id}/lists/{list-id}/items?expand=fields
```

### Debug Output

Add debug logging during development:

```cpp
#ifdef DEBUG
    std::cerr << "Request: " << request.str() << std::endl;
    std::cerr << "Response: " << response << std::endl;
#endif
```

## Common Issues

### SSL Certificate Verification Fails

If you get SSL errors, you may need to load system certificates:

```cpp
SSL_CTX_set_default_verify_paths(ctx);
```

### Connection Timeouts

Add timeout handling:

```cpp
BIO_set_nbio(bio, 1);  // Non-blocking
// Then use select() or poll() for timeout
```

### Large Responses

For large API responses, implement streaming:

```cpp
// Read in chunks and process incrementally
while ((read_bytes = BIO_read(bio, buffer, sizeof(buffer))) > 0) {
    // Process buffer incrementally
}
```

## What You've Accomplished

You now have:
- Working HTTPS client with OpenSSL
- Retry logic and error handling
- SharePoint API-specific wrappers
- URL parsing utilities
- Foundation for authentication and data retrieval

In the next module, you'll implement Azure AD OAuth to get the access tokens needed for these API calls.

---

**Navigation:**
- Previous: [02 - Extension Basics](./02-extension-basics.md)
- Next: [04 - Authentication](./04-authentication.md)
