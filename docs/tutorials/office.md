# Office: spreadsheets, documents, slides and PDFs

`office` covers the everyday office work: budgets and lists in spreadsheets, letters and reports, slide decks, and PDF chores. It has four parts:

- **Spreadsheets** (`office.sheet_*`): a formula engine with the common Excel functions (SUM, IF, VLOOKUP, XLOOKUP, SUMIFS, TEXT, dates and more), number formats, sorting, filtering and fill, charts as PNG. It reads and writes `.xlsx` and `.csv`, reads `.ods`, and prints to PDF.
- **Documents** (`office.doc_*`): headings, styled paragraphs, lists, tables, images, quotes, code and page breaks. It reads and writes `.docx`, Markdown and HTML, reads `.odt`, and exports PDF through its own layout engine.
- **Slide decks** (`office.deck_*`): build slides from a Markdown outline or one by one, pick from eight colour themes, add speaker notes, and save `.pptx` (PowerPoint, Keynote, Google Slides and LibreOffice open it), PDF and PNG.
- **PDF tools** (`office.pdf_*`): info, text extraction, merge, split, extract, delete or rotate pages, and images to PDF. Any PDF can also be rendered to PNG (this needs `pdftoppm` from poppler-utils). To view, mark up, edit, redact, fill in or sign a PDF, use the [PDF editor](pdf.md) that ships in the same app.

All of the formats above are handled inside the suite, with no other programs. `office.convert` converts between them in one step. When LibreOffice is installed it adds the rest: `.doc`, `.xls`, `.ppt`, `.rtf`, OpenDocument output (`.odt`, `.ods`, `.odp`) and more.

There is no GUI yet; the GUI that comes later will call these same commands. Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands that read files and write files.
- **Batch**: a script for multi-step work with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools office` serves just the office tools.

Every example below was run against the real binary, and the outputs are copied from those runs. Where an output was long it is shortened, and the cut is marked.

## How office works

**Documents and ids.** `office.sheet_new` and `office.sheet_open` open a workbook with an id such as `sheet1`. `office.doc_*` documents get ids like `doc1`, and decks get ids like `deck1`. The other commands take `doc=<id>`. When only one document of that kind is open you can leave `doc` out. Every change is one undo step, and the session's `undo`, `redo` and `history` commands work as everywhere else in the suite. `*_save` writes the document; the extension of `path` picks the format. `*_export` writes another format without changing which file the document belongs to.

**One-shot commands with `file=`.** The commands that only read take `file=` instead of `doc=`: they load the file, answer, and forget it. This works for info, get, find, calc, preview, chart, word count and export. It is how the CLI examples work without a batch script:

```sh
wetwaresuite office.sheet_calc file=orders.csv formula="SUMIF(D:D,\"shipped\",C:C)"
wetwaresuite office.doc_word_count file=report.docx
wetwaresuite office.deck_export file=talk.md path=talk.png
```

**Several lines in one argument.** CSV text, bullets, list items and table rows have one item per line. A command line cannot hold a real line break, so write `\n` and it becomes one. Inside a batch file write `\\n`, because the batch reader removes one level of backslashes, and write `\"` for a quote inside a quoted value. Over MCP, Claude sends real line breaks. Every such parameter also has a `_file` twin (`csv_file=`, `text_file=`, `bullets_file=`, `markdown_file=`, `outline_file=`) that reads the text from a file instead.

**Previews.** Each kind has a preview command (`office.sheet_preview`, `office.doc_preview`, `office.deck_preview`) that returns a PNG drawn exactly as the PDF export draws it. Most editing commands also take `preview=true`, which attaches a picture of the result. Claude uses these to check its own work.

**In the window.** `wetwaresuite-office` (or `wetwaresuite-gui`) opens spreadsheets as an editable grid: click a cell, type a value or `=formula`, press Enter. Each cell you type is an `office.sheet_set` (or `office.sheet_clear`) step, so it undoes the same way as a script. See [the window tutorial](gui.md#spreadsheets-type-into-the-cells).

**Built in or LibreOffice.** `wetwaresuite office.formats` shows what is built in, and whether LibreOffice and pdftoppm were found:

```sh
$ wetwaresuite office.formats
spreadsheets (office.sheet_*): read csv ods tab tsv xlsm xlsx; write csv html pdf tsv xlsx
  through LibreOffice: read dbf fods numbers sxc xls xlsb xlt xltx; write fods ods xls
documents (office.doc_*): read docx htm html markdown md odt txt; write docx htm html markdown md pdf txt
  through LibreOffice: read doc docm dot dotx fodt lwp pages rtf sxw wpd wps; write doc epub fodt odt rtf
slide decks (office.deck_*): read markdown md pptx; write markdown md pdf pptx png
  through LibreOffice: read fodp key odp potx pps ppsx ppt pptm sxi; write fodp odp ppt
pdf (office.pdf_*): info, text, merge, split, extract, delete pages, rotate, images to pdf (built in); render pages to png (pdftoppm)
LibreOffice: installed; pdftoppm: installed
libreoffice: true
pdftoppm: true
```

The LibreOffice formats work in every open, save and export command, not only in `office.convert`. The suite runs LibreOffice in the background with its own profile, so it doesn't disturb a LibreOffice window you have open. Install it with your package manager (`libreoffice` on Linux, `brew install --cask libreoffice` on macOS, or the installer from libreoffice.org on Windows). Install `poppler-utils` (or `brew install poppler`) for `office.pdf_render`. Everything else works without either program.

## Task 1: a budget spreadsheet with formulas and a chart

You want a quarterly budget with monthly spending per category, totals across and down, a column that flags categories over plan, currency formatting, a bar chart and a pie chart, saved as `.xlsx` and printed to PDF.

**CLI.** A CSV file can hold formulas: any cell that starts with `=` is a formula when the suite reads it. Write the numbers and formulas in `budget.csv`:

```text
Category,Jan,Feb,Mar,Total
Rent,1200,1200,1200,=SUM(B2:D2)
Groceries,420,385,450,=SUM(B3:D3)
Transport,95,110,80,=SUM(B4:D4)
Fun,150,90,200,=SUM(B5:D5)
Total,=SUM(B2:B5),=SUM(C2:C5),=SUM(D2:D5),=SUM(E2:E5)
```

Then convert it, look at the results, chart it, and ask a question without changing anything:

```sh
$ wetwaresuite office.convert input=budget.csv output=budget.xlsx
converted budget.csv to budget.xlsx (built-in, 1 sheet(s), 5 KB)
budget.xlsx
output: budget.xlsx
backend: built-in
bytes: 5469

$ wetwaresuite office.sheet_get file=budget.xlsx
A1:E6 (6 rows x 5 cols)
	A	B	C	D	E
1	Category	Jan	Feb	Mar	Total
2	Rent	1200	1200	1200	3600
3	Groceries	420	385	450	1255
4	Transport	95	110	80	285
5	Fun	150	90	200	440
6	Total	1865	1785	1930	5580
range: A1:E6
rows: 6
cols: 5

$ wetwaresuite office.sheet_chart file=budget.xlsx range=A1:D5 title="Spending by month" path=budget-chart.png
bar chart of A1:D5: 3 series x 4 categories (Jan, Feb, Mar); wrote budget-chart.png
budget-chart.png
path: budget-chart.png
series: 3
categories: 4
attachment: chart.png (29436 bytes)

$ wetwaresuite office.sheet_calc file=budget.xlsx formula="AVERAGE(E2:E5)"
=AVERAGE(E2:E5) -> 1395
1395
value: 1395
type: number
```

The `.xlsx` keeps the formulas, and their computed values are stored alongside them, so Excel, LibreOffice and Google Sheets show the numbers straight away. `sheet_chart` reads the usual layout by itself: the header row names the series (Jan, Feb, Mar) and the first column names the categories. Add `by_rows=true` to swap the two.

**Batch.** A script builds the same budget step by step and adds formatting:

```text
# budget.txt - a quarterly budget with totals, a check column and a chart
office.sheet_new name=Budget -> b

# the numbers: one row per line (\\n in a batch file is a line break)
office.sheet_set doc=$b range=A1 csv="Category,Jan,Feb,Mar\\nRent,1200,1200,1200\\nGroceries,420,385,450\\nTransport,95,110,80\\nFun,150,90,200"

# totals across and down; the formula is copied with relative refs
office.sheet_set doc=$b range=E1 value=Total
office.sheet_set doc=$b range=E2:E5 value="=SUM(B2:D2)"
office.sheet_set doc=$b range=F1 value="vs plan"
office.sheet_set doc=$b range=F2:F5 value="=IF(E2>1500,\"over\",\"ok\")"
office.sheet_set doc=$b range=A6 value=Total
office.sheet_set doc=$b range=B6:E6 value="=SUM(B2:B5)"
office.sheet_set doc=$b range=A7 value="Monthly average"
office.sheet_set doc=$b range=B7 value="=ROUND(AVERAGE(B6:D6),2)"

# looks
office.sheet_format doc=$b range=B2:E7 format=currency
office.sheet_format doc=$b range=A1:F1 bold=true
office.sheet_format doc=$b range=A6:E6 bold=true
office.sheet_format doc=$b range=A:F autofit=true

office.sheet_get doc=$b show=both
office.sheet_chart doc=$b range=A1:D5 type=bar title="Spending by month" path=budget-chart.png
office.sheet_chart doc=$b range=A1:E5 series=Total type=pie title="Where Q1 went" path=budget-pie.png
office.sheet_save doc=$b path=budget-q1.xlsx
office.sheet_save doc=$b path=budget-q1.pdf
```

```sh
$ wetwaresuite batch budget.txt
created sheet1: 1 sheet: Budget (empty)
...
set A1:D5 (20 cells); A1 = Category
...
set E2:E5 (4 cells); E2 = 3600
...
set F2:F5 (4 cells); F2 = over
...
set B7; B7 = 1860
...
formatted B2:E7: format $#,##0.00
formatted A1:F1: bold
formatted A6:E6: bold
formatted A:F: autofit widths
A1:F7 (7 rows x 6 cols)
	A	B	C	D	E	F
1	Category	Jan	Feb	Mar	Total	vs plan
2	Rent	$1,200.00	$1,200.00	$1,200.00	$3,600.00 [=SUM(B2:D2)]	over [=IF(E2>1500,"over","ok")]
3	Groceries	$420.00	$385.00	$450.00	$1,255.00 [=SUM(B3:D3)]	ok [=IF(E3>1500,"over","ok")]
4	Transport	$95.00	$110.00	$80.00	$285.00 [=SUM(B4:D4)]	ok [=IF(E4>1500,"over","ok")]
5	Fun	$150.00	$90.00	$200.00	$440.00 [=SUM(B5:D5)]	ok [=IF(E5>1500,"over","ok")]
6	Total	$1,865.00 [=SUM(B2:B5)]	$1,785.00 [=SUM(C2:C5)]	$1,930.00 [=SUM(D2:D5)]	$5,580.00 [=SUM(E2:E5)]	
7	Monthly average	$1,860.00 [=ROUND(AVERAGE(B6:D6),2)]				
...
bar chart of A1:D5: 3 series x 4 categories (Jan, Feb, Mar); wrote budget-chart.png
...
pie chart of A1:E5: 1 series x 4 categories (Total); wrote budget-pie.png
...
saved budget-q1.xlsx
...
saved budget-q1.pdf
```

(Shortened: each command also prints its `key: value` fields.) Some things to notice:

- `value="=SUM(B2:D2)"` over `E2:E5` works like dragging a formula down in Excel: E3 gets `=SUM(B3:D3)`, and so on. Put `$` in a reference (`$B$2`) to keep it fixed.
- `csv=` pastes a block starting at the top-left cell of `range`. Numbers, `$1,200`, `15%`, `2026-03-01` dates and `TRUE` become typed values. Use `value=` for formulas you want copied across a range.
- `show=both` puts each formula in brackets after its value. `show=formulas` lists what you would type to rebuild the sheet.
- `series=Total` picks one column for the pie chart. The pie's legend shows each share as a percentage.
- The PDF is a printable grid with the number formats and bold applied. Wide sheets continue on more A4 landscape pages.

To change a number later, open the file, set the cell and save. Every formula that depends on it is recalculated:

```text
# change.txt
office.sheet_open path=budget-q1.xlsx -> b
office.sheet_set doc=$b range=C3 value=410
office.sheet_get doc=$b range=E3
office.sheet_save doc=$b
```

```sh
$ wetwaresuite batch change.txt
opened sheet1: 1 sheet: Budget A1:F7 - budget-q1.xlsx; 13 formulas
...
set C3; C3 = $410.00
...
E3 = $1,280.00  (formula =SUM(B3:D3))
type number, format $#,##0.00
reads B3:D3
used by F3, E6
...
saved budget-q1.xlsx
```

`office.sheet_get` on one cell also lists the cells it reads from and the cells that read it, which helps when you're tracing a wrong total.

**Claude.**

> Make me a Q1 budget spreadsheet in /home/me/money/budget-q1.xlsx: rent 1200 every month, groceries 420/385/450, transport 95/110/80, fun 150/90/200. Totals per category and per month, flag anything over 1500 for the quarter, currency format, and a bar chart of spending by month next to it as a PNG.

Claude calls:

1. `office_sheet_new`, then `office_sheet_set` with `csv` for the numbers.
2. `office_sheet_set` with `value="=SUM(B2:D2)"` over `E2:E5`, and the same for the totals row and the `IF` column.
3. `office_sheet_format` with `format=currency`, `bold` and `autofit`.
4. `office_sheet_get` with `show=both` to check the values, or `office_sheet_preview` to look at the sheet as a picture.
5. `office_sheet_chart`, which returns the chart image so Claude sees what it made.
6. `office_sheet_save`.

If Claude isn't sure a formula is right, `office_sheet_calc` with `formula=` evaluates it without writing anything.

## Task 2: clean up a CSV export and join it with VLOOKUP

An online shop exported `orders.csv`. The customer ids have stray spaces and mixed case, the status column is spelled three ways, and the amounts are text with dollar signs:

```text
Order,Customer,Amount,Status
1001, c-002 ,"$1,250.00",shipped
1002,C-001,$310.50,SHIPPED
1003,c-004,"$2,400.00",pending
1004,C-003,$95.00,Shipped
1005,c-002,$780.25,cancelled
1006,C-005,"$1,020.00",shipped
1007, C-001,$45.99,pending
1008,c-003,$660.00,shipped
```

Customer names and regions are in another file, `customers.csv`:

```text
ID,Name,Region
C-001,Acme Ltd,North
C-002,Birch & Co,South
C-003,Cobalt Inc,North
C-004,Delta GmbH,East
```

You want a clean table with a proper id, the customer's name and region, one spelling of the status, without the cancelled orders and with the biggest orders first, plus revenue per region.

**CLI.** Look at the data first and ask questions straight from the CSV:

```sh
$ wetwaresuite office.sheet_info file=orders.csv
orders.csv: 1 sheet(s)
  1. orders (active): A1:D9 (9 rows x 4 cols), 36 cells, 0 formulas
     row 1: Order | Customer | Amount | Status
doc: orders.csv
sheets: 1
active: orders
formulas: 0
errors: 0
path: orders.csv

$ wetwaresuite office.sheet_calc file=orders.csv formula="SUMIF(D:D,\"shipped\",C:C)"
=SUMIF(D:D,"shipped",C:C) -> 3335.5
3335.5
value: 3335.5
type: number

$ wetwaresuite office.sheet_find file=orders.csv query=c-002
2 cell(s) match "c-002":
  orders!B2:  c-002 
  orders!B6: c-002
2
count: 2
first: orders!B2

$ wetwaresuite office.convert input=orders.csv output=orders.xlsx
converted orders.csv to orders.xlsx (built-in, 1 sheet(s), 6 KB)
...
```

The amounts were read as numbers (`"$1,250.00"` becomes 1250), and criteria such as `"shipped"` ignore case like Excel's do, so the total covers `SHIPPED` and `Shipped` too. The delimiter is detected, so semicolon-separated files from European Excel work as well (`delimiter=` overrides it).

**Batch.** The cleanup needs several steps, so it goes in a script:

```text
# cleanup.txt - tidy an orders export and join customer names onto it
office.sheet_open path=orders.csv -> o
office.sheet_add doc=$o name=Customers csv_file=customers.csv

# 1. a clean customer id next to the messy one: trimmed and upper-case
office.sheet_insert_cols doc=$o sheet=orders at=C
office.sheet_set doc=$o range=orders!C1 value=ID
office.sheet_set doc=$o range=orders!C2:C9 value="=UPPER(TRIM(B2))"

# 2. the join: VLOOKUP the name, XLOOKUP the region (with a default for unknown ids)
office.sheet_set doc=$o range=orders!F1 value=Name
office.sheet_set doc=$o range=orders!F2:F9 value="=IFERROR(VLOOKUP(C2,Customers!A:C,2,FALSE),\"unknown\")"
office.sheet_set doc=$o range=orders!G1 value=Region
office.sheet_set doc=$o range=orders!G2:G9 value="=XLOOKUP(C2,Customers!A:A,Customers!C:C,\"?\")"

# 3. one spelling for the status column
office.sheet_set doc=$o range=orders!H1 value=State
office.sheet_set doc=$o range=orders!H2:H9 value="=LOWER(E2)"

office.sheet_format doc=$o range=orders!A1:H1 bold=true
office.sheet_format doc=$o range=orders!A:H autofit=true
office.sheet_get doc=$o sheet=orders

# 4. drop the cancelled orders, biggest first
office.sheet_filter doc=$o sheet=orders column=State criteria="<>cancelled" mode=delete
office.sheet_sort doc=$o sheet=orders by="Amount desc"

# 5. totals per region on their own sheet
office.sheet_add doc=$o name=Summary csv="Region,Orders,Revenue\\nNorth\\nSouth\\nEast"
office.sheet_set doc=$o range=Summary!B2:B4 value="=COUNTIF(orders!G:G,A2)"
office.sheet_set doc=$o range=Summary!C2:C4 value="=SUMIF(orders!G:G,A2,orders!D:D)"
office.sheet_format doc=$o range=Summary!C2:C4 format=currency
office.sheet_get doc=$o sheet=Summary
office.sheet_find doc=$o query=unknown

office.sheet_save doc=$o path=orders-clean.xlsx
office.sheet_save doc=$o path=orders-clean.csv sheet=orders
```

```sh
$ wetwaresuite batch cleanup.txt
opened sheet1: 1 sheet: orders A1:D9 - orders.csv
...
added sheet Customers at position 2
...
inserted column C in orders; formulas adjusted
...
set orders!C2:C9 (8 cells); C2 = C-002
...
set orders!F2:F9 (8 cells); F2 = Birch & Co
...
orders!A1:H9 (9 rows x 8 cols)
	A	B	C	D	E	F	G	H
1	Order	Customer	ID	Amount	Status	Name	Region	State
2	1001	 c-002 	C-002	$1,250.00	shipped	Birch & Co	South	shipped
3	1002	C-001	C-001	$310.50	SHIPPED	Acme Ltd	North	shipped
4	1003	c-004	C-004	$2,400.00	pending	Delta GmbH	East	pending
5	1004	C-003	C-003	$95.00	Shipped	Cobalt Inc	North	shipped
6	1005	c-002	C-002	$780.25	cancelled	Birch & Co	South	cancelled
7	1006	C-005	C-005	$1,020.00	shipped	unknown	?	shipped
8	1007	 C-001	C-001	$45.99	pending	Acme Ltd	North	pending
9	1008	c-003	C-003	$660.00	shipped	Cobalt Inc	North	shipped
...
7 of 8 rows match H <>cancelled; deleted the other 1 rows
...
sorted 7 rows of orders!A1:H8 by D desc (header kept)
...
Summary!A1:C4 (4 rows x 3 cols)
	A	B	C
1	Region	Orders	Revenue
2	North	4	$1,111.49
3	South	1	$1,250.00
4	East	1	$2,400.00
...
1 cell(s) match "unknown":
  orders!F4: unknown  (=IFERROR(VLOOKUP(C4,Customers!A:C,2,FALSE),"unknown"))
...
saved orders-clean.xlsx
...
saved orders-clean.csv (sheet orders only; CSV holds one sheet) (formulas saved as their values)
```

Some things to notice:

- `csv_file=` reads a whole file into a new sheet, so the two exports end up in one workbook and formulas can refer across them (`Customers!A:C`).
- The amounts kept their `$1,250.00` look: text read as money gets a currency format along with its number.
- `office.sheet_filter` with `mode=delete` removes the rows that don't match, in place. The default `mode=copy` writes the matching rows to a new sheet instead, and `mode=count` only counts. Criteria work like COUNTIF: `>100`, `<>done`, `a*`.
- `office.sheet_sort` takes header names or column letters (`by="Region, Amount desc"`). The header row stays on top, and formulas move with their rows.
- Customer `C-005` isn't in the customer list. `IFERROR` (or XLOOKUP's fourth argument) turns the `#N/A` into a readable value, and `office.sheet_find` then finds the row (row 4 after the sort).
- A CSV holds one sheet and no formulas, so the CSV save writes the `orders` sheet's computed values. The `.xlsx` keeps all three sheets with their formulas.

**Claude.**

> /home/me/shop/orders.csv is a messy export: customer ids have spaces and mixed case, statuses are spelled differently. Join on the customer names and regions from /home/me/shop/customers.csv, drop cancelled orders, sort biggest first, and add a sheet with order count and revenue per region. Tell me which orders have a customer id that isn't in the list. Save it as orders-clean.xlsx next to the input.

Claude calls:

1. `office_sheet_open` and `office_sheet_get`, to see the columns and the mess.
2. `office_sheet_add` with `csv_file` for the customer list.
3. `office_sheet_insert_cols`, then `office_sheet_set` with `=UPPER(TRIM(B2))` down the new column.
4. `office_sheet_set` with the `VLOOKUP`/`XLOOKUP` formulas, and `office_sheet_find` with `query=unknown` to answer your question.
5. `office_sheet_filter` with `mode=delete`, then `office_sheet_sort`.
6. `office_sheet_add` for the summary, with `COUNTIF`/`SUMIF` formulas.
7. `office_sheet_save`.

## Task 3: a report document exported to PDF

You want a short report with headings, a bold total, a bulleted list, a table, the chart from task 1, a quote, and a second page with numbered next steps. You want it as a PDF to send and a `.docx` for colleagues who use Word.

**CLI.** The quickest way is to write it in Markdown, `q1-budget.md`:

```markdown
---
title: Q1 household budget
author: Sam Rivera
---

# Q1 household budget

Spending came to **$5,580** over the quarter, about *$1,860 a month*. Rent is
fixed; groceries and fun moved the most. The raw numbers are in
[budget-q1.xlsx](budget-q1.xlsx).

## Highlights

- Rent stayed at $1,200 a month
- Groceries peaked in March at $450
- Fun spending more than doubled from February to March
    - one concert, one weekend away

## By category

| Category  | Q1 total | Share |
|-----------|---------:|------:|
| Rent      |   $3,600 |   65% |
| Groceries |   $1,255 |   22% |
| Transport |     $285 |    5% |
| Fun       |     $440 |    8% |

![Spending by month](budget-chart.png)

> Next quarter: cap fun at $150 a month and plan groceries weekly.

<!-- pagebreak -->

## Next steps

1. Review the grocery list every Sunday
2. Move $200 a month into savings
3. Check the numbers again in July
```

Then convert it:

```sh
$ wetwaresuite office.convert input=q1-budget.md output=q1-budget.pdf
converted q1-budget.md to q1-budget.pdf (built-in, 11 blocks, 27 KB)
q1-budget.pdf
output: q1-budget.pdf
backend: built-in
bytes: 27937

$ wetwaresuite office.convert input=q1-budget.md output=q1-budget.docx
converted q1-budget.md to q1-budget.docx (built-in, 11 blocks, 46 KB)
...

$ wetwaresuite office.doc_word_count file=q1-budget.md
106 words, 582 characters (504 without spaces), 14 paragraphs, 2 page(s); about 1 min to read
106
words: 106
characters: 582
characters_no_spaces: 504
paragraphs: 14
pages: 2

$ wetwaresuite office.doc_export file=q1-budget.md path=q1-budget-page1.png dpi=80
exported page 1 of 2 to q1-budget-page1.png
q1-budget-page1.png
path: q1-budget-page1.png

$ wetwaresuite office.convert input=q1-budget.docx output=q1-budget.odt
converted q1-budget.docx to q1-budget.odt (built-in + LibreOffice, 11 blocks, 51 KB)
...
```

The PDF is laid out by the suite itself. It uses the standard PDF fonts (Helvetica, Times and Courier), which every PDF reader has, so the files stay small. It has wrapped paragraphs, real bold and italic, clickable links, bullets with sub-levels, a table with a shaded header row, the image scaled to the text width, a page break and "1 / 2" page numbers. The front matter becomes the PDF's title and author. `.odt` isn't written by the suite, so that conversion goes through LibreOffice ("built-in + LibreOffice": read here, written by LibreOffice).

**Batch.** To build the document piece by piece, or to edit an existing `.docx`, use the block commands. A document is a list of numbered blocks; `office.doc_info` lists them, and `doc_edit`, `doc_delete`, `doc_move` and `doc_add at=` use those numbers:

```text
# writeup.txt - build a two-page write-up block by block, then export it
office.doc_new title="Q1 household budget" author="Sam Rivera" font=serif -> d

office.doc_add doc=$d kind=heading text="Q1 household budget"
office.doc_add doc=$d text="Spending came to **$5,580** over the quarter, about *$1,860 a month*."
office.doc_add doc=$d kind=heading level=2 text=Highlights
office.doc_add doc=$d kind=list text="Rent stayed at $1,200 a month\\nGroceries peaked in March\\nFun spending doubled\\n  one concert, one weekend away"
office.doc_add doc=$d kind=heading level=2 text="By category"
office.doc_add doc=$d kind=table text="Category | Q1 total\\nRent | $3,600\\nGroceries | $1,255\\nTransport | $285\\nFun | $440"
office.doc_add doc=$d kind=image path=budget-chart.png width=400 alt="Spending by month"
office.doc_add doc=$d kind=pagebreak
office.doc_add doc=$d kind=heading level=2 text="Next steps"
office.doc_add doc=$d kind=numbered text="Review the grocery list every Sunday\\nMove $200 a month into savings"

# fix things after the fact
office.doc_info doc=$d
office.doc_edit doc=$d block=2 text="Spending came to **$5,580** over the quarter, about *$1,860 a month*. Rent is fixed."
office.doc_add doc=$d kind=quote at=8 text="Next quarter: cap fun at $150 a month."
office.doc_find_replace doc=$d find="Fun spending doubled" replace="Fun spending more than doubled"
office.doc_word_count doc=$d

office.doc_setup doc=$d page=letter margin=20
office.doc_export doc=$d path=writeup.pdf
office.doc_save doc=$d path=writeup.docx
office.doc_export doc=$d path=writeup.html
office.doc_export doc=$d path=writeup-p1.png page=1 dpi=80
```

```sh
$ wetwaresuite batch writeup.txt
created doc1: "Q1 household budget", 0 blocks
...
added 4. list (4 items): Rent stayed at $1,200 a month / Groceries peaked in March / Fun spending doubled / one con...
...
added 6. table (5x2): Category | Q1 total / Rent | $3,600 / Groceries | $1,255 / Transport | $285 / Fun | $440
...
added 7. image (800x500 px): Spending by month
...
doc1: "Q1 household budget" by Sam Rivera
a4, margins 25.4 mm, serif 11 pt; 2 page(s), 60 words, 10 blocks:
  1. heading 1: Q1 household budget
  2. paragraph: Spending came to $5,580 over the quarter, about $1,860 a month.
  3. heading 2: Highlights
  4. list (4 items): Rent stayed at $1,200 a month / Groceries peaked in March / Fun spending doubled / one con...
  5. heading 2: By category
  6. table (5x2): Category | Q1 total / Rent | $3,600 / Groceries | $1,255 / Transport | $285 / Fun | $440
  7. image (800x500 px): Spending by month
  8. page_break
  9. heading 2: Next steps
  10. numbered list (2 items): Review the grocery list every Sunday / Move $200 a month into savings
...
block 2. paragraph: Spending came to $5,580 over the quarter, about $1,860 a month. Rent is fixed.
added 8. quote: Next quarter: cap fun at $150 a month.
...
replaced 1 match(es) of "Fun spending doubled" with "Fun spending more than doubled" in block(s) 4
...
73 words, 406 characters (355 without spaces), 13 paragraphs, 2 page(s); about 1 min to read
...
page setup: 612 x 792 pt, margins 20 mm, serif 11 pt; 2 page(s)
exported writeup.pdf (2 pages)
...
saved writeup.docx
...
exported writeup.html
...
exported page 1 of 2 to writeup-p1.png
```

Some things to notice:

- Paragraphs, headings and quotes take inline Markdown: `**bold**`, `*italic*`, `<u>underline</u>`, `` `code` `` and `[links](https://...)`. Lists take one item per line, and two spaces of indent make a sub-item. Table cells are split by `|`, tabs or commas, and the first row is the header.
- `kind=markdown` adds a whole Markdown text as blocks. That is handy for pasting a section in the middle of a `.docx`.
- `doc_find_replace` keeps each run's style, and it searches lists and table cells too.
- `.html` is one self-contained file with the images inside it. A `.md` save puts its images in a `<name>_media/` folder next to it.
- To edit a `.docx` someone sent you, `office.doc_open` it, change blocks, and `office.doc_save` it. Headings, bold/italic/underline, links, lists, tables, images and page breaks survive the round trip; the limits section lists what doesn't.

**Claude.**

> Write a one-page summary of my Q1 budget as a PDF at /home/me/money/q1-report.pdf: a heading, two sentences with the total in bold, highlights as bullets, a table of the category totals, and the chart /home/me/money/budget-chart.png. Put the next steps on a second page. Also save a .docx copy.

Claude usually writes the whole thing as Markdown in one `office_doc_new` call (`markdown=` with real line breaks). It then calls `office_doc_preview` to look at page 1 and `office_doc_info` to check the page count. It fixes things with `office_doc_edit` or `office_doc_setup` (for example `margin=20` when the content just spills onto another page), and finishes with `office_doc_export` to `.pdf` and `office_doc_save` to `.docx`. To revise an existing file it calls `office_doc_open`, `office_doc_get_text` (Markdown with block numbers) and then the block commands.

## Task 4: a 5-slide deck from bullet points

You have notes for a short talk and want slides: a title slide, two content slides (one with two columns), a numbered plan and a closing slide, in a teal theme with speaker notes. You want a `.pptx` to present from and a PDF to hand out.

**CLI.** Write the outline in Markdown, `talk.md`. Each `#` heading starts a slide:

```markdown
---
title: Moving to the new build system
author: Platform team
theme: teal
---

# Moving to the new build system
Platform team, October 2026

# Why change
- Clean builds take 14 minutes
- Nobody can explain the old scripts
  - three generations of Makefiles
- CI cost doubled this year
> Start with the CI bill, it gets attention.

<!-- layout: two_column -->
# Old vs new
- 14 min clean build
- Hand-written rules
<!-- column -->
- 3 min clean build
- One CMake file per target

# Plan
1. Port the core libraries in November
2. Port the tools in December
3. Delete the Makefiles in January

# Questions?
build-team@example.com
```

```sh
$ wetwaresuite office.deck_info file=talk.md
talk.md: "Moving to the new build system", 5 slide(s), theme teal, 16:9
  1. title: "Moving to the new build system" / Platform team, October 2026
  2. content: "Why change", 4 bullet(s), notes
  3. two_column: "Old vs new", 2 bullet(s) + 2 right
  4. content: "Plan", 3 bullet(s)
  5. section: "Questions?" / build-team@example.com
doc: talk.md
slides: 5
theme: teal

$ wetwaresuite office.convert input=talk.md output=talk.pptx
converted talk.md to talk.pptx (built-in, 5 slides, 41 KB)
...

$ wetwaresuite office.convert input=talk.md output=talk.pdf
converted talk.md to talk.pdf (built-in, 5 slides, 4 KB)
...

$ wetwaresuite office.deck_export file=talk.md path=talk.png width=640
exported 5 slides as talk-1.png ... talk-5.png
talk-1.png
files: 5
```

The layouts were worked out from the content. A first slide without bullets became the title slide, a closing slide with only a title and a line became a section slide, and the rest are content slides. The `1.` items stay numbered. The `>` line is the speaker notes, which appear in PowerPoint's notes pane. Title and section slides use the theme colour as background; content slides have the title in the theme colour over a short accent bar, and a slide number. When a slide has too many bullets the text shrinks to fit, and `office.deck_info` warns about slides where it still can't.

The outline syntax:

| Line | Meaning |
| --- | --- |
| front matter `title:`, `author:`, `theme:`, `font:`, `aspect:` | deck settings (`aspect: 4:3` for old projectors) |
| `# Heading` | starts a slide with that title (`---` on its own line also starts one) |
| `- item`, `* item`, `1. item` | bullets; indent two spaces per level, up to five levels |
| `> text` or `Notes: text` | speaker notes |
| `![](picture.png)` | the slide's picture, relative to the outline file |
| other lines | the subtitle |
| `<!-- layout: name -->` | picks the next slide's layout: `title`, `section`, `content`, `two_column`, `image`, `image_right`, `title_only`, `blank` |
| `<!-- column -->` | starts the right column of a `two_column` slide |

Markdown is read as a slide outline when it has deck markers (`theme:` or `aspect:` in the front matter, or layout comments), or when the output is a deck format such as `.pptx`. For anything else, `office.convert kind=deck` or `kind=doc` decides.

**Batch.** To build the deck slide by slide, or to change an existing `.pptx`:

```text
# talk.txt - a five-slide deck built slide by slide
office.deck_new title="Moving to the new build system" theme=teal -> t

office.deck_add_slide doc=$t layout=title title="Moving to the new build system" subtitle="Platform team, October 2026"
office.deck_add_slide doc=$t title="Why change" bullets="Clean builds take 14 minutes\\nNobody can explain the old scripts\\n  three generations of Makefiles\\nCI cost doubled this year" notes="Start with the CI bill."
office.deck_add_slide doc=$t layout=two_column title="Old vs new" bullets="14 min clean build\\nHand-written rules" right="3 min clean build\\nOne CMake file per target"
office.deck_add_slide doc=$t title="Plan" bullets="1. Port the core libraries in November\\n2. Port the tools in December\\n3. Delete the Makefiles in January"
office.deck_add_slide doc=$t layout=section title="Questions?" subtitle="build-team@example.com"

# a picture next to the bullets, then a different colour
office.deck_edit_slide doc=$t slide=2 image=budget-chart.png
office.deck_theme doc=$t theme=#5b3cc4
office.deck_info doc=$t

office.deck_save doc=$t path=talk-built.pptx
office.deck_export doc=$t path=talk-built.pdf
office.deck_export doc=$t path=talk-built-2.png slide=2 width=960
office.deck_export doc=$t path=talk-built.md
```

```sh
$ wetwaresuite batch talk.txt
created deck1: "Moving to the new build system", 0 slides, theme teal
...
added slide 1. title: "Moving to the new build system" / Platform team, October 2026
...
added slide 2. content: "Why change", 4 bullet(s), notes
...
added slide 3. two_column: "Old vs new", 2 bullet(s) + 2 right
...
added slide 4. content: "Plan", 3 bullet(s)
...
added slide 5. section: "Questions?" / build-team@example.com
...
slide 2. content: "Why change", 4 bullet(s), image 800x500, notes
theme #5b3cc4
deck1: "Moving to the new build system", 5 slide(s), theme #5b3cc4, 16:9
  1. title: "Moving to the new build system" / Platform team, October 2026
  2. content: "Why change", 4 bullet(s), image 800x500, notes
  3. two_column: "Old vs new", 2 bullet(s) + 2 right
  4. content: "Plan", 3 bullet(s)
  5. section: "Questions?" / build-team@example.com
...
saved talk-built.pptx (5 slides)
...
exported talk-built.pdf (5 slides)
...
exported slide 2 to talk-built-2.png
...
exported talk-built.md (5 slides)
```

Some things to notice:

- A content slide with a picture puts the picture to the right of the bullets. `layout=image` makes the picture fill the slide under the title instead.
- The themes are `blue` (the default), `teal`, `green`, `orange`, `red`, `purple`, `slate` (light grey background) and `dark` (dark background). Any colour such as `#5b3cc4` works as the accent. `font=serif` or `font=mono` changes every slide's font.
- Exporting to `.md` writes the deck back as an outline, with its pictures in `talk-built_media/`. You can edit that file as text and convert it again.
- `office.deck_open` reads `.pptx` files from PowerPoint, Google Slides and LibreOffice: titles, subtitles, bullets with levels, the first picture on each slide, speaker notes and the theme colour. Then `deck_edit_slide`, `deck_move_slide` and `deck_delete_slide` change them. The deck is saved in the suite's own layouts, so a heavily designed corporate template comes back plainer (see the limits).

**Claude.**

> Turn these notes into a 5-slide deck at /home/me/talks/build.pptx, teal theme, with a PDF copy: title "Moving to the new build system", why change (14 min builds, nobody understands the scripts, CI cost doubled), old vs new side by side, a 3-step plan, and a questions slide. Put "start with the CI bill" in the speaker notes.

Claude calls:

1. `office_deck_new` with an `outline`. Writing the whole deck as one outline is the fastest way.
2. `office_deck_preview` with `all=true`, which returns every slide as an image so Claude can look for crowded slides.
3. `office_deck_info`, which also warns about text that doesn't fit.
4. `office_deck_edit_slide` to fix anything it doesn't like.
5. `office_deck_save` to `.pptx` and `office_deck_export` to `.pdf`.

## Task 5: merge and split PDFs

You want to combine the report and part of the slide deck into one handout, split it up again in different ways, reorder and rotate pages, read the text, and turn the two chart images into a PDF.

**CLI.** The PDF commands work on files directly; there is nothing to open.

```sh
$ wetwaresuite office.pdf_info path=talk.pdf
talk.pdf: 5 page(s), PDF 1.4
title: Moving to the new build system
author: Platform team
creator: wetwaresuite office
producer: wetwaresuite
  pages 1-5: 960 x 540 pt (16:9)
has extractable text
5
pages: 5
version: 1.4
encrypted: false
width: 960
height: 540

$ wetwaresuite office.pdf_merge inputs="q1-budget.pdf; talk.pdf[2-4]" output=pack.pdf
merged q1-budget.pdf + talk.pdf[2-4] -> pack.pdf (5 pages)
pack.pdf
output: pack.pdf
pages: 5

$ wetwaresuite office.pdf_split path=pack.pdf output=parts/pack-{n}.pdf
split pack.pdf into 5 files:
  parts/pack-1.pdf: pages 1
  parts/pack-2.pdf: pages 2
  parts/pack-3.pdf: pages 3
  parts/pack-4.pdf: pages 4
  parts/pack-5.pdf: pages 5
parts/pack-1.pdf
files: 5
first: parts/pack-1.pdf

$ wetwaresuite office.pdf_split path=pack.pdf ranges="1-2; 3-" output=parts/section.pdf
split pack.pdf into 2 files:
  parts/section-1.pdf: pages 1-2
  parts/section-2.pdf: pages 3-5
...

$ wetwaresuite office.pdf_extract path=talk.pdf pages="5,1" output=bookends.pdf
extracted pages 5,1 -> bookends.pdf (2 pages)
...

$ wetwaresuite office.pdf_rotate path=bookends.pdf pages=2 angle=90 output=rotated.pdf
rotated pages 2 by 90 degrees -> rotated.pdf (2 pages)
...

$ wetwaresuite office.pdf_delete_pages path=pack.pdf pages=last output=pack-short.pdf
deleted pages 5 -> pack-short.pdf (4 pages)
...

$ wetwaresuite office.pdf_text path=pack.pdf pages=3
--- page 3 ---
Why change

• Clean builds take 14 minutes
• Nobody can explain the old scripts
– three generations of Makefiles
• CI cost doubled this year

2
pages: 1

$ wetwaresuite office.pdf_from_images images="budget-chart.png, budget-pie.png" output=charts.pdf page=a4
made charts.pdf from 2 image(s)
...

$ wetwaresuite office.pdf_render path=lo/talk-built.pdf page=4 dpi=50 output=lo-page4.png
page 4 of 5 at 50 dpi (667x375); wrote lo-page4.png
output: lo-page4.png
attachment: page4.png (13574 bytes)

$ wetwaresuite office.convert input=pack.pdf output=pack.txt
converted pack.pdf to pack.txt (built-in, 5 pages of text, 1 KB)
...
```

Page selections are the same everywhere: `1-3,7`, `10-` (to the end), `last`, `5-1` (reversed), and repeats are allowed in `pdf_extract` and `pdf_merge`. In `pdf_merge`, each input can carry a selection in brackets (`report.pdf[2-5]`). The inputs don't have to come from the suite: PDFs from LibreOffice, browsers and scanners merge the same way (the `lo/talk-built.pdf` above was made by LibreOffice). Splitting with `{n}` in the name numbers the parts. Without `{n}`, the number goes before the extension (`section-1.pdf`). Leave `output` out of `pdf_rotate` and `pdf_delete_pages` to change the file in place.

**Batch.** PDF commands are not undoable (they write files), so a batch script is just a list of them:

```text
# handout.txt - the report plus three slides, then two pages per file for printing
office.deck_export file=talk.md path=talk.pdf
office.doc_export file=q1-budget.md path=q1-budget.pdf
office.pdf_merge inputs="q1-budget.pdf; talk.pdf[2-4]" output=handout.pdf
office.pdf_info path=handout.pdf
office.pdf_split path=handout.pdf every=2 output=print/sheet-{n}.pdf
```

```sh
$ wetwaresuite batch handout.txt
exported talk.pdf (5 slides)
...
exported q1-budget.pdf (2 pages)
...
merged q1-budget.pdf + talk.pdf[2-4] -> handout.pdf (5 pages)
...
handout.pdf: 5 page(s), PDF 1.7
producer: wetwaresuite
  pages 1-2: 595.3 x 841.9 pt (A4)
  pages 3-5: 960 x 540 pt (16:9)
has extractable text
...
split handout.pdf into 3 files:
  print/sheet-1.pdf: pages 1-2
  print/sheet-2.pdf: pages 3-4
  print/sheet-3.pdf: pages 5
...
```

The first two lines show that the one-shot exports work inside scripts as well.

**Claude.**

> In /home/me/handouts, merge report.pdf and pages 2-4 of slides.pdf into handout.pdf, then make a separate file of just the last page turned sideways. Show me page 3 of the handout.

Claude calls `office_pdf_info` on each input to check the page counts, then `office_pdf_merge`, then `office_pdf_extract` with `pages=last` and `office_pdf_rotate` with `angle=90`. To look at page 3 it calls `office_pdf_render` (when pdftoppm is installed) and gets the page as an image; without pdftoppm, `office_pdf_text` reads the page instead. When a PDF has no text (`pdf_info` says so), it is a scan, and Claude will say that the text can't be extracted.

## Task 6: all of it by asking Claude

Connect the server once, with just the office tools:

```sh
claude mcp add wetwaresuite-office -- /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp --tools office
```

(or add `"args": ["mcp", "--tools", "office"]` in Claude Desktop's config, see [mcp.md](mcp.md)). Claude then sees 63 `office_*` tools plus `undo`, `redo`, `history` and `documents`. A whole afternoon's work can be one request:

> In /home/me/q1:
> 1. Make budget.xlsx with rent 1200/1200/1200, groceries 420/385/450, transport 95/110/80 and fun 150/90/200 for Jan-Mar, with totals across and down in currency format and a bar chart of spending by month saved as budget-chart.png.
> 2. Clean up orders.csv: trimmed upper-case customer ids, customer names joined from customers.csv (mark unknown ids), saved as orders-clean.csv. Tell me which ids weren't found.
> 3. Write a report from q1-notes.md and save it as report.pdf, with the chart in it.
> 4. Make a 5-slide deck about the quarter in the teal theme, as talk.pptx and talk.pdf.
> 5. Merge report.pdf and talk.pdf into pack.pdf, and split pack.pdf into the report part and the slides part.
> 6. Convert talk.pptx to talk.odp for the people on LibreOffice.

This sequence was run against the server over JSON-RPC (the same messages Claude sends), using the files from tasks 1 to 5. Here are the tool calls, each with the first line of its answer:

```text
office_sheet_new        {"name": "Budget"}                                  created sheet1: 1 sheet: Budget (empty)
office_sheet_set        {"doc": "sheet1", "range": "A1", "csv": "Category,Jan,Feb,Mar\nRent,1200,..."}
                                                                            set A1:D5 (20 cells); A1 = Category
office_sheet_set        {"range": "E2:E5", "value": "=SUM(B2:D2)"}          set E2:E5 (4 cells); E2 = 3600
office_sheet_set        {"range": "B6:E6", "value": "=SUM(B2:B5)"}          set B6:E6 (4 cells); B6 = 1865
office_sheet_format     {"range": "B2:E6", "format": "currency"}            formatted B2:E6: format $#,##0.00
office_sheet_chart      {"range": "A1:D5", "title": "Spending by month", "path": ".../budget-chart.png"}
                                                                            bar chart of A1:D5: 3 series x 4 categories ... [1 image]
office_sheet_save       {"path": ".../budget.xlsx"}                         saved .../budget.xlsx
office_sheet_open       {"path": ".../orders.csv"}                          opened sheet2: 1 sheet: orders A1:D9
office_sheet_add        {"doc": "sheet2", "name": "Customers", "csv_file": ".../customers.csv"}
                                                                            added sheet Customers at position 2
office_sheet_insert_cols {"doc": "sheet2", "sheet": "orders", "at": "C"}    inserted column C in orders; formulas adjusted
office_sheet_set        {"range": "orders!C2:C9", "value": "=UPPER(TRIM(B2))"}
                                                                            set orders!C2:C9 (8 cells); C2 = C-002
office_sheet_set        {"range": "orders!F2:F9", "value": "=IFERROR(VLOOKUP(C2,Customers!A:C,2,FALSE),\"unknown\")"}
                                                                            set orders!F2:F9 (8 cells); F2 = Birch & Co
office_sheet_find       {"doc": "sheet2", "query": "unknown"}               1 cell(s) match "unknown": orders!F7 ...
office_sheet_save       {"path": ".../orders-clean.csv", "sheet": "orders"} saved .../orders-clean.csv (sheet orders only; ...)
office_doc_new          {"markdown_file": ".../q1-notes.md"}                created doc1: "Q1 household budget", 11 blocks
office_doc_preview      {"doc": "doc1", "page": 1}                          page 1 of 2 [1 image]
office_doc_export       {"path": ".../report.pdf"}                          exported .../report.pdf (2 pages)
office_deck_new         {"outline": "# Moving to ...\n...", "theme": "teal"} created deck1: "...", 5 slides, theme teal
office_deck_info        {"doc": "deck1"}                                    deck1: "...", 5 slide(s), theme teal, 16:9
office_deck_preview     {"doc": "deck1", "all": true}                       previews of 5 slides [5 images]
office_deck_save        {"path": ".../talk.pptx"}                           saved .../talk.pptx (5 slides)
office_deck_export      {"path": ".../talk.pdf"}                            exported .../talk.pdf (5 slides)
office_pdf_merge        {"inputs": ".../report.pdf;.../talk.pdf", "output": ".../pack.pdf"}
                                                                            merged report.pdf + talk.pdf -> .../pack.pdf (7 pages)
office_pdf_split        {"path": ".../pack.pdf", "ranges": "1-2;3-"}        split .../pack.pdf into 2 files: pack-1.pdf: pages 1-2, pack-2.pdf: pages 3-7
office_convert          {"input": ".../talk.pptx", "output": ".../talk.odp"}
                                                                            converted ... (built-in + LibreOffice, 5 slides, 36 KB)
```

(Shortened: `...` stands for the folder, and `doc` is left out where only one document of that kind was open.) Things that make this go well:

- **Give absolute paths**, or say which folder to work in. The server runs wherever Claude started it.
- **Let Claude look.** The preview commands and `preview=true` return images, and the chart command always returns its PNG. Claude checks its own slides and pages and fixes crowded ones before saving. Ask "show me slide 3" or "what does page 2 look like" to see it yourself.
- **Ask questions without changing anything.** "What did we spend on groceries in March?" becomes `office_sheet_calc` or `office_sheet_get`. "How long is this report?" becomes `office_doc_word_count`. "What's in this PDF?" becomes `office_pdf_info` plus `office_pdf_text`. These are read-only.
- **Undo works.** "Undo that sort" calls `undo`, and `history` lists the steps. Saves and PDF commands write files, so they can't be undone; the file on disk stays.
- **`documents` lists what's open**, with ids, so a long conversation can go back to `sheet2` or `deck1`.

## Formulas

Formulas start with `=` (optional in `office.sheet_calc`). They are recalculated whenever a cell changes, in dependency order, and circular references give `#CYCLE!`.

| | |
| --- | --- |
| References | `B2`, `$B$2`, `B2:D13`, whole columns `C:C`, whole rows `2:5`, other sheets `Sheet2!A1` and `'My sheet'!A1:B5` |
| Operators | `+ - * / ^`, `&` (join text), `= <> < > <= >=`, `%` (`50%` is 0.5), unary minus |
| Values | numbers, `"text"`, `TRUE`/`FALSE`; dates are day numbers like Excel's (`2026-03-01` typed into a cell becomes a date) |
| Errors | `#DIV/0!`, `#N/A`, `#NAME?` (unknown function), `#REF!` (deleted cells), `#VALUE!`, `#NUM!`, `#CYCLE!` |
| Math | `SUM`, `PRODUCT`, `AVERAGE`, `MIN`, `MAX`, `MEDIAN`, `COUNT`, `COUNTA`, `COUNTBLANK`, `ROUND`, `ROUNDUP`, `ROUNDDOWN`, `INT`, `MOD`, `ABS`, `SQRT`, `POWER`, `PI` |
| Conditions | `SUMIF`, `COUNTIF`, `AVERAGEIF`, `SUMIFS`, `COUNTIFS`, `AVERAGEIFS`, `MAXIFS`, `MINIFS` (criteria such as `">100"`, `"<>done"`, `"a*"`, `""`) |
| Logic | `IF`, `IFS`, `SWITCH`, `IFERROR`, `AND`, `OR`, `NOT`, `ISBLANK`, `ISNUMBER`, `ISTEXT`, `ISERROR` |
| Lookup | `VLOOKUP`, `HLOOKUP`, `XLOOKUP`, `INDEX`, `MATCH`, `ROW`, `COLUMN` |
| Text | `LEFT`, `RIGHT`, `MID`, `LEN`, `UPPER`, `LOWER`, `TRIM`, `SUBSTITUTE`, `CONCAT`, `CONCATENATE`, `TEXTJOIN`, `TEXT`, `VALUE` |
| Dates | `DATE`, `YEAR`, `MONTH`, `DAY`, `TODAY`, `NOW` |

Number formats (`office.sheet_format format=`) accept presets or Excel codes. The presets are `general`, `number` (`#,##0.00`), `integer`, `currency` (`$#,##0.00`), `euro`, `pound`, `accounting`, `percent`, `percent2`, `date` (`yyyy-mm-dd`), `datetime`, `time`, `scientific` and `text`. Codes look like `#,##0.00`, `0.0%`, `"0 kg"` or `dd/mm/yyyy`.

## Command reference

"Read-only" means the command changes no files and no open document; `*_new` and `*_open` are read-only in that sense even though they open a document. "Undoable" commands change an open document and are one undo step each. "Not undoable" commands write files.

Two parameters are left out of the tables below because so many commands share them:

- `doc`: the id of the open document. It can be left out when only one document of that kind is open.
- `preview`: attach a PNG of the result (default false). The commands that edit a document take it.

### Spreadsheets

#### office.sheet_new
Create an empty workbook and return its id (sheet1, sheet2, ...). Optionally fill the first sheet from CSV/TSV text (cells are typed: numbers, =formulas, 15%, $1,200, 2026-03-01 dates, TRUE/FALSE). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `name` | string |  | name of the first sheet (default Sheet1) |
| `sheets` | integer |  | how many sheets to start with (default 1) |
| `csv` | string |  | initial cells as CSV or tab-separated text, one row per line |
| `csv_file` | string |  | read the CSV/TSV text from this file instead |

#### office.sheet_open
Open a spreadsheet: .xlsx/.xlsm (values, formulas, number formats, bold, column widths, all sheets), .csv/.tsv (delimiter detected), .ods; .xls and other formats through LibreOffice. Formulas are recalculated on open. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string | yes | file to open |
| `delimiter` | string |  | CSV delimiter [auto\|comma\|semicolon\|tab\|pipe] (default auto) |

#### office.sheet_save
Save the workbook. The extension picks the format: .xlsx (all sheets, formulas with cached values, formats, bold, widths), .csv/.tsv (one sheet's computed values, no formulas), .pdf (printable grid), .html (tables); .ods/.xls through LibreOffice. Without path it saves back to where it was opened or last saved. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string |  | output file (default: the document's own path) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_close
Close a workbook without saving (undo reopens it). Undoable.

#### office.sheet_info
Describe a workbook: its sheets with used ranges, cell and formula counts, errors, the active sheet and file path. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |

#### office.sheet_get
Read cells. One cell gives its value, type, formula, format and what it reads from / what reads it. A range gives a tab-separated grid with column letters and row numbers (show=formulas lists what to type to recreate each cell; show=both adds formulas next to values). Without range: the used area of the sheet. format=csv returns plain CSV. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `range` | string |  | cell or range: B2, A1:D20, C:C, 'My sheet'!A1:B5 |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `show` | string |  | what to show for each cell [values\|formulas\|both] (default values) |
| `format` | string |  | grid (row numbers + letters, tab-separated) or csv [grid\|csv] (default grid) |
| `max_rows` | integer |  | stop after this many rows (default 200) |

#### office.sheet_set
Write cells. value= goes into every cell of range: a number, text, TRUE/FALSE, 15%, $1,200, a 2026-03-01 date or a =formula (copied down/across a range with relative references moving, like Excel's fill). csv= pastes a block of comma- or tab-separated rows starting at the range's top-left cell. Everything is recalculated. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | target cell or range, e.g. B2 or D2:D13 or Sheet2!A1 |
| `value` | string |  | value or =formula |
| `csv` | string |  | rows of CSV/TSV text to paste at range's top-left |
| `csv_file` | string |  | read the CSV/TSV text from this file instead |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_clear
Clear a range: everything, only the contents, or only the formatting (number format and bold). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells to clear, e.g. B2:D10 |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `what` | string |  | what to clear [all\|contents\|formats] (default all) |

#### office.sheet_fill
Fill a range from its first cell (or first row/column): formulas are copied with relative references moving, values are repeated. With step= the first cell's number (or date) grows by step each cell: a series 1, 2, 3... or dates a week apart (step=7). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | range whose first cell/row is the source, e.g. D2:D13 |
| `direction` | string |  | fill down or right (default: down when the range is taller than wide) [down\|right] |
| `step` | number |  | make a series that grows by this much per cell |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_format
Format a range: a number format (a preset - accounting, currency, date, datetime, euro, general, integer, number, percent, percent2, pound, scientific, text, time - or a code like #,##0.00, 0.0%, $#,##0, yyyy-mm-dd, "0 kg"), bold on/off, column width in characters, or autofit widths to the contents. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string | yes | cells to format, e.g. B2:D13 or C:C |
| `format` | string |  | number format preset or code |
| `bold` | boolean |  | bold text |
| `width` | number |  | column width in characters (Excel units, default columns are 8.43) |
| `autofit` | boolean |  | set column widths to fit their contents (default false) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_insert_rows / office.sheet_insert_cols
Insert empty rows before row `at`, or columns before column `at`; formulas that point past them move. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `at` | string | yes | first row number (5), or first column as a letter (C) or number (3) |
| `count` | integer |  | how many (default 1) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_delete_rows / office.sheet_delete_cols
Delete rows or columns starting at `at`; references to deleted cells become #REF!. Undoable. Same parameters as the insert commands.

#### office.sheet_sort
Sort the rows of a range by one or more columns. by= takes column letters or header names, each optionally followed by asc or desc: by="C desc, A". The header row stays on top (detected, or set header=). Numbers sort before text, blanks go last; formulas move with their rows. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string |  | rows to sort (default: the used area) |
| `by` | string | yes | sort keys, e.g. B or "Sales desc, Region" |
| `descending` | boolean |  | default direction for keys without asc/desc (default false) |
| `header` | boolean |  | the first row is a header (default: detected) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_filter
Keep the rows where a column matches a condition. criteria works like COUNTIF: ">100", "<=0", "North", "<>done", "a*" (wildcards), "" (blank). mode=copy (default) writes the header and matching rows as values to a new sheet; mode=delete removes the other rows in place; mode=count only counts. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `range` | string |  | table to filter including its header (default: the used area) |
| `column` | string | yes | column letter or header name to test |
| `criteria` | string | yes | condition, e.g. >100 or North |
| `mode` | string |  | what to do with matches [copy\|delete\|count] (default copy) |
| `to` | string |  | mode=copy: name of the new sheet (default: <sheet> filtered) |
| `header` | boolean |  | the first row is a header (default: detected) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |

#### office.sheet_find
Find cells whose displayed value (and optionally formula) contains the text. Lists up to 200 matches. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `query` | string | yes | text to look for |
| `sheet` | string |  | only this sheet (default: all sheets) |
| `match_case` | boolean |  | case-sensitive (default false) |
| `whole_cell` | boolean |  | the whole cell must equal the query (default false) |
| `formulas` | boolean |  | also search formula text (default false) |

#### office.sheet_replace
Replace text inside text cells (and formulas with formulas=true) across the workbook or one sheet. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `find` | string | yes | text to replace |
| `replace` | string |  | replacement (may be empty) |
| `sheet` | string |  | only this sheet (default: all) |
| `match_case` | boolean |  | case-sensitive (default false) |
| `formulas` | boolean |  | also replace inside formulas (default false) |

#### office.sheet_calc
Recalculate every formula, or evaluate a formula without storing it, as if it sat in cell `at` (e.g. formula="SUMIF(A:A,\"North\",C:C)"). Handy to check a result before writing it into the sheet. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `formula` | string |  | formula to evaluate, with or without the leading = |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `at` | string |  | cell the formula is evaluated in (for relative refs) (default A1) |

#### office.sheet_chart
Draw a bar, hbar (horizontal bars), line or pie chart of a range as a PNG. The usual layout is detected: a header row of series names and a first column of category labels (e.g. A1:C13 with months in A and two series in B and C). by_rows=true reads series from rows instead. With path the PNG is written to that file; it is always attached. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `range` | string |  | data range including headers/labels, e.g. A1:C13 (default: the used area) |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `type` | string |  | chart type [bar\|hbar\|line\|pie] (default bar) |
| `title` | string |  | chart title |
| `by_rows` | boolean |  | series are rows, not columns (default false) |
| `series` | string |  | only these series: header names or numbers, e.g. Total or 1,3 |
| `path` | string |  | write the PNG here (e.g. chart.png) |
| `width` | integer |  | pixels (default 800) |
| `height` | integer |  | pixels (default 500) |

#### office.sheet_preview
Render cells as a PNG grid with column letters and row numbers, the way a spreadsheet app shows them. Without range: the used area (up to 50 rows x 20 columns). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open spreadsheet: read this file (for one-shot use) |
| `range` | string |  | cells to show, e.g. A1:H30 |
| `sheet` | string |  | sheet name or 1-based number (default: the active sheet) |
| `scale` | number |  | pixels per point (default 1.5) |

#### Sheets in a workbook

| Command | Parameters | What it does |
| --- | --- | --- |
| `office.sheet_add` | `name` (default SheetN), `at` (position, default last), `csv`, `csv_file` | add a sheet and make it active, optionally filled from CSV/TSV. Undoable |
| `office.sheet_rename` | `sheet` (required), `name` (required) | rename a sheet; formulas that refer to it are updated. Undoable |
| `office.sheet_delete` | `sheet` (required) | delete a sheet; references to it become #REF!. Undoable |
| `office.sheet_select` | `sheet` (required) | make a sheet the active one (the default for other commands and for CSV saves). Undoable |

### Documents

#### office.doc_new
Create a document and return its id (doc1, ...). markdown= fills it straight away (# headings, **bold**, *italic*, [links](url), - lists, 1. numbered lists, | tables |, > quotes, ``` code, ![images](file.png), <!-- pagebreak -->). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `markdown` | string |  | initial content as Markdown |
| `markdown_file` | string |  | read the Markdown from this file (images relative to it) |
| `page` | string |  | paper size [a4\|letter\|legal\|a5\|a3] |
| `landscape` | boolean |  | landscape orientation |
| `margin` | number |  | page margin in mm (all sides, default 25.4) |
| `font` | string |  | body font [sans\|serif\|mono] |
| `font_size` | number |  | body text size in pt (default 11) |
| `page_numbers` | boolean |  | "n / N" page numbers in the footer |
| `title` | string |  | document title (file metadata) |
| `author` | string |  | author (file metadata) |

#### office.doc_open
Open a document: .docx (headings, bold/italic/underline, links, lists, tables, images, page breaks), .md, .html, .odt, .txt; .doc, .rtf and others through LibreOffice. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string | yes | file to open |

#### office.doc_save
Save the document; the extension picks the format: .docx (Word), .md (images go to <name>_media/), .html (one self-contained file), .txt, .pdf; .odt, .rtf, .doc through LibreOffice. Without path it saves back to its own file. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string |  | output file (default: the document's own path) |

#### office.doc_export
Export to another file without changing which file the document is saved to: .pdf (laid out here: fonts, word wrap, lists, tables, images, page numbers), .docx, .md, .html, .txt, .png/.jpg (one page, page= and dpi=); .odt/.rtf/.doc through LibreOffice. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `path` | string | yes | output file; the extension picks the format |
| `page` | integer |  | png/jpg: which page (default 1) |
| `dpi` | number |  | png/jpg resolution (default 110) |

#### office.doc_close
Close a document without saving (undo reopens it). Undoable.

#### office.doc_info
Describe a document: title, author, page setup, page count, word count and every block with its number (the numbers doc_edit, doc_delete, doc_move and doc_add at= use). Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `max_blocks` | integer |  | list at most this many blocks (default 200) |

#### office.doc_get_text
Read the document's content as Markdown (default) or plain text, each block prefixed with its number [n] unless numbered=false. from/to pick a block range. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `format` | string |  | markdown keeps styles, text is plain [markdown\|text] (default markdown) |
| `numbered` | boolean |  | prefix each block with [n] (default true) |
| `from` | integer |  | first block (1-based) |
| `to` | integer |  | last block |

#### office.doc_add
Add content. kind=paragraph|heading|quote take inline Markdown text (**bold**, *italic*, <u>underline</u>, `code`, [link](url)); list/numbered take one item per line (indent 2 spaces for sub-items); table takes one row per line with cells split by |, tabs or commas (first row is the header); code keeps text as-is; image needs path=; pagebreak needs nothing; markdown adds every block of a Markdown text. at= inserts before that block number (default: at the end). Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `kind` | string |  | what to add [paragraph\|heading\|list\|numbered\|table\|image\|code\|quote\|pagebreak\|markdown] (default paragraph) |
| `text` | string |  | the content |
| `text_file` | string |  | read the content from this file |
| `level` | integer |  | heading level 1-6 (default 1) |
| `at` | integer |  | insert before this block number (default: append) |
| `path` | string |  | image file (kind=image) |
| `width` | number |  | image width in pt (default: natural size, at most the text width) |
| `alt` | string |  | image description |
| `header` | boolean |  | table: the first row is a header (default true) |

#### office.doc_edit
Change a block: new text (same rules as doc_add for its kind), a different kind (e.g. paragraph -> heading), a heading level, or for images a new path/width/alt. Fields left out stay as they are. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `block` | integer | yes | block number (office.doc_info lists them) |
| `text` | string |  | new content |
| `text_file` | string |  | read the new content from this file |
| `kind` | string |  | change the block into this kind (same kinds as doc_add) |
| `level` | integer |  | heading level 1-6 |
| `path` | string |  | image file |
| `width` | number |  | image width in pt |
| `alt` | string |  | image description |
| `header` | boolean |  | table: first row is a header |

#### office.doc_delete / office.doc_move
`office.doc_delete` deletes `block` (required), or `count` blocks starting there. `office.doc_move` moves `block` (required) so it ends up at position `to` (required). Both undoable.

#### office.doc_find_replace
Find text everywhere in the document (headings, paragraphs, lists, tables, quotes, code) and replace it, keeping each run's style. Without replace it only counts and lists where the text is. A match must sit inside one style run. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `find` | string | yes | text to find |
| `replace` | string |  | replacement (may be empty) |
| `match_case` | boolean |  | case-sensitive (default false) |
| `whole_word` | boolean |  | only whole words (default false) |

#### office.doc_setup
Change the paper size, orientation, margins, body font and size, page numbers, title or author. Undoable. Takes the same `page`, `landscape`, `margin`, `font`, `font_size`, `page_numbers`, `title` and `author` parameters as `office.doc_new`.

#### office.doc_word_count
Count words, characters (with and without spaces), paragraphs and pages, with a reading time. Read-only. Takes `file` for one-shot use.

#### office.doc_preview
Render one page as a PNG, exactly as the PDF export lays it out. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open document: read this file (for one-shot use) |
| `page` | integer |  | page number (default 1) |
| `scale` | number |  | pixels per point (default 1.25) |

### Slide decks

#### office.deck_new
Create a 16:9 slide deck and return its id (deck1, ...). outline= builds every slide from Markdown at once (see the outline syntax in task 4). A first slide without bullets becomes the title slide. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `title` | string |  | deck title (file metadata) |
| `author` | string |  | author |
| `theme` | string |  | colour theme: blue, teal, green, orange, red, purple, slate, dark, or an accent colour like #0a7d5a (default blue) |
| `font` | string |  | font family [sans\|serif\|mono] (default sans) |
| `outline` | string |  | slides as a Markdown outline |
| `outline_file` | string |  | read the outline from this .md file |
| `aspect` | string |  | slide shape [16:9\|4:3] (default 16:9) |

#### office.deck_open
Open a deck: .pptx (titles, subtitles, bullets with levels, the first picture and speaker notes of each slide, theme accent colour) or a Markdown outline (.md); .odp and .ppt through LibreOffice. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string | yes | file to open |

#### office.deck_save
Save the deck: .pptx (PowerPoint, Keynote, Google Slides and LibreOffice open it), .pdf (one page per slide), .md (outline; pictures go to <name>_media/); .odp/.ppt through LibreOffice. Without path it saves back to its own file. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `path` | string |  | output file (default: the deck's own path) |

#### office.deck_export
Export without changing the deck's own file: .pdf, .pptx, .md, .odp/.ppt (LibreOffice), or .png - one slide with slide=, otherwise every slide as name-1.png, name-2.png, ... Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |
| `path` | string | yes | output file; the extension picks the format |
| `slide` | integer |  | png: only this slide |
| `width` | integer |  | png width in pixels (default 1280) |

#### office.deck_close
Close a deck without saving (undo reopens it). Undoable.

#### office.deck_info
List the slides with their layouts, titles, bullet counts, pictures and notes, and warn about slides with too much text. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |
| `outline` | boolean |  | also return the whole deck as a Markdown outline (default false) |

#### office.deck_add_slide
Add a slide. Layouts: title, section, content, two_column, image, image_right, title_only, blank. Without layout: the first slide of an empty deck with no bullets is a title slide, otherwise content (which puts a picture beside the bullets when image= is given). Long bullet lists shrink to fit; info warns when they no longer can. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `layout` | string |  | slide layout [title\|section\|content\|two_column\|image\|image_right\|title_only\|blank] |
| `title` | string |  | slide title (inline Markdown) |
| `subtitle` | string |  | subtitle (title/section slides) or a line under the title |
| `bullets` | string |  | bullet points, one per line; indent 2 spaces for sub-bullets; - or 1. markers optional |
| `bullets_file` | string |  | read the bullets from this file |
| `right` | string |  | two_column: the right column's bullets |
| `image` | string |  | picture file (png/jpg) for image layouts |
| `notes` | string |  | speaker notes |
| `at` | integer |  | insert at this position (default: last) |

#### office.deck_edit_slide
Change a slide's layout, title, subtitle, bullets, right column, image or notes; fields left out stay. Undoable. Takes `slide` (required), the same fields as `office.deck_add_slide` except `at`, and `clear_image=true` to remove the picture.

#### office.deck_delete_slide / office.deck_move_slide
`office.deck_delete_slide` deletes `slide` (required). `office.deck_move_slide` moves `slide` (required) so it ends up at position `to` (required). Both undoable.

#### office.deck_theme
Change the colour theme and/or the font of the whole deck. Undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `theme` | string |  | colour theme: blue, teal, green, orange, red, purple, slate, dark, or an accent colour like #0a7d5a |
| `font` | string |  | font family [sans\|serif\|mono] |

#### office.deck_preview
Render a slide (or all=true for every slide) as PNG, exactly as the PDF export draws it. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `file` | string |  | instead of an open slide deck: read this file (for one-shot use) |
| `slide` | integer |  | slide number (default 1) |
| `all` | boolean |  | attach every slide (default false) |
| `width` | integer |  | pixels (default 960) |

### PDF files

All PDF commands except `pdf_info` and `pdf_text` write files and are not undoable. Page selections look like `1-3,7`, `10-`, `last` or `5-1`.

| Command | Parameters (defaults) | What it does |
| --- | --- | --- |
| `office.pdf_info` | `path` (required) | page count, page sizes, PDF version, title/author/producer, whether it is encrypted or has text. Read-only |
| `office.pdf_text` | `path` (required), `pages` (all) | the text of each page; works for text PDFs, not scans. Read-only |
| `office.pdf_merge` | `inputs` (required: files separated by `;` or `,`, each optionally `file.pdf[pages]`), `output` (required) | join PDFs in order |
| `office.pdf_extract` | `path`, `pages`, `output` (all required) | write the chosen pages, in the order given (repeats allowed), to a new PDF |
| `office.pdf_delete_pages` | `path` (required), `pages` (required), `output` (default: overwrite `path`) | remove pages |
| `office.pdf_split` | `path` (required), `every` (1), `ranges` (parts separated by `;`), `output` (name pattern with `{n}`; default `<name>-{n}.pdf` next to the input) | split into several files |
| `office.pdf_rotate` | `path` (required), `angle` (90; also 180, 270, -90), `pages` (all), `output` (default: overwrite `path`) | turn pages clockwise |
| `office.pdf_from_images` | `images` (required: files separated by `;` or `,`), `output` (required), `page` (`fit`, `a4`, `letter`; default fit), `margin` (36 pt on paper), `dpi` (96, for fit) | one image per page; JPEGs are embedded unchanged, paper pages turn to match each image |
| `office.pdf_render` | `path` (required), `page` (1), `dpi` (100), `output` | a page as PNG through pdftoppm, attached and optionally written to `output` |

### Conversion

#### office.convert
Convert an office file in one step; the output's extension picks the format. The built-in pairs (no LibreOffice) are:

- spreadsheets: csv/tsv/xlsx/ods to xlsx/csv/tsv/pdf/html
- documents: docx/md/html/odt/txt to docx/md/html/txt/pdf
- decks: pptx or a Markdown outline to pptx/pdf/md
- pdf to txt (text extraction)

Everything else (doc, xls, ppt, rtf, odt/ods/odp output, ...) goes through LibreOffice when it is installed. When only one side is built in, the suite reads or writes that side itself and LibreOffice does the other. Not undoable.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `input` | string | yes | file to convert |
| `output` | string | yes | file to write; its extension sets the format |
| `backend` | string |  | `auto` uses the built-in engines when they handle the pair; `builtin` refuses anything else; `libreoffice` forces LibreOffice (default auto) |
| `sheet` | integer |  | spreadsheet to csv/tsv: which sheet (1-based; default the active one) |
| `kind` | string |  | how to read a Markdown input: as a document or a slide outline [auto\|doc\|deck] (default auto) |

`convert.file` from the [convert tool](convert.md) also handles office files. `office.convert` is the one to use for office work, because it needs LibreOffice only for the formats that aren't built in.

#### office.formats
List the file formats the office tool reads and writes, which are built in and which need LibreOffice, and whether LibreOffice and pdftoppm are installed. Read-only.

## Limits

- **Scripts in PDFs.** Western European text uses the standard PDF fonts. Greek and Cyrillic are set in the built-in font (Roboto, embedded with only the letters used, so the file stays small) and copy and search correctly. Chinese, Japanese, Korean, Arabic, Hebrew, Indic scripts and emoji still come out as `?` in PDFs and previews. `.docx`, `.xlsx`, `.pptx`, `.md` and `.html` keep every character, so export those through LibreOffice when you need those scripts in a PDF (`office.convert input=report.docx output=report.pdf backend=libreoffice`).
- **Spreadsheets:**
  - Cell styles are limited to number formats, bold and column widths. Colours, borders, merged cells, conditional formatting, data validation, pivot tables and charts inside the `.xlsx` are not read or written; charts are PNG files.
  - `.xlsx` files from Excel open with their values and formulas. Functions the engine doesn't know show `#NAME?` until the cell is rewritten.
  - CSV saves hold one sheet's computed values.
  - Sorting moves formulas with their rows but doesn't rewrite references to those rows from elsewhere.
- **Documents:**
  - Table column alignment (`---:`) and column widths are ignored: columns share the width evenly.
  - Headers and footers other than page numbers, footnotes, comments, tracked changes, text boxes, columns and fonts other than sans/serif/mono are dropped when a `.docx` is opened.
  - Images are placed inline, one per block.
- **Slides:**
  - Decks use the suite's eight layouts. A `.pptx` from elsewhere is read for its text, the first picture on each slide, notes and accent colour, then laid out again in those layouts. Its own master slides, shapes, animations, transitions and extra pictures are not kept.
  - There are no charts or tables on slides; put a chart PNG on an image slide.
- **PDFs:**
  - Merge, split, extract and rotate keep each page as it is, but the new file has no bookmarks and no working form fields.
  - Encrypted PDFs can't be merged, split or read; `office.pdf_info` says when a file is encrypted.
  - Text extraction reads text PDFs, not scans (there is no OCR), and complex layouts may come out in an odd order.
  - `office.pdf_render` needs `pdftoppm` (poppler-utils); the suite's own previews don't.
- **LibreOffice formats** (`.doc`, `.xls`, `.ppt`, `.rtf`, `.odt`/`.ods`/`.odp` output and the rest) need LibreOffice installed. Without it those commands say so and change nothing. The first LibreOffice conversion takes a few seconds while LibreOffice starts.
