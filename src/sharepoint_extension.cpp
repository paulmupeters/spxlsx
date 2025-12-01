#define DUCKDB_EXTENSION_MAIN

#include "sharepoint_extension.hpp"
#include "sharepoint_auth.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace duckdb {


void RegisterSharepointReadFunction(ExtensionLoader &loader);

static void LoadInternal(ExtensionLoader &loader) {
    // Initialize OpenSSL (required for HTTPS)
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Register authentication functions
    RegisterSharepointAuthFunctions(loader);

}

void SharepointExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string SharepointExtension::Name() {
    return "sharepoint";
}

std::string SharepointExtension::Version() const {
    #ifdef EXT_VERSION_SHAREPOINT
        return EXT_VERSION_SHAREPOINT;
    #else
        return "";
    #endif
}

} // namespace duckdb

// This macro creates the extension entry point
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(sharepoint, loader) {
    duckdb::LoadInternal(loader);
}

}
