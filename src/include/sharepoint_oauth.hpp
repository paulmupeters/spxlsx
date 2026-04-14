#pragma once

#include <chrono>
#include <ctime>
#include <string>

namespace duckdb {

struct SharepointRefreshTokenResult {
	std::string access_token;
	std::string refresh_token;
	std::time_t expires_at;
};

std::string BuildRefreshTokenRequestBody(const std::string &refresh_token);

SharepointRefreshTokenResult ParseRefreshTokenResponse(const std::string &response_body,
                                                       const std::string &existing_refresh_token,
                                                       std::chrono::system_clock::time_point now);

} // namespace duckdb
