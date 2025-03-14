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
target_include_directories(ar_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench")
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
# Windows only for now: GDI is the one rival that needs no vendoring, and it is
# the comparison a prospective user cares about most. The others arrive as they
# are vendored under third_party/bench/.
if(WIN32)
    add_executable(ar_compare
        bench/compare/ar_compare.c
        bench/compare/cmp_gdi.c
        bench/compare/cmp_clay.c
        bench/compare/cmp_microui.c
        third_party/bench/microui.c
        bench/bench_util.c
        bench/bench_time.c
        bench/bench_json.c)
    target_include_directories(ar_compare PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bench"
                                                  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bench")
    target_link_libraries(ar_compare PRIVATE areole_instrumented gdi32 user32 msimg32 winmm)
    areole_set_warnings(ar_compare)
    set_target_properties(ar_compare PROPERTIES C_EXTENSIONS ON)
    # Clay is C99 and uses compound literals throughout, which is exactly why
    # areole could not be built on it. Its translation unit gets its own
    # standard; the library keeps C89.
    # Clay cannot compile under -pedantic at all: it uses unnamed structs and
    # unions and empty initializer braces throughout. That is not a criticism of
    # Clay, it is the concrete reason areole could not be built on it, and it is
    # worth seeing in the build file rather than only in a design document.
    # microui is not C89 either: long long, an unsuffixed unsigned constant, a
    # const-discarding qsort comparator. Two out of two of the libraries areole
    # is measured against would have to be relaxed to build under its own rules,
    # which is the argument for the rules stated as a build fact.
    set_source_files_properties(third_party/bench/microui.c PROPERTIES
        COMPILE_FLAGS "-std=gnu99 -Wno-pedantic -Wno-cast-qual -Wno-shadow -Wno-long-long -Wno-error")

    set_source_files_properties(bench/compare/cmp_clay.c PROPERTIES
        COMPILE_FLAGS "-std=gnu99 -Wno-pedantic -Wno-missing-prototypes -Wno-error -Wno-unused-parameter")
endif()
