set(TARGET_NAME OffsetExporter.Dwarf)

add_executable(${TARGET_NAME}
    main.cpp
    pch.h
)

target_precompile_headers(${TARGET_NAME} PRIVATE pch.h)

target_link_libraries(${TARGET_NAME} PRIVATE
    Boost::json
    Boost::program_options
    fmt::fmt
    libdwarf::dwarf
)
