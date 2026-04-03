# Testing this extension
This directory contains the tests for the SharePoint extension. The `sql` directory holds [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html), and the shell script provides a simple load/build smoke test.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```