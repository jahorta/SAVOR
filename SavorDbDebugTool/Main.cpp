#include <iostream>
#include <string>

#include "ReprojectUiRead.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        savor::debugtool::PrintUsage(std::cerr);
        return 2;
    }

    const std::string command = argv[1];
    if (command != "reproject-ui-read") {
        std::cerr << "unknown command: " << command << "\n";
        savor::debugtool::PrintUsage(std::cerr);
        return 2;
    }

    savor::debugtool::ReprojectUiReadOptions options;
    std::string error;
    if (!savor::debugtool::ParseOptions(argc - 1, argv + 1, &options, &error)) {
        if (error != "help requested") {
            std::cerr << error << "\n";
        }
        savor::debugtool::PrintUsage(error == "help requested" ? std::cout : std::cerr);
        return error == "help requested" ? 0 : 2;
    }

    savor::debugtool::ReprojectUiReadPlan plan;
    if (!savor::debugtool::BuildPlan(options, &plan, &error)) {
        std::cerr << error << "\n";
        return 2;
    }

    savor::debugtool::PrintPlan(plan, std::cout);

    savor::debugtool::ReprojectUiReadResult result;
    if (!savor::debugtool::ExecutePlan(plan, &result, &error)) {
        std::cerr << error << "\n";
        return 1;
    }

    savor::debugtool::PrintResult(result, std::cout);
    return 0;
}
