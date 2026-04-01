#include "sharepoint_excel.hpp"
#include "sharepoint_auth.hpp"
#include "sharepoint_requests.hpp"
#include "sharepoint_utils.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <random>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <cctype>

using json = duckdb_yyjson::yyjson_doc;
using json_val = duckdb_yyjson::yyjson_val;

namespace duckdb {

// Global cache for downloaded files (URL -> temp_path)
static std::unordered_map<std::string, std::string> file_cache;
static std::mutex cache_mutex;
static std::vector<std::string> temp_files_to_cleanup;

static bool LooksLikeZipArchive(const std::string &content) {
    return content.size() >= 4 &&
           static_cast<unsigned char>(content[0]) == 0x50 &&
           static_cast<unsigned char>(content[1]) == 0x4B &&
           static_cast<unsigned char>(content[2]) == 0x03 &&
           static_cast<unsigned char>(content[3]) == 0x04;
}

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

static std::string UrlDecode(const std::string &value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] == '%' && i + 2 < value.length()) {
            std::string hex = value.substr(i + 1, 2);
            char ch = static_cast<char>(std::stoi(hex, nullptr, 16));
            decoded += ch;
            i += 2;
        } else if (value[i] == '+') {
            decoded += ' ';
        } else {
            decoded += value[i];
        }
    }

    return decoded;
}

static std::string GetQueryParameter(const std::string &url, const std::string &param_name) {
    size_t query_pos = url.find('?');
    if (query_pos == std::string::npos || query_pos + 1 >= url.length()) {
        return "";
    }

    std::string query = url.substr(query_pos + 1);
    std::string needle = param_name + "=";
    size_t start = 0;

    while (start < query.length()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) {
            end = query.length();
        }

        std::string pair = query.substr(start, end - start);
        if (pair.rfind(needle, 0) == 0) {
            return UrlDecode(pair.substr(needle.length()));
        }

        start = end + 1;
    }

    return "";
}

static std::string Base64UrlEncode(const std::string &value) {
    static const char *BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((value.size() + 2) / 3) * 4);

    int val = 0;
    int valb = -6;
    for (unsigned char c : value) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            encoded.push_back(BASE64_CHARS[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        encoded.push_back(BASE64_CHARS[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (encoded.size() % 4) {
        encoded.push_back('=');
    }

    for (auto &ch : encoded) {
        if (ch == '+') {
            ch = '-';
        } else if (ch == '/') {
            ch = '_';
        }
    }

    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }

    return encoded;
}

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
    // For SharePoint Doc.aspx links, prefer the explicit ?file=... query parameter.
    info.filename = GetQueryParameter(url, "file");
    if (info.filename.empty()) {
        size_t last_slash = url.find_last_of('/');
        if (last_slash != std::string::npos) {
            size_t query_pos = url.find('?', last_slash + 1);
            if (query_pos == std::string::npos) {
                info.filename = UrlDecode(url.substr(last_slash + 1));
            } else {
                info.filename = UrlDecode(url.substr(last_slash + 1, query_pos - (last_slash + 1)));
            }
        }
    }
    if (info.filename.empty()) {
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
        // Fallback for SharePoint sharing links (e.g. /_layouts/15/Doc.aspx?...).
        // Graph can resolve these URLs directly through /shares/{encodedUrl}/driveItem.
        std::string encoded_url = Base64UrlEncode(url);

        std::ostringstream shared_item_path;
        shared_item_path << "/v1.0/shares/u!" << encoded_url << "/driveItem";

        std::string shared_item_response = PerformHttpsRequest(
            "graph.microsoft.com",
            shared_item_path.str(),
            token,
            HttpMethod::GET
        );

        auto shared_doc = duckdb_yyjson::yyjson_read(shared_item_response.c_str(), shared_item_response.length(), 0);
        if (!shared_doc) {
            throw IOException("Failed to parse driveItem response from sharing URL");
        }

        auto shared_root = duckdb_yyjson::yyjson_doc_get_root(shared_doc);
        auto item_id_val = duckdb_yyjson::yyjson_obj_get(shared_root, "id");
        auto parent_ref = duckdb_yyjson::yyjson_obj_get(shared_root, "parentReference");
        auto drive_id_val = parent_ref ? duckdb_yyjson::yyjson_obj_get(parent_ref, "driveId") : nullptr;
        auto site_id_val = parent_ref ? duckdb_yyjson::yyjson_obj_get(parent_ref, "siteId") : nullptr;

        if (!item_id_val || !drive_id_val) {
            duckdb_yyjson::yyjson_doc_free(shared_doc);
            throw InvalidInputException(
                "Could not resolve SharePoint sharing URL to a drive item. "
                "Please use a direct file URL or a valid sharing URL."
            );
        }

        info.item_id = duckdb_yyjson::yyjson_get_str(item_id_val);
        info.drive_id = duckdb_yyjson::yyjson_get_str(drive_id_val);
        if (site_id_val) {
            info.site_id = duckdb_yyjson::yyjson_get_str(site_id_val);
        }

        duckdb_yyjson::yyjson_doc_free(shared_doc);
        return info;
    }

    // Get the relative path to the file
    std::string file_path_part = url.substr(docs_pos);
    
    // URL decode the path
    std::string file_path = UrlDecode(file_path_part);

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

        if (content.empty()) {
            throw IOException(
                "Downloaded file is empty. SharePoint may have returned an empty redirect response or the file content could not be retrieved."
            );
        }

        if (!LooksLikeZipArchive(content)) {
            std::ostringstream error;
            error << "Downloaded file is not a valid XLSX/XLSM ZIP archive. First bytes:";
            auto preview_length = std::min<idx_t>(content.size(), 8);
            for (idx_t j = 0; j < preview_length; j++) {
                error << " "
                      << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                      << static_cast<int>(static_cast<unsigned char>(content[j]));
            }
            throw IOException(error.str());
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
