#include <iostream>
#include <string>

#include "BackfillAnalysisBattle.h"
#include "ReprojectUiRead.h"

namespace {

void PrintUsage(std::ostream& out) {
    savor::debugtool::PrintBackfillAnalysisBattleUsage(out);
    savor::debugtool::PrintUsage(out);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(std::cerr);
        return 2;
    }

    const std::string command = argv[1];
    if (command == "backfill-analysis-battle") {
        savor::debugtool::BackfillAnalysisBattleOptions options;
        std::string error;
        if (!savor::debugtool::ParseBackfillAnalysisBattleOptions(argc - 1, argv + 1, &options, &error)) {
            if (error != "help requested") {
                std::cerr << error << "\n";
            }
            savor::debugtool::PrintBackfillAnalysisBattleUsage(error == "help requested" ? std::cout : std::cerr);
            return error == "help requested" ? 0 : 2;
        }

        savor::debugtool::BackfillAnalysisBattlePlan plan;
        if (!savor::debugtool::BuildBackfillAnalysisBattlePlan(options, &plan, &error)) {
            std::cerr << error << "\n";
            return 2;
        }

        savor::debugtool::PrintBackfillAnalysisBattlePlan(plan, std::cout);

        savor::debugtool::BackfillAnalysisBattleResult result;
        if (!savor::debugtool::ExecuteBackfillAnalysisBattlePlan(plan, &result, &error)) {
            std::cerr << error << "\n";
            return 1;
        }

        savor::debugtool::PrintBackfillAnalysisBattleResult(result, std::cout);
        return 0;
    }

    if (command != "reproject-ui-read") {
        std::cerr << "unknown command: " << command << "\n";
        PrintUsage(std::cerr);
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
