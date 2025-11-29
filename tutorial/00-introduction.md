# Building a DuckDB SharePoint Extension

## Welcome!

This tutorial will guide you through building a DuckDB extension that connects to SharePoint Online, allowing you to query SharePoint Lists and Document Libraries using SQL. By the end, you'll be able to run queries like:

```sql
-- Query a SharePoint list
SELECT * FROM read_sharepoint('https://contoso.sharepoint.com/sites/MyTeam/Lists/Employees');

-- Query document library metadata
SELECT Name, Size, Modified FROM read_sharepoint_library('https://contoso.sharepoint.com/sites/MyTeam/Documents');
```

## What You'll Build

Your extension will:
- **Authenticate** with SharePoint Online using Azure AD OAuth
- **Read data** from SharePoint Lists with automatic schema detection
- **Access file metadata** from Document Libraries
- **Handle API communication** with Microsoft Graph API
- **Integrate seamlessly** with DuckDB's query engine

## Prerequisites

### Knowledge Requirements
- **C++ fundamentals**: Classes, pointers, STL containers
- **Basic understanding of**:
  - HTTP/REST APIs
  - JSON data format
  - OAuth authentication flow (we'll explain the details)
  - CMake build system
- **SharePoint access**: A Microsoft 365 account with access to SharePoint Online

### Development Environment
- **Operating System**: macOS, Linux, or Windows with WSL
- **Compiler**: GCC 9+ or Clang 10+
- **CMake**: Version 3.15 or higher
- **Git**: For cloning DuckDB and managing your code
- **OpenSSL**: Development libraries (libssl-dev)
- **A text editor or IDE**: VSCode, CLion, or your favorite

### SharePoint Setup
You'll need:
- A SharePoint Online site you can access
- Permission to create Azure AD app registrations (or work with your admin)
- A test SharePoint list with some data

## Architecture Overview

Your extension will follow the same pattern as the Google Sheets extension you've seen:

```
┌─────────────────────────────────────────────────────────┐
│                    DuckDB Core                          │
│  (Query Parser, Optimizer, Execution Engine)            │
└────────────────────┬────────────────────────────────────┘
                     │
                     │ Extension API
                     ▼
┌─────────────────────────────────────────────────────────┐
│              Your SharePoint Extension                   │
│                                                          │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐ │
│  │   Table     │  │ OAuth/Token  │  │  HTTP/HTTPS   │ │
│  │  Functions  │  │  Management  │  │  Client       │ │
│  └─────────────┘  └──────────────┘  └───────────────┘ │
│                                                          │
│  ┌─────────────────────────────────────────────────┐   │
│  │  Type Mapping & Data Conversion                 │   │
│  │  (SharePoint → DuckDB types)                    │   │
│  └─────────────────────────────────────────────────┘   │
└────────────────────┬────────────────────────────────────┘
                     │
                     │ Microsoft Graph API / SharePoint REST API
                     ▼
┌─────────────────────────────────────────────────────────┐
│              SharePoint Online (Microsoft 365)           │
│                                                          │
│     Lists  │  Libraries  │  Sites  │  Permissions       │
└─────────────────────────────────────────────────────────┘
```

### Key Components You'll Build

1. **Extension Entry Point** (`sharepoint_extension.cpp`)
   - Registers your extension with DuckDB
   - Initializes OpenSSL and other dependencies

2. **Table Functions** (`sharepoint_read.cpp`)
   - `read_sharepoint()`: Reads SharePoint Lists
   - `read_sharepoint_library()`: Reads Document Library metadata
   - Schema discovery and type inference

3. **Authentication Module** (`sharepoint_auth.cpp`)
   - Azure AD OAuth flow with browser redirect
   - Token management and caching
   - Integration with DuckDB's secret manager

4. **HTTP Client** (`sharepoint_requests.cpp`)
   - HTTPS communication using OpenSSL
   - Microsoft Graph API calls
   - Error handling and retry logic

5. **Utilities** (`sharepoint_utils.cpp`)
   - URL parsing (SharePoint URLs)
   - Type conversion helpers
   - JSON parsing (using nlohmann/json)

## Learning Path

This tutorial is organized into 9 modules, designed to be completed in order:

1. **Project Setup**: Create the extension scaffold, configure build system
2. **Extension Basics**: Minimal working extension that loads into DuckDB
3. **HTTP Layer**: Build HTTPS client for API communication
4. **Authentication**: Implement Azure AD OAuth flow
5. **SharePoint Lists**: Create table function for reading list data
6. **Document Libraries**: Extend to read file metadata
7. **Testing & Debugging**: Write tests and debug effectively
8. **Build & Deployment**: Package and distribute your extension
9. **Next Steps**: Write operations, optimizations, advanced features

## Time Estimate

- **Quick path** (basic read functionality): 6-10 hours
- **Complete path** (all features): 15-25 hours
- Spread this over several sessions - no need to rush!

## Reference: Google Sheets Extension

Throughout this tutorial, we'll reference the Google Sheets extension at `/Users/paulpeters/Developer/duckdb_gsheets` as a working example. The patterns are very similar:

| Component | Google Sheets | SharePoint |
|-----------|---------------|------------|
| **API** | Google Sheets API | Microsoft Graph API |
| **Auth** | Google OAuth | Azure AD OAuth |
| **Data Source** | Spreadsheets | Lists/Libraries |
| **URL Format** | `docs.google.com/spreadsheets/d/{ID}` | `{tenant}.sharepoint.com/sites/{site}` |
| **Primary Data** | Cells in ranges | List items / Files |

## Getting Help

As you work through this tutorial:
- **Read the code**: The Google Sheets extension is fully commented
- **Check API docs**: [Microsoft Graph API documentation](https://docs.microsoft.com/en-us/graph/api/overview)
- **DuckDB docs**: [Extension development guide](https://duckdb.org/docs/extensions/overview)
- **Experiment**: Build incrementally, test frequently

## Ready?

Let's start building! Head to **01-project-setup.md** to create your project structure.

---

**Navigation:**
- Next: [01 - Project Setup](./01-project-setup.md)
