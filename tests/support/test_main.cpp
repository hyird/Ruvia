#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "test_harness.h"

int main() {
#if defined(_WIN32) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif

    using namespace ruvia::testing;
    int total_failures = 0;
    int failed_cases = 0;
    std::size_t executed_cases = 0;
    auto& cases = registry();
    const char* filter = std::getenv("RUVIA_TEST_FILTER");
    const char* first_text = std::getenv("RUVIA_TEST_FIRST");
    const char* last_text = std::getenv("RUVIA_TEST_LAST");
    const auto first = first_text != nullptr
                           ? static_cast<std::size_t>(std::strtoull(first_text, nullptr, 10))
                           : std::size_t{0};
    const auto last = last_text != nullptr
                          ? static_cast<std::size_t>(std::strtoull(last_text, nullptr, 10))
                          : cases.size();
    std::size_t case_index = 0;
    for (auto& c : cases) {
        if (case_index < first || case_index > last) {
            ++case_index;
            continue;
        }
        if (filter != nullptr) {
            const std::string_view filters(filter);
            bool matched = false;
            for (std::size_t begin = 0; begin <= filters.size();) {
                const auto end = filters.find('|', begin);
                const auto token = filters.substr(
                    begin, end == std::string_view::npos ? filters.size() - begin : end - begin);
                if (!token.empty() &&
                    std::string_view(c.name_).find(token) != std::string_view::npos) {
                    matched = true;
                    break;
                }
                if (end == std::string_view::npos) {
                    break;
                }
                begin = end + 1;
            }
            if (!matched) {
                ++case_index;
                continue;
            }
        }
        ++executed_cases;
        test_context ctx;
        ctx.current_ = c.name_;
        std::printf("[ RUN ] %s (#%zu)\n", c.name_, case_index);
        std::fflush(stdout);
        try {
            c.fn_(ctx);
        } catch (const std::exception& error) {
            report_failure(ctx, __FILE__, __LINE__, error.what());
        } catch (...) {
            report_failure(ctx, __FILE__, __LINE__, "unknown exception");
        }
        if (ctx.failures_ == 0) {
            std::printf("[ ok ] %s\n", c.name_);
        } else {
            std::printf("[FAIL] %s (%d checks failed)\n", c.name_, ctx.failures_);
            ++failed_cases;
        }
        std::fflush(stdout);
        total_failures += ctx.failures_;
        ++case_index;
    }
    if (executed_cases == 0) {
        std::fputs("No test cases were selected. Check RUVIA_TEST_FILTER, RUVIA_TEST_FIRST, and RUVIA_TEST_LAST.\n", stderr);
        return 1;
    }
    std::printf("\n%zu tests, %d failed cases, %d failed checks\n", executed_cases, failed_cases,
        total_failures);
    return total_failures == 0 ? 0 : 1;
}
