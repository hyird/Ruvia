#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/http/request/request_fields_access.h"
#include "ruvia/web/detail/json/json_number.h"
#include "ruvia/web/detail/json/json_scanner.h"
#include "ruvia/web/detail/json/json_string.h"
#include "ruvia/web/detail/model/model_binary.h"
#include "ruvia/web/detail/model/model_input.h"
#include "ruvia/web/detail/model/parse/form_parser.h"
#include "ruvia/web/detail/model/parse/json_writer.h"
#include "ruvia/web/detail/model/parse/model_input_visitors.h"
#include "ruvia/web/detail/model/rule/rules.h"
#include "ruvia/web/detail/model/traits.h"

// Internal JSON value parser for RUVIA_MODEL.

namespace ruvia::detail {

// A parse-local, conservative representation budget. Charging cumulative growth
// (rather than only live sizes) also bounds storage retained by monotonic arenas.
class json_parse_budget final {
public:
    explicit json_parse_budget(model_parse_options options = {}) noexcept
        : elements_(options.max_array_elements_),
          bytes_(options.max_representation_bytes_) {}

    [[nodiscard]] bool consume_bytes(std::size_t bytes_value) noexcept {
        if (bytes_value > bytes_) {
            exhausted_ = true;
            return false;
        }
        bytes_ -= bytes_value;
        return true;
    }

    [[nodiscard]] bool consume_element() noexcept {
        if (elements_ == 0) {
            exhausted_ = true;
            return false;
        }
        --elements_;
        return true;
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return exhausted_;
    }

private:
    std::size_t elements_;
    std::size_t bytes_;
    bool exhausted_{false};
};

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth,
    model_string_storage string_storage, json_parse_budget& budget);

struct model_parse_access final {
    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_value(std::string_view& input,
        std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
        json_parse_budget& budget) {
        if (depth > max_json_depth) {
            return std::nullopt;
        }
        auto value = model::model_access::empty<model_t_type>(resource);
        if (!materialize_json(value, input, depth, string_storage, budget)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_json_borrowed(
        std::string_view body, std::pmr::memory_resource* resource) {
        auto value = parse_json_borrowed_partial<model_t_type>(body, resource);
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_json_borrowed_partial(
        std::string_view body, std::pmr::memory_resource* resource) {
        json_parse_budget budget;
        if (!budget.consume_bytes(sizeof(model_t_type))) {
            return std::nullopt;
        }
        auto input = body;
        auto value = parse_value<model_t_type>(input, resource, 0, model_string_storage::borrowed, budget);
        skip_json_whitespace(input);
        if (!value || !input.empty()) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_form_owned(
        std::string_view body, std::pmr::memory_resource* resource) {
        auto value = materialize_form_input<model_t_type>(
            make_form_model_input(body, resource, model_string_storage::owned));
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_form_borrowed(
        std::string_view body, std::pmr::memory_resource* resource) {
        auto value = parse_form_borrowed_partial<model_t_type>(body, resource);
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_form_borrowed_partial(
        std::string_view body, std::pmr::memory_resource* resource) {
        return materialize_form_input<model_t_type>(make_form_model_input(body, resource));
    }

    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_form_fields(
        const request_name_value_list& fields_value, std::pmr::memory_resource* resource) {
        auto value = parse_form_fields_partial<model_t_type>(fields_value, resource);
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_t_type>
    [[nodiscard]] static std::optional<model_t_type> parse_form_fields_partial(
        const request_name_value_list& fields_value, std::pmr::memory_resource* resource) {
        return materialize_form_input<model_t_type>(make_form_fields_model_input(fields_value, resource));
    }

private:
    template <typename model_type>
    [[nodiscard]] static bool has_valid_structure(const std::optional<model_type>& value) {
        return value && model_validation_access::structure_valid(*value);
    }

    template <typename model_type>
    static void apply_defaults(model_type& value) {
        model::visit_model_fields(value, model::model_access::schema<model_type>(),
            [resource = value.resource()](const auto&, auto& slot) { slot.apply_default(resource); });
    }

    template <typename model_type>
    [[nodiscard]] static std::optional<model_type> materialize_form_input(const model_input& input) {
        auto value = model::model_access::empty<model_type>(input.resource());
        if (!materialize_form(value, input)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_type>
    static bool materialize_json(model_type& value, std::string_view& input, std::size_t depth,
        model_string_storage string_storage, json_parse_budget& budget) {
        auto* const resource = value.resource();
        const bool valid = consume_json_object_fields(resolved_pmr_resource_tag{}, input, resource, depth,
            [&value, resource, depth, string_storage, &budget](
                std::string_view key, std::string_view& value_input) -> bool {
                const auto key_hash = model::model_field_name_hash(key);
                bool field_result = true;
                const bool matched = model::visit_model_field_by_wire_name(value,
                    model::model_access::schema<model_type>(), key_hash, key, field_result,
                    [&](auto& slot) -> bool {
                        if (slot.is_present()) {
                            slot.mark_duplicate();
                            return skip_json_value(value_input, depth + 1);
                        }
                        const auto original_input = value_input;
                        using slot_type = std::remove_cvref_t<decltype(slot)>;
                        using value_type = typename slot_type::value_type;
                        auto null_input = value_input;
                        if (consume_json_literal(null_input, "null")) {
                            value_input = null_input;
                            if constexpr (slot_type::nullable) {
                                slot.mark_null();
                            } else {
                                slot.mark_invalid_type();
                            }
                            return true;
                        }
                        if (auto parsed_value = parse_json_value<value_type>(
                                value_input, resource, depth + 1, string_storage, budget);
                            parsed_value) {
                            model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
                            return true;
                        }
                        if (budget.exhausted()) {
                            return false;
                        }
                        // Wrong-type values need a structural skip only when the
                        // value parser did not consume a complete JSON token.
                        if (value_input.data() == original_input.data() &&
                            !skip_json_value(value_input, depth + 1)) {
                            return false;
                        }
                        slot.mark_invalid_type();
                        return true;
                    });
                return matched ? field_result : skip_json_value(value_input, depth + 1);
            });
        if (valid) {
            apply_defaults(value);
        }
        return valid;
    }

    template <typename model_type>
    static bool materialize_form(model_type& value, const model_input& input) {
        auto* const resource = value.resource();
        const auto encoding = input.kind() == model_input_kind::form_fields
                                  ? form_value_encoding::decoded
                                  : form_value_encoding::url_encoded;
        const auto* fields_value = input.fields();
        const bool case_insensitive =
            fields_value != nullptr && request_name_value_list_access::case_insensitive(*fields_value);
        const auto string_storage = input.string_storage();
        const bool valid = visit_model_input_form_fields(input,
            [&value, resource, encoding, fields_value, case_insensitive, string_storage](
                std::string_view key, std::string_view field_value) {
                auto bind = [&](auto& slot) {
                    using slot_type = std::remove_cvref_t<decltype(slot)>;
                    if constexpr (is_form_field<typename slot_type::value_type>) {
                        if (slot.is_present()) {
                            slot.mark_duplicate();
                            return;
                        }
                        using value_type = typename slot_type::value_type;
                        auto parsed_value = parse_form_value<value_type>(resolved_pmr_resource_tag{},
                            field_value, encoding, resource, string_storage);
                        if (parsed_value) {
                            model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
                        } else {
                            slot.mark_invalid_type();
                        }
                    }
                };
                if (case_insensitive) {
                    bool matched = false;
                    model::visit_model_fields(value, model::model_access::schema<model_type>(),
                        [&](const auto&, auto& slot) {
                            using slot_type = std::remove_cvref_t<decltype(slot)>;
                            if constexpr (is_form_field<typename slot_type::value_type>) {
                                if (matched || !request_name_value_list_access::names_equal(
                                                   *fields_value, key, slot.wire_name())) {
                                    return;
                                }
                                matched = true;
                                bind(slot);
                            }
                        });
                    return true;
                }
                bool field_result = true;
                (void)model::visit_model_field_by_wire_name(value, model::model_access::schema<model_type>(),
                    model::model_field_name_hash(key), key, field_result, [&](auto& slot) -> bool {
                        bind(slot);
                        return true;
                    });
                return field_result;
            });
        if (valid) {
            apply_defaults(value);
        }
        return valid;
    }
};

template <typename sequence_t_type>
struct json_sequence_value_traits;

template <typename value_t_type>
struct json_sequence_value_traits<array<value_t_type>> {
    using value_type = value_t_type;

    static void emplace(array<value_t_type>& value, value_t_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename value_t_type>
struct json_sequence_value_traits<boxed_array<value_t_type>> {
    using value_type = value_t_type;

    static void emplace(boxed_array<value_t_type>& value, value_t_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename sequence_t_type>
[[nodiscard]] std::optional<sequence_t_type> parse_json_sequence_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    using traits_type = json_sequence_value_traits<std::remove_cvref_t<sequence_t_type>>;
    using element_t_type = typename traits_type::value_type;

    if (depth > max_json_depth) {
        return std::nullopt;
    }
    auto remaining = input;
    if (!consume_json_char(remaining, '[')) {
        return std::nullopt;
    }

    sequence_t_type value = make_request_value<sequence_t_type>(resource);
    skip_json_whitespace(remaining);
    if (!remaining.empty() && remaining.front() == ']') {
        remaining.remove_prefix(1);
        input = remaining;
        return value;
    }

    for (;;) {
        if (!budget.consume_element()) {
            return std::nullopt;
        }
        // Four element slots cover all geometric vector allocations, including
        // old buffers held by an arena. Boxed values additionally own pointers.
        constexpr auto element_bytes = 4 * (sizeof(element_t_type) + sizeof(element_t_type*));
        if (!budget.consume_bytes(element_bytes)) {
            return std::nullopt;
        }
        auto element = parse_json_value<element_t_type>(remaining, resource, depth + 1, string_storage, budget);
        if (!element.has_value()) {
            return std::nullopt;
        }
        traits_type::emplace(value, std::move(*element));

        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == ']') {
            remaining.remove_prefix(1);
            input = remaining;
            return value;
        }
        if (!consume_json_char(remaining, ',')) {
            return std::nullopt;
        }
    }
}

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    using field_t_type = std::remove_cvref_t<t_type>;
    if (depth > max_json_depth || budget.exhausted()) {
        return std::nullopt;
    }
    auto remaining = input;
    if constexpr (is_ruvia_string<field_t_type>) {
        const auto parsed_value = parse_json_string(remaining);
        if (!parsed_value.has_value()) {
            return std::nullopt;
        }
        if ((string_storage == model_string_storage::owned ||
                parsed_value->encoding() != json_string_encoding::literal) &&
            (!budget.consume_bytes(parsed_value->raw().size()) ||
                !budget.consume_bytes(parsed_value->raw().size() + sizeof(std::pmr::string)))) {
            return std::nullopt;
        }
        if (parsed_value->encoding() == json_string_encoding::literal) {
            input = remaining;
            if (string_storage == model_string_storage::owned) {
                return field_t_type(parsed_value->raw(), ::ruvia::model_options{.resource_ = resource});
            }
            return model_value_factory::make_string(parsed_value->raw(), resource);
        }
        auto decoded = decode_json_string(parsed_value->raw(), resource);
        input = remaining;
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        field_t_type value = make_request_value<field_t_type>(resource);
        value.assign_owned(std::move(*decoded));
        return value;
    } else if constexpr (is_ruvia_bytes<field_t_type>) {
        // Escaped base64 may allocate both a decoded token and a byte value.
        const auto token = parse_json_string(remaining);
        if (!token || !budget.consume_bytes(token->raw().size()) ||
            !budget.consume_bytes(token->raw().size() + sizeof(std::pmr::string))) {
            return std::nullopt;
        }
        std::optional<bytes> value;
        if (token->encoding() == json_string_encoding::literal) {
            value = decode_model_binary(token->raw(), resource);
        } else {
            const auto decoded = decode_json_string(token->raw(), resource);
            if (!decoded) {
                return std::nullopt;
            }
            value = decode_model_binary(*decoded, resource);
        }
        if (!value) {
            return std::nullopt;
        }
        input = remaining;
        return value;
    } else if constexpr (std::is_same_v<field_t_type, std::string_view>) {
        const auto parsed_value = parse_json_string(remaining);
        if (!parsed_value.has_value() || parsed_value->encoding() != json_string_encoding::literal) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value->raw();
    } else if constexpr (is_ruvia_array<field_t_type> || is_ruvia_boxed_array<field_t_type>) {
        auto parsed_value = parse_json_sequence_value<field_t_type>(remaining, resource, depth, string_storage, budget);
        if (!parsed_value.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value;
    } else if constexpr (is_ruvia_scalar<field_t_type>) {
        using scalar_t_type = model_scalar_value_t_type<field_t_type>;
        scalar_t_type parsed_value{};
        if constexpr (std::is_same_v<scalar_t_type, bool>) {
            if (consume_json_literal(remaining, "true")) {
                parsed_value = true;
            } else if (consume_json_literal(remaining, "false")) {
                parsed_value = false;
            } else {
                return std::nullopt;
            }
        } else {
            if (!parse_json_number_value(remaining, parsed_value)) {
                return std::nullopt;
            }
        }
        input = remaining;
        return field_t_type(parsed_value);
    } else if constexpr (is_model<field_t_type>) {
        auto nested =
            model_parse_access::parse_value<field_t_type>(remaining, resource, depth, string_storage, budget);
        if (!nested.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return nested;
    } else {
        static_assert(always_false<field_t_type>, "RUVIA_MODEL JSON field type is not supported");
    }
}

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth = 0,
    model_string_storage string_storage = model_string_storage::borrowed) {
    json_parse_budget budget;
    if (!budget.consume_bytes(sizeof(t_type))) {
        return std::nullopt;
    }
    return parse_json_value<t_type>(input, resource, depth, string_storage, budget);
}

// One complete typed JSON value, including structural checks inside arrays.
// Borrowed reads are used by JSON views; standalone codecs request owned data.
template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_document(std::string_view input,
    std::pmr::memory_resource* resource, model_string_storage string_storage,
    model_parse_options options = {}) {
    json_parse_budget budget(options);
    if (!budget.consume_bytes(sizeof(t_type))) {
        return std::nullopt;
    }
    auto value = parse_json_value<t_type>(input, resource, 0, string_storage, budget);
    skip_json_whitespace(input);
    if (!value || !input.empty() || !model_validation_access::value_structure_valid(*value)) {
        return std::nullopt;
    }
    return value;
}

}  // namespace ruvia::detail
