#include "sharepoint_auth.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

std::string SharepointAuth::GetAccessToken(ClientContext &context) {
    throw NotImplementedException("SharePoint authentication not yet implemented");
}

} // namespace duckdb