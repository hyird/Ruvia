#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_request_target.h"

#include "http3/http3_connection_driver.h"
#include "http3/http3_server_connection.h"
#include "http3/http3_server_request_entry.h"

// HTTP/3 server push (RFC 9114 §4.6): PUSH_PROMISE on the parent request
// stream and the promised response on a server push stream.

namespace ruvia::detail {

task<bool> http3_server_connection::push_request(std::uint64_t parent_stream_id, http_push_request_view request) {
    auto* parent_value = requests_.find(parent_stream_id);
    const auto* original = session_.request(parent_stream_id);
    const auto maximum = session_.peer_max_push_id();
    if (!on_worker() || admission_closed_ || parent_value == nullptr || parent_value->push_id_ ||
        parent_value->entry_ == nullptr || parent_value->entry_->dispatch_.response_aborted() || original == nullptr ||
        !maximum || next_push_id_ > *maximum || next_push_id_ >= session_.max_remembered_pushes()) {
        co_return false;
    }
    if (!http_ascii_equals_ignore_case(original->request().scheme(), request.scheme_) ||
        !http_authorities_equal(borrowed_text(original->request().authority()), borrowed_text(request.authority_),
            original->request().scheme() == "https" ? 443 : 80)) {
        throw std::invalid_argument("push request must use its associated request origin");
    }
    if (requests_.tracked() >= max_tracked_streams_) {
        co_return false;
    }
    const auto push_id = next_push_id_;
    auto promise = session_.prepare_push_promise(parent_stream_id, push_id, request);
    if (promise.index() != 0) {
        if (std::get<1>(promise) == http3_connection_error_code::message_error) {
            throw std::invalid_argument("invalid HTTP/3 push request");
        }
        co_return false;
    }
    ++next_push_id_;
    const auto stream_id = transport_.open_push_stream(push_id);
    if (!stream_id) {
        co_return false;
    }
    bool created = false;
    auto* pushed = requests_.find_or_create(*stream_id, created);
    if (pushed == nullptr || !created) {
        close_connection(http3_connection_error_code::excessive_load);
        co_return false;
    }
    pushed->push_id_ = push_id;
    if (admission_closed_ || parent_value->entry_ == nullptr || parent_value->entry_->dispatch_.response_aborted()) {
        pushed->status_ = request_status_type::cancelled;
        reset_stream(*pushed);
        co_return false;
    }
    try {
        auto prefix = session_.admit_push_stream(*stream_id, push_id);
        if (prefix.index() != 0) {
            pushed->status_ = request_status_type::cancelled;
            reset_stream(*pushed);
            co_return false;
        }
        pushed->push_prefix_.emplace(std::move(std::get<0>(prefix)));
        co_await parent_value->entry_->dispatch_.publish_response_bytes(std::get<0>(promise));
        if (admission_closed_ || pushed->push_cancelled_) {
            pushed->push_prefix_.reset();
            (void)session_.cancel_request(*stream_id);
            pushed->status_ = request_status_type::cancelled;
            reset_stream(*pushed);
            co_return false;
        }
        co_return admit_finished_request(*stream_id);
    } catch (...) {
        pushed->push_prefix_.reset();
        (void)session_.cancel_request(*stream_id);
        pushed->status_ = request_status_type::cancelled;
        reset_stream(*pushed);
        throw;
    }
}

void http3_server_connection::observe_push_cancellation(std::uint64_t push_id) noexcept {
    for (auto& slot : requests_.slots()) {
        if (slot.occupied_ && slot.push_id_ == push_id) {
            slot.push_cancelled_ = true;
        }
    }
}

void http3_server_connection::process_push_cancellations() noexcept {
    for (auto& slot : requests_.slots()) {
        if (slot.occupied_ && slot.push_id_ && slot.push_cancelled_ && slot.entry_ != nullptr &&
            !slot.entry_->retirement_granted_) {
            (void)session_.cancel_request(slot.stream_id_);
            reset_stream(slot);
            cancel_entry(*slot.entry_, request_status_type::cancelled);
        }
    }
}

void http3_server_connection::cancel_push(request_index_slot_type& slot) noexcept {
    // The push stream is terminal at the transport (peer STOP_SENDING or a
    // write timeout); its response handler is cancelled.
    slot.push_cancelled_ = true;
    slot.push_prefix_.reset();
    (void)session_.cancel_request(slot.stream_id_);
    if (slot.entry_ != nullptr) {
        cancel_entry(*slot.entry_, request_status_type::cancelled);
    }
}

}  // namespace ruvia::detail
