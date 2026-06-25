PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=spxlsx
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: test_spxlsx_unit_release test_spxlsx_unit_debug test_spxlsx_unit_reldebug

test_release: test_spxlsx_unit_release
test_debug: test_spxlsx_unit_debug
test_reldebug: test_spxlsx_unit_reldebug

test_spxlsx_unit_release:
	./build/release/extension/spxlsx/sharepoint_auth_refresh_test
	./build/release/extension/spxlsx/sharepoint_requests_test

test_spxlsx_unit_debug:
	./build/debug/extension/spxlsx/sharepoint_auth_refresh_test
	./build/debug/extension/spxlsx/sharepoint_requests_test

test_spxlsx_unit_reldebug:
	./build/reldebug/extension/spxlsx/sharepoint_auth_refresh_test
	./build/reldebug/extension/spxlsx/sharepoint_requests_test
