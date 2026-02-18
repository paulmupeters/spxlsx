#include "sharepoint_excel.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "sharepoint_utils.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "yyjson.hpp"

#include <random>
#include <sstream>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdlib>

using json = duckdb_yyjson::yyjson_doc;
using json_val = duckdb_yyjson::yyjson_val;

namespace duckdb {

// Global cache for downloaded files (URL -> temp_path)
static std::unordered_map<std::string, std::string> file_cache;
static std::mutex cache_mutex;
static std::vector<std::string> temp_files_to_cleanup;

// Generate temporary file path (cross-platform)
static std::string GenerateTempPath(const std::string &filename) {
    const char* temp_dir = nullptr;
    
    // Try different environment variables for temp directory
#ifdef _WIN32
    temp_dir = std::getenv("TEMP");
    if (!temp_dir) {
        temp_dir = std::getenv("TMP");
    }
    if (!temp_dir) {
        temp_dir = "C:\\Temp";
    }
#else
    temp_dir = std::getenv("TMPDIR");
    if (!temp_dir) {
        temp_dir = "/tmp";
    }
#endif

    // Generate unique filename with random component
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(10000, 99999);

    std::ostringstream path;
    path << temp_dir;
    
#ifdef _WIN32
    path << "\\duckdb_sharepoint_";
#else
    path << "/duckdb_sharepoint_";
#endif
    
    path << dis(gen) << "_" << filename;

    return path.str();
}

// Helper: Extract file ID and drive ID from SharePoint URL
struct SharePointFileInfo {
    std::string site_id;
    std::string drive_id;
    std::string item_id;
    std::string filename;
};

static SharePointFileInfo GetFileInfo(
    const std::string &url,
    const std::string &token) {

    SharePointFileInfo info;

    // Extract site
    std::string site_url = SharepointUtils::ExtractSiteUrl(url);
    std::string tenant = SharepointUtils::ExtractTenantFromUrl(url);

    // Get site ID
    std::ostringstream site_path;
    if (site_url == "/" || site_url.empty()) {
        site_path << "/v1.0/sites/" << tenant << ".sharepoint.com";
    } else {
        site_path << "/v1.0/sites/" << tenant << ".sharepoint.com:" << site_url;
    }

    std::string site_response = PerformHttpsRequest(
        "graph.microsoft.com",
        site_path.str(),
        token,
        HttpMethod::GET
    );

    auto site_doc = duckdb_yyjson::yyjson_read(site_response.c_str(), site_response.length(), 0);
    if (!site_doc) {
        throw IOException("Failed to parse site response");
    }
    auto site_root = duckdb_yyjson::yyjson_doc_get_root(site_doc);
    auto site_id_val = duckdb_yyjson::yyjson_obj_get(site_root, "id");
    if (!site_id_val) {
        duckdb_yyjson::yyjson_doc_free(site_doc);
        throw IOException("Site ID not found in response");
    }
    info.site_id = duckdb_yyjson::yyjson_get_str(site_id_val);
    duckdb_yyjson::yyjson_doc_free(site_doc);

    // Extract filename from URL
    size_t last_slash = url.find_last_of('/');
    if (last_slash != std::string::npos) {
        info.filename = url.substr(last_slash + 1);
        
        // URL decode the filename if needed
        std::string decoded;
        for (size_t i = 0; i < info.filename.length(); ++i) {
            if (info.filename[i] == '%' && i + 2 < info.filename.length()) {
                std::string hex = info.filename.substr(i + 1, 2);
                char ch = static_cast<char>(std::stoi(hex, nullptr, 16));
                decoded += ch;
                i += 2;
            } else {
                decoded += info.filename[i];
            }
        }
        info.filename = decoded;
    } else {
        info.filename = "file.xlsx";
    }

    // Extract the path after the site URL
    size_t docs_pos = url.find("/Shared%20Documents/");
    if (docs_pos == std::string::npos) {
        docs_pos = url.find("/Documents/");
    }
    if (docs_pos == std::string::npos) {
        docs_pos = url.find("/Shared Documents/");
    }

    if (docs_pos == std::string::npos) {
        throw InvalidInputException("Could not parse file path from URL. Expected /Documents/ or /Shared Documents/ in path.");
    }

    // Get the relative path to the file
    std::string file_path_part = url.substr(docs_pos);
    
    // URL decode the path
    std::string file_path;
    for (size_t i = 0; i < file_path_part.length(); ++i) {
        if (file_path_part[i] == '%' && i + 2 < file_path_part.length()) {
            std::string hex = file_path_part.substr(i + 1, 2);
            char ch = static_cast<char>(std::stoi(hex, nullptr, 16));
            file_path += ch;
            i += 2;
        } else {
            file_path += file_path_part[i];
        }
    }

    // Get drives
    std::ostringstream drives_path;
    drives_path << "/v1.0/sites/" << info.site_id << "/drives";

    std::string drives_response = PerformHttpsRequest(
        "graph.microsoft.com",
        drives_path.str(),
        token,
        HttpMethod::GET
    );

    auto drives_doc = duckdb_yyjson::yyjson_read(drives_response.c_str(), drives_response.length(), 0);
    if (!drives_doc) {
        throw IOException("Failed to parse drives response");
    }
    auto drives_root = duckdb_yyjson::yyjson_doc_get_root(drives_doc);
    auto drives_value = duckdb_yyjson::yyjson_obj_get(drives_root, "value");
    
    if (!drives_value || !duckdb_yyjson::yyjson_is_arr(drives_value) || duckdb_yyjson::yyjson_arr_size(drives_value) == 0) {
        duckdb_yyjson::yyjson_doc_free(drives_doc);
        throw IOException("No drives found in SharePoint site");
    }
    
    // Use first drive (typically "Documents")
    auto first_drive = duckdb_yyjson::yyjson_arr_get_first(drives_value);
    auto drive_id_val = duckdb_yyjson::yyjson_obj_get(first_drive, "id");
    if (!drive_id_val) {
        duckdb_yyjson::yyjson_doc_free(drives_doc);
        throw IOException("Drive ID not found");
    }
    info.drive_id = duckdb_yyjson::yyjson_get_str(drive_id_val);
    duckdb_yyjson::yyjson_doc_free(drives_doc);

    // Get item by path
    std::ostringstream item_path;
    item_path << "/v1.0/sites/" << info.site_id
              << "/drives/" << info.drive_id
              << "/root:" << file_path;

    std::string item_response = PerformHttpsRequest(
        "graph.microsoft.com",
        item_path.str(),
        token,
        HttpMethod::GET
    );

    auto item_doc = duckdb_yyjson::yyjson_read(item_response.c_str(), item_response.length(), 0);
    if (!item_doc) {
        throw IOException("Failed to parse item response");
    }
    auto item_root = duckdb_yyjson::yyjson_doc_get_root(item_doc);
    auto item_id_val = duckdb_yyjson::yyjson_obj_get(item_root, "id");
    if (!item_id_val) {
        duckdb_yyjson::yyjson_doc_free(item_doc);
        throw IOException("Item ID not found. File may not exist at the specified path.");
    }
    info.item_id = duckdb_yyjson::yyjson_get_str(item_id_val);
    duckdb_yyjson::yyjson_doc_free(item_doc);

    return info;
}

// Scalar function: Download Excel file and return temp path
static void SharepointDownloadExcelScalar(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &url_vector = args.data[0];
    UnifiedVectorFormat url_data;
    url_vector.ToUnifiedFormat(args.size(), url_data);

    auto result_data = FlatVector::GetData<string_t>(result);

    for (idx_t i = 0; i < args.size(); i++) {
        auto idx = url_data.sel->get_index(i);

        if (!url_data.validity.RowIsValid(idx)) {
            FlatVector::SetNull(result, i, true);
            continue;
        }

        // Get URL
        string_t url_str = ((string_t*)url_data.data)[idx];
        std::string url = url_str.GetString();

        // Verify it's an Excel file
        if (url.find(".xlsx") == std::string::npos &&
            url.find(".xlsm") == std::string::npos &&
            url.find(".XLSX") == std::string::npos &&
            url.find(".XLSM") == std::string::npos) {
            throw InvalidInputException(
                "File must be an Excel file (.xlsx or .xlsm). "
                "Legacy .xls files are not supported."
            );
        }

        std::string temp_path;
        
        // Check cache first
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = file_cache.find(url);
            if (it != file_cache.end()) {
                // Verify file still exists
                std::ifstream test(it->second);
                if (test.good()) {
                    temp_path = it->second;
                    result_data[i] = StringVector::AddString(result, temp_path);
                    continue;
                }
                // File no longer exists, remove from cache
                file_cache.erase(it);
            }
        }

        // Get authentication token
        std::string token = SharepointAuth::GetAccessToken(state.GetContext());

        // Get file information
        SharePointFileInfo file_info;
        try {
            file_info = GetFileInfo(url, token);
        } catch (const std::exception &e) {
            throw IOException("Failed to get file information: " + std::string(e.what()));
        }

        // Generate temp file path
        temp_path = GenerateTempPath(file_info.filename);

        // Download file content
        std::string content;
        try {
            content = DownloadSharepointFileContent(
                file_info.site_id,
                file_info.drive_id,
                file_info.item_id,
                token
            );
        } catch (const std::exception &e) {
            throw IOException("Failed to download Excel file from SharePoint: " + std::string(e.what()));
        }

        // Write to file
        std::ofstream outfile(temp_path, std::ios::binary);
        if (!outfile) {
            throw IOException("Failed to create temporary file: " + temp_path);
        }

        outfile.write(content.c_str(), content.size());
        outfile.close();

        if (outfile.fail()) {
            throw IOException("Failed to write file content to: " + temp_path);
        }

        // Add to cache and cleanup list
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            file_cache[url] = temp_path;
            temp_files_to_cleanup.push_back(temp_path);
        }

        result_data[i] = StringVector::AddString(result, temp_path);
    }
}

// Cleanup function to be called on extension unload
class ExcelCleanup {
public:
    ~ExcelCleanup() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (const auto &path : temp_files_to_cleanup) {
            std::remove(path.c_str());
        }
        temp_files_to_cleanup.clear();
        file_cache.clear();
    }
};

// Global cleanup instance
static ExcelCleanup global_cleanup;

// Register the Excel integration function
void RegisterSharepointExcelFunction(ExtensionLoader &loader) {
    ScalarFunction download_func(
        "sharepoint_download_excel",
        {LogicalType::VARCHAR},
        LogicalType::VARCHAR,
        SharepointDownloadExcelScalar
    );

    loader.RegisterFunction(download_func);
}

} // namespace duckdb
