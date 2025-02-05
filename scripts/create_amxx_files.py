import argparse
import json
from enum import Enum
from pathlib import Path

AT_STRINGINT = 'stringint'

class EPlatform(Enum):
    WINDOWS = 1
    LINUX = 2

PLATFORMS = [
    EPlatform.WINDOWS,
    EPlatform.LINUX,
]

PLATFORM_AMXX = {
    EPlatform.WINDOWS: 'windows',
    EPlatform.LINUX: 'linux',
}

COMMON_VTABLE = [
    ('CBaseEntity', ''),
    ('CBaseMonster', 'player_'), # Yes, "player"
    ('CBasePlayer', 'player_'),
    ('CBasePlayerItem', 'item_'),
    ('CBasePlayerWeapon', 'weapon_'),
]

class FieldInfo:
    def __init__(self):
        self.name: str = ''
        self.array_size: int | None = None
        self.amxx_type: str = ''
        self.unsigned: bool | None = None

        self.plat_offset: dict[EPlatform, int] = {}
        self.plat_type: dict[EPlatform, str | None] = {}

class VirtualMethodInfo:
    def __init__(self):
        self.name: str = ''

        self.plat_link_name: dict[EPlatform, str | None] = {}
        self.plat_index: dict[EPlatform, int] = {}

class ClassInfo:
    def __init__(self):
        self.base_class_name: str | None = None
        self.fields: list[FieldInfo] = []
        self.vtable: list[VirtualMethodInfo] = []

    def find_field(self, name: str) -> FieldInfo | None:
        for i in self.fields:
            if i.name == name:
                return i

        return None
    
    def find_virtual_method(self, name: str) -> VirtualMethodInfo | None:
        for i in self.vtable:
            if i.name == name:
                return i

        return None

def log_warn(s: str):
    print(f'WARN   {s}')
def log_note(s: str):
    print(f'NOTE   {s}')

def read_json(path, platform: EPlatform) -> dict[str, ClassInfo]:
    with open(path, 'r') as f:
        jroot = json.load(f)

        result: dict[str, ClassInfo] = {}

        for class_name, jclass in jroot['classes'].items():
            ci = ClassInfo()
            ci.base_class_name = jclass['baseClass']

            for jfield in jclass['fields']:
                fi = FieldInfo()
                fi.name = jfield['name']
                fi.array_size = jfield['arraySize']
                fi.amxx_type = jfield['amxxType']
                fi.unsigned = jfield['unsigned']
                
                fi.plat_offset[platform] = jfield['offset']
                fi.plat_type[platform] = jfield['type']

                ci.fields.append(fi)

            method_map: dict[str, VirtualMethodInfo] = {}
            method_overload_names: set[str] = set()

            for jmethod in jclass['vtable']:
                vmi = VirtualMethodInfo()
                vmi.name = jmethod['name']

                vmi.plat_link_name[platform] = jmethod['linkName']
                vmi.plat_index[platform] = jmethod['index']

                ci.vtable.append(vmi)

                if vmi.name in method_map:
                    method_overload_names.add(vmi.name)
                
                method_map[vmi.name] = vmi

            # Remove overloads (not supported)
            for i in method_overload_names:
                log_warn(f'Removing overload {class_name}::{i}')
                ci.vtable.remove(method_map[i])

            result[class_name] = ci

        # Check that all base classes were exported
        for class_name, ci in result.items():
            if ci.base_class_name is None:
                continue
            if ci.base_class_name not in result:
                log_warn(f'Base class {ci.base_class_name} of {class_name} was not exported')

        # Remove virtual methods that exist in base
        for class_name, ci in result.items():
            def find_method_in_base(name: str, index: int) -> str | None:
                cur_class = ci.base_class_name

                while cur_class is not None:
                    base_ci = result[cur_class]
                    for i in base_ci.vtable:
                        if i.plat_index[platform] == index and i.name == name:
                            return cur_class
                    cur_class = base_ci.base_class_name
                return None
            
            for i in range(len(ci.vtable) - 1, -1, -1):
                vmi = ci.vtable[i]
                vmi_in_base_name = find_method_in_base(vmi.name, vmi.plat_index[platform])

                if vmi_in_base_name is not None:
                    # log_note(f'Removed virtual method {class_name} :: {vmi.name} that exists in base {vmi_in_base_name}')
                    ci.vtable.pop(i)

        return result

def combine_files(files: dict[EPlatform, dict[str, ClassInfo]]) -> dict[str, ClassInfo]:
    result: dict[str, ClassInfo] = {}

    for platform, file in files.items():
        for class_name, ci in file.items():
            def check_mismatch(context: str, var_name: str, base_value, platform_value):
                if base_value != platform_value:
                    log_warn(f'{class_name} :: {context}: {var_name} mismatch: {base_value} != {platform}:{platform_value}')

            # Find class
            if class_name in result:
                out_ci = result[class_name]
            else:
                out_ci = ClassInfo()
                out_ci.base_class_name = ci.base_class_name
                result[class_name] = out_ci

            # Check values
            check_mismatch('', 'Base class', out_ci.base_class_name, ci.base_class_name)

            # Fields
            for fi in ci.fields:
                # Find field
                out_fi = out_ci.find_field(fi.name)

                if out_fi is None:
                    out_fi = FieldInfo()
                    out_fi.name = fi.name
                    out_fi.array_size = fi.array_size
                    out_fi.amxx_type = fi.amxx_type
                    out_fi.unsigned = fi.unsigned
                    out_ci.fields.append(out_fi)
                
                # Fix strings
                if out_fi.amxx_type != fi.amxx_type and (out_fi.amxx_type == AT_STRINGINT or fi.amxx_type == AT_STRINGINT):
                    log_note(f'{class_name} :: {out_fi.name}: Fixing {AT_STRINGINT} type')
                    out_fi.amxx_type = AT_STRINGINT
                    out_fi.unsigned = None
                    fi.amxx_type = AT_STRINGINT
                    fi.unsigned = None

                # Check values
                check_mismatch(out_fi.name, 'Array size', out_fi.array_size, fi.array_size)
                check_mismatch(out_fi.name, 'AMXX type', out_fi.amxx_type, fi.amxx_type)
                check_mismatch(out_fi.name, 'Unsigned', out_fi.unsigned, fi.unsigned)

                # Add platform
                out_fi.plat_offset[platform] = fi.plat_offset[platform]
                out_fi.plat_type[platform] = fi.plat_type[platform]

            # VTable
            for vmi in ci.vtable:
                # Find VMI
                out_vmi = out_ci.find_virtual_method(vmi.name)

                if out_vmi is None:
                    out_vmi = VirtualMethodInfo()
                    out_vmi.name = vmi.name
                    out_ci.vtable.append(out_vmi)

                # Add platform
                out_vmi.plat_link_name[platform] = vmi.plat_link_name[platform]
                out_vmi.plat_index[platform] = vmi.plat_index[platform]

    return result

def create_gamedata(classes: dict[str, ClassInfo], out_dir: Path, banner: str | None, file_prefix: str | None):
    if file_prefix is not None:
        file_prefix = file_prefix + '-'

    def write_banner(f):
        if banner is not None:
            f.write('//\n')
            f.write(f'// {banner}\n')
            f.write('//\n\n')

    def write_indent(f, indent: int, text: str):
        indent_str = '\t' * indent
        f.write(indent_str)
        f.write(text)
        f.write('\n')

    def write_kv(f, indent: int, key: str, value: str, comment: str | None = None):
        if comment is not None:
            comment = ' // ' + comment
        else:
            comment = ''
        
        write_indent(f, indent, f'"{key}" "{value}"{comment}')

    out_dir.mkdir(parents=True, exist_ok=True)

    # Build inheritance tree
    base_class_chain: dict[str, list[str]] = {}
    derived_classes: dict[str, list[str]] = {}

    for class_name, ci in classes.items():
        # Base class chain
        chain = []
        cur_base: str | None = ci.base_class_name

        while cur_base is not None:
            chain.append(cur_base)
            cur_base = classes[cur_base].base_class_name

        chain.reverse()
        base_class_chain[class_name] = chain

        if class_name not in derived_classes:
            derived_classes[class_name] = []
        
        if ci.base_class_name is not None:
            # Add to base's derived list
            if ci.base_class_name not in derived_classes:
                derived_classes[ci.base_class_name] = []
            derived_classes[ci.base_class_name].append(class_name)

    # Sort tree
    for i, j in base_class_chain.items():
        base_class_chain[i] = sorted(j)
    for i, j in derived_classes.items():
        derived_classes[i] = sorted(j)

    for class_name, ci in classes.items():
        class_name_lower = class_name.lower()

        # Offset file
        def print_field_offsets():
            with open(out_dir / f'{file_prefix}offsets-{class_name_lower}.txt', 'w') as f:
                write_banner(f)

                write_indent(f, 0, '"Games"')
                write_indent(f, 0, '{')

                write_indent(f, 1, '"#default"')
                write_indent(f, 1, '{')

                write_indent(f, 2, '"Classes"')
                write_indent(f, 2, '{')

                write_indent(f, 3, f'"{class_name}"')
                write_indent(f, 3, '{')

                write_indent(f, 4, '"Offsets"')
                write_indent(f, 4, '{')

                for fi in ci.fields:
                    # Print type
                    # // W: int fieldName[123]
                    for platform in PLATFORMS:
                        if platform in fi.plat_type:
                            write_indent(f, 5, f'// {str(platform.name)[:3]}: {fi.plat_type[platform]}')
                    
                    write_indent(f, 5, f'"{fi.name}"')
                    write_indent(f, 5, '{')

                    write_kv(f, 6, 'type', fi.amxx_type)

                    if fi.array_size is not None:
                        write_kv(f, 6, 'size', str(fi.array_size))

                    if fi.unsigned is not None:
                        write_kv(f, 6, 'unsigned', '1' if fi.unsigned else '0')

                    for platform in PLATFORMS:
                        if platform in fi.plat_offset:
                            write_kv(f, 6, PLATFORM_AMXX[platform], str(fi.plat_offset[platform]))

                    write_indent(f, 5, '}\n')

                write_indent(f, 4, '}')
                write_indent(f, 3, '}')
                write_indent(f, 2, '}')
                write_indent(f, 1, '}')
                write_indent(f, 0, '}')

                # Print hierarchy
                f.write('\n')
                f.write('//\n')
                f.write('// Class Hierarchy:\n')
                f.write('// -\n')

                hier_indent = 1

                # Base classes
                for i in base_class_chain[class_name]:
                    f.write('//' + ('\t' * hier_indent) + i + '\n')
                    hier_indent += 1

                # This class
                f.write('//' + ('\t' * hier_indent) + class_name + '\n')
                hier_indent += 1

                # Derived classes
                def print_derived_of(derived_name: str, indent: int):
                    for i in derived_classes[derived_name]:
                        f.write('//' + ('\t' * indent) + i + '\n')
                        print_derived_of(i, indent + 1)

                print_derived_of(class_name, hier_indent)

                f.write('//\n')

        # VTable file
        def print_vtable_offsets():
            if len(ci.vtable) == 0:
                return

            with open(out_dir / f'{file_prefix}offsets-virtual-{class_name_lower}.txt', 'w') as f:
                write_banner(f)

                write_indent(f, 0, '"Games"')
                write_indent(f, 0, '{')

                write_indent(f, 1, '"#default"')
                write_indent(f, 1, '{')

                write_indent(f, 2, '"Classes"')
                write_indent(f, 2, '{')

                write_indent(f, 3, f'"{class_name}"')
                write_indent(f, 3, '{')

                write_indent(f, 4, '"Offsets"')
                write_indent(f, 4, '{')

                for vfi in ci.vtable:
                    write_indent(f, 5, f'"{vfi.name}"')
                    write_indent(f, 5, '{')

                    for platform in PLATFORMS:
                        if platform in vfi.plat_index:
                            link_name = ''

                            if platform in vfi.plat_link_name and vfi.plat_link_name[platform] is not None:
                                link_name = str(vfi.plat_link_name[platform])

                            write_kv(f, 6, PLATFORM_AMXX[platform], str(vfi.plat_index[platform]), link_name)

                    write_indent(f, 5, '}\n')

                write_indent(f, 4, '}')
                write_indent(f, 3, '}')
                write_indent(f, 2, '}')
                write_indent(f, 1, '}')
                write_indent(f, 0, '}')

        print_field_offsets()
        print_vtable_offsets()

    # Create common vtable file
    with open(out_dir / f'{file_prefix}offsets-virtual-common.txt', 'w') as f:
        write_banner(f)

        write_indent(f, 0, '"Games"')
        write_indent(f, 0, '{')

        write_indent(f, 1, '"#default"')
        write_indent(f, 1, '{')

        write_indent(f, 2, '"Offsets"')
        write_indent(f, 2, '{')

        for class_name, name_prefix in COMMON_VTABLE:
            if class_name not in classes:
                continue

            ci = classes[class_name]

            for vfi in ci.vtable:
                write_indent(f, 3, f'"{name_prefix}{vfi.name.lower()}"')
                write_indent(f, 3, '{')

                for platform in PLATFORMS:
                    if platform in vfi.plat_index:
                        link_name = ''

                        if platform in vfi.plat_link_name and vfi.plat_link_name[platform] is not None:
                            link_name = str(vfi.plat_link_name[platform])

                        write_kv(f, 4, PLATFORM_AMXX[platform], str(vfi.plat_index[platform]), link_name)
                
                write_indent(f, 3, '}\n')

        write_indent(f, 2, '}')
        write_indent(f, 1, '}')
        write_indent(f, 0, '}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--windows', help='Path to Windows JSON')
    parser.add_argument('--linux', help='Path to Linux JSON')
    parser.add_argument('--out', help='Path to custom gamedata directory', required=True)
    parser.add_argument('--banner', help='Top comment in generated files')
    parser.add_argument('--file-prefix', help='File name prefix')

    args = parser.parse_args()

    files: dict[EPlatform, dict[str, ClassInfo]] = {}

    if args.windows is not None:
        print(f'==== Loading Windows file: {args.windows}')
        files[EPlatform.WINDOWS] = read_json(args.windows, EPlatform.WINDOWS)

    if args.linux is not None:
        print(f'==== Loading Linux file: {args.linux}')
        files[EPlatform.LINUX] = read_json(args.linux, EPlatform.LINUX)

    print(f'==== Combining {len(files)} files')
    classes = combine_files(files)
    
    print(f'==== Creating gamedata in {args.out}')
    create_gamedata(classes, Path(args.out), args.banner, args.file_prefix)

main()
