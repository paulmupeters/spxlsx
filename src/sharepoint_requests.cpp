#include "sharepoint_requests.hpp"
#include "duckdb/common/exception.hpp"
#include "yyjson.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <sstream>
#include <thread>
#include <chrono>
#include <cctype>

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

// Helper: Check if response uses chunked transfer encoding
static bool IsChunkedEncoding(const std::string &response) {
	// Look for "Transfer-Encoding: chunked" in headers (case-insensitive search)
	// Try \r\n\r\n first, then \n\n
	size_t header_end = response.find("\r\n\r\n");
	if (header_end == std::string::npos) {
		header_end = response.find("\n\n");
	}
	if (header_end == std::string::npos) {
		return false;
	}
	std::string headers = response.substr(0, header_end);

	// Convert to lowercase for comparison
	std::string headers_lower = headers;
	for (char &c : headers_lower) {
		c = std::tolower(c);
	}

	// Check for various formats of the chunked header
	return headers_lower.find("transfer-encoding: chunked") != std::string::npos ||
	       headers_lower.find("transfer-encoding:chunked") != std::string::npos;
}

// Helper: Find line ending (handles both \r\n and \n)
static size_t FindLineEnd(const std::string &str, size_t start, size_t &line_end_len) {
	size_t crlf = str.find("\r\n", start);
	size_t lf = str.find("\n", start);

	if (crlf != std::string::npos && (lf == std::string::npos || crlf <= lf)) {
		line_end_len = 2;
		return crlf;
	} else if (lf != std::string::npos) {
		line_end_len = 1;
		return lf;
	}

	line_end_len = 0;
	return std::string::npos;
}

// Helper: Decode chunked transfer encoding
static std::string DecodeChunkedBody(const std::string &chunked_body) {
	std::string result;
	size_t pos = 0;

	while (pos < chunked_body.size()) {
		// Find the end of the chunk size line (handle both \r\n and \n)
		size_t line_end_len = 0;
		size_t line_end = FindLineEnd(chunked_body, pos, line_end_len);
		if (line_end == std::string::npos) {
			break;
		}

		// Parse chunk size (hexadecimal)
		std::string size_str = chunked_body.substr(pos, line_end - pos);

		// Remove any chunk extensions (after semicolon)
		size_t semicolon = size_str.find(';');
		if (semicolon != std::string::npos) {
			size_str = size_str.substr(0, semicolon);
		}

		// Trim whitespace (including \r if present)
		while (!size_str.empty() && (std::isspace(size_str.front()) || size_str.front() == '\r')) {
			size_str.erase(0, 1);
		}
		while (!size_str.empty() && (std::isspace(size_str.back()) || size_str.back() == '\r')) {
			size_str.pop_back();
		}

		if (size_str.empty()) {
			pos = line_end + line_end_len;
			continue;
		}

		// Convert hex to integer
		size_t chunk_size = 0;
		try {
			chunk_size = std::stoul(size_str, nullptr, 16);
		} catch (...) {
			// If we can't parse the chunk size, return what we have
			break;
		}

		// Size of 0 indicates end of chunks
		if (chunk_size == 0) {
			break;
		}

		// Move past the size line
		pos = line_end + line_end_len;

		// Check if we have enough data for this chunk
		if (pos + chunk_size > chunked_body.size()) {
			// Incomplete chunk, append what we can
			result += chunked_body.substr(pos);
			break;
		}

		// Append the chunk data
		result += chunked_body.substr(pos, chunk_size);

		// Move past chunk data and trailing line ending
		pos += chunk_size;
		// Skip the trailing \r\n or \n after chunk data
		if (pos < chunked_body.size() && chunked_body[pos] == '\r') {
			pos++;
		}
		if (pos < chunked_body.size() && chunked_body[pos] == '\n') {
			pos++;
		}
	}

	return result;
}

// Helper: Extract body from HTTP response
static std::string ExtractBody(const std::string &response) {
	// Try \r\n\r\n first (standard HTTP)
	size_t header_end = response.find("\r\n\r\n");
	size_t body_start_offset = 4;

	// Fall back to \n\n if standard not found
	if (header_end == std::string::npos) {
		header_end = response.find("\n\n");
		body_start_offset = 2;
	}

	if (header_end == std::string::npos) {
		return "";
	}

	std::string body = response.substr(header_end + body_start_offset);

	// Check if chunked encoding is used
	if (IsChunkedEncoding(response)) {
		return DecodeChunkedBody(body);
	}

	return body;
}

static std::string ExtractHeaderValue(const std::string &response, const std::string &header_name) {
	size_t header_end = response.find("\r\n\r\n");
	size_t line_ending_size = 2;
	if (header_end == std::string::npos) {
		header_end = response.find("\n\n");
		line_ending_size = 1;
	}
	if (header_end == std::string::npos) {
		return "";
	}

	std::string headers = response.substr(0, header_end);
	std::string needle = header_name;
	for (char &c : needle) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	needle += ":";

	size_t pos = 0;
	while (pos < headers.size()) {
		size_t line_end = headers.find(line_ending_size == 2 ? "\r\n" : "\n", pos);
		if (line_end == std::string::npos) {
			line_end = headers.size();
		}

		std::string line = headers.substr(pos, line_end - pos);
		std::string line_lower = line;
		for (char &c : line_lower) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		if (line_lower.rfind(needle, 0) == 0) {
			std::string value = line.substr(header_name.size() + 1);
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
				value.erase(0, 1);
			}
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
				value.pop_back();
			}
			return value;
		}

		pos = line_end + line_ending_size;
	}

	return "";
}

struct ParsedHttpsUrl {
	std::string host;
	std::string path;
};

static ParsedHttpsUrl ParseHttpsUrl(const std::string &url) {
	const std::string https_prefix = "https://";
	if (url.rfind(https_prefix, 0) != 0) {
		throw IOException("Only HTTPS redirect URLs are supported: " + url);
	}

	size_t host_start = https_prefix.size();
	size_t path_start = url.find('/', host_start);

	ParsedHttpsUrl result;
	if (path_start == std::string::npos) {
		result.host = url.substr(host_start);
		result.path = "/";
	} else {
		result.host = url.substr(host_start, path_start - host_start);
		result.path = url.substr(path_start);
	}

	if (result.host.empty()) {
		throw IOException("Redirect URL is missing a host: " + url);
	}

	return result;
}

static std::string PerformHttpsRequestInternal(const std::string &host, const std::string &path,
                                               const std::string &token, HttpMethod method, const std::string &body,
                                               const std::string &content_type, const std::string &accept,
                                               int redirect_count) {
	if (redirect_count > 5) {
		throw IOException("Too many HTTP redirects while requesting " + host + path);
	}

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
			case HttpMethod::DELETE_REQUEST:
				request << "DELETE ";
				break;
			}
			request << path << " HTTP/1.1\r\n";

			// Headers
			request << "Host: " << host << "\r\n";
			if (!token.empty()) {
				request << "Authorization: Bearer " << token << "\r\n";
			}
			request << "Accept: " << accept << "\r\n";
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

			while ((read_bytes = BIO_read(bio, buffer, sizeof(buffer))) > 0) {
				response.append(buffer, read_bytes);
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
					backoff_seconds *= 2; // Exponential backoff
					continue;             // Retry
				} else {
					throw IOException("Rate limit exceeded (429) - too many requests");
				}
			}

			// 15. Follow redirects, which Graph commonly uses for /content downloads.
			if (status_code >= 300 && status_code < 400) {
				std::string location = ExtractHeaderValue(response, "Location");
				if (location.empty()) {
					throw IOException("HTTP " + std::to_string(status_code) + " redirect without a Location header");
				}

				ParsedHttpsUrl redirect_target;
				if (location.rfind("https://", 0) == 0) {
					redirect_target = ParseHttpsUrl(location);
				} else if (!location.empty() && location[0] == '/') {
					redirect_target = {host, location};
				} else {
					throw IOException("Unsupported redirect URL: " + location);
				}

				// Graph download redirects often point to a signed URL on another host.
				// Only forward the bearer token when staying on the same host.
				std::string redirect_token = redirect_target.host == host ? token : "";

				return PerformHttpsRequestInternal(redirect_target.host, redirect_target.path, redirect_token, method,
				                                   body, content_type, accept, redirect_count + 1);
			}

			// 16. Handle other HTTP errors
			if (status_code >= 400) {
				std::string response_body = ExtractBody(response);
				throw IOException("HTTP " + std::to_string(status_code) + ": " + response_body);
			}

			// 17. Return response body
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

std::string PerformHttpsRequest(const std::string &host, const std::string &path, const std::string &token,
                                HttpMethod method, const std::string &body, const std::string &content_type,
                                const std::string &accept) {
	return PerformHttpsRequestInternal(host, path, token, method, body, content_type, accept, 0);
}

// Call Microsoft Graph API to get list items
std::string CallGraphApiListItems(const std::string &site_id, const std::string &list_id, const std::string &token,
                                  const std::string &select_fields, const std::string &filter, int top) {
	std::ostringstream path;
	path << "/v1.0/sites/" << site_id << "/lists/" << list_id << "/items";
	path << "?$expand=fields";

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
std::string GetListMetadata(const std::string &site_id, const std::string &list_id, const std::string &token) {
	std::ostringstream path;
	path << "/v1.0/sites/" << site_id << "/lists/" << list_id;
	path << "?$expand=columns";

	return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);
}

// Get site information from URL
std::string GetSiteByUrl(const std::string &site_url, const std::string &token) {
	// URL encode the site URL
	// For simplicity, we'll handle this in utils module
	std::ostringstream path;
	path << "/v1.0/sites/root:/" << site_url;

	return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET);
}

// Get document library items
std::string GetLibraryItems(const std::string &site_id, const std::string &drive_id, const std::string &token,
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

// Download file content from SharePoint (binary data)
std::string DownloadSharepointFileContent(const std::string &site_id, const std::string &drive_id,
                                          const std::string &item_id, const std::string &token) {
	std::ostringstream path;
	path << "/v1.0/sites/" << site_id << "/drives/" << drive_id << "/items/" << item_id << "/content";

	// This returns the binary file content
	return PerformHttpsRequest("graph.microsoft.com", path.str(), token, HttpMethod::GET, "", "application/json",
	                           "*/*");
}

} // namespace duckdb
