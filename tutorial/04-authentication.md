# 04 - Authentication

In this module, you'll implement Azure AD OAuth authentication to get access tokens for SharePoint. This is the most complex part of the extension, but we'll break it down step by step.

## Goals
- Understand Azure AD OAuth flow
- Register an Azure AD application
- Implement interactive OAuth with browser redirect
- Integrate with DuckDB's secret manager
- Handle token refresh and caching

## Overview: OAuth Flow

```
┌─────────────────────────────────────────────────────────────┐
│                    OAuth 2.0 Flow                           │
└─────────────────────────────────────────────────────────────┘

1. User runs: CREATE SECRET sp_secret (TYPE sharepoint, PROVIDER oauth);

2. Extension generates authorization URL
   └→ Opens browser to Azure AD login page

3. User logs in and grants permissions
   └→ Azure AD redirects to callback URL with auth code

4. Extension exchanges auth code for access token
   └→ Stores token in DuckDB secret manager

5. User queries: SELECT * FROM read_sharepoint('...')
   └→ Extension retrieves token from secret manager
   └→ Uses token for API requests
```

## Step 1: Register Azure AD Application

Before coding, you need to register an app in Azure AD:

### 1.1: Go to Azure Portal

Visit: https://portal.azure.com
Navigate to: **Azure Active Directory** → **App registrations** → **New registration**

### 1.2: Configure the App

- **Name**: "DuckDB SharePoint Extension"
- **Supported account types**: "Accounts in any organizational directory and personal Microsoft accounts"
- **Redirect URI**:
  - Platform: **Web**
  - URI: `http://localhost:8080/callback`

Click **Register**.

### 1.3: Note Your Credentials

After registration, note these values:
- **Application (client) ID**: e.g., `12345678-1234-1234-1234-123456789abc`
- **Directory (tenant) ID**: e.g., `87654321-4321-4321-4321-987654321cba`

### 1.4: Configure API Permissions

Go to **API permissions** → **Add a permission**:

1. Select **Microsoft Graph**
2. Select **Delegated permissions**
3. Add these permissions:
   - `Sites.Read.All` - Read items in all site collections
   - `Files.Read.All` - Read files (for document libraries)
   - `User.Read` - Sign in and read user profile

Click **Add permissions**.

For production, ask admin to **Grant admin consent** (or users will see a consent prompt).

### 1.5: Configure Authentication

Go to **Authentication**:
- Enable **Access tokens** and **ID tokens** under "Implicit grant and hybrid flows"
- Set **Allow public client flows**: Yes (for device code flow - future enhancement)

Save changes.

## Step 2: Implement Secret Types

Update `src/sharepoint_auth.cpp` to integrate with DuckDB's secret manager:

```cpp
#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/common/types/value.hpp"
#include "third_party/json.hpp"

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
static const std::string CLIENT_ID = "YOUR_CLIENT_ID_HERE";

// Scopes needed for SharePoint access
static const std::string SCOPES = "https://graph.microsoft.com/Sites.Read.All "
                                  "https://graph.microsoft.com/Files.Read.All "
                                  "offline_access";

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

    std::cout << "\n=================================================\n";
    std::cout << "After authorizing, you'll be redirected to:\n";
    std::cout << "http://localhost:8080/callback?code=...\n";
    std::cout << "\nCopy the ENTIRE URL from your browser and paste it here:\n";
    std::cout << "=================================================\n";
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

// Helper: Exchange authorization code for access token
static json ExchangeCodeForToken(const std::string &auth_code) {
    std::ostringstream body;
    body << "client_id=" << CLIENT_ID
         << "&scope=" << SCOPES
         << "&code=" << auth_code
         << "&redirect_uri=" << REDIRECT_URI
         << "&grant_type=authorization_code";

    std::string token_endpoint = "/" + TENANT_ID + "/oauth2/v2.0/token";

    std::string response = PerformHttpsRequest(
        "login.microsoftonline.com",
        token_endpoint,
        "",  // No token needed for this request
        HttpMethod::POST,
        body.str()
    );

    return json::parse(response);
}

// Create secret from OAuth flow
static unique_ptr<BaseSecret> CreateSharepointSecretFromOAuth(
    ClientContext &context,
    const std::string &secret_name) {

    std::cout << "\n=== SharePoint OAuth Authentication ===\n\n";

    // 1. Generate authorization URL
    std::ostringstream auth_url;
    auth_url << AZURE_AUTH_URL << "/" << TENANT_ID << "/oauth2/v2.0/authorize"
             << "?client_id=" << CLIENT_ID
             << "&response_type=code"
             << "&redirect_uri=" << REDIRECT_URI
             << "&scope=" << SCOPES
             << "&response_mode=query";

    std::string url = auth_url.str();

    std::cout << "Opening browser for authentication...\n";
    std::cout << "If the browser doesn't open, visit this URL:\n";
    std::cout << url << "\n\n";

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
    auto secret = make_uniq<KeyValueSecret>(secret_name, "sharepoint", "oauth");
    secret->secret_map["access_token"] = access_token;
    secret->secret_map["refresh_token"] = refresh_token;
    secret->secret_map["expires_at"] = std::to_string(expiration_time);

    return secret;
}

// Create secret from manual access token
static unique_ptr<BaseSecret> CreateSharepointSecretFromToken(
    ClientContext &context,
    const std::string &secret_name,
    const std::string &access_token) {

    auto secret = make_uniq<KeyValueSecret>(secret_name, "sharepoint", "token");
    secret->secret_map["access_token"] = access_token;

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
    auto secret = secret_entry->secret;
    auto kv_secret = dynamic_cast<KeyValueSecret*>(secret.get());

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
void RegisterSharepointAuthFunctions(DatabaseInstance &db) {
    // Register secret type
    SecretType secret_type;
    secret_type.name = "sharepoint";
    secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
    secret_type.default_provider = "oauth";

    ExtensionUtil::RegisterSecretType(db, secret_type);

    // Register OAuth provider
    CreateSecretFunction oauth_function = {
        "oauth",
        [](ClientContext &context, CreateSecretInput &input) -> unique_ptr<BaseSecret> {
            return CreateSharepointSecretFromOAuth(context, input.name);
        }
    };

    ExtensionUtil::RegisterCreateSecretFunction(db, oauth_function);

    // Register manual token provider
    CreateSecretFunction token_function = {
        "token",
        [](ClientContext &context, CreateSecretInput &input) -> unique_ptr<BaseSecret> {
            auto token_it = input.options.find("token");
            if (token_it == input.options.end()) {
                throw InvalidInputException("TOKEN option is required for token provider");
            }
            return CreateSharepointSecretFromToken(context, input.name, token_it->second.ToString());
        }
    };

    ExtensionUtil::RegisterCreateSecretFunction(db, token_function);
}

} // namespace duckdb
```

Update the header `src/include/sharepoint_auth.hpp`:

```cpp
#pragma once

#include "duckdb.hpp"

namespace duckdb {

class SharepointAuth {
public:
    // Get access token from secret manager
    // Throws exception if no valid credentials found
    static std::string GetAccessToken(ClientContext &context);
};

// Register authentication functions with DuckDB
void RegisterSharepointAuthFunctions(DatabaseInstance &db);

} // namespace duckdb
```

## Step 3: Update Extension Entry Point

Update `src/sharepoint_extension.cpp` to register auth functions:

```cpp
#include "sharepoint_extension.hpp"
#include "sharepoint_auth.hpp"
// ... other includes ...

static void LoadInternal(DatabaseInstance &instance) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Register authentication functions
    RegisterSharepointAuthFunctions(instance);

    // Register other functions (coming in next modules)
    // RegisterSharepointReadFunction(instance);
}
```

## Step 4: Configure Your Client ID

**IMPORTANT**: Replace `YOUR_CLIENT_ID_HERE` in the code above with your actual Azure AD Application (client) ID from Step 1.

In `src/sharepoint_auth.cpp`, find this line:

```cpp
static const std::string CLIENT_ID = "YOUR_CLIENT_ID_HERE";
```

Replace with your client ID:

```cpp
static const std::string CLIENT_ID = "12345678-1234-1234-1234-123456789abc";
```

## Step 5: Test Authentication

Build and test:

```bash
make

cd duckdb
./build/release/duckdb
```

In DuckDB:

```sql
-- Load extension
LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension';

-- Create secret with OAuth
CREATE SECRET sharepoint_secret (TYPE sharepoint, PROVIDER oauth);
```

You should see:
1. Browser opens to Azure login page
2. You log in with your Microsoft account
3. You grant permissions to the app
4. You're redirected to `http://localhost:8080/callback?code=...`
5. Copy the URL and paste it into DuckDB
6. Extension exchanges code for token
7. Success message appears

```sql
-- Verify secret was created
SELECT name, type, provider FROM duckdb_secrets();
```

## Step 6: Alternative - Manual Token (Simpler for Testing)

For quick testing, you can use Graph Explorer to get a token manually:

1. Visit: https://developer.microsoft.com/en-us/graph/graph-explorer
2. Sign in
3. Click **Access token** in the sidebar
4. Copy the token

Then in DuckDB:

```sql
CREATE SECRET sharepoint_secret (
    TYPE sharepoint,
    PROVIDER token,
    TOKEN 'eyJ0eXAiOiJKV1QiLCJub...'  -- Your token here
);
```

## Step 7: Implement Token Refresh (Advanced)

To implement token refresh, add this function to `src/sharepoint_auth.cpp`:

```cpp
static std::string RefreshAccessToken(const std::string &refresh_token) {
    std::ostringstream body;
    body << "client_id=" << CLIENT_ID
         << "&scope=" << SCOPES
         << "&refresh_token=" << refresh_token
         << "&grant_type=refresh_token";

    std::string token_endpoint = "/" + TENANT_ID + "/oauth2/v2.0/token";

    std::string response = PerformHttpsRequest(
        "login.microsoftonline.com",
        token_endpoint,
        "",
        HttpMethod::POST,
        body.str()
    );

    json token_response = json::parse(response);
    return token_response["access_token"];
}
```

Then update `GetAccessToken()` to call this when the token expires.

## Common Issues

### Issue: Browser doesn't open

**Solution**: Manually copy the URL printed in the console and open it in your browser.

### Issue: Redirect URI mismatch

**Error**: "The redirect URI '...' specified in the request does not match..."

**Solution**: Make sure the redirect URI in your code matches exactly what's registered in Azure AD (including http vs https, trailing slashes, etc.).

### Issue: Permission denied

**Error**: "Insufficient privileges to complete the operation"

**Solution**:
1. Check that you've added the correct API permissions in Azure AD
2. Ask your admin to grant consent for the permissions
3. Try logging in with an account that has admin privileges

### Issue: Token expired

**Solution**: Implement token refresh or re-authenticate:

```sql
DROP SECRET sharepoint_secret;
CREATE SECRET sharepoint_secret (TYPE sharepoint, PROVIDER oauth);
```

## Security Best Practices

1. **Never hardcode tokens** in your code
2. **Use HTTPS** for redirect URIs in production
3. **Store secrets securely** - DuckDB encrypts secrets at rest
4. **Implement token refresh** to avoid repeated logins
5. **Use least privilege** - only request permissions you need
6. **Validate redirect URIs** - check they match expected values

## What You've Accomplished

You now have:
- Azure AD application configured
- OAuth flow implemented
- DuckDB secret manager integration
- Token retrieval and validation
- Manual token provider for testing

In the next module, you'll use these tokens to actually read data from SharePoint Lists!

---

**Navigation:**
- Previous: [03 - HTTP Layer](./03-http-layer.md)
- Next: [05 - SharePoint Lists](./05-sharepoint-lists.md)
