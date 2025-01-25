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

static std::string ConvertTypeToCString(
    Dwarf_Debug dbg,
    Dwarf_Die typeDie,
    std::string_view name)
{
    switch (GetDieTag(typeDie))
    {
    case DW_TAG_base_type:
    case DW_TAG_unspecified_type:
    case DW_TAG_typedef:
    case DW_TAG_structure_type:
    case DW_TAG_union_type:
    case DW_TAG_class_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_template_alias:
        return fmt::format("{} {}", GetStringAttr(typeDie, DW_AT_name), name);
    case DW_TAG_const_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        return ConvertTypeToCString(dbg, utype, fmt::format("const {}", name));
    }
    case DW_TAG_pointer_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        return ConvertTypeToCString(dbg, utype, fmt::format("*{}", name));
    }
    case DW_TAG_reference_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        return ConvertTypeToCString(dbg, utype, fmt::format("&{}", name));
    }
    case DW_TAG_restrict_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        return ConvertTypeToCString(dbg, utype, fmt::format("restrict {}", name));
    }
    case DW_TAG_rvalue_reference_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        return ConvertTypeToCString(dbg, utype, fmt::format("&&{}", name));
    }
    case DW_TAG_volatile_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        return ConvertTypeToCString(dbg, utype, fmt::format("volatile {}", name));
    }
    case DW_TAG_array_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        int64_t size = -1;

        ForEachChild(dbg, typeDie, [&](Dwarf_Die childDie)
        {
            //fmt::println("WTF {}", GetDieTagString(childDie));
            //PrintDieAttrs(dbg, childDie);

            if (GetDieTag(childDie) == DW_TAG_subrange_type)
            {
                size = GetUIntAttr(dbg, childDie, DW_AT_upper_bound);
                return;
            }
        });

        return ConvertTypeToCString(dbg, utype, fmt::format("{}[{}]", name, size));
    }
    case DW_TAG_subroutine_type:
        return fmt::format("__subroutine {}", name);
    case DW_TAG_ptr_to_member_type:
        return fmt::format("__member_func *{}", name);
    default:
        return fmt::format("unk_{} {}", GetDieTagString(typeDie), name);
    }
}

static Dwarf_Die ClearModifiers(
    Dwarf_Debug dbg,
    Dwarf_Die typeDie,
    bool modifiers,
    bool typedefs)
{
    if (modifiers)
    {
        switch (GetDieTag(typeDie))
        {
        case DW_TAG_const_type:
        case DW_TAG_restrict_type:
        case DW_TAG_volatile_type:
        {
            Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
            return ClearModifiers(dbg, utype, modifiers, typedefs);
        }
        }
    }

    if (typedefs)
    {
        switch (GetDieTag(typeDie))
        {
        case DW_TAG_typedef:
        case DW_TAG_template_alias:
        {
            Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
            return ClearModifiers(dbg, utype, modifiers, typedefs);
        }
        }
    }

    return typeDie;
}

static std::optional<int64_t> FindArraySize(
    Dwarf_Debug dbg,
    Dwarf_Die typeDie)
{
    typeDie = ClearModifiers(dbg, typeDie, true, true);

    if (GetDieTag(typeDie) != DW_TAG_array_type)
        return std::nullopt;

    int64_t size = -1;

    ForEachChild(dbg, typeDie, [&](Dwarf_Die childDie)
    {
        if (GetDieTag(childDie) == DW_TAG_subrange_type)
        {
            size = GetUIntAttr(dbg, childDie, DW_AT_upper_bound);

            if (size == -1)
                throw std::runtime_error("DW_AT_upper_bound not set");

            // +1 to convert upper bound to size
            size += 1;
            return;
        }
    });

    return size;
}

static std::string_view ConvertTypeToAmxx(
    Dwarf_Debug dbg,
    Dwarf_Die typeDie,
    std::string_view name,
    std::optional<bool>& outUnsigned)
{
    typeDie = ClearModifiers(dbg, typeDie, true, false);
    Dwarf_Half typeDieTag = GetDieTag(typeDie);

    switch (typeDieTag)
    {
    case DW_TAG_base_type:
    {
        int64_t encoding = GetUIntAttr(dbg, typeDie, DW_AT_encoding);
        int64_t bitSize = GetSizeAttrBits(dbg, typeDie);

        switch (encoding)
        {
        case DW_ATE_signed:
        case DW_ATE_signed_char:
            outUnsigned = false;
            break;
        case DW_ATE_unsigned:
        case DW_ATE_unsigned_char:
            outUnsigned = true;
            break;
        }

        switch (encoding)
        {
        case DW_ATE_boolean: return "character";
        case DW_ATE_address: return "pointer";
        case DW_ATE_signed:
        case DW_ATE_unsigned:
        {
            switch (bitSize)
            {
            case 8: return "character";
            case 16: return "short";
            case 32: return "integer";
            case 64: return "long long";
            default: throw std::logic_error("int size invalid");
            }
        }
        case DW_ATE_signed_char:
        case DW_ATE_unsigned_char:
        case DW_ATE_ASCII:
        case DW_ATE_UCS:
        case DW_ATE_UTF:
            return "character";
        case DW_ATE_float: return "float";
        {
            switch (bitSize)
            {
            case 32: return "float";
            case 64: return "double";
            default: throw std::logic_error("float size invalid");
            }
        }
        default:
            throw std::logic_error("unknown base type");
        }
    }
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    {
        Dwarf_Die utype = ClearModifiers(dbg, FollowReference(dbg, typeDie, DW_AT_type), true, false);

        switch (GetDieTag(utype))
        {
        case DW_TAG_base_type:
        {
            if (GetStringAttr(utype, DW_AT_name) == "char")
                return "stringptr";

            break;
        }
        case DW_TAG_class_type:
        {
            std::string classname = GetStringAttr(utype, DW_AT_name);

            if (classname == "entvars_s")
                return "entvars";
            if (classname == "edict_s")
                return "edict";
            if (classname[0] == 'C')
                return "classptr";

            break;
        }
        case DW_TAG_subroutine_type:
            return "function";
        case DW_TAG_typedef:
        {
            std::string typedefName = GetStringAttr(utype, DW_AT_name);

            if (typedefName == "entvars_t")
                return "entvars";
            if (typedefName == "edict_t")
                return "edict";
        }
        }

        return "pointer";
    }
    case DW_TAG_typedef:
    {
        std::string typeName = GetStringAttr(typeDie, DW_AT_name);

        if (typeName == "string_t")
            return "stringint";

        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        return ConvertTypeToAmxx(dbg, utype, name, outUnsigned);
    }
    case DW_TAG_structure_type:
    case DW_TAG_class_type:
    {
        std::string classname = GetStringAttr(typeDie, DW_AT_name, true);

        if (classname == "Vector")
            return "vector";
        if (classname == "EHANDLE")
            return "ehandle";

        return "structure";
    }
    case DW_TAG_ptr_to_member_type:
        return "function";
    case DW_TAG_array_type:
    {
        Dwarf_Die utype = FollowReference(dbg, typeDie, DW_AT_type);
        std::string utypeName = GetStringAttr(utype, DW_AT_name, true);

        if (utypeName == "char")
            return "string";

        return ConvertTypeToAmxx(dbg, utype, name, outUnsigned);
    }
    case DW_TAG_enumeration_type:
    {
        int64_t bitSize = GetSizeAttrBits(dbg, typeDie);

        switch (bitSize)
        {
        case 8: return "character";
        case 16: return "short";
        case 32: return "integer";
        case 64: return "long long";
        default: throw std::logic_error("enum size invalid");
        }
    }
    default:
        throw std::logic_error("unknown type");
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

    boost::json::array jFields;
    boost::json::array jVTable;

    ForEachChild(dbg, die, [&](Dwarf_Die childDie)
    {
        switch (GetDieTag(childDie))
        {
        case DW_TAG_inheritance:
        {
            Dwarf_Die baseClassDie = FollowReference(dbg, childDie, DW_AT_type);
            std::string baseClassName = GetStringAttr(baseClassDie, DW_AT_name);
            fmt::println("  base: {}", baseClassName);
            jClass["baseClass"] = baseClassName;

            dwarf_dealloc(dbg, baseClassDie, DW_DLA_DIE);
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

            if (HasAttr(dbg, childDie, DW_AT_artificial))
            {
                // Skip compiler-generated
                break;
            }

            Dwarf_Attribute typeAttr;
            res = dwarf_attr(childDie, DW_AT_type, &typeAttr, &error);
            CheckError(res, error);
            Dwarf_Die fieldType = FollowReference(dbg, typeAttr);

            std::optional<uint64_t> arraySize = FindArraySize(dbg, fieldType);
            std::string typeName = ConvertTypeToCString(dbg, fieldType, fieldName);
            std::optional<bool> isUnsigned;
            std::string_view amxxType = ConvertTypeToAmxx(dbg, fieldType, fieldName, isUnsigned);

            boost::json::object jField;
            jField["name"] = fieldName;
            jField["offset"] = offset;
            jField["arraySize"] = arraySize.has_value() ? boost::json::value(*arraySize) : nullptr;
            jField["type"] = typeName;
            jField["amxxType"] = amxxType;
            jField["unsigned"] = isUnsigned.has_value() ? boost::json::value(*isUnsigned) : nullptr;

            // fmt::println("    {}", GetDieTagString(fieldType));

            // PrintDieAttrs(dbg, childDie);

            fmt::println("  [0x{:04X}] {}", offset, typeName);

            jFields.push_back(std::move(jField));
            break;
        }
        case DW_TAG_subprogram:
        {
            if (!HasAttr(dbg, childDie, DW_AT_virtuality))
                break;

            Dwarf_Attribute vtableElemLocation;
            res = dwarf_attr(childDie, DW_AT_vtable_elem_location, &vtableElemLocation, &error);
            CheckError(res, error);

            int vtableIdx = -1;

            ForEachLocEntry(vtableElemLocation, [&](const LocListEntry& entry)
            {
                // Only support single DW_OP_constu
                if (vtableIdx != -1)
                    throw std::runtime_error("Vtable idx already set");

                if (entry.loclist_expr_op_count > 1)
                    throw std::runtime_error("More than one operation");

                for (int i = 0; i < entry.loclist_expr_op_count; i++)
                {
                    LocOperation op = entry.GetOperation(i);

                    if (op.op != DW_OP_constu)
                        throw std::runtime_error("Unknown op");

                    vtableIdx = op.opd1;
                }
            });

            if (vtableIdx == -1)
                throw std::runtime_error("Vtable idx not found");

            std::string methodName = GetStringAttr(childDie, DW_AT_name);
            std::string linkageName = GetStringAttr(childDie, DW_AT_linkage_name);
            // fmt::println("[{}] {} ({})", vtableIdx, methodName, linkageName);

            boost::json::object jMethod;
            jMethod["name"] = methodName;
            jMethod["linkName"] = linkageName;
            jMethod["index"] = vtableIdx;
            jVTable.push_back(std::move(jMethod));
        }
        }
    });

    fmt::println("}}");

    jClass["fields"] = std::move(jFields);
    jClass["vtable"] = std::move(jVTable);
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