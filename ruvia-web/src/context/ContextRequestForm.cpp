#include "ruvia/web/ContextRequest.h"

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia {

template <typename EntryName>
void ContextRequest::RequestFormData::buildGroups(const std::pmr::vector<RequestFormField>& fields,
    std::pmr::vector<std::size_t> order, std::pmr::vector<Group>& groups, EntryName entryName) {
    if (order.empty()) {
        return;
    }
    std::ranges::stable_sort(order, [&](std::size_t left, std::size_t right) noexcept {
        const auto leftName = entryName(fields[left]);
        const auto rightName = entryName(fields[right]);
        return leftName == rightName ? left < right : leftName < rightName;
    });

    struct GroupRange final {
        std::size_t firstIndex;
        std::size_t begin;
        std::size_t end;
    };
    std::pmr::vector<GroupRange> ranges(order.get_allocator().resource());
    ranges.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto firstIndex = order[offset];
        const auto name = entryName(fields[firstIndex]);
        do {
            ++offset;
        } while (offset < order.size() && entryName(fields[order[offset]]) == name);
        ranges.push_back(GroupRange{.firstIndex = firstIndex, .begin = begin, .end = offset});
    }
    std::ranges::stable_sort(ranges, [](const GroupRange& left, const GroupRange& right) noexcept {
        return left.firstIndex < right.firstIndex;
    });

    groups.reserve(ranges.size());
    for (const auto& range : ranges) {
        groups.push_back(Group::make(
            groups.get_allocator().resource(), entryName(fields[range.firstIndex]), false));
        auto& group = groups.back();
        for (std::size_t index = range.begin; index < range.end; ++index) {
            group.add(fields[order[index]]);
        }
    }
}

ContextRequest::RequestFormData::Object::Object(
    const RequestFormData& form, std::string_view dotPath)
    : form_(&form),
      dotPath_(dotPath, resourceFor(form)),
      entries_(resourceFor(form)) {
    rebuildEntries();
}

ContextRequest::RequestFormData::Object ContextRequest::RequestFormData::Object::object(
    std::string_view name) const {
    if (path().empty()) {
        return Object(*form_, name);
    }

    std::pmr::string childPath(resource());
    childPath.reserve(path().size() + (name.empty() ? 0 : 1 + name.size()));
    childPath.append(path());
    if (!name.empty()) {
        childPath.push_back('.');
        childPath.append(name);
    }
    return Object(*form_, std::string_view(childPath));
}

std::string_view ContextRequest::RequestFormData::Object::directChildName(
    const ContextRequest::RequestFormField& field, std::string_view dotPath) noexcept {
    const auto path = field.path();
    if (path.empty()) {
        return {};
    }

    std::size_t index = 0;
    if (!consumePath(field, index, dotPath) || index >= path.size() || index + 1 != path.size()) {
        return {};
    }

    const auto& child = path[index];
    return child;
}

void ContextRequest::RequestFormData::Object::rebuildEntries() {
    entries_.clear();
    auto* const currentResource = resource();
    std::pmr::vector<std::size_t> order(currentResource);
    order.reserve(form_->fields_.size());
    for (std::size_t i = 0; i < form_->fields_.size(); ++i) {
        if (!directChildName(form_->fields_[i], path()).empty()) {
            order.push_back(i);
        }
    }
    RequestFormData::buildGroups(form_->fields_, std::move(order), entries_,
        [this](const RequestFormField& field) noexcept { return directChildName(field, path()); });
}

ContextRequest::RequestFormData::RequestFormData(std::pmr::vector<RequestFormField>&& fields)
    : fields_(std::move(fields)),
      entries_(fields_.get_allocator().resource()),
      pathEntries_(fields_.get_allocator().resource()) {
    rebuildEntries();
}

bool ContextRequest::RequestFormData::consumePath(
    const RequestFormField& field, std::size_t& index, std::string_view dotPath) noexcept {
    if (dotPath.empty()) {
        return true;
    }

    std::size_t offset = 0;
    while (true) {
        const auto dot = dotPath.find('.', offset);
        const auto segment = dot == std::string_view::npos ? dotPath.substr(offset)
                                                           : dotPath.substr(offset, dot - offset);
        const auto path = field.path();
        if (segment.empty() || index >= path.size()) {
            return false;
        }
        const std::string_view stored = path[index];
        if (stored != segment) {
            return false;
        }
        ++index;
        if (dot == std::string_view::npos) {
            return true;
        }
        offset = dot + 1;
    }
}

void ContextRequest::RequestFormData::rebuildEntries() {
    entries_.clear();
    pathEntries_.clear();
    if (fields_.empty()) {
        return;
    }

    auto* const resource = fields_.get_allocator().resource();
    std::pmr::vector<std::size_t> order(resource);
    order.reserve(fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        order.push_back(i);
    }
    buildGroups(fields_, std::move(order), entries_,
        [](const RequestFormField& field) noexcept { return entryName(field); });

    rebuildPathEntries(resource);
}

void ContextRequest::RequestFormData::rebuildPathEntries(std::pmr::memory_resource* resource) {
    std::pmr::vector<std::size_t> order(resource);
    order.reserve(fields_.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (!fields_[i].path().empty()) {
            order.push_back(i);
        }
    }
    buildGroups(fields_, std::move(order), pathEntries_,
        [](const RequestFormField& field) noexcept { return pathEntryName(field); });
}

}  // namespace ruvia
