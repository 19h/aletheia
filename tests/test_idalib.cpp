#include <ida/idax.hpp>
#include <ida/database.hpp>
#include <ida/name.hpp>
#include <ida/core.hpp>
#include <iostream>
#include <vector>

#include "../src/dewolf/dewolf.hpp"
#include "../src/dewolf/pipeline/pipeline.hpp"
#include "../src/dewolf/pipeline/optimization_stages.hpp"
#include "../src/dewolf/lifter.hpp"
#include "../src/dewolf/structuring/structuring_stage.hpp"
#include "../src/dewolf/codegen/codegen.hpp"

using namespace dewolf;

void test_function(const std::string& func_name) {
    std::cout << "\n=============================================\n";
    std::cout << "Testing DeWolf Pipeline on: " << func_name << "\n";
    std::cout << "=============================================\n";

    auto ea_res = ida::name::resolve(func_name);
    if (!ea_res || *ea_res == ida::BadAddress) {
        // macOS C symbols might have a prepended underscore
        ea_res = ida::name::resolve("_" + func_name);
        if (!ea_res || *ea_res == ida::BadAddress) {
            std::cerr << "[-] Failed to resolve function: " << func_name << std::endl;
            return;
        }
    }
    
    ida::Address ea = *ea_res;
    std::cout << "[+] Found " << func_name << " at 0x" << std::hex << ea << std::dec << "\n";

    DecompilerTask task(ea);
    dewolf_idioms::IdiomMatcher matcher;
    Lifter lifter(task.arena(), matcher);

    auto cfg_res = lifter.lift_function(ea);
    if (!cfg_res) {
        std::cerr << "[-] Failed to lift function\n";
        return;
    }
    
    std::cout << "[+] Function lifted successfully. Basic blocks: " << (*cfg_res)->blocks().size() << "\n";
    task.set_cfg(std::move(*cfg_res));

    // Run pipeline stages
    DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<PatternIndependentRestructuringStage>());
    
    pipeline.run(task);

    if (task.ast() && task.ast()->root()) {
        std::cout << "[+] Structuring successful. Generated AST.\n";
        CodeVisitor visitor;
        auto lines = visitor.generate_code(task.ast());
        std::cout << "\n[Decompiled C Code]\n";
        for (const auto& line : lines) {
            std::cout << line << "\n";
        }
    } else {
        std::cerr << "[-] Structuring failed to generate AST root.\n";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <binary_path>\n";
        return 1;
    }

    std::string binary_path = argv[1];
    
    // Initialize IDALib
    std::cout << "[*] Initializing IDA...\n";
    auto init_res = ida::database::init(argc, argv);
    if (!init_res) {
        std::cerr << "[-] Failed to init IDA: " << init_res.error().message << " | ctx: " << init_res.error().context << "\n";
        return 1;
    }

    std::cout << "[*] Opening database: " << binary_path << "\n";
    auto open_res = ida::database::open(binary_path, true); // true = run autoanalysis
    if (!open_res) {
        std::cerr << "[-] Failed to open database: " << open_res.error().message << "\n";
        return 1;
    }
    
    std::cout << "[+] Database opened and analyzed.\n";

    test_function("simple_math");
    test_function("diamond_cfg");
    test_function("loop_cfg");
    test_function("nested_cfg");

    return 0;
}
