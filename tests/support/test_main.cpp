#include "test_harness.h"

#include <cstdio>
#include <print>

int main() {
    using namespace ruvia::testing;
    int totalFailures = 0;
    int failedCases = 0;
    auto& cases = registry();
    for (auto& c : cases) {
        TestContext ctx;
        ctx.current = c.name;
        std::println("[ RUN ] {}", c.name);
        std::fflush(stdout);
        c.fn(ctx);
        if (ctx.failures == 0) {
            std::println("[ ok ] {}", c.name);
        } else {
            std::println("[FAIL] {} ({} checks failed)", c.name, ctx.failures);
            ++failedCases;
        }
        std::fflush(stdout);
        totalFailures += ctx.failures;
    }
    std::println("\n{} tests, {} failed cases, {} failed checks",
                 cases.size(), failedCases, totalFailures);
    return totalFailures == 0 ? 0 : 1;
}
