#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <vector>

#include "ruvia/http/detail/field/http_header_section_size.h"
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

    http3_trailer_collector(std::pmr::memory_resource* resource, http3_field_section_limits limits)
        : fields_(resource),
          section_size_(limits.max_decoded_bytes_),
          max_fields_(limits.max_fields_) {}

    static bool collect(void* opaque, http3_field_section_field_view field) {
        auto& collector_value = *static_cast<http3_trailer_collector*>(opaque);
        if (collector_value.fields_.size() >= collector_value.max_fields_ ||
            !collector_value.section_size_.add(field.name_, field.value_)) {
            collector_value.valid_ = false;
            collector_value.limit_exceeded_ = true;
            return false;
        }
        if (!is_valid_binary_field_name(field.name_) || !validate_field_policy(field)) {
            collector_value.valid_ = false;
            return false;
        }
        collector_value.fields_.emplace_back(field, collector_value.fields_.get_allocator().resource());
        return true;
    }

    std::pmr::vector<field_storage> fields_;
    bool valid_{true};
    bool limit_exceeded_{false};

private:
    http_header_section_size section_size_;
    std::size_t max_fields_;
};

}  // namespace ruvia::detail
