#pragma once

// Minimal dependency-free test harness for Ruvia unit tests. Tests register
// themselves with RUVIA_TEST; the shared main() in test_main.cpp runs them all
// and reports pass/fail counts, returning non-zero if anything failed.

#include <print>
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

inline void reportFailure(TestContext& ctx, const char* file, int line, std::string_view expr) {
    ++ctx.failures;
    std::print(stderr, "  [FAIL] {}\n    at {}:{}\n    check: {}\n",
               ctx.current, file, line, expr);
}

}  // namespace ruvia::testing

#define RUVIA_TEST(name)                                                            \
    static void name(ruvia::testing::TestContext&);                                 \
    static const ruvia::testing::Registrar ruvia_reg_##name{#name, &name};          \
    static void name(ruvia::testing::TestContext& ruvia_ctx)

#define RUVIA_CHECK(cond)                                                           \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ruvia::testing::reportFailure(ruvia_ctx, __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)

#define RUVIA_CHECK_EQ(a, b)                                                        \
    do {                                                                            \
        if (!((a) == (b))) {                                                        \
            ruvia::testing::reportFailure(ruvia_ctx, __FILE__, __LINE__,            \
                                          #a " == " #b);                            \
        }                                                                           \
    } while (0)
