cmake_minimum_required(VERSION 3.10)

# One source of truth for the version, which is include/areole.h. The copy that
# used to live here said 0.1.0 while the header said 0.6.0, because two places
# to edit means one of them is always wrong.
file(STRINGS include/areole.h AR_VERSION_LINE
     REGEX "^#define AR_VERSION_STRING")
string(REGEX MATCH "[0-9]+\.[0-9]+\.[0-9]+" AR_VERSION "${AR_VERSION_LINE}")
if(NOT AR_VERSION)
    message(FATAL_ERROR "could not read AR_VERSION_STRING from include/areole.h")
endif()

project(areole
        VERSION ${AR_VERSION}
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

    add_executable(example_tour examples/02_tour/main.c)
    target_link_libraries(example_tour PRIVATE areole_win32)
    areole_set_warnings(example_tour)

    # The block corpus. A window by default, and --dump for the browser
    # comparison and for CI, which has no display.
    add_executable(example_block examples/03_block/main.c)
    target_link_libraries(example_block PRIVATE areole_win32)
    areole_set_warnings(example_block)

    # The snap corpus. Same shape as the block corpus: a window by default,
    # --dump for the browser comparison and for CI.
    add_executable(example_snap examples/05_snap/main.c)
    target_link_libraries(example_snap PRIVATE areole_win32)
    areole_set_warnings(example_snap)

    # The sticky corpus. Same shape again: window by default, --dump for the
    # browser comparison and for CI.
    add_executable(example_sticky examples/06_sticky/main.c)
    target_link_libraries(example_sticky PRIVATE areole_win32)
    areole_set_warnings(example_sticky)

    # The safe-area corpus. Same shape again.
    add_executable(example_env examples/07_env/main.c)
    target_link_libraries(example_env PRIVATE areole_win32)
    areole_set_warnings(example_env)

    # The anchor corpus. A real comparison: Edge implements anchor positioning.
    add_executable(example_anchor examples/08_anchor/main.c)
    target_link_libraries(example_anchor PRIVATE areole_win32)
    areole_set_warnings(example_anchor)

    add_executable(example_table examples/09_table/main.c)
    target_link_libraries(example_table PRIVATE areole_win32)
    areole_set_warnings(example_table)

    add_executable(example_showcase examples/04_showcase/main.c)
    target_link_libraries(example_showcase PRIVATE areole_win32)
    areole_set_warnings(example_showcase)
endif()

# --- benchmark ----------------------------------------------------------------
# A second build of the core with counters compiled in. The shipping library
# carries no counter in any inner loop, and this is how both claims stay true
# at once: the benchmark measures an instrumented build and publishes the
# overhead rather than assuming it away.
add_library(areole_instrumented STATIC ${AREOLE_CORE_SOURCES})
target_include_directories(areole_instrumented PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(areole_instrumented PUBLIC AR_INSTRUMENT)
areole_set_warnings(areole_instrumented)

file(GLOB AREOLE_BENCH_SCENES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench/scenes/*.c")

add_executable(ar_bench
    bench/ar_bench.c
    bench/bench_util.c
    bench/bench_time.c
    bench/bench_json.c
    ${AREOLE_BENCH_SCENES})
# src/ is on the path because the outline scenes measure ar_text, which is not
# public API yet -- ar_font_load is the public face of it and does not let a
# benchmark control the cache.
target_include_directories(ar_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench"
                                            "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ar_bench PRIVATE areole_instrumented)
areole_set_warnings(ar_bench)
if(WIN32)
    target_link_libraries(ar_bench PRIVATE winmm)
endif()
# bench_time.c is the one file here that includes a platform header, for the
# same reason the library has that rule.
set_source_files_properties(bench/bench_time.c PROPERTIES COMPILE_FLAGS "")
set_target_properties(ar_bench PROPERTIES C_EXTENSIONS ON)

add_executable(ar_hwprobe bench/ar_hwprobe.c bench/bench_util.c bench/bench_time.c)
target_include_directories(ar_hwprobe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
target_link_libraries(ar_hwprobe PRIVATE areole)
areole_set_warnings(ar_hwprobe)
if(WIN32)
    target_link_libraries(ar_hwprobe PRIVATE winmm)
endif()
set_target_properties(ar_hwprobe PROPERTIES C_EXTENSIONS ON)

add_executable(ar_bench_selftest bench/bench_selftest.c bench/bench_util.c bench/bench_time.c
                                 bench/bench_json.c)
target_include_directories(ar_bench_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
target_link_libraries(ar_bench_selftest PRIVATE areole_instrumented)
areole_set_warnings(ar_bench_selftest)
if(WIN32)
    target_link_libraries(ar_bench_selftest PRIVATE winmm)
endif()
set_target_properties(ar_bench_selftest PROPERTIES C_EXTENSIONS ON)

add_test(NAME ar_bench_selftest COMMAND ar_bench_selftest)

add_executable(ar_require bench/ar_require.c bench/bench_util.c bench/bench_time.c
                          bench/bench_json.c)
target_include_directories(ar_require PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
target_link_libraries(ar_require PRIVATE areole)
areole_set_warnings(ar_require)
if(WIN32)
    target_link_libraries(ar_require PRIVATE winmm)
endif()
set_target_properties(ar_require PROPERTIES C_EXTENSIONS ON)

# --- comparison harness -------------------------------------------------------
# Clay and microui are pure layout and build anywhere, so CI compiles them and
# they cannot rot. GDI needs Windows and is added only there; ar_compare.c
# guards the engine registration with _WIN32 to match.
add_executable(ar_compare
    bench/compare/ar_compare.c
    bench/compare/cmp_clay.c
    bench/compare/cmp_microui.c
    third_party/bench/microui.c
    bench/bench_util.c
    bench/bench_time.c
    bench/bench_json.c)
target_include_directories(ar_compare PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench"
                                              "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bench")
target_link_libraries(ar_compare PRIVATE areole_instrumented)
areole_set_warnings(ar_compare)
set_target_properties(ar_compare PROPERTIES C_EXTENSIONS ON)

if(WIN32)
    target_sources(ar_compare PRIVATE bench/compare/cmp_gdi.c)
    target_link_libraries(ar_compare PRIVATE gdi32 user32 msimg32 winmm)
endif()

# Neither rival is C89, and the specifics are worth keeping in the build file
# rather than only in a design document.
#
#   Clay     unnamed structs and unions, empty initializer braces, extra
#            semicolons at file scope -- it cannot compile under -pedantic at
#            all, which is the concrete reason areole was not built on it.
#   microui  long long, an unsuffixed unsigned constant, a const-discarding
#            qsort comparator.
#
# Two out of two of the libraries areole is measured against would fail its own
# build rules. Their translation units get relaxed flags; the library keeps C89.
#
# The flags have to be spelled per compiler, and were not: cl rejects
# -Wno-pedantic outright with "D8021: invalid numeric argument", so the msvc job
# could not build this target at all. A gcc spelling does not reach MSVC just
# because both compile C.
if(MSVC)
    # /W0 rather than a list of suppressions: MSVC names these warnings
    # differently, and enumerating them buys nothing on two vendored files
    # nobody here will edit. /std:c11 is what Clay's compound literals and
    # designated initializers need in C mode.
    set(AREOLE_VENDOR_CLAY "/std:c11 /W0")
    set(AREOLE_VENDOR_MICROUI "/std:c11 /W0")
else()
    set(AREOLE_VENDOR_CLAY
        "-std=gnu99 -Wno-pedantic -Wno-missing-prototypes -Wno-error -Wno-unused-parameter")
    set(AREOLE_VENDOR_MICROUI
        "-std=gnu99 -Wno-pedantic -Wno-cast-qual -Wno-shadow -Wno-long-long -Wno-error")
endif()

set_source_files_properties(bench/compare/cmp_clay.c PROPERTIES
    COMPILE_FLAGS "${AREOLE_VENDOR_CLAY}")
set_source_files_properties(third_party/bench/microui.c PROPERTIES
    COMPILE_FLAGS "${AREOLE_VENDOR_MICROUI}")
