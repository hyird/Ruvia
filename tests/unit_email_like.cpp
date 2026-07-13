#include "test_harness.h"

#include <string_view>

#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/model/RuleSupport.h"

namespace {

using ruvia::detail::model::isEmailLike;

}  // namespace

RUVIA_TEST(email_like_accepts_basic_addresses) {
    RUVIA_CHECK(isEmailLike("user@example.com"));
    RUVIA_CHECK(isEmailLike("a@b.c"));  // minimal form
    RUVIA_CHECK(isEmailLike("first.last@sub.example.org"));
}

RUVIA_TEST(email_like_accepts_utf8_and_still_rejects_control_bytes) {
    // A UTF-8 byte (>= 0x80) is not a control character. A signed-char comparison
    // treated it as <= 0x20 and wrongly rejected internationalized addresses
    // (RFC 6531); they must be accepted.
    RUVIA_CHECK(isEmailLike(std::string_view("caf\xC3\xA9@example.com", 17)));   // café@... (é, 2-byte)
    RUVIA_CHECK(isEmailLike(std::string_view("\xE4\xBD\xA0@example.com", 15)));  // 3-byte local part
    // Control bytes, SP, and DEL stay rejected via the same (now unsigned) guard.
    RUVIA_CHECK(!isEmailLike(std::string_view("a@b.c\x01", 6)));  // 0x01 control
    RUVIA_CHECK(!isEmailLike(std::string_view("a@b.c\x7f", 6)));  // DEL
    RUVIA_CHECK(!isEmailLike("user name@ex.com"));                // space
}

RUVIA_TEST(email_like_rejects_malformed) {
    RUVIA_CHECK(!isEmailLike("userexample.com"));  // no '@'
    RUVIA_CHECK(!isEmailLike("@example.com"));      // '@' at the start
    RUVIA_CHECK(!isEmailLike("user@"));             // '@' at the end
    RUVIA_CHECK(!isEmailLike("user@example"));      // no '.' after '@'
    RUVIA_CHECK(!isEmailLike("user@example."));     // '.' at the end
    // The required dot must be in the DOMAIN: a dot only in the local part (the
    // scan starts after '@') does not satisfy it.
    RUVIA_CHECK(!isEmailLike("a.b@cd"));
    RUVIA_CHECK(!isEmailLike("user name@ex.com"));  // a space is a control byte
    RUVIA_CHECK(!isEmailLike(std::string_view("a@b\t.c", 6)));  // a tab is rejected
    // DEL (0x7F) is rejected by the distinct high-control guard, not the <= 0x20 one.
    RUVIA_CHECK(!isEmailLike(std::string_view("a@b.c\x7f", 6)));
    RUVIA_CHECK(!isEmailLike(""));                  // empty
}
