cmake_minimum_required(VERSION 3.20)
project(recovery_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "This project must be built with MSVC (Visual Studio) to match the prebuilt libuvcpp binaries.")
endif()

# ---------------------------------------------------------------------------
# Third-party libuvcpp (referenced, not modified).
# ---------------------------------------------------------------------------
set(LIBUVCPP_ROOT      "D:/public/libuvcpp/libuvcpp" CACHE PATH "libuvcpp source root")
set(LIBUVCPP_INCLUDE   "${LIBUVCPP_ROOT}/src")
set(LIBUV_INCLUDE      "${LIBUVCPP_ROOT}/_local_deps/libuv/include")
set(LIBUVCPP_BIN_DIR   "${LIBUVCPP_ROOT}/build_web/Release")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
add_executable(recovery_server
    src/main.cpp
    src/http/server.cpp
    src/ntfs/disk_reader.cpp
    src/ntfs/boot_parser.cpp
    src/ntfs/data_run.cpp
    src/ntfs/mft_parser.cpp
    src/ntfs/tree_builder.cpp
    src/fat/fat.cpp
    src/recovery/scanner.cpp
    src/recovery/restorer.cpp
    src/include/util.cpp
    resources/fsrec.rc
)

target_include_directories(recovery_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
    ${LIBUVCPP_INCLUDE}
    ${LIBUV_INCLUDE}
)

target_compile_definitions(recovery_server PRIVATE
    UVCPP_WEB_ENABLE=1
    UVCPP_NET_ENABLE=1
    UVCPP_IMPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Treat all sources as UTF-8 (suppresses C4819 warnings from libuvcpp's
# Chinese doc comments and our own source).
target_compile_options(recovery_server PRIVATE /utf-8)

target_link_directories(recovery_server PRIVATE ${LIBUVCPP_BIN_DIR})

target_link_libraries(recovery_server PRIVATE
    uvcpp      # import lib for uvcpp.dll
    advapi32   # AllocateAndInitializeSid / CheckTokenMembership
    shell32    # ShellExecuteA (auto-open the web UI)
)

# Mark the executable as requiring elevation (double-clicking triggers UAC).
# The linker auto-generates and embeds a requireAdministrator manifest — the
# same approach HBOP uses, verified to trigger UAC reliably. (A hand-embedded
# .rc manifest turned out not to trigger UAC, so we use the linker flag.)
target_link_options(recovery_server PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

# ---------------------------------------------------------------------------
# Copy runtime DLLs next to the executable.
# ---------------------------------------------------------------------------
foreach(_dll
        "${LIBUVCPP_BIN_DIR}/uvcpp.dll"
        "${LIBUVCPP_ROOT}/build_web/_deps/libuv-build/Release/uv.dll"
        "${LIBUVCPP_ROOT}/build_web/_deps/llhttp-build/Release/llhttp.dll")
    add_custom_command(TARGET recovery_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:recovery_server>"
        COMMENT "Copying ${_dll}")
endforeach()

# Note: CMake's default MSVC runtime (/MD Release, /MDd Debug) matches the
# prebuilt libuvcpp DLL, so no explicit MSVC_RUNTIME_LIBRARY is required.
