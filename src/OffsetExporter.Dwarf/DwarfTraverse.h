#pragma once
#include "DwarfCommon.h"

template<typename T>
concept DwarfFunc = std::invocable<T, Dwarf_Die>;

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
