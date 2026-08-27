#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/web/detail/model/parse/FormParser.h"

namespace {

using ruvia::detail::parseFormBool;
using ruvia::detail::parseFormNumber;
using ruvia::detail::parseFormValue;

}  // namespace

RUVIA_TEST(form_bool_accepts_strict_set_only) {
    RUVIA_CHECK(parseFormBool("true") == true);
    RUVIA_CHECK(parseFormBool("1") == true);
    RUVIA_CHECK(parseFormBool("false") == false);
    RUVIA_CHECK(parseFormBool("0") == false);

    // Strict: no case-folding, no yes/on, no other numerics, no surrounding space.
    for (const std::string_view bad : {"TRUE", "False", "yes", "on", "2", "", " 1", "1 "}) {
        RUVIA_CHECK(!parseFormBool(bad).has_value());
    }
}

RUVIA_TEST(form_number_integer_is_strict) {
    RUVIA_CHECK(parseFormNumber<int>("42") == 42);
    RUVIA_CHECK(parseFormNumber<int>("-7") == -7);

    // Rejections: empty, trailing junk, fractional (no truncation to 1), overflow.
    RUVIA_CHECK(!parseFormNumber<int>("").has_value());
    RUVIA_CHECK(!parseFormNumber<int>("9x").has_value());
    RUVIA_CHECK(!parseFormNumber<int>("1.5").has_value());
    RUVIA_CHECK(!parseFormNumber<int>("99999999999999999999").has_value());

    // A negative cannot bind an unsigned target.
    RUVIA_CHECK(!parseFormNumber<unsigned>("-5").has_value());
    RUVIA_CHECK(parseFormNumber<unsigned>("5") == 5u);
}

RUVIA_TEST(form_number_floating_accepts_fraction_and_exponent) {
    RUVIA_CHECK(parseFormNumber<double>("1.5") == 1.5);
    RUVIA_CHECK(parseFormNumber<double>("-2e3") == -2000.0);
    RUVIA_CHECK(!parseFormNumber<double>("").has_value());
    RUVIA_CHECK(
        !parseFormNumber<double>("1.2.3").has_value());  // trailing junk after a valid prefix
}

RUVIA_TEST(form_number_floating_rejects_non_finite) {
    // The floating parser accepts these, but the JSON grammar rejects them on input,
    // the model JSON writer maps them to null, and the finite formatter throws --
    // so a bound floating field must never become inf/nan.
    RUVIA_CHECK(!parseFormNumber<double>("inf").has_value());
    RUVIA_CHECK(!parseFormNumber<double>("infinity").has_value());
    RUVIA_CHECK(!parseFormNumber<double>("nan").has_value());
    RUVIA_CHECK(!parseFormNumber<double>("-inf").has_value());
    RUVIA_CHECK(!parseFormNumber<float>("inf").has_value());
}

RUVIA_TEST(form_value_decode_failure_returns_no_partial_value) {
    auto* resource = std::pmr::get_default_resource();

    const auto rejected = parseFormValue<ruvia::String>(
        "decoded%2", ruvia::detail::FormValueEncoding::kUrlEncoded, resource);
    RUVIA_CHECK(!rejected.has_value());

    const auto parsed = parseFormValue<ruvia::String>(
        "decoded%20value", ruvia::detail::FormValueEncoding::kUrlEncoded, resource);
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK_EQ(parsed->view(), std::string_view("decoded value"));
}

RUVIA_TEST(form_value_encoding_is_explicit) {
    auto* resource = std::pmr::get_default_resource();

    const auto encoded = parseFormValue<ruvia::Int32>(
        "%34%32", ruvia::detail::FormValueEncoding::kUrlEncoded, resource);
    RUVIA_CHECK(encoded.has_value());
    RUVIA_CHECK_EQ(static_cast<std::int32_t>(*encoded), 42);

    const auto decoded = parseFormValue<ruvia::String>(
        "literal%20value+plus", ruvia::detail::FormValueEncoding::kDecoded, resource);
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK_EQ(decoded->view(), std::string_view("literal%20value+plus"));
}
