#pragma once

#include <memory_resource>
#include <string>
#include <vector>

#include "ruvia/http/http3_field_section.h"

#include "field/binary_field_name.h"

namespace ruvia::detail {

// Own decoded bytes until the complete section passes validation. Direction
// policy is compile-time; HTTP/3 wire spelling and collection are shared.
// The direction policy receives an already validated lowercase field name.
template <auto validate_field_policy>
struct http3_trailer_collector final {
    struct field_storage final {
        field_storage(http3_field_section_field_view field, std::pmr::memory_resource* resource)
            : name_(field.name_, resource),
              value_(field.value_, resource),
              never_indexed_(field.never_indexed_) {}
        std::pmr::string name_;
        std::pmr::string value_;
        bool never_indexed_;
    };

    explicit http3_trailer_collector(std::pmr::memory_resource* resource)
        : fields_(resource) {}

    static bool collect(void* opaque, http3_field_section_field_view field) {
        auto& collector_value = *static_cast<http3_trailer_collector*>(opaque);
        if (!is_valid_binary_field_name(field.name_) || !validate_field_policy(field)) {
            collector_value.valid_ = false;
            return false;
        }
        collector_value.fields_.emplace_back(field, collector_value.fields_.get_allocator().resource());
        return true;
    }

    std::pmr::vector<field_storage> fields_;
    bool valid_{true};
};

}  // namespace ruvia::detail
