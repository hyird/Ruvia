#include "test_harness.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

int main() {
    using namespace ruvia::testing;
    int totalFailures = 0;
    int failedCases = 0;
    std::size_t executedCases = 0;
    auto& cases = registry();
    const char* filter = std::getenv("RUVIA_TEST_FILTER");
    const char* firstText = std::getenv("RUVIA_TEST_FIRST");
    const char* lastText = std::getenv("RUVIA_TEST_LAST");
    const auto first = firstText != nullptr ? static_cast<std::size_t>(std::strtoull(firstText, nullptr, 10)) : std::size_t{0};
    const auto last = lastText != nullptr ? static_cast<std::size_t>(std::strtoull(lastText, nullptr, 10)) : cases.size();
    std::size_t caseIndex = 0;
    for (auto& c : cases) {
        if (caseIndex < first || caseIndex > last) {
            ++caseIndex;
            continue;
        }
        if (filter != nullptr) {
            const std::string_view filters(filter);
            bool matched = false;
            for (std::size_t begin = 0; begin <= filters.size();) {
                const auto end = filters.find('|', begin);
                const auto token = filters.substr(begin, end == std::string_view::npos ? filters.size() - begin : end - begin);
                if (!token.empty() && std::string_view(c.name).find(token) != std::string_view::npos) {
                    matched = true;
                    break;
                }
                if (end == std::string_view::npos) {
                    break;
                }
                begin = end + 1;
            }
            if (!matched) {
                ++caseIndex;
                continue;
            }
        }
        ++executedCases;
        TestContext ctx;
        ctx.current = c.name;
        std::printf("[ RUN ] %s (#%zu)\n", c.name, caseIndex);
        std::fflush(stdout);
        c.fn(ctx);
        if (ctx.failures == 0) {
            std::printf("[ ok ] %s\n", c.name);
        } else {
            std::printf("[FAIL] %s (%d checks failed)\n", c.name, ctx.failures);
            ++failedCases;
        }
        std::fflush(stdout);
        totalFailures += ctx.failures;
        ++caseIndex;
    }
    std::printf("\n%zu tests, %d failed cases, %d failed checks\n", executedCases, failedCases, totalFailures);
    return totalFailures == 0 ? 0 : 1;
}
