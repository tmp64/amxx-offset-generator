cmake_minimum_required(VERSION 3.20)

project(amxx-offset-generator)

# C++ version
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Third-party libraries
find_package(Boost CONFIG REQUIRED COMPONENTS json program_options)
find_package(fmt CONFIG REQUIRED)
find_package(raw-pdb CONFIG REQUIRED)

# Projects
add_subdirectory(src/OffsetExporter.Pdb)
