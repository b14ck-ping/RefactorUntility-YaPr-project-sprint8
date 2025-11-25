#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include <clang/AST/ASTContext.h>
#include <iostream>

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto &Diag = Result.Context->getDiagnostics();
    auto &SM = *Result.SourceManager;  // Получаем SourceManager для проверки
                                       // isInMainFile

    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("classDecl")) {
        handle_nv_dtor(Dtor, Diag, SM);
    }

    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("methodDecl");
        Method && Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>()) {
        handle_miss_override(Method, Diag, SM);
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("VarDecl")) {
        handle_crange_for(LoopVar, Diag, SM);
    }
}

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor, DiagnosticsEngine &Diag, SourceManager &SM) {

    if (!SM.isInMainFile(Dtor->getLocation())) {
        return;
    }

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Destructor declared");
    Diag.Report(Dtor->getLocation(), DiagID);

    SourceLocation spellingLoc = SM.getSpellingLoc(Dtor->getLocation());
    unsigned locHash = spellingLoc.getRawEncoding();

    if (virtualDtorLocations.find(locHash) != virtualDtorLocations.end()) {
        return;
    }

    const CXXRecordDecl *record = Dtor->getParent();

    if (Dtor->isVirtual() || !Dtor->isThisDeclarationADefinition() || record->hasAttr<FinalAttr>()) {
        return;
    }

    bool hasDerived = false;

    auto CheckDerived = [&](const CXXRecordDecl *Derived) {
        if (!Derived || Derived == record || !Derived->isThisDeclarationADefinition() ||
            Derived->getDescribedClassTemplate())
            return;

        for (const auto &BaseSpecifier : Derived->bases()) {
            const CXXRecordDecl *BaseDecl = BaseSpecifier.getType()->getAsCXXRecordDecl();
            if (BaseDecl && BaseDecl->getCanonicalDecl() == record->getCanonicalDecl()) {
                hasDerived = true;
                return;
            }
        }
    };

    ASTContext &Context = Dtor->getParent()->getASTContext();

    for (const auto *Decl : Context.getTranslationUnitDecl()->decls()) {
        if (const auto *Record = dyn_cast<CXXRecordDecl>(Decl)) {
            CheckDerived(Record);
            if (hasDerived)
                break;
        }
    }

    if (!hasDerived)
        return;

    SourceLocation loc = Dtor->getLocation();

    if (loc.isMacroID()) {
        loc = SM.getExpansionLoc(loc);
    }

    if (!loc.isValid() || SM.isInSystemHeader(loc)) {
        loc = SourceLocation();
    }

    if (loc.isValid()) {

        Rewrite.InsertTextBefore(loc, "virtual ");

        virtualDtorLocations.insert(locHash);

        std::string className = record->getNameAsString();

        unsigned ID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Added 'virtual' to destructor of class '%0' "
                                                                      "(base class with derived classes)");
        Diag.Report(Dtor->getLocation(), ID) << className;

        llvm::errs() << "Fixed: destructor " << className << " is now virtual\n";
    }
}

void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method, DiagnosticsEngine &Diag, SourceManager &SM) {

    if (SM.isInSystemHeader(Method->getLocation()) || !SM.isInMainFile(SM.getExpansionLoc(Method->getLocation()))) {
        return;
    }

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Missing override");
    Diag.Report(Method->getLocation(), DiagID);

    if (Method->hasAttr<OverrideAttr>()) {
        return;
    }

    if (Method->isOverloadedOperator()) {
        return;
    }

    SourceLocation insertLoc = Method->getLocation();
    if (Method->hasBody()) {
        const Stmt *body = Method->getBody();
        if (body) {
            SourceLocation bodyStart = body->getBeginLoc();
            if (bodyStart.isValid()) {
                insertLoc = bodyStart;
            }
        }
    } else {
        SourceLocation endLoc = Method->getEndLoc();
        if (endLoc.isValid()) {
            insertLoc = endLoc;
        }
    }

    if (insertLoc.isValid()) {
        Rewrite.InsertTextBefore(insertLoc, "override ");
        std::string methodName = Method->getNameAsString();
        std::string className = Method->getParent()->getNameAsString();

        unsigned ID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Added 'override' to method '%0' in class '%1'");
        Diag.Report(Method->getLocation(), ID) << methodName << className;

        llvm::errs() << "Fixed: method " << className << "::" << methodName << " is now marked override\n";
    }
}

void RefactorHandler::handle_crange_for(const VarDecl *LoopVar, DiagnosticsEngine &Diag, SourceManager &SM) {
    SourceLocation Loc = SM.getExpansionLoc(LoopVar->getLocation());
    if (SM.isInSystemHeader(Loc) || !SM.isInMainFile(Loc))
        return;

    QualType VarType = LoopVar->getType();

    if (VarType->isReferenceType())
        return;

    if (!VarType.isConstQualified())
        return;

    QualType Base = VarType.getNonReferenceType().getUnqualifiedType();
    if (Base->isFundamentalType() || Base->isPointerType() || Base->isEnumeralType())
        return;

    if (const AutoType *AT = Base->getAs<AutoType>()) {
        QualType Deduced = AT->getDeducedType();
        if (!Deduced.isNull()) {

            QualType DeducedBase = Deduced.getNonReferenceType().getUnqualifiedType();

            if (DeducedBase->isFundamentalType() || DeducedBase->isPointerType() || DeducedBase->isEnumeralType() ||
                DeducedBase->isReferenceType()) {
                return;
            }
        }
    }

    TypeSourceInfo *TSI = LoopVar->getTypeSourceInfo();
    if (!TSI)
        return;

    TypeLoc TL = TSI->getTypeLoc();
    SourceLocation TypeEnd = TL.getEndLoc();

    if (!TypeEnd.isValid())
        return;

    Rewrite.InsertTextAfterToken(TypeEnd, "&");

    std::string VarName = LoopVar->getNameAsString();
    std::string TypeName = VarType.getAsString();

    unsigned ID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "added reference to variable '%0' "
                                                                  "of type '%1' to avoid copying");

    Diag.Report(LoopVar->getLocation(), ID) << VarName << TypeName;

    llvm::errs() << "Fixed: variable " << VarName << " now has type " << TypeName << "&\n";
}

auto NvDtorMatcher() {
    return cxxDestructorDecl(unless(isVirtual()), unless(isImplicit()),
                             hasParent(cxxRecordDecl(unless(isFinal()), hasDescendant(cxxRecordDecl()))),
                             unless(hasParent(cxxRecordDecl(isTemplateInstantiation()))))
        .bind("classDecl");
}

auto NoOverrideMatcher() {
    return cxxMethodDecl(unless(isImplicit()), unless(cxxDestructorDecl()), isDefinition()).bind("methodDecl");
}

auto NoRefConstVarInRangeLoopMatcher() {
    return varDecl(hasType(qualType(isConstQualified(), unless(referenceType()))), hasAncestor(cxxForRangeStmt()))
        .bind("VarDecl");
}

// Конструктор принимает Rewriter для изменения кода.
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
