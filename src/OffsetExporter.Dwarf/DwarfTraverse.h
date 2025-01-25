#pragma once
#include "DwarfCommon.h"

template<typename T>
concept DwarfFunc = std::invocable<T, Dwarf_Die>;

struct LocOperation
{
    Dwarf_Small op = 0;
    Dwarf_Unsigned opd1 = 0;
    Dwarf_Unsigned opd2 = 0;
    Dwarf_Unsigned opd3 = 0;
    Dwarf_Unsigned offsetforbranch = 0;
};

struct LocListEntry
{
    Dwarf_Small loclist_lkind = 0;
    Dwarf_Small lle_value = 0;
    Dwarf_Unsigned rawval1 = 0;
    Dwarf_Unsigned rawval2 = 0;
    Dwarf_Bool debug_addr_unavailable = false;
    Dwarf_Addr lopc = 0;
    Dwarf_Addr hipc = 0;
    Dwarf_Unsigned loclist_expr_op_count = 0;
    Dwarf_Locdesc_c locdesc_entry = 0;
    Dwarf_Unsigned expression_offset = 0;
    Dwarf_Unsigned locdesc_offset = 0;

    int ReadEntry(Dwarf_Loc_Head_c head, int idx, Dwarf_Error* error)
    {
        return dwarf_get_locdesc_entry_d(
            head,
            idx,
            &lle_value,
            &rawval1, &rawval2,
            &debug_addr_unavailable,
            &lopc, &hipc,
            &loclist_expr_op_count,
            &locdesc_entry,
            &loclist_lkind,
            &expression_offset,
            &locdesc_offset,
            error);
    }

    LocOperation GetOperation(int idx) const
    {
        Dwarf_Error error;
        LocOperation op;
        int res = dwarf_get_location_op_value_c(
            locdesc_entry, idx, &op.op,
            &op.opd1, &op.opd2, &op.opd3,
            &op.offsetforbranch,
            &error);
        CheckError(res, error);
        return op;
    }
};

template <DwarfFunc T>
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

template <DwarfFunc T>
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

template <DwarfFunc T>
void RecursiveProcessDie(Dwarf_Debug dbg, Dwarf_Die die, T&& func)
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
            RecursiveProcessDie(dbg, child, func);
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
        RecursiveProcessDie(dbg, die, func);
        dwarf_dealloc_die(die);
    }
}

template <std::invocable<const LocListEntry&> T>
inline void ForEachLocEntry(Dwarf_Attribute attr, T&& func)
{
    int res;
    Dwarf_Error error;

    Dwarf_Unsigned lcount = 0;
    Dwarf_Loc_Head_c loclistHead = 0;
    res = dwarf_get_loclist_c(attr, &loclistHead, &lcount, &error);
    CheckError(res, error);

    for (Dwarf_Unsigned i = 0; i < lcount; i++)
    {
        LocListEntry entry;
        res = entry.ReadEntry(loclistHead, i, &error);
        CheckError(res, error);
        std::invoke(func, entry);
    }

    dwarf_dealloc_loc_head_c(loclistHead);
}