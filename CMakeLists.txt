cmake_minimum_required(VERSION 3.10)

project(areole
        VERSION 0.1.0
        DESCRIPTION "A GUI library in strict C89 with no graphics API"
        LANGUAGES C)

# The portable core is strict C89. Platform backends relax this per target,
# because <windows.h> does not compile with language extensions disabled.
set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- warnings -----------------------------------------------------------------
# areole is a library other people vendor into their build. It has no business
# emitting a warning, so warnings are errors here from the first commit.
function(areole_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors
                                                 -Wshadow -Wcast-qual -Wstrict-prototypes
                                                 -Wmissing-prototypes -Wwrite-strings)
    endif()
endfunction()

# --- core ---------------------------------------------------------------------
file(GLOB AREOLE_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_library(areole STATIC ${AREOLE_CORE_SOURCES})
target_include_directories(areole PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
areole_set_warnings(areole)

# --- tests --------------------------------------------------------------------
# Tests include src/ar_internal.h on purpose. They check invariants the public
# API deliberately does not expose, such as which end of the arena an allocation
# came from.
enable_testing()

add_executable(ar_test tests/ar_test.c)
target_include_directories(ar_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ar_test PRIVATE areole)
areole_set_warnings(ar_test)

add_test(NAME ar_test COMMAND ar_test)

# --- platform backend ---------------------------------------------------------
# Its own target because <windows.h> uses nameless unions and __int64 and will
# not compile with language extensions disabled. The core keeps the strict
# setting; only this target relaxes it.
if(WIN32)
    add_library(areole_win32 STATIC platform/ar_win32.c)
    target_link_libraries(areole_win32 PUBLIC areole)
    target_link_libraries(areole_win32 PUBLIC gdi32 user32 winmm)
    set_target_properties(areole_win32 PROPERTIES C_EXTENSIONS ON)
    areole_set_warnings(areole_win32)

    add_executable(example_hello examples/01_hello/main.c)
    target_link_libraries(example_hello PRIVATE areole_win32)
    areole_set_warnings(example_hello)
endif()
