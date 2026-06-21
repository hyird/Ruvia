#pragma once

#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/detail/model/Parser.h"
#include "ruvia/http/detail/model/RequestFieldVisitors.h"
#include "ruvia/http/detail/model/Traits.h"

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

class JsonObject final {
public:
    JsonObject() noexcept = default;

    explicit JsonObject(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : body_(body), resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] static std::optional<JsonObject> parse(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept {
        detail::JsonScanner scanner(body);
        if (!scanner.consumeObject()) {
            return std::nullopt;
        }
        scanner.skipWhitespace();
        if (!scanner.empty()) {
            return std::nullopt;
        }
        return JsonObject(body, resource);
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        std::optional<T> result;
        bool parseFailed = false;
        const bool valid = detail::visitJsonObjectFields(body_, resource_, [&](std::string_view key, std::string_view valueView) {
            if (key != field) {
                return true;
            }

            auto valueInput = valueView;
            auto value = detail::parseJsonValue<T>(valueInput, resource_);
            detail::skipJsonWhitespace(valueInput);
            if (!value || !valueInput.empty()) {
                parseFailed = true;
                return false;
            }
            result.emplace(std::move(*value));
            return false;
        });

        if (!valid || parseFailed) {
            return std::nullopt;
        }
        return result;
    }

private:
    std::string_view body_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
};

class FormObject final {
public:
    FormObject() noexcept = default;

    explicit FormObject(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : body_(body), resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] static std::optional<FormObject> parse(
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept {
        if (!detail::validateFormEncoding(body)) {
            return std::nullopt;
        }
        return FormObject(body, resource);
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        std::optional<T> result;
        bool parseFailed = false;
        detail::visitRawFormFields(body_, [&](std::string_view name, std::string_view valueView) {
            if (!detail::formFieldNameEquals(name, field)) {
                return true;
            }

            T value = detail::makeRequestValue<T>(resource_);
            if (!detail::parseFormValue(valueView, value, resource_)) {
                parseFailed = true;
                return false;
            }
            result.emplace(std::move(value));
            return false;
        });

        if (parseFailed) {
            return std::nullopt;
        }
        return result;
    }

private:
    std::string_view body_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
};

enum class RequestObjectKind {
    kJson,
    kForm
};

class RequestObject final {
public:
    RequestObject() noexcept = default;

    RequestObject(
        RequestObjectKind kind,
        std::string_view body,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept
        : kind_(kind),
          body_(body),
          resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    RequestObject(const RequestObject& other)
        : kind_(other.kind_),
          body_(other.body_),
          resource_(other.resource_) {}

    RequestObject& operator=(const RequestObject& other) {
        if (this == &other) {
            return *this;
        }
        kind_ = other.kind_;
        body_ = other.body_;
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

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view field) const {
        if (kind_ == RequestObjectKind::kJson) {
            return JsonObject(body_, resource_).get<T>(field);
        }
        if constexpr (detail::isFormField<T>) {
            return FormObject(body_, resource_).get<T>(field);
        } else {
            (void)field;
            return std::nullopt;
        }
    }

private:
    RequestObjectKind kind_{RequestObjectKind::kJson};
    std::string_view body_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
};

template <typename T>
void appendJson(std::pmr::string& output, const T& value) {
    output.reserve(output.size() + detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
}

template <typename T>
[[nodiscard]] std::pmr::string toJson(
    const T& value,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    std::pmr::string output(resource);
    output.reserve(detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
    return output;
}

}  // namespace ruvia
