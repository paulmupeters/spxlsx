#pragma once
#include "duckdb.hpp"
#include <string>

namespace duckdb {

class ClientContext;
class DatabaseInstance;

class SharepointAuth {
public:
	// Get access token from secret manager
	// Throws exception if no valid credentials found
	static std::string GetAccessToken(ClientContext &context);
};

// Register authentication functions with DuckDB
void RegisterSharepointAuthFunctions(ExtensionLoader &loader);

} // namespace duckdb
