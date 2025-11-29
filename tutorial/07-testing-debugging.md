# 07 - Testing & Debugging

In this module, you'll learn how to test and debug your SharePoint extension effectively.

## Goals
- Write SQL-based tests
- Debug extension code
- Handle common errors
- Profile performance
- Test edge cases

## Step 1: Create Test Files

DuckDB uses `.test` files for SQL-based testing. Create `test/sql/sharepoint_basic.test`:

```sql
# name: test/sql/sharepoint_basic.test
# description: Basic SharePoint extension tests
# group: [sharepoint]

# Load extension
statement ok
LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension';

# Test that extension loaded
query I
SELECT 1;
----
1

# Test secret creation (manual token)
statement ok
CREATE SECRET sharepoint_test (
    TYPE sharepoint,
    PROVIDER token,
    TOKEN 'test_token_12345'
);

# Verify secret was created
query III
SELECT name, type, provider
FROM duckdb_secrets()
WHERE name = 'sharepoint_test';
----
sharepoint_test	sharepoint	token

# Clean up
statement ok
DROP SECRET sharepoint_test;
```

## Step 2: Test List Reading

Create `test/sql/sharepoint_lists.test`:

```sql
# name: test/sql/sharepoint_lists.test
# description: Test SharePoint list reading
# group: [sharepoint]

require sharepoint

# This test requires environment variables:
# SHAREPOINT_TOKEN - Valid access token
# SHAREPOINT_LIST_URL - URL to a test list

# Skip if no token provided
require-env SHAREPOINT_TOKEN

statement ok
CREATE SECRET sharepoint_test (
    TYPE sharepoint,
    PROVIDER token,
    TOKEN '${SHAREPOINT_TOKEN}'
);

# Test reading a list
require-env SHAREPOINT_LIST_URL

query I
SELECT COUNT(*) > 0
FROM read_sharepoint('${SHAREPOINT_LIST_URL}');
----
true

# Test filtering
query I
SELECT COUNT(*) >= 0
FROM read_sharepoint('${SHAREPOINT_LIST_URL}')
WHERE 1=1;
----
true

# Clean up
statement ok
DROP SECRET sharepoint_test;
```

## Step 3: Run Tests

```bash
# Set environment variables for real API tests
export SHAREPOINT_TOKEN="your-access-token"
export SHAREPOINT_LIST_URL="https://contoso.sharepoint.com/sites/Test/Lists/TestList"

# Run tests
cd duckdb
BUILD_SHAREPOINT=1 make

# Run specific test
./build/release/test/unittest "test/sql/sharepoint_basic.test"

# Run all SharePoint tests
./build/release/test/unittest "[sharepoint]"
```

## Step 4: Add Debug Logging

Add debug macros to your code:

```cpp
// In sharepoint_extension.hpp or a new debug.hpp:
#ifdef DEBUG_SHAREPOINT
    #define SP_DEBUG(msg) std::cerr << "[SP DEBUG] " << msg << std::endl
    #define SP_DEBUG_VAR(name, value) std::cerr << "[SP DEBUG] " << name << " = " << value << std::endl
#else
    #define SP_DEBUG(msg)
    #define SP_DEBUG_VAR(name, value)
#endif
```

Use in your code:

```cpp
// In sharepoint_read.cpp:
static unique_ptr<FunctionData> SharepointReadBind(...) {
    SP_DEBUG("Starting SharePoint list bind");

    std::string url = input.inputs[0].ToString();
    SP_DEBUG_VAR("URL", url);

    bind_data->token = SharepointAuth::GetAccessToken(context);
    SP_DEBUG("Retrieved access token");

    // ... rest of bind logic ...

    SP_DEBUG_VAR("Found columns", names.size());

    return std::move(bind_data);
}
```

Build with debug output:

```bash
make debug
cd duckdb
./build/debug/duckdb -c "
    LOAD '../build/debug/extension/sharepoint/sharepoint.duckdb_extension';
    -- Your query here
" 2>&1 | grep "SP DEBUG"
```

## Step 5: Debug with GDB/LLDB

### Using GDB (Linux)

```bash
# Build debug version
make debug

# Start GDB
cd duckdb
gdb ./build/debug/duckdb

# In GDB:
(gdb) break SharepointReadBind
(gdb) run

# In DuckDB:
LOAD '../build/debug/extension/sharepoint/sharepoint.duckdb_extension';
SELECT * FROM read_sharepoint('...');

# GDB will break at your breakpoint
(gdb) print url
(gdb) next
(gdb) continue
```

### Using LLDB (macOS)

```bash
cd duckdb
lldb ./build/debug/duckdb

# In LLDB:
(lldb) breakpoint set --name SharepointReadBind
(lldb) run

# Continue as above
(lldb) print url
(lldb) next
(lldb) continue
```

## Step 6: Common Error Patterns

### Error 1: Authentication Failures

```cpp
// Add detailed error messages:
std::string SharepointAuth::GetAccessToken(ClientContext &context) {
    try {
        // ... existing logic ...
    } catch (const std::exception &e) {
        throw IOException(
            "Failed to get SharePoint access token:\n" +
            std::string(e.what()) + "\n\n" +
            "Make sure you've created a secret:\n" +
            "  CREATE SECRET (TYPE sharepoint, PROVIDER oauth);"
        );
    }
}
```

### Error 2: API Request Failures

```cpp
// In PerformHttpsRequest, add detailed error context:
if (status_code >= 400) {
    std::string error_body = ExtractBody(response);

    std::ostringstream error_msg;
    error_msg << "SharePoint API request failed:\n"
              << "  Status: " << status_code << "\n"
              << "  Host: " << host << "\n"
              << "  Path: " << path << "\n"
              << "  Response: " << error_body;

    throw IOException(error_msg.str());
}
```

### Error 3: Type Conversion Errors

```cpp
// Add try-catch around type conversions:
try {
    double num_value = field_value.get<double>();
    FlatVector::GetData<double>(output.data[col_idx])[output_idx] = num_value;
} catch (const json::exception &e) {
    // Log warning and set to NULL
    SP_DEBUG_VAR("Failed to convert field to double", col_name);
    FlatVector::SetNull(output.data[col_idx], output_idx, true);
}
```

## Step 7: Test Edge Cases

Create `test/sql/sharepoint_edge_cases.test`:

```sql
# Test empty list
query I
SELECT COUNT(*)
FROM read_sharepoint('${EMPTY_LIST_URL}');
----
0

# Test list with NULL values
query I
SELECT COUNT(*)
FROM read_sharepoint('${LIST_WITH_NULLS_URL}')
WHERE some_column IS NULL;
----
1

# Test list with special characters
query I
SELECT LENGTH(name) > 0
FROM read_sharepoint('${LIST_WITH_SPECIAL_CHARS_URL}')
LIMIT 1;
----
true

# Test very long strings
query I
SELECT LENGTH(description) > 1000
FROM read_sharepoint('${LIST_WITH_LONG_TEXT_URL}')
WHERE description IS NOT NULL
LIMIT 1;
----
true

# Test invalid URL
statement error
SELECT * FROM read_sharepoint('not-a-valid-url');
----
Invalid SharePoint URL

# Test missing credentials
statement error
DROP SECRET IF EXISTS sharepoint_test;
SELECT * FROM read_sharepoint('${SHAREPOINT_LIST_URL}');
----
No SharePoint credentials found
```

## Step 8: Performance Profiling

### Timing Queries

```sql
-- Enable timing
.timer on

-- Measure query time
SELECT COUNT(*) FROM read_sharepoint('...');

-- Compare different approaches
-- Approach 1: Fetch all then filter
SELECT * FROM read_sharepoint('...')
WHERE status = 'Active';

-- Approach 2: Server-side filter
SELECT * FROM read_sharepoint('...', filter := 'Status eq ''Active''');
```

### Profile with `valgrind` (Linux)

```bash
valgrind --tool=callgrind ./build/release/duckdb <<EOF
LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension';
SELECT * FROM read_sharepoint('...');
EOF

# Analyze with kcachegrind
kcachegrind callgrind.out.*
```

### Profile with Instruments (macOS)

```bash
# Start Instruments
instruments -t "Time Profiler" ./build/release/duckdb &

# Run your query
# Instruments will show time spent in each function
```

## Step 9: Memory Leak Detection

```bash
# Build with AddressSanitizer
make debug EXTRA_CMAKE_FLAGS="-DSANITIZER=address"

# Run tests
cd duckdb
./build/debug/duckdb -c "
    LOAD '../build/debug/extension/sharepoint/sharepoint.duckdb_extension';
    SELECT * FROM read_sharepoint('...');
"

# Any memory leaks will be reported
```

## Step 10: Integration Tests

Create a comprehensive integration test:

```python
#!/usr/bin/env python3
# test_integration.py

import duckdb
import os

def test_sharepoint_extension():
    # Connect to DuckDB
    con = duckdb.connect()

    # Load extension
    con.execute("LOAD '../build/release/extension/sharepoint/sharepoint.duckdb_extension'")

    # Create secret
    token = os.environ['SHAREPOINT_TOKEN']
    con.execute(f"CREATE SECRET sp (TYPE sharepoint, PROVIDER token, TOKEN '{token}')")

    # Test 1: Read list
    list_url = os.environ['SHAREPOINT_LIST_URL']
    result = con.execute(f"SELECT COUNT(*) FROM read_sharepoint('{list_url}')").fetchone()
    assert result[0] > 0, "List should have items"

    # Test 2: Filter data
    result = con.execute(f"""
        SELECT * FROM read_sharepoint('{list_url}')
        WHERE 1=1
        LIMIT 10
    """).fetchall()
    assert len(result) > 0, "Should return filtered results"

    # Test 3: Aggregation
    result = con.execute(f"""
        SELECT COUNT(*) as total
        FROM read_sharepoint('{list_url}')
    """).fetchone()
    assert result[0] >= 0, "Should aggregate"

    # Test 4: Join with local data
    con.execute("CREATE TABLE local_data AS SELECT 'test' as value")
    result = con.execute(f"""
        SELECT l.value, COUNT(s.*) as count
        FROM local_data l
        LEFT JOIN read_sharepoint('{list_url}') s ON 1=1
        GROUP BY l.value
    """).fetchone()
    assert result is not None, "Should join successfully"

    print("✓ All integration tests passed!")

if __name__ == '__main__':
    test_sharepoint_extension()
```

Run it:

```bash
pip install duckdb
python test_integration.py
```

## Step 11: Continuous Testing

Create `.github/workflows/test.yml` for CI:

```yaml
name: Test Extension

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest

    steps:
    - uses: actions/checkout@v3
      with:
        submodules: recursive

    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y ninja-build cmake

    - name: Setup VCPKG
      run: |
        git clone https://github.com/Microsoft/vcpkg.git
        cd vcpkg
        ./bootstrap-vcpkg.sh

    - name: Build extension
      run: |
        export VCPKG_TOOLCHAIN_PATH=$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake
        GEN=ninja make

    - name: Run unit tests
      run: |
        cd duckdb
        ./build/release/test/unittest "[sharepoint]"

    # Skip integration tests in CI (require real SharePoint access)
    # - name: Run integration tests
    #   env:
    #     SHAREPOINT_TOKEN: ${{ secrets.SHAREPOINT_TOKEN }}
    #   run: python test_integration.py
```

## Debugging Checklist

When something goes wrong:

- [ ] Check extension loads: `LOAD '...'` - no errors?
- [ ] Check secret exists: `SELECT * FROM duckdb_secrets()`
- [ ] Test token manually with Graph Explorer
- [ ] Enable debug logging: `#define DEBUG_SHAREPOINT`
- [ ] Check API response: Add `std::cerr << response` in requests
- [ ] Verify URL parsing: Print extracted site/list names
- [ ] Check type mapping: Print column types discovered
- [ ] Test with simple data first (small list, basic types)
- [ ] Use debugger: Set breakpoints in bind and execute functions
- [ ] Check for memory leaks: Run with AddressSanitizer

## Common Issues & Solutions

| Issue | Solution |
|-------|----------|
| Extension won't load | Check OpenSSL is linked properly |
| "Symbol not found" error | Verify all DuckDB types are defined correctly |
| Crashes on query | Check NULL handling in execution function |
| Slow queries | Profile and optimize HTTP requests |
| Memory leaks | Run with AddressSanitizer |
| Type conversion errors | Add try-catch and default to VARCHAR |
| Authentication fails | Verify token is valid with Graph Explorer |
| API returns 404 | Check site/list URLs are correct |

## What You've Accomplished

You now have:
- Automated test suite
- Debug logging infrastructure
- Error handling patterns
- Performance profiling tools
- Integration test framework
- CI/CD pipeline template

With solid testing and debugging practices, you can confidently develop and maintain your extension!

---

**Navigation:**
- Previous: [06 - Document Libraries](./06-document-libraries.md)
- Next: [08 - Build & Deployment](./08-build-deployment.md)
