#define DUCKDB_EXTENSION_MAIN

#include "sharepoint_extension.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_read.hpp"
#include "sharepoint_excel.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace duckdb {

static constexpr const char *MISSING_EXCEL_ERROR =
    "read_sharepoint_excel requires the DuckDB excel extension. Run INSTALL excel; LOAD excel; or enable "
    "autoinstall/autoload known extensions.";

static void SharepointRequireExcelScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)args;
	auto &db = *state.GetContext().db;
	if (!db.ExtensionIsLoaded("excel")) {
		ExtensionHelper::TryAutoLoadAvailableExtension(db, "excel");
	}
	if (!db.ExtensionIsLoaded("excel")) {
		throw InvalidInputException(MISSING_EXCEL_ERROR);
	}
	result.Reference(Value::BOOLEAN(true));
}

// Define read_sharepoint_excel as a table macro wrapping sharepoint_download_excel + read_xlsx
static const DefaultTableMacro sharepoint_table_macros[] = {
    {DEFAULT_SCHEMA,
     "read_sharepoint_excel",
     {"url", nullptr},
     {{"sheet", "''"},
      {"header", "true"},
      {"all_varchar", "false"},
      {"ignore_errors", "true"},
      {"range", "NULL"},
      {"stop_at_empty", "NULL"},
      {"empty_as_varchar", "NULL"},
      {nullptr, nullptr}},
     R"(SELECT *
        FROM query(
            CASE
                WHEN __sharepoint_require_excel() THEN
                    'SELECT * FROM read_xlsx(''' ||
                    replace(sharepoint_download_excel(url), '''', '''''') ||
                    '''' ||
                    CASE
                        WHEN sheet IS NULL OR sheet = '' THEN ''
                        ELSE ', sheet := ''' || replace(sheet, '''', '''''') || ''''
                    END ||
                    ', header := ' || CASE WHEN header THEN 'true' ELSE 'false' END ||
                    ', all_varchar := ' || CASE WHEN all_varchar THEN 'true' ELSE 'false' END ||
                    ', ignore_errors := ' || CASE WHEN ignore_errors THEN 'true' ELSE 'false' END ||
                    CASE
                        WHEN range IS NULL THEN ''
                        ELSE ', range := ''' || replace(range, '''', '''''') || ''''
                    END ||
                    CASE
                        WHEN stop_at_empty IS NULL THEN ''
                        ELSE ', stop_at_empty := ' || CASE WHEN stop_at_empty THEN 'true' ELSE 'false' END
                    END ||
                    CASE
                        WHEN empty_as_varchar IS NULL THEN ''
                        ELSE ', empty_as_varchar := ' || CASE WHEN empty_as_varchar THEN 'true' ELSE 'false' END
                    END ||
                    ')'
                ELSE
                    'SELECT 1 WHERE FALSE'
            END
        ))"},
    {nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}};

static void LoadInternal(ExtensionLoader &loader) {
	// Initialize OpenSSL (required for HTTPS)
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();

	// Excel powers read_sharepoint_excel, but sharepoint itself should still load if excel is not installed yet.
	// Try loading excel only when it is already available locally.
	if (!ExtensionHelper::TryAutoLoadAvailableExtension(loader.GetDatabaseInstance(), "excel")) {
		DUCKDB_LOG_WARNING(loader.GetDatabaseInstance(),
		                   "Could not load optional dependency 'excel'; Excel table functions will remain "
		                   "unavailable until it is installed");
	}

	// Register authentication functions
	RegisterSharepointAuthFunctions(loader);

	// Register table functions
	RegisterSharepointReadFunction(loader);

	// Guard used by read_sharepoint_excel to provide a clear message when excel is missing.
	ScalarFunction require_excel_func("__sharepoint_require_excel", {}, LogicalType::BOOLEAN,
	                                  SharepointRequireExcelScalar);
	require_excel_func.SetVolatile();
	loader.RegisterFunction(require_excel_func);

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
	return "spxlsx";
}

std::string SharepointExtension::Version() const {
#ifdef EXT_VERSION_SPXLSX
	return EXT_VERSION_SPXLSX;
#else
	return "";
#endif
}

} // namespace duckdb

// This macro creates the extension entry point
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(spxlsx, loader) {
	duckdb::LoadInternal(loader);
}
}
