#pragma once

#include <memory_resource>
#include <string>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"

namespace ruvia::detail {

// Own decoded bytes until the complete section passes validation. Direction
// policy is compile-time; HTTP/3 wire spelling and collection are shared.
template <auto validate_field>
struct http3_trailer_collector final {
    struct field_storage final {
        field_storage(Http3FieldSectionFieldView field, std::pmr::memory_resource* resource)
            : name_(field.name, resource),
              value_(field.value, resource),
              never_indexed_(field.neverIndexed) {}
        std::pmr::string name_;
        std::pmr::string value_;
        bool never_indexed_;
    };

    explicit http3_trailer_collector(std::pmr::memory_resource* resource)
        : fields_(resource) {}

    static bool collect(void* opaque, Http3FieldSectionFieldView field) {
        auto& collector = *static_cast<http3_trailer_collector*>(opaque);
        bool valid = !field.name.empty() && field.name.front() != ':';
        for (const unsigned char ch : field.name) {
            if (ch >= 'A' && ch <= 'Z') {
                valid = false;
                break;
            }
        }
        if (!valid || !validate_field(field)) {
            collector.valid_ = false;
            return false;
        }
        collector.fields_.emplace_back(field, collector.fields_.get_allocator().resource());
        return true;
    }

    std::pmr::vector<field_storage> fields_;
    bool valid_{true};
};

}  // namespace ruvia::detail
