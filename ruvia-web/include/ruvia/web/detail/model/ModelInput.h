#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/RequestFields.h"
#include "ruvia/web/detail/model/Traits.h"

namespace ruvia::detail {

enum class ModelInputKind : std::uint8_t { kJson,
    kForm,
    kFormFields };

// Internal borrowed input for schema materialization, never a dynamic public model.
class ModelInput final {
public:
    ModelInput(ModelInputKind kind, std::string_view body, std::pmr::memory_resource* resource,
        ModelStringStorage storage = ModelStringStorage::kBorrowed) noexcept
        : kind_(kind),
          body_(body),
          resource_(pmrResourceOrDefault(resource)),
          stringStorage_(storage) {}
    ModelInput(const RequestNameValueList& fields, std::pmr::memory_resource* resource) noexcept
        : kind_(ModelInputKind::kFormFields),
          fields_(&fields),
          resource_(pmrResourceOrDefault(resource)) {}

    [[nodiscard]] ModelInputKind kind() const noexcept {
        return kind_;
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }
    [[nodiscard]] const RequestNameValueList* fields() const noexcept {
        return fields_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    [[nodiscard]] ModelStringStorage stringStorage() const noexcept {
        return stringStorage_;
    }

private:
    ModelInputKind kind_;
    std::string_view body_{};
    const RequestNameValueList* fields_{nullptr};
    std::pmr::memory_resource* resource_;
    ModelStringStorage stringStorage_{ModelStringStorage::kBorrowed};
};

[[nodiscard]] inline ModelInput makeJsonModelInput(std::string_view body,
    std::pmr::memory_resource* resource, ModelStringStorage storage = ModelStringStorage::kBorrowed) noexcept {
    return ModelInput(ModelInputKind::kJson, body, resource, storage);
}
[[nodiscard]] inline ModelInput makeFormModelInput(std::string_view body,
    std::pmr::memory_resource* resource, ModelStringStorage storage = ModelStringStorage::kBorrowed) noexcept {
    return ModelInput(ModelInputKind::kForm, body, resource, storage);
}
[[nodiscard]] inline ModelInput makeFormFieldsModelInput(
    const RequestNameValueList& fields, std::pmr::memory_resource* resource) noexcept {
    return ModelInput(fields, resource);
}

}  // namespace ruvia::detail
