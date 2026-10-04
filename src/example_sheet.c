#include "example_sheet.h"
/* Explicit row-major sources keep the fixture small and independently readable.
 * Amounts are illustrative, not a recommendation or anyone's personal data. */
typedef struct { unsigned short index; unsigned char kind; const char *text; } Cell;
static const Cell cells[] = {
    {0,1,"Category"},{1,1,"Planned"},{2,1,"Actual"},{3,1,"Remaining"},
    {26,1,"Rent"},{27,2,"1200"},{28,2,"1200"},{29,3,"=B2-C2"},
    {52,1,"Food"},{53,2,"400"},{54,2,"365.50"},{55,3,"=B3-C3"},
    {78,1,"Transport"},{79,2,"120"},{80,2,"98.75"},{81,3,"=B4-C4"},
    {104,1,"Utilities"},{105,2,"180"},{106,2,"172.10"},{107,3,"=B5-C5"},
    {156,1,"Total"},{157,3,"=SUM(B2:B5)"},{158,3,"=SUM(C2:C5)"},{159,3,"=B7-C7"},
    {208,1,"Example only: edit the amounts to plan your own budget."}
};
unsigned example_budget_sheet(unsigned char *out, unsigned capacity) {
    unsigned size=16;
    for(unsigned i=0;i<sizeof cells/sizeof cells[0];i++){
        unsigned n=0;while(cells[i].text[n])n++;size+=4+n;
    }
    if(!out||capacity<size)return 0;
    const unsigned char header[16]={'B','S','H','1',1,0,0,0,128,0,26,0,sizeof cells/sizeof cells[0],0,0,0};
    for(unsigned i=0;i<16;i++)out[i]=header[i];
    unsigned at=16;
    for(unsigned i=0;i<sizeof cells/sizeof cells[0];i++){
        unsigned n=0;while(cells[i].text[n])n++;
        out[at++]=(unsigned char)cells[i].index;out[at++]=(unsigned char)(cells[i].index>>8);
        out[at++]=cells[i].kind;out[at++]=(unsigned char)n;
        for(unsigned j=0;j<n;j++)out[at++]=(unsigned char)cells[i].text[j];
    }
    return at;
}
const char example_budget_csv[]=
    "Category,Planned,Actual,Remaining\r\n"
    "Rent,1200,1200,0\r\nFood,400,365.5,34.5\r\nTransport,120,98.75,21.25\r\n"
    "Utilities,180,172.1,7.9\r\n,,,\r\nTotal,1900,1836.35,63.65\r\n,,,\r\n"
    "Example only: edit the amounts to plan your own budget.,,,\r\n";
const char example_sheet_guide[]=
    "Spreadsheet quick guide\n\n"
    "Open budget.bsh for a small original budget with live formulas. Its amounts are illustrative.\n"
    "Click a cell, type, then Enter or Tab to commit. F2 edits the exact source. Escape cancels.\n"
    "Arrow keys move; Shift+arrows select a range. Ctrl+C/X/V copy/cut/paste a range.\n"
    "Ctrl+Z undoes; Ctrl+Y redoes. Formula references are copied verbatim, never shifted.\n"
    "Ctrl+G jumps to an A1 coordinate. Ctrl+Home returns to A1. Use both scrollbars.\n"
    "Ctrl+S saves native .bsh; Ctrl+Shift+S saves with a new name. Existing other files are protected.\n"
    "Ctrl+O opens .bsh or .csv. Ctrl+N and Ctrl+W ask Save/Discard/Cancel for unsaved work.\n"
    "Ctrl+Shift+E exports calculated values as CSV without changing the native save or dirty state.\n"
    "CSV formulas import as literal text. Other programs may execute formula-like CSV text.\n\n"
    "Limits: one sheet, 128 rows x 26 columns, 95 ASCII bytes per cell, four undo steps.\n"
    "Numbers use fixed-point arithmetic with three decimal places, truncating toward zero.\n"
    "Formulas: + - * /, parentheses, A1 references, SUM/AVG/MIN/MAX/COUNT and ranges.\n"
    "No charts, extra workbook tabs, Unicode, Excel file support or relative-reference rewriting.\n"
    "Native .bsh preserves formulas and cell types. CSV is values-only interchange, not a native backup.\n"
    "Automatic session recovery includes a pending cell edit. A changed source recovers as an unsaved copy.\n";
