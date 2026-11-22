## CMakeLists.txt
## Summary: Cross-platform build for the in-memory key-value collection.
##
## Author:  KaisarCode
## Website: https://kaisarcode.com
## License: https://www.gnu.org/licenses/lgpl-3.0.html

cmake_minimum_required(VERSION 3.14)
project(kv C)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" KC_VERSION_LINE
    REGEX "^[ \t]*\"version\"[ \t]*:")
string(REGEX REPLACE
    ".*\"version\"[ \t]*:[ \t]*\"([^\"]+)\".*" "\\1"
    KC_VERSION "${KC_VERSION_LINE}")
if(NOT KC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "Invalid semantic version in manifest.json")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")
add_compile_definitions("KC_MANIFEST_VERSION=\"${KC_VERSION}\"")

add_compile_definitions("KC_MANIFEST_VERSION=\"${KC_VERSION}\"")

set(CMAKE_C_STANDARD 11)

option(KV_BUILD_TESTS "Build kv contract tests" OFF)
set(KV_TEST_EXTERNAL_ARTIFACTS OFF)
if(KV_BUILD_TESTS AND KV_TEST_SHARED_LIBRARY)
    set(KV_TEST_EXTERNAL_ARTIFACTS ON)
endif()

if(NOT MSVC)
    add_compile_options(-O3 -Wall -Wextra -Werror)
else()
    add_compile_options(/O2 /W3 /WX)
endif()

set(KV_SOURCES src/libkv.c)

set_source_files_properties(src/libkv.c PROPERTIES
    COMPILE_DEFINITIONS "KC_KV_VERSION=\"${KC_VERSION}\""
)

if(EMSCRIPTEN)
    add_executable(kv_wasm ${KV_SOURCES})
    set_target_properties(kv_wasm PROPERTIES
        OUTPUT_NAME "libkv"
        SUFFIX ".mjs"
    )
    target_include_directories(kv_wasm PRIVATE src)
    target_link_options(kv_wasm PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,getValue,setValue,HEAPU8
        -sEXPORTED_FUNCTIONS=_kc_kv_open,_kc_kv_set,_kc_kv_get,_kc_kv_del,_kc_kv_count,_kc_kv_at,_kc_kv_export,_kc_kv_close,_kc_kv_strerror,_kc_kv_version,_malloc,_free
    )
    add_library(kv_wasm_static STATIC ${KV_SOURCES})
    set_target_properties(kv_wasm_static PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "kv"
        POSITION_INDEPENDENT_CODE ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    )
    target_include_directories(kv_wasm_static PRIVATE src)

    add_library(kv_wasm_shared SHARED ${KV_SOURCES})
    set_target_properties(kv_wasm_shared PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "kv"
        SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    )
    target_include_directories(kv_wasm_shared PRIVATE src)
    target_link_options(kv_wasm_shared PRIVATE -sSIDE_MODULE=1)
elseif(NOT KV_TEST_EXTERNAL_ARTIFACTS)
    add_library(kv_static STATIC ${KV_SOURCES})
    set_target_properties(kv_static PROPERTIES OUTPUT_NAME "kv" POSITION_INDEPENDENT_CODE ON)
    target_include_directories(kv_static PRIVATE src)
    add_library(kv_shared SHARED ${KV_SOURCES})
    set_target_properties(kv_shared PROPERTIES
        OUTPUT_NAME "kv"
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_include_directories(kv_shared PRIVATE src)
endif()

if(KV_BUILD_TESTS)
    enable_testing()
    if(EMSCRIPTEN)
        add_executable(kv_contract_test src/test.c ${KV_SOURCES})
        target_include_directories(kv_contract_test PRIVATE src)
    else()
        if(KV_TEST_SHARED_LIBRARY)
            add_library(kv_test_shared SHARED IMPORTED)
            set_target_properties(kv_test_shared PROPERTIES
                IMPORTED_LOCATION "${KV_TEST_SHARED_LIBRARY}"
            )
            if(KV_TEST_IMPORT_LIBRARY)
                set_target_properties(kv_test_shared PROPERTIES
                    IMPORTED_IMPLIB "${KV_TEST_IMPORT_LIBRARY}"
                )
            endif()
            set(KV_TEST_SHARED_TARGET kv_test_shared)
        else()
            set(KV_TEST_SHARED_TARGET kv_shared)
        endif()
        add_executable(kv_contract_test src/test.c)
        target_include_directories(kv_contract_test PRIVATE src)
        target_link_libraries(kv_contract_test PRIVATE ${KV_TEST_SHARED_TARGET})
        if(KV_CLI_PATH)
            target_compile_definitions(kv_contract_test PRIVATE
                KC_CLI_PATH=\"${KV_CLI_PATH}\"
            )
        endif()
        add_custom_command(TARGET kv_contract_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${KV_TEST_SHARED_TARGET}>
                $<TARGET_FILE_DIR:kv_contract_test>
        )
    endif()
endif()

if(TARGET kv_static AND NOT EMSCRIPTEN AND
    NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(kv_cli src/kv.c)
    set_target_properties(kv_cli PROPERTIES
        OUTPUT_NAME "kv"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    )
    target_include_directories(kv_cli PRIVATE src)
    target_link_libraries(kv_cli PRIVATE kv_static)
    if(APPLE)
        set_target_properties(kv_cli PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@loader_path"
        )
    elseif(UNIX)
        set_target_properties(kv_cli PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN"
        )
    endif()
endif()
