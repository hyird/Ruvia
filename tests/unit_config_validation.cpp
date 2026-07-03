#include "test_harness.h"

#include <chrono>
#include <stdexcept>
#include <string_view>

#include "core/ConfigValidation.h"

namespace {

using ruvia::detail::ensureNonNegativeDurations;
using ruvia::detail::ensureNonZeroPort;
using ruvia::detail::ensurePositiveDuration;
using ruvia::detail::ensurePositiveSize;
using ruvia::detail::isValidConfigHost;
using ruvia::detail::kSeparatedPortHostRules;

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(config_host_validation_default_rules) {
    RUVIA_CHECK(isValidConfigHost("localhost"));
    RUVIA_CHECK(isValidConfigHost("0.0.0.0"));
    RUVIA_CHECK(isValidConfigHost("example.com"));
    RUVIA_CHECK(isValidConfigHost("::1"));
    RUVIA_CHECK(!isValidConfigHost(""));                            // empty
    RUVIA_CHECK(!isValidConfigHost("has space"));                   // control/space bytes
    RUVIA_CHECK(!isValidConfigHost("a/b"));                         // '/'
    RUVIA_CHECK(!isValidConfigHost("a\\b"));                        // '\\'
    RUVIA_CHECK(!isValidConfigHost(std::string_view("a\rb", 3)));   // CR
    RUVIA_CHECK(!isValidConfigHost(std::string_view("a\x7f" "b", 3)));  // DEL
}

RUVIA_TEST(config_host_validation_separated_port_rules) {
    // For a "host:port" style listen address, brackets and a single colon are
    // disallowed (the colon separates the port).
    RUVIA_CHECK(!isValidConfigHost("[::1]", kSeparatedPortHostRules));   // brackets rejected
    RUVIA_CHECK(!isValidConfigHost("host:80", kSeparatedPortHostRules)); // single colon rejected
    RUVIA_CHECK(isValidConfigHost("host", kSeparatedPortHostRules));     // bare host is fine
    RUVIA_CHECK(isValidConfigHost("::1", kSeparatedPortHostRules));      // two colons is not "single"
}

RUVIA_TEST(config_size_port_duration_guards) {
    RUVIA_CHECK(throwsInvalid([] { ensurePositiveSize(0, "size"); }));
    RUVIA_CHECK(!throwsInvalid([] { ensurePositiveSize(1, "size"); }));

    RUVIA_CHECK(throwsInvalid([] { ensureNonZeroPort(0, "port"); }));
    RUVIA_CHECK(!throwsInvalid([] { ensureNonZeroPort(8080, "port"); }));

    using namespace std::chrono;
    // All non-negative is fine; any single negative throws.
    RUVIA_CHECK(!throwsInvalid([] { ensureNonNegativeDurations("d", seconds(0), seconds(5), milliseconds(1)); }));
    RUVIA_CHECK(throwsInvalid([] { ensureNonNegativeDurations("d", seconds(5), seconds(-1)); }));

    // Positive means strictly greater than zero.
    RUVIA_CHECK(throwsInvalid([] { ensurePositiveDuration(seconds(0), "d"); }));
    RUVIA_CHECK(!throwsInvalid([] { ensurePositiveDuration(milliseconds(1), "d"); }));
}
