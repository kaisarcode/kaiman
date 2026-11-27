# CMakeLists.txt
# Summary: Cross-platform build for kaiman with source dependencies.
#
# Author:  KaisarCode
# Website: https://kaisarcode.com
# License: https://www.gnu.org/licenses/lgpl-3.0.html

cmake_minimum_required(VERSION 3.14)
project(kaiman C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

if(NOT KC_TARGET)
    message(FATAL_ERROR "KC_TARGET is required")
endif()
set(KC_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/${KC_TARGET}")
file(MAKE_DIRECTORY "${KC_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${KC_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${KC_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${KC_OUTPUT_DIR}")

if(KC_GUI AND NOT KC_LIBRARY_ONLY)
    file(MAKE_DIRECTORY "${KC_OUTPUT_DIR}/gui")
    foreach(asset index.html app.js styles.css logo-46.png logo-92.png
        logo-128.png logo.ico)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/gui/${asset}"
            "${KC_OUTPUT_DIR}/gui/${asset}" COPYONLY)
    endforeach()
    if(WIN32)
        set(KC_WEBVIEW2_LOADER
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/kaisarcode/libwvw.c/lib/webview2/WebView2Loader.dll")
        configure_file("${KC_WEBVIEW2_LOADER}"
            "${KC_OUTPUT_DIR}/WebView2Loader.dll" COPYONLY)
    endif()
endif()

set(KC_DEPENDENCIES http netl tls)
set(KC_GUI_SOURCES)
set(KC_GUI_LINK_LIBS)
if(KC_GUI AND NOT KC_LIBRARY_ONLY)
    list(APPEND KC_DEPENDENCIES wvw tray)
    list(APPEND KC_GUI_SOURCES src/gui.c)
    if(WIN32)
        list(APPEND KC_GUI_LINK_LIBS ole32 uuid user32 gdi32 shell32 advapi32)
    elseif(APPLE)
        list(APPEND KC_GUI_LINK_LIBS "-framework WebKit"
            "-framework Cocoa" "-framework Foundation"
            "-framework CoreFoundation")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(KC_WVW REQUIRED IMPORTED_TARGET
            gtk+-3.0 webkit2gtk-4.1)
        list(APPEND KC_GUI_LINK_LIBS PkgConfig::KC_WVW)
    endif()
endif()
set(KC_STATIC_LIBS)
set(KC_SHARED_LIBS)
set(KC_DEP_INCLUDES)
foreach(dependency IN LISTS KC_DEPENDENCIES)
    set(root "${CMAKE_CURRENT_SOURCE_DIR}/lib/kaisarcode/lib${dependency}.c")
    set(header "${root}/src/lib${dependency}.h")
    set(library "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/${KC_TARGET}/lib${dependency}.a")
    if(NOT EXISTS "${header}" OR NOT EXISTS "${library}")
        message(FATAL_ERROR "Missing dependency for ${KC_TARGET}: ${dependency}")
    endif()
    list(APPEND KC_DEP_INCLUDES "${root}/src")
    list(APPEND KC_STATIC_LIBS "${library}")
    if(KC_LIBRARY_ONLY OR NOT dependency STREQUAL "tls")
        list(APPEND KC_SHARED_LIBS "${library}")
    endif()
endforeach()
if(NOT KC_LIBRARY_ONLY)
    set(KC_TLS_SHARED "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/${KC_TARGET}/libtls.so")
    if(WIN32)
        set(KC_TLS_SHARED "${CMAKE_CURRENT_SOURCE_DIR}/bin/${KC_TARGET}/libtls.dll")
        set(KC_TLS_IMPORT "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/${KC_TARGET}/libtls.dll.a")
    elseif(APPLE)
        set(KC_TLS_SHARED "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/${KC_TARGET}/libtls.dylib")
    endif()
    if(NOT EXISTS "${KC_TLS_SHARED}" OR (WIN32 AND NOT EXISTS "${KC_TLS_IMPORT}"))
        message(FATAL_ERROR "Missing shared libtls for ${KC_TARGET}")
    endif()
    add_library(kc_tls_shared SHARED IMPORTED)
    set_target_properties(kc_tls_shared PROPERTIES
        IMPORTED_LOCATION "${KC_TLS_SHARED}")
    if(WIN32)
        set_target_properties(kc_tls_shared PROPERTIES
            IMPORTED_IMPLIB "${KC_TLS_IMPORT}")
    endif()
    list(APPEND KC_SHARED_LIBS kc_tls_shared)
endif()
include_directories(src ${KC_DEP_INCLUDES})
separate_arguments(KC_LINK_FLAGS NATIVE_COMMAND "${KC_SHARED_FLAGS}")
if(WIN32)
    set(KC_LINK_FLAGS)
endif()
separate_arguments(KC_SYSTEM_LINKS NATIVE_COMMAND "${KC_SYSTEM_LIBS}")

set(KC_SOURCES
    src/libkaiman.c
    src/json.c
    src/util.c
    src/upstream.c
    src/keyring.c
    src/child.c
)

add_library(kaiman_static STATIC ${KC_SOURCES})
set_target_properties(kaiman_static PROPERTIES OUTPUT_NAME "kaiman")
target_compile_options(kaiman_static PRIVATE -O2 -Wall -Wextra -Werror)
target_compile_definitions(kaiman_static PRIVATE "KC_KAIMAN_VERSION=\"${KC_VERSION}\"")

if(NOT KC_LIBRARY_ONLY)
    add_library(kaiman_shared SHARED ${KC_SOURCES})
    set_target_properties(kaiman_shared PROPERTIES
        OUTPUT_NAME "kaiman" WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_options(kaiman_shared PRIVATE -O2 -Wall -Wextra -Werror)
    target_compile_definitions(kaiman_shared PRIVATE "KC_KAIMAN_VERSION=\"${KC_VERSION}\"")
    target_link_options(kaiman_shared PRIVATE ${KC_LINK_FLAGS})
    if(APPLE)
        set_target_properties(kaiman_shared PROPERTIES BUILD_RPATH "@loader_path")
    elseif(NOT WIN32)
        set_target_properties(kaiman_shared PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
    target_link_libraries(kaiman_shared PRIVATE ${KC_SHARED_LIBS} ${KC_SYSTEM_LINKS})

    add_executable(kaiman_cli src/kaiman.c ${KC_GUI_SOURCES})
    set_target_properties(kaiman_cli PROPERTIES OUTPUT_NAME "kaiman")
    target_compile_options(kaiman_cli PRIVATE -O2 -Wall -Wextra -Werror)
    target_compile_definitions(kaiman_cli PRIVATE
        "KC_KAIMAN_VERSION=\"${KC_VERSION}\"")
    if(KC_GUI)
        target_compile_definitions(kaiman_cli PRIVATE KC_KAIMAN_GUI=1)
    endif()
    target_link_libraries(kaiman_cli PRIVATE kaiman_static ${KC_STATIC_LIBS}
        ${KC_GUI_LINK_LIBS} ${KC_SYSTEM_LINKS})
endif()
