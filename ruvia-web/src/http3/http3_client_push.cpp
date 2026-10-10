#include <algorithm>
#include <limits>
#include <stdexcept>

#include "client/http_client_response_decoding.h"
#include "http3/http3_client_connection.h"

namespace ruvia::detail {
http3_client_connection::push_list_type::iterator http3_client_connection::find_push(request_id_type id) noexcept {
    return std::find_if(pushes_.begin(), pushes_.end(), [id](const push_type& push) { return push.id_ == id; });
}
http3_client_connection::push_list_type::iterator http3_client_connection::find_push_by_stream(std::uint64_t stream_id) noexcept {
    return std::find_if(pushes_.begin(), pushes_.end(), [stream_id](const push_type& push) { return push.stream_id_ == stream_id; });
}
void http3_client_connection::settle_push(std::uint64_t push_id) noexcept {
    if (!attempt_) {
        return;
    }
    auto& attempt = *attempt_;
    if (push_id < max_remembered_pushes && !attempt.settled_pushes_.test(push_id)) {
        attempt.settled_pushes_.set(push_id);
        ++attempt.pending_push_credits_;
    }
}
void http3_client_connection::on_push_event(void* raw, const http3_connection_event& event) {
    auto& owner_value = *static_cast<http3_client_connection*>(raw);
    auto& attempt = owner_value.attempt();
    if (!event.push_id_ || *event.push_id_ >= max_remembered_pushes) {
        return;
    }
    const auto push_id = *event.push_id_;
    if (event.kind_ == http3_connection_event_kind::push_stream) {
        std::optional<time_point_type> deadline;
        if (owner_value.push_observer_.config_.timeout_) {
            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration_cast<time_point_type::duration>(*owner_value.push_observer_.config_.timeout_);
            deadline = now > time_point_type::max() - duration ? time_point_type::max() : now + duration;
        }
        attempt.peer_push_streams_.push_back({push_id, event.stream_id_, deadline});
        const auto found = std::find_if(owner_value.pushes_.begin(), owner_value.pushes_.end(),
            [push_id](const push_type& push) { return push.push_id_ == push_id; });
        if (found != owner_value.pushes_.end()) {
            found->stream_id_ = event.stream_id_;
        }
        return;
    }
    if (event.kind_ == http3_connection_event_kind::push_canceled) {
        const auto found = std::find_if(owner_value.pushes_.begin(), owner_value.pushes_.end(),
            [push_id](const push_type& push) { return push.push_id_ == push_id; });
        if (found != owner_value.pushes_.end()) {
            found->peer_cancelled_ = true;
        } else {
            attempt.seen_pushes_.set(push_id);
            owner_value.settle_push(push_id);
        }
        return;
    }
    if (event.kind_ == http3_connection_event_kind::push_promise) {
        if (attempt.seen_pushes_.test(push_id)) {
            return;  // The core already compared repeated promises byte for byte.
        }
        attempt.seen_pushes_.set(push_id);
        if (event.head_ == nullptr) {
            throw std::logic_error("HTTP/3 push promise lacks its parsed request");
        }
        // A node exists before publishing the movable consumer. Allocation or
        // queue rejection remains local to the push and is retired after feed.
        auto& push = owner_value.pushes_.emplace_back();
        push.id_ = ++owner_value.next_request_id_;
        if (push.id_ == 0) {
            push.id_ = ++owner_value.next_request_id_;
        }
        push.push_id_ = push_id;
        if (owner_value.push_observer_.config_.timeout_) {
            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration_cast<time_point_type::duration>(*owner_value.push_observer_.config_.timeout_);
            push.deadline_ = now > time_point_type::max() - duration ? time_point_type::max() : now + duration;
        }
        const auto stream = std::find_if(attempt.peer_push_streams_.begin(), attempt.peer_push_streams_.end(),
            [push_id](const peer_push_stream_type& binding) { return binding.push_id_ == push_id; });
        if (stream != attempt.peer_push_streams_.end()) {
            push.stream_id_ = stream->stream_id_;
            if (stream->deadline_ && (!push.deadline_ || *stream->deadline_ < *push.deadline_)) {
                push.deadline_ = stream->deadline_;
            }
        }
        push.state_ = owner_value.push_observer_.receive_(owner_value.push_observer_.context_, owner_value.push_observer_.connection_slot_,
            owner_value, push.id_, *event.head_);
        if (push.state_ == nullptr) {
            push.cancel_requested_ = true;
            return;
        }
        try {
            push.delivery_.emplace(*push.state_, owner_value.receive_body_budget_);
        } catch (...) {
            push.state_->failure_ = std::current_exception();
            push.cancel_requested_ = true;
        }
        return;
    }
    const auto push = owner_value.find_push_by_stream(event.stream_id_);
    if (push == owner_value.pushes_.end() || !push->delivery_) {
        return;
    }
    const auto sink_value = push->delivery_->event_sink();
    sink_value.callback_(sink_value.context_, event);
}

void http3_client_connection::finish_push(push_list_type::iterator found, outcome_type outcome) {
    auto& push = *found;
    // Only the sole driver stops stream delivery, before releasing the parser
    // and the borrowed response sink. SSL_free stops a peer UNI receive half.
    auto* session = live_session();
    if (push.stream_id_) {
        if (session != nullptr) {
            const auto closed = session->transport().close_stream(*push.stream_id_);
            if (closed != ruvia::quic_operation_status::accepted &&
                closed != ruvia::quic_operation_status::completed &&
                closed != ruvia::quic_operation_status::retired) {
                throw std::runtime_error("HTTP/3 push receive stream could not retire");
            }
            if (!attempt_->engine_.retire_push_stream(*push.stream_id_)) {
                throw std::runtime_error("HTTP/3 push parser could not retire");
            }
        }
        if (attempt_) {
            attempt_->receiver_.retire(*push.stream_id_);
            std::erase(attempt_->peer_streams_, *push.stream_id_);
            std::erase_if(attempt_->peer_push_streams_, [&push](const peer_push_stream_type& binding) { return binding.push_id_ == push.push_id_; });
        }
    } else if (outcome != outcome_type::complete && !push.peer_cancelled_ && session != nullptr) {
        attempt_->cancelled_pushes_.set(push.push_id_);
    }
    auto* state_value = push.state_;
    if (state_value != nullptr) {
        auto error = outcome == outcome_type::cancelled            ? http_client_error::code_type::cancelled
                     : outcome == outcome_type::deadline           ? http_client_error::code_type::timeout
                     : outcome == outcome_type::response_too_large ? http_client_error::code_type::response_too_large
                     : outcome == outcome_type::transport_error    ? http_client_error::code_type::io_error
                                                                   : http_client_error::code_type::protocol_error;
        if (push.delivery_) {
            if (push.delivery_->callback_failure()) {
                (void)push.delivery_->commit_failure(push.delivery_->callback_failure());
            } else if (push.delivery_->retirement_reason() != http3_client_response_delivery::retirement_reason_type::none) {
                error = push.delivery_->retirement_reason() == http3_client_response_delivery::retirement_reason_type::response_too_large
                            ? http_client_error::code_type::response_too_large
                            : http_client_error::code_type::protocol_error;
                (void)push.delivery_->commit_retirement_failure(error);
            } else if (outcome == outcome_type::complete) {
                try {
                    const auto plan = push.delivery_->response_body_plan();
                    // Only a decode-required body is withheld from incremental
                    // readers; a streamed body may be borrowed across a pipe write.
                    const bool body_withheld = state_value->body_decode_required_;
                    decode_http_client_response_content_encoding(*state_value,
                        plan && plan->content_semantics() == http_response_content_semantics_type::with_content,
                        max_response_bytes_);
                    if (push.deadline_ && std::chrono::steady_clock::now() >= *push.deadline_) {
                        if (body_withheld) {
                            state_value->discard_response_body();
                        }
                        (void)push.delivery_->commit_terminal_error(http_client_error::code_type::timeout);
                    } else if (push.delivery_->commit_complete() != http3_client_response_delivery::commit_status_type::committed) {
                        (void)push.delivery_->commit_terminal_error(http_client_error::code_type::protocol_error);
                    }
                } catch (...) {
                    (void)push.delivery_->commit_failure(std::current_exception());
                }
            } else {
                (void)push.delivery_->commit_terminal_error(error);
            }
        } else {
            state_value->error_code_ = static_cast<std::uint8_t>(error);
            state_value->complete_ = true;
            state_value->head_signal_.notify();
            state_value->data_signal_.notify();
            state_value->space_signal_.notify();
        }
        // Results and body reservations belong to the pool's separate memory
        // and budget domains and can outlive this connection or the client.
        state_value->transport_ = http_client_response_transport::unassigned;
        state_value->http3_connection_ = nullptr;
        state_value->http3_request_id_ = 0;
        state_value->request_id_ = 0;
        push.delivery_.reset();
        push.state_ = nullptr;
        state_value->release_reference();
        push_observer_.finished_(push_observer_.context_);
    }
    settle_push(push.push_id_);
    pushes_.erase(found);
}

bool http3_client_connection::sweep_pushes() {
    bool progress_value = false;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pushes_.begin(); it != pushes_.end();) {
        auto current = it++;
        if (current->cancel_requested_ || current->peer_cancelled_ ||
            (current->state_ != nullptr && current->state_->abandoned_)) {
            finish_push(current, outcome_type::cancelled);
            progress_value = true;
        } else if (current->deadline_ && now >= *current->deadline_) {
            finish_push(current, outcome_type::deadline);
            progress_value = true;
        } else if (current->delivery_ && current->delivery_->retirement_reason() != http3_client_response_delivery::retirement_reason_type::none) {
            finish_push(current, outcome_type::protocol_error);
            progress_value = true;
        }
    }
    auto& attempt = this->attempt();
    auto& peer_push_streams = attempt.peer_push_streams_;
    for (std::size_t index = 0; index < peer_push_streams.size();) {
        const auto binding = peer_push_streams[index];
        if (find_push_by_stream(binding.stream_id_) == pushes_.end() &&
            (attempt.settled_pushes_.test(binding.push_id_) || (binding.deadline_ && now >= *binding.deadline_))) {
            if (auto* session = attempt.session_.get()) {
                const auto closed = session->transport().close_stream(binding.stream_id_);
                if ((closed != ruvia::quic_operation_status::accepted &&
                        closed != ruvia::quic_operation_status::completed &&
                        closed != ruvia::quic_operation_status::retired) ||
                    !attempt.engine_.retire_push_stream(binding.stream_id_)) {
                    throw std::runtime_error("HTTP/3 unpromised push stream could not retire");
                }
            }
            attempt.receiver_.retire(binding.stream_id_);
            std::erase(attempt.peer_streams_, binding.stream_id_);
            peer_push_streams.erase(peer_push_streams.begin() + static_cast<std::ptrdiff_t>(index));
            attempt.seen_pushes_.set(binding.push_id_);
            settle_push(binding.push_id_);
            progress_value = true;
        } else {
            ++index;
        }
    }
    return progress_value;
}

bool http3_client_connection::flush_push_control() {
    if (push_observer_.receive_ == nullptr) {
        return false;
    }
    auto& attempt = this->attempt();
    bool progress_value = false;
    for (std::size_t id = 0; id < attempt.cancelled_pushes_.size(); ++id) {
        if (attempt.cancelled_pushes_.test(id)) {
            if (!attempt.engine_.queue_cancel_push(id)) {
                return progress_value;
            }
            attempt.cancelled_pushes_.reset(id);
            progress_value = true;
        }
    }
    // Remembered promise metadata is deliberately finite per connection. It
    // cannot be forgotten while a valid repeated promise may still arrive.
    while (attempt.pending_push_credits_ != 0 && attempt.authorized_push_id_ + 1 < max_remembered_pushes) {
        if (!attempt.engine_.queue_max_push_id(attempt.authorized_push_id_ + 1)) {
            break;
        }
        ++attempt.authorized_push_id_;
        --attempt.pending_push_credits_;
        progress_value = true;
    }
    return progress_value;
}
}  // namespace ruvia::detail
