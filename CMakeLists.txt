cmake_minimum_required(VERSION 3.27)
project(aletheia_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    idax
    SOURCE_DIR /Users/int/dev/idax
)

FetchContent_MakeAvailable(idax)

# Include Z3 headers (from Homebrew or system)
find_path(Z3_INCLUDE_DIR z3++.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
if(NOT Z3_INCLUDE_DIR)
    message(FATAL_ERROR "Z3 headers not found. Please install z3 (brew install z3)")
endif()

# Configure targets:
#   logos    — constraint/range/logic engine   (was dewolf_logic)
#   idiomata — compiler idiom pattern matcher  (was dewolf_idioms)
#   aletheia — core decompiler library         (was dewolf)

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

# IDA Plugin target
set(IDA_INSTALL_DIR "/Applications/IDA Professional 9.3.app/Contents/MacOS")
ida_add_plugin(aletheia_ida
    SOURCES src/plugin/plugin.cpp
    LIBRARIES aletheia logos idiomata idax::idax "${IDA_INSTALL_DIR}/libz3.dylib"
)

# aletheia_core: Everything except lifter.cpp and plugin.cpp (no idax runtime dependency)
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

# Rigorous Headless Tests (uses aletheia_core without idax/IDA runtime dependency)
add_executable(aletheia_tests tests/test_main.cpp)
target_link_libraries(aletheia_tests aletheia_core logos "${IDA_INSTALL_DIR}/libz3.dylib")
if(APPLE)
    set_target_properties(aletheia_tests PROPERTIES LINK_FLAGS "-Wl,-undefined,dynamic_lookup")
endif()

# Real Binary End-to-End Test via idalib
add_executable(aletheia_idalib_test tests/test_idalib.cpp)
target_link_libraries(aletheia_idalib_test
    PRIVATE
        aletheia
        logos
        idiomata
        idax::idax
        "${IDA_INSTALL_DIR}/libidalib.dylib"
        "${IDA_INSTALL_DIR}/libida.dylib"
        "${IDA_INSTALL_DIR}/libz3.dylib"
)
target_compile_definitions(aletheia_idalib_test PRIVATE __EA64__ IDALIB_IMPL)
target_include_directories(aletheia_idalib_test PRIVATE "${IDASDK}/include")
set_target_properties(aletheia_idalib_test PROPERTIES
    BUILD_RPATH "${IDA_INSTALL_DIR}"
    INSTALL_RPATH "${IDA_INSTALL_DIR}"
)

add_executable(idump src/cli/idump.cpp)
target_link_libraries(idump
    PRIVATE
        aletheia
        logos
        idiomata
        idax::idax
        "${IDA_INSTALL_DIR}/libidalib.dylib"
        "${IDA_INSTALL_DIR}/libida.dylib"
        "${IDA_INSTALL_DIR}/libz3.dylib"
)
target_compile_definitions(idump PRIVATE __EA64__ IDALIB_IMPL)
target_include_directories(idump PRIVATE "${IDASDK}/include")
set_target_properties(idump PROPERTIES
    BUILD_RPATH "${IDA_INSTALL_DIR}"
    INSTALL_RPATH "${IDA_INSTALL_DIR}"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(test_idiom_resolver tests/test_idiom_resolver.cpp)
target_link_libraries(test_idiom_resolver idiomata idax::idax)
