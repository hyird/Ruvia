#include "ruvia/web/HttpClientAdvertisement.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/client/HttpClientAdvertisementQueue.h"

namespace ruvia::detail {

class HttpClientAdvertisementMemory final {
public:
    explicit HttpClientAdvertisementMemory(const WorkerHandle& worker, std::pmr::memory_resource& upstream)
        : memory_(upstream),
          worker_(worker) {}

    void retain(std::size_t bytes) noexcept {
        requireCurrent();
        if (references_ == std::numeric_limits<std::size_t>::max() || bytes > std::numeric_limits<std::size_t>::max() - retainedBytes_) {
            std::terminate();
        }
        ++references_;
        retainedBytes_ += bytes;
        active_ = true;
    }
    void release(std::size_t bytes) noexcept {
        requireCurrent();
        if (bytes > retainedBytes_ || references_ <= 1) {
            std::terminate();
        }
        retainedBytes_ -= bytes;
        --references_;
        if (retired_ && references_ == 1) {
            destroy();
        }
    }
    void retire() noexcept {
        if (active_) {
            requireCurrent();
        }
        if (retired_) {
            std::terminate();
        }
        retired_ = true;
        if (references_ == 1) {
            destroy();
        }
    }
    [[nodiscard]] std::size_t retainedBytes() const noexcept {
        return retainedBytes_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() noexcept {
        return memory_.resource();
    }
    void requireCurrent() const noexcept {
        if (!worker_.isCurrent()) {
            std::terminate();
        }
    }

private:
    void destroy() noexcept {
        if (retainedBytes_ != 0 || references_ != 1) {
            std::terminate();
        }
        destroyPmrObject(this, processResource());
    }
    WorkerMemory memory_;
    WorkerHandle worker_;
    std::size_t references_{1};
    std::size_t retainedBytes_{};
    bool active_{};
    bool retired_{};
};

struct HttpClientAdvertisementState final {
    HttpClientAdvertisementState(HttpClientAdvertisementMemory& owner, std::size_t slot,
        HttpProtocolVersion protocol, const HttpOriginAdvertisement& origins, std::size_t bytes)
        : memory(owner),
          slot(slot),
          protocol(protocol),
          bytes(bytes),
          value(std::in_place_type<HttpOriginAdvertisement>, owner.resource()) {
        auto& retained = std::get<HttpOriginAdvertisement>(value);
        retained.origins.reserve(origins.origins.size());
        for (const auto& origin : origins.origins) {
            retained.origins.emplace_back(origin);
        }
        memory.retain(bytes);
    }
    HttpClientAdvertisementState(HttpClientAdvertisementMemory& owner, std::size_t slot,
        const HttpAlternativeServiceAdvertisement& service, std::size_t bytes)
        : memory(owner),
          slot(slot),
          protocol(HttpProtocolVersion::kHttp2),
          bytes(bytes),
          value(std::in_place_type<HttpAlternativeServiceAdvertisement>, owner.resource()) {
        auto& retained = std::get<HttpAlternativeServiceAdvertisement>(value);
        retained.streamId = service.streamId;
        retained.origin.assign(service.origin);
        retained.fieldValue.assign(service.fieldValue);
        memory.retain(bytes);
    }
    HttpClientAdvertisementMemory& memory;
    std::size_t slot;
    HttpProtocolVersion protocol;
    std::size_t bytes;
    std::variant<HttpOriginAdvertisement, HttpAlternativeServiceAdvertisement> value;
};

namespace {
void releaseAdvertisement(HttpClientAdvertisementState* state) noexcept {
    if (state == nullptr) {
        return;
    }
    auto& memory = state->memory;
    memory.requireCurrent();
    const auto bytes = state->bytes;
    destroyPmrObject(state, memory.resource());
    memory.release(bytes);
}
}  // namespace

HttpClientAdvertisementQueue::HttpClientAdvertisementQueue(const WorkerHandle& worker,
    HttpClientAdvertisementConfig config, std::pmr::memory_resource* resource)
    : HttpClientAdvertisementQueue(worker, config, resource, *processResource()) {}
HttpClientAdvertisementQueue::HttpClientAdvertisementQueue(const WorkerHandle& worker,
    HttpClientAdvertisementConfig config, std::pmr::memory_resource* resource, std::pmr::memory_resource& upstream)
    : worker_(worker),
      config_(config),
      pending_(resource) {
    if (!worker.valid() || resource == nullptr || config.maxQueuedAdvertisements == 0 || config.maxRetainedBytes == 0) {
        throw std::invalid_argument("HTTP advertisement queue requires worker, memory and positive bounds");
    }
    if (config.receiveOrigins || config.receiveAlternativeServices) {
        memory_ = makePmrObject<HttpClientAdvertisementMemory>(processResource(), worker, upstream).release();
    }
}
HttpClientAdvertisementQueue::~HttpClientAdvertisementQueue() {
    pending_.clear();
    if (memory_ != nullptr) {
        memory_->retire();
    }
}
void HttpClientAdvertisementQueue::requireCurrent() const noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
}
bool HttpClientAdvertisementQueue::admit(std::size_t bytes) noexcept {
    if (memory_ == nullptr || pending_.size() >= config_.maxQueuedAdvertisements ||
        bytes > config_.maxRetainedBytes - std::min(config_.maxRetainedBytes, memory_->retainedBytes())) {
        ++dropped_;
        return false;
    }
    return true;
}
bool HttpClientAdvertisementQueue::retain(std::size_t slot, HttpProtocolVersion protocol, const HttpOriginAdvertisement& origins) {
    requireCurrent();
    if (!config_.receiveOrigins) {
        return false;
    }
    if (origins.origins.size() > (config_.maxRetainedBytes / sizeof(std::pmr::string))) {
        ++dropped_;
        return false;
    }
    auto bytes = sizeof(HttpClientAdvertisementState) + origins.origins.size() * sizeof(std::pmr::string);
    for (const auto& origin : origins.origins) {
        if (origin.size() > config_.maxRetainedBytes - std::min(bytes, config_.maxRetainedBytes)) {
            ++dropped_;
            return false;
        }
        bytes += origin.size();
    }
    if (!admit(bytes)) {
        return false;
    }
    auto owned = makePmrObject<HttpClientAdvertisementState>(memory_->resource(), *memory_, slot, protocol, origins, bytes);
    HttpClientAdvertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
    return true;
}
bool HttpClientAdvertisementQueue::retain(std::size_t slot, const HttpAlternativeServiceAdvertisement& service) {
    requireCurrent();
    if (!config_.receiveAlternativeServices) {
        return false;
    }
    auto bytes = sizeof(HttpClientAdvertisementState);
    for (const auto size : {service.origin.size(), service.fieldValue.size()}) {
        if (size > config_.maxRetainedBytes - std::min(bytes, config_.maxRetainedBytes)) {
            ++dropped_;
            return false;
        }
        bytes += size;
    }
    if (!admit(bytes)) {
        return false;
    }
    auto owned = makePmrObject<HttpClientAdvertisementState>(memory_->resource(), *memory_, slot, service, bytes);
    HttpClientAdvertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
    return true;
}
std::optional<HttpClientAdvertisement> HttpClientAdvertisementQueue::next() {
    requireCurrent();
    if (pending_.empty()) {
        return std::nullopt;
    }
    std::optional<HttpClientAdvertisement> advertisement(std::in_place, std::move(pending_.front()));
    pending_.pop_front();
    return advertisement;
}
void HttpClientAdvertisementQueue::clear() noexcept {
    requireCurrent();
    pending_.clear();
}
void HttpClientAdvertisementQueue::retire() noexcept {
    requireCurrent();
    pending_.clear();
    if (auto* memory = std::exchange(memory_, nullptr)) {
        memory->retire();
    }
}
std::size_t HttpClientAdvertisementQueue::retainedBytes() const noexcept {
    return memory_ == nullptr ? 0 : memory_->retainedBytes();
}

}  // namespace ruvia::detail

namespace ruvia {
HttpClientAdvertisement::HttpClientAdvertisement(HttpClientAdvertisement&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
HttpClientAdvertisement& HttpClientAdvertisement::operator=(HttpClientAdvertisement&& other) noexcept {
    if (this != &other) {
        detail::releaseAdvertisement(state_);
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
HttpClientAdvertisement::~HttpClientAdvertisement() {
    detail::releaseAdvertisement(state_);
}
HttpProtocolVersion HttpClientAdvertisement::protocolVersion() const noexcept {
    return state_->protocol;
}
std::size_t HttpClientAdvertisement::connectionSlot() const noexcept {
    return state_->slot;
}
const HttpOriginAdvertisement* HttpClientAdvertisement::origins() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<HttpOriginAdvertisement>(&state_->value);
}
const HttpAlternativeServiceAdvertisement* HttpClientAdvertisement::alternativeService() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<HttpAlternativeServiceAdvertisement>(&state_->value);
}
}  // namespace ruvia
