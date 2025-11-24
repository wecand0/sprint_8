#include "RefactorTool.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"
#include <unordered_set>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

class ASTVisitor : public RecursiveASTVisitor<ASTVisitor> {
public:
    explicit ASTVisitor(const CXXRecordDecl *base_class) : base_class_{base_class}, has_descendants{false} {}

    bool VisitCXXRecordDecl(CXXRecordDecl *decl) {
        if (!decl->hasDefinition()) {
            return true;
        }

        if (decl->isDerivedFrom(base_class_)) {
            has_descendants = true;
            return false;
        }

        return true;
    }

    bool hasDescendants() const { return has_descendants; }

private:
    const CXXRecordDecl *base_class_;
    bool has_descendants;
};

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto &Diag = Result.Context->getDiagnostics();
    auto &SM = *Result.SourceManager;  // Получаем SourceManager для проверки isInMainFile

    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("classDecl")) {
        handle_nv_dtor(Dtor, *Result.Context, Diag, SM);
    }

    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("methodDecl")) {
        handle_miss_override(Method, Diag, SM);
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar")) {
        handle_crange_for(LoopVar, Diag, SM);
    }
}

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor, ASTContext &Context, DiagnosticsEngine &Diag,
                                     SourceManager &SM) {
    if (!SM.isInMainFile(Dtor->getLocation())) {
        return;
    }

    const CXXRecordDecl *base_class = Dtor->getParent();
    const CXXRecordDecl *base_class_def = base_class->getDefinition();
    if (!base_class_def) {
        return;
    }

    ASTVisitor visitor(base_class_def);
    visitor.TraverseDecl(Context.getTranslationUnitDecl());

    if (!visitor.hasDescendants()) {
        return;
    }

    unsigned loc = Dtor->getBeginLoc().getRawEncoding();
    if (virtualDtorLocations.count(loc)) {
        return;
    }

    Rewrite.InsertText(Dtor->getLocation(), "virtual ");
    virtualDtorLocations.insert(loc);

    const unsigned DiagID =
        Diag.getCustomDiagID(DiagnosticsEngine::Warning, "non-virtual destructor in the base class; added 'virtual'");
    Diag.Report(Dtor->getLocation(), DiagID);
}

void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method, DiagnosticsEngine &Diag, SourceManager &SM) {
    if (!SM.isInMainFile(Method->getLocation())) {
        return;
    }

    if (auto type_info = Method->getTypeSourceInfo()) {
        auto type_loc = type_info->getTypeLoc().getAs<FunctionProtoTypeLoc>();
        if (type_loc.isNull()) {
            return;
        }

        SourceLocation loc = type_loc.getRParenLoc();
        if (!loc.isValid()) {
            return;
        }

        Rewrite.InsertTextAfterToken(loc, " override");
        const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Warning,
                                                     "the base method is overrided, but not marked; added 'override'");
        Diag.Report(Method->getLocation(), DiagID);
    }
}

void RefactorHandler::handle_crange_for(const VarDecl *LoopVar, DiagnosticsEngine &Diag, SourceManager &SM) {
    if (!SM.isInMainFile(LoopVar->getLocation())) {
        return;
    }

    QualType var_type = LoopVar->getType();
    if (!var_type.isConstQualified() || var_type->isReferenceType() || var_type->isFundamentalType()) {
        return;
    }

    if (auto type_info = LoopVar->getTypeSourceInfo()) {
        TypeLoc type_loc = type_info->getTypeLoc();
        if (type_loc.isNull()) {
            return;
        }

        SourceLocation loc = type_loc.getEndLoc();
        if (!loc.isValid()) {
            return;
        }

        Rewrite.InsertTextAfterToken(loc, "&");
        const unsigned DiagID =
            Diag.getCustomDiagID(DiagnosticsEngine::Warning, "range-based for loop uses copying; added '&'");
        Diag.Report(LoopVar->getLocation(), DiagID);
    }
}

auto NvDtorMatcher() { return cxxDestructorDecl(unless(isVirtual()), unless(isImplicit())).bind("classDecl"); }

auto NoOverrideMatcher() {
    return cxxMethodDecl(isOverride(), unless(hasAttr(attr::Override)), unless(cxxDestructorDecl())).bind("methodDecl");
}

// Конструктор принимает Rewriter для изменения кода.
auto NoRefConstVarInRangeLoopMatcher() { return cxxForRangeStmt(hasLoopVariable(varDecl().bind("loopVar"))); }

ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) { Finder.matchAST(Context); }

std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI) {
    // Инициализируем Rewriter для рефакторинга.
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
    // Применяем изменения в файле.
    if (RewriterForCodeRefactor.overwriteChangedFiles()) {
        llvm::errs() << "Error applying changes to files.\n";
    }
}

int main(int argc, const char **argv) {
    // Парсер опций: Обрабатывает флаги командной строки, компиляционные базы данных.
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    // Создаем ClangTool
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    // Запускаем RefactorAction.
    return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}