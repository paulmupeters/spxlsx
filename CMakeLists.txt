cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME spxlsx)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

if (MSVC)
  # Newer MSVC toolchains need C++17 to compile DuckDB's bundled fmt headers.
  set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to enforce" FORCE)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  get_directory_property(SPXLSX_PARENT_DIRECTORY PARENT_DIRECTORY)
  if (SPXLSX_PARENT_DIRECTORY)
    set(CMAKE_CXX_STANDARD 17 PARENT_SCOPE)
    set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)
    set(CMAKE_CXX_EXTENSIONS OFF PARENT_SCOPE)
  endif()
endif()

include_directories(src/include)
include_directories(third_party/json/single_include)

# Source files
set(EXTENSION_SOURCES
    src/sharepoint_extension.cpp
    src/sharepoint_auth.cpp
    src/sharepoint_oauth.cpp
    src/sharepoint_read.cpp
    src/sharepoint_requests.cpp
    src/sharepoint_utils.cpp
    src/sharepoint_excel.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if (NOT EMSCRIPTEN)
  enable_testing()

  add_executable(sharepoint_auth_refresh_test
      test/sharepoint_auth_refresh_test.cpp
      src/sharepoint_oauth.cpp
  )

  add_executable(sharepoint_requests_test
      test/sharepoint_requests_test.cpp
  )
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW)
    # GNU ld resolves static archives from left to right. DuckDB's generated
    # extension loader and the extension archives depend on each other, so
    # GNU-style linkers need the archive group to be rescanned.
    target_link_libraries(sharepoint_requests_test "-Wl,--start-group")
    link_extension_libraries(sharepoint_requests_test "")
    target_link_libraries(sharepoint_requests_test duckdb_static "-Wl,--end-group")
  else()
    link_extension_libraries(sharepoint_requests_test "")
  endif()

  add_test(NAME sharepoint_auth_refresh_test COMMAND sharepoint_auth_refresh_test)
  add_test(NAME sharepoint_requests_test COMMAND sharepoint_requests_test)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
