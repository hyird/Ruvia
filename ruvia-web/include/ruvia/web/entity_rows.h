#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"

namespace ruvia {

// A move-only entity collection. Elements share this collection's resource;
// the resource owner must outlive the collection and every element.
template <typename entity_type>
class entity_rows final {
public:
    explicit entity_rows(std::pmr::memory_resource* resource = nullptr)
        : rows_(detail::pmr_resource_or_default(resource)) {}
    entity_rows(const entity_rows&) = delete;
    entity_rows& operator=(const entity_rows&) = delete;
    entity_rows(entity_rows&&) noexcept = default;
    entity_rows& operator=(entity_rows&&) = delete;
    std::size_t size() const noexcept {
        return rows_.size();
    }
    bool empty() const noexcept {
        return rows_.empty();
    }
    const entity_type& operator[](std::size_t i) const& noexcept {
        return rows_[i];
    }
    entity_type& operator[](std::size_t i) & noexcept {
        return rows_[i];
    }
    const entity_type& operator[](std::size_t) const&& = delete;
    const entity_type* begin() const& noexcept {
        return rows_.data();
    }
    const entity_type* end() const& noexcept {
        return rows_.data() + rows_.size();
    }
    const entity_type* begin() const&& = delete;
    const entity_type* end() const&& = delete;
    void reserve(std::size_t count) {
        rows_.reserve(count);
    }
    void push_back(entity_type&& entity) {
        if (entity.resource() != rows_.get_allocator().resource()) {
            throw std::invalid_argument("entity result elements must use the result's memory resource");
        }
        rows_.push_back(std::move(entity));
    }

private:
    std::pmr::vector<entity_type> rows_;
};

}  // namespace ruvia
