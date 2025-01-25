#include <boost/json.hpp>
#include <boost/program_options.hpp>
#include "MemoryMappedFile.h"
#include "TypeTable.h"

namespace po = boost::program_options;

namespace
{

bool IsError(PDB::ErrorCode errorCode)
{
    switch (errorCode)
    {
    case PDB::ErrorCode::Success:
        return false;

    case PDB::ErrorCode::InvalidSuperBlock:
        printf("Invalid Superblock\n");
        return true;

    case PDB::ErrorCode::InvalidFreeBlockMap:
        printf("Invalid free block map\n");
        return true;

    case PDB::ErrorCode::InvalidStream:
        printf("Invalid stream\n");
        return true;

    case PDB::ErrorCode::InvalidSignature:
        printf("Invalid stream signature\n");
        return true;

    case PDB::ErrorCode::InvalidStreamIndex:
        printf("Invalid stream index\n");
        return true;

    case PDB::ErrorCode::UnknownVersion:
        printf("Unknown version\n");
        return true;
    }

    // only ErrorCode::Success means there wasn't an error, so all other paths have to assume there was an error
    return true;
}

bool HasValidDBIStreams(const PDB::RawFile& rawPdbFile, const PDB::DBIStream& dbiStream)
{
    // check whether the DBI stream offers all sub-streams we need
    if (IsError(dbiStream.HasValidSymbolRecordStream(rawPdbFile)))
    {
        return false;
    }

    if (IsError(dbiStream.HasValidPublicSymbolStream(rawPdbFile)))
    {
        return false;
    }

    if (IsError(dbiStream.HasValidGlobalSymbolStream(rawPdbFile)))
    {
        return false;
    }

    if (IsError(dbiStream.HasValidSectionContributionStream(rawPdbFile)))
    {
        return false;
    }

    if (IsError(dbiStream.HasValidImageSectionStream(rawPdbFile)))
    {
        return false;
    }

    return true;
}

uint8_t GetLeafSize(PDB::CodeView::TPI::TypeRecordKind kind)
{
    if (kind < PDB::CodeView::TPI::TypeRecordKind::LF_NUMERIC)
    {
        // No leaf can have an index less than LF_NUMERIC (0x8000)
        // so word is the value...
        return sizeof(PDB::CodeView::TPI::TypeRecordKind);
    }

    switch (kind)
    {
    case PDB::CodeView::TPI::TypeRecordKind::LF_CHAR:
        return sizeof(PDB::CodeView::TPI::TypeRecordKind) + sizeof(uint8_t);

    case PDB::CodeView::TPI::TypeRecordKind::LF_USHORT:
    case PDB::CodeView::TPI::TypeRecordKind::LF_SHORT:
        return sizeof(PDB::CodeView::TPI::TypeRecordKind) + sizeof(uint16_t);

    case PDB::CodeView::TPI::TypeRecordKind::LF_LONG:
    case PDB::CodeView::TPI::TypeRecordKind::LF_ULONG:
        return sizeof(PDB::CodeView::TPI::TypeRecordKind) + sizeof(uint32_t);

    case PDB::CodeView::TPI::TypeRecordKind::LF_QUADWORD:
    case PDB::CodeView::TPI::TypeRecordKind::LF_UQUADWORD:
        return sizeof(PDB::CodeView::TPI::TypeRecordKind) + sizeof(uint64_t);

    default:
        printf("Error! 0x%04x bogus type encountered, aborting...\n", PDB_AS_UNDERLYING(kind));
    }
    return 0;
}

template <typename T>
T UnalignedRead(const char* data)
{
	T value = {};
	memcpy(&value, data, sizeof(T));
	return value;
}

uint64_t ReadUIntLeaf(const char* data, PDB::CodeView::TPI::TypeRecordKind kind)
{
	const char* leafData = data + sizeof(PDB::CodeView::TPI::TypeRecordKind);

	switch (kind)
	{
	case PDB::CodeView::TPI::TypeRecordKind::LF_CHAR:
		return UnalignedRead<uint8_t>(leafData);

	case PDB::CodeView::TPI::TypeRecordKind::LF_USHORT:
	case PDB::CodeView::TPI::TypeRecordKind::LF_SHORT:
		return UnalignedRead<uint16_t>(leafData);

	case PDB::CodeView::TPI::TypeRecordKind::LF_LONG:
	case PDB::CodeView::TPI::TypeRecordKind::LF_ULONG:
		return UnalignedRead<uint32_t>(leafData);

	case PDB::CodeView::TPI::TypeRecordKind::LF_QUADWORD:
	case PDB::CodeView::TPI::TypeRecordKind::LF_UQUADWORD:
		return UnalignedRead<uint64_t>(leafData);

	default:
		printf("Error! 0x%04x bogus type encountered, aborting...\n", PDB_AS_UNDERLYING(kind));
	}

	return 0;
}

uint64_t ReadSizeLeaf(const char* data)
{
	auto kind = UnalignedRead<PDB::CodeView::TPI::TypeRecordKind>(data);

	if (kind < PDB::CodeView::TPI::TypeRecordKind::LF_NUMERIC)
	{
		// Kind itself is the size
		return (uint64_t)kind;
	}

	return ReadUIntLeaf(data, kind);
}

const char* GetLeafName(const char* data, PDB::CodeView::TPI::TypeRecordKind kind)
{
    return &data[GetLeafSize(kind)];
}

static std::string GetModifierName(const PDB::CodeView::TPI::Record* modifierRecord)
{
	std::string result;

	if (modifierRecord->data.LF_MODIFIER.attr.MOD_const)
		result += "const ";
	if (modifierRecord->data.LF_MODIFIER.attr.MOD_volatile)
		result += "volatile";
	if (modifierRecord->data.LF_MODIFIER.attr.MOD_unaligned)
		result += "unaligned";

	return result;
}

uint32_t ResolveTypes(const TypeTable& typeTable, uint32_t typeIndex,
	bool resolveModifiers = false,
	bool resolvePointers = false,
	bool resolveArrays = false)
{
	while (true)
	{
		auto typeRecord = typeTable.GetTypeRecord(typeIndex);

		if (!typeRecord)
			break;

		if (resolveModifiers && typeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_MODIFIER)
		{
			typeIndex = typeRecord->data.LF_MODIFIER.type;
		}
		else if (resolvePointers && typeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_POINTER)
		{
			typeIndex = typeRecord->data.LF_POINTER.utype;
		}
		else if (resolveArrays && typeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_ARRAY)
		{
			typeIndex = typeRecord->data.LF_ARRAY.elemtype;
		}
		else
		{
			break;
		}
	}

	return typeIndex;
}

uint32_t ResolveFwdRef(const TypeTable& typeTable, uint32_t typeIndex)
{
	auto record = typeTable.GetTypeRecord(typeIndex);

	if (!record)
		return typeIndex;

	if (record->header.kind != PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE &&
		record->header.kind != PDB::CodeView::TPI::TypeRecordKind::LF_CLASS)
		return typeIndex;

	auto leafName = GetLeafName(record->data.LF_CLASS.data, record->data.LF_CLASS.lfEasy.kind);

	for (uint32_t i = typeTable.GetFirstTypeIndex(); i <= typeTable.GetLastTypeIndex(); i++)
	{
		auto record2 = typeTable.GetTypeRecord(i);

		if (record2->header.kind != PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE &&
			record2->header.kind != PDB::CodeView::TPI::TypeRecordKind::LF_CLASS)
			continue;

		if (record2->data.LF_CLASS.property.fwdref)
			continue;

		auto leafName2 = GetLeafName(record2->data.LF_CLASS.data, record2->data.LF_CLASS.lfEasy.kind);

		if (!strcmp(leafName, leafName2))
			return i;
	}

	return typeIndex;
}

static const char* GetTypeName(const TypeTable& typeTable, uint32_t typeIndex, uint8_t& pointerLevel, const PDB::CodeView::TPI::Record** referencedType, const PDB::CodeView::TPI::Record** modifierRecord)
{
	const char* typeName = nullptr;
	const PDB::CodeView::TPI::Record* underlyingType = nullptr;

	if (referencedType)
		*referencedType = nullptr;

	if (modifierRecord)
		*modifierRecord = nullptr;

	auto typeIndexBegin = typeTable.GetFirstTypeIndex();
	if (typeIndex < typeIndexBegin)
	{
		auto type = static_cast<PDB::CodeView::TPI::TypeIndexKind>(typeIndex);
		switch (type)
		{
		case PDB::CodeView::TPI::TypeIndexKind::T_NOTYPE:
			return "<NO TYPE>";
		case PDB::CodeView::TPI::TypeIndexKind::T_HRESULT:
			return "HRESULT";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PHRESULT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PHRESULT:
			return "PHRESULT";

		case PDB::CodeView::TPI::TypeIndexKind::T_UNKNOWN_0600:
			return "UNKNOWN_0x0600";

		case PDB::CodeView::TPI::TypeIndexKind::T_VOID:
			return "void";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_PVOID:
			return "PVOID";

		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL64:
			return "PBOOL";

		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL32:
			return "BOOL";

		case PDB::CodeView::TPI::TypeIndexKind::T_RCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR:
			return "CHAR";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR:
			return "PCHAR";

		case PDB::CodeView::TPI::TypeIndexKind::T_UCHAR:
			return "UCHAR";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUCHAR:
			return "PUCHAR";

		case PDB::CodeView::TPI::TypeIndexKind::T_WCHAR:
			return "WCHAR";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PWCHAR:
			return "PWCHAR";

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR8:
			return "CHAR8";
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR8:
			return "PCHAR8";

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR16:
			return "CHAR16";
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR16:
			return "PCHAR16";

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR32:
			return "CHAR32";
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR32:
			return "PCHAR32";

		case PDB::CodeView::TPI::TypeIndexKind::T_SHORT:
			return "SHORT";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PSHORT:
			return "PSHORT";
		case PDB::CodeView::TPI::TypeIndexKind::T_USHORT:
			return "USHORT";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUSHORT:
			return "PUSHORT";
		case PDB::CodeView::TPI::TypeIndexKind::T_LONG:
			return "LONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PLONG:
			return "PLONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_ULONG:
			return "ULONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PULONG:
			return "PULONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL32:
			return "FLOAT";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL32:
			return "PFLOAT";
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL64:
			return "DOUBLE";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL64:
			return "PDOUBLE";
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL80:
			return "REAL80";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL80:
			return "PREAL80";
		case PDB::CodeView::TPI::TypeIndexKind::T_QUAD:
			return "LONGLONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PQUAD:
			return "PLONGLONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_UQUAD:
			return "ULONGLONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUQUAD:
			return "PULONGLONG";
		case PDB::CodeView::TPI::TypeIndexKind::T_INT4:
			return "INT";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT4:
			return "PINT";
		case PDB::CodeView::TPI::TypeIndexKind::T_UINT4:
			return "UINT";
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT4:
			return "PUINT";

		case PDB::CodeView::TPI::TypeIndexKind::T_UINT8:
			return "UINT8";
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT8:
			return "PUINT8";

		case PDB::CodeView::TPI::TypeIndexKind::T_INT8:
			return "INT8";
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT8:
			return "PINT8";

		case PDB::CodeView::TPI::TypeIndexKind::T_OCT:
			return "OCTAL";

		case PDB::CodeView::TPI::TypeIndexKind::T_POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64POCT:
			return "POCTAL";

		case PDB::CodeView::TPI::TypeIndexKind::T_UOCT:
			return "UOCTAL";

		case PDB::CodeView::TPI::TypeIndexKind::T_PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUOCT:
			return "PUOCTAL";

		default:
			PDB_ASSERT(false, "Unhandled special type 0x%X", typeIndex);
			return "unhandled_special_type";
		}
	}
	else
	{
		auto typeRecord = typeTable.GetTypeRecord(typeIndex);
		if (!typeRecord)
			return nullptr;

		switch (typeRecord->header.kind)
		{
		case PDB::CodeView::TPI::TypeRecordKind::LF_MODIFIER:
			if (modifierRecord)
				*modifierRecord = typeRecord;
			return GetTypeName(typeTable, typeRecord->data.LF_MODIFIER.type, pointerLevel, referencedType, nullptr);
		case PDB::CodeView::TPI::TypeRecordKind::LF_POINTER:
			++pointerLevel;
			if (referencedType)
				*referencedType = typeRecord;
			if (typeRecord->data.LF_POINTER.utype >= typeIndexBegin)
			{
				underlyingType = typeTable.GetTypeRecord(typeRecord->data.LF_POINTER.utype);
				if (!underlyingType)
					return nullptr;

				if (underlyingType->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_POINTER)
					return GetTypeName(typeTable, typeRecord->data.LF_POINTER.utype, pointerLevel, referencedType, modifierRecord);

				// Type record order can be LF_POINTER -> LF_MODIFIER -> LF_POINTER -> ...
				if (underlyingType->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_MODIFIER)
				{
					if (modifierRecord)
						*modifierRecord = underlyingType;

					return GetTypeName(typeTable, underlyingType->data.LF_MODIFIER.type, pointerLevel, referencedType, nullptr);
				}
			}

			return GetTypeName(typeTable, typeRecord->data.LF_POINTER.utype, pointerLevel, &typeRecord, modifierRecord);
		case PDB::CodeView::TPI::TypeRecordKind::LF_PROCEDURE:
			if (referencedType)
				*referencedType = typeRecord;
			return nullptr;
		case PDB::CodeView::TPI::TypeRecordKind::LF_BITFIELD:
			if (typeRecord->data.LF_BITFIELD.type < typeIndexBegin)
			{
				typeName = GetTypeName(typeTable, typeRecord->data.LF_BITFIELD.type, pointerLevel, nullptr, modifierRecord);
				if (referencedType)
					*referencedType = typeRecord;
				return typeName;
			}
			else
			{
				if (referencedType)
					*referencedType = typeRecord;
				return nullptr;
			}
		case PDB::CodeView::TPI::TypeRecordKind::LF_ARRAY:
			if (referencedType)
				*referencedType = typeRecord;
			return GetTypeName(typeTable, typeRecord->data.LF_ARRAY.elemtype, pointerLevel, &typeRecord, modifierRecord);
		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE:
			return GetLeafName(typeRecord->data.LF_CLASS.data, typeRecord->header.kind);

		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS2:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE2:
			return GetLeafName(typeRecord->data.LF_CLASS2.data, typeRecord->header.kind);

		case  PDB::CodeView::TPI::TypeRecordKind::LF_UNION:
			return GetLeafName(typeRecord->data.LF_UNION.data, typeRecord->header.kind);
		case PDB::CodeView::TPI::TypeRecordKind::LF_ENUM:
			return &typeRecord->data.LF_ENUM.name[0];
		case PDB::CodeView::TPI::TypeRecordKind::LF_MFUNCTION:
			if (referencedType)
				*referencedType = typeRecord;
			return nullptr;

		default:
			PDB_ASSERT(false, "Unhandled TypeRecordKind 0x%X", typeRecord->header.kind);
			break;
		}

	}

	return "unknown_type";
}

static uint64_t GetTypeSize(
	const TypeTable& typeTable,
	uint32_t typeIndex)
{
	auto typeIndexBegin = typeTable.GetFirstTypeIndex();
	if (typeIndex < typeIndexBegin)
	{
		auto type = static_cast<PDB::CodeView::TPI::TypeIndexKind>(typeIndex);
		switch (type)
		{
		case PDB::CodeView::TPI::TypeIndexKind::T_NOTYPE:
			return 0;
		case PDB::CodeView::TPI::TypeIndexKind::T_HRESULT:
			return 4;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PHRESULT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUOCT:
			return 4;
		case PDB::CodeView::TPI::TypeIndexKind::T_64PHRESULT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUOCT:
			return 8;

		case PDB::CodeView::TPI::TypeIndexKind::T_PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUOCT:
			return 4;

		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL08:
			return 1;
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL16:
			return 2;
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL32:
			return 4;

		case PDB::CodeView::TPI::TypeIndexKind::T_RCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_UCHAR:

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR8:
			return 1;

		case PDB::CodeView::TPI::TypeIndexKind::T_WCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR16:
			return 2;

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR32:
			return 4;

		case PDB::CodeView::TPI::TypeIndexKind::T_SHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_USHORT:
			return 2;
		case PDB::CodeView::TPI::TypeIndexKind::T_LONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_ULONG:
			return 4;
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL32:
			return 4;
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL64:
			return 8;
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL80:
			return 16;
		case PDB::CodeView::TPI::TypeIndexKind::T_QUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_UQUAD:
			return 8;

		case PDB::CodeView::TPI::TypeIndexKind::T_INT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_UINT4:
			return 4;

		case PDB::CodeView::TPI::TypeIndexKind::T_UINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_INT8:
			return 8;

		//case PDB::CodeView::TPI::TypeIndexKind::T_OCT:
		//case PDB::CodeView::TPI::TypeIndexKind::T_UOCT:

		default:
			PDB_ASSERT(false, "Unhandled special type 0x%X", typeIndex);
			return 0;
		}
	}
	else
	{
		auto typeRecord = typeTable.GetTypeRecord(typeIndex);
		if (!typeRecord)
			return 0;

		switch (typeRecord->header.kind)
		{
		case PDB::CodeView::TPI::TypeRecordKind::LF_MODIFIER:
		{
			return GetTypeSize(typeTable, typeRecord->data.LF_MODIFIER.type);
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_POINTER:
		{
			// return GetTypeSize(typeTable, typeRecord->data.LF_POINTER.pbase.btype.index);
			constexpr int CV_PTR_NEAR32 = 0x0a;
			constexpr int CV_PTR_FAR32 = 0x0b;
			constexpr int CV_PTR_64 = 0x0c;
			switch (typeRecord->data.LF_POINTER.attr.ptrtype)
			{
			case CV_PTR_NEAR32:
			case CV_PTR_FAR32:
				return 4;
			case CV_PTR_64:
				return 8;
			default:
				return 0;
			}
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_PROCEDURE:
		case PDB::CodeView::TPI::TypeRecordKind::LF_BITFIELD:
			// TODO??
			return 0;
		case PDB::CodeView::TPI::TypeRecordKind::LF_ARRAY:
		{
			uint64_t arraySizeInBytes = ReadSizeLeaf(typeRecord->data.LF_ARRAY.data);
			return arraySizeInBytes;
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE:
		{
			uint32_t resolvedType = ResolveFwdRef(typeTable, typeIndex);
			auto resolvedRecord = typeTable.GetTypeRecord(resolvedType);

			return ReadSizeLeaf(resolvedRecord->data.LF_CLASS.data);
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS2:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE2:
		{
			return ReadUIntLeaf(typeRecord->data.LF_CLASS2.data, typeRecord->data.LF_CLASS.lfEasy.kind);
		}

		case PDB::CodeView::TPI::TypeRecordKind::LF_UNION:
		{
			// TODO
			return 0;
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_ENUM:
		{
			// TODO
			return 0;
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_MFUNCTION:
		{
			// TODO
			return 0;
		}

		default:
			PDB_ASSERT(false, "Unhandled TypeRecordKind 0x%X", typeRecord->header.kind);
			break;
		}
	}

	return 0;
}

static const char* ConvertTypeToAmxx(
	const TypeTable& typeTable,
	uint32_t typeIndex)
{
	auto typeIndexBegin = typeTable.GetFirstTypeIndex();
	if (typeIndex < typeIndexBegin)
	{
		auto type = static_cast<PDB::CodeView::TPI::TypeIndexKind>(typeIndex);
		switch (type)
		{
		case PDB::CodeView::TPI::TypeIndexKind::T_32PHRESULT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PHRESULT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUOCT:
			return "pointer";

		case PDB::CodeView::TPI::TypeIndexKind::T_NOTYPE:
			return "<NO TYPE>";
		case PDB::CodeView::TPI::TypeIndexKind::T_HRESULT:
			return "integer";

		case PDB::CodeView::TPI::TypeIndexKind::T_VOID:
			return "void";

		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL08:
			return "character";
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL16:
			return "short";
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL32:
			return "integer";

		case PDB::CodeView::TPI::TypeIndexKind::T_RCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_UCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR8:
			return "character";

		case PDB::CodeView::TPI::TypeIndexKind::T_WCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR16:
			return "short";

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR32:
			return "integer";

		case PDB::CodeView::TPI::TypeIndexKind::T_SHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_USHORT:
			return "short";

		case PDB::CodeView::TPI::TypeIndexKind::T_LONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_ULONG:
			return "integer";

		case PDB::CodeView::TPI::TypeIndexKind::T_REAL32:
			return "float";
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL64:
			return "double";
		case PDB::CodeView::TPI::TypeIndexKind::T_QUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_UQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_INT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_UINT8:
			return "long long";

		case PDB::CodeView::TPI::TypeIndexKind::T_INT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_UINT4:
			return "integer";

		default:
			PDB_ASSERT(false, "Unhandled special type 0x%X", typeIndex);
			return "unhandled_special_type";
		}
	}
	else
	{
		auto typeRecord = typeTable.GetTypeRecord(typeIndex);
		if (!typeRecord)
			return "";

		switch (typeRecord->header.kind)
		{
		case PDB::CodeView::TPI::TypeRecordKind::LF_MODIFIER:
		{
			return ConvertTypeToAmxx(typeTable, typeRecord->data.LF_MODIFIER.type);
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_POINTER:
		{
			uint32_t resolvedType = ResolveTypes(typeTable, typeRecord->data.LF_POINTER.utype, true);

			if (resolvedType != typeIndex)
			{
				auto resTypeRecord = typeTable.GetTypeRecord(resolvedType);

				if (static_cast<PDB::CodeView::TPI::TypeIndexKind>(resolvedType) == PDB::CodeView::TPI::TypeIndexKind::T_RCHAR)
				{
					return "stringptr";
				}
				else if (resTypeRecord)
				{
					if (resTypeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_CLASS ||
						resTypeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_CLASS2 ||
						resTypeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE ||
						resTypeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE2)
					{
						const char* className = GetLeafName(resTypeRecord->data.LF_CLASS.data, resTypeRecord->data.LF_CLASS.lfEasy.kind);

						if (!strcmp(className, "entvars_s"))
							return "entvars";
						if (!strcmp(className, "edict_s"))
							return "edict";

						if (className[0] == 'C')
							return "classptr";
					}
					else if (resTypeRecord->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_MFUNCTION)
					{
						return "function";
					}
				}
			}

			return "pointer";
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_ARRAY:
		{
			uint32_t resolvedType = ResolveTypes(typeTable, typeRecord->data.LF_ARRAY.elemtype, true, false, false);

			if (resolvedType < typeIndexBegin &&
				static_cast<PDB::CodeView::TPI::TypeIndexKind>(resolvedType) == PDB::CodeView::TPI::TypeIndexKind::T_RCHAR)
				return "string";

			return ConvertTypeToAmxx(typeTable, typeRecord->data.LF_ARRAY.elemtype);
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE:
		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS2:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE2:
		case PDB::CodeView::TPI::TypeRecordKind::LF_UNION:
		{
			const char* className = GetLeafName(typeRecord->data.LF_CLASS.data, typeRecord->data.LF_CLASS.lfEasy.kind);
			if (!strcmp(className, "Vector"))
				return "vector";
			if (!strcmp(className, "EHANDLE"))
				return "ehandle";
			return "structure";
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_ENUM:
			return ConvertTypeToAmxx(typeTable, typeRecord->data.LF_ENUM.utype);
		case PDB::CodeView::TPI::TypeRecordKind::LF_MFUNCTION:
		{
			return "function";
		}

		default:
			PDB_ASSERT(false, "Unhandled TypeRecordKind 0x%X", typeRecord->header.kind);
			break;
		}
	}

	return "unknown_type";
}

static std::string ConvertTypeToCString(
	std::string_view fieldName,
	const TypeTable& typeTable,
	uint32_t typeIndex,
	uint64_t* outArraySize)
{
	auto typeIndexBegin = typeTable.GetFirstTypeIndex();
	if (typeIndex < typeIndexBegin)
	{
		std::string result;
		auto type = static_cast<PDB::CodeView::TPI::TypeIndexKind>(typeIndex);
		switch (type)
		{
		case PDB::CodeView::TPI::TypeIndexKind::T_NOTYPE:
			result = "<NO TYPE>"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_HRESULT:
			result = "HRESULT"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PHRESULT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PHRESULT:
			result = "PHRESULT"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_UNKNOWN_0600:
			result = "UNKNOWN_0x0600"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_VOID:
			result = "void"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PVOID:
		case PDB::CodeView::TPI::TypeIndexKind::T_PVOID:
			result = "void*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PBOOL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL08:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PBOOL64:
			result = "BOOL*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL08:
			result = "bool"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL16:
			result = "BOOL16"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_BOOL32:
			result = "BOOL"; break; // Win32 BOOL == int

		case PDB::CodeView::TPI::TypeIndexKind::T_RCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR:
			result = "char"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PRCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR:
			result = "char*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_UCHAR:
			result = "byte"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUCHAR:
			result = "byte*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_WCHAR:
			result = "wchar_t"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PWCHAR:
		case PDB::CodeView::TPI::TypeIndexKind::T_PWCHAR:
			result = "wchar_t*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR8:
			result = "CHAR8"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFCHAR8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR8:
			result = "CHAR8*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR16:
			result = "CHAR16"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR16:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR16:
			result = "CHAR16*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_CHAR32:
			result = "CHAR32"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PCHAR32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PCHAR32:
			result = "CHAR32*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_SHORT:
			result = "short"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PSHORT:
			result = "short*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_USHORT:
			result = "unsigned short"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUSHORT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUSHORT:
			result = "unsigned short*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_LONG:
			result = "long"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PLONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PLONG:
			result = "long*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_ULONG:
			result = "unsigned long"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PULONG:
		case PDB::CodeView::TPI::TypeIndexKind::T_PULONG:
			result = "unsigned long*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL32:
			result = "float"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL32:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL32:
			result = "float*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL64:
			result = "double"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL64:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL64:
			result = "double*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_REAL80:
			result = "REAL80"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PREAL80:
		case PDB::CodeView::TPI::TypeIndexKind::T_PREAL80:
			result = "PREAL80"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_QUAD:
			result = "int64_t"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PQUAD:
			result = "int64_t*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_UQUAD:
			result = "uint64_t"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUQUAD:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUQUAD:
			result = "uint64_t*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_INT4:
			result = "int"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT4:
			result = "int*"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_UINT4:
			result = "unsigned"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT4:
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT4:
			result = "unsigned*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_UINT8:
			result = "uint64_t"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUINT8:
			result = "uint64_t*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_INT8:
			result = "uint64_t"; break;
		case PDB::CodeView::TPI::TypeIndexKind::T_PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFINT8:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PINT8:
			result = "uint64_t*"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_OCT:
			result = "OCTAL"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32POCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64POCT:
			result = "POCTAL"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_UOCT:
			result = "UOCTAL"; break;

		case PDB::CodeView::TPI::TypeIndexKind::T_PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PFUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_PHUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_32PFUOCT:
		case PDB::CodeView::TPI::TypeIndexKind::T_64PUOCT:
			result = "PUOCTAL"; break;

		default:
			PDB_ASSERT(false, "Unhandled special type 0x%X", typeIndex);
			result = "unhandled_special_type"; break;
		}

		if (!fieldName.empty())
			return fmt::format("{} {}", result, fieldName);
		else
			return result;
	}
	else
	{
		auto typeRecord = typeTable.GetTypeRecord(typeIndex);
		if (!typeRecord)
			return "";

		switch (typeRecord->header.kind)
		{
		case PDB::CodeView::TPI::TypeRecordKind::LF_MODIFIER:
		{
			std::string modifiers = GetModifierName(typeRecord);
			return ConvertTypeToCString(modifiers + std::string(fieldName), typeTable, typeRecord->data.LF_MODIFIER.type, outArraySize);
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_POINTER:
		{
			std::string pointerMods;

			if (typeRecord->data.LF_POINTER.attr.isconst)
				pointerMods += "const ";
			if (typeRecord->data.LF_POINTER.attr.isvolatile)
				pointerMods += "volatile ";
			if (typeRecord->data.LF_POINTER.attr.isunaligned)
				pointerMods += "unaligned ";

			return ConvertTypeToCString(fmt::format("*{}{}", pointerMods, fieldName), typeTable, typeRecord->data.LF_POINTER.utype, nullptr);
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_PROCEDURE:
			// TODO??
			return fmt::format("LF_PROCEDURE {}", fieldName);
		case PDB::CodeView::TPI::TypeRecordKind::LF_BITFIELD:
			// TODO??
			return fmt::format("LF_BITFIELD {}", fieldName);
		case PDB::CodeView::TPI::TypeRecordKind::LF_ARRAY:
		{
			// TODO 2024-11-10: Can be larger than uint16_t
			uint64_t arraySizeInBytes = GetTypeSize(typeTable, typeIndex);

			if (arraySizeInBytes != 0)
			{
				uint64_t elemSizeInBytes = GetTypeSize(typeTable, typeRecord->data.LF_ARRAY.elemtype);
				uint64_t elemCount = arraySizeInBytes / elemSizeInBytes;
				if (outArraySize)
					*outArraySize = elemCount;
				return ConvertTypeToCString(fmt::format("{}[{}]", fieldName, elemCount), typeTable, typeRecord->data.LF_ARRAY.elemtype, nullptr);
			}
			else
			{
				if (outArraySize)
					*outArraySize = 0;
				return ConvertTypeToCString(fmt::format("{}[]", fieldName), typeTable, typeRecord->data.LF_ARRAY.elemtype, nullptr);
			}
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE:
		{
			const char* className = GetLeafName(typeRecord->data.LF_CLASS.data, typeRecord->header.kind);

			if (!fieldName.empty())
				return fmt::format("{} {}", className, fieldName);
			else
				return className;
		}
		case PDB::CodeView::TPI::TypeRecordKind::LF_CLASS2:
		case PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE2:
		{
			const char* className = GetLeafName(typeRecord->data.LF_CLASS2.data, typeRecord->header.kind);

			if (!fieldName.empty())
				return fmt::format("{} {}", className, fieldName);
			else
				return className;
		}

		case PDB::CodeView::TPI::TypeRecordKind::LF_UNION:
			return fmt::format("{} {}", GetLeafName(typeRecord->data.LF_UNION.data, typeRecord->header.kind), fieldName);
		case PDB::CodeView::TPI::TypeRecordKind::LF_ENUM:
			return fmt::format("{} {}", &typeRecord->data.LF_ENUM.name[0], fieldName);
		case PDB::CodeView::TPI::TypeRecordKind::LF_MFUNCTION:
		{
			std::string result;
			std::string returnType = ConvertTypeToCString(std::string_view(), typeTable, typeRecord->data.LF_MFUNCTION.rvtype, nullptr);
			result = fmt::format("{} ({})(", returnType, fieldName);

			auto argList = typeTable.GetTypeRecord(typeRecord->data.LF_MFUNCTION.arglist);
			
			for (uint32_t argIdx = 0; argIdx < argList->data.LF_ARGLIST.count; argIdx++)
			{
				uint32_t argTypeIdx = argList->data.LF_ARGLIST.arg[argIdx];
				
				if (argIdx != 0)
					result += ", ";

				result += ConvertTypeToCString(std::string_view(), typeTable, argTypeIdx, nullptr);
			}

			result += ")";
			return result;
		}

		default:
			PDB_ASSERT(false, "Unhandled TypeRecordKind 0x%X", typeRecord->header.kind);
			break;
		}
	}

	return "unknown_type";
}

const char* GetMethodName(const PDB::CodeView::TPI::FieldList* fieldRecord)
{
	auto methodAttributes = static_cast<PDB::CodeView::TPI::MethodProperty>(fieldRecord->data.LF_ONEMETHOD.attributes.mprop);
	switch (methodAttributes)
	{
	case PDB::CodeView::TPI::MethodProperty::Intro:
	case PDB::CodeView::TPI::MethodProperty::PureIntro:
		return &reinterpret_cast<const char*>(fieldRecord->data.LF_ONEMETHOD.vbaseoff)[sizeof(uint32_t)];
	default:
		break;
	}

	return  &reinterpret_cast<const char*>(fieldRecord->data.LF_ONEMETHOD.vbaseoff)[0];
}

void DisplayFields(const TypeTable& typeTable, const PDB::CodeView::TPI::Record* record, boost::json::object& jClass)
{
	const char* leafName = nullptr;

	auto maximumSize = record->header.size - sizeof(uint16_t);

	boost::json::array jFields;
	boost::json::array jVTable;

	for (size_t i = 0; i < maximumSize;)
	{
		uint8_t pointerLevel = 0;
		auto fieldRecord = reinterpret_cast<const PDB::CodeView::TPI::FieldList*>(reinterpret_cast<const uint8_t*>(&record->data.LF_FIELD.list) + i);

		// Other kinds of records are not implemented
		PDB_ASSERT(
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_BCLASS ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_VBCLASS ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_IVBCLASS ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_INDEX ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_VFUNCTAB ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_NESTTYPE ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_ENUM ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_MEMBER ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_STMEMBER ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_METHOD ||
			fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_ONEMETHOD,
			"Unknown record kind %X",
			static_cast<unsigned int>(fieldRecord->kind));

		if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_MEMBER)
		{
			uint64_t offset = ReadSizeLeaf(fieldRecord->data.LF_MEMBER.offset);

			uint64_t arraySize = 0;
			leafName = GetLeafName(fieldRecord->data.LF_MEMBER.offset, fieldRecord->data.LF_MEMBER.lfEasy.kind);
			std::string typeName = ConvertTypeToCString(leafName, typeTable, fieldRecord->data.LF_MEMBER.index, &arraySize);
			std::string_view amxxType = ConvertTypeToAmxx(typeTable, fieldRecord->data.LF_MEMBER.index);

			bool isStringT = false;

			if (amxxType == "integer")
			{
				// Check if this is a string_t
				std::string_view leafNameView = leafName;
				isStringT =
					leafNameView.starts_with("m_str") ||
					leafNameView.starts_with("m_isz") ||
					leafNameView == "m_sMaster" ||
					leafNameView == "m_globalstate" ||
					leafNameView == "m_altName";

				if (isStringT)
				{
					amxxType = "stringint";
					typeName = fmt::format("string_t {}", leafName);
				}
			}

			boost::json::object jField;
			jField["name"] = leafName;
			jField["offset"] = offset;
			jField["arraySize"] = arraySize != 0 ? boost::json::value(arraySize) : nullptr;
			jField["type"] = typeName;
			jField["amxxType"] = amxxType;
			jField["unsigned"] = nullptr;

			if (!isStringT && amxxType != "stringptr" && amxxType != "string")
			{
				switch (static_cast<PDB::CodeView::TPI::TypeIndexKind>(ResolveTypes(typeTable, fieldRecord->data.LF_MEMBER.index, true, true, true)))
				{
				case PDB::CodeView::TPI::TypeIndexKind::T_CHAR:
				case PDB::CodeView::TPI::TypeIndexKind::T_RCHAR:
				case PDB::CodeView::TPI::TypeIndexKind::T_SHORT:
				case PDB::CodeView::TPI::TypeIndexKind::T_LONG:
				case PDB::CodeView::TPI::TypeIndexKind::T_QUAD:
				case PDB::CodeView::TPI::TypeIndexKind::T_INT4:
				case PDB::CodeView::TPI::TypeIndexKind::T_INT8:
					jField["unsigned"] = false;
					break;

				case PDB::CodeView::TPI::TypeIndexKind::T_UCHAR:
				case PDB::CodeView::TPI::TypeIndexKind::T_USHORT:
				case PDB::CodeView::TPI::TypeIndexKind::T_ULONG:
				case PDB::CodeView::TPI::TypeIndexKind::T_UQUAD:
				case PDB::CodeView::TPI::TypeIndexKind::T_UINT4:
				case PDB::CodeView::TPI::TypeIndexKind::T_UINT8:
					jField["unsigned"] = true;
					break;
				}
			}

			jFields.push_back(std::move(jField));
			printf("[0x%llX]%s\n", offset, typeName.c_str());
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_NESTTYPE)
		{
			leafName = &fieldRecord->data.LF_NESTTYPE.name[0];
			std::string typeName = ConvertTypeToCString(leafName, typeTable, fieldRecord->data.LF_NESTTYPE.index, nullptr);

			printf("%s\n", typeName.c_str());
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_STMEMBER)
		{
			leafName = &fieldRecord->data.LF_STMEMBER.name[0];
			std::string typeName = ConvertTypeToCString(leafName, typeTable, fieldRecord->data.LF_STMEMBER.index, nullptr);

			printf("%s\n", typeName.c_str());
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_METHOD)
		{
			// Add to vtable
			leafName = fieldRecord->data.LF_METHOD.name;

			auto methodList = typeTable.GetTypeRecord(fieldRecord->data.LF_METHOD.mList);
			if (methodList)
			{
				// https://github.com/microsoft/microsoft-pdb/blob/master/PDB/include/symtypeutils.h#L220
				size_t offsetInMethodList = 0;
				for (size_t j = 0; j < fieldRecord->data.LF_METHOD.count; j++)
				{
					size_t entrySize = 2 * sizeof(uint32_t);
					PDB::CodeView::TPI::MethodListEntry* entry = (PDB::CodeView::TPI::MethodListEntry*)(methodList->data.LF_METHODLIST.mList + offsetInMethodList);

					PDB::CodeView::TPI::MethodProperty methodProp = (PDB::CodeView::TPI::MethodProperty)entry->attributes.mprop;

					if (methodProp == PDB::CodeView::TPI::MethodProperty::Intro ||
						methodProp == PDB::CodeView::TPI::MethodProperty::PureIntro)
					{
						// printf("METHOD %s %u %u\n", leafName, (uint32_t)methodAttributes, fieldRecord->data.LF_ONEMETHOD.vbaseoff[0] / 4);
						boost::json::object jMethod;
						jMethod["name"] = leafName;
						jMethod["linkName"] = nullptr;
						jMethod["index"] = entry->vbaseoff[0] / 4;
						jVTable.push_back(std::move(jMethod));
					}

					if (methodProp == PDB::CodeView::TPI::MethodProperty::Intro || methodProp == PDB::CodeView::TPI::MethodProperty::PureIntro)
						entrySize += sizeof(uint32_t);
					offsetInMethodList += entrySize;
				}
			}
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_ONEMETHOD)
		{
			// Add to vtable
			leafName = GetMethodName(fieldRecord);

			auto methodAttributes = static_cast<PDB::CodeView::TPI::MethodProperty>(fieldRecord->data.LF_ONEMETHOD.attributes.mprop);

			if (methodAttributes == PDB::CodeView::TPI::MethodProperty::Intro ||
				methodAttributes == PDB::CodeView::TPI::MethodProperty::PureIntro)
			{
				// printf("METHOD %s %u %u\n", leafName, (uint32_t)methodAttributes, fieldRecord->data.LF_ONEMETHOD.vbaseoff[0] / 4);
				boost::json::object jMethod;
				jMethod["name"] = leafName;
				jMethod["linkName"] = nullptr;
				jMethod["index"] = fieldRecord->data.LF_ONEMETHOD.vbaseoff[0] / 4;
				jVTable.push_back(std::move(jMethod));
			}
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_BCLASS)
		{
			leafName = GetLeafName(fieldRecord->data.LF_BCLASS.offset, fieldRecord->data.LF_BCLASS.lfEasy.kind);

			auto baseTypeRecord = typeTable.GetTypeRecord(fieldRecord->data.LF_BCLASS.index);
			if (baseTypeRecord)
			{
				auto baseLeafName = GetLeafName(baseTypeRecord->data.LF_CLASS.data, baseTypeRecord->data.LF_CLASS.lfEasy.kind);
				jClass["baseClass"] = baseLeafName;
			}

			i += static_cast<size_t>(leafName - reinterpret_cast<const char*>(fieldRecord));
			i = (i + (sizeof(uint32_t) - 1)) & (0 - sizeof(uint32_t));
			continue;
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_VBCLASS || fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_IVBCLASS)
		{
			// virtual base pointer offset from address point
			// followed by virtual base offset from vbtable

			const  PDB::CodeView::TPI::TypeRecordKind vbpOffsetAddressPointKind = *(PDB::CodeView::TPI::TypeRecordKind*)(fieldRecord->data.LF_IVBCLASS.vbpOffset);
			const uint8_t vbpOffsetAddressPointSize = GetLeafSize(vbpOffsetAddressPointKind);

			const  PDB::CodeView::TPI::TypeRecordKind vbpOffsetVBTableKind = *(PDB::CodeView::TPI::TypeRecordKind*)(fieldRecord->data.LF_IVBCLASS.vbpOffset + vbpOffsetAddressPointSize);
			const uint8_t vbpOffsetVBTableSize = GetLeafSize(vbpOffsetVBTableKind);

			i += sizeof(PDB::CodeView::TPI::FieldList::Data::LF_VBCLASS);
			i += vbpOffsetAddressPointSize + vbpOffsetVBTableSize;
			i = (i + (sizeof(uint32_t) - 1)) & (0 - sizeof(uint32_t));
			continue;
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_INDEX)
		{
			i += sizeof(PDB::CodeView::TPI::FieldList::Data::LF_INDEX);
			i = (i + (sizeof(uint32_t) - 1)) & (0 - sizeof(uint32_t));
			continue;
		}
		else if (fieldRecord->kind == PDB::CodeView::TPI::TypeRecordKind::LF_VFUNCTAB)
		{
			i += sizeof(PDB::CodeView::TPI::FieldList::Data::LF_VFUNCTAB);
			i = (i + (sizeof(uint32_t) - 1)) & (0 - sizeof(uint32_t));
			continue;
		}
		else
		{
			break;
		}

		i += static_cast<size_t>(leafName - reinterpret_cast<const char*>(fieldRecord));
		i += strnlen(leafName, maximumSize - i - 1) + 1;
		i = (i + (sizeof(uint32_t) - 1)) & (0 - sizeof(uint32_t));
	}

	jClass["fields"] = std::move(jFields);
	jClass["vtable"] = std::move(jVTable);
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
            ("pdb", po::value<std::string>()->required(), "path to the PDB")
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
        std::string pdbFilePath = vm["pdb"].as<std::string>();
        fmt::println("Opening PDB file {}", pdbFilePath);

        MemoryMappedFile::Handle pdbFile = MemoryMappedFile::Open(pdbFilePath.c_str());
        if (!pdbFile.baseAddress)
            throw std::runtime_error("Cannot memory-map file");

        if (IsError(PDB::ValidateFile(pdbFile.baseAddress, pdbFile.len)))
            throw std::runtime_error("Invalid file");

        const PDB::RawFile rawPdbFile = PDB::CreateRawFile(pdbFile.baseAddress);
        if (IsError(PDB::HasValidDBIStream(rawPdbFile)))
            throw std::runtime_error("Invalid DBI stream");

        const PDB::InfoStream infoStream(rawPdbFile);
        if (infoStream.UsesDebugFastLink())
            throw std::runtime_error("PDB was linked using unsupported option /DEBUG:FASTLINK");

        const auto h = infoStream.GetHeader();
        printf("Version %u, signature %u, age %u, GUID %08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x\n",
            static_cast<uint32_t>(h->version), h->signature, h->age,
            h->guid.Data1, h->guid.Data2, h->guid.Data3,
            h->guid.Data4[0], h->guid.Data4[1], h->guid.Data4[2], h->guid.Data4[3], h->guid.Data4[4], h->guid.Data4[5], h->guid.Data4[6], h->guid.Data4[7]);

        const PDB::DBIStream dbiStream = PDB::CreateDBIStream(rawPdbFile);
        if (!HasValidDBIStreams(rawPdbFile, dbiStream))
            throw std::runtime_error("Invalid DBI stream");

        const PDB::TPIStream tpiStream = PDB::CreateTPIStream(rawPdbFile);
        if (PDB::HasValidTPIStream(rawPdbFile) != PDB::ErrorCode::Success)
            throw std::runtime_error("Invalid TPI stream");

        // Read class list
        std::string classListPath = vm["class-list"].as<std::string>();
        fmt::println("Opening class list file {}", classListPath);
        std::ifstream classListFile(classListPath);
        std::set<std::string> classList;
        std::string line;

        while (std::getline(classListFile, line))
        {
            fmt::println("- {}", line);
            classList.insert(line);
        }

        // Iterate over all types
        TypeTable typeTable(tpiStream);
		boost::json::object jRoot;
		boost::json::object jClasses;

        for (const auto& record : typeTable.GetTypeRecords())
        {
            if (record->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_STRUCTURE ||
				record->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_CLASS ||
				record->header.kind == PDB::CodeView::TPI::TypeRecordKind::LF_CLASS2)
            {
                if (record->data.LF_CLASS.property.fwdref)
                    continue;

                auto typeRecord = typeTable.GetTypeRecord(record->data.LF_CLASS.field);
                if (!typeRecord)
                    continue;

                auto leafName = GetLeafName(record->data.LF_CLASS.data, record->data.LF_CLASS.lfEasy.kind);
				// fmt::println("{}", leafName);

                if (!classList.contains(leafName))
                    continue;

				boost::json::object jClass;
				jClass["baseClass"] = nullptr;

                printf("struct %s\n{\n", leafName);

                DisplayFields(typeTable, typeRecord, jClass);

                printf("}\n");

				jClasses[leafName] = std::move(jClass);
            }
        }

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
