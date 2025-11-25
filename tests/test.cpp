#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <string>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::tooling;

// Test version of CodeRefactorAction that captures output to string
class TestCodeRefactorAction : public ASTFrontendAction {
public:
    TestCodeRefactorAction(std::string &output) : output_(output) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override {
        rewriter_.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return std::make_unique<ComplexConsumer>(rewriter_);
    }

    bool BeginSourceFileAction(CompilerInstance &CI) override {
        rewriter_.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return true;
    }

    void EndSourceFileAction() override {
        std::string result;
        llvm::raw_string_ostream stream(result);
        rewriter_.getEditBuffer(rewriter_.getSourceMgr().getMainFileID())
            .write(stream);
        output_ = stream.str();
    }

private:
    Rewriter rewriter_;
    std::string &output_;
};

// Test fixture
class RefactorToolTest : public ::testing::Test {
protected:
    std::string runRefactorOnCode(const std::string &code) {
        std::string output;

        // Use Clang's runToolOnCode to process the code string
        bool success = runToolOnCode(
            std::make_unique<TestCodeRefactorAction>(output), code);

        if (!success) {
            return "COMPILATION_ERROR";
        }

        return output;
    }

    bool codeContains(const std::string &code, const std::string &substring) {
        return code.find(substring) != std::string::npos;
    }
};

// Virtual destructors tests
TEST_F(RefactorToolTest, AddsVirtualToDestructorWhenDerivedClassesExist) {
    std::string Code = R"(
        class Base {
        public:
            ~Base() {}
        };
        class Derived : public Base {};
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_TRUE(codeContains(Result, "virtual ~Base()"))
        << "Should add virtual to destructor. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddVirtualWhenNoDerivedClasses) {
    std::string Code = R"(
        class Base {
        public:
            ~Base() {}
        };
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "virtual ~Base()"))
        << "Should NOT add virtual when there are no derived classes. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotModifyAlreadyVirtualDestructor) {
    std::string Code = R"(
        class Base {
        public:
            virtual ~Base() {}
        };
        class Derived : public Base {};
    )";

    std::string Original = runRefactorOnCode(Code);
    std::string Result = runRefactorOnCode(Original);

    size_t virtualCount = 0;
    size_t pos = 0;
    while ((pos = Result.find("virtual", pos)) != std::string::npos) {
        virtualCount++;
        pos += 7;
    }

    EXPECT_EQ(virtualCount, 1) << "Should not duplicate virtual. Result:\n"
                               << Result;
}

// Override adding tests
TEST_F(RefactorToolTest, AddsOverrideToOverridingMethod) {
    std::string Code = R"(
        class Base {
        public:
            virtual void method() {}
        };
        class Derived : public Base {
        public:
            void method() {}
        };
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_TRUE(codeContains(Result, "void method() override"))
        << "Should add override to overriding method. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddOverrideToNonOverridingMethod) {
    std::string Code = R"(
        class Base {
        public:
            void method() {} 
        };
        class Derived : public Base {
        public:
            void method() {} 
        };
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "override"))
        << "Should NOT add override to non-overriding method. "
           "Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotDuplicateExistingOverride) {
    std::string Code = R"(
        class Base {
        public:
            virtual void method() {}
        };
        class Derived : public Base {
        public:
            void method() override {}
        };
    )";

    std::string Result = runRefactorOnCode(Code);

    size_t overrideCount = 0;
    size_t pos = 0;
    while ((pos = Result.find("override", pos)) != std::string::npos) {
        overrideCount++;
        pos += 8;
    }

    EXPECT_EQ(overrideCount, 1) << "Should not duplicate override. Result:\n"
                                << Result;
}

TEST_F(RefactorToolTest, AddsOverrideToMultipleOverridingMethods) {
    std::string Code = R"(
        class Base {
        public:
            virtual void method1() {}
            virtual void method2() {}
        };
        class Derived : public Base {
        public:
            void method1() {}
            void method2() {}
        };
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_TRUE(codeContains(Result, "void method1() override"))
        << "Should add override to method1. Result:\n"
        << Result;
    EXPECT_TRUE(codeContains(Result, "void method2() override"))
        << "Should add override to method2. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddOverrideToOperator) {
    std::string Code = R"(
        class Base {
        public:
            virtual bool operator==(const Base& other) { return true; }
        };
        class Derived : public Base {
        public:
            bool operator==(const Base& other) {} 
        };
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "operator==(const Base& other) override"))
        << "Should NOT add override to operator. Result:\n"
        << Result;
}

// range-for fixes tests
TEST_F(RefactorToolTest, AddsReferenceToConstUserTypeInRangeFor) {
    std::string Code = R"(
        struct MyStruct { int value; };
        struct Container {
            MyStruct* begin() { return nullptr; }
            MyStruct* end() { return nullptr; }
            const MyStruct* begin() const { return nullptr; }
            const MyStruct* end() const { return nullptr; }
        };
        void test() {
            const Container c;
            for (const MyStruct element : c) {}
        }
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_TRUE(codeContains(Result, "const MyStruct& element"))
        << "Should add & to user type in range-for. "
           "Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, AddsReferenceToConstAutoUserTypeInRangeFor) {
    std::string Code = R"(
        struct MyStruct { int value; };
        struct Container {
            MyStruct* begin() { return nullptr; }
            MyStruct* end() { return nullptr; }
            const MyStruct* begin() const { return nullptr; }
            const MyStruct* end() const { return nullptr; }
        };
        void test() {
            const Container c;
            for (const auto element : c) {} 
        }
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_TRUE(codeContains(Result, "const auto& element"))
        << "Should add & to const auto (user type) in range-for. "
           "Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddReferenceToConstFundamentalType) {
    std::string Code = R"(
        struct Container {
            int* begin() { return nullptr; }
            int* end() { return nullptr; }
            const int* begin() const { return nullptr; }
            const int* end() const { return nullptr; }
        };
        void test() {
            const Container c;
            for (const int element : c) {}
        }
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "const int& element"))
        << "Should NOT add & to fundamental types. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddReferenceToConstAutoFundamentalType) {
    std::string Code = R"(
        struct Container {
            int* begin() { return nullptr; }
            int* end() { return nullptr; }
            const int* begin() const { return nullptr; }
            const int* end() const { return nullptr; }
        };
        void test() {
            const Container c;
            for (const auto element : c) {}
        }
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "const auto& element"))
        << "Should NOT add & to const auto (fundamental type) in "
           "range-for. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddReferenceToNonConstValue) {
    std::string Code = R"(
        struct MyStruct { int value; };
        struct Container {
            MyStruct* begin() { return nullptr; }
            MyStruct* end() { return nullptr; }
            const MyStruct* begin() const { return nullptr; }
            const MyStruct* end() const { return nullptr; }
        };
        void test() {
            Container c;
            for (MyStruct element : c) {}
        }
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "MyStruct& element"))
        << "Should NOT add & to non-const variable. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddReferenceToAlreadyReferencedValue) {
    std::string Code = R"(
        struct MyStruct { int value; };
        struct Container {
            MyStruct* begin() { return nullptr; }
            MyStruct* end() { return nullptr; }
            const MyStruct* begin() const { return nullptr; }
            const MyStruct* end() const { return nullptr; }
        };
        void test() {
            const Container c;
            for (const MyStruct& element : c) {}
        }
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "const MyStruct& & element"))
        << "Should not duplicate &. Result:\n"
        << Result;
}

TEST_F(RefactorToolTest, DoesNotAddReferenceToPointerTypes) {
    std::string Code = R"(
        struct Container {
            int** begin() { return nullptr; }
            int** end() { return nullptr; }
            int* const* begin() const { return nullptr; }
            int* const* end() const { return nullptr; }
        };
        void test() {
            const Container c;
            for (const int* element : c) {}
        }
    )";

    std::string Result = runRefactorOnCode(Code);

    EXPECT_FALSE(codeContains(Result, "const int*& element"))
        << "Should NOT add & to pointers. Result:\n"
        << Result;
}