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

struct test_case {
    const char* name_;
    void (*fn_)(struct test_context&);
};

struct test_context {
    int failures_ = 0;
    const char* current_ = "";
};

inline std::vector<test_case>& registry() {
    static std::vector<test_case> cases;
    return cases;
}

struct registrar {
    registrar(const char* name, void (*fn)(test_context&)) {
        registry().push_back(test_case{name, fn});
    }
};

template <typename fn_type>
[[nodiscard]] bool throws_on(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

inline void report_failure(test_context& ctx, const char* file, int line, std::string_view expr) {
    ++ctx.failures_;
    std::fprintf(stderr, "  [FAIL] %s\n    at %s:%d\n    check: %.*s\n", ctx.current_, file, line,
        static_cast<int>(expr.size()), expr.data());
}

inline void report_check(test_context& ctx, bool failed, const char* file, int line,
    std::string_view expr) {
    if (failed) {
        report_failure(ctx, file, line, expr);
    }
}

}  // namespace ruvia::testing

#define RUVIA_TEST(name)                                                   \
    static void name(ruvia::testing::test_context&);                       \
    static const ruvia::testing::registrar ruvia_reg_##name{#name, &name}; \
    static void name([[maybe_unused]] ruvia::testing::test_context& ruvia_ctx)

#define RUVIA_CHECK(cond) \
    ruvia::testing::report_check(ruvia_ctx, static_cast<bool>(!(cond)), __FILE__, __LINE__, #cond)

#define RUVIA_CHECK_EQ(a, b) \
    ruvia::testing::report_check(ruvia_ctx, static_cast<bool>(!((a) == (b))), __FILE__, __LINE__, #a " == " #b)
