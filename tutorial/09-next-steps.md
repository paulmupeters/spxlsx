# 09 - Next Steps

Congratulations on building your SharePoint extension! This module covers advanced features you can add and ways to improve your extension.

## What You've Built

You now have a working DuckDB extension that:
- ✅ Authenticates with Azure AD OAuth
- ✅ Reads SharePoint Lists
- ✅ Reads Document Library metadata
- ✅ Maps SharePoint types to DuckDB types
- ✅ Handles pagination and filtering
- ✅ Integrates with DuckDB's secret manager
- ✅ Includes tests and documentation

## Advanced Features to Add

### 1. Write Operations (COPY TO)

Allow users to write data back to SharePoint:

```sql
-- Insert new list items
COPY (SELECT * FROM local_table)
TO sharepoint('https://contoso.sharepoint.com/sites/Team/Lists/NewData');

-- Update existing items
UPDATE sharepoint_list('...')
SET Status = 'Complete'
WHERE ID IN (1, 2, 3);
```

**Implementation hints:**
- Use PATCH requests to update items
- Use POST requests to create items
- Implement batch operations for efficiency
- Handle conflicts and versioning

**Key code:**
```cpp
// In sharepoint_copy.cpp
void SharepointCopyFunction(...) {
    // Collect rows from input
    // Batch into groups of 20
    // POST to SharePoint API
    // Handle errors and retry
}
```

### 2. Token Refresh

Automatically refresh expired tokens:

```cpp
// In sharepoint_auth.cpp
std::string RefreshToken(const std::string &refresh_token) {
    // POST to token endpoint with refresh_token grant
    // Update secret with new access_token
    // Return new token
}

// In GetAccessToken():
if (TokenIsExpired(expires_at)) {
    if (HasRefreshToken()) {
        new_token = RefreshToken(refresh_token);
        UpdateSecret(new_token);
        return new_token;
    }
}
```

### 3. Change Tracking / Delta Queries

Only fetch items that changed since last query:

```sql
-- First query: Get all items and delta token
SELECT * FROM read_sharepoint_delta('...') AS (data, delta_token TEXT);

-- Subsequent queries: Only get changes
SELECT * FROM read_sharepoint_delta('...', delta_token := '...');
```

**Microsoft Graph API endpoint:**
```
GET /sites/{site-id}/lists/{list-id}/items/delta
```

### 4. App-Only Authentication

Support service accounts for automation:

```cpp
// Use client credentials flow
// No user interaction required
// Good for scheduled jobs

CREATE SECRET sp_app (
    TYPE sharepoint,
    PROVIDER client_credentials,
    CLIENT_ID '...',
    CLIENT_SECRET '...',
    TENANT_ID '...'
);
```

### 5. Query Pushdown

Push WHERE clauses to SharePoint to reduce data transfer:

```sql
-- This should use server-side filtering
SELECT * FROM read_sharepoint('...')
WHERE Status = 'Active' AND CreatedDate > '2024-01-01';
```

**Implementation:**
- Parse WHERE clause in bind function
- Convert to OData $filter syntax
- Add to API request

```cpp
// In SharepointReadBind:
auto filter = BuildODataFilter(input.filters);
// filter = "Status eq 'Active' and CreatedDate gt 2024-01-01"
```

### 6. Multi-Value Field Support

Handle complex SharePoint field types:

```sql
-- Choice fields with multiple selections
SELECT Name, array_to_string(Skills, ', ') as skills_list
FROM read_sharepoint('...');

-- Lookup fields
SELECT Name, Department.Title
FROM read_sharepoint('...');

-- Person fields
SELECT Name, Manager.Email, Manager.DisplayName
FROM read_sharepoint('...');
```

### 7. File Download Function

Download actual file contents:

```sql
-- Download file to local filesystem
CALL sharepoint_download_file(
    'https://contoso.sharepoint.com/.../document.pdf',
    '/local/path/document.pdf'
);

-- Read file contents as BLOB
SELECT name, sharepoint_get_content(id) as content
FROM read_sharepoint_library('...')
WHERE name LIKE '%.txt';
```

### 8. Batch Operations

Optimize multiple operations:

```sql
-- Insert 1000 rows in batches of 20
COPY (SELECT * FROM generate_series(1, 1000))
TO sharepoint('...', batch_size := 20);
```

### 9. Metadata Functions

Explore SharePoint structure:

```sql
-- List all sites
SELECT * FROM sharepoint_sites();

-- List all lists in a site
SELECT * FROM sharepoint_lists('site-url');

-- Get list schema
SELECT * FROM sharepoint_columns('list-url');
```

### 10. Caching Layer

Cache frequently accessed data:

```cpp
// Cache list schemas for 1 hour
// Cache items for configurable duration
// Invalidate on write operations

struct Cache {
    std::unordered_map<std::string, CachedData> data;
    std::chrono::steady_clock::time_point last_update;
};
```

## Performance Optimizations

### 1. Parallel Requests

Fetch multiple pages concurrently:

```cpp
std::vector<std::future<std::string>> futures;
for (const auto &page_url : page_urls) {
    futures.push_back(std::async(std::launch::async,
        FetchPage, page_url, token));
}

for (auto &future : futures) {
    results.push_back(future.get());
}
```

### 2. Connection Pooling

Reuse SSL connections:

```cpp
class ConnectionPool {
    std::queue<SSL*> available_connections;
    std::mutex mutex;

public:
    SSL* GetConnection();
    void ReturnConnection(SSL* conn);
};
```

### 3. Compression

Request compressed responses:

```cpp
request << "Accept-Encoding: gzip, deflate\r\n";
// Then decompress response
```

### 4. Column Pruning

Only fetch requested columns:

```sql
-- This should only fetch 'Name' and 'Status' columns
SELECT Name, Status FROM read_sharepoint('...');
```

```cpp
// In bind function:
auto projected_columns = ExtractProjectedColumns(input);
auto select_clause = BuildSelectClause(projected_columns);
// $select=Name,Status
```

### 5. Predicate Pushdown

Push filters, limits, and sorts to SharePoint:

```sql
-- Should translate to: $filter=Status eq 'Active'&$top=10&$orderby=Name
SELECT * FROM read_sharepoint('...')
WHERE Status = 'Active'
ORDER BY Name
LIMIT 10;
```

## Security Enhancements

### 1. Certificate Validation

Properly validate SSL certificates:

```cpp
SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
SSL_CTX_set_default_verify_paths(ctx);
```

### 2. Secret Encryption

Encrypt secrets at rest:

```cpp
// DuckDB handles this, but ensure tokens are marked as sensitive
secret->is_sensitive = true;
```

### 3. Permission Checking

Verify user has required permissions:

```cpp
// Check before attempting operations
bool HasPermission(const std::string &site_id, const std::string &permission) {
    // Query permissions API
    // Cache results
}
```

### 4. Rate Limiting

Respect SharePoint throttling:

```cpp
struct RateLimiter {
    int requests_per_minute = 600;
    std::chrono::steady_clock::time_point window_start;
    int request_count = 0;

    void CheckRateLimit();
};
```

## User Experience Improvements

### 1. Better Error Messages

Provide actionable error messages:

```cpp
if (status_code == 403) {
    throw IOException(
        "Permission denied. Please check:\n"
        "1. Your Azure AD app has the required permissions\n"
        "2. Admin has granted consent for the permissions\n"
        "3. You have access to this SharePoint site\n"
        "4. Try re-authenticating: DROP SECRET sharepoint_creds; CREATE SECRET ..."
    );
}
```

### 2. Progress Reporting

Show progress for long operations:

```cpp
// For large lists
std::cout << "Fetching page " << page << " of " << total_pages << "...\r" << std::flush;
```

### 3. Dry Run Mode

Preview changes before applying:

```sql
-- See what would be changed without actually changing
COPY (SELECT * FROM data)
TO sharepoint('...', dry_run := true);
```

### 4. Configuration Options

Allow users to tune behavior:

```sql
-- Configure behavior
SET sharepoint_timeout = 30;  -- seconds
SET sharepoint_retry_count = 3;
SET sharepoint_cache_ttl = 3600;  -- seconds
```

## Integration Ideas

### 1. Python Integration

```python
import duckdb

con = duckdb.connect()
con.load_extension('sharepoint')

# Query SharePoint from Python
df = con.execute("""
    SELECT * FROM read_sharepoint('...')
""").df()

# Now use pandas, matplotlib, etc.
df.plot()
```

### 2. BI Tool Integration

Connect Tableau, Power BI, or Looker to DuckDB with SharePoint extension loaded.

### 3. Data Pipelines

```python
# ETL pipeline
import duckdb

con = duckdb.connect()
con.load_extension('sharepoint')

# Extract from SharePoint
con.execute("""
    CREATE TABLE staging AS
    SELECT * FROM read_sharepoint('...')
    WHERE modified > CURRENT_DATE - 1
""")

# Transform
con.execute("""
    CREATE TABLE processed AS
    SELECT
        UPPER(name) as name,
        status,
        date_trunc('month', created) as month
    FROM staging
""")

# Load to other systems
con.execute("COPY processed TO 'output.parquet'")
```

### 4. Scheduled Sync

```bash
#!/bin/bash
# sync_sharepoint.sh

duckdb analytics.db << EOF
    LOAD sharepoint;

    -- Incremental sync
    INSERT INTO local_table
    SELECT * FROM read_sharepoint('...')
    WHERE modified > (SELECT MAX(last_sync) FROM sync_metadata);

    -- Update sync metadata
    UPDATE sync_metadata SET last_sync = CURRENT_TIMESTAMP;
EOF
```

Run with cron:
```
0 * * * * /path/to/sync_sharepoint.sh
```

## Learning Resources

### DuckDB Extension Development
- [Official extension docs](https://duckdb.org/docs/extensions/overview)
- [Extension template](https://github.com/duckdb/extension-template)
- [Example extensions](https://github.com/duckdb/duckdb/tree/main/extension)

### SharePoint & Microsoft Graph
- [Microsoft Graph API docs](https://docs.microsoft.com/en-us/graph/api/overview)
- [SharePoint REST API](https://docs.microsoft.com/en-us/sharepoint/dev/sp-add-ins/get-to-know-the-sharepoint-rest-service)
- [Graph Explorer](https://developer.microsoft.com/en-us/graph/graph-explorer)

### C++ and OpenSSL
- [OpenSSL documentation](https://www.openssl.org/docs/)
- [Modern C++ best practices](https://isocpp.github.io/CppCoreGuidelines/)

## Community & Contribution

### Share Your Extension
- Publish to GitHub
- Submit to DuckDB extension repository
- Write blog posts about your experience
- Create video tutorials

### Get Feedback
- Ask users what features they need
- Monitor GitHub issues
- Join DuckDB Discord/Slack
- Present at conferences

### Contribute Back
- Improve documentation
- Add examples
- Fix bugs
- Help other developers

## Real-World Use Cases

### 1. SharePoint Data Warehouse

```sql
-- Build a data warehouse from SharePoint lists
CREATE TABLE dim_employees AS
SELECT * FROM read_sharepoint('https://.../Employees');

CREATE TABLE fact_timesheets AS
SELECT * FROM read_sharepoint('https://.../Timesheets');

-- Analytics queries
SELECT e.Name, SUM(t.Hours) as total_hours
FROM fact_timesheets t
JOIN dim_employees e ON t.EmployeeID = e.ID
GROUP BY e.Name;
```

### 2. Document Compliance Audit

```sql
-- Find documents missing required metadata
SELECT name, web_url
FROM read_sharepoint_library('...', recursive := true)
WHERE (custom_metadata_field IS NULL
       OR modified > CURRENT_DATE - 90)
  AND NOT is_folder;
```

### 3. Cross-Site Reporting

```sql
-- Aggregate data from multiple SharePoint sites
CREATE VIEW all_projects AS
    SELECT 'Site A' as site, * FROM read_sharepoint('https://site-a/Lists/Projects')
    UNION ALL
    SELECT 'Site B' as site, * FROM read_sharepoint('https://site-b/Lists/Projects')
    UNION ALL
    SELECT 'Site C' as site, * FROM read_sharepoint('https://site-c/Lists/Projects');

-- Global project dashboard
SELECT site, Status, COUNT(*) as count
FROM all_projects
GROUP BY site, Status;
```

### 4. Migration Tool

```sql
-- Migrate from old SharePoint to new
INSERT INTO new_sharepoint_list
SELECT * FROM read_sharepoint('https://old-site/Lists/Data')
WHERE active = true;
```

## Final Thoughts

You've learned how to:
- Build a complex DuckDB extension from scratch
- Integrate with external APIs (SharePoint)
- Handle authentication flows (OAuth)
- Parse and transform data
- Test and debug extensions
- Package and distribute software

These skills are transferable to:
- Other DuckDB extensions (Salesforce, Jira, etc.)
- Database drivers and connectors
- Data integration tools
- API clients and SDKs

## Next Challenge

Try building an extension for another service:
- **Jira**: Query issues, projects, sprints
- **Salesforce**: CRM data and reports
- **Confluence**: Wiki pages and content
- **Azure DevOps**: Work items and repositories
- **GitHub**: Issues, PRs, and repositories

The patterns you've learned apply to all of these!

## Thank You!

Thank you for following this tutorial. I hope you found it valuable and that your SharePoint extension becomes useful to many people.

Happy coding! 🚀

---

**Navigation:**
- Previous: [08 - Build & Deployment](./08-build-deployment.md)
- Back to: [00 - Introduction](./00-introduction.md)

## Appendix: Quick Reference

### Common Commands

```bash
# Build
make

# Debug build
make debug

# Clean
make clean

# Run tests
make test

# Format code
make format
```

### SQL Examples

```sql
-- Load extension
LOAD 'sharepoint';

-- Authenticate
CREATE SECRET (TYPE sharepoint, PROVIDER oauth);

-- Query list
SELECT * FROM read_sharepoint('...');

-- Query library
SELECT * FROM read_sharepoint_library('...');

-- Filtered query
SELECT * FROM read_sharepoint('...', filter := 'Status eq ''Active''');

-- Limited results
SELECT * FROM read_sharepoint('...', top := 100);
```

### Useful Links

- GitHub repository template: https://github.com/duckdb/extension-template
- DuckDB Discord: https://discord.duckdb.org
- Microsoft Graph Explorer: https://developer.microsoft.com/en-us/graph/graph-explorer
- This tutorial: https://github.com/you/duckdb-sharepoint/tutorial
