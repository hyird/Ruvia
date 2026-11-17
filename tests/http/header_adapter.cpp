#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "http_cors.h"
#include "http_header.h"
#include "http_response_headers.h"

namespace {

template <typename name_type, typename value_type>
struct header_field_with_accessors final {
    name_type name() const;
    value_type value() const;
};

static_assert(!ruvia::http_header_field_range<std::vector<header_field_with_accessors<std::string, std::string_view>>>);
static_assert(!ruvia::http_header_field_range<std::vector<header_field_with_accessors<std::string_view, std::string>>>);
static_assert(ruvia::http_header_field_range<std::vector<header_field_with_accessors<const std::string&, const std::string&>>>);

constexpr auto copy_header = [](int) {
    return ruvia::http_header("X-Test", "value");
};
using copied_header_range = decltype(std::views::transform(std::array{1}, copy_header));
static_assert(std::ranges::forward_range<const copied_header_range>);
static_assert(std::ranges::sized_range<const copied_header_range>);
static_assert(!std::is_constructible_v<ruvia::http_header_range_view, const copied_header_range&>);

// A forward-only, by-value range over a single text block and offset table.
class packed_fields final {
    struct entry final {
        std::size_t offset_;
        std::size_t name_size_;
        std::size_t value_size_;
    };

public:
    class const_iterator final {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ruvia::http_header_view;
        using difference_type = std::ptrdiff_t;
        const_iterator() noexcept = default;
        const_iterator(const packed_fields* owner, std::size_t index) noexcept :
            owner_(owner),
            index_(index) {}
        value_type operator*() const noexcept {
            return owner_->at(index_);
        }
        const_iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        const_iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const const_iterator&) const noexcept = default;

    private:
        const packed_fields* owner_{};
        std::size_t index_{};
    };

    packed_fields() {
        text_.reserve(4096);
        entries_.reserve(32);
    }
    std::size_t size() const noexcept {
        return entries_.size();
    }
    const_iterator begin() const noexcept {
        return {this, 0};
    }
    const_iterator end() const noexcept {
        return {this, size()};
    }
    ruvia::http_header_view at(std::size_t index) const noexcept {
        const auto& item = entries_[index];
        return {std::string_view(text_).substr(item.offset_, item.name_size_), std::string_view(text_).substr(item.offset_ + item.name_size_, item.value_size_)};
    }
    void append(std::string_view name, std::string_view value) {
        const std::string bytes = std::string(name) + std::string(value);
        entries_.reserve(size() + 1);
        const auto start = text_.size();
        text_.append(bytes);
        entries_.push_back({start, name.size(), value.size()});
    }
    void set(std::string_view name, std::string_view value) {
        append(name, value);
        const auto retained_name = at(size() - 1).name();
        entries_.erase(std::remove_if(entries_.begin(), entries_.end() - 1, [&](const entry& item) noexcept {
            return ruvia::http_ascii_equals_ignore_case(std::string_view(text_).substr(item.offset_, item.name_size_), retained_name);
        }),
            entries_.end() - 1);
    }
    void remove(std::string_view name) noexcept {
        std::erase_if(entries_, [&](const entry& item) noexcept {
            return ruvia::http_ascii_equals_ignore_case(std::string_view(text_).substr(item.offset_, item.name_size_), name);
        });
    }

private:
    std::string text_;
    std::vector<entry> entries_;
};

static_assert(ruvia::http_header_field_range<packed_fields>);
static_assert(!std::ranges::contiguous_range<packed_fields>);

}  // namespace

TEST(RuviaHttpPublicApi, packed_header_range_and_cors_edit_preserve_unrelated_storage) {
    packed_fields fields;
    fields.append("Content-Type", "text/plain");
    fields.append("X-Unchanged", "payload");
    fields.append("Access-Control-Allow-Origin", "*");
    fields.append("Vary", "Accept-Encoding");
    const auto original = ruvia::find_first_http_header(fields, "X-Unchanged");
    ASSERT_TRUE(original);
    const ruvia::http_header_range_view view(fields);
    EXPECT_EQ(view[1].value(), "payload");
    auto cursor = view.begin();
    ++cursor;
    auto copied = cursor;
    ++cursor;
    EXPECT_EQ((*copied).name(), "X-Unchanged");
    EXPECT_EQ((*cursor).name(), "Access-Control-Allow-Origin");

    ruvia::http_cors_policy_options config;
    config.allowed_origins_.emplace_back("https://app.test");
    config.expose_headers_.emplace_back("X-Trace");
    const ruvia::http_cors_policy policy(config);
    ASSERT_TRUE(policy.rewrite_response_headers("https://app.test", fields));
    EXPECT_EQ(ruvia::find_first_http_header(fields, "Access-Control-Allow-Origin"), "https://app.test");
    EXPECT_EQ(ruvia::find_first_http_header(fields, "Vary"), "Accept-Encoding, Origin");
    EXPECT_EQ(ruvia::find_first_http_header(fields, "Access-Control-Expose-Headers"), "X-Trace");
    const auto unchanged = ruvia::find_first_http_header(fields, "X-Unchanged");
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->data(), original->data());
    EXPECT_FALSE(policy.rewrite_response_headers("https://denied.test", fields));
    EXPECT_FALSE(ruvia::find_first_http_header(fields, "Access-Control-Allow-Origin"));
    EXPECT_EQ(ruvia::find_first_http_header(fields, "X-Unchanged"), "payload");

    ruvia::http_response_headers independent;
    independent.header("Vary", "Accept-Encoding");
    ruvia::merge_http_vary_token(independent, "Origin");
    EXPECT_EQ(independent.header("Vary"), "Accept-Encoding, Origin");
}

TEST(RuviaHttpPublicApi, preflight_scans_packed_field_lines_with_one_shared_budget) {
    ruvia::http_cors_policy_options config;
    config.allow_any_origin_ = true;
    config.allowed_methods_.emplace_back("GET");
    config.reflect_request_headers_ = true;
    const ruvia::http_cors_policy policy(config);
    packed_fields fields;
    fields.append("Origin", "https://app.test");
    fields.append("Access-Control-Request-Method", "GET");
    fields.append("Access-Control-Request-Headers", "X-One, X-Two");
    fields.append("access-control-request-headers", "X-Three");
    const auto allowed = policy.evaluate_preflight("OPTIONS", fields, 3);
    ASSERT_TRUE(allowed);
    EXPECT_EQ(ruvia::find_first_http_header(allowed.headers(), "Access-Control-Allow-Headers"), "X-One, X-Two, X-Three");
    EXPECT_FALSE(policy.evaluate_preflight("OPTIONS", fields, 2));
    fields.set("Access-Control-Request-Headers", "X-One, *");
    EXPECT_FALSE(policy.evaluate_preflight("OPTIONS", fields));
    fields.set("Access-Control-Request-Headers", "X-One");
    fields.append("Origin", "https://other.test");
    EXPECT_FALSE(policy.evaluate_preflight("OPTIONS", fields));
    fields.set("Origin", "https://app.test");
    fields.append("Access-Control-Request-Method", "GET");
    EXPECT_FALSE(policy.evaluate_preflight("OPTIONS", fields));
}
