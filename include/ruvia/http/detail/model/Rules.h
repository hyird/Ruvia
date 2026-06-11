#pragma once

#include <charconv>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/model/RuleTypes.h"

namespace ruvia::detail::model {

template <typename T>
[[nodiscard]] std::size_t modelSize(const T& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view().size();
    } else {
        return value.size();
    }
}

template <typename T>
[[nodiscard]] long double modelNumber(const T& value) noexcept {
    using ValueT = std::remove_cvref_t<T>;
    if constexpr (detail::isRuviaScalar<ValueT>) {
        return static_cast<long double>(value.value);
    } else {
        return static_cast<long double>(value);
    }
}

template <typename T>
[[nodiscard]] std::string_view modelString(const T& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

[[nodiscard]] inline bool isEmailLike(std::string_view value) noexcept {
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size()) {
        return false;
    }
    const auto dot = value.find('.', at + 1);
    if (dot == std::string_view::npos || dot + 1 >= value.size()) {
        return false;
    }
    for (const char c : value) {
        if (c <= 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

template <typename T>
[[nodiscard]] bool isEmptyValue(const T& value) noexcept {
    if constexpr (detail::isRuviaString<T> || detail::isRuviaArray<T> || detail::isRuviaList<T>) {
        return value.empty();
    } else {
        return false;
    }
}

template <typename T>
[[nodiscard]] constexpr std::string_view expectedTypeName() noexcept {
    using ValueT = std::remove_cvref_t<T>;
    if constexpr (detail::isRuviaString<ValueT>) {
        return "must be a string";
    } else if constexpr (detail::isRuviaArray<ValueT> || detail::isRuviaList<ValueT>) {
        return "must be an array";
    } else if constexpr (JsonBody<ValueT>::value) {
        return "must be an object";
    } else if constexpr (detail::isRuviaScalar<ValueT>) {
        using ScalarT = typename detail::RuviaScalarTraits<ValueT>::value_type;
        if constexpr (std::is_same_v<ScalarT, bool>) {
            return "must be a boolean";
        } else {
            return "must be a number";
        }
    } else {
        return "has invalid type";
    }
}

template <typename T>
[[nodiscard]] constexpr bool modelHasSizeRule() noexcept {
    using ValueT = std::remove_cvref_t<T>;
    return detail::isRuviaString<ValueT> || detail::isRuviaArray<ValueT> || detail::isRuviaList<ValueT>;
}

template <typename T>
[[nodiscard]] constexpr bool modelHasNumberRule() noexcept {
    using ValueT = std::remove_cvref_t<T>;
    if constexpr (detail::isRuviaScalar<ValueT>) {
        using ScalarT = typename detail::RuviaScalarTraits<ValueT>::value_type;
        return std::is_arithmetic_v<ScalarT> && !std::is_same_v<ScalarT, bool>;
    } else {
        return std::is_arithmetic_v<ValueT> && !std::is_same_v<ValueT, bool>;
    }
}

inline void appendPath(
    std::pmr::string& output,
    std::string_view prefix,
    std::string_view field) {
    output.clear();
    if (!prefix.empty()) {
        output.append(prefix.data(), prefix.size());
        output.push_back('.');
    }
    output.append(field.data(), field.size());
}

inline void appendIndexPath(
    std::pmr::string& output,
    std::string_view prefix,
    std::size_t index) {
    output.clear();
    output.append(prefix.data(), prefix.size());
    output.push_back('[');
    char buffer[32]{};
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), index);
    if (ec == std::errc{}) {
        output.append(buffer, static_cast<std::size_t>(ptr - buffer));
    }
    output.push_back(']');
}

template <typename Rule>
[[nodiscard]] constexpr bool isRequiredRule() noexcept {
    return std::is_same_v<std::remove_cvref_t<Rule>, Required>;
}

template <typename Rule>
[[nodiscard]] constexpr bool isDefaultRule() noexcept {
    using RuleT = std::remove_cvref_t<Rule>;
    return requires { typename RuleT::RuviaDefaultRuleMarker; };
}

template <typename Rule>
[[nodiscard]] constexpr bool isModelOption() noexcept {
    using RuleT = std::remove_cvref_t<Rule>;
    return requires { typename RuleT::RuviaModelOptionMarker; };
}

template <typename Rule>
[[nodiscard]] constexpr bool isValidationRule() noexcept {
    using RuleT = std::remove_cvref_t<Rule>;
    return requires { typename RuleT::RuviaValidationRuleMarker; };
}

template <typename... OptionTs>
class ModelOptions final {
public:
    constexpr explicit ModelOptions(OptionTs... options) noexcept : options_(options...) {
        static_assert((isModelOption<OptionTs>() && ...),
            "RUVIA_FIELD accepts only model options: RUVIA_DEFAULT, RUVIA_OMIT_EMPTY, RUVIA_EMIT_NULL. "
            "Move validation rules to RUVIA_VALIDATE_JSON or RUVIA_VALIDATE_FORM with RUVIA_RULE.");
    }

    [[nodiscard]] constexpr bool emitNull() const noexcept {
        return containsOption<EmitNull>(std::index_sequence_for<OptionTs...>{});
    }

    [[nodiscard]] constexpr bool omitEmpty() const noexcept {
        return containsOption<OmitEmpty>(std::index_sequence_for<OptionTs...>{});
    }

    template <typename OptionalT>
    void applyDefault(OptionalT& value, std::pmr::memory_resource* resource) const {
        if (value) {
            return;
        }
        std::apply(
            [&value, resource](const auto&... options) {
                (applyDefaultOption(value, resource, options), ...);
            },
            options_);
    }

private:
    template <typename OptionT, std::size_t... Indexes>
    [[nodiscard]] constexpr bool containsOption(std::index_sequence<Indexes...>) const noexcept {
        return ((std::is_same_v<std::remove_cvref_t<decltype(std::get<Indexes>(options_))>, OptionT>) || ... || false);
    }

    template <typename OptionalT, typename OptionT>
    static void applyDefaultOption(OptionalT& value, std::pmr::memory_resource* resource, const OptionT& option) {
        if constexpr (isDefaultRule<OptionT>()) {
            using FieldT = typename OptionalT::value_type;
            assignDefaultValue<FieldT>(value, option.value, resource);
        } else {
            (void)value;
            (void)resource;
            (void)option;
        }
    }

    template <typename FieldT, typename ValueT>
    static void assignDefaultValue(
        std::optional<FieldT>& target,
        const ValueT& value,
        std::pmr::memory_resource* resource) {
        if constexpr (detail::isRuviaString<FieldT> && std::is_convertible_v<const ValueT&, std::string_view>) {
            target.emplace(std::string_view(value), resource);
        } else {
            target.emplace(value);
        }
    }

    std::tuple<OptionTs...> options_;
};

template <typename... OptionTs>
ModelOptions(OptionTs...) -> ModelOptions<OptionTs...>;

template <typename... RuleTs>
class Rules final {
public:
    constexpr explicit Rules(RuleTs... rules) noexcept : rules_(rules...) {
        static_assert((isValidationRule<RuleTs>() && ...),
            "RUVIA_RULE accepts only validation rules such as RUVIA_REQUIRED, RUVIA_MIN, RUVIA_MAX, "
            "RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, RUVIA_REGEX, RUVIA_MATCH, RUVIA_CUSTOM, "
            "RUVIA_NESTED, and RUVIA_EACH.");
    }

    [[nodiscard]] constexpr bool required() const noexcept {
        return requiredImpl(std::index_sequence_for<RuleTs...>{});
    }

    [[nodiscard]] constexpr std::string_view requiredMessage() const noexcept {
        return requiredMessageImpl(std::index_sequence_for<RuleTs...>{});
    }

    template <typename OptionalT, typename ValidatorT>
    void validate(
        ModelFieldState state,
        const OptionalT& value,
        std::string_view path,
        ValidatorT& validator) const {
        if (state == ModelFieldState::kDuplicate) {
            validator.add(path, "duplicate", "is duplicated");
            return;
        }
        if (state == ModelFieldState::kInvalidType) {
            using FieldT = typename OptionalT::value_type;
            validator.add(path, "invalid_type", expectedTypeName<FieldT>());
            return;
        }
        if (!value) {
            if (required()) {
                validator.add(path, "required", requiredMessage());
            }
            return;
        }
        validatePresent(*value, path, validator);
    }

private:
    template <typename RuleT, std::size_t... Indexes>
    [[nodiscard]] constexpr bool containsRule(std::index_sequence<Indexes...>) const noexcept {
        return ((std::is_same_v<std::remove_cvref_t<decltype(std::get<Indexes>(rules_))>, RuleT>) || ... || false);
    }

    template <std::size_t... Indexes>
    [[nodiscard]] constexpr bool requiredImpl(std::index_sequence<Indexes...>) const noexcept {
        return ((isRequiredRule<decltype(std::get<Indexes>(rules_))>()) || ... || false);
    }

    template <std::size_t... Indexes>
    [[nodiscard]] constexpr std::string_view requiredMessageImpl(std::index_sequence<Indexes...>) const noexcept {
        std::string_view result{"is required"};
        (setRequiredMessage(result, std::get<Indexes>(rules_)), ...);
        return result;
    }

    template <typename RuleT>
    static constexpr void setRequiredMessage(std::string_view& result, const RuleT& rule) noexcept {
        if constexpr (isRequiredRule<RuleT>()) {
            result = rule.message;
        } else {
            (void)result;
            (void)rule;
        }
    }

    template <typename ValueT, typename ValidatorT>
    void validatePresent(const ValueT& value, std::string_view path, ValidatorT& validator) const {
        std::apply(
            [&value, path, &validator](const auto&... rules) {
                (validateRule(value, path, validator, rules), ...);
            },
            rules_);
    }

    template <typename ValueT, typename ValidatorT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Required&) {
        (void)value;
        (void)path;
        (void)validator;
    }

    template <typename ValueT, typename ValidatorT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Min& rule) {
        if constexpr (modelHasSizeRule<ValueT>()) {
            if (modelSize(value) < static_cast<std::size_t>(rule.value)) {
                validator.add(path, "too_small", rule.message);
            }
        } else if constexpr (modelHasNumberRule<ValueT>()) {
            if (modelNumber(value) < rule.value) {
                validator.add(path, "too_small", rule.message);
            }
        }
    }

    template <typename ValueT, typename ValidatorT, FixedString... Values>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const OneOf<Values...>& rule) {
        const auto actual = modelString(value);
        if (!((actual == Values.view()) || ...)) {
            validator.add(path, "one_of", rule.message);
        }
    }

    template <typename ValueT, typename ValidatorT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Email& rule) {
        if (!isEmailLike(modelString(value))) {
            validator.add(path, "email", rule.message);
        }
    }

    template <typename ValueT, typename ValidatorT, FixedString Pattern>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const PatternRule<Pattern>& rule) {
        const auto actual = modelString(value);
        if (!matchPatternPlan<Pattern>(actual)) {
            validator.add(path, "pattern", rule.message);
        }
    }

    template <typename ValueT, typename ValidatorT, FixedString Pattern>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const RegexRule<Pattern>& rule) {
        try {
            const auto actual = modelString(value);
            const auto& regex = compiledPattern<Pattern>();
            if (!std::regex_match(actual.begin(), actual.end(), regex)) {
                validator.add(path, "regex", rule.message);
            }
        } catch (const std::regex_error&) {
            validator.add(path, "regex", rule.message);
        }
    }

    template <typename ValueT, typename ValidatorT, typename PredicateT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Custom<PredicateT>& rule) {
        if (!rule.predicate(value)) {
            validator.add(path, "custom", rule.message);
        }
    }

    template <typename ValueT, typename ValidatorT, typename PredicateT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Match<PredicateT>& rule) {
        if (!rule.predicate(modelString(value))) {
            validator.add(path, "match", rule.message);
        }
    }

    template <typename ValueT, typename ValidatorT, typename ValidationSchemaT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Nested<ValidationSchemaT>&) {
        static_assert(
            requires(const ValidationSchemaT& schema, const ValueT& nested, std::string_view nestedPath, ValidatorT& nestedValidator) {
                schema.validateNested(nested, nestedPath, nestedValidator);
            },
            "RUVIA_NESTED validator must provide validateNested(const FieldT&, std::string_view, ruvia::Validator&)");
        ValidationSchemaT schema;
        schema.validateNested(value, path, validator);
    }

    template <typename ValueT, typename ValidatorT, typename ValidationSchemaT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Each<ValidationSchemaT>&) {
        static_assert(detail::isRuviaArray<ValueT> || detail::isRuviaList<ValueT>,
            "RUVIA_EACH can only validate ruvia::Array<T> or ruvia::List<T> fields");
        static_assert(
            requires(const ValidationSchemaT& schema, const typename std::remove_cvref_t<ValueT>::value_type& item, std::string_view itemPath, ValidatorT& nestedValidator) {
                schema.validateNested(item, itemPath, nestedValidator);
            },
            "RUVIA_EACH validator must provide validateNested(const ItemT&, std::string_view, ruvia::Validator&)");

        ValidationSchemaT schema;
        std::size_t index = 0;
        for (const auto& item : value) {
            std::pmr::string itemPath(validator.resource());
            appendIndexPath(itemPath, path, index);
            schema.validateNested(item, itemPath, validator);
            ++index;
        }
    }

    template <typename ValueT, typename ValidatorT, typename RuleT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const RuleT&) requires (
            isDefaultRule<RuleT>() ||
            std::is_same_v<std::remove_cvref_t<RuleT>, OmitEmpty> ||
            std::is_same_v<std::remove_cvref_t<RuleT>, EmitNull>) {
        (void)value;
        (void)path;
        (void)validator;
    }

    template <typename ValueT, typename ValidatorT>
    static void validateRule(
        const ValueT& value,
        std::string_view path,
        ValidatorT& validator,
        const Max& rule) {
        if constexpr (modelHasSizeRule<ValueT>()) {
            if (modelSize(value) > static_cast<std::size_t>(rule.value)) {
                validator.add(path, "too_big", rule.message);
            }
        } else if constexpr (modelHasNumberRule<ValueT>()) {
            if (modelNumber(value) > rule.value) {
                validator.add(path, "too_big", rule.message);
            }
        }
    }

    std::tuple<RuleTs...> rules_;
};

template <typename... RuleTs>
Rules(RuleTs...) -> Rules<RuleTs...>;

template <typename FieldT, typename ValueT>
void assignFieldValue(
    std::optional<FieldT>& target,
    ValueT&& value,
    std::pmr::memory_resource* resource) {
    if constexpr (detail::isRuviaString<FieldT> &&
        std::is_same_v<std::remove_cvref_t<ValueT>, std::pmr::string> &&
        std::is_rvalue_reference_v<ValueT&&>) {
        FieldT field(resource);
        field.assignOwned(std::forward<ValueT>(value));
        target.emplace(std::move(field));
    } else if constexpr (detail::isRuviaString<FieldT> &&
        std::is_convertible_v<ValueT&&, std::string_view> &&
        !std::is_same_v<std::remove_cvref_t<ValueT>, FieldT>) {
        FieldT field(resource);
        field.assignOwned(std::string_view(std::forward<ValueT>(value)));
        target.emplace(std::move(field));
    } else {
        target.emplace(std::forward<ValueT>(value));
    }
}

}  // namespace ruvia::detail::model
