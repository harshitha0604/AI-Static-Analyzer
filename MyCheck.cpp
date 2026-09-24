#include "MyCheck.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Lexer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace clang;
using namespace clang::ast_matchers;

// ============================================================
// AI Reviewer
// ============================================================
struct MISRAViolation {
    unsigned LineNumber;
    std::string RuleNumber;
    std::string WarningMessage;
    std::string SourceLine;
};

struct AIReviewResult {
    unsigned LineNumber;
    std::string CorrectedLine;
    std::string ExpectedOutput;
};

static std::vector<MISRAViolation> gViolations;
static std::vector<std::string> gReportedWarningLines;
static std::string gInputFilePath;
static bool gReportedRule106 = false;

static std::string trimCopy(std::string Text) {
    const char* WS = " \t\r\n";
    const std::size_t First = Text.find_first_not_of(WS);
    if (First == std::string::npos) return "";
    const std::size_t Last = Text.find_last_not_of(WS);
    return Text.substr(First, Last - First + 1);
}

static std::string extractRuleNumber(const std::string& Message) {
    const std::size_t RulePos = Message.find("Rule ");
    if (RulePos == std::string::npos) return "";

    const std::size_t Start = RulePos + 5;
    std::size_t End = Message.find(':', Start);
    if (End == std::string::npos) End = Message.find(' ', Start);
    if (End == std::string::npos) return Message.substr(Start);
    return Message.substr(Start, End - Start);
}

static std::string getSourceLine(SourceLocation Loc,
    const SourceManager& SM,
    const LangOptions& LangOpts) {
    if (Loc.isInvalid()) return "";

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);
    if (SpellingLoc.isInvalid()) return "";

    const unsigned Line = SM.getSpellingLineNumber(SpellingLoc);
    FileID FID = SM.getFileID(SpellingLoc);
    SourceLocation LineStart = SM.translateLineCol(FID, Line, 1);
    SourceLocation NextLine = SM.translateLineCol(FID, Line + 1, 1);
    if (LineStart.isInvalid() || NextLine.isInvalid()) return "";

    bool Invalid = false;
    StringRef Text = Lexer::getSourceText(
        CharSourceRange::getCharRange(LineStart, NextLine),
        SM, LangOpts, &Invalid);
    if (Invalid) return "";

    return trimCopy(Text.str());
}

static std::string getWarningLineKey(SourceLocation Loc,
    const SourceManager& SM) {
    if (Loc.isInvalid()) return "";

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);
    if (SpellingLoc.isInvalid()) return "";

    return SM.getFilename(SpellingLoc).str() + ":" +
        std::to_string(SM.getSpellingLineNumber(SpellingLoc));
}

static bool claimWarningLine(SourceLocation Loc, const SourceManager& SM) {
    const std::string Key = getWarningLineKey(Loc, SM);
    if (Key.empty()) return false;

    for (const std::string& Existing : gReportedWarningLines) {
        if (Existing == Key)
            return false;
    }

    gReportedWarningLines.push_back(Key);
    return true;
}

static void recordViolation(SourceLocation Loc,
    const char* Message,
    const SourceManager& SM,
    const LangOptions& LangOpts) {
    if (Loc.isInvalid()) return;
    if (SM.isInSystemHeader(Loc)) return;

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);
    if (SpellingLoc.isInvalid()) return;

    MISRAViolation V;
    V.LineNumber = SM.getSpellingLineNumber(SpellingLoc);
    V.WarningMessage = Message;
    V.RuleNumber = extractRuleNumber(V.WarningMessage);
    V.SourceLine = getSourceLine(Loc, SM, LangOpts);
    gViolations.push_back(V);
}

static bool isSingleLineCodeCorrection(const std::string& Text) {
    std::string Line = trimCopy(Text);
    if (Line.empty()) return false;
    if (Line.find('\n') != std::string::npos || Line.find('\r') != std::string::npos)
        return false;
    if (Line.rfind("#", 0) == 0 || Line.rfind("```", 0) == 0)
        return false;
    if (Line.rfind("Line ", 0) == 0 ||
        Line.rfind("Output:", 0) == 0 ||
        Line.rfind("Corrected Line:", 0) == 0)
        return false;
    if (Line.find("AI generated") != std::string::npos ||
        Line.find("warning") != std::string::npos ||
        Line.find("error") != std::string::npos ||
        Line.find("must ") != std::string::npos ||
        Line.find("shall ") != std::string::npos)
        return false;
    return true;
}

static bool isAssignmentToken(const std::string& Line, std::size_t Pos) {
    if (Line[Pos] != '=') return false;
    const char Prev = Pos == 0 ? '\0' : Line[Pos - 1];
    const char Next = Pos + 1 >= Line.size() ? '\0' : Line[Pos + 1];
    return Prev != '=' && Prev != '!' && Prev != '<' && Prev != '>' && Next != '=';
}

static std::string buildLocalCorrection(const MISRAViolation& V) {
    std::string Line = trimCopy(V.SourceLine);
    if (Line.empty()) return "";

    if (V.RuleNumber == "27") {
        for (std::size_t I = 0; I < Line.size(); ++I) {
            if (isAssignmentToken(Line, I)) {
                Line.replace(I, 1, "==");
                return Line;
            }
        }
    }

    if (V.RuleNumber == "8") {
        const std::string Prefix = "int ";
        if (Line.rfind(Prefix, 0) == 0)
            return "int32_t " + Line.substr(Prefix.size());
    }

    if (V.RuleNumber == "181") {
        if (Line.find('{') == std::string::npos)
            return Line + " {";
    }

    if (V.RuleNumber == "182") {
        if (Line == "else")
            return "else {";
    }

    return "";
}

static bool shouldPreferViolation(const MISRAViolation& Candidate,
    const MISRAViolation& Current) {
    if (Candidate.RuleNumber == "27" && Current.RuleNumber != "27")
        return true;
    if (Candidate.RuleNumber == "8" && Current.RuleNumber != "27" &&
        Current.RuleNumber != "8")
        return true;
    return false;
}

static std::vector<MISRAViolation> compactViolationsByLine(
    const std::vector<MISRAViolation>& Violations) {
    std::vector<MISRAViolation> Unique;
    for (const MISRAViolation& V : Violations) {
        bool Found = false;
        for (MISRAViolation& Existing : Unique) {
            if (Existing.LineNumber == V.LineNumber) {
                if (shouldPreferViolation(V, Existing))
                    Existing = V;
                Found = true;
                break;
            }
        }
        if (!Found)
            Unique.push_back(V);
    }
    return Unique;
}

static std::string unescapeCStringText(const std::string& Text) {
    std::string Out;
    for (std::size_t I = 0; I < Text.size(); ++I) {
        if (Text[I] != '\\' || I + 1 >= Text.size()) {
            Out += Text[I];
            continue;
        }

        const char Escaped = Text[++I];
        switch (Escaped) {
        case 'n': Out += '\n'; break;
        case 't': Out += '\t'; break;
        case 'r': Out += '\r'; break;
        case '\\': Out += '\\'; break;
        case '"': Out += '"'; break;
        default: Out += Escaped; break;
        }
    }
    return Out;
}

static std::string inferExpectedOutput(
    const std::vector<MISRAViolation>& Violations) {
    std::string Output;
    if (!gInputFilePath.empty()) {
        std::ifstream In(gInputFilePath.c_str());
        std::string Line;
        while (std::getline(In, Line)) {
            if (Line.find("scanf(") != std::string::npos)
                return "Interactive input required; see Corrected Program Run below";
        }
    }

    for (const MISRAViolation& V : Violations) {
        if (V.SourceLine.find("scanf(") != std::string::npos)
            return "Interactive input required; see Corrected Program Run below";
    }

    for (const MISRAViolation& V : Violations) {
        const std::string Line = V.SourceLine;
        std::size_t PrintfPos = Line.find("printf(");
        if (PrintfPos == std::string::npos)
            continue;

        std::size_t FirstQuote = Line.find('"', PrintfPos);
        if (FirstQuote == std::string::npos)
            continue;

        std::size_t LastQuote = FirstQuote + 1;
        bool Escaped = false;
        for (; LastQuote < Line.size(); ++LastQuote) {
            if (Escaped) {
                Escaped = false;
                continue;
            }
            if (Line[LastQuote] == '\\') {
                Escaped = true;
                continue;
            }
            if (Line[LastQuote] == '"')
                break;
        }
        if (LastQuote >= Line.size())
            continue;

        Output += unescapeCStringText(
            Line.substr(FirstQuote + 1, LastQuote - FirstQuote - 1));
    }
    return Output;
}

static std::string quotePath(const std::string& Path) {
    return "\"" + Path + "\"";
}

static std::string getTempFilePath(const std::string& Name) {
    const char* TempDir = std::getenv("TEMP");
    if (!TempDir || std::string(TempDir).empty())
        TempDir = std::getenv("TMP");
    if (!TempDir || std::string(TempDir).empty())
        TempDir = ".";

#if defined(_WIN32)
    const int Pid = _getpid();
    const char Separator = '\\';
#else
    const int Pid = getpid();
    const char Separator = '/';
#endif
    return std::string(TempDir) + Separator + Name + "_" +
        std::to_string(Pid);
}

static bool readSourceFile(const std::string& Path,
    std::vector<std::string>& Lines) {
    std::ifstream In(Path.c_str());
    if (!In) return false;

    std::string Line;
    while (std::getline(In, Line))
        Lines.push_back(Line);
    return true;
}

static bool writeSourceFile(const std::string& Path,
    const std::vector<std::string>& Lines) {
    std::ofstream Out(Path.c_str(), std::ios::out | std::ios::trunc);
    if (!Out) return false;

    for (const std::string& Line : Lines)
        Out << Line << "\n";
    return true;
}

static std::string leadingWhitespace(const std::string& Line) {
    std::size_t End = Line.find_first_not_of(" \t");
    if (End == std::string::npos) return Line;
    return Line.substr(0, End);
}

static bool startsWithToken(const std::string& Text,
    const std::string& Token) {
    return Text.rfind(Token, 0) == 0;
}

static std::size_t findNextCodeLine(
    const std::vector<std::string>& Lines,
    std::size_t Start) {
    for (std::size_t I = Start; I < Lines.size(); ++I) {
        if (!trimCopy(Lines[I]).empty())
            return I;
    }
    return Lines.size();
}

static void addBracesForSimpleControlStatements(
    std::vector<std::string>& Lines) {
    for (std::size_t I = 0; I < Lines.size(); ++I) {
        std::string Trimmed = trimCopy(Lines[I]);
        const bool IsIf = startsWithToken(Trimmed, "if ") ||
            startsWithToken(Trimmed, "if(");
        const bool IsElse = Trimmed == "else";
        if (!IsIf && !IsElse)
            continue;
        if (Trimmed.find('{') != std::string::npos ||
            Trimmed.find(';') != std::string::npos)
            continue;

        std::size_t BodyLine = findNextCodeLine(Lines, I + 1);
        if (BodyLine >= Lines.size())
            continue;
        if (trimCopy(Lines[BodyLine]).rfind("{", 0) == 0)
            continue;

        const std::string Indent = leadingWhitespace(Lines[I]);
        Lines[I] += " {";
        Lines.insert(Lines.begin() + BodyLine + 1, Indent + "}");
        ++I;
    }
}

static void applyLineCorrections(std::vector<std::string>& Lines,
    const std::vector<MISRAViolation>& Violations) {
    bool NeedsStdint = false;

    for (const MISRAViolation& V : Violations) {
        if (V.LineNumber == 0 || V.LineNumber > Lines.size())
            continue;

        std::string& Line = Lines[V.LineNumber - 1];
        if (V.RuleNumber == "27") {
            for (std::size_t I = 0; I < Line.size(); ++I) {
                if (isAssignmentToken(Line, I)) {
                    Line.replace(I, 1, "==");
                    break;
                }
            }
        }
        else if (V.RuleNumber == "8") {
            std::string Trimmed = trimCopy(Line);
            if (Trimmed.rfind("int ", 0) == 0) {
                const std::string Indent = leadingWhitespace(Line);
                Line = Indent + "int32_t " + Trimmed.substr(4);
                NeedsStdint = true;
            }
        }
    }

    if (NeedsStdint) {
        bool HasStdint = false;
        std::size_t InsertAt = 0;
        for (std::size_t I = 0; I < Lines.size(); ++I) {
            const std::string Trimmed = trimCopy(Lines[I]);
            if (Trimmed.find("<stdint.h>") != std::string::npos ||
                Trimmed.find("<cstdint>") != std::string::npos)
                HasStdint = true;
            if (Trimmed.rfind("#include", 0) == 0)
                InsertAt = I + 1;
        }
        if (!HasStdint)
            Lines.insert(Lines.begin() + InsertAt, "#include <stdint.h>");
    }

    addBracesForSimpleControlStatements(Lines);
}

static void runCorrectedProgram(
    const std::vector<MISRAViolation>& Violations) {
    if (gInputFilePath.empty())
        return;

    std::vector<std::string> Lines;
    if (!readSourceFile(gInputFilePath, Lines))
        return;

    applyLineCorrections(Lines, compactViolationsByLine(Violations));

    const std::string CorrectedPath =
        getTempFilePath("misra_corrected") + ".cpp";
#if defined(_WIN32)
    const std::string ExePath =
        getTempFilePath("misra_corrected") + ".exe";
#else
    const std::string ExePath =
        getTempFilePath("misra_corrected");
#endif

    if (!writeSourceFile(CorrectedPath, Lines))
        return;

    llvm::outs() << "\nCorrected Program Run:\n";
    llvm::outs() << "Compiling corrected temporary file...\n";
    llvm::outs().flush();

    const std::string CompileCommand =
        "clang++ -std=c++17 -Wno-deprecated-declarations " +
        quotePath(CorrectedPath) +
        " -o " + quotePath(ExePath);
    if (std::system(CompileCommand.c_str()) != 0) {
        llvm::outs() << "Could not compile corrected program.\n";
        llvm::outs() << "Temporary file: " << CorrectedPath << "\n\n";
        return;
    }

    llvm::outs() << "Running corrected program. Enter input when asked:\n";
    llvm::outs().flush();
    std::system(quotePath(ExePath).c_str());
    llvm::outs() << "\n";
}

class AIReviewer {
public:
    std::vector<AIReviewResult> review(
        const std::vector<MISRAViolation>& Violations) const {
        std::vector<AIReviewResult> Results;
        if (Violations.empty()) return Results;

        const std::vector<MISRAViolation> ReviewViolations =
            compactViolationsByLine(Violations);

        const char* Command = std::getenv("MISRA_AI_COMMAND");
        if (!Command || std::string(Command).empty()) {
            return buildFallbackResults(ReviewViolations, "");
        }

        std::string PromptPath = writePrompt(ReviewViolations);
        if (PromptPath.empty()) {
            Results.push_back(AIReviewResult{
                0,
                "AI prompt file could not be created",
                "Check TEMP/TMP permissions"
            });
            return Results;
        }

        std::string FullCommand = std::string(Command) + " \"" + PromptPath + "\" 2>&1";
        std::string Response = runCommand(FullCommand);
        Results = parseResponse(Response);
        Results = normalizeResults(ReviewViolations, Results);
        if (!Results.empty()) return Results;

        return buildFallbackResults(ReviewViolations, Response);
    }

private:
    static std::string writePrompt(
        const std::vector<MISRAViolation>& Violations) {
        const char* TempDir = std::getenv("TEMP");
        if (!TempDir || std::string(TempDir).empty())
            TempDir = std::getenv("TMP");
        if (!TempDir || std::string(TempDir).empty())
            TempDir = ".";

#if defined(_WIN32)
        const int Pid = _getpid();
        const char Separator = '\\';
#else
        const int Pid = getpid();
        const char Separator = '/';
#endif
        std::string Path = std::string(TempDir) + Separator +
            "misra_ai_prompt_" + std::to_string(Pid) + ".txt";
        std::ofstream Out(Path.c_str(), std::ios::out | std::ios::trunc);
        if (!Out) return "";

        Out << "You are a coding assistant for a C/C++ MISRA checker.\n";
        Out << "For each violation, generate a corrected single source line ";
        Out << "and predict the expected program output after that correction.\n";
        Out << "Do not include file names, paths, AST details, explanations, ";
        Out << "or full corrected source code.\n";
        Out << "Return only blocks in exactly this format:\n";
        Out << "Line <line_number>\n";
        Out << "Corrected Line: <corrected line>\n";
        Out << "Output: <expected output>\n\n";

        for (const MISRAViolation& V : Violations) {
            Out << "Violation:\n";
            Out << "Line Number: " << V.LineNumber << "\n";
            Out << "Rule Number: " << V.RuleNumber << "\n";
            Out << "Warning Message: " << V.WarningMessage << "\n";
            Out << "Violating Source Line: " << V.SourceLine << "\n\n";
        }

        return Path;
    }

    static std::string runCommand(const std::string& Command) {
        std::string Output;
#if defined(_WIN32)
        FILE* Pipe = _popen(Command.c_str(), "r");
#else
        FILE* Pipe = popen(Command.c_str(), "r");
#endif
        if (!Pipe) return Output;

        std::array<char, 256> Buffer;
        while (fgets(Buffer.data(), static_cast<int>(Buffer.size()), Pipe)) {
            Output += Buffer.data();
        }

#if defined(_WIN32)
        _pclose(Pipe);
#else
        pclose(Pipe);
#endif
        return Output;
    }

    static std::vector<AIReviewResult> parseResponse(
        const std::string& Response) {
        std::vector<AIReviewResult> Results;
        std::istringstream In(Response);
        std::string Line;
        AIReviewResult Current{ 0, "", "" };
        enum class PendingField { None, CorrectedLine, Output };
        PendingField Pending = PendingField::None;

        while (std::getline(In, Line)) {
            std::string Text = trimCopy(Line);
            if (Text.empty()) continue;

            if (Text.rfind("Line ", 0) == 0) {
                if (Current.LineNumber != 0)
                    Results.push_back(Current);
                Current = AIReviewResult{ 0, "", "" };
                Pending = PendingField::None;
                Current.LineNumber =
                    static_cast<unsigned>(std::strtoul(
                        Text.substr(5).c_str(), nullptr, 10));
            }
            else if (Text.rfind("Corrected Line:", 0) == 0) {
                Current.CorrectedLine =
                    trimCopy(Text.substr(std::string("Corrected Line:").size()));
                Pending = Current.CorrectedLine.empty()
                    ? PendingField::CorrectedLine
                    : PendingField::None;
            }
            else if (Text.rfind("Output:", 0) == 0) {
                Current.ExpectedOutput =
                    trimCopy(Text.substr(std::string("Output:").size()));
                Pending = Current.ExpectedOutput.empty()
                    ? PendingField::Output
                    : PendingField::None;
            }
            else if (Pending == PendingField::CorrectedLine) {
                Current.CorrectedLine = Text;
                Pending = PendingField::None;
            }
            else if (Pending == PendingField::Output) {
                Current.ExpectedOutput = Text;
                Pending = PendingField::None;
            }
        }

        if (Current.LineNumber != 0)
            Results.push_back(Current);

        return Results;
    }

    static std::vector<AIReviewResult> normalizeResults(
        const std::vector<MISRAViolation>& Violations,
        const std::vector<AIReviewResult>& Parsed) {
        std::vector<AIReviewResult> Results;
        const std::string InferredOutput = inferExpectedOutput(Violations);
        for (const MISRAViolation& V : Violations) {
            AIReviewResult R;
            R.LineNumber = V.LineNumber;
            R.CorrectedLine = "";
            R.ExpectedOutput = "";

            for (const AIReviewResult& ParsedResult : Parsed) {
                if (ParsedResult.LineNumber == V.LineNumber &&
                    isSingleLineCodeCorrection(ParsedResult.CorrectedLine)) {
                    R.CorrectedLine = trimCopy(ParsedResult.CorrectedLine);
                    R.ExpectedOutput = trimCopy(ParsedResult.ExpectedOutput);
                    break;
                }
            }

            if (R.CorrectedLine.empty()) {
                R.CorrectedLine = buildLocalCorrection(V);
                R.ExpectedOutput = InferredOutput.empty()
                    ? "Not evaluated"
                    : InferredOutput;
            }

            if (isSingleLineCodeCorrection(R.CorrectedLine))
                Results.push_back(R);
        }
        return Results;
    }

    static std::vector<AIReviewResult> buildFallbackResults(
        const std::vector<MISRAViolation>& Violations,
        const std::string& Response) {
        std::vector<AIReviewResult> Results;
        std::string Text = trimCopy(Response);
        const std::string InferredOutput = inferExpectedOutput(Violations);

        std::string SuggestedLine = "";
        std::istringstream In(Text);
        std::string Line;
        while (std::getline(In, Line)) {
            std::string Candidate = trimCopy(Line);
            if (Candidate.find("if (") != std::string::npos ||
                Candidate.find("printf(") != std::string::npos ||
                Candidate.find("=") != std::string::npos) {
                if (isSingleLineCodeCorrection(Candidate) &&
                    Candidate != "{" && Candidate != "}") {
                    SuggestedLine = Candidate;
                    break;
                }
            }
        }

        const std::size_t MaxItems = Violations.size();
        if (MaxItems == 0) {
            AIReviewResult R;
            R.LineNumber = 0;
            R.CorrectedLine = "No MISRA violations were collected for AI review";
            R.ExpectedOutput = Text;
            Results.push_back(R);
            return Results;
        }

        for (std::size_t I = 0; I < MaxItems; ++I) {
            AIReviewResult R;
            R.LineNumber = Violations[I].LineNumber;
            R.CorrectedLine = buildLocalCorrection(Violations[I]);
            if (R.CorrectedLine.empty())
                R.CorrectedLine = SuggestedLine;
            R.ExpectedOutput = Text.empty()
                ? (InferredOutput.empty() ? "Not evaluated" : InferredOutput)
                : Text;
            if (isSingleLineCodeCorrection(R.CorrectedLine))
                Results.push_back(R);
        }

        return Results;
    }
};

static void printAIResults(const std::vector<AIReviewResult>& Results) {
    llvm::outs() << "\nCorrected Output:\n\n";
    if (Results.empty()) {
        llvm::outs() << "Line 0\n\n";
        llvm::outs() << "Corrected Line:\n";
        llvm::outs() << "AI reviewer did not return a correction\n\n";
        llvm::outs() << "Output:\n";
        llvm::outs() << "Check MISRA_AI_COMMAND, MISRA_AI_BASE_URL, and Ollama status\n\n";
        return;
    }

    for (const AIReviewResult& R : Results) {
        llvm::outs() << "Line " << R.LineNumber << "\n";
        llvm::outs() << "Corrected Line: " << R.CorrectedLine << "\n";
        llvm::outs() << "Output: " << R.ExpectedOutput << "\n\n";
    }
}

// ============================================================
// PP Callbacks
// ============================================================
class MISRAPreprocessorCallback : public PPCallbacks {
public:
    Preprocessor& PP;
    DiagnosticsEngine& DE;

    MISRAPreprocessorCallback(Preprocessor& PP, DiagnosticsEngine& DE)
        : PP(PP), DE(DE) {
    }

    void warn(SourceLocation loc, const char* msg) {
        if (loc.isInvalid()) return;
        if (PP.getSourceManager().isInSystemHeader(loc)) return;
        if (!claimWarningLine(loc, PP.getSourceManager())) return;
        recordViolation(loc, msg, PP.getSourceManager(), PP.getLangOpts());
        unsigned ID = DE.getCustomDiagID(DiagnosticsEngine::Warning, "%0");
        DE.Report(loc, ID).AddString(msg);
    }

    // Rule 70: #include only preceded by directives/comments
    void InclusionDirective(SourceLocation HashLoc,
        const Token& IncludeTok,
        StringRef FileName,
        bool IsAngled,
        CharSourceRange FilenameRange,
        OptionalFileEntryRef File,
        StringRef SearchPath,
        StringRef RelativePath,
        const Module* Imported,
        bool ModuleImported,
        SrcMgr::CharacteristicKind FileType) override {
        SourceManager& SM = PP.getSourceManager();
        if (SM.isInSystemHeader(HashLoc)) return;
        unsigned col = SM.getSpellingColumnNumber(SM.getSpellingLoc(HashLoc));
        if (col > 1)
            warn(HashLoc,
                "Rule 70: #include statements shall only be preceded by "
                "other pre-processor directives or comments");
    }

    void MacroDefined(const Token& Tok, const MacroDirective* MD) override {
        const MacroInfo* MI = MD->getMacroInfo();
        SourceLocation   loc = Tok.getLocation();
        if (!MI || PP.getSourceManager().isInSystemHeader(loc)) return;

        StringRef name = Tok.getIdentifierInfo()->getName();

        // Rule 98: Standard library names shall not be reused
        static const char* stdNames[] = {
            "malloc","free","calloc","realloc",
            "printf","scanf","fprintf","fscanf",
            "exit","abort","system","getenv",
            "memcpy","memset","strlen","strcpy",
            "strcat","strcmp","sprintf","sscanf",
            nullptr
        };
        for (int i = 0; stdNames[i]; i++) {
            if (name == stdNames[i]) {
                warn(loc,
                    "Rule 98: Standard library function names shall not be reused");
                break;
            }
        }

        // Rule 74: Function-like macros
        if (MI->isFunctionLike())
            warn(loc, "Rule 74: Prefer a function over a function-like macro");

        // Rule 78: Expression macro not parenthesized
        if (!MI->isFunctionLike() && MI->getNumTokens() > 1) {
            bool hasOp = false;
            for (auto it = MI->tokens_begin(); it != MI->tokens_end(); ++it)
                if (it->isOneOf(tok::plus, tok::minus, tok::star,
                    tok::slash, tok::percent, tok::pipe,
                    tok::amp, tok::caret))
                {
                    hasOp = true; break;
                }
            if (hasOp) {
                bool lp = MI->tokens_begin()->is(tok::l_paren);
                bool rp = (MI->tokens_end() - 1)->is(tok::r_paren);
                if (!lp || !rp)
                    warn(loc,
                        "Rule 78: Macro expression must be enclosed in parentheses");
            }
        }

        // Rule 80: More than one # or ## in macro
        int cnt = 0;
        for (auto it = MI->tokens_begin(); it != MI->tokens_end(); ++it)
            if (it->isOneOf(tok::hash, tok::hashhash)) cnt++;
        if (cnt > 1)
            warn(loc,
                "Rule 80: At most one # or ## operator allowed in a macro");

        // Rule 76: Macro args must not look like directives
        if (MI->isFunctionLike()) {
            for (auto it = MI->tokens_begin(); it != MI->tokens_end(); ++it) {
                if (it->is(tok::hash)) {
                    auto next = it + 1;
                    if (next != MI->tokens_end() &&
                        next->isOneOf(tok::kw_if, tok::identifier)) {
                        warn(loc,
                            "Rule 76: Macro arguments shall not contain tokens "
                            "that look like pre-processing directives");
                        break;
                    }
                }
            }
        }

        // Rule 72: #define inside a block
        SourceManager& SM = PP.getSourceManager();
        unsigned col = SM.getSpellingColumnNumber(SM.getSpellingLoc(loc));
        if (col > 1 && !SM.isInSystemHeader(loc))
            warn(loc, "Rule 72: Macros shall not be #defined within a block");
    }

    void MacroUndefined(const Token& Tok,
        const MacroDefinition&,
        const MacroDirective*) override {
        SourceLocation loc = Tok.getLocation();
        if (PP.getSourceManager().isInSystemHeader(loc)) return;
        // Rule 73
        warn(loc, "Rule 73: #undef should not be used");
        // Rule 72
        SourceManager& SM = PP.getSourceManager();
        unsigned col = SM.getSpellingColumnNumber(SM.getSpellingLoc(loc));
        if (col > 1)
            warn(loc, "Rule 72: Macros shall not be #undefined within a block");
    }

    // Rule 100: errno macro expansion
    void MacroExpands(const Token& Tok,
        const MacroDefinition& MD,
        SourceRange Range,
        const MacroArgs* Args) override {
        SourceLocation loc = Tok.getLocation();
        if (loc.isInvalid()) return;
        if (PP.getSourceManager().isInSystemHeader(loc)) return;
        IdentifierInfo* II = Tok.getIdentifierInfo();
        if (II && II->getName() == "errno")
            warn(loc, "Rule 100: errno shall not be used");
    }

    // Rule 17: /* inside a C-style comment (nested comment)
    void HandleComment(Preprocessor& PP, SourceRange Comment) {
        SourceManager& SM = PP.getSourceManager();
        SourceLocation loc = Comment.getBegin();
        if (SM.isInSystemHeader(loc)) return;

        bool invalid = false;
        StringRef text = Lexer::getSourceText(
            CharSourceRange::getCharRange(Comment),
            SM, PP.getLangOpts(), &invalid);
        if (!invalid && text.starts_with("/*")) {
            // Check if /* appears inside the comment body
            size_t inner = text.find("/*", 2);
            if (inner != StringRef::npos)
                warn(loc,
                    "Rule 17: The character sequence /* shall not be used "
                    "within a C-style comment");
        }
    }
};

// ============================================================
// Helpers
// ============================================================
static unsigned countReturns(const Stmt* S) {
    if (!S) return 0;
    unsigned cnt = isa<ReturnStmt>(S) ? 1 : 0;
    for (const Stmt* child : S->children())
        cnt += countReturns(child);
    return cnt;
}

static unsigned countDirectBreaks(const Stmt* S, bool top = true) {
    if (!S) return 0;
    if (!top && (isa<ForStmt>(S) || isa<WhileStmt>(S) ||
        isa<DoStmt>(S) || isa<SwitchStmt>(S)))
        return 0;
    unsigned cnt = isa<BreakStmt>(S) ? 1 : 0;
    for (const Stmt* child : S->children())
        cnt += countDirectBreaks(child, false);
    return cnt;
}

// ============================================================
// registerMatchers
// ============================================================
void MyCheck::registerMatchers(MatchFinder& Finder) {

    // ── TYPE A RULES ─────────────────────────────────────────────────

    // Rule 8: basic types
    Finder.addMatcher(
        varDecl(
            unless(hasAncestor(recordDecl())),
            anyOf(
                hasType(asString("int")),
                hasType(asString("char")),
                hasType(asString("short")),
                hasType(asString("long")),
                hasType(asString("double")),
                hasType(asString("unsigned int")),
                hasType(asString("unsigned char")),
                hasType(asString("unsigned short")),
                hasType(asString("unsigned long"))
            )
        ).bind("r8"), this);

    // Rule 27: assignment in if condition
    Finder.addMatcher(
        ifStmt(hasCondition(
            expr(hasDescendant(binaryOperator(hasOperatorName("="))))
        )).bind("r27"), this);

    // Rule 34: Literal suffixes shall be uppercase (e.g. 1L not 1l)
    Finder.addMatcher(
        integerLiteral().bind("r34int"), this);
    Finder.addMatcher(
        floatLiteral().bind("r34float"), this);

    // Rule 38: float == or !=
    Finder.addMatcher(
        binaryOperator(
            anyOf(hasOperatorName("=="), hasOperatorName("!=")),
            hasLHS(expr(hasType(realFloatingPointType())))
        ).bind("r38"), this);

    // Rule 41: null statement
    Finder.addMatcher(nullStmt().bind("r41"), this);

    // Rule 43: goto
    Finder.addMatcher(gotoStmt().bind("r43"), this);

    // Rule 44: break in loop
    Finder.addMatcher(
        breakStmt(anyOf(
            hasAncestor(forStmt()),
            hasAncestor(whileStmt()),
            hasAncestor(doStmt())
        )).bind("r44b"), this);

    // Rule 44: continue in loop
    Finder.addMatcher(
        continueStmt(anyOf(
            hasAncestor(forStmt()),
            hasAncestor(whileStmt()),
            hasAncestor(doStmt())
        )).bind("r44c"), this);

    // Rule 46: if with no else
    Finder.addMatcher(
        ifStmt(unless(hasElse(anything()))).bind("r46"), this);

    // Rule 48/113: switch no default
    Finder.addMatcher(
        switchStmt(
            unless(hasDescendant(defaultStmt()))
        ).bind("r48"), this);

    // Rule 49: switch on boolean
    Finder.addMatcher(
        switchStmt(
            hasCondition(
                implicitCastExpr(
                    hasSourceExpression(
                        anyOf(
                            binaryOperator(anyOf(
                                hasOperatorName("=="),
                                hasOperatorName("!="),
                                hasOperatorName("<"),
                                hasOperatorName(">"),
                                hasOperatorName("<="),
                                hasOperatorName(">="),
                                hasOperatorName("&&"),
                                hasOperatorName("||")
                            )),
                            unaryOperator(hasOperatorName("!"))
                        )
                    )
                )
            )
        ).bind("r49"), this);

    // Rule 50: switch no case
    Finder.addMatcher(
        switchStmt(
            unless(hasDescendant(caseStmt()))
        ).bind("r50"), this);

    // Rule 51: float loop counter
    Finder.addMatcher(
        varDecl(
            hasType(realFloatingPointType()),
            hasAncestor(forStmt())
        ).bind("r51"), this);

    // Rule 59: explicit return type
    Finder.addMatcher(
        functionDecl(
            isDefinition(),
            unless(isMain()),
            unless(hasParent(recordDecl())),
            returns(asString("int"))
        ).bind("r59"), this);

    // Rule 60: parameter count mismatch
    Finder.addMatcher(
        callExpr(
            argumentCountIs(0),
            callee(functionDecl(unless(parameterCountIs(0))))
        ).bind("r60"), this);

    // Rule 61: void function return value used
    Finder.addMatcher(
        callExpr(
            callee(functionDecl(returns(asString("void")))),
            hasParent(expr())
        ).bind("r61"), this);

    // Rule 83: pointer arithmetic
    Finder.addMatcher(
        binaryOperator(
            anyOf(hasOperatorName("+"), hasOperatorName("-")),
            hasLHS(expr(hasType(pointerType())))
        ).bind("r83"), this);

    // Rule 86: function pointer variable
    Finder.addMatcher(
        varDecl(hasType(pointsTo(functionType()))).bind("r86"), this);

    // Rule 93: bit field not int/unsigned int
    Finder.addMatcher(
        fieldDecl(
            isBitField(),
            unless(anyOf(
                hasType(asString("unsigned int")),
                hasType(asString("int"))
            ))
        ).bind("r93"), this);

    // Rule 94: signed int bit field < 2 bits
    Finder.addMatcher(
        fieldDecl(isBitField(), hasType(asString("int"))).bind("r94"), this);

    // Rule 98: standard library names reused
    Finder.addMatcher(
        functionDecl(
            isDefinition(),
            unless(isExpansionInSystemHeader()),
            anyOf(
                hasName("malloc"), hasName("free"),
                hasName("calloc"), hasName("realloc"),
                hasName("printf"), hasName("scanf"),
                hasName("exit"), hasName("abort"),
                hasName("memcpy"), hasName("memset"),
                hasName("strlen"), hasName("strcpy")
            )
        ).bind("r98"), this);

    // Rule 99: malloc/free
    Finder.addMatcher(
        callExpr(callee(functionDecl(anyOf(
            hasName("malloc"), hasName("calloc"),
            hasName("realloc"), hasName("free")
        )))).bind("r99"), this);

    // Rule 100: errno AST fallback
    Finder.addMatcher(
        declRefExpr(to(varDecl(hasName("errno")))).bind("r100"), this);

    // Rule 105: signal/raise
    Finder.addMatcher(
        callExpr(callee(functionDecl(anyOf(
            hasName("signal"), hasName("raise")
        )))).bind("r105"), this);

    // Rule 106: stdio
    Finder.addMatcher(
        callExpr(callee(functionDecl(anyOf(
            hasName("printf"), hasName("scanf"),
            hasName("fprintf"), hasName("fscanf"),
            hasName("fopen"), hasName("fclose"),
            hasName("fread"), hasName("fwrite"),
            hasName("fgets"), hasName("fputs"),
            hasName("puts"), hasName("gets"),
            hasName("sprintf"), hasName("sscanf")
        )))).bind("r106"), this);

    // Rule 107: atof/atoi/atol
    Finder.addMatcher(
        callExpr(callee(functionDecl(anyOf(
            hasName("atof"), hasName("atoi"), hasName("atol")
        )))).bind("r107"), this);

    // Rule 108: abort/exit/getenv/system
    Finder.addMatcher(
        callExpr(callee(functionDecl(anyOf(
            hasName("abort"), hasName("exit"),
            hasName("getenv"), hasName("system")
        )))).bind("r108"), this);

    // Rule 119: incomplete array
    Finder.addMatcher(
        varDecl(hasType(incompleteArrayType())).bind("r119"), this);

    // Rule 120: array not fully initialized
    Finder.addMatcher(varDecl(hasType(arrayType())).bind("r120"), this);

    // Rule 123: int to pointer cast
    Finder.addMatcher(
        cStyleCastExpr(
            hasSourceExpression(expr(hasType(isInteger()))),
            hasType(pointerType())
        ).bind("r123"), this);

    // Rule 124 & 161: pointer cast
    Finder.addMatcher(
        cStyleCastExpr(
            hasSourceExpression(expr(hasType(pointerType())))
        ).bind("r124"), this);

    // Rule 125: conditional operator type mismatch
    Finder.addMatcher(conditionalOperator().bind("r125"), this);

    // Rule 127: implicit int to float
    Finder.addMatcher(implicitCastExpr().bind("r127"), this);

    // Rule 128: narrower float
    Finder.addMatcher(
        implicitCastExpr(hasCastKind(CK_FloatingCast)).bind("r128"), this);

    // Rule 129: narrower int
    Finder.addMatcher(
        implicitCastExpr(hasCastKind(CK_IntegralCast)).bind("r129"), this);

    // Rule 130: digraph in string
    Finder.addMatcher(stringLiteral().bind("r130"), this);

    // Rule 136: magic numbers
    Finder.addMatcher(
        integerLiteral(
            unless(anyOf(equals(0), equals(1)))
        ).bind("r136"), this);

    // Rule 137: literal array subscript
    Finder.addMatcher(
        arraySubscriptExpr(
            hasIndex(integerLiteral(unless(equals(0))))
        ).bind("r137"), this);

    // Rule 139: float cast to non-float
    Finder.addMatcher(
        cStyleCastExpr(
            hasSourceExpression(expr(hasType(realFloatingPointType())))
        ).bind("r139"), this);

    // Rule 143: empty switch
    Finder.addMatcher(
        switchStmt(
            unless(hasDescendant(caseStmt())),
            unless(hasDescendant(defaultStmt()))
        ).bind("r143"), this);

    // Rule 163: ++/-- mixed in binary expression
    Finder.addMatcher(
        unaryOperator(
            anyOf(hasOperatorName("++"), hasOperatorName("--")),
            hasParent(binaryOperator())
        ).bind("r163"), this);

    // Rule 165: && || operands not bool
    Finder.addMatcher(
        binaryOperator(
            anyOf(hasOperatorName("&&"), hasOperatorName("||")),
            hasLHS(expr(unless(hasType(booleanType()))))
        ).bind("r165"), this);

    // Rule 166: unary minus on unsigned
    Finder.addMatcher(
        unaryOperator(
            hasOperatorName("-"),
            hasUnaryOperand(expr(hasType(isUnsignedInteger())))
        ).bind("r166"), this);

    // Rule 171: comma operator
    Finder.addMatcher(
        binaryOperator(hasOperatorName(",")).bind("r171"), this);

    // Rule 181: if without braces
    Finder.addMatcher(ifStmt().bind("r181"), this);

    // Rule 182: else without braces
    Finder.addMatcher(
        ifStmt(hasElse(
            stmt(unless(anyOf(compoundStmt(), ifStmt())))
        )).bind("r182"), this);

    // Rule 188: float for loop counter
    Finder.addMatcher(
        forStmt(hasLoopInit(declStmt(
            containsDeclaration(0,
                varDecl(hasType(realFloatingPointType()))
            )
        ))).bind("r188"), this);

    // Rule 195: more than one break in a loop
    Finder.addMatcher(forStmt().bind("r195for"), this);
    Finder.addMatcher(whileStmt().bind("r195while"), this);
    Finder.addMatcher(doStmt().bind("r195do"), this);

    // Rule 196: multiple returns in function
    Finder.addMatcher(
        functionDecl(isDefinition(), unless(isMain())).bind("r196"), this);

    // Rule 200: global non-static variable
    Finder.addMatcher(
        varDecl(
            hasGlobalStorage(),
            unless(isStaticStorageClass()),
            unless(hasAncestor(functionDecl()))
        ).bind("r200"), this);

    // ── TYPE A NEW RULES ─────────────────────────────────────────────

    // Rule 4: No unused variables
    Finder.addMatcher(
        varDecl(
            isDefinition(),
            unless(hasAncestor(recordDecl())),
            unless(parmVarDecl())
        ).bind("r4"), this);

    // Rule 12: Every defined function shall be called at least once
    Finder.addMatcher(
        functionDecl(
            isDefinition(),
            unless(isMain()),
            unless(isExpansionInSystemHeader())
        ).bind("r12"), this);

    // Rule 28: typedef name shall be unique identifier
    Finder.addMatcher(
        typedefDecl().bind("r28"), this);

    // Rule 29: enum/union/struct name shall be unique
    Finder.addMatcher(
        enumDecl(isDefinition()).bind("r29enum"), this);

    // ── TYPE B RULES ─────────────────────────────────────────────────

    // Rule 2: Trigraphs in string literals
    Finder.addMatcher(stringLiteral().bind("r2"), this);

    // Rule 13: Octal constants
    Finder.addMatcher(integerLiteral().bind("r13"), this);

    // Rule 23: Braces for initialization
    Finder.addMatcher(
        varDecl(
            hasType(arrayType()),
            hasInitializer(initListExpr())
        ).bind("r23"), this);

    // Rule 25: Side effects in RHS of && or ||
    Finder.addMatcher(
        binaryOperator(
            anyOf(hasOperatorName("&&"), hasOperatorName("||")),
            hasRHS(expr(hasDescendant(
                unaryOperator(anyOf(
                    hasOperatorName("++"),
                    hasOperatorName("--")
                ))
            )))
        ).bind("r25"), this);

    // Rule 32: Comma operator outside for loop
    Finder.addMatcher(
        binaryOperator(
            hasOperatorName(","),
            unless(hasAncestor(forStmt()))
        ).bind("r32"), this);

    // Rule 39: Dead code after return/goto
    Finder.addMatcher(
        compoundStmt(
            forEach(returnStmt().bind("retStmt"))
        ).bind("r39block"), this);

    // Rule 47: break mandatory in case clauses
    Finder.addMatcher(
        switchStmt(
            forEachDescendant(
                caseStmt(
                    unless(hasDescendant(breakStmt()))
                ).bind("r47case")
            )
        ).bind("r47switch"), this);

    // Rule 53: For counter not modified in body
    Finder.addMatcher(
        forStmt(
            hasLoopInit(declStmt(
                containsDeclaration(0, varDecl().bind("forVar"))
            )),
            hasBody(stmt(hasDescendant(
                binaryOperator(
                    anyOf(
                        hasOperatorName("="),
                        hasOperatorName("+="),
                        hasOperatorName("-=")
                    ),
                    hasLHS(declRefExpr(
                        to(varDecl(equalsBoundNode("forVar")))
                    ))
                )
            )))
        ).bind("r53"), this);

    // Rule 55: Cyclomatic complexity — detect functions with many branches
    Finder.addMatcher(
        functionDecl(
            isDefinition(),
            unless(isMain()),
            unless(isExpansionInSystemHeader())
        ).bind("r55"), this);

    // Rule 57: Recursion
    Finder.addMatcher(
        callExpr(
            callee(functionDecl().bind("calleeFunc")),
            hasAncestor(
                functionDecl(equalsBoundNode("calleeFunc")).bind("r57func")
            )
        ).bind("r57"), this);

    // Rule 64: At most one return per function
    Finder.addMatcher(
        functionDecl(
            isDefinition(),
            unless(isMain())
        ).bind("r64"), this);
}

// ============================================================
// Helper: count branches for cyclomatic complexity
// ============================================================
static unsigned countBranches(const Stmt* S) {
    if (!S) return 0;
    unsigned cnt = 0;
    if (isa<IfStmt>(S) || isa<ForStmt>(S) ||
        isa<WhileStmt>(S) || isa<DoStmt>(S) ||
        isa<CaseStmt>(S) || isa<ConditionalOperator>(S))
        cnt++;
    for (const Stmt* child : S->children())
        cnt += countBranches(child);
    return cnt;
}

// ============================================================
// run
// ============================================================
void MyCheck::run(const MatchFinder::MatchResult& Result) {

    DiagnosticsEngine& DE = Result.Context->getDiagnostics();

    auto warn = [&](SourceLocation loc, const char* msg) {
        if (loc.isInvalid()) return;
        SourceManager& SM = Result.Context->getSourceManager();
        if (SM.isInSystemHeader(loc)) return;
        if (std::string(msg).find("Rule 106:") == 0) {
            if (gReportedRule106) return;
            gReportedRule106 = true;
        }
        if (!claimWarningLine(loc, SM)) return;
        recordViolation(loc, msg, SM,
            Result.Context->getLangOpts());
        unsigned ID = DE.getCustomDiagID(DiagnosticsEngine::Warning, "%0");
        DE.Report(loc, ID).AddString(msg);
        };

    // ── TYPE A ────────────────────────────────────────────────────────

    // Rule 4: Unused variable detection
    if (const VarDecl* V = Result.Nodes.getNodeAs<VarDecl>("r4")) {
        if (!V->isReferenced() &&
            !Result.Context->getSourceManager().isInSystemHeader(V->getLocation()))
            warn(V->getBeginLoc(),
                "Rule 4: There shall be no unused variables");
    }

    // Rule 8
    if (const VarDecl* V = Result.Nodes.getNodeAs<VarDecl>("r8"))
        warn(V->getBeginLoc(),
            "Rule 8: Basic types shall not be used; "
            "use fixed-length typedefs e.g. int32_t, uint8_t");

    // Rule 12: Function defined but never called
    if (const FunctionDecl* FD = Result.Nodes.getNodeAs<FunctionDecl>("r12")) {
        if (!FD->isUsed() && !FD->getBuiltinID())
            warn(FD->getBeginLoc(),
                "Rule 12: Every defined function shall be called at least once");
    }

    // Rule 27
    if (const IfStmt* I = Result.Nodes.getNodeAs<IfStmt>("r27"))
        warn(I->getBeginLoc(),
            "Rule 27: Assignment operators shall not be used in boolean expressions");

    // Rule 28: typedef uniqueness
    if (const TypedefDecl* TD = Result.Nodes.getNodeAs<TypedefDecl>("r28")) {
        if (!Result.Context->getSourceManager().isInSystemHeader(TD->getLocation()))
            warn(TD->getBeginLoc(),
                "Rule 28: A typedef name shall be a unique identifier");
    }

    // Rule 29: enum uniqueness
    if (const EnumDecl* ED = Result.Nodes.getNodeAs<EnumDecl>("r29enum")) {
        if (!Result.Context->getSourceManager().isInSystemHeader(ED->getLocation()))
            warn(ED->getBeginLoc(),
                "Rule 29: An enum name shall be a unique identifier");
    }

    // Rule 34: Literal suffixes uppercase
    if (const IntegerLiteral* IL =
        Result.Nodes.getNodeAs<IntegerLiteral>("r34int")) {
        SourceManager& SM = Result.Context->getSourceManager();
        SourceLocation loc = IL->getBeginLoc();
        if (!SM.isInSystemHeader(loc)) {
            bool invalid = false;
            StringRef text = Lexer::getSourceText(
                CharSourceRange::getTokenRange(loc),
                SM, Result.Context->getLangOpts(), &invalid);
            if (!invalid) {
                // Check for lowercase l, u, ul, lu suffixes
                if (text.ends_with("l") || text.ends_with("u") ||
                    text.ends_with("ul") || text.ends_with("lu") ||
                    text.ends_with("ll") || text.ends_with("ull"))
                    warn(loc,
                        "Rule 34: Literal suffixes shall be upper case "
                        "(use L, U, UL, LL, ULL instead of l, u, ul, ll, ull)");
            }
        }
    }

    if (const FloatingLiteral* FL =
        Result.Nodes.getNodeAs<FloatingLiteral>("r34float")) {
        SourceManager& SM = Result.Context->getSourceManager();
        SourceLocation loc = FL->getBeginLoc();
        if (!SM.isInSystemHeader(loc)) {
            bool invalid = false;
            StringRef text = Lexer::getSourceText(
                CharSourceRange::getTokenRange(loc),
                SM, Result.Context->getLangOpts(), &invalid);
            if (!invalid) {
                if (text.ends_with("f") || text.ends_with("l"))
                    warn(loc,
                        "Rule 34: Literal suffixes shall be upper case "
                        "(use F, L instead of f, l)");
            }
        }
    }

    // Rule 38
    if (const BinaryOperator* B = Result.Nodes.getNodeAs<BinaryOperator>("r38"))
        warn(B->getBeginLoc(),
            "Rule 38: Floating point shall not be tested for exact equality or inequality");

    // Rule 41
    if (const NullStmt* N = Result.Nodes.getNodeAs<NullStmt>("r41"))
        warn(N->getBeginLoc(),
            "Rule 41: Null statement shall only occur on a line by itself");

    // Rule 43
    if (const GotoStmt* G = Result.Nodes.getNodeAs<GotoStmt>("r43"))
        warn(G->getBeginLoc(), "Rule 43: goto statement is prohibited");

    // Rule 44 break
    if (const BreakStmt* B = Result.Nodes.getNodeAs<BreakStmt>("r44b"))
        warn(B->getBeginLoc(), "Rule 44: break is forbidden in loops");

    // Rule 44 continue
    if (const ContinueStmt* C = Result.Nodes.getNodeAs<ContinueStmt>("r44c"))
        warn(C->getBeginLoc(), "Rule 44: continue is forbidden in loops");

    // Rule 46
    if (const IfStmt* I = Result.Nodes.getNodeAs<IfStmt>("r46"))
        warn(I->getBeginLoc(),
            "Rule 46: if/else-if must have a final else clause");

    // Rule 48/113
    if (const SwitchStmt* S = Result.Nodes.getNodeAs<SwitchStmt>("r48"))
        warn(S->getBeginLoc(),
            "Rule 48/113: switch must have a default clause");

    // Rule 49
    if (const SwitchStmt* S = Result.Nodes.getNodeAs<SwitchStmt>("r49"))
        warn(S->getBeginLoc(),
            "Rule 49: switch expression must not be a boolean value");

    // Rule 50
    if (const SwitchStmt* S = Result.Nodes.getNodeAs<SwitchStmt>("r50"))
        warn(S->getBeginLoc(),
            "Rule 50: switch must have at least one case");

    // Rule 51
    if (const VarDecl* V = Result.Nodes.getNodeAs<VarDecl>("r51"))
        warn(V->getBeginLoc(),
            "Rule 51: No floating point variables as loop counters");

    // Rule 59
    if (const FunctionDecl* FD = Result.Nodes.getNodeAs<FunctionDecl>("r59"))
        if (!FD->getBuiltinID())
            warn(FD->getBeginLoc(),
                "Rule 59: Every function shall have an explicit return type");

    // Rule 60
    if (const CallExpr* C = Result.Nodes.getNodeAs<CallExpr>("r60"))
        warn(C->getBeginLoc(),
            "Rule 60: Number of parameters passed does not match prototype");

    // Rule 61
    if (const CallExpr* C = Result.Nodes.getNodeAs<CallExpr>("r61"))
        warn(C->getBeginLoc(),
            "Rule 61: Values returned by void functions shall not be used");

    // Rule 83
    if (const BinaryOperator* B = Result.Nodes.getNodeAs<BinaryOperator>("r83"))
        warn(B->getBeginLoc(), "Rule 83: Pointer arithmetic should not be used");

    // Rule 86
    if (const VarDecl* V = Result.Nodes.getNodeAs<VarDecl>("r86"))
        warn(V->getBeginLoc(),
            "Rule 86: Non-constant pointers to functions shall not be used");

    // Rule 93
    if (const FieldDecl* F = Result.Nodes.getNodeAs<FieldDecl>("r93"))
        warn(F->getBeginLoc(),
            "Rule 93: Bit fields shall only be of type unsigned int or signed int");

    // Rule 94
    if (const FieldDecl* F = Result.Nodes.getNodeAs<FieldDecl>("r94"))
        if (F->getBitWidthValue() < 2)
            warn(F->getBeginLoc(),
                "Rule 94: Bit fields of signed int shall be at least 2 bits long");

    // Rule 98
    if (const FunctionDecl* FD = Result.Nodes.getNodeAs<FunctionDecl>("r98"))
        warn(FD->getBeginLoc(),
            "Rule 98: Standard library function names shall not be reused");

    // Rule 99
    if (const CallExpr* C = Result.Nodes.getNodeAs<CallExpr>("r99"))
        warn(C->getBeginLoc(),
            "Rule 99: Dynamic memory allocation shall not be used");

    // Rule 100
    if (const DeclRefExpr* D = Result.Nodes.getNodeAs<DeclRefExpr>("r100"))
        warn(D->getBeginLoc(), "Rule 100: errno shall not be used");

    // Rule 105
    if (const CallExpr* C = Result.Nodes.getNodeAs<CallExpr>("r105"))
        warn(C->getBeginLoc(),
            "Rule 105: signal.h facilities shall not be used");

    // Rule 106
    if (const CallExpr* C = Result.Nodes.getNodeAs<CallExpr>("r106"))
        warn(C->getBeginLoc(),
            "Rule 106: stdio.h shall not be used in production code");

    // Rule 107
    if (const CallExpr* C = Result.Nodes.getNodeAs<CallExpr>("r107"))
        warn(C->getBeginLoc(),
            "Rule 107: atof/atoi/atol shall not be used");

    // Rule 108
    if (const CallExpr* C = Result.Nodes.getNodeAs<CallExpr>("r108"))
        warn(C->getBeginLoc(),
            "Rule 108: abort/exit/getenv/system shall not be used");

    // Rule 119
    if (const VarDecl* V = Result.Nodes.getNodeAs<VarDecl>("r119"))
        warn(V->getBeginLoc(),
            "Rule 119: Incomplete array declarations are not permitted");

    // Rule 120
    if (const VarDecl* V = Result.Nodes.getNodeAs<VarDecl>("r120")) {
        if (V->hasInit()) {
            if (const InitListExpr* IL = dyn_cast<InitListExpr>(V->getInit())) {
                if (const ConstantArrayType* CA =
                    dyn_cast<ConstantArrayType>(V->getType().getTypePtr())) {
                    if (IL->getNumInits() < CA->getSize().getZExtValue())
                        warn(V->getBeginLoc(),
                            "Rule 120: Array not fully initialized");
                }
            }
        }
    }

    // Rule 123
    if (const CStyleCastExpr* C = Result.Nodes.getNodeAs<CStyleCastExpr>("r123"))
        warn(C->getBeginLoc(),
            "Rule 123: Cast from integer to pointer shall not be performed");

    // Rule 124 & 161
    if (const CStyleCastExpr* C = Result.Nodes.getNodeAs<CStyleCastExpr>("r124")) {
        QualType dest = C->getType();
        if (dest->isIntegerType())
            warn(C->getBeginLoc(),
                "Rule 124: Pointer cast to integer is not allowed");
        if (dest->isPointerType())
            warn(C->getBeginLoc(),
                "Rule 161: Invalid pointer conversion from void pointer");
    }

    // Rule 125
    if (const ConditionalOperator* C =
        Result.Nodes.getNodeAs<ConditionalOperator>("r125")) {
        QualType t1 = C->getTrueExpr()->IgnoreImpCasts()->getType()
            .getUnqualifiedType();
        QualType t2 = C->getFalseExpr()->IgnoreImpCasts()->getType()
            .getUnqualifiedType();
        if (!Result.Context->hasSameType(t1, t2))
            warn(C->getBeginLoc(),
                "Rule 125: Conditional operator has incompatible types");
    }

    // Rule 127
    if (const ImplicitCastExpr* I =
        Result.Nodes.getNodeAs<ImplicitCastExpr>("r127")) {
        if (I->getSubExpr()->getType()->isIntegerType() &&
            I->getType()->isFloatingType())
            warn(I->getBeginLoc(),
                "Rule 127: Implicit integer to float conversion");
    }

    // Rule 128
    if (const ImplicitCastExpr* I =
        Result.Nodes.getNodeAs<ImplicitCastExpr>("r128")) {
        QualType src = I->getSubExpr()->getType();
        QualType dst = I->getType();
        if (src->isFloatingType() && dst->isFloatingType())
            if (Result.Context->getTypeSize(dst) < Result.Context->getTypeSize(src))
                warn(I->getBeginLoc(),
                    "Rule 128: Narrower float conversion without explicit cast");
    }

    // Rule 129
    if (const ImplicitCastExpr* I =
        Result.Nodes.getNodeAs<ImplicitCastExpr>("r129")) {
        QualType src = I->getSubExpr()->getType();
        QualType dst = I->getType();
        if (src->isIntegerType() && dst->isIntegerType())
            if (Result.Context->getTypeSize(dst) < Result.Context->getTypeSize(src))
                warn(I->getBeginLoc(),
                    "Rule 129: Narrower int conversion without explicit cast");
    }

    // Rule 130
    if (const StringLiteral* SL = Result.Nodes.getNodeAs<StringLiteral>("r130")) {
        StringRef s = SL->getString();
        if (s.contains("<:") || s.contains(":>") ||
            s.contains("<%") || s.contains("%>") ||
            s.contains("%:"))
            warn(SL->getBeginLoc(),
                "Rule 130: Use of digraph characters is not permitted");
    }

    // Rule 136
    if (const IntegerLiteral* I = Result.Nodes.getNodeAs<IntegerLiteral>("r136"))
        warn(I->getBeginLoc(),
            "Rule 136: Magic number used; use symbolic constants instead");

    // Rule 137
    if (const ArraySubscriptExpr* A =
        Result.Nodes.getNodeAs<ArraySubscriptExpr>("r137"))
        warn(A->getBeginLoc(),
            "Rule 137: Numeric literal used as array subscript; "
            "use symbolic name instead");

    // Rule 139
    if (const CStyleCastExpr* C = Result.Nodes.getNodeAs<CStyleCastExpr>("r139"))
        if (!C->getType()->isFloatingType())
            warn(C->getBeginLoc(),
                "Rule 139: Float value cast to non-float type");

    // Rule 143
    if (const SwitchStmt* S = Result.Nodes.getNodeAs<SwitchStmt>("r143"))
        warn(S->getBeginLoc(), "Rule 143: Empty switch statement");

    // Rule 163
    if (const UnaryOperator* U = Result.Nodes.getNodeAs<UnaryOperator>("r163"))
        warn(U->getBeginLoc(),
            "Rule 163: Increment/decrement should not be mixed with other operators");

    // Rule 165
    if (const BinaryOperator* B = Result.Nodes.getNodeAs<BinaryOperator>("r165"))
        warn(B->getBeginLoc(),
            "Rule 165: Each operand of && or || shall have essentially Boolean type");

    // Rule 166
    if (const UnaryOperator* U = Result.Nodes.getNodeAs<UnaryOperator>("r166"))
        warn(U->getBeginLoc(),
            "Rule 166: Unary minus shall not be applied to an unsigned expression");

    // Rule 171
    if (const BinaryOperator* B = Result.Nodes.getNodeAs<BinaryOperator>("r171"))
        warn(B->getBeginLoc(), "Rule 171: The comma operator shall not be used");

    // Rule 181
    if (const IfStmt* I = Result.Nodes.getNodeAs<IfStmt>("r181"))
        if (!isa<CompoundStmt>(I->getThen()))
            warn(I->getBeginLoc(),
                "Rule 181: If must use compound statement {}");

    // Rule 182
    if (const IfStmt* I = Result.Nodes.getNodeAs<IfStmt>("r182"))
        warn(I->getBeginLoc(),
            "Rule 182: else must be followed by compound statement {}");

    // Rule 188
    if (const ForStmt* F = Result.Nodes.getNodeAs<ForStmt>("r188"))
        warn(F->getBeginLoc(),
            "Rule 188: for loop shall not have a float loop counter");

    // Rule 195
    if (const ForStmt* F = Result.Nodes.getNodeAs<ForStmt>("r195for"))
        if (F->getBody() && countDirectBreaks(F->getBody()) > 1)
            warn(F->getBeginLoc(),
                "Rule 195: No more than one break statement per loop");
    if (const WhileStmt* W = Result.Nodes.getNodeAs<WhileStmt>("r195while"))
        if (W->getBody() && countDirectBreaks(W->getBody()) > 1)
            warn(W->getBeginLoc(),
                "Rule 195: No more than one break statement per loop");
    if (const DoStmt* D = Result.Nodes.getNodeAs<DoStmt>("r195do"))
        if (D->getBody() && countDirectBreaks(D->getBody()) > 1)
            warn(D->getBeginLoc(),
                "Rule 195: No more than one break statement per loop");

    // Rule 196
    if (const FunctionDecl* FD = Result.Nodes.getNodeAs<FunctionDecl>("r196"))
        if (!FD->getBuiltinID() && FD->hasBody())
            if (countReturns(FD->getBody()) > 1)
                warn(FD->getBeginLoc(),
                    "Rule 196: Function shall have a single point of exit");

    // Rule 200
    if (const VarDecl* V = Result.Nodes.getNodeAs<VarDecl>("r200"))
        warn(V->getBeginLoc(),
            "Rule 200: Global variables should be avoided");

    // ── TYPE B ────────────────────────────────────────────────────────

    // Rule 2: Trigraphs
    if (const StringLiteral* SL = Result.Nodes.getNodeAs<StringLiteral>("r2")) {
        StringRef s = SL->getString();
        if (s.contains("??=") || s.contains("??(") || s.contains("??)") ||
            s.contains("??/") || s.contains("??'") || s.contains("??<") ||
            s.contains("??>") || s.contains("??!") || s.contains("??-"))
            warn(SL->getBeginLoc(),
                "Rule 2: Trigraphs shall not be used");
    }

    // Rule 13: Octal constants
    if (const IntegerLiteral* IL =
        Result.Nodes.getNodeAs<IntegerLiteral>("r13")) {
        SourceManager& SM = Result.Context->getSourceManager();
        SourceLocation loc = IL->getBeginLoc();
        if (loc.isValid() && !SM.isInSystemHeader(loc)) {
            bool invalid = false;
            StringRef text = Lexer::getSourceText(
                CharSourceRange::getTokenRange(loc),
                SM, Result.Context->getLangOpts(), &invalid);
            if (!invalid && text.size() > 1 &&
                text[0] == '0' &&
                text[1] >= '1' && text[1] <= '7')
                warn(loc,
                    "Rule 13: Octal constants other than zero shall not be used");
        }
    }

    // Rule 25: Side effects in RHS of && or ||
    if (const BinaryOperator* B = Result.Nodes.getNodeAs<BinaryOperator>("r25"))
        warn(B->getBeginLoc(),
            "Rule 25: The right-hand operand of && or || "
            "shall not contain side effects");

    // Rule 32: Comma outside for loop
    if (const BinaryOperator* B = Result.Nodes.getNodeAs<BinaryOperator>("r32"))
        warn(B->getBeginLoc(),
            "Rule 32: The comma operator shall not be used "
            "except in the control expression of a for loop");

    // Rule 39: Dead code after return/goto
    if (const CompoundStmt* CS =
        Result.Nodes.getNodeAs<CompoundStmt>("r39block")) {
        bool seenTerminator = false;
        for (const Stmt* S : CS->body()) {
            if (seenTerminator && !isa<LabelStmt>(S)) {
                warn(S->getBeginLoc(),
                    "Rule 39: No inaccessible code after a goto or return statement");
                break;
            }
            if (isa<ReturnStmt>(S) || isa<GotoStmt>(S))
                seenTerminator = true;
        }
    }

    // Rule 47: break in case
    if (const CaseStmt* CS = Result.Nodes.getNodeAs<CaseStmt>("r47case")) {
        const Stmt* sub = CS->getSubStmt();
        bool hasTerminator = false;
        if (sub) {
            if (isa<BreakStmt>(sub) || isa<ReturnStmt>(sub))
                hasTerminator = true;
            if (const CompoundStmt* comp = dyn_cast<CompoundStmt>(sub)) {
                for (const Stmt* s : comp->body())
                    if (isa<BreakStmt>(s) || isa<ReturnStmt>(s)) {
                        hasTerminator = true; break;
                    }
            }
        }
        if (!hasTerminator)
            warn(CS->getBeginLoc(),
                "Rule 47: The break statement shall be the last statement "
                "in each case clause of a switch statement");
    }

    // Rule 53: For counter modified in body
    if (const ForStmt* F = Result.Nodes.getNodeAs<ForStmt>("r53"))
        warn(F->getBeginLoc(),
            "Rule 53: The counter in a for statement must not be "
            "modified inside the loop body");

    // Rule 55: Cyclomatic complexity > 10
    if (const FunctionDecl* FD = Result.Nodes.getNodeAs<FunctionDecl>("r55")) {
        if (!FD->getBuiltinID() && FD->hasBody()) {
            unsigned complexity = 1 + countBranches(FD->getBody());
            if (complexity > 10)
                warn(FD->getBeginLoc(),
                    "Rule 55: Functions shall have a cyclomatic complexity "
                    "number of 10 or less");
        }
    }

    // Rule 57: Recursion
    if (Result.Nodes.getNodeAs<CallExpr>("r57")) {
        const FunctionDecl* FD =
            Result.Nodes.getNodeAs<FunctionDecl>("r57func");
        if (FD && !FD->getBuiltinID())
            warn(FD->getBeginLoc(),
                "Rule 57: It is not recommended to use recursion");
    }

    // Rule 64: At most one return per function
    if (const FunctionDecl* FD = Result.Nodes.getNodeAs<FunctionDecl>("r64")) {
        if (!FD->getBuiltinID() && FD->hasBody())
            if (countReturns(FD->getBody()) > 1)
                warn(FD->getBeginLoc(),
                    "Rule 64: At most one return statement per function");
    }
}

// ============================================================
// Consumer + Action
// ============================================================
class MyASTConsumer : public ASTConsumer {
public:
    MatchFinder Finder;
    MyCheck     Check;
    MyASTConsumer() { Check.registerMatchers(Finder); }
    void HandleTranslationUnit(ASTContext& Ctx) override {
        gViolations.clear();
        gReportedWarningLines.clear();
        gReportedRule106 = false;
        Finder.matchAST(Ctx);
        AIReviewer Reviewer;
        printAIResults(Reviewer.review(gViolations));
        runCorrectedProgram(gViolations);
    }
};

std::unique_ptr<ASTConsumer>
MyFrontendAction::CreateASTConsumer(CompilerInstance& CI, StringRef file) {
    gInputFilePath = file.str();
    CI.getDiagnostics().setSeverity(
        diag::warn_condition_is_assignment,
        diag::Severity::Ignored,
        SourceLocation());
    CI.getDiagnostics().setSeverity(
        diag::warn_deprecated,
        diag::Severity::Ignored,
        SourceLocation());
    CI.getDiagnostics().setSeverity(
        diag::warn_deprecated_message,
        diag::Severity::Ignored,
        SourceLocation());
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<MISRAPreprocessorCallback>(
            CI.getPreprocessor(), CI.getDiagnostics()));
    return std::make_unique<MyASTConsumer>();
}
