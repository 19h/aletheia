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
    # Pinned for reproducible IDA/Sleigh API and build behavior.
    GIT_TAG        07a6f750cbdb9aa6a2eae32475ca53469b5a6cd6
)

FetchContent_MakeAvailable(idax)

option(ALETHEIA_ENABLE_PCODE "Enable experimental Sleigh-backed P-Code frontend when available" OFF)
option(ALETHEIA_PCODE_BUILD_SPECS "Build the Sleigh specifications required by the P-Code frontend" ON)
set(ALETHEIA_HAS_PCODE OFF)
set(ALETHEIA_SLEIGH_SECURITY_VERIFIED OFF)
set(ALETHEIA_SLEIGH_SOURCE_DIR "${idax_SOURCE_DIR}/third-party/sleigh"
    CACHE PATH "Path to Sleigh source tree for experimental P-Code support")
set(ALETHEIA_SLEIGH_SPEC_ROOT "" CACHE PATH
    "Root containing adjacent Ghidra processor .sla and .pspec files")

if(ALETHEIA_ENABLE_PCODE)
    if(NOT TARGET sleigh::sla)
        if(EXISTS "${ALETHEIA_SLEIGH_SOURCE_DIR}/CMakeLists.txt")
            # Ghidra 12.0.3's public Sleigh::oneInstruction path contains the
            # PcodeCacher UAF fixed in 12.1 (GHSA-gqh9-2c72-wpjc). Backport the
            # upstream deque-based pointer-stability fix through Sleigh's
            # supported additional-patch mechanism.
            set(_aletheia_sleigh_security_patch
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/ghidra-12.0.3-sleigh-pcodecacher-uaf.patch")
            set(_aletheia_sleigh_patches "${_aletheia_sleigh_security_patch}")
            if(DEFINED sleigh_ADDITIONAL_PATCHES AND sleigh_ADDITIONAL_PATCHES)
                foreach(_sleigh_patch IN LISTS sleigh_ADDITIONAL_PATCHES)
                    if(NOT _sleigh_patch STREQUAL _aletheia_sleigh_security_patch)
                        list(APPEND _aletheia_sleigh_patches "${_sleigh_patch}")
                    endif()
                endforeach()
            endif()
            set(sleigh_ADDITIONAL_PATCHES "${_aletheia_sleigh_patches}"
                CACHE STRING "Additional patches applied to the pinned Ghidra source" FORCE)
            set(sleigh_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
            set(sleigh_BUILD_EXTRATOOLS OFF CACHE BOOL "" FORCE)
            set(sleigh_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
            set(sleigh_BUILD_SUPPORT ON CACHE BOOL "" FORCE)
            set(sleigh_BUILD_SLEIGHSPECS ${ALETHEIA_PCODE_BUILD_SPECS} CACHE BOOL "" FORCE)
            add_subdirectory("${ALETHEIA_SLEIGH_SOURCE_DIR}"
                             "${CMAKE_CURRENT_BINARY_DIR}/third-party/sleigh"
                             EXCLUDE_FROM_ALL)

            FetchContent_GetProperties(GhidraSource)
            if(ghidrasource_SOURCE_DIR)
                set(_aletheia_sleigh_header
                    "${ghidrasource_SOURCE_DIR}/Ghidra/Features/Decompiler/src/decompile/cpp/sleigh.hh")
                if(NOT EXISTS "${_aletheia_sleigh_header}")
                    message(FATAL_ERROR
                        "Cannot verify Sleigh security state: expected header is missing: "
                        "${_aletheia_sleigh_header}")
                endif()
                file(READ "${_aletheia_sleigh_header}" _aletheia_sleigh_header_text)
                if(NOT _aletheia_sleigh_header_text MATCHES "deque<PcodeData>[ \t]+issued")
                    message(FATAL_ERROR
                        "Refusing vulnerable Sleigh source: PcodeCacher::issued is not pointer-stable "
                        "(GHSA-gqh9-2c72-wpjc / CVE-2026-49496).")
                endif()
                set(ALETHEIA_SLEIGH_SECURITY_VERIFIED ON)
            endif()
        else()
            find_package(sleigh QUIET COMPONENTS Support)
        endif()
    endif()

    if(TARGET sleigh::sla AND TARGET sleigh::decomp AND TARGET sleigh::support)
        if(NOT ALETHEIA_SLEIGH_SECURITY_VERIFIED)
            message(FATAL_ERROR
                "Refusing unverified external Sleigh targets. Set ALETHEIA_SLEIGH_SOURCE_DIR "
                "to the pinned source tree so the PcodeCacher security check can run.")
        endif()
        set(ALETHEIA_HAS_PCODE ON)
        message(STATUS "Experimental P-Code frontend enabled")
    else()
        message(FATAL_ERROR
            "ALETHEIA_ENABLE_PCODE=ON but required Sleigh targets are unavailable. "
            "Set ALETHEIA_SLEIGH_SOURCE_DIR to a compatible source tree.")
    endif()
else()
    message(STATUS "Experimental P-Code frontend disabled by ALETHEIA_ENABLE_PCODE=OFF")
endif()

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

    # Keep source-built Z3 on the same Release baseline as project libraries.
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

# ── Aggressive micro-optimizations ───────────────────────────────────────
# Keep this as an INTERFACE target so we can apply it selectively and avoid
# impacting compile hotspots like idiomata's large embedded pattern table.
add_library(aletheia_aggressive_opts INTERFACE)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Enabling Aggressive Micro-Optimizations")

    # Hide symbols by default to improve devirtualization/inlining in C++.
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aletheia_aggressive_opts INTERFACE
            "-march=native"
            "-mtune=native"
            "-ffunction-sections"
            "-fdata-sections"
            "-fomit-frame-pointer"
        )
        if(NOT APPLE)
            target_compile_options(aletheia_aggressive_opts INTERFACE "-fno-semantic-interposition")
        endif()
    elseif(MSVC)
        target_compile_options(aletheia_aggressive_opts INTERFACE
            "/Ob3"
            "/Gw"
            "/arch:AVX2"
        )
    endif()

    if(NOT APPLE AND NOT WIN32)
        target_link_options(aletheia_aggressive_opts INTERFACE
            "-Wl,--gc-sections"
            "-Wl,--icf=all"
        )
    elseif(APPLE)
        target_link_options(aletheia_aggressive_opts INTERFACE "-Wl,-dead_strip")
    elseif(WIN32)
        target_link_options(aletheia_aggressive_opts INTERFACE "/OPT:REF" "/OPT:ICF")
    endif()
endif()

# Apply aggressive flags to source-built Z3 directly to avoid introducing
# non-exported parent targets into Z3's own exported target set.
if(TARGET libz3 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(libz3 PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(libz3 PRIVATE
            "-march=native"
            "-mtune=native"
            "-ffunction-sections"
            "-fdata-sections"
            "-fomit-frame-pointer"
        )
        if(NOT APPLE)
            target_compile_options(libz3 PRIVATE "-fno-semantic-interposition")
        endif()
    elseif(MSVC)
        target_compile_options(libz3 PRIVATE
            "/Ob3"
            "/Gw"
            "/arch:AVX2"
        )
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
target_link_libraries(logos PUBLIC aletheia_z3 aletheia_aggressive_opts)
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
    src/aletheia/frontend/frontend.cpp
    src/aletheia/frontend/shared_support.cpp
    src/aletheia/lifter.cpp
    src/aletheia/pcode/pcode_lifter.cpp
    src/aletheia/pcode/pcode_lowering.cpp
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
    src/aletheia/codegen/portable_c.cpp
    src/aletheia/structuring/graph_slice/graph_slice.cpp
    src/aletheia/structuring/reaching_conditions/reaching_conditions.cpp
    src/aletheia/structuring/cbr/cbr.cpp
    src/aletheia/structuring/car/car.cpp
    src/aletheia/pipeline/expressions/graph_expression_folding.cpp
    src/aletheia/pipeline/dataflow_analysis/dead_code_elimination.cpp
    src/aletheia/debug/ir_serializer.cpp
    src/aletheia/debug/expr_tree.cpp
    src/aletheia/debug/stage_metrics.cpp
    src/aletheia/debug/ir_diff.cpp
    src/aletheia/debug/ir_invariants.cpp
    src/aletheia/debug/variable_provenance.cpp
    src/aletheia/debug/ir_query.cpp
    src/aletheia/debug/debug_observer.cpp
)
target_link_libraries(aletheia PUBLIC idax::idax logos idiomata aletheia_aggressive_opts)
if(ALETHEIA_HAS_PCODE)
    target_link_libraries(aletheia PRIVATE sleigh::sla sleigh::decomp sleigh::support)
    if(ALETHEIA_PCODE_BUILD_SPECS AND TARGET sleigh_spec_AARCH64)
        add_dependencies(aletheia sleigh_spec_AARCH64)
    endif()
    if(ALETHEIA_SLEIGH_SPEC_ROOT)
        target_compile_definitions(aletheia PRIVATE
            ALETHEIA_SLEIGH_SPEC_ROOT_DIR="${ALETHEIA_SLEIGH_SPEC_ROOT}")
    elseif(ALETHEIA_PCODE_BUILD_SPECS)
        target_compile_definitions(aletheia PRIVATE
            ALETHEIA_SLEIGH_SPEC_ROOT_DIR="${CMAKE_CURRENT_BINARY_DIR}/third-party/sleigh/specfiles")
    endif()
endif()
target_include_directories(aletheia PUBLIC src/aletheia src/common ${ALETHEIA_Z3_INCLUDE_DIRS})
target_compile_options(aletheia PRIVATE $<$<CONFIG:Release>:-O3>)
target_compile_definitions(aletheia PRIVATE ALETHEIA_HAS_PCODE=$<BOOL:${ALETHEIA_HAS_PCODE}>)

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
    src/aletheia/pcode/pcode_lowering.cpp
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
    src/aletheia/codegen/portable_c.cpp
    src/aletheia/structuring/graph_slice/graph_slice.cpp
    src/aletheia/structuring/reaching_conditions/reaching_conditions.cpp
    src/aletheia/structuring/cbr/cbr.cpp
    src/aletheia/structuring/car/car.cpp
    src/aletheia/pipeline/expressions/graph_expression_folding.cpp
    src/aletheia/pipeline/dataflow_analysis/dead_code_elimination.cpp
    src/aletheia/debug/ir_serializer.cpp
    src/aletheia/debug/expr_tree.cpp
    src/aletheia/debug/stage_metrics.cpp
    src/aletheia/debug/ir_diff.cpp
    src/aletheia/debug/ir_invariants.cpp
    src/aletheia/debug/variable_provenance.cpp
    src/aletheia/debug/ir_query.cpp
    src/aletheia/debug/debug_observer.cpp
)
target_link_libraries(aletheia_core PUBLIC logos aletheia_aggressive_opts)
target_include_directories(aletheia_core PUBLIC src/aletheia src/common ${ALETHEIA_Z3_INCLUDE_DIRS})
target_compile_options(aletheia_core PRIVATE $<$<CONFIG:Release>:-O3>)
target_compile_definitions(aletheia_core PUBLIC ALETHEIA_HAS_PCODE=0)
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

# Debug infrastructure unit tests (uses aletheia_core, no IDA runtime)
add_executable(aletheia_debug_tests tests/test_debug.cpp)
target_link_libraries(aletheia_debug_tests aletheia_core logos)
if(APPLE)
    set_target_properties(aletheia_debug_tests PROPERTIES LINK_FLAGS "-Wl,-undefined,dynamic_lookup")
endif()
add_test(NAME aletheia_debug_tests COMMAND aletheia_debug_tests)

# Raw P-Code lowering tests are deliberately independent of Sleigh and IDA.
add_executable(aletheia_pcode_lowering_tests tests/test_pcode_lowering.cpp)
target_link_libraries(aletheia_pcode_lowering_tests aletheia_core logos)
if(APPLE)
    set_target_properties(aletheia_pcode_lowering_tests PROPERTIES
        LINK_FLAGS "-Wl,-undefined,dynamic_lookup")
endif()
add_test(NAME aletheia_pcode_lowering_tests COMMAND aletheia_pcode_lowering_tests)

# Portable-C codegen + runtime differential fixture (no IDA runtime).
add_executable(aletheia_portable_c_fixture_generator
    tests/portable_c_fixture_generator.cpp)
target_link_libraries(aletheia_portable_c_fixture_generator aletheia_core logos)
if(APPLE)
    set_target_properties(aletheia_portable_c_fixture_generator PROPERTIES
        LINK_FLAGS "-Wl,-undefined,dynamic_lookup")
endif()
find_program(ALETHEIA_PORTABLE_C_COMPILER NAMES clang cc)
if(ALETHEIA_PORTABLE_C_COMPILER)
    set(ALETHEIA_PORTABLE_C_SDKROOT "")
    if(APPLE)
        execute_process(
            COMMAND xcrun --show-sdk-path
            OUTPUT_VARIABLE ALETHEIA_PORTABLE_C_SDKROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    add_test(NAME aletheia_portable_c_runtime_test
        COMMAND "${CMAKE_COMMAND}"
            "-DGENERATOR=$<TARGET_FILE:aletheia_portable_c_fixture_generator>"
            "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/portable-c-runtime-test"
            "-DC_COMPILER=${ALETHEIA_PORTABLE_C_COMPILER}"
            "-DSDKROOT=${ALETHEIA_PORTABLE_C_SDKROOT}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_portable_c_runtime.cmake")
else()
    message(WARNING "Portable-C runtime gate disabled: no clang/cc executable found")
endif()

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

    if(ALETHEIA_HAS_PCODE)
        if(ALETHEIA_PORTABLE_C_COMPILER)
            add_test(NAME aletheia_portable_c_test
                COMMAND "${CMAKE_COMMAND}"
                    "-DIDUMP=$<TARGET_FILE:idump>"
                    "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/targets/test_binary"
                    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/portable-c-test"
                    "-DC_COMPILER=${ALETHEIA_PORTABLE_C_COMPILER}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_portable_c.cmake")
            set_tests_properties(aletheia_portable_c_test PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 600
                ENVIRONMENT "IDADIR=${IDA_INSTALL_DIR}")
        else()
            message(WARNING "Portable-C compiler gate disabled: no clang/cc executable found")
        endif()
    endif()
endif()
