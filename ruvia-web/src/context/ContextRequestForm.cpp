#include "ruvia/web/ContextRequest.h"

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia {

ContextRequest::RequestFormData::Object::Object(const RequestFormData& form, std::string_view dotPath)
    : form_(&form),
      dotPath_(dotPath, resourceFor(form)),
      entries_(resourceFor(form)) {
    rebuildEntries();
}

ContextRequest::RequestFormData::Object ContextRequest::RequestFormData::Object::object(std::string_view name) const {
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

std::string_view ContextRequest::RequestFormData::Object::directChildName(const ContextRequest::RequestFormField& field, std::string_view dotPath) noexcept {
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
    if (order.empty()) {
        return;
    }

    std::ranges::stable_sort(order, [this](std::size_t left, std::size_t right) noexcept {
        const auto leftName = directChildName(form_->fields_[left], path());
        const auto rightName = directChildName(form_->fields_[right], path());
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });

    struct EntryBuild final {
        std::size_t firstIndex;
        std::size_t begin;
        std::size_t end;
    };
    std::pmr::vector<EntryBuild> builds(currentResource);
    builds.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto firstIndex = order[offset];
        const auto name = directChildName(form_->fields_[firstIndex], path());
        do {
            ++offset;
        } while (offset < order.size() && directChildName(form_->fields_[order[offset]], path()) == name);
        builds.push_back(EntryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
    }
    std::ranges::stable_sort(builds, [](const EntryBuild& left, const EntryBuild& right) noexcept { return left.firstIndex < right.firstIndex; });

    entries_.reserve(builds.size());
    for (const auto& build : builds) {
        entries_.push_back(Entry::make(currentResource, directChildName(form_->fields_[build.firstIndex], path()), false));
        auto& formEntry = entries_.back();
        for (std::size_t i = build.begin; i < build.end; ++i) {
            formEntry.add(form_->fields_[order[i]]);
        }
    }
}

ContextRequest::RequestFormData::RequestFormData(std::pmr::vector<RequestFormField>&& fields)
    : fields_(std::move(fields)),
      entries_(fields_.get_allocator().resource()),
      pathEntries_(fields_.get_allocator().resource()) {
    rebuildEntries();
}

bool ContextRequest::RequestFormData::consumePath(const RequestFormField& field, std::size_t& index, std::string_view dotPath) noexcept {
    if (dotPath.empty()) {
        return true;
    }

    std::size_t offset = 0;
    while (true) {
        const auto dot = dotPath.find('.', offset);
        const auto segment = dot == std::string_view::npos ? dotPath.substr(offset) : dotPath.substr(offset, dot - offset);
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
    std::ranges::stable_sort(order, [this](std::size_t left, std::size_t right) noexcept {
        const auto leftName = entryName(fields_[left]);
        const auto rightName = entryName(fields_[right]);
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });

    struct EntryBuild final {
        std::size_t firstIndex;
        std::size_t begin;
        std::size_t end;
    };
    std::pmr::vector<EntryBuild> builds(resource);
    builds.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto firstIndex = order[offset];
        const auto name = entryName(fields_[firstIndex]);
        do {
            ++offset;
        } while (offset < order.size() && entryName(fields_[order[offset]]) == name);
        builds.push_back(EntryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
    }
    std::ranges::stable_sort(builds, [](const EntryBuild& left, const EntryBuild& right) noexcept { return left.firstIndex < right.firstIndex; });

    entries_.reserve(builds.size());
    for (const auto& build : builds) {
        entries_.push_back(Entry::make(resource, entryName(fields_[build.firstIndex]), false));
        auto& entry = entries_.back();
        for (std::size_t i = build.begin; i < build.end; ++i) {
            entry.add(fields_[order[i]]);
        }
    }

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
    if (order.empty()) {
        return;
    }

    std::ranges::stable_sort(order, [this](std::size_t left, std::size_t right) noexcept {
        const auto leftName = pathEntryName(fields_[left]);
        const auto rightName = pathEntryName(fields_[right]);
        if (leftName == rightName) {
            return left < right;
        }
        return leftName < rightName;
    });

    struct EntryBuild final {
        std::size_t firstIndex;
        std::size_t begin;
        std::size_t end;
    };
    std::pmr::vector<EntryBuild> builds(resource);
    builds.reserve(order.size());
    for (std::size_t offset = 0; offset < order.size();) {
        const auto begin = offset;
        const auto firstIndex = order[offset];
        const auto name = pathEntryName(fields_[firstIndex]);
        do {
            ++offset;
        } while (offset < order.size() && pathEntryName(fields_[order[offset]]) == name);
        builds.push_back(EntryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
    }
    std::ranges::stable_sort(builds, [](const EntryBuild& left, const EntryBuild& right) noexcept { return left.firstIndex < right.firstIndex; });

    pathEntries_.reserve(builds.size());
    for (const auto& build : builds) {
        pathEntries_.push_back(Entry::make(resource, pathEntryName(fields_[build.firstIndex]), false));
        auto& entry = pathEntries_.back();
        for (std::size_t i = build.begin; i < build.end; ++i) {
            entry.add(fields_[order[i]]);
        }
    }
}

}  // namespace ruvia
