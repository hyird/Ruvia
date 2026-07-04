#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/model/Traits.h"
#include "ruvia/http/detail/model/RuleSupport.h"

namespace {

using ruvia::detail::model::isEmailLike;

}  // namespace

RUVIA_TEST(email_like_accepts_basic_addresses) {
    RUVIA_CHECK(isEmailLike("user@example.com"));
    RUVIA_CHECK(isEmailLike("a@b.c"));  // minimal form
    RUVIA_CHECK(isEmailLike("first.last@sub.example.org"));
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
