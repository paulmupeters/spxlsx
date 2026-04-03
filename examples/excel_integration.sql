-- Example: Using SharePoint Excel Integration
-- This example shows how to query Excel files from SharePoint

-- Step 1: Load the SharePoint extension
LOAD sharepoint;

-- If excel has never been installed in this DuckDB environment, run this once first:
-- INSTALL excel;

-- Or enable DuckDB's automatic install/load path before LOAD sharepoint:
-- SET autoinstall_known_extensions = 1;
-- SET autoload_known_extensions = 1;

-- Step 2: Authenticate with SharePoint
CREATE SECRET (TYPE sharepoint, PROVIDER oauth);

-- Step 3: Query an Excel file
-- read_sharepoint_excel is a built-in table macro, no CREATE MACRO needed!
-- Replace with your actual SharePoint URL
SELECT * FROM read_sharepoint_excel(
    'https://contoso.sharepoint.com/sites/Finance/Documents/Budget2024.xlsx',
    sheet := 'Q1 Data'
)
LIMIT 10;

-- Example: Join SharePoint List with Excel file
SELECT
    l.Title,
    e.Value,
    e.Category
FROM read_sharepoint('https://contoso.sharepoint.com/sites/MyTeam/Lists/Projects') l
JOIN read_sharepoint_excel(
    'https://contoso.sharepoint.com/sites/MyTeam/Documents/ProjectData.xlsx',
    sheet := 'Financials'
) e ON l.ID = e.ProjectID;

-- Example: Read with all options
SELECT * FROM read_sharepoint_excel(
    'https://contoso.sharepoint.com/sites/HR/Documents/Employees.xlsx',
    sheet := 'Current',
    header := true,
    all_varchar := false,
    ignore_errors := true
);

-- Example: Aggregate data from multiple Excel files
SELECT
    'Q1' AS quarter,
    SUM(Amount) AS total_amount,
    COUNT(*) AS transaction_count
FROM read_sharepoint_excel('https://contoso.sharepoint.com/.../Q1_2024.xlsx')
UNION ALL
SELECT
    'Q2' AS quarter,
    SUM(Amount) AS total_amount,
    COUNT(*) AS transaction_count
FROM read_sharepoint_excel('https://contoso.sharepoint.com/.../Q2_2024.xlsx');

-- Example: Using the scalar function directly with read_xlsx for extra options
-- (e.g. range, stop_at_empty, empty_as_varchar not exposed via the macro)
SELECT * FROM read_xlsx(
    (SELECT sharepoint_download_excel(
        'https://contoso.sharepoint.com/sites/Finance/Documents/Report.xlsx'
    )),
    sheet := 'Summary',
    range := 'A1:F100'
);
