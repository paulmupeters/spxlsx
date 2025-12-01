# 01 - Project Setup

In this module, you'll create the project structure and configure the build system for your SharePoint extension.

## Goals
- Create the directory structure
- Set up CMake configuration
- Configure dependencies (OpenSSL, JSON library)
- Create initial source files
- Verify the build system works

## Step 1: Create Directory Structure

Create your project directory and navigate into it:

```bash
mkdir duckdb_sharepoint
cd duckdb_sharepoint
```

Now create the following directory structure:

```
duckdb_sharepoint/
├── CMakeLists.txt
├── Makefile
├── extension_config.cmake
├── vcpkg.json
├── src/
│   ├── include/
│   │   ├── sharepoint_extension.hpp
│   │   ├── sharepoint_auth.hpp
│   │   ├── sharepoint_read.hpp
│   │   ├── sharepoint_requests.hpp
│   │   └── sharepoint_utils.hpp
│   ├── sharepoint_extension.cpp
│   ├── sharepoint_auth.cpp
│   ├── sharepoint_read.cpp
│   ├── sharepoint_requests.cpp
│   └── sharepoint_utils.cpp
├── third_party/
│   └── json/
│       └── single_include/
│           └── nlohmann/
│               └── json.hpp
└── test/
    └── sql/
        └── read_sharepoint.test
```

Create these directories:

```bash
mkdir -p src/include
mkdir -p third_party/json/single_include/nlohmann
mkdir -p test/sql
```

## Step 2: Download Dependencies

### Get nlohmann/json Library

This is a header-only JSON library that makes parsing SharePoint API responses easy:

```bash
curl -L https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp -o third_party/json/single_include/nlohmann/json.hpp
```

### Set Up VCPKG for OpenSSL

VCPKG is a package manager that will handle OpenSSL compilation across platforms. Create `vcpkg.json`:

```json
{
  "name": "sharepoint",
  "version": "0.1.0",
  "dependencies": [
    "openssl"
  ]
}
```

This tells VCPKG to install OpenSSL for us during the build process.

## Step 3: Configure Build System

### Create extension_config.cmake

This file tells DuckDB about your extension:

```cmake
# Extension metadata
duckdb_extension_load(sharepoint
    DONT_LINK
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

### Create CMakeLists.txt

This is your main build configuration:

```cmake
cmake_minimum_required(VERSION 3.15)
project(SharePointExtension)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Extension name
set(TARGET_NAME sharepoint)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Include DuckDB extension CMake helpers
include_directories(src/include)
include_directories(third_party/json/single_include)

# Source files
set(EXTENSION_SOURCES
    src/sharepoint_extension.cpp
    src/sharepoint_auth.cpp
    src/sharepoint_read.cpp
    src/sharepoint_requests.cpp
    src/sharepoint_utils.cpp
)

# Build static extension (embedded in DuckDB)
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})

# Build loadable extension (.duckdb_extension file)
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL to both versions
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

# Include directories for OpenSSL
target_include_directories(${EXTENSION_NAME} PRIVATE ${OPENSSL_INCLUDE_DIR})
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${OPENSSL_INCLUDE_DIR})

# Install loadable extension
install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
```

### Key CMake Concepts

- **`build_static_extension`**: Creates a version that's compiled directly into DuckDB
- **`build_loadable_extension`**: Creates a `.duckdb_extension` file that can be loaded dynamically
- **OpenSSL linking**: Required for HTTPS communication with SharePoint
- **`EXTENSION_SOURCES`**: List all your .cpp files here

### Create Makefile

This is a convenience wrapper around CMake (mimicking the Google Sheets extension):

```makefile
.PHONY: all clean format debug release pull update

all: release

MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
PROJ_DIR := $(dir $(MKFILE_PATH))

# Check if VCPKG_TOOLCHAIN_PATH is set
ifeq ($(VCPKG_TOOLCHAIN_PATH),)
    $(error VCPKG_TOOLCHAIN_PATH is not set. Please set it to your VCPKG toolchain file path.)
endif

# Set build generator (ninja is faster than make)
ifeq ($(GEN),ninja)
    GENERATOR=-G "Ninja"
    FORCE_COLOR=-DFORCE_COLORED_OUTPUT=1
else
    GENERATOR=
    FORCE_COLOR=
endif

# Build configuration
BUILD_FLAGS=-DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN_PATH) \
            -DEXTENSION_STATIC_BUILD=1 \
            $(GENERATOR) $(FORCE_COLOR)

release:
	mkdir -p build/release && \
	cd build/release && \
	cmake $(PROJ_DIR) $(BUILD_FLAGS) && \
	cmake --build . --config Release

debug:
	mkdir -p build/debug && \
	cd build/debug && \
	cmake $(PROJ_DIR) -DCMAKE_BUILD_TYPE=Debug \
	      -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_TOOLCHAIN_PATH) \
	      $(GENERATOR) && \
	cmake --build . --config Debug

clean:
	rm -rf build

test: release
	cd build/release && ctest --output-on-failure

format:
	find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

pull:
	git submodule update --remote --merge

update:
	git submodule update --remote --merge
```

## Step 4: Set Up DuckDB Submodule

Your extension needs to build against DuckDB's source code. Add DuckDB as a git submodule:

```bash
# Initialize git repo if you haven't
git init

# Add DuckDB as submodule
git submodule add https://github.com/duckdb/duckdb.git
git submodule update --init --recursive
```

## Step 5: Install VCPKG

VCPKG will manage OpenSSL for us:

```bash
# Clone VCPKG
git clone https://github.com/Microsoft/vcpkg.git

# Bootstrap VCPKG
cd vcpkg
./bootstrap-vcpkg.sh  # On Linux/macOS
# or
./bootstrap-vcpkg.bat  # On Windows

cd ..
```

## Step 6: Set Environment Variable

Tell the build system where to find VCPKG:

```bash
export VCPKG_TOOLCHAIN_PATH=$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Add this to your `.bashrc` or `.zshrc` to make it permanent:

```bash
echo "export VCPKG_TOOLCHAIN_PATH=$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake" >> ~/.bashrc
source ~/.bashrc
```

## Step 7: Project Structure Reference

Your project should now look like this:

```
duckdb_sharepoint/
├── CMakeLists.txt           # Main build config
├── Makefile                 # Build wrapper
├── extension_config.cmake   # DuckDB extension registration
├── vcpkg.json              # Dependencies
├── duckdb/                 # Git submodule (DuckDB source)
├── vcpkg/                  # Package manager
├── src/                    # Your source code
│   ├── include/            # Header files
│   └── *.cpp              # Implementation files
├── third_party/
│   └── json.hpp           # JSON library
└── test/
    └── sql/               # SQL test files
```

## Step 8: Verify Setup (Coming in Next Module)

We'll create a minimal extension in the next module to verify everything builds correctly. For now, you have:

- Project structure created
- Build system configured
- Dependencies ready to install
- DuckDB source code available

## Common Setup Issues

### Issue: CMake can't find OpenSSL

**Solution**: Ensure VCPKG is properly bootstrapped and `VCPKG_TOOLCHAIN_PATH` is set:
```bash
echo $VCPKG_TOOLCHAIN_PATH  # Should output path to vcpkg cmake file
```

### Issue: DuckDB submodule is empty

**Solution**: Initialize submodules:
```bash
git submodule update --init --recursive
```

### Issue: Build generator 'Ninja' not found

**Solution**: Either install ninja or build without it:
```bash
# Install ninja (recommended for faster builds)
brew install ninja  # macOS
apt-get install ninja-build  # Ubuntu

# Or build with make
make  # Uses default make generator
```

## What's Next?

Now that your project is set up, you'll create a minimal working extension that loads into DuckDB. This will verify your build system works before we add complexity.

---

**Navigation:**
- Previous: [00 - Introduction](./00-introduction.md)
- Next: [02 - Extension Basics](./02-extension-basics.md)
