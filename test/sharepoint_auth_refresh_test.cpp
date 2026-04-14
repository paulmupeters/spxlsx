#include "sharepoint_oauth.hpp"

#include <cassert>
#include <chrono>
#include <string>

using namespace duckdb;

static void TestRefreshBodyEncoding() {
	const std::string body = BuildRefreshTokenRequestBody("refresh+/=? token");
	assert(body.find("grant_type=refresh_token") != std::string::npos);
	assert(body.find("client_id=") != std::string::npos);
	assert(body.find("scope=") != std::string::npos);
	assert(body.find("refresh%2B%2F%3D%3F%20token") != std::string::npos);
}

static void TestRefreshResponseKeepsExistingRefreshTokenWhenRotatedTokenMissing() {
	const auto now = std::chrono::system_clock::from_time_t(1700000000);
	const auto refreshed = ParseRefreshTokenResponse(
	    R"({"access_token":"new-access","expires_in":7200})", "existing-refresh", now);

	assert(refreshed.access_token == "new-access");
	assert(refreshed.refresh_token == "existing-refresh");
	assert(refreshed.expires_at == 1700007200);
}

static void TestRefreshResponseUsesReturnedRefreshToken() {
	const auto now = std::chrono::system_clock::from_time_t(1700000000);
	const auto refreshed = ParseRefreshTokenResponse(
	    R"({"access_token":"new-access","refresh_token":"rotated-refresh","expires_in":3600})", "old-refresh", now);

	assert(refreshed.access_token == "new-access");
	assert(refreshed.refresh_token == "rotated-refresh");
	assert(refreshed.expires_at == 1700003600);
}

int main() {
	TestRefreshBodyEncoding();
	TestRefreshResponseKeepsExistingRefreshTokenWhenRotatedTokenMissing();
	TestRefreshResponseUsesReturnedRefreshToken();
	return 0;
}
