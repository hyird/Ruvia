#include "test_harness.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/http/JsonUtils.h"

namespace {

using ruvia::detail::appendJsonString;
using ruvia::detail::jsonStringSizeHint;

// The hint must exactly equal what appendJsonString writes, so the output buffer
// is reserved precisely (undersizing would force a reallocation).
void checkConsistent(ruvia::testing::TestContext& ruvia_ctx, std::string_view value) {
    std::string out;
    appendJsonString(out, value);
    RUVIA_CHECK_EQ(jsonStringSizeHint(value), out.size());
}

}  // namespace

RUVIA_TEST(json_string_size_hint_matches_output) {
    checkConsistent(ruvia_ctx, "");
    checkConsistent(ruvia_ctx, "plain text 123");
    checkConsistent(ruvia_ctx, "with \"quote\" and \\ backslash");

    // Every named short escape.
    std::string named = "a";
    named += '\b';
    named += '\f';
    named += '\n';
    named += '\r';
    named += '\t';
    named += 'z';
    checkConsistent(ruvia_ctx, named);

    // Non-named control bytes take the \u00XX form.
    std::string ctrl = "x";
    ctrl += '\x01';
    ctrl += '\x1f';
    checkConsistent(ruvia_ctx, ctrl);

    // An embedded NUL is a control byte.
    std::string withNull = "a";
    withNull += '\0';
    withNull += 'b';
    checkConsistent(ruvia_ctx, withNull);

    // High (UTF-8) bytes pass through as single bytes.
    std::string high = "a";
    high += static_cast<char>(0x80);
    high += static_cast<char>(0xff);
    high += 'b';
    checkConsistent(ruvia_ctx, high);
}
