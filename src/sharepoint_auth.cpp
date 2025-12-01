#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/common/types/value.hpp"
#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <unistd.h>
#endif

using json = nlohmann::json;

namespace duckdb {

// Constants for Azure AD OAuth
static const std::string AZURE_AUTH_URL = "https://login.microsoftonline.com";
static const std::string REDIRECT_URI = "http://localhost:8080/callback";
static const std::string TENANT_ID = "common";  // Use "common" for multi-tenant

// TODO: Replace with your Azure AD application client ID
static const std::string CLIENT_ID = "936651d5-f197-4ee7-9917-3d8d8d86a4d0";

// Scopes needed for SharePoint access (space-separated)
static const std::string SCOPES = "https://graph.microsoft.com/Sites.Read.All "
                                  "https://graph.microsoft.com/Files.Read.All "
                                  "offline_access";

// URL-encoded version for use in URLs and POST bodies
static const std::string SCOPES_ENCODED = "https%3A%2F%2Fgraph.microsoft.com%2FSites.Read.All%20"
                                          "https%3A%2F%2Fgraph.microsoft.com%2FFiles.Read.All%20"
                                          "offline_access";
static const std::string REDIRECT_URI_ENCODED = "http%3A%2F%2Flocalhost%3A8080%2Fcallback";

// Helper: Open URL in browser
static void OpenBrowser(const std::string &url) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif __APPLE__
    std::string command = "open \"" + url + "\"";
    system(command.c_str());
#else
    std::string command = "xdg-open \"" + url + "\" || firefox \"" + url + "\" || google-chrome \"" + url + "\"";
    system(command.c_str());
#endif
}

// Helper: Start local HTTP server to receive OAuth callback
static std::string StartCallbackServer(int port = 8080) {
    // This is a simplified version - in production, use a proper HTTP library
    // For now, we'll simulate this with user input

    std::cout << "\n=\n";
    std::cout << "After authorizing, you'll be redirected to:\n";
    std::cout << "http://localhost:8080/callback?code=...\n";
    std::cout << "\nCopy the ENTIRE URL from your browser and paste it here:\n";
    std::cout << "=\n";
    std::cout << "URL: ";

    std::string callback_url;
    std::getline(std::cin, callback_url);

    // Extract code parameter from URL
    size_t code_pos = callback_url.find("code=");
    if (code_pos == std::string::npos) {
        throw IOException("No authorization code found in URL");
    }

    size_t code_start = code_pos + 5;  // Length of "code="
    size_t code_end = callback_url.find("&", code_start);

    if (code_end == std::string::npos) {
        code_end = callback_url.length();
    }

    return callback_url.substr(code_start, code_end - code_start);
}

static vector<string> NormalizeSecretScope(const CreateSecretInput &input) {
    auto scope = input.scope;
    if (scope.empty()) {
        scope.emplace_back("");
    }
    return scope;
}

// Helper: Exchange authorization code for access token
static json ExchangeCodeForToken(const std::string &auth_code) {
    std::ostringstream body;
    body << "client_id=" << CLIENT_ID
         << "&scope=" << SCOPES_ENCODED
         << "&code=" << auth_code
         << "&redirect_uri=" << REDIRECT_URI_ENCODED
         << "&grant_type=authorization_code";

    std::string token_endpoint = "/" + TENANT_ID + "/oauth2/v2.0/token";
    
    // Debug logging
    std::cout << "DEBUG: Token endpoint: " << token_endpoint << std::endl;
    std::cout << "DEBUG: Request body: " << body.str() << std::endl;
    std::cout << "DEBUG: Auth code: " << auth_code << std::endl;

    std::string response = PerformHttpsRequest(
        "login.microsoftonline.com",
        token_endpoint,
        "",  // No token needed for this request
        HttpMethod::POST,
        body.str(),
        "application/x-www-form-urlencoded"
    );
    
    // Debug response
    std::cout << "DEBUG: Response: " << response << std::endl;

    return json::parse(response);
}

// Create secret from OAuth flow
static unique_ptr<BaseSecret> CreateSharepointSecretFromOAuth(ClientContext &context, CreateSecretInput &input) {

    std::cout << "\n= SharePoint OAuth Authentication =\n\n";

    // 1. Generate authorization URL
    std::ostringstream auth_url;
    auth_url << AZURE_AUTH_URL << "/" << TENANT_ID << "/oauth2/v2.0/authorize"
             << "?client_id=" << CLIENT_ID
             << "&response_type=code"
             << "&redirect_uri=" << REDIRECT_URI
             << "&scope=" << SCOPES_ENCODED
             << "&response_mode=query";

    std::string url = auth_url.str();

    std::cout << "Opening browser for authentication...\n";
    std::cout << "If the browser doesn't open, visit this URL:\n";
    std::cout << url << "\n\n";
    
    // Debug: Show the authorization URL
    std::cout << "DEBUG: Authorization URL: " << url << std::endl;

    // 2. Open browser
    OpenBrowser(url);

    // 3. Wait for callback
    std::string auth_code = StartCallbackServer();

    std::cout << "\nExchanging authorization code for token...\n";

    // 4. Exchange code for token
    json token_response = ExchangeCodeForToken(auth_code);

    // 5. Extract tokens
    std::string access_token = token_response["access_token"];
    std::string refresh_token = token_response.value("refresh_token", "");
    int expires_in = token_response.value("expires_in", 3600);

    // 6. Calculate expiration time
    auto now = std::chrono::system_clock::now();
    auto expiration = now + std::chrono::seconds(expires_in);
    auto expiration_time = std::chrono::system_clock::to_time_t(expiration);

    std::cout << "✓ Authentication successful!\n";
    std::cout << "Token expires in " << expires_in << " seconds\n\n";

    // 7. Create DuckDB secret
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

    // 2. Find SharePoint secret
    auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secret_entry = secret_manager.GetSecretByName(transaction, "sharepoint");

    if (!secret_entry) {
        throw IOException(
            "No SharePoint credentials found. Create a secret first:\n"
            "  CREATE SECRET (TYPE sharepoint, PROVIDER oauth);\n"
            "Or provide a token:\n"
            "  CREATE SECRET (TYPE sharepoint, TOKEN 'your-token-here');"
        );
    }

    // 3. Get the secret
    auto secret_ptr = secret_entry->secret.get();
    if (!secret_ptr) {
        throw IOException("Invalid SharePoint secret entry");
    }
    auto kv_secret = dynamic_cast<const KeyValueSecret*>(secret_ptr);

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