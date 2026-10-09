#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/request_fields.h"

namespace ruvia::detail {

enum class model_input_kind : std::uint8_t { json,
    form,
    form_fields };

// Internal borrowed input for schema materialization, never a dynamic public model.
class model_input final {
public:
    model_input(model_input_kind kind, std::string_view body, std::pmr::memory_resource* resource,
        model_string_storage storage = model_string_storage::borrowed) noexcept
        : kind_(kind),
          body_(body),
          resource_(pmr_resource_or_default(resource)),
          string_storage_(storage) {}
    model_input(const request_name_value_list& fields_value, std::pmr::memory_resource* resource) noexcept
        : kind_(model_input_kind::form_fields),
          fields_(&fields_value),
          resource_(pmr_resource_or_default(resource)) {}

    [[nodiscard]] model_input_kind kind() const noexcept {
        return kind_;
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return body_;
    }
    [[nodiscard]] const request_name_value_list* fields() const noexcept {
        return fields_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    [[nodiscard]] model_string_storage string_storage() const noexcept {
        return string_storage_;
    }

private:
    model_input_kind kind_;
    std::string_view body_{};
    const request_name_value_list* fields_{nullptr};
    std::pmr::memory_resource* resource_;
    model_string_storage string_storage_{model_string_storage::borrowed};
};

[[nodiscard]] inline model_input make_json_model_input(std::string_view body,
    std::pmr::memory_resource* resource, model_string_storage storage = model_string_storage::borrowed) noexcept {
    return model_input(model_input_kind::json, body, resource, storage);
}
[[nodiscard]] inline model_input make_form_model_input(std::string_view body,
    std::pmr::memory_resource* resource, model_string_storage storage = model_string_storage::borrowed) noexcept {
    return model_input(model_input_kind::form, body, resource, storage);
}
[[nodiscard]] inline model_input make_form_fields_model_input(
    const request_name_value_list& fields_value, std::pmr::memory_resource* resource) noexcept {
    return model_input(fields_value, resource);
}

}  // namespace ruvia::detail
