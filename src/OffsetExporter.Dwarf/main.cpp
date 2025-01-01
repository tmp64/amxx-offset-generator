#include <boost/json.hpp>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace
{

std::set<std::string> g_ClassList;
std::set<std::string> g_ProcessedClasses;

void CheckError(int result, Dwarf_Error error)
{
    if (result == DW_DLV_OK)
        return;
    
    std::string message = "libdwarf error";

    if (result == DW_DLV_ERROR)
    {
        const char* msg = dwarf_errmsg(error);
        message += ": ";
        message += msg;
    }

    throw std::runtime_error(message);
}

bool HasAttr(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attrNum)
{
    int res;
    Dwarf_Error error;
    Dwarf_Attribute attr;

    res = dwarf_attr(die, attrNum, &attr, &error);

    if (res == DW_DLV_NO_ENTRY)
        return false;

    CheckError(res, error);
    dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
    return true;
}

Dwarf_Half GetDieTag(Dwarf_Die die)
{
    int res;
    Dwarf_Error error;

    Dwarf_Half dieTag;
    res = dwarf_tag(die, &dieTag, &error);
    CheckError(res, error);

    return dieTag;
}

std::string GetStringAttr(Dwarf_Die die, Dwarf_Half attrNum)
{
    int res;
    Dwarf_Error error;
    char* buf = nullptr;

    res = dwarf_die_text(die, attrNum, &buf, &error);
    CheckError(res, error);
    return buf;
}

int64_t GetUIntAttr(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attrNum, int64_t def = -1)
{
    int res;
    Dwarf_Error error;

    Dwarf_Attribute attr;
    res = dwarf_attr(die, attrNum, &attr, &error);

    if (res == DW_DLV_NO_ENTRY)
        return def;

    CheckError(res, error);

    Dwarf_Unsigned value;
    res = dwarf_formudata(attr, &value, &error);
    CheckError(res, error);

    dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
    return value;
}

void PrintDieAttrs(Dwarf_Debug dbg, Dwarf_Die die)
{
    int res;
    Dwarf_Error error;
    Dwarf_Attribute* attrs;
    Dwarf_Signed attrNum;
    res = dwarf_attrlist(die, &attrs, &attrNum, &error);
    CheckError(res, error);

    for (Dwarf_Signed i = 0; i < attrNum; i++)
    {
        Dwarf_Half num;
        Dwarf_Half form;

        res = dwarf_whatattr(attrs[i], &num, &error);
        CheckError(res, error);
        res = dwarf_whatform(attrs[i], &form, &error);
        CheckError(res, error);

        const char* attrName;
        dwarf_get_AT_name(num, &attrName);

        const char* formName;
        dwarf_get_FORM_name(form, &formName);

        fmt::println("  n: 0x{:X} ({}), f: 0x{:X} ({})", num, attrName, form, formName);
    }

    dwarf_dealloc(dbg, attrs, DW_DLA_ATTR);
}

Dwarf_Die FollowReference(Dwarf_Debug dbg, Dwarf_Attribute attr)
{
    int res;
    Dwarf_Error error;
    Dwarf_Off offset;
    res = dwarf_global_formref(attr, &offset, &error);
    CheckError(res, error);

    Dwarf_Die die;
    res = dwarf_offdie_b(dbg, offset, true, &die, &error);
    CheckError(res, error);

    return die;
}

template <std::invocable<Dwarf_Die> T>
void ForEachSibling(Dwarf_Debug dbg, Dwarf_Die die, T&& func)
{
    int res;
    Dwarf_Error error;

    // Process siblings
    Dwarf_Die curDie = die;

    while (true)
    {
        std::invoke(func, curDie);

        Dwarf_Die sibling = nullptr;
        res = dwarf_siblingof_c(curDie, &sibling, &error);

        if (res == DW_DLV_NO_ENTRY)
        {
            break;
        }

        CheckError(res, error);

        if (curDie != die)
            dwarf_dealloc(dbg, curDie, DW_DLA_DIE);
        curDie = sibling;
    }

    if (curDie != die)
    {
        dwarf_dealloc(dbg, curDie, DW_DLA_DIE);
        curDie = nullptr;
    }
}

template <std::invocable<Dwarf_Die> T>
void ForEachChild(Dwarf_Debug dbg, Dwarf_Die die, T&& func)
{
    int res;
    Dwarf_Error error;

    // Get the first child
    Dwarf_Die firstChild = nullptr;
    res = dwarf_child(die, &firstChild, &error);

    if (res == DW_DLV_NO_ENTRY)
        return;

    CheckError(res, error);

    // Process firstChild and siblings
    ForEachSibling(dbg, firstChild, func);
}

template <std::invocable<Dwarf_Die> T>
void RecursiveProcessDie(Dwarf_Debug dbg, Dwarf_Die die, T&& func)
{
    auto recursiveFunc = [&](Dwarf_Die die2) -> void
    {
        RecursiveProcessDie(dbg, die2, func);
    };

    ForEachSibling(dbg, die, func);
    ForEachChild(dbg, die, recursiveFunc);

    /*ProcessDie(dbg, die);

    Dwarf_Die curDie = die;
    int res;
    Dwarf_Error error;

    while (true)
    {
        Dwarf_Die child = nullptr;
        res = dwarf_child(curDie, &child, &error);

        if (res == DW_DLV_ERROR)
        {
            CheckError(res, error);
        }
        else if (res == DW_DLV_OK)
        {
            RecursiveProcessDie(dbg, child);
            dwarf_dealloc(dbg, child, DW_DLA_DIE);
            child = nullptr;
        }

        Dwarf_Die sibling = nullptr;
        res = dwarf_siblingof_c(curDie, &sibling, &error);

        if (res == DW_DLV_NO_ENTRY) {
            break;
        }

        CheckError(res, error);

        if (curDie != die) {
            dwarf_dealloc(dbg, curDie, DW_DLA_DIE);
            curDie = nullptr;
        }

        curDie = sibling;
        ProcessDie(dbg, sibling);
    }*/
}

template <std::invocable<Dwarf_Die> T>
void RecursiveProcessDie2(Dwarf_Debug dbg, Dwarf_Die die, T&& func)
{
    std::invoke(func, die);

    Dwarf_Die curDie = die;
    int res;
    Dwarf_Error error;

    while (true)
    {
        Dwarf_Die child = nullptr;
        res = dwarf_child(curDie, &child, &error);

        if (res == DW_DLV_ERROR)
        {
            CheckError(res, error);
        }
        else if (res == DW_DLV_OK)
        {
            RecursiveProcessDie2(dbg, child, func);
            dwarf_dealloc(dbg, child, DW_DLA_DIE);
            child = nullptr;
        }

        Dwarf_Die sibling = nullptr;
        res = dwarf_siblingof_c(curDie, &sibling, &error);

        if (res == DW_DLV_NO_ENTRY) {
            break;
        }

        CheckError(res, error);

        if (curDie != die) {
            dwarf_dealloc(dbg, curDie, DW_DLA_DIE);
            curDie = nullptr;
        }

        curDie = sibling;
        std::invoke(func, sibling);
    }
}

void ProcessDie(Dwarf_Debug dbg, Dwarf_Die die, boost::json::object& jClasses)
{
    int res;
    Dwarf_Error error;

    Dwarf_Half dieTag;
    res = dwarf_tag(die, &dieTag, &error);
    CheckError(res, error);

    if (dieTag != DW_TAG_class_type)
        return;

    if (HasAttr(dbg, die, DW_AT_declaration)) // Forward-decl
        return;

    std::string className = GetStringAttr(die, DW_AT_name);

    if (!g_ClassList.contains(className))
        return;

    if (g_ProcessedClasses.contains(className))
        return;

    g_ProcessedClasses.insert(className);

    boost::json::object jClass;
    jClass["baseClass"] = nullptr;

    fmt::println("class {}\n{{", className);

    ForEachChild(dbg, die, [&](Dwarf_Die childDie)
    {
        switch (GetDieTag(childDie))
        {
        case DW_TAG_inheritance:
        {
            Dwarf_Attribute attr;
            res = dwarf_attr(childDie, DW_AT_type, &attr, &error);

            Dwarf_Die baseClassDie = FollowReference(dbg, attr);
            std::string baseClassName = GetStringAttr(baseClassDie, DW_AT_name);
            fmt::println("  base: {}", baseClassName);
            jClass["baseClass"] = baseClassName;

            dwarf_dealloc(dbg, baseClassDie, DW_DLA_DIE);
            dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
            break;
        }
        case DW_TAG_member:
        {
            std::string fieldName = GetStringAttr(childDie, DW_AT_name);
            int64_t offset = GetUIntAttr(dbg, childDie, DW_AT_data_member_location);

            if (offset == -1)
            {
                // Skip statics
                break;
            }

            fmt::println("  [0x{:04X}] {}", offset, fieldName);
            //PrintDieAttrs(dbg, childDie);
            break;
        }
        }
    });

    fmt::println("}}");

    jClasses[className] = std::move(jClass);
}

template <std::invocable<Dwarf_Die> T>
void ProcessAllDies(Dwarf_Debug dbg, T&& func)
{
    int res;
    Dwarf_Error error;

    while (true)
    {
        Dwarf_Die die = nullptr;
        Dwarf_Unsigned cu_header_length = 0;

        Dwarf_Unsigned abbrev_offset = 0;
        Dwarf_Half     address_size = 0;
        Dwarf_Half     version_stamp = 0;
        Dwarf_Half     offset_size = 0;
        Dwarf_Half     extension_size = 0;
        Dwarf_Sig8     signature;
        Dwarf_Unsigned typeoffset = 0;
        Dwarf_Unsigned next_cu_header = 0;
        Dwarf_Half     header_cu_type = 0;
        Dwarf_Bool     is_info = true;

        res = dwarf_next_cu_header_e(
            dbg,
            is_info,
            &die,
            &cu_header_length,
            &version_stamp,
            &abbrev_offset,
            &address_size,
            &offset_size,
            &extension_size,
            &signature,
            &typeoffset,
            &next_cu_header,
            &header_cu_type,
            &error);

        if (res == DW_DLV_NO_ENTRY)
        {
            // Finished
            break;
        }

        CheckError(res, error);
        RecursiveProcessDie2(dbg, die, func);
        dwarf_dealloc_die(die);
    }
}

std::set<std::string> ReadClassList(const std::string& path)
{
    // Read class list
    fmt::println("Opening class list file {}", path);
    std::ifstream classListFile(path);
    std::set<std::string> classList;
    std::string line;

    while (std::getline(classListFile, line))
    {
        fmt::println("- {}", line);
        classList.insert(line);
    }

    return classList;
}

} // namespace

int main(int argc, char** argv)
{
    po::options_description desc("Extracts offsets from a PDB");
    po::variables_map vm;

    try
    {
        desc.add_options()
            ("help", "produce help message")
            ("class-list", po::value<std::string>()->required(), "list of classes to extract")
            ("so", po::value<std::string>()->required(), "path to the .so")
            ("out", po::value<std::string>()->required(), "path to output JSON");

        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 1;
        }

        po::notify(vm);
    }
    catch (const std::exception& e)
    {
        std::cout << "Error: " << e.what() << "\n";
        std::cout << desc << "\n";
        return 1;
    }

    try
    {
        std::string soFilePath = vm["so"].as<std::string>();
        fmt::println("Opening so file {}", soFilePath);
        Dwarf_Debug dbg = nullptr;
        Dwarf_Error error = 0;
        int res = 0;

        res = dwarf_init_path(
            soFilePath.c_str(),
            nullptr,
            0,
            DW_GROUPNUMBER_ANY,
            nullptr,
            nullptr,
            &dbg,
            &error);

        CheckError(res, error);

        g_ClassList = ReadClassList(vm["class-list"].as<std::string>());

        boost::json::object jRoot;
        boost::json::object jClasses;

        ProcessAllDies(dbg, [&](Dwarf_Die die2) {
            ProcessDie(dbg, die2, jClasses);
        });

        jRoot["classes"] = std::move(jClasses);

        // Save JSON
        std::string outPath = vm["out"].as<std::string>();
        std::ofstream outFile(outPath);
        outFile << jRoot << "\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "Error: " << e.what() << "\n";
        return 1;
    }
}