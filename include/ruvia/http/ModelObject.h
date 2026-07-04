#pragma once

#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/ModelJson.h"
#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/detail/model/Parser.h"
#include "ruvia/http/detail/model/RequestFieldVisitors.h"
#include "ruvia/http/detail/model/Traits.h"
#include "ruvia/memory/PmrResource.h"

#include <memory_resource>
#include <optional>
#include <string_view>

namespace ruvia {

class JsonValue final {
public:
    enum class Kind : unsigned char {
        kObject,
        kArray,
        kString,
        kNumber,
        kBoolean,
        kNull
    };

    [[nodiscard]] static std::optional<JsonValue> parse(
        std::string_view body,
        std::pmr::memory_resource* resource = nullptr) noexcept {
        auto input = body;
        if (!detail::skipJsonValue(input)) {
            return std::nullopt;
        }
        detail::skipJsonWhitespace(input);
        if (!input.empty()) {
            return std::nullopt;
        }
        return JsonValue(detail::ResolvedPmrResourceTag{}, body, detail::pmrResourceOrDefault(resource));
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    [[nodiscard]] Kind kind() const noexcept {
        auto input = body_;
        detail::skipJsonWhitespace(input);
        if (input.empty()) {
            return Kind::kNull;
        }
        switch (input.front()) {
            case '{':
                return Kind::kObject;
            case '[':
                return Kind::kArray;
            case '"':
                return Kind::kString;
            case 't':
            case 'f':
                return Kind::kBoolean;
            case 'n':
                return Kind::kNull;
            default:
                return Kind::kNumber;
        }
    }

    [[nodiscard]] bool isObject() const noexcept {
        return kind() == Kind::kObject;
    }

    [[nodiscard]] bool isArray() const noexcept {
        return kind() == Kind::kArray;
    }

    [[nodiscard]] bool isString() const noexcept {
        return kind() == Kind::kString;
    }

    [[nodiscard]] bool isNumber() const noexcept {
        return kind() == Kind::kNumber;
    }

    [[nodiscard]] bool isBoolean() const noexcept {
        return kind() == Kind::kBoolean;
    }

    [[nodiscard]] bool isNull() const noexcept {
        return kind() == Kind::kNull;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const;

private:
    JsonValue() noexcept = default;

    JsonValue(
        detail::ResolvedPmrResourceTag,
        std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : body_(body), resource_(resource) {}

    std::string_view body_;
    std::pmr::memory_resource* resource_{nullptr};
};

class JsonObject final {
public:
    [[nodiscard]] static std::optional<JsonObject> parse(
        std::string_view body,
        std::pmr::memory_resource* resource = nullptr) noexcept {
        detail::JsonScanner scanner(body);
        if (!scanner.consumeObject()) {
            return std::nullopt;
        }
        scanner.skipWhitespace();
        if (!scanner.empty()) {
            return std::nullopt;
        }
        return JsonObject(detail::ResolvedPmrResourceTag{}, body, detail::pmrResourceOrDefault(resource));
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        auto* const resource = resource_;
        std::optional<T> result;
        bool lastMatchFailed = false;
        const bool valid = detail::visitJsonObjectFields(
            detail::ResolvedPmrResourceTag{},
            body_,
            resource,
            [&](std::string_view key, std::string_view valueView) {
                if (key != field) {
                    return true;
                }

                auto valueInput = valueView;
                auto value = detail::parseJsonValue<T>(valueInput, resource);
                detail::skipJsonWhitespace(valueInput);
                if (!value || !valueInput.empty()) {
                    result.reset();
                    lastMatchFailed = true;
                    return true;
                }
                result.emplace(std::move(*value));
                lastMatchFailed = false;
                return true;
            });

        if (!valid || lastMatchFailed) {
            return std::nullopt;
        }
        return result;
    }

private:
    friend class JsonValue;
    friend class RequestObject;

    JsonObject() noexcept = default;

    JsonObject(
        detail::ResolvedPmrResourceTag,
        std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : body_(body), resource_(resource) {}

    std::string_view body_;
    std::pmr::memory_resource* resource_{nullptr};
};

template <typename T>
[[nodiscard]] inline std::optional<T> JsonValue::get(std::string_view field) const {
    if (!isObject()) {
        return std::nullopt;
    }
    return JsonObject(detail::ResolvedPmrResourceTag{}, body_, resource_).get<T>(field);
}

class FormObject final {
public:
    [[nodiscard]] static std::optional<FormObject> parse(
        std::string_view body,
        std::pmr::memory_resource* resource = nullptr) noexcept {
        if (!detail::validateFormEncoding(body)) {
            return std::nullopt;
        }
        return FormObject(detail::ResolvedPmrResourceTag{}, body, detail::pmrResourceOrDefault(resource));
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        auto* const resource = resource_;
        std::optional<T> result;
        bool parseFailed = false;
        const bool valid = detail::visitRawFormFields(body_, [&](std::string_view name, std::string_view valueView) {
            if (!detail::formFieldNameEquals(name, field)) {
                return true;
            }

            T value = detail::makeRequestValue<T>(detail::ResolvedPmrResourceTag{}, resource);
            if (!detail::parseFormValue(
                    detail::ResolvedPmrResourceTag{},
                    valueView,
                    value,
                    resource)) {
                parseFailed = true;
                return false;
            }
            result.emplace(std::move(value));
            return true;
        });

        if (!valid || parseFailed) {
            return std::nullopt;
        }
        return result;
    }

private:
    friend class RequestObject;

    FormObject() noexcept = default;

    FormObject(
        detail::ResolvedPmrResourceTag,
        std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : body_(body), resource_(resource) {}

    std::string_view body_;
    std::pmr::memory_resource* resource_{nullptr};
};

namespace detail {

struct RequestObjectAccess;

template <typename T>
[[nodiscard]] std::optional<T> getDecodedFormField(
    const RequestNameValueList& fields,
    std::string_view field,
    std::pmr::memory_resource* resource) {
    std::optional<T> result;
    bool parseFailed = false;
    for (const auto& item : fields) {
        if (item.name() != field) {
            continue;
        }

        T value = makeRequestValue<T>(ResolvedPmrResourceTag{}, resource);
        if (!parseDecodedFormValue(
                ResolvedPmrResourceTag{},
                item.value(),
                value,
                resource)) {
            parseFailed = true;
            break;
        }
        result.emplace(std::move(value));
        break;
    }

    if (parseFailed) {
        return std::nullopt;
    }
    return result;
}

}  // namespace detail

enum class RequestObjectKind {
    kJson,
    kForm,
    kFormFields
};

class RequestObject final {
public:
    RequestObject(const RequestObject& other)
        : kind_(other.kind_),
          body_(other.body_),
          fields_(other.fields_),
          resource_(other.resource_) {}

    RequestObject& operator=(const RequestObject& other) {
        if (this == &other) {
            return *this;
        }
        kind_ = other.kind_;
        body_ = other.body_;
        fields_ = other.fields_;
        resource_ = other.resource_;
        return *this;
    }

    RequestObject(RequestObject&&) noexcept = default;
    RequestObject& operator=(RequestObject&&) noexcept = default;

    [[nodiscard]] RequestObjectKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    [[nodiscard]] const RequestNameValueList* fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return detail::pmrResourceOrDefault(resource_);
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        auto* const resource = this->resource();
        if (kind_ == RequestObjectKind::kJson) {
            return JsonObject(detail::ResolvedPmrResourceTag{}, body_, resource).get<T>(field);
        }
        if constexpr (detail::isFormField<T>) {
            if (kind_ == RequestObjectKind::kFormFields) {
                if (fields_ == nullptr) {
                    return std::nullopt;
                }
                return detail::getDecodedFormField<T>(*fields_, field, resource);
            }
            return FormObject(detail::ResolvedPmrResourceTag{}, body_, resource).get<T>(field);
        } else {
            (void)field;
            return std::nullopt;
        }
    }

private:
    friend struct detail::RequestObjectAccess;

    RequestObject() noexcept = default;

    RequestObject(
        RequestObjectKind kind,
        std::string_view body,
        std::pmr::memory_resource* resource = nullptr) noexcept
        : RequestObject(
              detail::ResolvedPmrResourceTag{},
              kind,
              body,
              detail::pmrResourceOrDefault(resource)) {}

    RequestObject(
        const RequestNameValueList& fields,
        std::pmr::memory_resource* resource = nullptr) noexcept
        : kind_(RequestObjectKind::kFormFields),
          fields_(&fields),
          resource_(detail::pmrResourceOrDefault(resource)) {}

    RequestObject(
        detail::ResolvedPmrResourceTag,
        RequestObjectKind kind,
        std::string_view body,
        std::pmr::memory_resource* resource) noexcept
        : kind_(kind),
          body_(body),
          resource_(resource) {}

    RequestObjectKind kind_{RequestObjectKind::kJson};
    std::string_view body_;
    const RequestNameValueList* fields_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

namespace detail {

struct RequestObjectAccess final {
    [[nodiscard]] static RequestObject makeJson(
        std::string_view body,
        std::pmr::memory_resource* resource) noexcept {
        return RequestObject(RequestObjectKind::kJson, body, resource);
    }

    [[nodiscard]] static RequestObject makeForm(
        std::string_view body,
        std::pmr::memory_resource* resource) noexcept {
        return RequestObject(RequestObjectKind::kForm, body, resource);
    }

    [[nodiscard]] static RequestObject makeFormFields(
        const RequestNameValueList& fields,
        std::pmr::memory_resource* resource) noexcept {
        return RequestObject(fields, resource);
    }
};

}  // namespace detail

}  // namespace ruvia
