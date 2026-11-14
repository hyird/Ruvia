#include "http_multiplexed_stream.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ruvia {

namespace detail {

void http_received_data_target::release() noexcept {
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        delete this;
    }
}

void http_received_data_credit_access::swap(http_received_data_credit& left, http_received_data_credit& right) noexcept {
    std::swap(left.target_, right.target_);
    std::swap(left.stream_id_, right.stream_id_);
    std::swap(left.bytes_, right.bytes_);
}

}  // namespace detail

http_received_data_credit::http_received_data_credit(detail::http_received_data_target* target, std::uint64_t stream_id, std::size_t bytes) noexcept :
    target_(target),
    stream_id_(stream_id),
    bytes_(bytes) {
    target_->retain();
}

http_received_data_credit::~http_received_data_credit() {
    if (target_ == nullptr) {
        return;
    }
    target_->return_credit(stream_id_, bytes_);
    target_->release();
}

http_received_data_credit::http_received_data_credit(http_received_data_credit&& other) noexcept :
    target_(std::exchange(other.target_, nullptr)),
    stream_id_(std::exchange(other.stream_id_, 0)),
    bytes_(std::exchange(other.bytes_, 0)) {}

void http_received_data_credit::merge(http_received_data_credit&& other) {
    if (this == &other || !valid() || !other.valid()) {
        throw std::logic_error("invalid HTTP received data credit");
    }
    if (target_ != other.target_ || stream_id_ != other.stream_id_) {
        throw std::logic_error("HTTP received data credits belong to different streams");
    }
    if (bytes_ > (std::numeric_limits<std::size_t>::max)() - other.bytes_) {
        throw std::overflow_error("HTTP received data credit overflow");
    }
    bytes_ += other.bytes_;
    auto* target = std::exchange(other.target_, nullptr);
    other.stream_id_ = 0;
    other.bytes_ = 0;
    target->release();
}

}  // namespace ruvia
