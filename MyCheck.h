#ifndef MYCHECK_H
#define MYCHECK_H

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"

class MyCheck : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
    void registerMatchers(clang::ast_matchers::MatchFinder& Finder);
    void run(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

class MyFrontendAction : public clang::ASTFrontendAction {
public:
    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance& CI, clang::StringRef file) override;
};

#endif
