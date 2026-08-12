#pragma once

#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/web/detail/model/pattern/PatternCompiler.h"

namespace ruvia::detail::model {

// Validation schemas are retained by route/controller metadata.  A raw
// string_view here would accept std::string temporaries and leave a dangling
// message in the startup-built schema.  Keep the zero-copy representation,
// but make the lifetime contract part of the type.
class BorrowedText final {
public:
    constexpr BorrowedText() noexcept = default;

    constexpr BorrowedText(std::string_view value) noexcept
        : value_(value) {}

    constexpr BorrowedText(const char* value) noexcept
        : value_(::ruvia::detail::httpBorrowedCStringView(value)) {}

    template <typename Traits, typename Allocator>
    constexpr BorrowedText(const std::basic_string<char, Traits, Allocator>& value) noexcept
        : value_(value) {}

    template <::ruvia::detail::HttpTemporaryOwningCharString String>
    BorrowedText(String&&) = delete;

    constexpr BorrowedText& operator=(std::string_view value) noexcept {
        value_ = value;
        return *this;
    }

    constexpr BorrowedText& operator=(const char* value) noexcept {
        value_ = ::ruvia::detail::httpBorrowedCStringView(value);
        return *this;
    }

    template <typename Traits, typename Allocator>
    constexpr BorrowedText& operator=(const std::basic_string<char, Traits, Allocator>& value) noexcept {
        value_ = std::string_view(value);
        return *this;
    }

    template <::ruvia::detail::HttpTemporaryOwningCharString String>
    BorrowedText& operator=(String&&) = delete;

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return value_;
    }

    friend constexpr bool operator==(BorrowedText left, BorrowedText right) noexcept {
        return left.value_ == right.value_;
    }

    friend constexpr bool operator==(BorrowedText left, std::string_view right) noexcept {
        return left.value_ == right;
    }

    friend constexpr bool operator==(BorrowedText left, const char* right) noexcept {
        return left.value_ == ::ruvia::detail::httpBorrowedCStringView(right);
    }

private:
    std::string_view value_;
};

static_assert(sizeof(BorrowedText) == sizeof(std::string_view));

struct Required final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"is required"};
};

struct Min final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    BorrowedText message{"is too small"};
};

struct Max final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    BorrowedText message{"is too big"};
};

template <FixedString... Values>
struct OneOf final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"is not allowed"};
};

struct Email final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"must be a valid email"};
};

template <typename PredicateT>
struct Custom final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"is invalid"};
    PredicateT predicate;
};

template <typename ValidatorT>
struct Nested final {
    using RuviaValidationRuleMarker = void;
};

template <typename ValidatorT>
struct Each final {
    using RuviaValidationRuleMarker = void;
};

template <typename ValueT>
struct Default final {
    using RuviaDefaultRuleMarker = void;
    using RuviaModelOptionMarker = void;

    ValueT value;
};

template <typename ValueT>
Default(ValueT) -> Default<ValueT>;

template <auto Provider>
struct StaticDefault final {
    using RuviaDefaultRuleMarker = void;
    using RuviaModelOptionMarker = void;

    using value_type = std::remove_cvref_t<decltype(Provider())>;
    value_type value{Provider()};
};

struct OmitEmpty final {
    using RuviaModelOptionMarker = void;
};

struct EmitNull final {
    using RuviaModelOptionMarker = void;
};

template <FixedString Pattern>
struct PatternRule final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto plan = CompiledPatternPlan<Pattern>::value;

    BorrowedText message{"has invalid format"};
};

template <FixedString Pattern>
struct RegexRule final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"has invalid format"};
};

}  // namespace ruvia::detail::model
