#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Authentication and token management
class SharepointAuth {
public:
    // Get OAuth token (to be implemented)
    static std::string GetAccessToken(ClientContext &context);
};

} // namespace duckdb