#include "test_harness.h"

#include <cstdio>

int main() {
    using namespace ruvia::testing;
    int totalFailures = 0;
    int failedCases = 0;
    auto& cases = registry();
    for (auto& c : cases) {
        TestContext ctx;
        ctx.current = c.name;
        std::printf("[ RUN ] %s\n", c.name);
        std::fflush(stdout);
        c.fn(ctx);
        if (ctx.failures == 0) {
            std::printf("[ ok ] %s\n", c.name);
        } else {
            std::printf(
                "[FAIL] %s (%d checks failed)\n",
                c.name,
                ctx.failures);
            ++failedCases;
        }
        std::fflush(stdout);
        totalFailures += ctx.failures;
    }
    std::printf(
        "\n%zu tests, %d failed cases, %d failed checks\n",
        cases.size(),
        failedCases,
        totalFailures);
    return totalFailures == 0 ? 0 : 1;
}
