# 08 - Build & Deployment

In this module, you'll learn how to build, package, and distribute your SharePoint extension for different platforms and use cases.

## Goals
- Build for multiple platforms
- Package the extension properly
- Set up continuous integration
- Distribute to users
- Version management

## Step 1: Multi-Platform Builds

### Local Platform Build

```bash
# Build for your current platform
export VCPKG_TOOLCHAIN_PATH=$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake
GEN=ninja make

# Output location:
# build/release/extension/sharepoint/sharepoint.duckdb_extension
```

### Cross-Platform with Docker

Create `Dockerfile.build`:

```dockerfile
# Build extension for Linux
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    git \
    cmake \
    ninja-build \
    build-essential \
    libssl-dev \
    python3 \
    curl

WORKDIR /build

# Copy source
COPY . .

# Clone VCPKG
RUN git clone https://github.com/Microsoft/vcpkg.git && \
    cd vcpkg && \
    ./bootstrap-vcpkg.sh

# Build extension
ENV VCPKG_TOOLCHAIN_PATH=/build/vcpkg/scripts/buildsystems/vcpkg.cmake
RUN GEN=ninja make

# Extension will be in build/release/extension/sharepoint/
```

Build with Docker:

```bash
docker build -f Dockerfile.build -t sharepoint-extension-builder .
docker run -v $(pwd)/dist:/output sharepoint-extension-builder \
    cp build/release/extension/sharepoint/sharepoint.duckdb_extension /output/
```

## Step 2: CI/CD Pipeline

Create `.github/workflows/build.yml`:

```yaml
name: Build Extension

on:
  push:
    branches: [ main ]
  pull_request:
    branches: [ main ]
  release:
    types: [ created ]

jobs:
  linux:
    name: Linux Build
    runs-on: ubuntu-latest

    steps:
    - uses: actions/checkout@v3
      with:
        submodules: recursive

    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake ninja-build

    - name: Setup VCPKG
      run: |
        git clone https://github.com/Microsoft/vcpkg.git
        cd vcpkg && ./bootstrap-vcpkg.sh

    - name: Build
      run: |
        export VCPKG_TOOLCHAIN_PATH=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake
        GEN=ninja make

    - name: Upload artifact
      uses: actions/upload-artifact@v3
      with:
        name: sharepoint-linux-amd64
        path: build/release/extension/sharepoint/sharepoint.duckdb_extension

  macos:
    name: macOS Build
    runs-on: macos-latest

    steps:
    - uses: actions/checkout@v3
      with:
        submodules: recursive

    - name: Install dependencies
      run: |
        brew install cmake ninja

    - name: Setup VCPKG
      run: |
        git clone https://github.com/Microsoft/vcpkg.git
        cd vcpkg && ./bootstrap-vcpkg.sh

    - name: Build
      run: |
        export VCPKG_TOOLCHAIN_PATH=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake
        GEN=ninja make

    - name: Upload artifact
      uses: actions/upload-artifact@v3
      with:
        name: sharepoint-macos-universal
        path: build/release/extension/sharepoint/sharepoint.duckdb_extension

  windows:
    name: Windows Build
    runs-on: windows-latest

    steps:
    - uses: actions/checkout@v3
      with:
        submodules: recursive

    - name: Setup VCPKG
      run: |
        git clone https://github.com/Microsoft/vcpkg.git
        cd vcpkg
        .\bootstrap-vcpkg.bat

    - name: Build
      shell: bash
      run: |
        export VCPKG_TOOLCHAIN_PATH=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake
        GEN=ninja make

    - name: Upload artifact
      uses: actions/upload-artifact@v3
      with:
        name: sharepoint-windows-amd64
        path: build/release/extension/sharepoint/sharepoint.duckdb_extension

  release:
    name: Create Release
    needs: [linux, macos, windows]
    runs-on: ubuntu-latest
    if: github.event_name == 'release'

    steps:
    - name: Download all artifacts
      uses: actions/download-artifact@v3

    - name: Upload release assets
      uses: softprops/action-gh-release@v1
      with:
        files: |
          sharepoint-linux-amd64/sharepoint.duckdb_extension
          sharepoint-macos-universal/sharepoint.duckdb_extension
          sharepoint-windows-amd64/sharepoint.duckdb_extension
```

## Step 3: Version Management

Create `version.hpp`:

```cpp
#pragma once

#define SHAREPOINT_EXTENSION_VERSION_MAJOR 0
#define SHAREPOINT_EXTENSION_VERSION_MINOR 1
#define SHAREPOINT_EXTENSION_VERSION_PATCH 0

#define SHAREPOINT_EXTENSION_VERSION \
    STRINGIFY(SHAREPOINT_EXTENSION_VERSION_MAJOR) "." \
    STRINGIFY(SHAREPOINT_EXTENSION_VERSION_MINOR) "." \
    STRINGIFY(SHAREPOINT_EXTENSION_VERSION_PATCH)

#define STRINGIFY(x) STRINGIFY2(x)
#define STRINGIFY2(x) #x
```

Use in `sharepoint_extension.cpp`:

```cpp
#include "version.hpp"

extern "C" {
DUCKDB_EXTENSION_API const char *sharepoint_version() {
    return SHAREPOINT_EXTENSION_VERSION;
}
}
```

## Step 4: Package for Distribution

### Create Release Package

```bash
#!/bin/bash
# scripts/package.sh

VERSION="0.1.0"
PLATFORMS=("linux-amd64" "macos-universal" "windows-amd64")

mkdir -p dist

for platform in "${PLATFORMS[@]}"; do
    pkg_name="duckdb-sharepoint-${VERSION}-${platform}"
    pkg_dir="dist/${pkg_name}"

    mkdir -p "$pkg_dir"

    # Copy extension
    cp "build/release/extension/sharepoint/sharepoint.duckdb_extension" "$pkg_dir/"

    # Copy documentation
    cp README.md "$pkg_dir/"
    cp LICENSE "$pkg_dir/"

    # Create README for package
    cat > "$pkg_dir/INSTALL.md" << EOF
# DuckDB SharePoint Extension

Version: ${VERSION}
Platform: ${platform}

## Installation

1. Copy sharepoint.duckdb_extension to your DuckDB extension directory
2. Load in DuckDB:

\`\`\`sql
LOAD 'sharepoint';
\`\`\`

## Usage

\`\`\`sql
-- Authenticate
CREATE SECRET (TYPE sharepoint, PROVIDER oauth);

-- Query a list
SELECT * FROM read_sharepoint('https://...');
\`\`\`

## Documentation

See README.md for full documentation.
EOF

    # Create archive
    cd dist
    tar czf "${pkg_name}.tar.gz" "${pkg_name}"
    zip -r "${pkg_name}.zip" "${pkg_name}"
    cd ..

    echo "Created package: ${pkg_name}"
done
```

## Step 5: Installation Methods

### Method 1: Manual Installation

```bash
# Download extension
wget https://github.com/you/duckdb-sharepoint/releases/download/v0.1.0/duckdb-sharepoint-0.1.0-linux-amd64.tar.gz

# Extract
tar xzf duckdb-sharepoint-0.1.0-linux-amd64.tar.gz

# Copy to DuckDB extensions directory
mkdir -p ~/.duckdb/extensions
cp duckdb-sharepoint-0.1.0-linux-amd64/sharepoint.duckdb_extension ~/.duckdb/extensions/

# Use in DuckDB
duckdb -c "LOAD 'sharepoint'; SELECT 1;"
```

### Method 2: INSTALL Command (Future)

Once published to DuckDB extension repository:

```sql
INSTALL sharepoint FROM community;
LOAD sharepoint;
```

### Method 3: Direct Load

```sql
-- Load from local path
LOAD '/path/to/sharepoint.duckdb_extension';

-- Load from URL
LOAD 'https://github.com/you/duckdb-sharepoint/releases/download/v0.1.0/sharepoint.duckdb_extension';
```

## Step 6: Documentation for Users

Create `README.md`:

```markdown
# DuckDB SharePoint Extension

Query SharePoint Lists and Document Libraries with SQL.

## Features

- 🔐 Azure AD OAuth authentication
- 📊 Read SharePoint Lists as SQL tables
- 📁 Access Document Library metadata
- 🔍 Server-side filtering and pagination
- 🚀 High performance with caching
- 🔄 Automatic schema detection

## Installation

Download the extension for your platform from [Releases](https://github.com/you/duckdb-sharepoint/releases):

- Linux (x64)
- macOS (Universal)
- Windows (x64)

## Quick Start

```sql
-- Load extension
LOAD 'sharepoint';

-- Authenticate with SharePoint
CREATE SECRET sharepoint_creds (TYPE sharepoint, PROVIDER oauth);

-- Query a SharePoint list
SELECT * FROM read_sharepoint(
    'https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees'
);

-- Query document library
SELECT name, size, modified
FROM read_sharepoint_library(
    'https://contoso.sharepoint.com/sites/TeamSite/Documents'
);
```

## Authentication

### Interactive OAuth (Recommended)

```sql
CREATE SECRET sharepoint_creds (TYPE sharepoint, PROVIDER oauth);
```

### Manual Token

```sql
CREATE SECRET sharepoint_creds (
    TYPE sharepoint,
    PROVIDER token,
    TOKEN 'your-access-token-here'
);
```

## Examples

See [examples/](examples/) for more usage examples.

## Requirements

- DuckDB 0.9.0 or higher
- Azure AD application (for OAuth)
- SharePoint Online access

## Building from Source

See [BUILD.md](BUILD.md) for build instructions.

## License

MIT License - see [LICENSE](LICENSE)

## Support

- GitHub Issues: https://github.com/you/duckdb-sharepoint/issues
- Documentation: https://github.com/you/duckdb-sharepoint/wiki
```

## Step 7: Create Example Files

Create `examples/basic_queries.sql`:

```sql
-- Basic SharePoint queries examples

-- Load extension
LOAD 'sharepoint';

-- Create secret
CREATE SECRET sharepoint_creds (TYPE sharepoint, PROVIDER oauth);

-- Example 1: Simple list query
SELECT *
FROM read_sharepoint('https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees')
LIMIT 10;

-- Example 2: Aggregation
SELECT Department, COUNT(*) as employee_count, AVG(Salary) as avg_salary
FROM read_sharepoint('https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees')
GROUP BY Department
ORDER BY employee_count DESC;

-- Example 3: Join with local data
CREATE TABLE budget AS
    SELECT 'Engineering' as dept, 1000000 as budget
    UNION ALL
    SELECT 'Sales', 500000;

SELECT b.dept, b.budget, COUNT(e.*) as headcount,
       b.budget / COUNT(e.*) as budget_per_employee
FROM budget b
LEFT JOIN read_sharepoint('https://contoso.sharepoint.com/sites/TeamSite/Lists/Employees') e
    ON b.dept = e.Department
GROUP BY b.dept, b.budget;

-- Example 4: Export to CSV
COPY (
    SELECT *
    FROM read_sharepoint('https://contoso.sharepoint.com/sites/TeamSite/Lists/Projects')
    WHERE Status = 'Active'
) TO 'active_projects.csv' (HEADER, DELIMITER ',');
```

## Step 8: Semantic Versioning

Follow semantic versioning (MAJOR.MINOR.PATCH):

- **MAJOR**: Breaking changes (e.g., change function signatures)
- **MINOR**: New features (e.g., add new function)
- **PATCH**: Bug fixes

Example changelog in `CHANGELOG.md`:

```markdown
# Changelog

## [0.2.0] - 2024-02-01

### Added
- Support for document library recursive traversal
- Optional parameters for filtering
- Performance improvements with caching

### Fixed
- Handle NULL values in date fields
- URL encoding for special characters

## [0.1.0] - 2024-01-01

### Added
- Initial release
- Read SharePoint Lists
- Read Document Libraries
- OAuth authentication
- Manual token authentication
```

## Step 9: Distribution Checklist

Before releasing:

- [ ] All tests pass
- [ ] Documentation is complete
- [ ] Version number updated
- [ ] CHANGELOG updated
- [ ] Build artifacts for all platforms
- [ ] Example files included
- [ ] LICENSE file included
- [ ] Security audit completed
- [ ] Performance benchmarks run
- [ ] User feedback incorporated

## Step 10: Publishing to DuckDB Extension Repository

To publish to the official DuckDB extension repository:

1. **Fork** the DuckDB extension template
2. **Submit PR** to https://github.com/duckdb/extension-template
3. **Pass review** by DuckDB maintainers
4. **Automatic builds** will be set up
5. **Users can install** with `INSTALL sharepoint FROM community`

Requirements:
- Open source license
- Comprehensive tests
- Documentation
- Cross-platform support
- Security review

## What You've Accomplished

You now know how to:
- Build for multiple platforms
- Set up CI/CD pipelines
- Package and distribute extensions
- Version and document releases
- Publish to users

Your extension is ready for the world!

---

**Navigation:**
- Previous: [07 - Testing & Debugging](./07-testing-debugging.md)
- Next: [09 - Next Steps](./09-next-steps.md)
