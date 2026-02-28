cmake_minimum_required(VERSION 3.27)
project(aletheia_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ── idax dependency ─────────────────────────────────────────────────────
FetchContent_Declare(
    idax
    GIT_REPOSITORY https://github.com/19h/idax.git
    GIT_TAG        master
)

FetchContent_MakeAvailable(idax)

# ── Z3 dependency ───────────────────────────────────────────────────────
find_path(Z3_INCLUDE_DIR z3++.h
    PATHS /opt/homebrew/include /usr/local/include /usr/include
          "$ENV{Z3_ROOT}/include" "$ENV{PROGRAMFILES}/Z3/include")
if(NOT Z3_INCLUDE_DIR)
    message(FATAL_ERROR "Z3 headers not found. Install z3 or set Z3_ROOT.")
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

# ── Platform-specific IDA library names ──────────────────────────────────
if(WIN32)
    set(ALETHEIA_IDALIB_NAME "idalib.lib")
    set(ALETHEIA_IDA_NAME    "ida.lib")
    set(ALETHEIA_Z3_NAME     "libz3.lib")
elseif(APPLE)
    set(ALETHEIA_IDALIB_NAME "libidalib.dylib")
    set(ALETHEIA_IDA_NAME    "libida.dylib")
    set(ALETHEIA_Z3_NAME     "libz3.dylib")
else()
    set(ALETHEIA_IDALIB_NAME "libidalib.so")
    set(ALETHEIA_IDA_NAME    "libida.so")
    set(ALETHEIA_Z3_NAME     "libz3.so")
endif()

# ── Find Z3 library ─────────────────────────────────────────────────────
# Prefer the Z3 shipped with IDA (if available), then system locations.
set(_z3_search_paths /opt/homebrew/lib /usr/local/lib /usr/lib
    "$ENV{Z3_ROOT}/lib" "$ENV{Z3_ROOT}/bin" "$ENV{PROGRAMFILES}/Z3/lib")
if(IDA_INSTALL_DIR)
    list(INSERT _z3_search_paths 0 "${IDA_INSTALL_DIR}")
endif()
find_library(Z3_LIBRARY NAMES z3 libz3 PATHS ${_z3_search_paths})
if(NOT Z3_LIBRARY)
    message(FATAL_ERROR "Z3 library not found. Install z3 or set Z3_ROOT.")
endif()
message(STATUS "Z3 library: ${Z3_LIBRARY}")

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
target_include_directories(logos PUBLIC src/logos ${Z3_INCLUDE_DIR} ${IDAX_INC_DIRS})

add_library(idiomata STATIC
    src/idiomata/idioms.cpp
    src/idiomata/idiom_resolver.cpp
)
target_link_libraries(idiomata PUBLIC idax::idax)
target_include_directories(idiomata PUBLIC src/idiomata)

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
target_include_directories(aletheia PUBLIC src/aletheia src/common ${Z3_INCLUDE_DIR})

# ── IDA Plugin target (requires IDA installation) ───────────────────────
if(IDA_INSTALL_DIR AND COMMAND ida_add_plugin)
    ida_add_plugin(aletheia_ida
        SOURCES src/plugin/plugin.cpp
        LIBRARIES aletheia logos idiomata idax::idax "${IDA_INSTALL_DIR}/${ALETHEIA_Z3_NAME}"
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
target_include_directories(aletheia_core PUBLIC src/aletheia src/common ${Z3_INCLUDE_DIR})
# Provide idax include directories (for headers only) without linking the idax
# static library (which pulls in IDA kernel symbols like _callui).
target_include_directories(aletheia_core PUBLIC ${IDAX_INC_DIRS})

# ── Tests ────────────────────────────────────────────────────────────────
enable_testing()

# Headless unit tests (uses aletheia_core, no IDA runtime)
add_executable(aletheia_tests tests/test_main.cpp)
target_link_libraries(aletheia_tests aletheia_core logos "${Z3_LIBRARY}")
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
            "${IDA_INSTALL_DIR}/${ALETHEIA_Z3_NAME}"
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
            "${IDA_INSTALL_DIR}/${ALETHEIA_Z3_NAME}"
    )
    target_compile_definitions(idump PRIVATE __EA64__ IDALIB_IMPL)
    target_include_directories(idump PRIVATE "${IDASDK}/include")
    set_target_properties(idump PROPERTIES
        BUILD_RPATH "${IDA_INSTALL_DIR}"
        INSTALL_RPATH "${IDA_INSTALL_DIR}"
    )
endif()
