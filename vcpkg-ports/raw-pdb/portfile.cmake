vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MolecularMatters/raw_pdb
    REF 479cd28468481f4df43cb392953da7e6636c70b6
    SHA512 802748ce71c7de9320edb12a6cd918c92cb976e59c3177e0c355f49d0d6764685e4e378cd19ea0e270f3fc8df90aa24f645b157f39ef93480021a0ec7880ddd4
    HEAD_REF main
    PATCHES
        "add-cmake-install.patch"
)


vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRAWPDB_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup()
vcpkg_copy_pdbs()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
