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
    src/ext_loader.cpp
    src/sb3_cmds.cpp
    src/sb3_graph.cpp
    src/sb3_diff.cpp
    src/commands.cpp
    src/find_impl.cpp
    src/sbcli_parser.cpp
    src/sbcli_check.cpp
    src/sbcli_fix.cpp
    src/sbcli_view.cpp
    src/sbcli_project.cpp
    src/sbcli_search.cpp
    src/sbcli_pack.cpp
    src/sbcli_unpack.cpp
    src/main.cpp
)

target_include_directories(sb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# 自测（独立可执行文件，不进主程序）
#
# tests/ 源码已入库（1.2.1 起，CI 需要）；这里用 EXISTS 条件包含，
# 兼容没有测试源的老 checkout（如只 pull 了源码分支），保证 cmake 配置始终成功。
# 二进制产物由 .gitignore 的 *.exe/*.obj/*.o 排除。
# ---------------------------------------------------------------------------

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parser.cpp)
    add_executable(sb_parser_test
        tests/test_parser.cpp
        src/sbcli_parser.cpp
    )
    target_include_directories(sb_parser_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if (WIN32)
        target_compile_definitions(sb_parser_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parser2.cpp)
    add_executable(sb_parser_test2
        tests/test_parser2.cpp
        src/sbcli_parser.cpp
    )
    target_include_directories(sb_parser_test2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if (WIN32)
        target_compile_definitions(sb_parser_test2 PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fix.cpp)
    add_executable(sb_fix_test
        tests/test_fix.cpp
        src/sbcli_parser.cpp
        src/sbcli_fix.cpp
    )
    target_include_directories(sb_fix_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if (WIN32)
        target_compile_definitions(sb_fix_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

if (WIN32)
    target_compile_definitions(sb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# 源码根目录（编译期固化）：运行时定位 tools/fetch_tw_extension.js（扩展解析）。
# 优先级：环境变量 SBCLI_TOOLS > SBCLI_SOURCE_ROOT > cwd/tools > exe 旁/上级。
target_compile_definitions(sb PRIVATE
    SBCLI_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# pack / unpack 回归测试（round-trip 为核心）
#
# 同样是本地目录、不纳入版本控制，用 EXISTS 条件包含。
# 这套测试**链接源码**（不是跑子进程），所以往 sbcli_pack.cpp /
# sbcli_unpack.cpp 注 bug → 删对应 .obj → 重编 → 测试应当失败。
# 链接的文件与 sb 主程序保持一致（仅剔除命令分发与 sb1/sb3 展示命令）。
# ---------------------------------------------------------------------------

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pack_roundtrip.cpp)
    add_executable(sb_pack_test
        tests/test_pack_roundtrip.cpp
        src/jdoc.cpp
        src/sapi.cpp
        src/miniz_impl.cpp
        src/common.cpp
        src/sb2.cpp
        src/sb3_load.cpp
        src/sb3_helpers.cpp
        src/sb3_render.cpp
        src/sbcli_parser.cpp
        src/sbcli_pack.cpp
        src/sbcli_unpack.cpp
        third_party/simdjson.cpp
    )
    target_include_directories(sb_pack_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if (WIN32)
        target_compile_definitions(sb_pack_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# simdjson 不开优化基本没有意义，单独给它开 -O2
set_source_files_properties(third_party/simdjson.cpp PROPERTIES COMPILE_OPTIONS "-O2")

# ---------------------------------------------------------------------------
# 扩展模块端到端测试（注册 / 解析块数 / check 通过·报错 / round-trip / remove）
#
# 链接扩展相关源码（ext_loader / sbcli_check / sbcli_project）+ pack/unpack。
# 运行前 CWD 需在项目根（main() 自动 chdir）：node + tools/fetch_tw_extension.js
# 负责解析扩展 getInfo()（tools/ 相对 CWD 解析）。依赖 node 在 PATH 中。
# ---------------------------------------------------------------------------

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extension.cpp)
    add_executable(sb_extension_test
        tests/test_extension.cpp
        src/jdoc.cpp
        src/sapi.cpp
        src/miniz_impl.cpp
        src/common.cpp
        src/sb2.cpp
        src/sb3_load.cpp
        src/sb3_helpers.cpp
        src/sb3_render.cpp
        src/sbcli_parser.cpp
        src/sbcli_pack.cpp
        src/sbcli_unpack.cpp
        src/sbcli_check.cpp
        src/sbcli_project.cpp
        src/ext_loader.cpp
        third_party/simdjson.cpp
    )
    target_include_directories(sb_extension_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if (WIN32)
        target_compile_definitions(sb_extension_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()