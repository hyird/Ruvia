#include "ruvia/web/detail/model/parse/form_parser.h"

#include <cmath>
#include <memory_resource>
#include <string_view>

#include "test_harness.h"

namespace {

using ruvia::detail::parse_form_bool;
using ruvia::detail::parse_form_number;
using ruvia::detail::parse_form_value;

}  // namespace

RUVIA_TEST(form_bool_accepts_strict_set_only) {
    RUVIA_CHECK(parse_form_bool("true") == true);
    RUVIA_CHECK(parse_form_bool("1") == true);
    RUVIA_CHECK(parse_form_bool("false") == false);
    RUVIA_CHECK(parse_form_bool("0") == false);

    // Strict: no case-folding, no yes/on, no other numerics, no surrounding space.
    for (const std::string_view bad : {"TRUE", "False", "yes", "on", "2", "", " 1", "1 "}) {
        RUVIA_CHECK(!parse_form_bool(bad).has_value());
    }
}

RUVIA_TEST(form_number_integer_is_strict) {
    RUVIA_CHECK(parse_form_number<int>("42") == 42);
    RUVIA_CHECK(parse_form_number<int>("-7") == -7);

    // Rejections: empty, trailing junk, fractional (no truncation to 1), overflow.
    RUVIA_CHECK(!parse_form_number<int>("").has_value());
    RUVIA_CHECK(!parse_form_number<int>("9x").has_value());
    RUVIA_CHECK(!parse_form_number<int>("1.5").has_value());
    RUVIA_CHECK(!parse_form_number<int>("99999999999999999999").has_value());

    // A negative cannot bind an unsigned target.
    RUVIA_CHECK(!parse_form_number<unsigned>("-5").has_value());
    RUVIA_CHECK(parse_form_number<unsigned>("5") == 5u);
}

RUVIA_TEST(form_number_floating_accepts_fraction_and_exponent) {
    RUVIA_CHECK(parse_form_number<double>("1.5") == 1.5);
    RUVIA_CHECK(parse_form_number<double>("-2e3") == -2000.0);
    RUVIA_CHECK(!parse_form_number<double>("").has_value());
    RUVIA_CHECK(
        !parse_form_number<double>("1.2.3").has_value());  // trailing junk after a valid prefix
}

RUVIA_TEST(form_number_float_uses_target_precision_and_range) {
    const auto parsed_value = parse_form_number<float>("1.000000059604644775390626");
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value) {
        RUVIA_CHECK_EQ(*parsed_value, std::nextafter(1.0f, 2.0f));
    }
    RUVIA_CHECK(!parse_form_number<float>("1e-46"));
    RUVIA_CHECK(!parse_form_number<float>("1e39"));
}

RUVIA_TEST(form_number_floating_rejects_non_finite) {
    // The floating parser accepts these, but the JSON grammar rejects them on input,
    // the model JSON writer maps them to null, and the finite formatter throws --
    // so a bound floating field must never become inf/nan.
    RUVIA_CHECK(!parse_form_number<double>("inf").has_value());
    RUVIA_CHECK(!parse_form_number<double>("infinity").has_value());
    RUVIA_CHECK(!parse_form_number<double>("nan").has_value());
    RUVIA_CHECK(!parse_form_number<double>("-inf").has_value());
    RUVIA_CHECK(!parse_form_number<float>("inf").has_value());
}

RUVIA_TEST(form_value_decode_failure_returns_no_partial_value) {
    auto* resource = std::pmr::get_default_resource();

    const auto rejected = parse_form_value<ruvia::string>(
        "decoded%2", ruvia::detail::form_value_encoding::url_encoded, resource);
    RUVIA_CHECK(!rejected.has_value());

    const auto parsed_value = parse_form_value<ruvia::string>(
        "decoded%20value", ruvia::detail::form_value_encoding::url_encoded, resource);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->view(), std::string_view("decoded value"));
}

RUVIA_TEST(form_value_encoding_is_explicit) {
    auto* resource = std::pmr::get_default_resource();

    const auto encoded = parse_form_value<ruvia::int32>(
        "%34%32", ruvia::detail::form_value_encoding::url_encoded, resource);
    RUVIA_CHECK(encoded.has_value());
    RUVIA_CHECK_EQ(static_cast<std::int32_t>(*encoded), 42);

    const auto decoded = parse_form_value<ruvia::string>(
        "literal%20value+plus", ruvia::detail::form_value_encoding::decoded, resource);
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK_EQ(decoded->view(), std::string_view("literal%20value+plus"));
}
