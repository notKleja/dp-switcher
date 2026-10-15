#pragma once
// Deliberately tiny, dependency-free test scaffolding so the build stays offline-friendly.
#include <cstdio>
#include <string>
#include <vector>
#include <functional>

struct TestFailure { std::string message; };

struct TestCase { const char* name; void (*fn)(); };

inline std::vector<TestCase>& AllTests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const char* name, void (*fn)()) { AllTests().push_back({ name, fn }); }
};

#define TEST_CASE(name) \
    static void name(); \
    static TestRegistrar name##_registrar(#name, name); \
    static void name()

#define CHECK(expr) \
    do { if (!(expr)) throw TestFailure{ std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #expr ") failed" }; } while (0)

#define CHECK_EQ(a, b) \
    do { \
        auto _va = (a); auto _vb = (b); \
        if (!(_va == _vb)) throw TestFailure{ std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK_EQ(" #a ", " #b ") failed: " \
            + std::to_string(static_cast<long long>(_va)) + " != " + std::to_string(static_cast<long long>(_vb)) }; \
    } while (0)

inline int RunAllTests() {
    int failed = 0;
    for (const auto& t : AllTests()) {
        try {
            t.fn();
            std::printf("  ok    %s\n", t.name);
        } catch (const TestFailure& f) {
            ++failed;
            std::printf("  FAIL  %s\n        %s\n", t.name, f.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("  FAIL  %s\n        exception: %s\n", t.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", AllTests().size(), failed);
    return failed == 0 ? 0 : 1;
}
