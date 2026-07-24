#pragma once

#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/ContextRequest.h"

namespace ruvia::detail {

struct RequestFormFieldAccess final {
    [[nodiscard]] static ContextRequest::RequestFormField make(std::pmr::memory_resource* resource, std::pmr::string&& name, std::pmr::string&& value, std::pmr::string&& filename, std::pmr::string&& contentType, bool file, bool array) {
        return ContextRequest::RequestFormField(resource, std::move(name), std::move(value), std::move(filename), std::move(contentType), file, array);
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& path(ContextRequest::RequestFormField& field) noexcept {
        return field.path_;
    }

    [[nodiscard]] static const std::pmr::vector<std::pmr::string>& path(const ContextRequest::RequestFormField& field) noexcept {
        return field.path_;
    }
};

struct RequestFormDataAccess final {
    [[nodiscard]] static ContextRequest::RequestFormData empty(std::pmr::memory_resource* resource) {
        return ContextRequest::RequestFormData(resource);
    }

    [[nodiscard]] static ContextRequest::RequestFormData fromFields(std::pmr::vector<ContextRequest::RequestFormField>&& fields) {
        return ContextRequest::RequestFormData(std::move(fields));
    }
};

}  // namespace ruvia::detail
