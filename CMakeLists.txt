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

set(KC_DEPENDENCIES http kv netl)
set(KC_LIBS)
set(KC_DEP_INCLUDES)
foreach(dependency IN LISTS KC_DEPENDENCIES)
    set(root "${CMAKE_CURRENT_SOURCE_DIR}/lib/kaisarcode/lib${dependency}.c")
    set(header "${root}/src/lib${dependency}.h")
    set(library "${CMAKE_CURRENT_SOURCE_DIR}/.build/deps/${KC_TARGET}/lib${dependency}.a")
    if(NOT EXISTS "${header}" OR NOT EXISTS "${library}")
        message(FATAL_ERROR "Missing dependency for ${KC_TARGET}: ${dependency}")
    endif()
    list(APPEND KC_DEP_INCLUDES "${root}/src")
    list(APPEND KC_LIBS "${library}")
endforeach()
include_directories(src ${KC_DEP_INCLUDES})
separate_arguments(KC_LINK_FLAGS NATIVE_COMMAND "${KC_SHARED_FLAGS}")
if(WIN32)
    set(KC_LINK_FLAGS)
endif()
separate_arguments(KC_SYSTEM_LINKS NATIVE_COMMAND "${KC_SYSTEM_LIBS}")

add_library(kaiman_static STATIC src/libkaiman.c)
set_target_properties(kaiman_static PROPERTIES OUTPUT_NAME "kaiman")
target_compile_options(kaiman_static PRIVATE -O2 -Wall -Wextra -Werror)
target_compile_definitions(kaiman_static PRIVATE "KC_KAIMAN_VERSION=\"${KC_VERSION}\"")

if(NOT KC_LIBRARY_ONLY)
    add_library(kaiman_shared SHARED src/libkaiman.c)
    set_target_properties(kaiman_shared PROPERTIES
        OUTPUT_NAME "kaiman" WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_options(kaiman_shared PRIVATE -O2 -Wall -Wextra -Werror)
    target_compile_definitions(kaiman_shared PRIVATE "KC_KAIMAN_VERSION=\"${KC_VERSION}\"")
    target_link_options(kaiman_shared PRIVATE ${KC_LINK_FLAGS})
    target_link_libraries(kaiman_shared PRIVATE ${KC_LIBS} ${KC_SYSTEM_LINKS})

    add_executable(kaiman_cli src/main.c)
    set_target_properties(kaiman_cli PROPERTIES OUTPUT_NAME "kaiman")
    target_compile_options(kaiman_cli PRIVATE -O2 -Wall -Wextra -Werror)
    target_link_libraries(kaiman_cli PRIVATE kaiman_static ${KC_LIBS} ${KC_SYSTEM_LINKS})
endif()
