#pragma once

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/ModelObject.h"
#include "ruvia/web/detail/model/ModelSchema.h"
#include "ruvia/web/detail/model/parse/JsonParser.h"
#include "ruvia/web/detail/model/parse/JsonWriter.h"
#include "ruvia/web/detail/model/parse/ModelInputVisitors.h"
#include "ruvia/web/detail/model/rule/Rules.h"

namespace ruvia::detail::model {

template <typename DerivedT, typename... DescriptorTs>
class ModelStorage {
public:
    using RuviaSchema = ModelSchema<DescriptorTs...>;

    explicit ModelStorage(::ruvia::ModelOptions options = {}) noexcept
        : resource_(detail::pmrResourceOrDefault(options.resource)) {
        static_assert(uniqueModelFieldNames<DescriptorTs...>(), "Ruvia model source field names must be unique");
        static_assert(uniqueModelWireNames<DescriptorTs...>(), "Ruvia model JSON field names must be unique");
    }

    template <typename ResourceOwnerT>
        requires requires(ResourceOwnerT& owner) {
            { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        }
    explicit ModelStorage(ResourceOwnerT& owner) noexcept
        : ModelStorage(::ruvia::ModelOptions{.resource = owner.resource()}) {}

    template <FixedString Field>
    [[nodiscard]] decltype(auto) get() const& {
        constexpr auto index = modelFieldIndex<Field, DescriptorTs...>();
        using DescriptorT = std::tuple_element_t<index, std::tuple<DescriptorTs...>>;
        const auto& slot = std::get<index>(fields_);
        if constexpr (DescriptorT::required) {
            return slot.requiredValue();
        } else {
            return slot.value();
        }
    }

    template <FixedString Field>
    [[nodiscard]] decltype(auto) get() const&& = delete;

    template <FixedString Field, typename ValueT>
    DerivedT& set(ValueT&& value) & {
        constexpr auto index = modelFieldIndex<Field, DescriptorTs...>();
        std::get<index>(fields_).assign(std::forward<ValueT>(value), resource_);
        return static_cast<DerivedT&>(*this);
    }

    template <FixedString Field, typename ValueT>
    DerivedT& set(ValueT&&) && = delete;

    template <FixedString Field>
    [[nodiscard]] decltype(auto) ensure() & {
        constexpr auto index = modelFieldIndex<Field, DescriptorTs...>();
        return std::get<index>(fields_).ensure(resource_);
    }

    template <FixedString Field>
    [[nodiscard]] decltype(auto) ensure() && = delete;

    template <FixedString Field>
        requires(!std::tuple_element_t<
                 modelFieldIndex<Field, DescriptorTs...>(),
                 std::tuple<DescriptorTs...>>::required)
    void reset() & noexcept {
        constexpr auto index = modelFieldIndex<Field, DescriptorTs...>();
        std::get<index>(fields_).reset();
    }

    template <FixedString Field>
        requires(!std::tuple_element_t<
                 modelFieldIndex<Field, DescriptorTs...>(),
                 std::tuple<DescriptorTs...>>::required)
    void reset() && = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <std::size_t Index>
    [[nodiscard]] decltype(auto) ruviaSlot() & noexcept {
        return std::get<Index>(fields_);
    }

    template <std::size_t Index>
    [[nodiscard]] decltype(auto) ruviaSlot() const& noexcept {
        return std::get<Index>(fields_);
    }

protected:
    [[nodiscard]] static constexpr RuviaSchema ruviaSchema() noexcept {
        return {};
    }

    [[nodiscard]] DerivedT& derived() noexcept {
        return static_cast<DerivedT&>(*this);
    }

    [[nodiscard]] const DerivedT& derived() const noexcept {
        return static_cast<const DerivedT&>(*this);
    }

    std::pmr::memory_resource* resource_;

private:
    friend struct ::ruvia::detail::ModelValidationAccess;

    template <FixedString Field>
    [[nodiscard]] ModelFieldState ruviaFieldState() const {
        return modelFieldState<Field>(*this, ruviaSchema());
    }

    std::tuple<typename DescriptorTs::field_type...> fields_;
};

}  // namespace ruvia::detail::model

namespace ruvia {

template <typename DerivedT, typename... DescriptorTs>
class RequestModel : public detail::model::ModelStorage<DerivedT, DescriptorTs...> {
    using Base = detail::model::ModelStorage<DerivedT, DescriptorTs...>;

public:
    using RuviaModelBase = RequestModel;
    using RuviaRequestModelSchema = void;

    explicit RequestModel(::ruvia::ModelOptions options = {}) noexcept
        : Base(options) {
        validateFieldTypes();
    }

    template <typename ResourceOwnerT>
        requires requires(ResourceOwnerT& owner) {
            { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        }
    explicit RequestModel(ResourceOwnerT& owner) noexcept
        : RequestModel(::ruvia::ModelOptions{.resource = owner.resource()}) {}

private:
    friend struct detail::ModelJsonAccess;
    friend struct detail::ModelParseAccess;

    [[nodiscard]] static std::optional<DerivedT> ruviaParseJsonBody(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        auto model = ruviaParseJsonBodyPartial(body, resource);
        if (!model || !detail::ModelValidationAccess::structureValid(*model)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseJsonBodyPartial(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        return ruviaParseJsonBodyDepthPartial(
            body,
            resource,
            0,
            detail::ModelStringStorage::kBorrowed);
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseJsonBodyOwned(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        auto model = ruviaParseJsonBodyDepthPartial(
            body,
            resource,
            0,
            detail::ModelStringStorage::kOwned);
        if (!model || !detail::ModelValidationAccess::structureValid(*model)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseJsonBodyDepth(
        std::string_view body,
        std::pmr::memory_resource* resource,
        std::size_t depth,
        detail::ModelStringStorage stringStorage) {
        auto model = ruviaParseJsonBodyDepthPartial(body, resource, depth, stringStorage);
        if (!model || !detail::ModelValidationAccess::structureValid(*model)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseJsonBodyDepthPartial(
        std::string_view body,
        std::pmr::memory_resource* resource,
        std::size_t depth,
        detail::ModelStringStorage stringStorage) {
        auto input = body;
        auto model = ruviaParseJsonValue(input, resource, depth, stringStorage);
        detail::skipJsonWhitespace(input);
        if (!model || !input.empty()) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseJsonValue(
        std::string_view& input,
        std::pmr::memory_resource* resource,
        std::size_t depth,
        detail::ModelStringStorage stringStorage) {
        if (depth > detail::kMaxJsonDepth) {
            return std::nullopt;
        }
        DerivedT model{::ruvia::ModelOptions{.resource = resource}};
        if (!model.ruviaMaterializeJson(input, depth, stringStorage)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseFormBody(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        auto model = ruviaParseFormBodyPartial(body, resource);
        if (!model || !detail::ModelValidationAccess::structureValid(*model)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseFormBodyPartial(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        return ruviaMaterializeFormInput(
            detail::makeFormModelInput(body, resource));
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseFormBodyOwned(
        std::string_view body,
        std::pmr::memory_resource* resource) {
        auto model = ruviaMaterializeFormInput(
            detail::makeFormModelInput(
                body,
                resource,
                detail::ModelStringStorage::kOwned));
        if (!model || !detail::ModelValidationAccess::structureValid(*model)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseFormFields(
        const RequestNameValueList& fields,
        std::pmr::memory_resource* resource) {
        auto model = ruviaParseFormFieldsPartial(fields, resource);
        if (!model || !detail::ModelValidationAccess::structureValid(*model)) {
            return std::nullopt;
        }
        return model;
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaParseFormFieldsPartial(
        const RequestNameValueList& fields,
        std::pmr::memory_resource* resource) {
        return ruviaMaterializeFormInput(
            detail::makeFormFieldsModelInput(fields, resource));
    }

    [[nodiscard]] static std::optional<DerivedT> ruviaMaterializeFormInput(
        const detail::ModelInput& input) {
        DerivedT model{::ruvia::ModelOptions{.resource = input.resource()}};
        if (!model.ruviaMaterializeForm(input)) {
            return std::nullopt;
        }
        return model;
    }

    bool ruviaMaterializeJson(
        std::string_view& input,
        std::size_t depth,
        detail::ModelStringStorage stringStorage) {
        auto* const resource = this->resource_;
        const bool valid = detail::consumeJsonObjectFields(
            detail::ResolvedPmrResourceTag{},
            input,
            resource,
            depth,
            [this, resource, depth, stringStorage](
                std::string_view key,
                std::string_view& valueInput) -> bool {
                const auto keyHash = detail::model::modelFieldNameHash(key);
                bool fieldResult = true;
                const bool matched = detail::model::visitModelFieldByWireName(
                    this->derived(),
                    Base::ruviaSchema(),
                    keyHash,
                    key,
                    fieldResult,
                    [&](auto& slot) -> bool {
                        if (slot.state() != detail::ModelFieldState::kMissing) {
                            slot.markDuplicate();
                            return detail::skipJsonValue(valueInput, depth + 1);
                        }
                        const auto originalInput = valueInput;
                        using ValueT = typename std::remove_cvref_t<decltype(slot)>::value_type;
                        if (auto value = detail::parseJsonValue<ValueT>(
                                valueInput,
                                resource,
                                depth + 1,
                                stringStorage);
                            value) {
                            slot.emplaceParsed(std::move(*value));
                            return true;
                        }
                        valueInput = originalInput;
                        if (!detail::skipJsonValue(valueInput, depth + 1)) {
                            return false;
                        }
                        slot.markInvalidType();
                        return true;
                    });
                if (matched) {
                    return fieldResult;
                }
                return detail::skipJsonValue(valueInput, depth + 1);
            });
        if (valid) {
            detail::model::visitModelFields(
                this->derived(),
                Base::ruviaSchema(),
                [resource](const auto&, auto& slot) {
                    slot.applyDefault(resource);
                });
        }
        return valid;
    }

    bool ruviaMaterializeForm(const detail::ModelInput& input) {
        auto* const resource = this->resource_;
        const bool valid = detail::visitModelInputFormFields(
            input,
            [this, &input, resource](std::string_view key, std::string_view value) {
                bool matched = false;
                detail::model::visitModelFields(
                    this->derived(),
                    Base::ruviaSchema(),
                    [&](const auto&, auto& slot) {
                        using SlotT = std::remove_cvref_t<decltype(slot)>;
                        if constexpr (detail::isFormField<typename SlotT::value_type>) {
                            if (matched || key != slot.wireName()) {
                                return;
                            }
                            matched = true;
                            if (slot.state() != detail::ModelFieldState::kMissing) {
                                slot.markDuplicate();
                                return;
                            }
                            const auto encoding = input.kind() == detail::ModelInputKind::kFormFields
                                ? detail::FormValueEncoding::kDecoded
                                : detail::FormValueEncoding::kUrlEncoded;
                            using ValueT = typename SlotT::value_type;
                            auto parsed = detail::parseFormValue<ValueT>(
                                detail::ResolvedPmrResourceTag{},
                                value,
                                encoding,
                                resource,
                                input.stringStorage());
                            if (parsed) {
                                slot.emplaceParsed(std::move(*parsed));
                            } else {
                                slot.markInvalidType();
                            }
                        }
                    });
            });
        if (valid) {
            detail::model::visitModelFields(
                this->derived(),
                Base::ruviaSchema(),
                [resource](const auto&, auto& slot) {
                    slot.applyDefault(resource);
                });
        }
        return valid;
    }

    static consteval void validateFieldTypes() {
        static_assert(
            (detail::isRequestModelField<typename DescriptorTs::value_type> && ...),
            "request model fields must use Ruvia values or nested request models");
    }
};

template <typename DerivedT, typename... DescriptorTs>
class ResponseModel : public detail::model::ModelStorage<DerivedT, DescriptorTs...> {
    using Base = detail::model::ModelStorage<DerivedT, DescriptorTs...>;

public:
    using RuviaModelBase = ResponseModel;
    using RuviaResponseModelSchema = void;

    explicit ResponseModel(::ruvia::ModelOptions options = {}) noexcept
        : Base(options) {
        validateFieldTypes();
    }

    template <typename ResourceOwnerT>
        requires requires(ResourceOwnerT& owner) {
            { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        }
    explicit ResponseModel(ResourceOwnerT& owner) noexcept
        : ResponseModel(::ruvia::ModelOptions{.resource = owner.resource()}) {}

private:
    friend struct detail::ModelJsonAccess;

    void ruviaAppendJson(std::pmr::string& output) const {
        output.push_back('{');
        bool first = true;
        detail::model::visitModelFields(
            this->derived(),
            Base::ruviaSchema(),
            [&output, &first](const auto&, const auto& slot) {
                const auto& value = slot.value();
                if (value && !(slot.omitEmpty() && detail::model::isEmptyValue(*value))) {
                    if (!first) {
                        output.push_back(',');
                    }
                    first = false;
                    detail::appendJsonString(output, slot.wireName());
                    output.push_back(':');
                    detail::appendJsonValue(output, *value);
                } else if (!value && slot.emitNull()) {
                    if (!first) {
                        output.push_back(',');
                    }
                    first = false;
                    detail::appendJsonString(output, slot.wireName());
                    output.append(":null");
                }
            });
        output.push_back('}');
    }

    [[nodiscard]] std::size_t ruviaJsonSizeHint() const {
        std::size_t size = 2;
        bool first = true;
        detail::model::visitModelFields(
            this->derived(),
            Base::ruviaSchema(),
            [&size, &first](const auto&, const auto& slot) {
                const auto& value = slot.value();
                if (value && !(slot.omitEmpty() && detail::model::isEmptyValue(*value))) {
                    if (!first) {
                        ++size;
                    }
                    first = false;
                    size += detail::jsonStringSizeHint(slot.wireName()) + 1;
                    size += detail::jsonSizeHintValue(*value);
                } else if (!value && slot.emitNull()) {
                    if (!first) {
                        ++size;
                    }
                    first = false;
                    size += detail::jsonStringSizeHint(slot.wireName()) + 5;
                }
            });
        return size;
    }

    static consteval void validateFieldTypes() {
        static_assert(
            (detail::isResponseModelField<typename DescriptorTs::value_type> && ...),
            "response model fields must use Ruvia values or nested response models");
    }
};

}  // namespace ruvia
