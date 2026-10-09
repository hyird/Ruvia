#include "http3/http3_client_response_delivery.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_response_decoding.h"

namespace ruvia::detail {

http3_client_response_event_sink http3_client_response_delivery::event_sink() noexcept {
    return {.callback_ = on_event, .context_ = this};
}

http3_client_response_delivery::read_allowance_type http3_client_response_delivery::read_allowance(
    std::size_t max_input_bytes) const noexcept {
    if (state_.complete_) {
        return {.status_ = read_status_type::terminal};
    }
    if (retirement_reason_ != retirement_reason_type::none) {
        return {.status_ = read_status_type::retirement_required};
    }
    if (max_input_bytes == 0) {
        return {.status_ = read_status_type::backpressured};
    }
    const auto local_capacity = remaining_body_capacity();
    const auto shared_capacity = state_.producer_body_budget_available();
    const auto capacity = std::min({local_capacity, shared_capacity, max_input_bytes});
    if (state_.collect_all_ && shared_capacity == 0) {
        // One wire byte at a time lets the parser reach trailers or FIN without
        // admitting body storage past the shared cap. A DATA event with no
        // reservation is retired locally by deliver().
        return {.status_ = read_status_type::ready, .bytes_ = 1};
    }
    if (state_.collect_all_ && local_capacity <= shared_capacity) {
        // Probe only when this response's own limit is binding. A partially
        // available shared budget must not be misreported as local overflow.
        return {.status_ = read_status_type::ready,
            .bytes_ = local_capacity < max_input_bytes ? local_capacity + 1 : max_input_bytes};
    }
    if (capacity == 0) {
        return {.status_ = read_status_type::backpressured};
    }
    return {.status_ = read_status_type::ready, .bytes_ = capacity};
}

http3_client_response_delivery::commit_status_type http3_client_response_delivery::commit(
    const http3_client_receive_driver::result_type& result_value) noexcept {
    if (state_.complete_) {
        return commit_status_type::already_committed;
    }
    if (result_value.status_ == http3_client_receive_driver::status_type::connection_error) {
        commit_error(http_client_error::code_type::protocol_error);
        return commit_status_type::committed;
    }
    if (result_value.status_ == http3_client_receive_driver::status_type::transport_error) {
        commit_error(http_client_error::code_type::io_error);
        return commit_status_type::committed;
    }
    if (retirement_reason_ != retirement_reason_type::none) {
        return commit_status_type::retirement_required;
    }

    switch (result_value.status_) {
        case http3_client_receive_driver::status_type::blocked:
        case http3_client_receive_driver::status_type::progress:
            return commit_status_type::pending;
        case http3_client_receive_driver::status_type::response_complete:
            return commit_complete();
        case http3_client_receive_driver::status_type::stream_reset:
        case http3_client_receive_driver::status_type::peer_stream_ended:
        case http3_client_receive_driver::status_type::stream_error:
            commit_error(http_client_error::code_type::protocol_error);
            return commit_status_type::committed;
        case http3_client_receive_driver::status_type::connection_error:
        case http3_client_receive_driver::status_type::transport_error:
            break;
    }
    return commit_status_type::pending;
}

http3_client_response_delivery::commit_status_type http3_client_response_delivery::commit_complete() noexcept {
    if (state_.complete_) {
        return commit_status_type::already_committed;
    }
    if (!state_.head_ready_ || retirement_reason_ != retirement_reason_type::none) {
        return commit_status_type::retirement_required;
    }
    commit_terminal();
    return commit_status_type::committed;
}

bool http3_client_response_delivery::commit_terminal_error(http_client_error::code_type error) noexcept {
    if (state_.complete_) {
        return false;
    }
    commit_error(error);
    return true;
}

bool http3_client_response_delivery::commit_retirement_failure(
    http_client_error::code_type error) noexcept {
    if (state_.complete_ || retirement_reason_ == retirement_reason_type::none) {
        return false;
    }
    if (state_.collect_all_ || state_.body_decode_required_) {
        state_.discard_response_body();
    } else {
        state_.discard_pending_body();
    }
    commit_error(error);
    return true;
}

bool http3_client_response_delivery::commit_failure(std::exception_ptr failure) noexcept {
    if (state_.complete_) {
        return false;
    }
    if (failure == nullptr) {
        std::terminate();
    }
    if (state_.body_decode_required_) {
        state_.discard_response_body();
    } else {
        state_.discard_pending_body();
    }
    state_.failure_ = std::move(failure);
    commit_terminal();
    return true;
}

void http3_client_response_delivery::on_event(void* context_value, const http3_connection_event& event) {
    auto& self = *static_cast<http3_client_response_delivery*>(context_value);
    try {
        self.deliver(event);
    } catch (...) {
        // An allocation failure is local to the response stream. Never let it
        // escape http3_connection::feed(), which would fail sibling streams.
        if (self.callback_failure_ == nullptr) {
            self.callback_failure_ = std::current_exception();
        }
        self.request_retirement(retirement_reason_type::callback_failure);
    }
}

void http3_client_response_delivery::deliver(const http3_connection_event& event) {
    if (state_.complete_ || retirement_reason_ != retirement_reason_type::none) {
        return;
    }
    switch (event.kind_) {
        case http3_connection_event_kind::push_stream:
        case http3_connection_event_kind::push_promise:
        case http3_connection_event_kind::push_canceled:
        case http3_connection_event_kind::priority_update:
        case http3_connection_event_kind::origin_advertisement:
            return;
        case http3_connection_event_kind::final_head: {
            if (event.head_ == nullptr || state_.head_ready_) {
                request_retirement(retirement_reason_type::protocol_error);
                return;
            }
            const auto status = http_status_code::try_from_value(event.head_->status_);
            if (!status) {
                request_retirement(retirement_reason_type::protocol_error);
                return;
            }
            if (!event.response_body_plan_) {
                request_retirement(retirement_reason_type::protocol_error);
                return;
            }
            response_body_plan_ = event.response_body_plan_;
            state_.response_body_plan_ = response_body_plan_;
            state_.headers_.clear();
            state_.headers_.reserve(event.head_->headers_.size());
            for (const auto& field : event.head_->headers_) {
                state_.headers_.push_back(
                    http_header::copy_of(field.name_, field.value_, state_.resource_));
            }
            state_.status_ = *status;
            state_.protocol_version_ = http_protocol_version::http3;
            if (state_.tunnel_) {
                state_.tunnel_->accepted_ = response_body_plan_->content_semantics() == http_response_content_semantics_type::connect_tunnel;
                if (state_.tunnel_->accepted_ && state_.tunnel_->udp_) {
                    std::pmr::vector<http_header_view> fields_value(state_.resource_);
                    for (const auto& field : state_.headers_) {
                        fields_value.emplace_back(field.name(), field.value());
                    }
                    if ((validate_http_connect_udp_response(state_.protocol_version_, state_.status_.value(), fields_value).index() != 0)) {
                        request_retirement(retirement_reason_type::protocol_error);
                        return;
                    }
                }
                if (!state_.tunnel_->accepted_) {
                    state_.tunnel_->output_.stop();
                }
            }
            if (!state_.tunnel_ || !state_.tunnel_->accepted_) {
                configure_http_client_response_decoding(state_);
            }
            state_.head_ready_ = true;
            state_.head_signal_.notify();
            return;
        }
        case http3_connection_event_kind::body:
        case http3_connection_event_kind::tunnel_data:
            if (event.kind_ == http3_connection_event_kind::tunnel_data ? (!state_.tunnel_ || !state_.tunnel_->accepted_ || !response_body_plan_ || response_body_plan_->content_semantics() != http_response_content_semantics_type::connect_tunnel) : (response_body_plan_ && (response_body_plan_->body_suppressed() || !response_body_plan_->status_allows_body() || response_body_plan_->content_semantics() != http_response_content_semantics_type::with_content))) {
                request_retirement(retirement_reason_type::protocol_error);
                return;
            }
            if (event.body_.size() > remaining_body_capacity() ||
                !state_.retain_producer_body_bytes(event.body_.size())) {
                request_retirement(retirement_reason_type::response_too_large);
                return;
            }
            if (!event.body_.empty()) {
                try {
                    state_.pending_.append(event.body_.data(), event.body_.size());
                } catch (...) {
                    state_.release_producer_body_bytes(event.body_.size());
                    throw;
                }
                state_.data_signal_.notify();
            }
            return;
        case http3_connection_event_kind::trailer_field:
            state_.trailers_.push_back(http_header::copy_of(
                event.trailer_.name_, event.trailer_.value_, state_.resource_));
            return;
        case http3_connection_event_kind::message_end:
            if (event.response_body_plan_) {
                response_body_plan_ = event.response_body_plan_;
            }
            return;
        case http3_connection_event_kind::informational_head: {
            if (event.head_ == nullptr) {
                request_retirement(retirement_reason_type::protocol_error);
                return;
            }
            std::pmr::vector<http_header_view> fields_value(state_.resource_);
            for (const auto& field : event.head_->headers_) {
                fields_value.emplace_back(field.name_, field.value_);
            }
            state_.retain_informational(http_status_code::from_value(event.head_->status_), fields_value);
            return;
        }
        case http3_connection_event_kind::request_head:
        case http3_connection_event_kind::reset:
            return;
    }
}

void http3_client_response_delivery::commit_error(http_client_error::code_type error) noexcept {
    if (state_.body_decode_required_) {
        state_.discard_response_body();
    }
    state_.error_code_ = static_cast<std::uint8_t>(error);
    commit_terminal();
}

void http3_client_response_delivery::commit_terminal() noexcept {
    state_.complete_ = true;
    state_.head_signal_.notify();
    state_.data_signal_.notify();
}

void http3_client_response_delivery::request_retirement(retirement_reason_type reason) noexcept {
    if (retirement_reason_ == retirement_reason_type::none) {
        retirement_reason_ = reason;
    }
}

void http3_client_response_delivery::reconcile_body_bytes() noexcept {
    state_.reconcile_producer_body_bytes();
}

std::size_t http3_client_response_delivery::remaining_body_capacity() const noexcept {
    const auto limit = state_.buffered_limit_;
    const auto retained = state_.producer_body_bytes();
    if (retained >= limit) {
        return 0;
    }
    return limit - retained;
}

}  // namespace ruvia::detail
