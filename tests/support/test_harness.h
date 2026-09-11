#pragma once

// Minimal dependency-free test harness for Ruvia unit tests. Tests register
// themselves with RUVIA_TEST; the shared main() in test_main.cpp runs them all
// and reports pass/fail counts, returning non-zero if anything failed.

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::testing {

struct TestCase {
    const char* name;
    void (*fn)(struct TestContext&);
};

struct TestContext {
    int failures = 0;
    const char* current = "";
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)(TestContext&)) {
        registry().push_back(TestCase{name, fn});
    }
};

template <typename Fn>
[[nodiscard]] bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

inline void reportFailure(TestContext& ctx, const char* file, int line, std::string_view expr) {
    ++ctx.failures;
    std::fprintf(stderr, "  [FAIL] %s\n    at %s:%d\n    check: %.*s\n", ctx.current, file, line,
        static_cast<int>(expr.size()), expr.data());
}

inline void reportCheck(TestContext& ctx, bool failed, const char* file, int line,
    std::string_view expr) {
    if (failed) {
        reportFailure(ctx, file, line, expr);
    }
}

}  // namespace ruvia::testing

#define RUVIA_TEST(name)                                                   \
    static void name(ruvia::testing::TestContext&);                        \
    static const ruvia::testing::Registrar ruvia_reg_##name{#name, &name}; \
    static void name([[maybe_unused]] ruvia::testing::TestContext& ruvia_ctx)

#define RUVIA_CHECK(cond) \
    ruvia::testing::reportCheck(ruvia_ctx, static_cast<bool>(!(cond)), __FILE__, __LINE__, #cond)

#define RUVIA_CHECK_EQ(a, b) \
    ruvia::testing::reportCheck(ruvia_ctx, static_cast<bool>(!((a) == (b))), __FILE__, __LINE__, #a " == " #b)
