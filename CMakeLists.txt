cmake_minimum_required(VERSION 3.27)
project(aletheia_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Linker Optimization ────────────────────────────────────────────────
# mold (https://github.com/rui314/mold) is an extremely fast ELF linker
# but does NOT support macOS (Mach-O). Only use it on Linux/FreeBSD.
# On macOS, the system linker (ld) is the only real option; ld_new is
# available on newer Xcode toolchains and is faster for large archives.
if(NOT APPLE AND NOT WIN32)
    find_program(MOLD_LINKER mold
        HINTS "/usr/local/bin" "/usr/bin"
        DOC "Path to the mold linker"
    )
    if(MOLD_LINKER)
        message(STATUS "Using mold linker: ${MOLD_LINKER}")
        add_link_options("-fuse-ld=${MOLD_LINKER}")
    else()
        message(STATUS "mold linker not found — falling back to system default")
    endif()
elseif(APPLE)
    message(STATUS "Using system linker (macOS)")
endif()



include(FetchContent)

# Prevent FetchContent from re-checking remote repositories on every build.
# Without this, ninja triggers cmake reconfiguration on each invocation to
# check if upstream repos have changed, which stalls the build for seconds
# (or minutes if the network is slow). Run `cmake --fresh` to force updates.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Do not re-check FetchContent remotes on every build" FORCE)

# ── idax dependency ─────────────────────────────────────────────────────
FetchContent_Declare(
    idax
    GIT_REPOSITORY https://github.com/19h/idax.git
    GIT_TAG        master
)

FetchContent_MakeAvailable(idax)

# ── Z3 dependency ───────────────────────────────────────────────────────
# Prefer a system-installed Z3 (e.g. Homebrew, apt, vcpkg) for vastly faster
# builds. Building Z3 from source compiles ~860 files into a 50+ MB archive
# and can take 10-15 minutes even on fast hardware.
#
# Pass -DALETHEIA_Z3_FROM_SOURCE=ON to force building from source.
option(ALETHEIA_Z3_FROM_SOURCE "Force building Z3 from source instead of using system Z3" OFF)

set(_z3_found_system FALSE)

#if(NOT ALETHEIA_Z3_FROM_SOURCE)
#    find_package(Z3 4.12 CONFIG QUIET
#        HINTS "/opt/homebrew" "/usr/local" "$ENV{HOMEBREW_PREFIX}"
#    )
#    if(Z3_FOUND AND TARGET z3::libz3)
#        set(_z3_found_system TRUE)
#        message(STATUS "Using system Z3 ${Z3_VERSION_STRING} (${Z3_C_INCLUDE_DIRS})")
#        # Create a compatibility alias so downstream targets can use the same
#        # name regardless of how Z3 was resolved.
#        if(NOT TARGET aletheia_z3)
#            add_library(aletheia_z3 ALIAS z3::libz3)
#        endif()
#        # Expose include dirs for targets that reference them explicitly.
#        set(ALETHEIA_Z3_INCLUDE_DIRS ${Z3_C_INCLUDE_DIRS})
#    else()
#        message(STATUS "System Z3 not found — falling back to building from source")
#    endif()
#endif()

if(NOT _z3_found_system)
    message(STATUS "Building Z3 from source (this will take a while...)")
    FetchContent_Declare(
        z3
        GIT_REPOSITORY https://github.com/Z3Prover/z3.git
        GIT_TAG        z3-4.16.0
    )

    set(Z3_BUILD_LIBZ3_SHARED OFF CACHE BOOL "Build static Z3" FORCE)
    set(Z3_BUILD_EXECUTABLE OFF CACHE BOOL "Skip Z3 executable" FORCE)
    set(Z3_BUILD_TEST_EXECUTABLES OFF CACHE BOOL "Skip Z3 tests" FORCE)
    set(Z3_ENABLE_EXAMPLE_TARGETS OFF CACHE BOOL "Skip Z3 examples" FORCE)
    set(Z3_BUILD_DOCUMENTATION OFF CACHE BOOL "Skip Z3 docs" FORCE)
    set(WARNINGS_AS_ERRORS OFF CACHE BOOL "Disable warnings as errors" FORCE)

    FetchContent_MakeAvailable(z3)

    # Force Z3 to use the same aggressive optimizations as the rest of the project
    if(TARGET libz3)
        target_compile_options(libz3 PRIVATE $<$<CONFIG:Release>:-O3>)
    endif()

    # The FetchContent Z3 exposes a target called "libz3" (not z3::libz3).
    if(NOT TARGET aletheia_z3)
        add_library(aletheia_z3 ALIAS libz3)
    endif()
    set(ALETHEIA_Z3_INCLUDE_DIRS ${z3_SOURCE_DIR}/src/api ${z3_SOURCE_DIR}/src/api/c++)
endif()

# ── IDA installation (optional — needed for plugin, idalib tests, idump) ─
# Resolve IDA_INSTALL_DIR from explicit setting, IDADIR env, or well-known
# platform paths. Targets that need the IDA runtime are guarded behind this.
if(NOT DEFINED IDA_INSTALL_DIR)
    if(DEFINED ENV{IDADIR})
        set(IDA_INSTALL_DIR "$ENV{IDADIR}")
    elseif(APPLE)
        foreach(_ver 9.3 9.2 9.1 9.0)
            set(_try "/Applications/IDA Professional ${_ver}.app/Contents/MacOS")
            if(EXISTS "${_try}/libida.dylib")
                set(IDA_INSTALL_DIR "${_try}")
                break()
            endif()
        endforeach()
    endif()
endif()

if(IDA_INSTALL_DIR)
    message(STATUS "IDA installation: ${IDA_INSTALL_DIR}")
else()
    message(STATUS "IDA installation not found — plugin, idalib test, and idump targets will be skipped.")
endif()

# ── Interprocedural Optimization (LTO) ──────────────────────────────────
# Enable LTO for Release builds to allow cross-translation-unit inlining.
# We set this AFTER fetching Z3 so that Z3 itself isn't built with LTO,
# which would massively and unnecessarily inflate link times.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        message(STATUS "Enabling Interprocedural Optimization (LTO)")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "IPO / LTO not supported: ${ipo_error}")
    endif()
endif()

# ── Platform-specific IDA library names ──────────────────────────────────
if(WIN32)
    set(ALETHEIA_IDALIB_NAME "idalib.lib")
    set(ALETHEIA_IDA_NAME    "ida.lib")

elseif(APPLE)
    set(ALETHEIA_IDALIB_NAME "libidalib.dylib")
    set(ALETHEIA_IDA_NAME    "libida.dylib")

else()
    set(ALETHEIA_IDALIB_NAME "libidalib.so")
    set(ALETHEIA_IDA_NAME    "libida.so")

endif()



# ── Configure targets ───────────────────────────────────────────────────
#   logos    — constraint/range/logic engine
#   idiomata — compiler idiom pattern matcher
#   aletheia — core decompiler library

# Capture idax include dirs before creating targets
get_target_property(IDAX_INC_DIRS idax::idax INTERFACE_INCLUDE_DIRECTORIES)

add_library(logos STATIC
    src/logos/logic.cpp
    src/logos/dag.cpp
    src/logos/world.cpp
    src/logos/range_simplifier.cpp
    src/logos/bitwise_simplifier.cpp
    src/logos/z3_logic.cpp
    src/logos/operation_simplifier.cpp
    src/logos/normal_form.cpp
)
target_link_libraries(logos PUBLIC aletheia_z3)
target_include_directories(logos PUBLIC src/logos ${IDAX_INC_DIRS} ${ALETHEIA_Z3_INCLUDE_DIRS})
target_compile_options(logos PRIVATE $<$<CONFIG:Release>:-O3>)

add_library(idiomata STATIC
    src/idiomata/idioms.cpp
    src/idiomata/idiom_resolver.cpp
    src/idiomata/patterns_data.cpp
)
target_link_libraries(idiomata PUBLIC idax::idax)
target_include_directories(idiomata PUBLIC src/idiomata)
# NOTE: idiomata contains a 37K-line embedded pattern table (patterns.hpp).
# -O3 causes the compiler to spend 5+ minutes optimizing static data init
# that gains nothing from optimization. Use -O1 which compiles in seconds.
target_compile_options(idiomata PRIVATE $<$<CONFIG:Release>:-O1>)

add_library(aletheia STATIC
    src/aletheia/aletheia.cpp
    src/common/types.cpp
    src/common/arena.cpp
    src/aletheia/structures/cfg.cpp
    src/aletheia/lifter.cpp
    src/aletheia/ssa/dominators.cpp
    src/aletheia/ssa/liveness/liveness.cpp
    src/aletheia/ssa/ssa_constructor.cpp
    src/aletheia/ssa/ssa_destructor.cpp
    src/aletheia/ssa/minimal_variable_renamer.cpp
    src/aletheia/ssa/conditional_variable_renamer.cpp
    src/aletheia/ssa/phi_dependency_resolver.cpp
    src/aletheia/pipeline/pipeline.cpp
    src/aletheia/pipeline/preprocessing_stages.cpp
    src/aletheia/pipeline/optimization_stages.cpp
    src/aletheia/structuring/ast.cpp
    src/aletheia/structuring/ast_processor.cpp
    src/aletheia/structuring/loop_structurer.cpp
    src/aletheia/structuring/structurer.cpp
    src/aletheia/structuring/structuring_stage.cpp
    src/aletheia/structuring/instruction_length_handler.cpp
    src/aletheia/structuring/variable_name_generation.cpp
    src/aletheia/structuring/loop_name_generator.cpp
    src/aletheia/codegen/codegen.cpp
    src/aletheia/structuring/graph_slice/graph_slice.cpp
    src/aletheia/structuring/reaching_conditions/reaching_conditions.cpp
    src/aletheia/structuring/cbr/cbr.cpp
    src/aletheia/structuring/car/car.cpp
    src/aletheia/pipeline/expressions/graph_expression_folding.cpp
    src/aletheia/pipeline/dataflow_analysis/dead_code_elimination.cpp
)
target_link_libraries(aletheia PUBLIC idax::idax logos idiomata)
target_include_directories(aletheia PUBLIC src/aletheia src/common ${ALETHEIA_Z3_INCLUDE_DIRS})
target_compile_options(aletheia PRIVATE $<$<CONFIG:Release>:-O3>)

# ── IDA Plugin target (requires IDA installation) ───────────────────────
if(IDA_INSTALL_DIR AND COMMAND ida_add_plugin)
    ida_add_plugin(aletheia_ida
        SOURCES src/plugin/plugin.cpp
        LIBRARIES aletheia logos idiomata idax::idax
    )
endif()

# ── aletheia_core: no idax runtime dependency ───────────────────────────
# Everything except lifter.cpp and plugin.cpp. Compiles against idax
# headers only (no IDA kernel symbols).
add_library(aletheia_core STATIC
    src/aletheia/aletheia.cpp
    src/common/types.cpp
    src/common/arena.cpp
    src/aletheia/structures/cfg.cpp
    src/aletheia/ssa/dominators.cpp
    src/aletheia/ssa/liveness/liveness.cpp
    src/aletheia/ssa/ssa_constructor.cpp
    src/aletheia/ssa/ssa_destructor.cpp
    src/aletheia/ssa/minimal_variable_renamer.cpp
    src/aletheia/ssa/conditional_variable_renamer.cpp
    src/aletheia/ssa/phi_dependency_resolver.cpp
    src/aletheia/pipeline/pipeline.cpp
    src/aletheia/pipeline/preprocessing_stages.cpp
    src/aletheia/pipeline/optimization_stages.cpp
    src/aletheia/structuring/ast.cpp
    src/aletheia/structuring/ast_processor.cpp
    src/aletheia/structuring/loop_structurer.cpp
    src/aletheia/structuring/structurer.cpp
    src/aletheia/structuring/structuring_stage.cpp
    src/aletheia/structuring/instruction_length_handler.cpp
    src/aletheia/structuring/variable_name_generation.cpp
    src/aletheia/structuring/loop_name_generator.cpp
    src/aletheia/codegen/codegen.cpp
    src/aletheia/structuring/graph_slice/graph_slice.cpp
    src/aletheia/structuring/reaching_conditions/reaching_conditions.cpp
    src/aletheia/structuring/cbr/cbr.cpp
    src/aletheia/structuring/car/car.cpp
    src/aletheia/pipeline/expressions/graph_expression_folding.cpp
    src/aletheia/pipeline/dataflow_analysis/dead_code_elimination.cpp
)
target_link_libraries(aletheia_core PUBLIC logos)
target_include_directories(aletheia_core PUBLIC src/aletheia src/common ${ALETHEIA_Z3_INCLUDE_DIRS})
target_compile_options(aletheia_core PRIVATE $<$<CONFIG:Release>:-O3>)
# Provide idax include directories (for headers only) without linking the idax
# static library (which pulls in IDA kernel symbols like _callui).
target_include_directories(aletheia_core PUBLIC ${IDAX_INC_DIRS})

# ── Tests ────────────────────────────────────────────────────────────────
enable_testing()

# Headless unit tests (uses aletheia_core, no IDA runtime)
add_executable(aletheia_tests tests/test_main.cpp)
target_link_libraries(aletheia_tests aletheia_core logos)
if(APPLE)
    set_target_properties(aletheia_tests PROPERTIES LINK_FLAGS "-Wl,-undefined,dynamic_lookup")
endif()
add_test(NAME aletheia_unit_tests COMMAND aletheia_tests)

# Idiom resolver unit test
add_executable(test_idiom_resolver tests/test_idiom_resolver.cpp)
target_link_libraries(test_idiom_resolver idiomata idax::idax)
add_test(NAME idiom_resolver_tests COMMAND test_idiom_resolver)

# ── IDA-runtime targets (require IDA installation) ──────────────────────
if(IDA_INSTALL_DIR)
    # Real Binary End-to-End Test via idalib
    add_executable(aletheia_idalib_test tests/test_idalib.cpp)
    target_link_libraries(aletheia_idalib_test
        PRIVATE
            aletheia logos idiomata idax::idax
            "${IDA_INSTALL_DIR}/${ALETHEIA_IDALIB_NAME}"
            "${IDA_INSTALL_DIR}/${ALETHEIA_IDA_NAME}"

    )
    target_compile_definitions(aletheia_idalib_test PRIVATE __EA64__ IDALIB_IMPL)
    target_include_directories(aletheia_idalib_test PRIVATE "${IDASDK}/include")
    set_target_properties(aletheia_idalib_test PROPERTIES
        BUILD_RPATH "${IDA_INSTALL_DIR}"
        INSTALL_RPATH "${IDA_INSTALL_DIR}"
    )
    add_test(NAME aletheia_idalib_test
        COMMAND aletheia_idalib_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/targets/test_binary"
    )
    if(WIN32)
        set_tests_properties(aletheia_idalib_test PROPERTIES
            ENVIRONMENT "IDADIR=${IDA_INSTALL_DIR};PATH=${IDA_INSTALL_DIR}\;$ENV{PATH}"
        )
    elseif(NOT APPLE)
        set_tests_properties(aletheia_idalib_test PROPERTIES
            ENVIRONMENT "IDADIR=${IDA_INSTALL_DIR};LD_LIBRARY_PATH=${IDA_INSTALL_DIR}:$ENV{LD_LIBRARY_PATH}"
        )
    else()
        set_tests_properties(aletheia_idalib_test PROPERTIES
            ENVIRONMENT "IDADIR=${IDA_INSTALL_DIR}"
        )
    endif()

    # idump CLI
    add_executable(idump src/cli/idump.cpp)
    target_link_libraries(idump
        PRIVATE
            aletheia logos idiomata idax::idax
            "${IDA_INSTALL_DIR}/${ALETHEIA_IDALIB_NAME}"
            "${IDA_INSTALL_DIR}/${ALETHEIA_IDA_NAME}"

    )
    target_compile_definitions(idump PRIVATE __EA64__ IDALIB_IMPL)
    target_include_directories(idump PRIVATE "${IDASDK}/include")
    set_target_properties(idump PROPERTIES
        BUILD_RPATH "${IDA_INSTALL_DIR}"
        INSTALL_RPATH "${IDA_INSTALL_DIR}"
    )
endif()
