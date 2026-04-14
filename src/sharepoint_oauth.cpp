#include "sharepoint_oauth.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace duckdb {

static const std::string CLIENT_ID = "936651d5-f197-4ee7-9917-3d8d8d86a4d0";
static const std::string SCOPES = "https://graph.microsoft.com/Sites.Read.All "
                                  "https://graph.microsoft.com/Files.Read.All "
                                  "offline_access";

static std::string UrlEncode(const std::string &value) {
	std::ostringstream encoded;
	encoded << std::uppercase << std::hex;

	for (unsigned char ch : value) {
		if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
			encoded << static_cast<char>(ch);
		} else {
			encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
		}
	}

	return encoded.str();
}

std::string BuildRefreshTokenRequestBody(const std::string &refresh_token) {
	std::ostringstream body;
	body << "grant_type=refresh_token"
	     << "&client_id=" << CLIENT_ID
	     << "&scope=" << UrlEncode(SCOPES)
	     << "&refresh_token=" << UrlEncode(refresh_token);
	return body.str();
}

SharepointRefreshTokenResult ParseRefreshTokenResponse(const std::string &response_body,
                                                       const std::string &existing_refresh_token,
                                                       std::chrono::system_clock::time_point now) {
	const auto response = json::parse(response_body);

	const auto access_token_it = response.find("access_token");
	if (access_token_it == response.end() || !access_token_it->is_string() || access_token_it->get<std::string>().empty()) {
		throw std::runtime_error("Refresh token response did not include an access_token.");
	}

	const auto expires_in_it = response.find("expires_in");
	if (expires_in_it == response.end() || !expires_in_it->is_number_integer()) {
		throw std::runtime_error("Refresh token response did not include a valid expires_in.");
	}

	const auto expires_in = expires_in_it->get<int64_t>();
	if (expires_in <= 0) {
		throw std::runtime_error("Refresh token response returned a non-positive expires_in.");
	}

	SharepointRefreshTokenResult result;
	result.access_token = access_token_it->get<std::string>();
	result.refresh_token = response.value("refresh_token", existing_refresh_token);
	result.expires_at =
	    std::chrono::system_clock::to_time_t(now + std::chrono::seconds(expires_in));
	return result;
}

} // namespace duckdb
