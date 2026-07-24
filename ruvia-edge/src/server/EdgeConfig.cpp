#include "ruvia/edge/detail/server/EdgeConfig.h"

#include <cassert>
#include <memory>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"

namespace ruvia::edge {

struct EdgeOriginControl final {
    EdgeOriginControl(OriginSettings source, std::pmr::memory_resource* sourceResource)
        : settings(std::move(source)),
          resource(sourceResource) {}

    std::size_t references{1};  // the table owns the initial reference
    OriginSettings settings;
    std::pmr::memory_resource* resource;
};

namespace {

void releaseOrigin(EdgeOriginControl* control) noexcept {
    if (control == nullptr) {
        return;
    }
    assert(control->references > 0);
    if (--control->references == 0) {
        ::ruvia::detail::destroyPmrObject(control, control->resource);
    }
}

}  // namespace

OriginLease::OriginLease(EdgeOriginControl* control) noexcept
    : control_(control) {
    if (control_ != nullptr) {
        ++control_->references;
    }
}

OriginLease::~OriginLease() {
    releaseOrigin(control_);
}

OriginLease::OriginLease(OriginLease&& other) noexcept
    : control_(std::exchange(other.control_, nullptr)) {}

OriginLease& OriginLease::operator=(OriginLease&& other) noexcept {
    if (this != &other) {
        releaseOrigin(control_);
        control_ = std::exchange(other.control_, nullptr);
    }
    return *this;
}

const OriginSettings* OriginLease::get() const noexcept {
    return control_ != nullptr ? &control_->settings : nullptr;
}

const OriginSettings& OriginLease::operator*() const noexcept {
    assert(control_ != nullptr);
    return control_->settings;
}

const OriginSettings* OriginLease::operator->() const noexcept {
    assert(control_ != nullptr);
    return &control_->settings;
}

EdgeConfig::EdgeConfig(std::pmr::memory_resource* resource) noexcept
    : resource_(::ruvia::detail::pmrResourceOrDefault(resource)),
      origins_(resource_) {}

EdgeConfig::~EdgeConfig() {
    for (const auto& [key, control] : origins_) {
        (void)key;
        releaseOrigin(control);
    }
}

OriginLease EdgeConfig::findOrigin(std::string_view frontHost) noexcept {
    const auto it = origins_.find(frontHost);
    return OriginLease(it != origins_.end() ? it->second : nullptr);
}

bool EdgeConfig::addOrigin(std::string frontHost, OriginSettings settings) {
    auto replacement = ::ruvia::detail::makePmrObject<EdgeOriginControl>(resource_, std::move(settings), resource_);
    const auto [it, inserted] = origins_.try_emplace(std::pmr::string(std::move(frontHost), resource_), replacement.get());
    if (inserted) {
        (void)replacement.release();
        return true;
    }

    EdgeOriginControl* previous = std::exchange(it->second, replacement.release());
    releaseOrigin(previous);
    return false;
}

bool EdgeConfig::removeOrigin(std::string_view frontHost) noexcept {
    const auto it = origins_.find(frontHost);
    if (it == origins_.end()) {
        return false;
    }
    EdgeOriginControl* removed = it->second;
    origins_.erase(it);
    releaseOrigin(removed);
    return true;
}

}  // namespace ruvia::edge
