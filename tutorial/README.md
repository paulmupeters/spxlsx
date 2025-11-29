# DuckDB SharePoint Extension Tutorial

A complete, step-by-step guide to building a DuckDB extension that connects to SharePoint Online.

## Overview

This tutorial will guide you through creating a production-ready DuckDB extension that allows users to query SharePoint Lists and Document Libraries (including Excel files) using SQL.

## What You'll Build

```sql
-- Query SharePoint Lists
SELECT * FROM read_sharepoint('https://contoso.sharepoint.com/sites/Team/Lists/Employees');

-- Query Document Libraries
SELECT name, size, modified
FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/Team/Documents');

-- Query Excel files stored in SharePoint
SELECT * FROM read_sharepoint_excel(
    'https://contoso.sharepoint.com/sites/Finance/Reports/Budget2024.xlsx',
    sheet := 'Q1 Data'
);
```

## Tutorial Structure

### Core Modules (Essential)

| Module | File | Description | Time |
|--------|------|-------------|------|
| **00** | [introduction.md](./00-introduction.md) | Overview, architecture, prerequisites | 15 min |
| **01** | [project-setup.md](./01-project-setup.md) | Create project structure, dependencies | 30 min |
| **02** | [extension-basics.md](./02-extension-basics.md) | Minimal working extension | 45 min |
| **03** | [http-layer.md](./03-http-layer.md) | HTTPS client with OpenSSL | 1 hour |
| **04** | [authentication.md](./04-authentication.md) | Azure AD OAuth implementation | 1.5 hours |
| **05** | [sharepoint-lists.md](./05-sharepoint-lists.md) | Read SharePoint Lists | 2 hours |

**Total Core: ~6 hours**

### Extended Features (Important)

| Module | File | Description | Time |
|--------|------|-------------|------|
| **06** | [document-libraries.md](./06-document-libraries.md) | Read Document Library metadata | 1 hour |
| **07** | [testing-debugging.md](./07-testing-debugging.md) | Testing and debugging strategies | 1 hour |
| **08** | [build-deployment.md](./08-build-deployment.md) | Multi-platform builds, CI/CD | 1 hour |

**Total Extended: ~3 hours**

### Advanced Topics (Optional)

| Module | File | Description | Time |
|--------|------|-------------|------|
| **09** | [next-steps.md](./09-next-steps.md) | Advanced features, optimizations | 30 min |
| **10** | [excel-integration.md](./10-excel-integration.md) | Query Excel files from SharePoint | 1 hour |

**Total Advanced: ~1.5 hours**

## Learning Paths

### Quick Start (Core Features Only)
Complete modules 00-05 to get a working extension that reads SharePoint Lists.
**Time: ~6 hours**

### Full Implementation (Recommended)
Complete modules 00-08 for a production-ready extension with tests and deployment.
**Time: ~9 hours**

### Complete Mastery
Complete all modules including advanced Excel integration and optimizations.
**Time: ~10.5 hours**

## Prerequisites

### Knowledge
- ✅ C++ fundamentals (classes, pointers, STL)
- ✅ Basic understanding of HTTP/REST APIs
- ✅ Familiarity with JSON data format
- ✅ Basic OAuth concepts (explained in tutorial)
- ✅ CMake basics

### Environment
- ✅ macOS, Linux, or Windows with WSL
- ✅ GCC 9+ or Clang 10+
- ✅ CMake 3.15+
- ✅ Git
- ✅ OpenSSL development libraries

### SharePoint Access
- ✅ Microsoft 365 account with SharePoint Online
- ✅ Ability to create Azure AD app registrations
- ✅ Access to a test SharePoint site

## Quick Reference

### Key Technologies
- **DuckDB**: Analytical database engine
- **Microsoft Graph API**: SharePoint data access
- **Azure AD OAuth**: Authentication
- **OpenSSL**: HTTPS communication
- **nlohmann/json**: JSON parsing

### Important Files You'll Create

```
duckdb_sharepoint/
├── CMakeLists.txt                    # Build configuration
├── extension_config.cmake            # Extension registration
├── vcpkg.json                        # Dependencies
├── src/
│   ├── sharepoint_extension.cpp      # Entry point (Module 02)
│   ├── sharepoint_auth.cpp           # OAuth authentication (Module 04)
│   ├── sharepoint_requests.cpp       # HTTP client (Module 03)
│   ├── sharepoint_read.cpp           # Table functions (Module 05)
│   ├── sharepoint_utils.cpp          # Utilities (Module 03)
│   └── sharepoint_excel.cpp          # Excel integration (Module 10)
└── test/
    └── sql/*.test                    # SQL tests (Module 07)
```

### Reference Implementation
The Google Sheets extension at `/Users/paulpeters/Developer/duckdb_gsheets` serves as a reference for patterns and best practices.

## Getting Help

### During Development
- 📖 Read the code in the Google Sheets extension
- 🔍 Check [Microsoft Graph API docs](https://docs.microsoft.com/en-us/graph/api/overview)
- 🦆 Review [DuckDB extension docs](https://duckdb.org/docs/extensions/overview)
- 🧪 Test with [Graph Explorer](https://developer.microsoft.com/en-us/graph/graph-explorer)

### Common Issues
Each tutorial module includes:
- ✅ Common Issues & Solutions sections
- ✅ Debugging tips
- ✅ Error handling examples
- ✅ Troubleshooting checklists

## Example Use Cases

### Business Intelligence
```sql
-- Combine SharePoint lists, Excel files, and local data
SELECT
    e.Name,
    e.Department,
    s.Salary,
    b.Budget
FROM read_sharepoint('https://.../Lists/Employees') e
JOIN read_sharepoint_excel('https://.../Salaries.xlsx') s
    ON e.ID = s.EmployeeID
JOIN local_budgets b
    ON e.Department = b.Dept;
```

### Data Migration
```sql
-- Migrate SharePoint data to Parquet
COPY (
    SELECT * FROM read_sharepoint('https://.../Lists/OldData')
) TO 'migrated_data.parquet';
```

### Reporting
```sql
-- Generate reports from SharePoint document libraries
SELECT
    name,
    size / 1024 / 1024 as size_mb,
    modified,
    created_by
FROM read_sharepoint_library('https://.../Documents', recursive := true)
WHERE modified > CURRENT_DATE - 30
ORDER BY size DESC;
```

## Tips for Success

1. **Build Incrementally**: Complete each module before moving to the next
2. **Test Frequently**: Verify each component works before adding more
3. **Reference Often**: Use the Google Sheets extension as a guide
4. **Debug Early**: Use the debugging techniques from Module 07
5. **Ask Questions**: Comment your code with questions and revisit them

## After Completing This Tutorial

You'll be able to:
- ✅ Build complex DuckDB extensions
- ✅ Integrate with external APIs
- ✅ Implement OAuth authentication
- ✅ Parse and transform data
- ✅ Test and debug C++ code
- ✅ Package and distribute software

These skills transfer to building extensions for other services like:
- Salesforce, Jira, Confluence
- GitHub, GitLab
- Google Workspace (beyond Sheets)
- Any REST API!

## Contributing

After building your extension:
- 📝 Share your experience
- 🐛 Report issues or improvements to this tutorial
- 🎓 Help others learning extension development
- 🚀 Publish your extension for others to use

## License

This tutorial is provided as educational material. The code you create following this tutorial is yours to license as you wish.

---

**Ready to start?** → [Begin with Module 00: Introduction](./00-introduction.md)

**Have questions?** Review the prerequisites or check the reference implementation at `/Users/paulpeters/Developer/duckdb_gsheets`
