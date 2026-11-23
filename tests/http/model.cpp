#include "model.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ruvia::test_model_ns {

enum class order_status : std::uint8_t {
    pending,
    paid,
    cancelled,
};

RUVIA_ENUM(order_status,
    RUVIA_ENUM_VALUE(order_status::pending, "pending"),
    RUVIA_ENUM_VALUE(order_status::paid, "paid"),
    RUVIA_ENUM_VALUE(order_status::cancelled, "cancelled"));

RUVIA_MODEL(scalar_and_rich_model,
    RUVIA_MODEL_NAMING(ruvia::naming_convention::camel_case),
    RUVIA_REQUIRED_FIELD(user_id, ruvia::uuid),
    RUVIA_REQUIRED_FIELD(active_flag, bool),
    RUVIA_REQUIRED_FIELD(small_count, std::int16_t, RUVIA_GT(0, "must be positive"), RUVIA_LT(1000, "must be < 1000")),
    RUVIA_REQUIRED_FIELD(snowflake_id, std::int64_t, RUVIA_STRINGIFY_INT64),
    RUVIA_REQUIRED_FIELD(unit_price, ruvia::decimal),
    RUVIA_OPTIONAL_FIELD(tax_rate, ruvia::decimal, RUVIA_DECIMAL_NUMBER),
    RUVIA_REQUIRED_FIELD(status_code, order_status),
    RUVIA_REQUIRED_FIELD(created_at, ruvia::utc_milliseconds),
    RUVIA_REQUIRED_FIELD(birth_date, ruvia::civil_date),
    RUVIA_OPTIONAL_FIELD(raw_b64, ruvia::bytes, RUVIA_BYTES_BASE64),
    RUVIA_OPTIONAL_FIELD(raw_b64url, ruvia::bytes, RUVIA_BYTES_BASE64URL),
    RUVIA_OPTIONAL_FIELD(raw_hex, ruvia::bytes, RUVIA_BYTES_HEX),
    RUVIA_OPTIONAL_FIELD(page_size, std::uint32_t, RUVIA_DEFAULT(20U)));

RUVIA_MODEL(string_rules_model,
    RUVIA_REQUIRED_FIELD(display_name, ruvia::string,
        RUVIA_ALIAS("legacy_name", "nick"),
        RUVIA_MIN_CHARS(2, "at least 2 chars"),
        RUVIA_MAX_CHARS(4, "at most 4 chars"),
        RUVIA_MAX_BYTES(12, "at most 12 bytes")),
    RUVIA_OPTIONAL_FIELD(external_ref, ruvia::string, RUVIA_UUID("must be uuid")),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>,
        RUVIA_NOT_EMPTY("tags required"),
        RUVIA_EACH(RUVIA_MIN_BYTES(2, "tag too short"))),
    RUVIA_OPTIONAL_FIELD(scores, ruvia::map<std::int32_t>,
        RUVIA_NOT_EMPTY("scores required"),
        RUVIA_EACH(RUVIA_MIN(0, "score must be >= 0"))));

inline void validate_range_order(const auto& m, auto& v) {
    if (m.template has_value<"min_val">() && m.template has_value<"max_val">()) {
        if (m.template get<"min_val">() > m.template get<"max_val">()) {
            v.add("max_val", "invalid_range", "max_val must be >= min_val");
        }
    }
}

RUVIA_MODEL(range_filter_model,
    RUVIA_MODEL_VALIDATE(validate_range_order),
    RUVIA_REQUIRED_FIELD(min_val, std::int32_t),
    RUVIA_REQUIRED_FIELD(max_val, std::int32_t));

RUVIA_MODEL(card_payment,
    RUVIA_REQUIRED_FIELD(last4, ruvia::string, RUVIA_MIN_BYTES(4, "4 digits"), RUVIA_MAX_BYTES(4, "4 digits")),
    RUVIA_REQUIRED_FIELD(amount, std::int64_t, RUVIA_GT(0, "positive")));

RUVIA_MODEL(bank_payment,
    RUVIA_REQUIRED_FIELD(iban, ruvia::string, RUVIA_NOT_EMPTY("iban required")));

using payment_method = ruvia::tagged_variant<"kind",
    ruvia::tagged_case<"card", card_payment>,
    ruvia::tagged_case<"bank", bank_payment>>;

RUVIA_MODEL(nested_child,
    RUVIA_REQUIRED_FIELD(age, std::int32_t, RUVIA_MIN(18, "adult only")));

RUVIA_MODEL(nested_parent,
    RUVIA_REQUIRED_FIELD(children, ruvia::array<nested_child>),
    RUVIA_OPTIONAL_FIELD(by_key, ruvia::map<nested_child>));

RUVIA_MODEL(form_array_model,
    RUVIA_REQUIRED_FIELD(id, ruvia::array<std::int32_t>),
    RUVIA_OPTIONAL_FIELD(tag, ruvia::array<ruvia::string>));

}  // namespace ruvia::test_model_ns

TEST(RuviaHttpModel, native_scalars_camel_case_uuid_decimal_enum_chrono_and_bytes_roundtrip) {
    using namespace ruvia::test_model_ns;

    static_assert(std::is_same_v<decltype(std::declval<const scalar_and_rich_model&>().get<"small_count">()), const std::int16_t&>);
    static_assert(std::is_same_v<decltype(std::declval<const scalar_and_rich_model&>().get<"page_size">()), const std::uint32_t&>);
    static_assert(std::is_same_v<decltype(std::declval<const scalar_and_rich_model&>().optional<"tax_rate">()), const std::optional<ruvia::decimal>&>);

    constexpr std::string_view json_in = R"({
        "userId": "550E8400-E29B-41D4-A716-446655440000",
        "activeFlag": true,
        "smallCount": 42,
        "snowflakeId": "9007199254740993",
        "unitPrice": "12345.6789",
        "taxRate": 0.075,
        "statusCode": "paid",
        "createdAt": "2026-05-01T10:30:00.250+02:00",
        "birthDate": "1995-12-31",
        "rawB64": "+/8=",
        "rawB64url": "-_8",
        "rawHex": "01aBfF"
    })";

    auto parsed = ruvia::parse_json<scalar_and_rich_model>(json_in);
    ASSERT_TRUE(parsed.has_value());
    const auto& m = parsed.value();
    EXPECT_EQ(m.get<"user_id">().to_string(), "550e8400-e29b-41d4-a716-446655440000");
    EXPECT_EQ(m.get<"user_id">().version(), 4);
    EXPECT_TRUE(m.get<"active_flag">());
    EXPECT_EQ(m.get<"small_count">(), 42);
    EXPECT_EQ(m.get<"snowflake_id">(), 9007199254740993LL);
    EXPECT_EQ(m.get<"unit_price">(), ruvia::decimal("12345.6789"));
    ASSERT_TRUE(m.optional<"tax_rate">().has_value());
    EXPECT_EQ(*m.optional<"tax_rate">(), ruvia::decimal("0.0750"));
    EXPECT_EQ(m.get<"status_code">(), order_status::paid);
    EXPECT_EQ(m.get<"birth_date">(), (std::chrono::year{1995} / 12 / 31));
    // 10:30:00.250+02:00 == 08:30:00.250Z
    const auto expected_tp = std::chrono::sys_days{std::chrono::year{2026} / 5 / 1} + std::chrono::hours{8} + std::chrono::minutes{30} + std::chrono::milliseconds{250};
    EXPECT_EQ(m.get<"created_at">(), expected_tp);
    // Defaulted optional field returns direct const uint32_t&!
    EXPECT_EQ(m.get<"page_size">(), 20U);

    ASSERT_TRUE(m.get<"raw_b64">().has_value());
    EXPECT_EQ(m.get<"raw_b64">()->size(), std::size_t{2});
    EXPECT_EQ((*m.get<"raw_b64">())[0], std::byte{0xFB});
    EXPECT_EQ((*m.get<"raw_b64">())[1], std::byte{0xFF});
    ASSERT_TRUE(m.get<"raw_b64url">().has_value());
    EXPECT_EQ(*m.get<"raw_b64url">(), *m.get<"raw_b64">());
    ASSERT_TRUE(m.get<"raw_hex">().has_value());
    EXPECT_EQ(m.get<"raw_hex">()->size(), std::size_t{3});
    EXPECT_EQ((*m.get<"raw_hex">())[1], std::byte{0xAB});

    // Deep clone + JSON serialization roundtrip
    auto cloned = m.clone();
    const std::string out_json = ruvia::to_json(cloned);
    EXPECT_NE(out_json.find(R"("snowflakeId":"9007199254740993")"), std::string::npos);
    EXPECT_NE(out_json.find(R"("unitPrice":"12345.6789")"), std::string::npos);
    EXPECT_NE(out_json.find(R"("taxRate":0.075)"), std::string::npos);
    EXPECT_NE(out_json.find(R"("createdAt":"2026-05-01T08:30:00.250Z")"), std::string::npos);
    EXPECT_NE(out_json.find(R"("rawB64":"+/8=")"), std::string::npos);
    EXPECT_NE(out_json.find(R"("rawB64url":"-_8")"), std::string::npos);
    EXPECT_NE(out_json.find(R"("rawHex":"01abff")"), std::string::npos);

    const auto reparsed = ruvia::parse_json<scalar_and_rich_model>(out_json);
    ASSERT_TRUE(reparsed.has_value());
    EXPECT_EQ(reparsed.value().get<"snowflake_id">(), 9007199254740993LL);

    // outbound_bytes zero-copy handoff from ruvia::bytes && and ruvia::string &&
    auto taken_b64 = std::move(cloned).take<"raw_b64">();
    ASSERT_TRUE(taken_b64.has_value());
    ruvia::outbound_bytes ob(std::move(*taken_b64));
    EXPECT_EQ(ob.size(), std::size_t{2});
}

TEST(RuviaHttpModel, utf8_chars_vs_bytes_aliases_each_rules_and_cross_field_validate) {
    using namespace ruvia::test_model_ns;

    // "你好" is 2 UTF-8 codepoints (passes MIN_CHARS(2)..MAX_CHARS(4)) and 6 UTF-8 bytes (passes MAX_BYTES(12)),
    // accepted via RUVIA_ALIAS("nick")!
    const auto ok = ruvia::parse_json<string_rules_model>(R"({
        "nick": "你好",
        "external_ref": "550e8400-e29b-41d4-a716-446655440000",
        "tags": ["go", "c++"],
        "scores": {"alice": 10, "bob": 0}
    })");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok.value().get<"display_name">().view(), "你好");
    ASSERT_TRUE(ok.value().get<"scores">().has_value());
    ASSERT_NE(ok.value().get<"scores">()->find("alice"), nullptr);
    EXPECT_EQ(*ok.value().get<"scores">()->find("alice"), 10);

    // "你" is 3 bytes (>= 2 bytes!) but only 1 UTF-8 char -> fails MIN_CHARS(2)!
    const auto too_few_chars = ruvia::parse_json<string_rules_model>(R"({"display_name": "你"})");
    ASSERT_FALSE(too_few_chars.has_value());
    ASSERT_EQ(too_few_chars.error().issues().size(), std::size_t{1});
    EXPECT_EQ(too_few_chars.error().issues()[0].field(), "display_name");
    EXPECT_EQ(too_few_chars.error().issues()[0].message(), "at least 2 chars");

    // 4 4-byte emoji ("😀😀😀😀") = 4 chars (passes MAX_CHARS(4)) but 16 bytes -> fails MAX_BYTES(12)!
    const auto too_many_bytes = ruvia::parse_json<string_rules_model>(R"({"display_name": "😀😀😀😀"})");
    ASSERT_FALSE(too_many_bytes.has_value());
    ASSERT_EQ(too_many_bytes.error().issues().size(), std::size_t{1});
    EXPECT_EQ(too_many_bytes.error().issues()[0].message(), "at most 12 bytes");

    // Element rules RUVIA_EACH on array & map report exact indexed/keyed paths!
    const auto bad_elems = ruvia::parse_json<string_rules_model>(R"({
        "display_name": "abcd",
        "external_ref": "not-a-uuid",
        "tags": ["ok", "x"],
        "scores": {"alice": 5, "mallory": -1}
    })");
    ASSERT_FALSE(bad_elems.has_value());
    const auto& issues = bad_elems.error().issues();
    ASSERT_EQ(issues.size(), std::size_t{3});
    EXPECT_EQ(issues[0].field(), "external_ref");
    EXPECT_EQ(issues[0].code(), "uuid");
    EXPECT_EQ(issues[1].field(), "tags[1]");
    EXPECT_EQ(issues[2].field(), "scores.mallory");

    // Cross-field RUVIA_MODEL_VALIDATE
    const auto bad_range = ruvia::parse_json<range_filter_model>(R"({"min_val": 50, "max_val": 10})");
    ASSERT_FALSE(bad_range.has_value());
    ASSERT_EQ(bad_range.error().issues().size(), std::size_t{1});
    EXPECT_EQ(bad_range.error().issues()[0].field(), "max_val");
    EXPECT_EQ(bad_range.error().issues()[0].code(), "invalid_range");
}

TEST(RuviaHttpModel, tagged_variant_discriminated_union_and_bugfix_regressions) {
    using namespace ruvia::test_model_ns;

    const auto card = ruvia::parse_json<payment_method>(R"({"amount": 2500, "kind": "card", "last4": "4242"})");
    ASSERT_TRUE(card.has_value());
    EXPECT_TRUE(card.value().is<card_payment>());
    EXPECT_EQ(card.value().tag(), "card");
    EXPECT_EQ(card.value().get<card_payment>().get<"amount">(), 2500);
    EXPECT_EQ(ruvia::to_json(card.value()), R"({"kind":"card","last4":"4242","amount":2500})");

    const auto unknown_tag = ruvia::parse_json<payment_method>(R"({"kind": "crypto"})");
    ASSERT_FALSE(unknown_tag.has_value());
    ASSERT_FALSE(unknown_tag.error().issues().empty());
    EXPECT_EQ(unknown_tag.error().issues()[0].code(), "one_of");

    // Bugfix 1: to_json throws std::logic_error when a REQUIRED field is unset!
    range_filter_model incomplete;
    incomplete.set<"min_val">(1);
    EXPECT_THROW((void)ruvia::to_json(incomplete), std::logic_error);

    // Bugfix 2: Nested model validation error paths inside array & map!
    const auto bad_nested = ruvia::parse_json<nested_parent>(R"({
        "children": [{"age": 21}, {"age": 12}],
        "by_key": {"teen": {"age": 15}}
    })");
    ASSERT_FALSE(bad_nested.has_value());
    ASSERT_EQ(bad_nested.error().issues().size(), std::size_t{2});
    EXPECT_EQ(bad_nested.error().issues()[0].field(), "children[1].age");
    EXPECT_EQ(bad_nested.error().issues()[1].field(), "by_key.teen.age");

    // Bugfix 3: reject_unknown_fields_ collects ALL unknown fields across the object!
    const auto unknown_res = ruvia::parse_json<range_filter_model>(
        R"({"extra_a": [1, 2, {"deep": true}], "min_val": 1, "extra_b": "skip", "max_val": 5})",
        {.reject_unknown_fields_ = true});
    ASSERT_FALSE(unknown_res.has_value());
    ASSERT_EQ(unknown_res.error().issues().size(), std::size_t{2});
    EXPECT_EQ(unknown_res.error().issues()[0].field(), "extra_a");
    EXPECT_EQ(unknown_res.error().issues()[0].code(), "unknown_field");
    EXPECT_EQ(unknown_res.error().issues()[1].field(), "extra_b");
    EXPECT_EQ(unknown_res.error().issues()[1].code(), "unknown_field");

    // Escaped string charges max_representation_bytes_ at 2x peak geometric growth:
    // Raw escaped input inside quotes is 20 bytes -> charges sizeof(ruvia::string) + sizeof(std::string) + 40 bytes.
    const auto escaped_fit = ruvia::parse_json<ruvia::string>(
        R"("0123456789abcdef\"xy")",
        {.max_representation_bytes_ = sizeof(ruvia::string) + sizeof(std::string) + 40});
    ASSERT_TRUE(escaped_fit.has_value());
    EXPECT_EQ(escaped_fit.value().view(), "0123456789abcdef\"xy");

    const auto escaped_tight = ruvia::parse_json<ruvia::string>(
        R"("0123456789abcdef\"xy")",
        {.max_representation_bytes_ = sizeof(ruvia::string) + sizeof(std::string) + 39});
    EXPECT_FALSE(escaped_tight.has_value());

    // Zero-copy parse_json_borrowed borrows unescaped string directly from source buffer!
    const std::string backing = R"({"display_name": "zero"})";
    const auto borrowed = ruvia::parse_json_borrowed<string_rules_model>(backing);
    ASSERT_TRUE(borrowed.has_value());
    const auto slice = borrowed.value().get<"display_name">().view();
    EXPECT_EQ(slice, "zero");
    EXPECT_GE(reinterpret_cast<std::uintptr_t>(slice.data()), reinterpret_cast<std::uintptr_t>(backing.data()));
    EXPECT_LT(reinterpret_cast<std::uintptr_t>(slice.data()), reinterpret_cast<std::uintptr_t>(backing.data() + backing.size()));

    // Repeated-key array<T> in parse_form!
    const auto form = ruvia::parse_form<form_array_model>("id=10&tag=alpha&id=20&tag=beta&id=30");
    ASSERT_TRUE(form.has_value());
    ASSERT_EQ(form.value().get<"id">().size(), std::size_t{3});
    EXPECT_EQ(form.value().get<"id">()[0], 10);
    EXPECT_EQ(form.value().get<"id">()[1], 20);
    EXPECT_EQ(form.value().get<"id">()[2], 30);
    ASSERT_TRUE(form.value().get<"tag">().has_value());
    ASSERT_EQ(form.value().get<"tag">()->size(), std::size_t{2});
    EXPECT_EQ((*form.value().get<"tag">())[0].view(), "alpha");
    EXPECT_EQ((*form.value().get<"tag">())[1].view(), "beta");
}
