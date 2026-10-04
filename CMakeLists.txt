cmake_minimum_required(VERSION 3.16)
project(sbcli CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(sb
    src/jdoc.cpp
    src/sapi.cpp
    third_party/simdjson.cpp
    src/miniz_impl.cpp
    src/common.cpp
    src/squeak.cpp
    src/squeak_tables.cpp
    src/sb1_data.cpp
    src/sb1_image.cpp
    src/sb1_cmds.cpp
    src/sb2.cpp
    src/sb3_load.cpp
    src/sb3_render.cpp
    src/sb3_helpers.cpp
    src/sb3_cmds.cpp
    src/sb3_graph.cpp
    src/sb3_diff.cpp
    src/commands.cpp
    src/find_impl.cpp
    src/main.cpp
)

target_include_directories(sb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (WIN32)
    target_compile_definitions(sb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# simdjson 不开优化基本没有意义，单独给它开 -O2
set_source_files_properties(third_party/simdjson.cpp PROPERTIES COMPILE_OPTIONS "-O2")