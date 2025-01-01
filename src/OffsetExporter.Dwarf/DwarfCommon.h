#pragma once

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

