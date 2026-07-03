#include "test_harness.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>

#include "ruvia/http/Csrf.h"

namespace {

using ruvia::detail::generateCsrfToken;

bool isLowerHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

}  // namespace

RUVIA_TEST(csrf_token_is_48_lowercase_hex_chars) {
    std::array<char, 64> buffer{};
    const auto token = generateCsrfToken(buffer);
    RUVIA_CHECK_EQ(token.size(), std::size_t{48});  // 24 random bytes -> 48 hex chars
    for (const char c : token) {
        RUVIA_CHECK(isLowerHex(c));
    }
}

RUVIA_TEST(csrf_token_requires_a_large_enough_buffer) {
    std::array<char, 47> tooSmall{};
    RUVIA_CHECK(generateCsrfToken(tooSmall).empty());  // one byte short -> empty
    std::array<char, 48> exact{};
    RUVIA_CHECK_EQ(generateCsrfToken(exact).size(), std::size_t{48});  // exact fit works
}

RUVIA_TEST(csrf_token_is_unpredictable) {
    std::array<char, 64> a{};
    std::array<char, 64> b{};
    const std::string first(generateCsrfToken(a));
    const std::string second(generateCsrfToken(b));
    RUVIA_CHECK_EQ(first.size(), std::size_t{48});
    RUVIA_CHECK_EQ(second.size(), std::size_t{48});
    // 192 bits of CSPRNG entropy: a repeat is astronomically unlikely.
    RUVIA_CHECK(first != second);
}
