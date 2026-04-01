#define DUCKDB_EXTENSION_MAIN

#include "sharepoint_extension.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_read.hpp"
#include "sharepoint_excel.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace duckdb {

// Define read_sharepoint_excel as a table macro wrapping sharepoint_download_excel + read_xlsx
static const DefaultTableMacro sharepoint_table_macros[] = {
    {DEFAULT_SCHEMA, "read_sharepoint_excel", {"url", nullptr},
     {{"sheet", "''"}, {"header", "true"}, {"all_varchar", "false"}, {"ignore_errors", "false"}, {nullptr, nullptr}},
     R"(SELECT * FROM read_xlsx(sharepoint_download_excel(url), sheet := sheet, header := header, all_varchar := all_varchar, ignore_errors := ignore_errors))"},
    {nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}
};

static void LoadInternal(ExtensionLoader &loader) {
    // Initialize OpenSSL (required for HTTPS)
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Register authentication functions
    RegisterSharepointAuthFunctions(loader);

    // Register table functions
    RegisterSharepointReadFunction(loader);
    
    // Register Excel integration scalar function (sharepoint_download_excel)
    RegisterSharepointExcelFunction(loader);
    
    // Register table macros (read_sharepoint_excel)
    for (idx_t index = 0; sharepoint_table_macros[index].name != nullptr; index++) {
        auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(sharepoint_table_macros[index]);
        loader.RegisterFunction(*info);
    }
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
