#include "sharepoint_requests.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <sstream>

namespace duckdb {

struct ParsedHttpsUrl {
	std::string host;
	std::string path;
};

static ParsedHttpsUrl ParseHttpsUrl(const std::string &url) {
	const std::string https_prefix = "https://";
	if (url.rfind(https_prefix, 0) != 0) {
		throw IOException("Only HTTPS URLs are supported: " + url);
	}

	const auto host_start = https_prefix.size();
	const auto path_start = url.find('/', host_start);

	ParsedHttpsUrl result;
	if (path_start == std::string::npos) {
		result.host = url.substr(host_start);
		result.path = "/";
	} else {
		result.host = url.substr(host_start, path_start - host_start);
		result.path = url.substr(path_start);
	}

	if (result.host.empty()) {
		throw IOException("HTTPS URL is missing a host: " + url);
	}
	return result;
}

static unique_ptr<HTTPResponse> ExecuteRequest(HTTPUtil &http_util, HTTPParams &params, const std::string &url,
                                               const HTTPHeaders &headers, HttpMethod method, const std::string &body,
                                               const std::string &content_type) {
	unique_ptr<HTTPResponse> response;
	switch (method) {
	case HttpMethod::GET: {
		GetRequestInfo request(url, headers, params, nullptr, nullptr);
		request.try_request = true;
		response = http_util.Request(request);
		break;
	}
	case HttpMethod::POST: {
		PostRequestInfo request(url, headers, params, const_data_ptr_cast(body.data()), body.size());
		request.try_request = true;
		response = http_util.Request(request);
		if (response && !request.buffer_out.empty()) {
			response->body = std::move(request.buffer_out);
		}
		break;
	}
	case HttpMethod::PUT: {
		PutRequestInfo request(url, headers, params, const_data_ptr_cast(body.data()), body.size(), content_type);
		request.try_request = true;
		response = http_util.Request(request);
		break;
	}
	case HttpMethod::DELETE_REQUEST: {
		DeleteRequestInfo request(url, headers, params);
		request.try_request = true;
		response = http_util.Request(request);
		break;
	}
	}
	return response;
}

static std::string PerformHttpsRequestInternal(ClientContext &context, const std::string &host, const std::string &path,
                                               const std::string &token, HttpMethod method, const std::string &body,
                                               const std::string &content_type, const std::string &accept,
                                               idx_t redirect_count) {
	if (redirect_count > 5) {
		throw IOException("Too many HTTP redirects while requesting " + host + path);
	}

	const std::string url = "https://" + host + (path.empty() || path[0] != '/' ? "/" : "") + path;
	auto &http_util = HTTPUtil::Get(DatabaseInstance::GetDatabase(context));
	auto params = http_util.InitializeParameters(context, url);
	// Native clients expose redirects to us so credentials can be stripped on cross-host redirects.
	// Browser XHR follows redirects itself and applies the browser's credential/header rules.
	params->follow_location = false;

	HTTPHeaders headers;
	if (!token.empty()) {
		headers.Insert("Authorization", "Bearer " + token);
	}
	headers.Insert("Accept", accept);
	headers.Insert("User-Agent", "DuckDB-SharePoint-Extension/1.0");
	if (!body.empty() || method == HttpMethod::POST || method == HttpMethod::PUT) {
		headers.Insert("Content-Type", content_type);
	}

	auto response = ExecuteRequest(http_util, *params, url, headers, method, body, content_type);
	if (!response) {
		throw IOException("Empty HTTP response while requesting " + url);
	}
	if (response->HasRequestError()) {
		throw IOException(response->GetRequestError());
	}

	const auto status_code = static_cast<uint16_t>(response->status);
	if (status_code >= 300 && status_code < 400) {
		if (!response->HasHeader("Location")) {
			throw IOException("HTTP " + std::to_string(status_code) + " redirect without a Location header");
		}

		const auto location = response->GetHeaderValue("Location");
		ParsedHttpsUrl redirect_target;
		if (location.rfind("https://", 0) == 0) {
			redirect_target = ParseHttpsUrl(location);
		} else if (!location.empty() && location[0] == '/') {
			redirect_target = {host, location};
		} else {
			throw IOException("Unsupported redirect URL: " + location);
		}

		const auto redirect_token = redirect_target.host == host ? token : "";
		return PerformHttpsRequestInternal(context, redirect_target.host, redirect_target.path, redirect_token, method,
		                                   body, content_type, accept, redirect_count + 1);
	}

	if (status_code >= 400 || !response->Success()) {
		throw IOException("HTTP " + std::to_string(status_code) + ": " + response->body);
	}
	return response->body;
}

std::string PerformHttpsRequest(ClientContext &context, const std::string &host, const std::string &path,
                                const std::string &token, HttpMethod method, const std::string &body,
                                const std::string &content_type, const std::string &accept) {
	return PerformHttpsRequestInternal(context, host, path, token, method, body, content_type, accept, 0);
}

std::string CallGraphApiListItems(ClientContext &context, const std::string &site_id, const std::string &list_id,
                                  const std::string &token, const std::string &select_fields, const std::string &filter,
                                  int top) {
	std::ostringstream path;
	path << "/v1.0/sites/" << site_id << "/lists/" << list_id << "/items?$expand=fields";
	if (!select_fields.empty()) {
		path << "&$select=" << select_fields;
	}
	if (!filter.empty()) {
		path << "&$filter=" << filter;
	}
	if (top > 0) {
		path << "&$top=" << top;
	}
	return PerformHttpsRequest(context, "graph.microsoft.com", path.str(), token);
}

std::string GetListMetadata(ClientContext &context, const std::string &site_id, const std::string &list_id,
                            const std::string &token) {
	return PerformHttpsRequest(context, "graph.microsoft.com",
	                           "/v1.0/sites/" + site_id + "/lists/" + list_id + "?$expand=columns", token);
}

std::string GetSiteByUrl(ClientContext &context, const std::string &site_url, const std::string &token) {
	return PerformHttpsRequest(context, "graph.microsoft.com", "/v1.0/sites/root:/" + site_url, token);
}

std::string GetLibraryItems(ClientContext &context, const std::string &site_id, const std::string &drive_id,
                            const std::string &token, const std::string &folder_path) {
	std::string path = "/v1.0/sites/" + site_id + "/drives/" + drive_id;
	path += folder_path.empty() ? "/root/children" : "/root:/" + folder_path + ":/children";
	return PerformHttpsRequest(context, "graph.microsoft.com", path, token);
}

std::string DownloadSharepointFileContent(ClientContext &context, const std::string &site_id,
                                          const std::string &drive_id, const std::string &item_id,
                                          const std::string &token) {
	const auto path = "/v1.0/sites/" + site_id + "/drives/" + drive_id + "/items/" + item_id + "/content";
	return PerformHttpsRequest(context, "graph.microsoft.com", path, token, HttpMethod::GET, "", "application/json",
	                           "*/*");
}

} // namespace duckdb
