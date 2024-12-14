set(TARGET_NAME OffsetExporter.Pdb)

add_executable(${TARGET_NAME}
    main.cpp
    MemoryMappedFile.cpp
    MemoryMappedFile.h
    pch.h
    TypeTable.cpp
    TypeTable.h
)

target_precompile_headers(${TARGET_NAME} PRIVATE pch.h)

target_link_libraries(${TARGET_NAME} PRIVATE
    Boost::json
    Boost::program_options
    fmt::fmt
    raw_pdb::raw_pdb
)
