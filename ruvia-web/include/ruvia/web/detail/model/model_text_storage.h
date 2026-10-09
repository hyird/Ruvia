#pragma once

#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/web/attributes.h"

namespace ruvia::detail {

// Text semantics (ordinary string or JSON token) belong to the facade. This
// value owns the common borrow/clone/transfer and strong publication rules.
class model_text_storage final {
public:
    explicit model_text_storage(std::string_view borrowed = {}) noexcept
        : value_(std::in_place_type<std::string_view>, borrowed) {}

    model_text_storage(const model_text_storage&) = delete;
    model_text_storage& operator=(const model_text_storage&) = delete;
    model_text_storage(model_text_storage&&) noexcept = default;
    model_text_storage& operator=(model_text_storage&&) = delete;

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&value_)) {
            return *borrowed;
        }
        return std::get<std::pmr::string>(value_);
    }
    std::string_view view() const&& = delete;

    void assign_borrowed(std::string_view value) noexcept {
        value_.emplace<std::string_view>(value);
    }

    void assign_owned(std::string_view value, std::pmr::memory_resource* resource) {
        std::pmr::string owned(value, resource);
        value_.emplace<std::pmr::string>(std::move(owned));
    }

    void assign_owned(std::pmr::string&& value, std::pmr::memory_resource* resource) {
        std::pmr::string owned(std::move(value), resource);
        value_.emplace<std::pmr::string>(std::move(owned));
    }

    [[nodiscard]] model_text_storage rebind(std::pmr::memory_resource* resource) const& {
        return model_text_storage(std::pmr::string(view(), resource));
    }

    [[nodiscard]] model_text_storage rebind(std::pmr::memory_resource* resource) && {
        if (auto* owned = std::get_if<std::pmr::string>(&value_)) {
            if (owned->get_allocator().resource() == resource) {
                return model_text_storage(std::move(*owned));
            }
        }
        return static_cast<const model_text_storage&>(*this).rebind(resource);
    }

    void assign_from(model_text_storage&& source_value, std::pmr::memory_resource* resource) {
        if (this == &source_value) {
            return;
        }
        auto rebound = std::move(source_value).rebind(resource);
        // Rebinding may throw; replacing with an already-owned variant cannot.
        std::destroy_at(&value_);
        std::construct_at(&value_, std::move(rebound.value_));
    }

private:
    explicit model_text_storage(std::pmr::string&& owned) noexcept
        : value_(std::in_place_type<std::pmr::string>, std::move(owned)) {}

    std::variant<std::string_view, std::pmr::string> value_;
};

}  // namespace ruvia::detail
