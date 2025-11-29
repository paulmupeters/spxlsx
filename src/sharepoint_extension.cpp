#define DUCKDB_EXTENSION_MAIN

#include "sharepoint_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace duckdb {

// Forward declarations (we'll implement these in later modules)
void RegisterSharepointReadFunction(DatabaseInstance &db);
void RegisterSharepointAuthFunctions(DatabaseInstance &db);

// This is called when the extension is loaded
static void LoadInternal(ExtensionLoader &loader) {
    // Initialize OpenSSL (required for HTTPS)
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Register functions (we'll add these in later modules)
    // RegisterSharepointReadFunction(instance);
    // RegisterSharepointAuthFunctions(instance);

    // For now, just log that we loaded successfully
    // In production, you'd register actual functions here
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