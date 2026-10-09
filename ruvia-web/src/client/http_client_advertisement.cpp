#include "ruvia/web/http_client_advertisement.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"

#include "client/http_client_advertisement_queue.h"

namespace ruvia::detail {

class http_client_advertisement_memory final {
public:
    explicit http_client_advertisement_memory(const worker_handle& worker_value, std::pmr::memory_resource& upstream)
        : memory_(upstream),
          worker_(worker_value) {}

    void retain(std::size_t bytes_value) noexcept {
        require_current();
        if (references_ == std::numeric_limits<std::size_t>::max() || bytes_value > std::numeric_limits<std::size_t>::max() - retained_bytes_) {
            std::terminate();
        }
        ++references_;
        retained_bytes_ += bytes_value;
        active_ = true;
    }
    void release(std::size_t bytes_value) noexcept {
        require_current();
        if (bytes_value > retained_bytes_ || references_ <= 1) {
            std::terminate();
        }
        retained_bytes_ -= bytes_value;
        --references_;
        if (retired_ && references_ == 1) {
            destroy();
        }
    }
    void retire() noexcept {
        if (active_) {
            require_current();
        }
        if (retired_) {
            std::terminate();
        }
        retired_ = true;
        if (references_ == 1) {
            destroy();
        }
    }
    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() noexcept {
        return memory_.resource();
    }
    void require_current() const noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
    }

private:
    void destroy() noexcept {
        if (retained_bytes_ != 0 || references_ != 1) {
            std::terminate();
        }
        destroy_pmr_object(this, process_resource());
    }
    worker_memory memory_;
    worker_handle worker_;
    std::size_t references_{1};
    std::size_t retained_bytes_{};
    bool active_{};
    bool retired_{};
};

struct http_client_advertisement_state final {
    http_client_advertisement_state(http_client_advertisement_memory& owner_value, std::size_t slot,
        http_protocol_version protocol, const http_origin_advertisement& origins, std::size_t bytes_value)
        : memory_(owner_value),
          slot_(slot),
          protocol_(protocol),
          bytes_(bytes_value),
          value_(std::in_place_type<http_origin_advertisement>, owner_value.resource()) {
        auto& retained = std::get<http_origin_advertisement>(value_);
        retained.origins_.reserve(origins.origins_.size());
        for (const auto& origin : origins.origins_) {
            retained.origins_.emplace_back(origin);
        }
        memory_.retain(bytes_value);
    }
    http_client_advertisement_state(http_client_advertisement_memory& owner_value, std::size_t slot,
        const http_alternative_service_advertisement& service, std::size_t bytes_value)
        : memory_(owner_value),
          slot_(slot),
          protocol_(http_protocol_version::http2),
          bytes_(bytes_value),
          value_(std::in_place_type<http_alternative_service_advertisement>, owner_value.resource()) {
        auto& retained = std::get<http_alternative_service_advertisement>(value_);
        retained.stream_id_ = service.stream_id_;
        retained.origin_.assign(service.origin_);
        retained.field_value_.assign(service.field_value_);
        memory_.retain(bytes_value);
    }
    http_client_advertisement_memory& memory_;
    std::size_t slot_;
    http_protocol_version protocol_;
    std::size_t bytes_;
    std::variant<http_origin_advertisement, http_alternative_service_advertisement> value_;
};

namespace {
void release_advertisement(http_client_advertisement_state* state_value) noexcept {
    if (state_value == nullptr) {
        return;
    }
    auto& memory = state_value->memory_;
    memory.require_current();
    const auto bytes_value = state_value->bytes_;
    destroy_pmr_object(state_value, memory.resource());
    memory.release(bytes_value);
}
}  // namespace

http_client_advertisement_queue::http_client_advertisement_queue(const worker_handle& worker_value,
    http_client_advertisement_config config, std::pmr::memory_resource* resource)
    : http_client_advertisement_queue(worker_value, config, resource, *process_resource()) {}
http_client_advertisement_queue::http_client_advertisement_queue(const worker_handle& worker_value,
    http_client_advertisement_config config, std::pmr::memory_resource* resource, std::pmr::memory_resource& upstream)
    : worker_(worker_value),
      config_(config),
      pending_(resource) {
    if (!worker_value.valid() || resource == nullptr || config.max_queued_advertisements_ == 0 || config.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement queue requires worker, memory and positive bounds");
    }
    if (config.receive_origins_ || config.receive_alternative_services_) {
        memory_ = make_pmr_object<http_client_advertisement_memory>(process_resource(), worker_value, upstream).release();
    }
}
http_client_advertisement_queue::~http_client_advertisement_queue() {
    pending_.clear();
    if (memory_ != nullptr) {
        memory_->retire();
    }
}
void http_client_advertisement_queue::require_current() const noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
}
bool http_client_advertisement_queue::admit(std::size_t bytes_value) noexcept {
    if (memory_ == nullptr || pending_.size() >= config_.max_queued_advertisements_ ||
        bytes_value > config_.max_retained_bytes_ - std::min(config_.max_retained_bytes_, memory_->retained_bytes())) {
        ++dropped_;
        return false;
    }
    return true;
}
bool http_client_advertisement_queue::retain(std::size_t slot, http_protocol_version protocol, const http_origin_advertisement& origins) {
    require_current();
    if (!config_.receive_origins_) {
        return false;
    }
    if (origins.origins_.size() > (config_.max_retained_bytes_ / sizeof(std::pmr::string))) {
        ++dropped_;
        return false;
    }
    auto bytes_value = sizeof(http_client_advertisement_state) + origins.origins_.size() * sizeof(std::pmr::string);
    for (const auto& origin : origins.origins_) {
        if (origin.size() > config_.max_retained_bytes_ - std::min(bytes_value, config_.max_retained_bytes_)) {
            ++dropped_;
            return false;
        }
        bytes_value += origin.size();
    }
    if (!admit(bytes_value)) {
        return false;
    }
    auto owned = make_pmr_object<http_client_advertisement_state>(memory_->resource(), *memory_, slot, protocol, origins, bytes_value);
    http_client_advertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
    return true;
}
bool http_client_advertisement_queue::retain(std::size_t slot, const http_alternative_service_advertisement& service) {
    require_current();
    if (!config_.receive_alternative_services_) {
        return false;
    }
    auto bytes_value = sizeof(http_client_advertisement_state);
    for (const auto size : {service.origin_.size(), service.field_value_.size()}) {
        if (size > config_.max_retained_bytes_ - std::min(bytes_value, config_.max_retained_bytes_)) {
            ++dropped_;
            return false;
        }
        bytes_value += size;
    }
    if (!admit(bytes_value)) {
        return false;
    }
    auto owned = make_pmr_object<http_client_advertisement_state>(memory_->resource(), *memory_, slot, service, bytes_value);
    http_client_advertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
    return true;
}
std::optional<http_client_advertisement> http_client_advertisement_queue::next() {
    require_current();
    if (pending_.empty()) {
        return std::nullopt;
    }
    std::optional<http_client_advertisement> advertisement(std::in_place, std::move(pending_.front()));
    pending_.pop_front();
    return advertisement;
}
void http_client_advertisement_queue::clear() noexcept {
    require_current();
    pending_.clear();
}
void http_client_advertisement_queue::retire() noexcept {
    require_current();
    pending_.clear();
    if (auto* memory = std::exchange(memory_, nullptr)) {
        memory->retire();
    }
}
std::size_t http_client_advertisement_queue::retained_bytes() const noexcept {
    return memory_ == nullptr ? 0 : memory_->retained_bytes();
}

}  // namespace ruvia::detail

namespace ruvia {
http_client_advertisement::http_client_advertisement(http_client_advertisement&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
http_client_advertisement& http_client_advertisement::operator=(http_client_advertisement&& other) noexcept {
    if (this != &other) {
        detail::release_advertisement(state_);
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
http_client_advertisement::~http_client_advertisement() {
    detail::release_advertisement(state_);
}
http_protocol_version http_client_advertisement::protocol_version() const noexcept {
    return state_->protocol_;
}
std::size_t http_client_advertisement::connection_slot() const noexcept {
    return state_->slot_;
}
const http_origin_advertisement* http_client_advertisement::origins() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<http_origin_advertisement>(&state_->value_);
}
const http_alternative_service_advertisement* http_client_advertisement::alternative_service() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<http_alternative_service_advertisement>(&state_->value_);
}
}  // namespace ruvia
