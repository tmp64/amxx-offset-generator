# AMXX Offset Generator

This tool generates offset gamedata files for AMX Mod X from debug information
(PDB and DWARF).

## Usage
1. Prerequisites:
   1. Get debug symbols for your mod.
      - On Windows, compile the mod with Visual Studio in Release mode. Set Debug
        Information Format = Program Database (/Zi)
      - On Linux, compile the mod with `-g` compiler argument
      - If using CMake, use RelWithDebInfo configuration
   2. Download amxx-offset-generator from Releases page on GitHub.
2. Retrieve the list of all classes that inherit from `CBaseEntity` and
   `CGameRules`.
   
   For Half-Life, you can use the file in this repo: `test-data\class-list.txt`

   For other mods, you have to generate it youself.

   1. Open hl.dll source code folder
   2. In bash, run this to generate an intermediate class list.
      ```
      egrep -hr 'class\s+(\w+)\s+:' | sort -u > class-list.txt
      ```

      It should look like this:
      ```
      class CAGrunt : public CSquadMonster
      class CActAnimating : public CBaseAnimating
      class CAirtank : public CGrenade
      class CAmbientGeneric : public CBaseEntity
      ...
      ```
   3. Use regex replacement in a text editor: `class\s+(\w+)\s+.*` -> `$1`
   4. Add `CBaseEntity` and `CGameRules` manually
   5. Remove `CRestore`, `CSave`, `CMultiplayGameMgrHelper` manually
   6. Sort the file
3. Run this command to generate Windows offsets:
   ```
   OffsetGenerator.Pdb
     --class-list path-to/class-list.txt
     --pdb path-to/hl.pdb
     --out offsets_windows.json
   ```
4. Run this command to generate Linux offsets:
   ```
   OffsetGenerator.Dwarf
     --class-list path-to/class-list.txt
     --so path-to/hl.so
     --out offsets_linux.json
   ```
5. Run this command to combine JSONs and generate AMXX gamedata. You can omit
   `--windows` or `--linux` if you don't need offsets for one them.
   ```
   python scripts/create_amxx_files.py
     --windows=path-to/offsets_windows.json
     --linux=path-to/offsets_linux.json
     --out=path-to/valve/addons/gamedata/common.games/custom
     "--banner=Custom offsets for Mod Name, generated with amxx-offset-generator"
     --file-prefix=my-mod
   ```

## Building

> **Content Warning**  
> C++ code in this project is *awful*. PDB format is unpleasant to work with and
> DWARF generator is full of memory leaks. Proceed with caution.

1. Install vcpkg
2. Run CMake with vcpkg's toolchain file
3. Build the project

