#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/http/request/RequestFieldsAccess.h"
#include "ruvia/web/detail/json/JsonNumber.h"
#include "ruvia/web/detail/json/JsonScanner.h"
#include "ruvia/web/detail/json/JsonString.h"
#include "ruvia/web/detail/model/ModelBinary.h"
#include "ruvia/web/detail/model/ModelInput.h"
#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/model/parse/FormParser.h"
#include "ruvia/web/detail/model/parse/JsonWriter.h"
#include "ruvia/web/detail/model/parse/ModelInputVisitors.h"
#include "ruvia/web/detail/model/rule/Rules.h"

// Internal JSON value parser for RUVIA_MODEL.

namespace ruvia::detail {

// A parse-local, conservative representation budget. Charging cumulative growth
// (rather than only live sizes) also bounds storage retained by monotonic arenas.
class json_parse_budget final {
public:
    explicit json_parse_budget(ModelParseOptions options = {}) noexcept
        : elements_(options.max_array_elements),
          bytes_(options.max_representation_bytes) {}

    [[nodiscard]] bool consume_bytes(std::size_t bytes) noexcept {
        if (bytes > bytes_) {
            exhausted_ = true;
            return false;
        }
        bytes_ -= bytes;
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

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth,
    ModelStringStorage stringStorage, json_parse_budget& budget);

struct ModelParseAccess final {
    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseValue(std::string_view& input,
        std::pmr::memory_resource* resource, std::size_t depth, ModelStringStorage stringStorage,
        json_parse_budget& budget) {
        if (depth > kMaxJsonDepth) {
            return std::nullopt;
        }
        auto value = model::model_access::empty<ModelT>(resource);
        if (!materialize_json(value, input, depth, stringStorage, budget)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseJsonBorrowed(
        std::string_view body, std::pmr::memory_resource* resource) {
        auto value = parseJsonBorrowedPartial<ModelT>(body, resource);
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseJsonBorrowedPartial(
        std::string_view body, std::pmr::memory_resource* resource) {
        json_parse_budget budget;
        if (!budget.consume_bytes(sizeof(ModelT))) {
            return std::nullopt;
        }
        auto input = body;
        auto value = parseValue<ModelT>(input, resource, 0, ModelStringStorage::kBorrowed, budget);
        skipJsonWhitespace(input);
        if (!value || !input.empty()) {
            return std::nullopt;
        }
        return value;
    }

    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseFormOwned(
        std::string_view body, std::pmr::memory_resource* resource) {
        auto value = materialize_form_input<ModelT>(
            makeFormModelInput(body, resource, ModelStringStorage::kOwned));
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseFormBorrowed(
        std::string_view body, std::pmr::memory_resource* resource) {
        auto value = parseFormBorrowedPartial<ModelT>(body, resource);
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseFormBorrowedPartial(
        std::string_view body, std::pmr::memory_resource* resource) {
        return materialize_form_input<ModelT>(makeFormModelInput(body, resource));
    }

    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseFormFields(
        const RequestNameValueList& fields, std::pmr::memory_resource* resource) {
        auto value = parseFormFieldsPartial<ModelT>(fields, resource);
        if (!has_valid_structure(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseFormFieldsPartial(
        const RequestNameValueList& fields, std::pmr::memory_resource* resource) {
        return materialize_form_input<ModelT>(makeFormFieldsModelInput(fields, resource));
    }

private:
    template <typename model_type>
    [[nodiscard]] static bool has_valid_structure(const std::optional<model_type>& value) {
        return value && ModelValidationAccess::structureValid(*value);
    }

    template <typename model_type>
    static void apply_defaults(model_type& value) {
        model::visitModelFields(value, model::model_access::schema<model_type>(),
            [resource = value.resource()](const auto&, auto& slot) { slot.applyDefault(resource); });
    }

    template <typename model_type>
    [[nodiscard]] static std::optional<model_type> materialize_form_input(const ModelInput& input) {
        auto value = model::model_access::empty<model_type>(input.resource());
        if (!materialize_form(value, input)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename model_type>
    static bool materialize_json(model_type& value, std::string_view& input, std::size_t depth,
        ModelStringStorage string_storage, json_parse_budget& budget) {
        auto* const resource = value.resource();
        const bool valid = consumeJsonObjectFields(ResolvedPmrResourceTag{}, input, resource, depth,
            [&value, resource, depth, string_storage, &budget](
                std::string_view key, std::string_view& value_input) -> bool {
                const auto key_hash = model::modelFieldNameHash(key);
                bool field_result = true;
                const bool matched = model::visitModelFieldByWireName(value,
                    model::model_access::schema<model_type>(), key_hash, key, field_result,
                    [&](auto& slot) -> bool {
                        if (slot.isPresent()) {
                            slot.markDuplicate();
                            return skipJsonValue(value_input, depth + 1);
                        }
                        const auto original_input = value_input;
                        using slot_type = std::remove_cvref_t<decltype(slot)>;
                        using value_type = typename slot_type::value_type;
                        auto null_input = value_input;
                        if (consumeJsonLiteral(null_input, "null")) {
                            value_input = null_input;
                            if constexpr (slot_type::nullable) {
                                slot.markNull();
                            } else {
                                slot.markInvalidType();
                            }
                            return true;
                        }
                        if (auto parsed = parseJsonValue<value_type>(
                                value_input, resource, depth + 1, string_storage, budget);
                            parsed) {
                            ModelValueFactory::emplaceParsed(slot, std::move(*parsed));
                            return true;
                        }
                        if (budget.exhausted()) {
                            return false;
                        }
                        // Wrong-type values need a structural skip only when the
                        // value parser did not consume a complete JSON token.
                        if (value_input.data() == original_input.data() &&
                            !skipJsonValue(value_input, depth + 1)) {
                            return false;
                        }
                        slot.markInvalidType();
                        return true;
                    });
                return matched ? field_result : skipJsonValue(value_input, depth + 1);
            });
        if (valid) {
            apply_defaults(value);
        }
        return valid;
    }

    template <typename model_type>
    static bool materialize_form(model_type& value, const ModelInput& input) {
        auto* const resource = value.resource();
        const auto encoding = input.kind() == ModelInputKind::kFormFields
                                  ? FormValueEncoding::kDecoded
                                  : FormValueEncoding::kUrlEncoded;
        const auto* fields = input.fields();
        const bool case_insensitive =
            fields != nullptr && RequestNameValueListAccess::caseInsensitive(*fields);
        const auto string_storage = input.stringStorage();
        const bool valid = visitModelInputFormFields(input,
            [&value, resource, encoding, fields, case_insensitive, string_storage](
                std::string_view key, std::string_view field_value) {
                auto bind = [&](auto& slot) {
                    using slot_type = std::remove_cvref_t<decltype(slot)>;
                    if constexpr (isFormField<typename slot_type::value_type>) {
                        if (slot.isPresent()) {
                            slot.markDuplicate();
                            return;
                        }
                        using value_type = typename slot_type::value_type;
                        auto parsed = parseFormValue<value_type>(ResolvedPmrResourceTag{},
                            field_value, encoding, resource, string_storage);
                        if (parsed) {
                            ModelValueFactory::emplaceParsed(slot, std::move(*parsed));
                        } else {
                            slot.markInvalidType();
                        }
                    }
                };
                if (case_insensitive) {
                    bool matched = false;
                    model::visitModelFields(value, model::model_access::schema<model_type>(),
                        [&](const auto&, auto& slot) {
                            using slot_type = std::remove_cvref_t<decltype(slot)>;
                            if constexpr (isFormField<typename slot_type::value_type>) {
                                if (matched || !RequestNameValueListAccess::namesEqual(
                                                   *fields, key, slot.wireName())) {
                                    return;
                                }
                                matched = true;
                                bind(slot);
                            }
                        });
                    return true;
                }
                bool field_result = true;
                (void)model::visitModelFieldByWireName(value, model::model_access::schema<model_type>(),
                    model::modelFieldNameHash(key), key, field_result, [&](auto& slot) -> bool {
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

template <typename SequenceT>
struct JsonSequenceValueTraits;

template <typename ValueT>
struct JsonSequenceValueTraits<Array<ValueT>> {
    using value_type = ValueT;

    static void emplace(Array<ValueT>& value, ValueT&& element) {
        ModelValueFactory::emplaceParsed(value, std::move(element));
    }
};

template <typename ValueT>
struct JsonSequenceValueTraits<BoxedArray<ValueT>> {
    using value_type = ValueT;

    static void emplace(BoxedArray<ValueT>& value, ValueT&& element) {
        ModelValueFactory::emplaceParsed(value, std::move(element));
    }
};

template <typename SequenceT>
[[nodiscard]] std::optional<SequenceT> parseJsonSequenceValue(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, ModelStringStorage stringStorage,
    json_parse_budget& budget) {
    using Traits = JsonSequenceValueTraits<std::remove_cvref_t<SequenceT>>;
    using ElementT = typename Traits::value_type;

    if (depth > kMaxJsonDepth) {
        return std::nullopt;
    }
    auto remaining = input;
    if (!consumeJsonChar(remaining, '[')) {
        return std::nullopt;
    }

    SequenceT value = makeRequestValue<SequenceT>(resource);
    skipJsonWhitespace(remaining);
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
        constexpr auto element_bytes = 4 * (sizeof(ElementT) + sizeof(ElementT*));
        if (!budget.consume_bytes(element_bytes)) {
            return std::nullopt;
        }
        auto element = parseJsonValue<ElementT>(remaining, resource, depth + 1, stringStorage, budget);
        if (!element.has_value()) {
            return std::nullopt;
        }
        Traits::emplace(value, std::move(*element));

        skipJsonWhitespace(remaining);
        if (!remaining.empty() && remaining.front() == ']') {
            remaining.remove_prefix(1);
            input = remaining;
            return value;
        }
        if (!consumeJsonChar(remaining, ',')) {
            return std::nullopt;
        }
    }
}

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, ModelStringStorage stringStorage,
    json_parse_budget& budget) {
    using FieldT = std::remove_cvref_t<T>;
    if (depth > kMaxJsonDepth || budget.exhausted()) {
        return std::nullopt;
    }
    auto remaining = input;
    if constexpr (isRuviaString<FieldT>) {
        const auto parsed = parseJsonString(remaining);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        if ((stringStorage == ModelStringStorage::kOwned ||
                parsed->encoding() != JsonStringEncoding::kLiteral) &&
            (!budget.consume_bytes(parsed->raw().size()) ||
                !budget.consume_bytes(parsed->raw().size() + sizeof(std::pmr::string)))) {
            return std::nullopt;
        }
        if (parsed->encoding() == JsonStringEncoding::kLiteral) {
            input = remaining;
            if (stringStorage == ModelStringStorage::kOwned) {
                return FieldT(parsed->raw(), ::ruvia::ModelOptions{.resource = resource});
            }
            return ModelValueFactory::makeString(parsed->raw(), resource);
        }
        auto decoded = decodeJsonString(parsed->raw(), resource);
        input = remaining;
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        FieldT value = makeRequestValue<FieldT>(resource);
        value.assignOwned(std::move(*decoded));
        return value;
    } else if constexpr (isRuviaBytes<FieldT>) {
        // Escaped base64 may allocate both a decoded token and a byte value.
        const auto token = parseJsonString(remaining);
        if (!token || !budget.consume_bytes(token->raw().size()) ||
            !budget.consume_bytes(token->raw().size() + sizeof(std::pmr::string))) {
            return std::nullopt;
        }
        std::optional<Bytes> value;
        if (token->encoding() == JsonStringEncoding::kLiteral) {
            value = decodeModelBinary(token->raw(), resource);
        } else {
            const auto decoded = decodeJsonString(token->raw(), resource);
            if (!decoded) {
                return std::nullopt;
            }
            value = decodeModelBinary(*decoded, resource);
        }
        if (!value) {
            return std::nullopt;
        }
        input = remaining;
        return value;
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        const auto parsed = parseJsonString(remaining);
        if (!parsed.has_value() || parsed->encoding() != JsonStringEncoding::kLiteral) {
            return std::nullopt;
        }
        input = remaining;
        return parsed->raw();
    } else if constexpr (isRuviaArray<FieldT> || isRuviaBoxedArray<FieldT>) {
        auto parsed = parseJsonSequenceValue<FieldT>(remaining, resource, depth, stringStorage, budget);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed;
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = ModelScalarValueT<FieldT>;
        ScalarT parsed{};
        if constexpr (std::is_same_v<ScalarT, bool>) {
            if (consumeJsonLiteral(remaining, "true")) {
                parsed = true;
            } else if (consumeJsonLiteral(remaining, "false")) {
                parsed = false;
            } else {
                return std::nullopt;
            }
        } else {
            if (!parseJsonNumberValue(remaining, parsed)) {
                return std::nullopt;
            }
        }
        input = remaining;
        return FieldT(parsed);
    } else if constexpr (isModel<FieldT>) {
        auto nested =
            ModelParseAccess::parseValue<FieldT>(remaining, resource, depth, stringStorage, budget);
        if (!nested.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return nested;
    } else {
        static_assert(alwaysFalse<FieldT>, "RUVIA_MODEL JSON field type is not supported");
    }
}

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth = 0,
    ModelStringStorage stringStorage = ModelStringStorage::kBorrowed) {
    json_parse_budget budget;
    if (!budget.consume_bytes(sizeof(T))) {
        return std::nullopt;
    }
    return parseJsonValue<T>(input, resource, depth, stringStorage, budget);
}

// One complete typed JSON value, including structural checks inside arrays.
// Borrowed reads are used by JSON views; standalone codecs request owned data.
template <typename T>
[[nodiscard]] std::optional<T> parseJsonDocument(std::string_view input,
    std::pmr::memory_resource* resource, ModelStringStorage stringStorage,
    ModelParseOptions options = {}) {
    json_parse_budget budget(options);
    if (!budget.consume_bytes(sizeof(T))) {
        return std::nullopt;
    }
    auto value = parseJsonValue<T>(input, resource, 0, stringStorage, budget);
    skipJsonWhitespace(input);
    if (!value || !input.empty() || !ModelValidationAccess::valueStructureValid(*value)) {
        return std::nullopt;
    }
    return value;
}

}  // namespace ruvia::detail
