#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/decimal_number.h"
#include "ruvia/web/detail/http/request/request_fields_access.h"
#include "ruvia/web/detail/json/json_containers.h"
#include "ruvia/web/detail/json/json_escape.h"
#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_number.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/json/json_skip.h"
#include "ruvia/web/detail/json/json_string.h"
#include "ruvia/web/detail/model/model_schema.h"
#include "ruvia/web/detail/model/model_text_storage.h"
#include "ruvia/web/detail/model/rule/rule_support.h"
#include "ruvia/web/integer.h"
#include "ruvia/web/model.h"

namespace ruvia {
class request_name_value_list;
}  // namespace ruvia

namespace ruvia::detail {

struct form_body_input final {
    std::string_view body_;
    model_string_storage string_storage_{model_string_storage::borrowed};
};

using model_input = std::variant<form_body_input, std::reference_wrapper<const request_name_value_list>>;

template <typename visitor_type>
    requires std::invocable<visitor_type&, std::string_view, std::string_view>
void visit_form_body_fields(std::string_view body, std::pmr::memory_resource* resource, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    std::pmr::string decoded_name(resource);
    (void)ruvia::visit_url_encoded_pairs(body, [&](std::string_view name, std::string_view value) {
        visitor_ref(*ruvia::decode_url_component(name, decoded_name, url_decode_mode::form), value);
    });
}

enum class form_value_encoding : std::uint8_t { url_encoded,
    decoded };
[[nodiscard]] inline std::optional<bool> parse_form_bool(std::string_view decoded) noexcept {
    if (decoded == "true" || decoded == "1") {
        return true;
    }
    if (decoded == "false" || decoded == "0") {
        return false;
    }
    return std::nullopt;
}

template <typename number_type>
[[nodiscard]] std::optional<number_type> parse_form_number(std::string_view decoded) {
    if (decoded.empty()) {
        return std::nullopt;
    }
    number_type parsed_value{};
    if constexpr (std::is_floating_point_v<number_type>) {
        const auto value = detail::parse_decimal_number<number_type>(decoded);
        if (!value) {
            return std::nullopt;
        }
        parsed_value = *value;
        if (!std::isfinite(parsed_value)) {
            return std::nullopt;
        }
    } else {
        const auto integer = parse_integer<number_type>(decoded);
        if ((integer.index() != 0)) {
            return std::nullopt;
        }
        parsed_value = std::get<0>(integer);
    }
    return parsed_value;
}

[[nodiscard]] inline std::string_view decode_form_value(
    std::string_view input, form_value_encoding encoding, std::pmr::string& storage) {
    if (encoding == form_value_encoding::decoded) {
        return input;
    }
    return *ruvia::decode_url_component(input, storage, url_decode_mode::form);
}

template <typename t_type>
    requires is_form_field<t_type>
[[nodiscard]] std::optional<t_type> parse_form_value(resolved_pmr_resource_tag, std::string_view input,
    form_value_encoding encoding, std::pmr::memory_resource* resource, model_string_storage string_storage) {
    using field_type = std::remove_cvref_t<t_type>;

    if constexpr (is_ruvia_scalar<field_type>) {
        std::array<std::byte, 128> stack_bytes;
        std::pmr::monotonic_buffer_resource stack_resource(stack_bytes.data(), stack_bytes.size(), resource);
        std::pmr::string decoded_storage(&stack_resource);
        const auto decoded = decode_form_value(input, encoding, decoded_storage);
        using scalar_type = model_scalar_value_type<field_type>;
        if constexpr (std::is_same_v<scalar_type, bool>) {
            const auto parsed_value = parse_form_bool(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_type(*parsed_value);
        } else {
            const auto parsed_value = parse_form_number<scalar_type>(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_type(*parsed_value);
        }
    } else {
        std::pmr::string decoded_storage(resource);
        if (decode_form_value(input, encoding, decoded_storage).data() != input.data()) {
            field_type value = make_request_value<field_type>(resolved_pmr_resource_tag{}, resource);
            value.assign_owned(std::move(decoded_storage));
            return value;
        }
        if (string_storage == model_string_storage::owned) {
            return field_type(input, ::ruvia::model_options{.resource_ = resource});
        }
        return model_value_factory::make_string(input, resource);
    }
}

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

    // Charges one owned text of `bytes` plus its string object. `copies` counts
    // the buffers an arena may retain for it: 1 for a single sized copy, more
    // when the text is built by growth or yields a second representation.
    [[nodiscard]] bool consume_owned_text(std::size_t bytes, std::size_t copies = 1) noexcept {
        return consume_owned_text_chunk(bytes, copies, true);
    }

    [[nodiscard]] bool consume_owned_text_chunk(
        std::size_t bytes, std::size_t copies, bool first_chunk) noexcept {
        const auto string_bytes = first_chunk ? sizeof(std::pmr::string) : std::size_t{0};
        if (bytes > (std::numeric_limits<std::size_t>::max() - string_bytes) / copies) {
            exhausted_ = true;
            return false;
        }
        return consume_bytes(bytes * copies + string_bytes);
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

    [[nodiscard]] std::size_t element_checkpoint() const noexcept {
        return elements_;
    }

    void restore_elements(std::size_t checkpoint) noexcept {
        elements_ = checkpoint;
    }

    [[nodiscard]] bool skip_value(std::string_view& input, std::size_t depth) noexcept {
        auto consume = [this]() noexcept { return consume_element(); };
        return skip_json_value(input, depth, consume);
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

// Defined with json_value/json_object in model.h.
template <typename view_type>
[[nodiscard]] std::optional<view_type> parse_json_view_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    bool require_object, json_parse_budget& budget);

struct model_parse_access final {
    template <typename model_type>
    [[nodiscard]] static std::optional<model_type> parse_value(std::string_view& input,
        std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
        json_parse_budget& budget) {
        if (depth > max_json_depth) {
            return std::nullopt;
        }
        auto value = model::model_access::empty<model_type>(resource);
        if (!materialize_json(value, input, depth, string_storage, budget)) {
            return std::nullopt;
        }
        return value;
    }

    // Binds every recognized field without checking field structure; the
    // caller validates. Form input carries no syntax that can fail: values a
    // field cannot hold mark that field invalid_type instead.
    template <typename model_type>
    [[nodiscard]] static model_type parse_form(const model_input& input, std::pmr::memory_resource* resource) {
        auto value = model::model_access::empty<model_type>(resource);
        materialize_form(value, input);
        return value;
    }

    template <typename model_type>
    [[nodiscard]] static std::optional<model_type> parse_form_owned(
        std::string_view body, std::pmr::memory_resource* resource) {
        auto value = parse_form<model_type>(
            form_body_input{.body_ = body, .string_storage_ = model_string_storage::owned}, resource);
        if (!model_validation_access::structure_valid(value)) {
            return std::nullopt;
        }
        return value;
    }

private:
    template <typename model_type>
    static void apply_defaults(model_type& value) {
        model::visit_model_fields(value, model::model_access::schema<model_type>(),
            [resource = value.resource()](const auto&, auto& slot) { slot.apply_default(resource); });
    }

    template <typename slot_type>
    [[nodiscard]] static bool parse_json_field_slot(slot_type& slot, std::string_view& value_input,
        std::pmr::memory_resource* resource, std::size_t depth,
        model_string_storage string_storage, json_parse_budget& budget) {
        if (slot.is_present()) {
            slot.mark_duplicate();
            return budget.skip_value(value_input, depth + 1);
        }
        using unwrapped_slot_type = std::remove_cvref_t<slot_type>;
        using value_type = typename unwrapped_slot_type::value_type;
        auto null_input = value_input;
        if (consume_json_literal(null_input, "null")) {
            value_input = null_input;
            if constexpr (unwrapped_slot_type::nullable) {
                slot.mark_null();
            } else {
                slot.mark_invalid_type();
            }
            return true;
        }
        const auto element_checkpoint = budget.element_checkpoint();
        if (auto parsed_value =
                parse_json_value<value_type>(value_input, resource, depth + 1, string_storage, budget);
            parsed_value) {
            model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
            return true;
        }
        if (budget.exhausted()) {
            return false;
        }
        // A failed value parse leaves the cursor at the value, so the
        // wrong-type value is skipped structurally. The skip rescans the whole
        // value: count its elements once, while retaining byte charges for
        // allocations that an arena may still own.
        budget.restore_elements(element_checkpoint);
        if (!budget.skip_value(value_input, depth + 1)) {
            return false;
        }
        slot.mark_invalid_type();
        return true;
    }

    template <typename model_type>
    [[nodiscard]] static bool visit_json_field(model_type& value, std::string_view key,
        std::string_view& value_input, std::pmr::memory_resource* resource, std::size_t depth,
        model_string_storage string_storage, json_parse_budget& budget) {
        bool field_result = true;
        const bool matched = model::visit_model_field_by_wire_name(value,
            model::model_access::schema<model_type>(), key, field_result,
            [&](auto& slot) -> bool {
                return parse_json_field_slot(slot, value_input, resource, depth, string_storage, budget);
            });
        return matched ? field_result : budget.skip_value(value_input, depth + 1);
    }

    template <typename model_type>
    static bool materialize_json(model_type& value, std::string_view& input, std::size_t depth,
        model_string_storage string_storage, json_parse_budget& budget) {
        auto* const resource = value.resource();
        const auto visit_field = [&value, resource, depth, string_storage, &budget](
                                     std::string_view key, std::string_view& value_input) -> bool {
            return visit_json_field(value, key, value_input, resource, depth, string_storage, budget);
        };
        const auto consume_key_allocation = [&budget](std::size_t bytes_value, bool first_chunk) noexcept {
            return budget.consume_owned_text_chunk(bytes_value, 1, first_chunk);
        };
        const bool valid = consume_json_object_fields(resolved_pmr_resource_tag{}, input, resource,
            depth, visit_field, consume_key_allocation);
        if (valid) {
            apply_defaults(value);
        }
        return valid;
    }

    template <typename slot_type>
    static void bind_form_field_slot(slot_type& slot, std::string_view field_value,
        form_value_encoding encoding, std::pmr::memory_resource* resource,
        model_string_storage string_storage) {
        if (slot.is_present()) {
            slot.mark_duplicate();
            return;
        }
        using value_type = typename std::remove_cvref_t<slot_type>::value_type;
        // Name/value inputs only carry strings and scalars. A value for any
        // other field is a type error, never an absence that a default fills.
        if constexpr (is_form_field<value_type>) {
            auto parsed_value = parse_form_value<value_type>(
                resolved_pmr_resource_tag{}, field_value, encoding, resource, string_storage);
            if (parsed_value) {
                model_value_factory::emplace_parsed(slot, std::move(*parsed_value));
                return;
            }
        }
        slot.mark_invalid_type();
    }

    template <typename model_type>
    static void bind_form_field(model_type& value, std::string_view key, std::string_view field_value,
        form_value_encoding encoding, model_string_storage string_storage) {
        bool field_result = true;
        (void)model::visit_model_field_by_wire_name(value, model::model_access::schema<model_type>(),
            key, field_result, [&](auto& slot) -> bool {
                bind_form_field_slot(slot, field_value, encoding, value.resource(), string_storage);
                return true;
            });
    }

    // Case-insensitive sources (HTTP headers) bind the first field whose wire
    // name equals the name under the source's comparison.
    template <typename model_type>
    static void bind_form_field_ignore_case(model_type& value, const request_name_value_list& fields_value,
        std::string_view key, std::string_view field_value) {
        bool matched = false;
        model::visit_model_fields(value, model::model_access::schema<model_type>(),
            [&](const auto&, auto& slot) {
                if (matched || !request_name_value_list_access::names_equal(fields_value, key, slot.wire_name())) {
                    return;
                }
                matched = true;
                bind_form_field_slot(slot, field_value, form_value_encoding::decoded, value.resource(),
                    model_string_storage::borrowed);
            });
    }

    template <typename model_type>
    static void materialize_form(model_type& value, const model_input& input) {
        if (const auto* body = std::get_if<form_body_input>(&input)) {
            visit_form_body_fields(body->body_, value.resource(),
                [&value, string_storage = body->string_storage_](std::string_view key, std::string_view field_value) {
                    bind_form_field(value, key, field_value, form_value_encoding::url_encoded, string_storage);
                });
        } else {
            const request_name_value_list& fields_value = std::get<1>(input);
            const bool case_insensitive = request_name_value_list_access::case_insensitive(fields_value);
            for (const auto& field : fields_value) {
                if (case_insensitive) {
                    bind_form_field_ignore_case(value, fields_value, field.name(), field.value());
                } else {
                    bind_form_field(value, field.name(), field.value(), form_value_encoding::decoded,
                        model_string_storage::borrowed);
                }
            }
        }
        apply_defaults(value);
    }
};

template <typename sequence_type>
struct json_sequence_value_traits;

template <typename element_type>
struct json_sequence_value_traits<array<element_type>> {
    using value_type = element_type;

    static void emplace(array<element_type>& value, element_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename element_type>
struct json_sequence_value_traits<boxed_array<element_type>> {
    using value_type = element_type;

    static void emplace(boxed_array<element_type>& value, element_type&& element) {
        model_value_factory::emplace_parsed(value, std::move(element));
    }
};

template <typename sequence_type>
[[nodiscard]] std::optional<sequence_type> parse_json_sequence_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    using traits_type = json_sequence_value_traits<std::remove_cvref_t<sequence_type>>;
    using element_type = typename traits_type::value_type;

    sequence_type value = make_request_value<sequence_type>(resource);
    const bool consumed = consume_json_array_elements(input, depth,
        [&value, resource, depth, string_storage, &budget](std::string_view& element_input) {
            if (!budget.consume_element()) {
                return false;
            }
            // Four element slots cover all geometric vector allocations, including
            // old buffers held by an arena. Boxed values additionally own pointers.
            constexpr auto element_bytes = 4 * (sizeof(element_type) + sizeof(element_type*));
            if (!budget.consume_bytes(element_bytes)) {
                return false;
            }
            auto element =
                parse_json_value<element_type>(element_input, resource, depth + 1, string_storage, budget);
            if (!element.has_value()) {
                return false;
            }
            traits_type::emplace(value, std::move(*element));
            return true;
        });
    if (!consumed) {
        return std::nullopt;
    }
    return value;
}

// Every branch commits `input` only when it returns a value.
template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    using field_type = std::remove_cvref_t<t_type>;
    if (depth > max_json_depth || budget.exhausted()) {
        return std::nullopt;
    }
    auto remaining = input;
    if constexpr (is_ruvia_string<field_type>) {
        std::pmr::string decoded(pmr_resource_or_default(resource));
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            // Decoding grows its buffer geometrically, and an arena may retain
            // every superseded buffer.
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token.has_value()) {
            return std::nullopt;
        }
        if (token->encoding() == json_string_encoding::escaped) {
            input = remaining;
            field_type value = make_request_value<field_type>(resource);
            value.assign_owned(std::move(decoded));
            return value;
        }
        if (string_storage == model_string_storage::borrowed) {
            input = remaining;
            return model_value_factory::make_string(token->raw(), resource);
        }
        if (!budget.consume_owned_text(token->raw().size())) {
            return std::nullopt;
        }
        input = remaining;
        return field_type(token->raw(), ::ruvia::model_options{.resource_ = resource});
    } else if constexpr (is_ruvia_bytes<field_type>) {
        std::pmr::string decoded(pmr_resource_or_default(resource));
        bool started = false;
        const auto token = parse_json_string(remaining, &decoded, [&](std::size_t chunk_bytes) noexcept {
            // Escaped base64 may retain both decoded text and the byte value.
            const bool first = !std::exchange(started, true);
            return budget.consume_owned_text_chunk(chunk_bytes, 2, first);
        });
        if (!token ||
            (token->encoding() == json_string_encoding::literal &&
                !budget.consume_owned_text(token->raw().size(), 2))) {
            return std::nullopt;
        }
        auto value = decode_model_binary(json_string_text(*token, decoded), resource);
        if (!value) {
            return std::nullopt;
        }
        input = remaining;
        return value;
    } else if constexpr (is_ruvia_json_value<field_type> || is_ruvia_json_object<field_type>) {
        return parse_json_view_value<field_type>(
            input, resource, depth, string_storage, is_ruvia_json_object<field_type>, budget);
    } else if constexpr (is_ruvia_array<field_type> || is_ruvia_boxed_array<field_type>) {
        auto parsed_value = parse_json_sequence_value<field_type>(remaining, resource, depth, string_storage, budget);
        if (!parsed_value.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed_value;
    } else if constexpr (is_ruvia_scalar<field_type>) {
        using scalar_type = model_scalar_value_type<field_type>;
        scalar_type parsed_value{};
        if constexpr (std::is_same_v<scalar_type, bool>) {
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
        return field_type(parsed_value);
    } else if constexpr (is_model<field_type>) {
        auto nested =
            model_parse_access::parse_value<field_type>(remaining, resource, depth, string_storage, budget);
        if (!nested.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return nested;
    } else {
        static_assert(always_false<field_type>, "RUVIA_MODEL JSON field type is not supported");
    }
}

// Exactly one JSON value (with surrounding whitespace) as `t_type`, within the
// representation budget of `options`. Field structure inside the value is not
// checked; see parse_json_document.
template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_complete_json_value(std::string_view input,
    std::pmr::memory_resource* resource, model_string_storage string_storage,
    model_parse_options options = {}) {
    json_parse_budget budget(options);
    if (!budget.consume_bytes(sizeof(t_type))) {
        return std::nullopt;
    }
    auto value = parse_json_value<t_type>(input, resource, 0, string_storage, budget);
    skip_json_whitespace(input);
    if (!value || !input.empty()) {
        return std::nullopt;
    }
    return value;
}

// One complete typed JSON value, including structural checks inside arrays.
// Borrowed reads are used by JSON views; standalone codecs request owned data.
template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_json_document(std::string_view input,
    std::pmr::memory_resource* resource, model_string_storage string_storage,
    model_parse_options options = {}) {
    auto value = parse_complete_json_value<t_type>(input, resource, string_storage, options);
    if (!value || !model_validation_access::value_structure_valid(*value)) {
        return std::nullopt;
    }
    return value;
}

// Whether `body` is exactly one JSON value (an object when require_object),
// with surrounding whitespace, within the limits of `options`. Dynamic tokens
// validate their whole body this way and then borrow it unchanged.
[[nodiscard]] inline bool is_complete_json_value(
    std::string_view body, model_parse_options options, bool require_object) noexcept {
    json_parse_budget budget(options);
    skip_json_whitespace(body);
    if (require_object && (body.empty() || body.front() != '{')) {
        return false;
    }
    if (!budget.skip_value(body, 0)) {
        return false;
    }
    skip_json_whitespace(body);
    return body.empty();
}

struct model_json_access final {
    template <typename model_type>
    [[nodiscard]] static std::size_t size_hint(const model_type& model);

    template <typename model_type>
    static void append(std::pmr::string& output, const model_type& model);
};

template <typename value_type>
consteval std::size_t json_fixed_scalar_size_hint() noexcept {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        return json_fixed_scalar_size_hint<decltype(std::declval<t_type>().value_)>();
    } else if constexpr (std::is_same_v<t_type, bool>) {
        return 5;
    } else if constexpr (std::is_integral_v<t_type>) {
        return static_cast<std::size_t>(std::numeric_limits<t_type>::digits10) + 3;
    } else if constexpr (std::is_floating_point_v<t_type>) {
        return 32;
    } else {
        return 0;
    }
}

template <typename value_type>
[[nodiscard]] std::size_t json_size_hint_value(const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (json_fixed_scalar_size_hint<t_type>() != 0) {
        return json_fixed_scalar_size_hint<t_type>();
    } else if constexpr (is_ruvia_bytes<t_type>) {
        return http_base64_encoded_size(value.size()) + 2;
    } else if constexpr (is_ruvia_string<t_type>) {
        return json_string_size_hint(value.view());
    } else if constexpr (is_model<t_type>) {
        return model_json_access::size_hint(value);
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return value.view().size();
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        constexpr std::size_t item_hint = [] {
            if constexpr (is_ruvia_array<t_type>) {
                return json_fixed_scalar_size_hint<typename t_type::value_type>();
            } else {
                return std::size_t{0};
            }
        }();
        if constexpr (item_hint != 0) {
            return value.empty() ? 2 : 2 + value.size() * item_hint + (value.size() - 1);
        } else {
            std::size_t size = 2;
            bool first = true;
            for (const auto& item : value) {
                if (!first) {
                    ++size;
                }
                first = false;
                size += json_size_hint_value(item);
            }
            return size;
        }
    } else {
        static_assert(
            always_false<t_type>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
    }
}

template <typename value_type>
void append_json_value(std::pmr::string& output, const value_type& value);

template <typename sequence_type>
void append_json_sequence(std::pmr::string& output, const sequence_type& value) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json_value(output, item);
    }
    output.push_back(']');
}

template <typename value_type>
void append_json_value(std::pmr::string& output, const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        append_json_value(output, value.value_);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (std::is_unsigned_v<t_type>) {
        http_append_unsigned(output, value);
    } else if constexpr (std::is_integral_v<t_type>) {
        http_append_signed(output, value);
    } else if constexpr (std::is_floating_point_v<t_type>) {
        // JSON (RFC 8259) has no representation for infinity or NaN, and
        // std::to_chars would emit the bare tokens "inf"/"nan" — invalid JSON.
        // Serialize non-finite values as null, matching JSON.stringify.
        if (!std::isfinite(value)) {
            output.append("null");
            return;
        }
        http_append_formatted_number(output, value, "JSON number cannot be formatted");
    } else if constexpr (is_ruvia_bytes<t_type>) {
        append_model_binary(output, value);
    } else if constexpr (is_ruvia_string<t_type>) {
        append_json_string(output, value.view());
    } else if constexpr (is_model<t_type>) {
        model_json_access::append(output, value);
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        output.append(value.view());
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        append_json_sequence(output, value);
    } else {
        static_assert(
            always_false<t_type>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
    }
}

namespace model {

enum class serialized_field_kind { value,
    null };

// Both serialization passes share emission and comma decisions, with no
// intermediate representation or runtime field lookup.
template <typename model_type, typename visitor_type>
void visit_serialized_fields(const model_type& value, visitor_type&& visitor) {
    bool first = true;
    visit_model_fields(value, model_access::schema<model_type>(),
        [&](const auto&, const auto& slot) {
            const auto& field_value = slot.value();
            serialized_field_kind kind;
            if (field_value && !(slot.omit_empty() && is_empty_value(*field_value))) {
                kind = serialized_field_kind::value;
            } else if (!field_value && (slot.is_null() || slot.emit_null())) {
                kind = serialized_field_kind::null;
            } else {
                return;
            }
            visitor(slot, kind, !first);
            first = false;
        });
}

}  // namespace model

template <typename model_type>
std::size_t model_json_access::size_hint(const model_type& value) {
    std::size_t size = 2;
    model::visit_serialized_fields(value, [&size](const auto& slot, auto kind, bool comma) {
        size += slot.json_member_key(comma).size();
        size += kind == model::serialized_field_kind::value
                    ? json_size_hint_value(*slot.value())
                    : 4;
    });
    return size;
}

template <typename model_type>
void model_json_access::append(std::pmr::string& output, const model_type& value) {
    output.push_back('{');
    model::visit_serialized_fields(value, [&output](const auto& slot, auto kind, bool comma) {
        output.append(slot.json_member_key(comma));
        if (kind == model::serialized_field_kind::value) {
            append_json_value(output, *slot.value());
        } else {
            output.append("null");
        }
    });
    output.push_back('}');
}

}  // namespace ruvia::detail
