#pragma once
#include "DwarfCommon.h"

inline void PrintDieAttrs(Dwarf_Debug dbg, Dwarf_Die die)
{
    int res;
    Dwarf_Error error;
    Dwarf_Attribute* attrs;
    Dwarf_Signed attrNum;
    res = dwarf_attrlist(die, &attrs, &attrNum, &error);

    if (res == DW_DLV_NO_ENTRY)
        return;

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

inline Dwarf_Half GetDieTag(Dwarf_Die die)
{
    int res;
    Dwarf_Error error;

    Dwarf_Half dieTag;
    res = dwarf_tag(die, &dieTag, &error);
    CheckError(res, error);

    return dieTag;
}

inline const char* GetDieTagString(Dwarf_Die die)
{
    Dwarf_Half tag = GetDieTag(die);

    const char* name;
    dwarf_get_TAG_name(tag, &name);
    return name;
}

inline bool HasAttr(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attrNum)
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

inline std::string GetStringAttr(Dwarf_Die die, Dwarf_Half attrNum, bool allowOptional = false)
{
    int res;
    Dwarf_Error error;
    char* buf = nullptr;

    res = dwarf_die_text(die, attrNum, &buf, &error);

    if (allowOptional && res == DW_DLV_NO_ENTRY)
        return std::string();

    CheckError(res, error);
    return buf;
}

inline int64_t GetUIntAttr(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attrNum, int64_t def = -1)
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

inline int64_t GetSizeAttrBits(Dwarf_Debug dbg, Dwarf_Die die, int64_t def = -1)
{
    if (HasAttr(dbg, die, DW_AT_byte_size))
        return GetUIntAttr(dbg, die, DW_AT_byte_size, -1) * 8;
    else if (HasAttr(dbg, die, DW_AT_bit_size))
        return GetUIntAttr(dbg, die, DW_AT_bit_size, -1);
    else
        return def;
}

inline Dwarf_Die FollowReference(Dwarf_Debug dbg, Dwarf_Attribute attr)
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

inline Dwarf_Die FollowReference(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attrNum)
{
    int res;
    Dwarf_Error error;

    Dwarf_Attribute attr;
    res = dwarf_attr(die, attrNum, &attr, &error);
    CheckError(res, error);

    Dwarf_Die ref = FollowReference(dbg, attr);

    dwarf_dealloc(dbg, attr, DW_DLA_ATTR);

    return ref;
}
