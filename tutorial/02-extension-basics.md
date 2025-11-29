# 02 - Extension Basics

In this module, you'll create a minimal working extension that successfully loads into DuckDB. This verifies your build system and provides the foundation for adding features.

## Goals
- Understand DuckDB's extension entry point
- Create minimal header and source files
- Build and load the extension
- Verify it works with a simple test

## Step 1: Understanding Extension Loading

When DuckDB loads an extension, it looks for a specific C function with this signature:

```cpp
extern "C" {
    DUCKDB_EXTENSION_API void <extension_name>_init(duckdb::DatabaseInstance &db);
    DUCKDB_EXTENSION_API const char *<extension_name>_version();
}
```

DuckDB provides a macro `DUCKDB_EXTENSION_ENTRY` that handles this boilerplate for you.

## Step 2: Create Main Header File

Create `src/include/sharepoint_extension.hpp`:

```cpp
#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Main extension loader
class SharepointExtension : public Extension {
public:
    void Load(DuckDB &db) override;
    std::string Name() override;
};

} // namespace duckdb
```

### Understanding the Code

- **`Extension` class**: DuckDB's base class for all extensions
- **`Load()` method**: Called when the extension is loaded - this is where you'll register functions
- **`Name()` method**: Returns the extension name for logging/debugging
- **namespace duckdb**: All DuckDB extension code lives in this namespace

## Step 3: Create Main Implementation File

Create `src/sharepoint_extension.cpp`:

```cpp
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
static void LoadInternal(DatabaseInstance &instance) {
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

void SharepointExtension::Load(DuckDB &db) {
    LoadInternal(*db.instance);
}

std::string SharepointExtension::Name() {
    return "sharepoint";
}

} // namespace duckdb

// This macro creates the extension entry point
extern "C" {

DUCKDB_EXTENSION_API void sharepoint_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    duckdb::SharepointExtension extension;
    extension.Load(db_wrapper);
}

DUCKDB_EXTENSION_API const char *sharepoint_version() {
    return duckdb::DuckDB::LibraryVersion();
}

}
```

### Understanding the Code

1. **`DUCKDB_EXTENSION_MAIN`**: Define this before including DuckDB headers in exactly one .cpp file
2. **`LoadInternal()`**: The actual initialization logic
3. **OpenSSL initialization**: Required for making HTTPS requests to SharePoint
4. **Forward declarations**: We'll implement these functions in later modules
5. **Entry point macros**: The `extern "C"` block exports the functions DuckDB needs

## Step 4: Create Stub Files

For now, create empty stub files for the other modules. We'll fill these in later.

### src/include/sharepoint_auth.hpp

```cpp
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
```

### src/sharepoint_auth.cpp

```cpp
#include "sharepoint_auth.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

std::string SharepointAuth::GetAccessToken(ClientContext &context) {
    throw NotImplementedException("SharePoint authentication not yet implemented");
}

} // namespace duckdb
```

### src/include/sharepoint_read.hpp

```cpp
#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Forward declaration
struct SharepointReadBindData;

// Table function for reading SharePoint lists
void RegisterSharepointReadFunction(DatabaseInstance &db);

} // namespace duckdb
```

### src/sharepoint_read.cpp

```cpp
#include "sharepoint_read.hpp"

namespace duckdb {

void RegisterSharepointReadFunction(DatabaseInstance &db) {
    // To be implemented in module 05
}

} // namespace duckdb
```

### src/include/sharepoint_requests.hpp

```cpp
#pragma once

#include <string>

namespace duckdb {

// HTTP request methods
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE
};

// Perform HTTPS request to SharePoint/Graph API
std::string PerformHttpsRequest(
    const std::string &host,
    const std::string &path,
    const std::string &token,
    HttpMethod method = HttpMethod::GET,
    const std::string &body = ""
);

} // namespace duckdb
```

### src/sharepoint_requests.cpp

```cpp
#include "sharepoint_requests.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

std::string PerformHttpsRequest(
    const std::string &host,
    const std::string &path,
    const std::string &token,
    HttpMethod method,
    const std::string &body) {

    throw NotImplementedException("HTTP requests not yet implemented");
}

} // namespace duckdb
```

### src/include/sharepoint_utils.hpp

```cpp
#pragma once

#include <string>

namespace duckdb {

// Utility functions for URL parsing, etc.
namespace SharepointUtils {
    // Extract site URL from full SharePoint URL
    std::string ExtractSiteUrl(const std::string &url);

    // Extract list name from URL
    std::string ExtractListName(const std::string &url);

    // URL encode a string
    std::string UrlEncode(const std::string &value);
}

} // namespace duckdb
```

### src/sharepoint_utils.cpp

```cpp
#include "sharepoint_utils.hpp"

namespace duckdb {
namespace SharepointUtils {

std::string ExtractSiteUrl(const std::string &url) {
    // To be implemented in module 03
    return "";
}

std::string ExtractListName(const std::string &url) {
    // To be implemented in module 03
    return "";
}

std::string UrlEncode(const std::string &value) {
    // To be implemented in module 03
    return "";
}

} // namespace SharepointUtils
} // namespace duckdb
```

## Step 5: Build the Extension

Now let's build your minimal extension:

```bash
# Make sure you're in the project root
cd duckdb_sharepoint

# Build
GEN=ninja make
```

This will:
1. Configure the build system with CMake
2. Download and compile OpenSSL via VCPKG (first time only - takes a while!)
3. Compile your extension
4. Link everything together

### Expected Output

You should see output like:

```
-- Configuring done
-- Generating done
-- Build files have been written to: .../build/release
[1/X] Building CXX object ...
...
[X/X] Linking CXX shared library build/release/extension/sharepoint/sharepoint.duckdb_extension
```

### Common Build Errors

**Error: "Cannot find DuckDB headers"**
```bash
# Make sure DuckDB submodule is initialized
git submodule update --init --recursive
```

**Error: "OpenSSL not found"**
```bash
# Verify VCPKG_TOOLCHAIN_PATH is set
echo $VCPKG_TOOLCHAIN_PATH
```

**Error: "ninja: command not found"**
```bash
# Install ninja or build without it
brew install ninja  # macOS
# or
make  # Build without ninja
```

## Step 6: Test Loading the Extension

Start DuckDB and try loading your extension:

```bash
# Navigate to DuckDB directory
cd duckdb

# Start DuckDB
./build/release/duckdb

# In DuckDB, load your extension
D LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension';
```

### Expected Output

If successful, you should see no errors. Your extension is loaded!

Try this to verify:

```sql
-- This should work (DuckDB is running)
SELECT 1 + 1;

-- This will throw "not implemented" but proves the extension loaded
-- (We haven't implemented the function yet!)
-- SELECT * FROM read_sharepoint('test');
```

## Step 7: Understanding the Build Output

After building, you'll have:

```
build/release/
├── extension/
│   └── sharepoint/
│       └── sharepoint.duckdb_extension  ← Loadable extension file
└── src/
    └── libsharepoint_extension.a        ← Static library
```

- **`.duckdb_extension`**: Can be loaded into any DuckDB instance with `LOAD`
- **`.a` static library**: Can be compiled directly into a custom DuckDB build

## Step 8: Create a Helper Script

To make testing easier, create a helper script `test_extension.sh`:

```bash
#!/bin/bash

# Build the extension
GEN=ninja make

# Check if build succeeded
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

# Run DuckDB with extension auto-loaded
cd duckdb
./build/release/duckdb -c "LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension'; SELECT 'Extension loaded successfully!' as status;"
```

Make it executable:

```bash
chmod +x test_extension.sh
./test_extension.sh
```

## What You've Accomplished

At this point, you have:
- A compiling DuckDB extension
- Proper project structure
- OpenSSL integrated and initialized
- Extension entry points working
- Verified the extension loads into DuckDB

The extension doesn't do anything useful yet, but the foundation is solid. In the next module, you'll build the HTTP client to communicate with SharePoint.

## Debugging Tips

### View Compiler Warnings

```bash
GEN=ninja make 2>&1 | grep warning
```

### Build in Debug Mode

```bash
make debug
cd duckdb
./build/debug/duckdb
```

### Check Symbol Export

```bash
# Verify the extension exports the right symbols
nm build/release/extension/sharepoint/sharepoint.duckdb_extension | grep sharepoint_init
```

You should see:
```
... T sharepoint_init
... T sharepoint_version
```

## Next Steps

Now that your extension loads successfully, you'll implement the HTTP layer to communicate with SharePoint's APIs.

---

**Navigation:**
- Previous: [01 - Project Setup](./01-project-setup.md)
- Next: [03 - HTTP Layer](./03-http-layer.md)
