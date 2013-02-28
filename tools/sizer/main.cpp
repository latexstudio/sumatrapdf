#include "BaseUtil.h"
#include "BitManip.h"
#include "Dict.h"

#include "Dia2Subset.h"
#include "Util.h"

// Add SymTag to get the DIA name (i.e. Null => SymTagNull). You can then google
// that name to figure out what it means
//
// must match order of enum SymTagEnum in Dia2Subset.h
const char *g_symTypeNames[SymTagMax] = {
    "Null",
    "Exe",
    "Compiland",
    "CompilandDetails",
    "CompilandEnv",
    "Function",
    "Block",
    "Data",
    "Annotation",
    "Label",
    "PublicSymbol",
    "UDT",
    "Enum",
    "FunctionType",
    "PointerType",
    "ArrayType",
    "BaseType",
    "Typedef",
    "BaseClass",
    "Friend",
    "FunctionArgType",
    "FuncDebugStart",
    "FuncDebugEnd",
    "UsingNamespace",
    "VTableShape",
    "VTable",
    "Custom",
    "Thunk",
    "CustomType",
    "ManagedType",
    "Dimension"
};

const char *g_symTypeNamesCompact[SymTagMax] = {
    "N",        // Null
    "Exe",      // Exe
    "C",        // Compiland
    "CD",       // CompilandDetails
    "CE",       // CompilandEnv
    "F",        // Function
    "B",        // Block
    "D",        // Data
    "A",        // Annotation
    "L",        // Label
    "P",        // PublicSymbol
    "U",        // UDT
    "E",        // Enum
    "FT",       // FunctionType
    "PT",       // PointerType
    "AT",       // ArrayType
    "BT",       // BaseType
    "T",        // Typedef
    "BC",       // BaseClass
    "Friend",   // Friend
    "FAT",      // FunctionArgType
    "FDS",      // FuncDebugStart
    "FDE",      // FuncDebugEnd
    "UN",       // UsingNamespace
    "VTS",      // VTableShape
    "VT",       // VTable
    "Custom",   // Custom
    "Thunk",    // Thunk
    "CT",       // CustomType
    "MT",       // ManagedType
    "Dim"       // Dimension
};
static str::Str<char> g_strTmp;

static str::Str<char> g_report;
static StringInterner g_strInterner;

static bool           g_dumpSections = false;
static bool           g_dumpSymbols = false;
static bool           g_dumpTypes = false;
static bool           g_compact = false;

static int InternString(const char *s)
{
    return g_strInterner.Intern(s);
}

static void GetInternedStringsReport(str::Str<char>& resOut)
{
    resOut.Append("Strings:\n");
    int n = g_strInterner.StringsCount();
    for (int i = 0; i < n; i++) {
        resOut.AppendFmt("%d|%s\n", i, g_strInterner.GetByIndex(i));
    }
    resOut.Append("\n");
}

static const char *GetSymTypeName(int i)
{
    if (i >= SymTagMax)
        return "<unknown type>";
    if (g_compact)
        return g_symTypeNamesCompact[i];
    else
        return g_symTypeNames[i];
}

static const char *GetSectionType(IDiaSectionContrib *item)
{
    BOOL code=FALSE,initData=FALSE,uninitData=FALSE;
    item->get_code(&code);
    item->get_initializedData(&initData);
    item->get_uninitializedData(&uninitData);

    if (code && !initData && !uninitData)
        return "C";
    if (!code && initData && !uninitData)
        return "D";
    if (!code && !initData && uninitData)
        return "B";
    return "U";
}

static void DumpSection(IDiaSectionContrib *item)
{
    DWORD           sectionNo;
    DWORD           offset;
    DWORD           length;
    BSTR            objFileName = 0;
    int             objFileId;
    IDiaSymbol *    compiland = 0;

    item->get_addressSection(&sectionNo);
    item->get_addressOffset(&offset);
    item->get_length(&length);

    //DWORD compilandId;
    //item->get_compilandId(&compilandId);

    const char *sectionType = GetSectionType(item);

    item->get_compiland(&compiland);
    if (compiland)
    {
        compiland->get_name(&objFileName);
        compiland->Release();
    }

    BStrToString(g_strTmp, objFileName, "<noobjfile>");

    if (g_compact) {
        // type | sectionNo | length | offset | objFileId
        objFileId = InternString(g_strTmp.Get());
        g_report.AppendFmt("%s|%d|%d|%d|%d\n", sectionType, sectionNo, length, offset, objFileId);
    } else {
        // type | sectionNo | length | offset | objFile
        g_report.AppendFmt("%s|%d|%d|%d|%s\n", sectionType, sectionNo, length, offset, g_strTmp.Get());
    }
    if (objFileName)
        SysFreeString(objFileName);
}

static void DumpSymbol(IDiaSymbol *symbol)
{
    DWORD               section, offset, rva;
    DWORD               dwTag;
    enum SymTagEnum     tag;
    ULONGLONG           length = 0;
    BSTR                name = 0;
    BSTR                srcFileName = 0;
    const char *        typeName;

    symbol->get_symTag(&dwTag);
    tag = (enum SymTagEnum)dwTag;
    typeName = GetSymTypeName(tag);

    symbol->get_relativeVirtualAddress(&rva);
    symbol->get_length(&length);
    symbol->get_addressSection(&section);
    symbol->get_addressOffset(&offset);

    // get length from type for data
    if (tag == SymTagData)
    {
        IDiaSymbol *type = NULL;
        if (symbol->get_type(&type) == S_OK) // no SUCCEEDED test as may return S_FALSE!
        {
            if (FAILED(type->get_length(&length)))
                length = 0;
            type->Release();
        }
        else
            length = 0;
    }

    symbol->get_name(&name);
    BStrToString(g_strTmp, name, "<noname>", true);
    const char *nameStr = g_strTmp.Get();

    // type | section | length | offset | rva | name
    g_report.AppendFmt("%s|%d|%d|%d|%d|%s\n", typeName, (int)section, (int)length, (int)offset, (int)rva, nameStr);

    if (name)
        SysFreeString(name);
}

static void AddReportSepLine()
{
    if (g_report.Count() > 0)
        g_report.Append("\n");
}

static void DumpSymbols(IDiaSession *session)
{
    HRESULT                 hr;
    IDiaEnumSymbolsByAddr * enumByAddr = NULL;
    IDiaSymbol *            symbol = NULL;

    hr = session->getSymbolsByAddr(&enumByAddr);
    if (!SUCCEEDED(hr))
        goto Exit;

    // get first symbol to get first RVA (argh)
    hr = enumByAddr->symbolByAddr(1, 0, &symbol);
    if (!SUCCEEDED(hr))
        goto Exit;

    DWORD rva;
    hr = symbol->get_relativeVirtualAddress(&rva);
    if (S_OK != hr)
        goto Exit;

    symbol->Release();
    symbol = NULL;

    // enumerate by rva
    hr = enumByAddr->symbolByRVA(rva, &symbol);
    if (!SUCCEEDED(hr))
        goto Exit;

    AddReportSepLine();
    g_report.Append("Symbols:\n");

    ULONG numFetched;
    for (;;)
    {
        DumpSymbol(symbol);
        symbol->Release();
        symbol = NULL;

        hr = enumByAddr->Next(1, &symbol, &numFetched);
        if (FAILED(hr) || (numFetched != 1))
            break;
    }

Exit:
    if (symbol)
        symbol->Release();
    if (enumByAddr)
        enumByAddr->Release();
}

static void DumpSections(IDiaSession *session)
{
    HRESULT             hr;
    IDiaEnumTables *    enumTables = NULL;
    IDiaTable *         secTable = NULL;

    hr = session->getEnumTables(&enumTables);
    if (S_OK != hr)
        return;

    AddReportSepLine();
    g_report.Append("Sections:\n");

    VARIANT vIndex;
    vIndex.vt = VT_BSTR;
    vIndex.bstrVal = SysAllocString(L"Sections");

    hr = enumTables->Item(vIndex, &secTable);
    if (S_OK != hr)
        goto Exit;

    LONG count;

    secTable->get_Count(&count);

    IDiaSectionContrib *item;
    ULONG numFetched;
    for (;;)
    {
        hr = secTable->Next(1,(IUnknown **)&item, &numFetched);
        if (FAILED(hr) || (numFetched != 1))
            break;

        DumpSection(item);
        item->Release();
    }

Exit:
    if (secTable)
        secTable->Release();
    SysFreeString(vIndex.bstrVal);
    if (enumTables)
        enumTables->Release();
}

static void ProcessPdbFile(const char *fileNameA)
{
    HRESULT             hr;
    IDiaDataSource *    dia = NULL;
    IDiaSession *       session = NULL;
    str::Str<char>      report;

    dia = LoadDia();
    if (!dia)
        return;

    ScopedMem<WCHAR> fileName(str::conv::FromAnsi(fileNameA));
    hr = dia->loadDataForExe(fileName, 0, 0);
    if (FAILED(hr)) {
        log("  failed to load debug symbols (PDB not found)\n");
        goto Exit;
    }

    hr = dia->openSession(&session);
    if (FAILED(hr)) {
        log("  failed to open DIA session\n");
        goto Exit;
    }

    if (g_dumpSections) {
        DumpSections(session);
    }

    if (g_dumpSymbols) {
        DumpSymbols(session);
    }

    if (g_compact) {
        str::Str<char> res;
        GetInternedStringsReport(res);
        fputs(res.Get(), stdout);
    }
    fputs(g_report.Get(), stdout);

Exit:
    if (session)
        session->Release();
}

static char *g_fileName = NULL;

static void ParseCommandLine(int argc, char **argv)
{
    char *s;
    for (int i=0; i<argc; i++) {
        s = argv[i];
        if (str::EqI(s, "-compact"))
            g_compact = true;
        else if (str::EqI(s, "-sections"))
            g_dumpSections = true;
        else if (str::EqI(s, "-symbols"))
            g_dumpSymbols = true;
        else if (str::EqI(s, "-types"))
            g_dumpTypes = true;
        else {
            if (g_fileName != NULL)
                goto InvalidCmdLine;
            g_fileName = s;
        }
    }
    if (!g_fileName)
        goto InvalidCmdLine;

    if (!g_dumpSections && !g_dumpSymbols && !g_dumpTypes) {
        // no options specified so use default settings:
        // dump all information in non-compact way
        g_dumpSections = true;
        g_dumpSymbols = true;
        g_dumpTypes = true;
    }

    return;

InvalidCmdLine:
    log("Usage: sizer [-compact] [-sections] [-symbols] [-types] <exefile>\n");
    exit(1);
}

int main(int argc, char** argv)
{
    ScopedCom comInitializer;
    ParseCommandLine(argc-1, &argv[1]);
    ProcessPdbFile(g_fileName);
    return 0;
}