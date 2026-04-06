#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/common/types/value.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

using json = nlohmann::json;

namespace duckdb {

static const std::string TENANT_ID = "common"; // Use "common" for multi-tenant

// Client ID for the extension’s multi-tenant public client app.
static const std::string CLIENT_ID = "936651d5-f197-4ee7-9917-3d8d8d86a4d0";

// Scopes needed for SharePoint access (space-separated)
static const std::string SCOPES = "https://graph.microsoft.com/Sites.Read.All "
                                  "https://graph.microsoft.com/Files.Read.All "
                                  "offline_access";

// URL-encoded version for use in URLs and POST bodies
static const std::string SCOPES_ENCODED = "https%3A%2F%2Fgraph.microsoft.com%2FSites.Read.All%20"
                                          "https%3A%2F%2Fgraph.microsoft.com%2FFiles.Read.All%20"
                                          "offline_access";

static vector<string> NormalizeSecretScope(const CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope.emplace_back("");
	}
	return scope;
}

static json ParseJsonResponse(const std::string &response, const std::string &context) {
	try {
		return json::parse(response);
	} catch (const std::exception &ex) {
		throw IOException("Failed to parse " + context + " response: " + std::string(ex.what()));
	}
}

static std::string ExtractHttpErrorBody(const std::exception &ex) {
	std::string message = ex.what();
	if (!message.empty() && message.front() == '{') {
		try {
			auto exception_json = json::parse(message);
			auto exception_message = exception_json.value("exception_message", "");
			if (!exception_message.empty()) {
				message = std::move(exception_message);
			}
		} catch (...) {
		}
	}

	const std::string http_marker = "HTTP ";
	auto http_pos = message.find(http_marker);
	if (http_pos == std::string::npos) {
		return "";
	}

	auto separator = message.find(": ", http_pos);
	if (separator == std::string::npos || separator + 2 > message.size()) {
		return "";
	}

	return message.substr(separator + 2);
}

static json RequestDeviceCode() {
	std::ostringstream body;
	body << "client_id=" << CLIENT_ID << "&scope=" << SCOPES_ENCODED;

	std::string device_code_endpoint = "/" + TENANT_ID + "/oauth2/v2.0/devicecode";

	std::string response = PerformHttpsRequest("login.microsoftonline.com", device_code_endpoint,
	                                           "", // No token needed for this request
	                                           HttpMethod::POST, body.str(), "application/x-www-form-urlencoded");

	return ParseJsonResponse(response, "device code");
}

static json PollDeviceCodeToken(const std::string &device_code, int interval_seconds, int expires_in_seconds) {
	std::ostringstream body;
	body << "grant_type=urn:ietf:params:oauth:grant-type:device_code"
	     << "&client_id=" << CLIENT_ID << "&device_code=" << device_code;

	std::string token_endpoint = "/" + TENANT_ID + "/oauth2/v2.0/token";
	int poll_interval = std::max(1, interval_seconds);
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, expires_in_seconds));

	while (std::chrono::steady_clock::now() < deadline) {
		try {
			std::string response =
			    PerformHttpsRequest("login.microsoftonline.com", token_endpoint,
			                        "", // No bearer token needed during polling
			                        HttpMethod::POST, body.str(), "application/x-www-form-urlencoded");
			return ParseJsonResponse(response, "token");
		} catch (const std::exception &ex) {
			auto error_body = ExtractHttpErrorBody(ex);
			if (error_body.empty()) {
				throw;
			}

			auto error_response = ParseJsonResponse(error_body, "device code token error");
			auto error = error_response.value("error", "");
			auto error_description = error_response.value("error_description", "");

			if (error == "authorization_pending") {
				std::this_thread::sleep_for(std::chrono::seconds(poll_interval));
				continue;
			}
			if (error == "slow_down") {
				poll_interval += 5;
				std::this_thread::sleep_for(std::chrono::seconds(poll_interval));
				continue;
			}
			if (error == "authorization_declined") {
				throw IOException("Device code sign-in was declined. Please run CREATE SECRET again.");
			}
			if (error == "expired_token") {
				throw IOException("Device code expired before sign-in completed. Please run CREATE SECRET again.");
			}
			if (error == "bad_verification_code") {
				throw IOException("Microsoft rejected the device code during sign-in. Please run CREATE SECRET again.");
			}

			if (!error_description.empty()) {
				throw IOException("Device code sign-in failed: " + error_description);
			}
			if (!error.empty()) {
				throw IOException("Device code sign-in failed: " + error);
			}
			throw;
		}
	}

	throw IOException("Device code expired before sign-in completed. Please run CREATE SECRET again.");
}

// Create secret from OAuth flow
static unique_ptr<BaseSecret> CreateSharepointSecretFromOAuth(ClientContext &context, CreateSecretInput &input) {
	std::cout << "\n= SharePoint Device Code Authentication =\n\n";

	// 1. Request a device code from Microsoft
	json device_code_response = RequestDeviceCode();

	std::string device_code = device_code_response.value("device_code", "");
	std::string user_code = device_code_response.value("user_code", "");
	std::string verification_uri = device_code_response.value("verification_uri", "");
	std::string message = device_code_response.value("message", "");
	int interval = device_code_response.value("interval", 5);
	int expires_in = device_code_response.value("expires_in", 900);

	if (device_code.empty()) {
		throw IOException("Microsoft did not return a device code.");
	}

	// 2. Show the verification instructions to the user
	if (!message.empty()) {
		std::cout << message << "\n\n";
	} else {
		std::cout << "Open the Microsoft device login page and enter code: " << user_code << "\n\n";
		if (!verification_uri.empty()) {
			std::cout << "Verification URL: " << verification_uri << "\n\n";
		}
	}

	std::cout << "Waiting for Microsoft sign-in to complete...\n";

	// 3. Poll the token endpoint until the user finishes signing in
	json token_response = PollDeviceCodeToken(device_code, interval, expires_in);

	// 4. Extract tokens
	std::string access_token = token_response["access_token"];
	std::string refresh_token = token_response.value("refresh_token", "");
	int token_expires_in = token_response.value("expires_in", 3600);

	// 5. Calculate expiration time
	auto now = std::chrono::system_clock::now();
	auto expiration = now + std::chrono::seconds(token_expires_in);
	auto expiration_time = std::chrono::system_clock::to_time_t(expiration);

	std::cout << "Authentication successful.\n";
	std::cout << "Token expires in " << token_expires_in << " seconds\n\n";

	// 6. Create DuckDB secret
	auto scope = NormalizeSecretScope(input);
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->secret_map["access_token"] = access_token;
	secret->secret_map["refresh_token"] = refresh_token;
	secret->secret_map["expires_at"] = std::to_string(expiration_time);

	return secret;
}

// Create secret from manual access token
static unique_ptr<BaseSecret> CreateSharepointSecretFromToken(ClientContext &context, CreateSecretInput &input) {
	auto token_it = input.options.find("token");
	if (token_it == input.options.end()) {
		throw InvalidInputException("TOKEN option is required for token provider");
	}

	auto scope = NormalizeSecretScope(input);
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->secret_map["access_token"] = token_it->second.ToString();

	return secret;
}

// Get access token from secret (with refresh if needed)
std::string SharepointAuth::GetAccessToken(ClientContext &context) {
	// 1. Get secret manager
	auto &secret_manager = SecretManager::Get(context);

	// 2. Find SharePoint secret by type (not by name)
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_match = secret_manager.LookupSecret(transaction, "", "sharepoint");

	if (!secret_match.HasMatch()) {
		throw IOException("No SharePoint credentials found. Create a secret first:\n"
		                  "  CREATE SECRET (TYPE sharepoint, PROVIDER oauth);\n"
		                  "Or provide a token:\n"
		                  "  CREATE SECRET (TYPE sharepoint, TOKEN 'your-token-here');");
	}

	// 3. Get the secret
	auto &secret = secret_match.GetSecret();
	auto kv_secret = dynamic_cast<const KeyValueSecret *>(&secret);

	if (!kv_secret) {
		throw IOException("Invalid secret format");
	}

	// 4. Extract access token
	auto token_it = kv_secret->secret_map.find("access_token");
	if (token_it == kv_secret->secret_map.end()) {
		throw IOException("No access token in secret");
	}

	std::string access_token = token_it->second.ToString();

	// 5. Check if token is expired (if we have expiration info)
	auto expires_it = kv_secret->secret_map.find("expires_at");
	if (expires_it != kv_secret->secret_map.end()) {
		time_t expires_at = std::stol(expires_it->second.ToString());
		time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

		if (now >= expires_at) {
			// Token expired - try to refresh
			auto refresh_it = kv_secret->secret_map.find("refresh_token");
			if (refresh_it != kv_secret->secret_map.end()) {
				std::string refresh_token = refresh_it->second.ToString();
				// TODO: Implement token refresh
				throw IOException("Token expired. Please re-authenticate.");
			} else {
				throw IOException("Token expired and no refresh token available. Please re-authenticate.");
			}
		}
	}

	return access_token;
}

// Register secret functions with DuckDB
void RegisterSharepointAuthFunctions(ExtensionLoader &loader) {
	// Register secret type
	SecretType secret_type;
	secret_type.name = "sharepoint";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "oauth";

	loader.RegisterSecretType(secret_type);

	// Register OAuth provider
	CreateSecretFunction oauth_function = {"sharepoint", "oauth", CreateSharepointSecretFromOAuth};
	loader.RegisterFunction(oauth_function);

	// Register manual token provider
	CreateSecretFunction token_function = {"sharepoint", "token", CreateSharepointSecretFromToken};
	loader.RegisterFunction(token_function);
}

} // namespace duckdb
