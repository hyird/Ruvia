#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/json/json_skip.h"
#include "ruvia/web/detail/model/model_text_storage.h"
#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/detail/model/parse/json_writer.h"
#include "ruvia/web/model_types.h"

namespace ruvia {
class json_value;
class json_object;
}  // namespace ruvia

namespace ruvia::detail {

template <typename view_t_type>
[[nodiscard]] std::optional<view_t_type> parse_json_view_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    bool require_object, json_parse_budget& budget);

}

namespace ruvia {

// json_value and json_object borrow their complete input body by default. An
// owning string passed to parse() must therefore outlive the parsed view;
// basic_string rvalues are rejected before a dangling view can be created.
// Model fields may copy the JSON token when the parse owns its strings.
// Dynamic JSON token, not an ordinary field-validation schema. parse() borrows
// the complete token; from_json<Model>() owns it in the model resource instead.
// Move construction preserves borrowing/ownership. Move assignment keeps the
// target resource and owns the result (copying borrowed/incompatible storage).
// Views and potentially borrowed get<T>() results must not outlive their token.
class json_value final {
public:
    enum class kind_type : unsigned char { object,
        array,
        string,
        number,
        boolean,
        null };

    explicit json_value(model_options options = {})
        : json_value(detail::resolved_pmr_resource_tag{}, {},
              detail::pmr_resource_or_default(options.resource_)) {}

    [[nodiscard]] static std::optional<json_value> parse(
        std::string_view body, model_parse_options options = {}) noexcept {
        auto input = body;
        if (!detail::skip_json_value(input)) {
            return std::nullopt;
        }
        detail::skip_json_whitespace(input);
        if (!input.empty()) {
            return std::nullopt;
        }
        return json_value(
            detail::resolved_pmr_resource_tag{}, body, detail::pmr_resource_or_default(options.resource_));
    }

    template <typename traits_type, typename allocator_type>
    static std::optional<json_value> parse(
        std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    template <typename traits_type, typename allocator_type>
    static std::optional<json_value> parse(
        const std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    json_value(const json_value&) = delete;
    json_value& operator=(const json_value&) = delete;

    json_value(json_value&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    json_value& operator=(json_value&& other) {
        storage_.assign_from(std::move(other.storage_), resource_);
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] kind_type kind() const noexcept {
        auto input = view();
        detail::skip_json_whitespace(input);
        if (input.empty()) {
            return kind_type::null;
        }
        switch (input.front()) {
            case '{':
                return kind_type::object;
            case '[':
                return kind_type::array;
            case '"':
                return kind_type::string;
            case 't':
            case 'f':
                return kind_type::boolean;
            case 'n':
                return kind_type::null;
            default:
                return kind_type::number;
        }
    }

    [[nodiscard]] bool is_object() const noexcept {
        return kind() == kind_type::object;
    }

    [[nodiscard]] bool is_array() const noexcept {
        return kind() == kind_type::array;
    }

    [[nodiscard]] bool is_string() const noexcept {
        return kind() == kind_type::string;
    }

    [[nodiscard]] bool is_number() const noexcept {
        return kind() == kind_type::number;
    }

    [[nodiscard]] bool is_boolean() const noexcept {
        return kind() == kind_type::boolean;
    }

    [[nodiscard]] bool is_null() const noexcept {
        return kind() == kind_type::null;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get() const {
        return detail::parse_json_document<t_type>(view(), resource_, detail::model_string_storage::borrowed);
    }

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get(std::string_view field) const;

    // The callback receives a json_value borrowing this object's token; it is
    // valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_element(visitor_type&& visitor) const;

    // The callback receives a field name and json_value borrowing this object's
    // token; both are valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_field(visitor_type&& visitor) const;

    // Compare retained token bytes exactly, ignoring allocator and JSON
    // normalization, to preserve the zero-cost view semantics.
    [[nodiscard]] bool operator==(const json_value& other) const noexcept {
        return view() == other.view();
    }

private:
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    friend class json_object;
    template <typename view_t_type>
    friend std::optional<view_t_type> detail::parse_json_view_value(std::string_view&,
        std::pmr::memory_resource*, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    json_value(detail::resolved_pmr_resource_tag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : resource_(resource),
          storage_(body) {}

    json_value(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource,
        detail::model_text_storage&& storage) noexcept
        : resource_(resource),
          storage_(std::move(storage)) {}

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value, resource_);
    }

    [[nodiscard]] json_value rebind_for_model(std::pmr::memory_resource* resource) const& {
        return json_value(detail::resolved_pmr_resource_tag{}, resource, storage_.rebind(resource));
    }

    [[nodiscard]] json_value rebind_for_model(std::pmr::memory_resource* resource) && {
        return json_value(detail::resolved_pmr_resource_tag{}, resource, std::move(storage_).rebind(resource));
    }

    std::pmr::memory_resource* resource_;
    detail::model_text_storage storage_;
};

// Object-only dynamic token with the same borrowing, ownership and move
// contract as json_value. Supplying a resource to parse() does not copy input.
class json_object final {
public:
    explicit json_object(model_options options = {})
        : json_object(detail::resolved_pmr_resource_tag{}, {},
              detail::pmr_resource_or_default(options.resource_)) {}

    [[nodiscard]] static std::optional<json_object> parse(
        std::string_view body, model_parse_options options = {}) noexcept {
        detail::json_scanner scanner(body);
        if (!scanner.consume_object()) {
            return std::nullopt;
        }
        scanner.skip_whitespace();
        if (!scanner.empty()) {
            return std::nullopt;
        }
        return json_object(
            detail::resolved_pmr_resource_tag{}, body, detail::pmr_resource_or_default(options.resource_));
    }

    template <typename traits_type, typename allocator_type>
    static std::optional<json_object> parse(
        std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    template <typename traits_type, typename allocator_type>
    static std::optional<json_object> parse(
        const std::basic_string<char, traits_type, allocator_type>&&, model_parse_options = {}) = delete;

    json_object(const json_object&) = delete;
    json_object& operator=(const json_object&) = delete;

    json_object(json_object&& other) noexcept
        : resource_(other.resource_),
          storage_(std::move(other.storage_)) {}

    json_object& operator=(json_object&& other) {
        storage_.assign_from(std::move(other.storage_), resource_);
        return *this;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_.view();
    }
    [[nodiscard]] std::string_view view() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    // The callback receives a field name and json_value borrowing this object's
    // token; both are valid only for the duration of the callback.
    template <typename visitor_type>
    [[nodiscard]] bool for_each_field(visitor_type&& visitor) const;

    // Compare retained token bytes exactly, ignoring allocator and JSON
    // normalization, to preserve the zero-cost view semantics.
    [[nodiscard]] bool operator==(const json_object& other) const noexcept {
        return view() == other.view();
    }

    template <typename t_type>
        requires detail::is_model_field<t_type>
    [[nodiscard]] std::optional<t_type> get(std::string_view field) const {
        auto* const resource = resource_;
        std::optional<t_type> result;
        bool last_match_failed = false;
        const auto visited = detail::visit_json_object_fields(detail::resolved_pmr_resource_tag{}, view(),
            resource, [&](std::string_view key, std::string_view value_view) {
                if (key != field) {
                    return true;
                }

                detail::skip_json_whitespace(value_view);
                auto value = detail::parse_json_document<t_type>(value_view, resource,
                    detail::model_string_storage::borrowed);
                if (!value) {
                    result.reset();
                    last_match_failed = true;
                    return true;
                }
                result.emplace(std::move(*value));
                last_match_failed = false;
                return true;
            });

        if (visited != detail::json_object_visit_result::complete || last_match_failed) {
            return std::nullopt;
        }
        return result;
    }

private:
    friend class json_value;
    friend struct detail::model_value_factory;
    friend struct detail::model_value_rebind_access;
    template <typename view_t_type>
    friend std::optional<view_t_type> detail::parse_json_view_value(std::string_view&,
        std::pmr::memory_resource*, std::size_t, detail::model_string_storage, bool, detail::json_parse_budget&);

    json_object(detail::resolved_pmr_resource_tag, std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : resource_(resource),
          storage_(body) {}

    json_object(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource,
        detail::model_text_storage&& storage) noexcept
        : resource_(resource),
          storage_(std::move(storage)) {}

    void assign_owned(std::string_view value) {
        storage_.assign_owned(value, resource_);
    }

    [[nodiscard]] json_object rebind_for_model(std::pmr::memory_resource* resource) const& {
        return json_object(detail::resolved_pmr_resource_tag{}, resource, storage_.rebind(resource));
    }

    [[nodiscard]] json_object rebind_for_model(std::pmr::memory_resource* resource) && {
        return json_object(detail::resolved_pmr_resource_tag{}, resource, std::move(storage_).rebind(resource));
    }

    std::pmr::memory_resource* resource_;
    detail::model_text_storage storage_;
};

template <typename t_type>
    requires detail::is_model_field<t_type>
[[nodiscard]] inline std::optional<t_type> json_value::get(std::string_view field) const {
    if (!is_object()) {
        return std::nullopt;
    }
    return json_object(detail::resolved_pmr_resource_tag{}, view(), resource_).get<t_type>(field);
}

template <typename visitor_type>
[[nodiscard]] inline bool json_value::for_each_element(visitor_type&& visitor) const {
    if (!is_array()) {
        return false;
    }
    auto input = view();
    detail::skip_json_whitespace(input);
    if (!detail::consume_json_char(input, '[')) {
        return false;
    }
    detail::skip_json_whitespace(input);
    if (!input.empty() && input.front() == ']') {
        input.remove_prefix(1);
        detail::skip_json_whitespace(input);
        return input.empty();
    }
    while (true) {
        detail::skip_json_whitespace(input);
        const auto start = input;
        if (!detail::skip_json_value(input)) {
            return false;
        }
        json_value value(detail::resolved_pmr_resource_tag{},
            start.substr(0, start.size() - input.size()), resource_);
        if (!static_cast<bool>(visitor(value))) {
            return false;
        }
        detail::skip_json_whitespace(input);
        if (!input.empty() && input.front() == ']') {
            input.remove_prefix(1);
            detail::skip_json_whitespace(input);
            return input.empty();
        }
        if (!detail::consume_json_char(input, ',')) {
            return false;
        }
    }
}

template <typename visitor_type>
[[nodiscard]] inline bool json_value::for_each_field(visitor_type&& visitor) const {
    if (!is_object()) {
        return false;
    }

    return detail::visit_json_object_fields(detail::resolved_pmr_resource_tag{}, view(),
               resource_, [&](std::string_view key, std::string_view value_view) {
                   detail::skip_json_whitespace(value_view);
                   json_value value(detail::resolved_pmr_resource_tag{}, value_view, resource_);
                   return static_cast<bool>(visitor(key, value));
               }) == detail::json_object_visit_result::complete;
}

template <typename visitor_type>
[[nodiscard]] inline bool json_object::for_each_field(visitor_type&& visitor) const {
    json_value value(detail::resolved_pmr_resource_tag{}, view(), resource_);
    return value.for_each_field(std::forward<visitor_type>(visitor));
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename view_t_type>
[[nodiscard]] std::optional<view_t_type> parse_json_view_value(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    bool require_object, json_parse_budget& budget) {
    skip_json_whitespace(input);
    const auto start = input;
    if (!skip_json_value(input, depth)) {
        return std::nullopt;
    }
    const auto token = start.substr(0, start.size() - input.size());
    if (require_object) {
        auto probe_value = token;
        skip_json_whitespace(probe_value);
        if (probe_value.empty() || probe_value.front() != '{') {
            input = start;
            return std::nullopt;
        }
    }
    view_t_type value(model_options{.resource_ = resource});
    if (string_storage == model_string_storage::owned) {
        if (!budget.consume_bytes(token.size()) ||
            !budget.consume_bytes(token.size() + sizeof(std::pmr::string))) {
            return std::nullopt;
        }
        value.assign_owned(token);
    } else {
        value.storage_.assign_borrowed(token);
    }
    return value;
}

template <>
inline std::optional<json_value> parse_json_value<json_value>(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    return parse_json_view_value<json_value>(input, resource, depth, string_storage, false, budget);
}

template <>
inline std::optional<json_object> parse_json_value<json_object>(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, model_string_storage string_storage,
    json_parse_budget& budget) {
    return parse_json_view_value<json_object>(input, resource, depth, string_storage, true, budget);
}

template <>
inline std::size_t json_size_hint_value<json_value>(const json_value& value) {
    return value.view().size();
}

template <>
inline std::size_t json_size_hint_value<json_object>(const json_object& value) {
    return value.view().size();
}

template <>
inline void append_json_value<json_value>(std::pmr::string& output, const json_value& value) {
    output.append(value.view());
}

template <>
inline void append_json_value<json_object>(std::pmr::string& output, const json_object& value) {
    output.append(value.view());
}

}  // namespace ruvia::detail
