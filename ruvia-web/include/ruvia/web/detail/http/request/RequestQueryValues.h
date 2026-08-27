#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/RequestFields.h"

namespace ruvia::detail {

// Request-query multivalue indexing is an implementation detail of
// ContextRequest::queries(). It must not become a second public request-field
// model alongside RequestNameValueList.
class RequestQueryValues final {
public:
    class Group final {
    public:
        Group(std::pmr::memory_resource* resource, std::string_view name)
            : name_(name),
              values_(pmrResourceOrDefault(resource)) {}

        void add(std::string_view value) {
            values_.push_back(value);
        }

        [[nodiscard]] std::string_view name() const noexcept {
            return name_;
        }

        [[nodiscard]] std::span<const std::string_view> values() const noexcept {
            return values_;
        }

    private:
        std::string_view name_;
        std::pmr::vector<std::string_view> values_;
    };

    explicit RequestQueryValues(std::pmr::memory_resource* resource)
        : groups_(pmrResourceOrDefault(resource)) {}

    void reserve(std::size_t count) {
        groups_.reserve(count);
    }

    [[nodiscard]] Group& append(std::string_view name) {
        return groups_.emplace_back(groups_.get_allocator().resource(), name);
    }

    [[nodiscard]] std::span<const std::string_view> values(std::string_view name) const noexcept {
        for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
            if (it->name() == name) {
                return it->values();
            }
        }
        return {};
    }

private:
    std::pmr::vector<Group> groups_;
};

// The flattened scalar view and multivalue index are materialized together.
// One owner prevents Context from representing a half-built query cache.
class RequestQueryCache final {
public:
    RequestQueryCache(std::pmr::vector<std::pmr::string>&& storage, RequestNameValueList&& fields,
        RequestQueryValues&& values) noexcept
        : storage_(std::move(storage)),
          fields_(std::move(fields)),
          values_(std::move(values)) {}

    [[nodiscard]] const RequestNameValueList& fields() const& noexcept {
        return fields_;
    }
    const RequestNameValueList& fields() const&& = delete;

    [[nodiscard]] const RequestQueryValues& values() const& noexcept {
        return values_;
    }
    const RequestQueryValues& values() const&& = delete;

private:
    // The public fields and multivalue groups borrow these decoded strings.
    // Keep storage first so reverse member destruction drops borrowers before
    // their backing bytes.
    std::pmr::vector<std::pmr::string> storage_;
    RequestNameValueList fields_;
    RequestQueryValues values_;
};

}  // namespace ruvia::detail
