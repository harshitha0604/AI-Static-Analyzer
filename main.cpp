#include "MyCheck.h"

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

int main(int argc, const char **argv) {

    llvm::cl::OptionCategory ToolCategory("my-checker");

    auto ExpectedParser =
        CommonOptionsParser::create(argc, argv, ToolCategory);

    if (!ExpectedParser) {
        llvm::errs() << "Error creating parser\n";
        return 1;
    }

    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());

    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}