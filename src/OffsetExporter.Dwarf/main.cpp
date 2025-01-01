#include <boost/json.hpp>
#include <boost/program_options.hpp>
#include "DwarfAttributes.h"
#include "DwarfCommon.h"
#include "DwarfTraverse.h"

namespace po = boost::program_options;

namespace
{

std::set<std::string> g_ClassList;
std::set<std::string> g_ProcessedClasses;

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