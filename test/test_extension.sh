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
./build/release/duckdb -c "LOAD '../build/release/extension/spxlsx/spxlsx.duckdb_extension'; SELECT 'Extension loaded successfully!' as status;"
