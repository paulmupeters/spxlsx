# SharePoint Extension for DuckDB

DuckDB extension for querying SharePoint data directly from SQL.

> [!WARNING]
> This extension is experimental and not yet production-hardened. Expect rough edges around authentication, network behavior, and long-term compatibility while the project is still maturing.

## What this extension provides

- `read_sharepoint_list(url, ...)` table function to read SharePoint Lists
- `sharepoint_download_excel(url)` scalar function to download a SharePoint Excel file to a local temp file
- `read_sharepoint_excel(url, ...)` built-in table macro that combines `sharepoint_download_excel(...)` + DuckDB `read_xlsx(...)`
- SharePoint authentication through DuckDB secrets (`PROVIDER oauth` and `PROVIDER token`)

## Build

for incremental builds:

```sh
make GEN=ninja
```

Useful build artifacts:

- `./build/release/duckdb`
- `./build/release/test/unittest`
- `./build/release/extension/spxlsx/spxlsx.duckdb_extension`

## Load and authenticate

If you use the dev shell (`./build/release/duckdb`), the extension is already linked. For a regular DuckDB shell, load the extension explicitly:

```sql
LOAD '/absolute/path/to/build/release/extension/spxlsx/spxlsx.duckdb_extension';
```

Create a SharePoint secret (interactive OAuth flow):

```sql
CREATE SECRET (TYPE sharepoint, PROVIDER oauth);
```

Or provide an existing token:

```sql
CREATE SECRET (TYPE sharepoint, PROVIDER token, TOKEN 'your-access-token');
```

Use Persistent for the secret to work across restarts.

```sql
CREATE PERSISTENT SECRET (TYPE sharepoint, PROVIDER oauth);
```

## Reading SharePoint Lists

`read_sharepoint_list` expects a SharePoint list URL, for example:

```sql
SELECT *
FROM read_sharepoint_list('https://contoso.sharepoint.com/sites/MyTeam/Lists/Projects')
LIMIT 10;
```

Optional named parameters:

- `filter := '<odata filter expression>'`
- `top := <integer>`

Example:

```sql
SELECT ID, Title, Created
FROM read_sharepoint_list(
   'https://contoso.sharepoint.com/sites/MyTeam/Lists/Projects',
   filter := 'Status eq ''Active''',
   top := 100
)
ORDER BY Created DESC;
```

## Reading Excel files from SharePoint

Load the SharePoint extension:

```sql
LOAD spxlsx;
```

`LOAD spxlsx` now attempts to load DuckDB's `excel` extension automatically so `read_sharepoint_excel(...)` and `read_xlsx(...)` are available immediately.

If you are using an external DuckDB shell and `excel` has never been installed on that machine, install it once first:

```sql
INSTALL excel;
LOAD spxlsx;
```

Or enable DuckDB's automatic install/load path:

```sql
SET autoinstall_known_extensions = 1;
SET autoload_known_extensions = 1;
LOAD spxlsx;
```

### Fast path: `read_sharepoint_excel`

`read_sharepoint_excel` is pre-registered by this extension and supports:

- `sheet := ...`
- `header := ...`
- `all_varchar := ...`
- `ignore_errors := ...`
- `range := ...`
- `stop_at_empty := ...`
- `empty_as_varchar := ...`

By default, `ignore_errors := true` so mixed-type outliers in otherwise sparse columns are coerced to `NULL` instead of aborting the read. Use `ignore_errors := false` when you want strict parsing.

Selecting only a subset of output columns does not guarantee that the underlying Excel reader will skip other sheet columns during parsing. If unused columns contain problematic cells, narrow the workbook region explicitly with `range := ...`.

Then run:

```sql
SELECT *
FROM read_sharepoint_excel(
   'https://contoso.sharepoint.com/sites/Finance/Documents/Budget2024.xlsx',
   sheet := 'Q1 Data'
)
LIMIT 10;
```

### Advanced path: `sharepoint_download_excel` + `read_xlsx`

Use this when you need `read_xlsx` options or behavior beyond the macro surface:

```sql
SELECT *
FROM read_xlsx(
   (SELECT sharepoint_download_excel(
      'https://contoso.sharepoint.com/sites/Finance/Documents/Report.xlsx'
   )),
   sheet := 'Summary',
   range := 'A1:F100'
);
```

## Join list data with Excel data

```sql
SELECT
   l.Title,
   e.Value,
   e.Category
FROM read_sharepoint_list('https://contoso.sharepoint.com/sites/MyTeam/Lists/Projects') l
JOIN read_sharepoint_excel(
   'https://contoso.sharepoint.com/sites/MyTeam/Documents/ProjectData.xlsx',
   sheet := 'Financials'
) e ON l.ID = e.ProjectID;
```

## Tests

Run SQL tests:

```sh
make test
```

## Additional docs

- Example SQL script: `examples/excel_integration.sql`
- Update procedure notes: `docs/UPDATING.md`
