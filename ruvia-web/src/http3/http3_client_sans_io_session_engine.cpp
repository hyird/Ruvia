#include "http3/http3_client_sans_io_session_engine.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http3_peer_streams.h"

namespace ruvia::detail {
namespace {
http3_client_sans_io_session_result connection_error(http3_connection_error_code code) noexcept {
    return {http3_client_sans_io_session_status::connection_error,
        http3_connection_error_scope::connection, code};
}

std::pmr::memory_resource* checked_resource(std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("HTTP/3 client session requires its worker memory resource");
    }
    return resource;
}
}  // namespace

http3_client_sans_io_session_engine::http3_client_sans_io_session_engine(
    std::pmr::memory_resource* worker_resource, limits_type limits)
    : resource_(checked_resource(worker_resource)),
      limits_(limits),
      local_body_budget_(limits.max_total_body_bytes_),
      body_budget_(&local_body_budget_),
      connection_(http3_peer_role::client, resource_, limits.connection_),
      control_output_(resource_),
      responses_(resource_) {
    if (limits_.max_live_streams_ == 0) {
        throw std::invalid_argument("HTTP/3 response session must allow at least one live stream");
    }
}

http3_client_sans_io_session_engine::http3_client_sans_io_session_engine(
    std::pmr::memory_resource* worker_resource, http3_client_body_budget& body_budget, limits_type limits)
    : http3_client_sans_io_session_engine(worker_resource, limits) {
    body_budget_ = &body_budget;
}

http3_client_sans_io_session_engine::~http3_client_sans_io_session_engine() {
    body_budget_->release(retained_body_bytes_);
}

bool http3_client_sans_io_session_engine::queue_priority_update(std::uint64_t stream_id, http_priority priority) {
    if (feeding_ || connection_failure_.scope_ != http3_connection_error_scope::none ||
        !responses_.contains(stream_id) || responses_.at(stream_id).terminal_ || priority.urgency_ > 7) {
        return false;
    }
    auto frame = connection_.prepare_priority_update({.element_id_ = stream_id,
        .fields_ = {.urgency_ = priority.urgency_, .incremental_ = priority.incremental_}});
    if ((frame.index() != 0) || std::get<0>(frame).size() > max_http_header_bytes - std::min(control_output_.size(), max_http_header_bytes)) {
        return false;
    }
    control_output_.append(std::get<0>(frame).data(), std::get<0>(frame).size());
    return true;
}

bool http3_client_sans_io_session_engine::queue_max_push_id(std::uint64_t maximum) {
    if (feeding_ || connection_failure_.scope_ != http3_connection_error_scope::none || control_output_.size() > max_http_header_bytes - 24) {
        return false;
    }
    // Reserve before the protocol core commits authorization. Appending the
    // complete bounded control frame cannot allocate after that commit.
    control_output_.reserve(control_output_.size() + 24);
    auto frame = connection_.prepare_max_push_id(maximum);
    if ((frame.index() != 0)) {
        return false;
    }
    control_output_.append(std::get<0>(frame).data(), std::get<0>(frame).size());
    return true;
}

bool http3_client_sans_io_session_engine::queue_cancel_push(std::uint64_t push_id) {
    if (feeding_ || connection_failure_.scope_ != http3_connection_error_scope::none || control_output_.size() > max_http_header_bytes - 24) {
        return false;
    }
    control_output_.reserve(control_output_.size() + 24);
    auto frame = connection_.prepare_cancel_push(push_id);
    if ((frame.index() != 0)) {
        return false;
    }
    control_output_.append(std::get<0>(frame).data(), std::get<0>(frame).size());
    return true;
}

bool http3_client_sans_io_session_engine::queue_push_priority_update(std::uint64_t push_id, http_priority priority) {
    if (feeding_ || connection_failure_.scope_ != http3_connection_error_scope::none || priority.urgency_ > 7) {
        return false;
    }
    auto frame = connection_.prepare_priority_update({.element_id_ = push_id, .push_ = true, .fields_ = {.urgency_ = priority.urgency_, .incremental_ = priority.incremental_}});
    if ((frame.index() != 0) || std::get<0>(frame).size() > max_http_header_bytes - std::min(control_output_.size(), max_http_header_bytes)) {
        return false;
    }
    control_output_.append(std::get<0>(frame).data(), std::get<0>(frame).size());
    return true;
}

bool http3_client_sans_io_session_engine::retire_push_stream(std::uint64_t stream_id) {
    if (feeding_) {
        return false;
    }
    const auto result_value = connection_.cancel_request(stream_id);
    // A peer FIN/reset may have already released the push parser.
    return result_value.scope_ == http3_connection_error_scope::none ||
           (result_value.scope_ == http3_connection_error_scope::stream && result_value.code_ == http3_connection_error_code::stream_creation_error);
}

bool http3_client_sans_io_session_engine::consume_control_output(std::size_t bytes_value) noexcept {
    if (bytes_value > control_output_.size()) {
        return false;
    }
    control_output_.erase(0, bytes_value);
    if (control_output_.empty()) {
        std::pmr::string(resource_).swap(control_output_);
    }
    return true;
}

http3_client_sans_io_session_engine::result_type http3_client_sans_io_session_engine::register_request(
    std::uint64_t stream_id, http_known_method method, http3_client_response_event_sink sink_value) {
    if (feeding_) {
        return {http3_client_sans_io_session_status::invalid_state};
    }
    if (connection_failure_.scope_ == http3_connection_error_scope::connection) {
        return connection_failure_;
    }
    if (responses_.contains(stream_id)) {
        return {http3_client_sans_io_session_status::invalid_state};
    }
    if (responses_.size() >= limits_.max_live_streams_) {
        return {http3_client_sans_io_session_status::stream_limit_exceeded,
            http3_connection_error_scope::stream, http3_connection_error_code::excessive_load};
    }
    auto [entry_value, inserted] = responses_.try_emplace(stream_id, resource_);
    if (!inserted) {
        return {http3_client_sans_io_session_status::invalid_state};
    }
    entry_value->second.sink_ = sink_value;
    try {
        const auto registered = connection_.register_client_request(stream_id, method);
        if (registered.scope_ != http3_connection_error_scope::none) {
            responses_.erase(entry_value);
            const auto failure = from_connection(registered);
            if (failure.scope_ == http3_connection_error_scope::connection) {
                fail_connection(failure);
            }
            return failure;
        }
    } catch (...) {
        responses_.erase(entry_value);
        fail_connection(connection_error(http3_connection_error_code::internal_error));
        throw;
    }
    return {};
}

http3_client_sans_io_session_engine::result_type http3_client_sans_io_session_engine::feed(
    std::uint64_t stream_id, std::span<const char> bytes_value, bool fin, bool reset) {
    if (feeding_) {
        return {http3_client_sans_io_session_status::invalid_state};
    }
    if (connection_failure_.scope_ == http3_connection_error_scope::connection) {
        return connection_failure_;
    }
    auto found = responses_.find(stream_id);
    if (is_http3_request_stream_id(stream_id) &&
        (found == responses_.end() || found->second.terminal_)) {
        return {http3_client_sans_io_session_status::invalid_state};
    }
    feeding_ = true;
    struct feed_guard final {
        bool& feeding_;
        ~feed_guard() {
            feeding_ = false;
        }
    } guard_value{feeding_};
    try {
        const auto result_value = connection_.feed(stream_id, bytes_value, fin, reset, on_event, this);
        if (result_value.scope_ == http3_connection_error_scope::connection) {
            fail_connection(from_connection(result_value));
            return connection_failure_;
        }
        if (found == responses_.end()) {
            return from_connection(result_value);  // Peer control/QPACK or invalid peer stream.
        }
        auto& stored = found->second;
        if (stored.limit_exceeded_) {
            // A later stream error in the same feed must not hide a body
            // overflow that requires closing the entire connection. Only a
            // protocol connection error has higher priority.
            fail_connection(connection_error(http3_connection_error_code::excessive_load));
            stored.result_ = {http3_client_sans_io_session_status::body_limit_exceeded,
                http3_connection_error_scope::connection, http3_connection_error_code::excessive_load};
            return stored.result_;
        }
        if (result_value.scope_ == http3_connection_error_scope::stream) {
            stored.result_ = from_connection(result_value);
            stored.terminal_ = true;
            return stored.result_;
        }
        if (result_value.status_ == http3_connection_status::message_end) {
            stored.complete_ = true;
            stored.terminal_ = true;
            stored.result_ = {http3_client_sans_io_session_status::message_end};
            return stored.result_;
        }
        if (result_value.status_ == http3_connection_status::reset) {
            stored.result_ = {http3_client_sans_io_session_status::reset};
            stored.reset_ = true;
            stored.terminal_ = true;
            return stored.result_;
        }
        return from_connection(result_value);
    } catch (...) {
        fail_connection(connection_error(http3_connection_error_code::internal_error));
        throw;
    }
}

void http3_client_sans_io_session_engine::on_event(void* context_value, const http3_connection_event& event) {
    auto& self = *static_cast<http3_client_sans_io_session_engine*>(context_value);
    if (event.kind_ == http3_connection_event_kind::origin_advertisement && self.origin_observer_.callback_ != nullptr) {
        self.origin_observer_.callback_(self.origin_observer_.context_, event);
        return;
    }
    if (event.push_id_) {
        if (self.push_observer_.callback_ != nullptr) {
            self.push_observer_.callback_(self.push_observer_.context_, event);
        }
        return;
    }
    auto found = self.responses_.find(event.stream_id_);
    if (found == self.responses_.end()) {
        return;
    }
    auto& response = found->second;
    switch (event.kind_) {
        case http3_connection_event_kind::final_head:
            if (event.head_ == nullptr) {
                return;
            }
            response.response_body_plan_ = event.response_body_plan_;
            response.status_ = event.head_->status_;
            response.headers_.clear();
            response.headers_.reserve(event.head_->headers_.size());
            for (const auto& field : event.head_->headers_) {
                http3_client_sans_io_response_header copied(self.resource_);
                copied.name_.assign(field.name_);
                copied.value_.assign(field.value_);
                response.headers_.push_back(std::move(copied));
            }
            response.final_head_seen_ = true;
            if (response.sink_.callback_) {
                response.sink_.callback_(response.sink_.context_, event);
            }
            return;
        case http3_connection_event_kind::trailer_field: {
            http3_client_sans_io_response_header copied(self.resource_);
            copied.name_.assign(event.trailer_.name_);
            copied.value_.assign(event.trailer_.value_);
            response.trailers_.push_back(std::move(copied));
            if (response.sink_.callback_) {
                response.sink_.callback_(response.sink_.context_, event);
            }
            return;
        }
        case http3_connection_event_kind::body:
        case http3_connection_event_kind::tunnel_data: {
            if (response.limit_exceeded_) {
                return;
            }
            if (response.sink_.callback_) {
                response.sink_.callback_(response.sink_.context_, event);
                return;
            }
            const auto size = event.body_.size();
            if (size > self.limits_.max_body_bytes_per_stream_ -
                           std::min(response.body_.size(), self.limits_.max_body_bytes_per_stream_) ||
                size > self.limits_.max_total_body_bytes_ -
                           std::min(self.retained_body_bytes_, self.limits_.max_total_body_bytes_) ||
                !self.body_budget_->try_retain(size)) {
                response.result_ = {http3_client_sans_io_session_status::body_limit_exceeded,
                    http3_connection_error_scope::stream, http3_connection_error_code::excessive_load};
                response.limit_exceeded_ = true;
                return;
            }
            try {
                response.body_.insert(response.body_.end(), event.body_.begin(), event.body_.end());
            } catch (...) {
                self.body_budget_->release(size);
                throw;
            }
            self.retained_body_bytes_ += size;
            return;
        }
        case http3_connection_event_kind::message_end:
            // The final plan is a value from the protocol layer and is repeated
            // unchanged here. Do not let an absent value erase the observed head.
            if (event.response_body_plan_) {
                response.response_body_plan_ = event.response_body_plan_;
            }
            // Feed can still report a later connection error for the same
            // input; only the successful return publishes a complete response.
            return;
        case http3_connection_event_kind::reset:
            // Publish only after feed() confirms there was no higher-priority
            // connection error in this same input batch.
            return;
        case http3_connection_event_kind::informational_head:
            if (response.sink_.callback_) {
                response.sink_.callback_(response.sink_.context_, event);
            }
            return;
        case http3_connection_event_kind::push_stream:
        case http3_connection_event_kind::push_promise:
        case http3_connection_event_kind::push_canceled:
        case http3_connection_event_kind::origin_advertisement:
        case http3_connection_event_kind::priority_update:
        case http3_connection_event_kind::request_head:
            return;
    }
}

std::optional<http3_client_sans_io_response_view> http3_client_sans_io_session_engine::response(
    std::uint64_t stream_id) const noexcept {
    const auto found = responses_.find(stream_id);
    if (found == responses_.end() || !found->second.terminal_) {
        return std::nullopt;
    }
    const auto& response = found->second;
    return http3_client_sans_io_response_view{response.status_, response.response_body_plan_,
        response.headers_, response.trailers_, response.body_, response.complete_,
        response.reset_, response.result_};
}

std::optional<std::pmr::string> http3_client_sans_io_session_engine::take_body(std::uint64_t stream_id) {
    if (feeding_) {
        return std::nullopt;
    }
    const auto found = responses_.find(stream_id);
    if (found == responses_.end()) {
        return std::nullopt;
    }
    auto& stored = found->second;
    if (!stored.complete_ || stored.body_transferred_ || stored.sink_.callback_) {
        return std::nullopt;
    }
    const auto bytes_value = stored.body_.size();
    std::optional<std::pmr::string> body(std::in_place, std::move(stored.body_));
    stored.body_.clear();
    stored.body_transferred_ = true;
    retained_body_bytes_ -= bytes_value;
    body_budget_->release(bytes_value);
    return body;
}

bool http3_client_sans_io_session_engine::release(std::uint64_t stream_id) noexcept {
    if (feeding_) {
        return false;
    }
    const auto found = responses_.find(stream_id);
    if (found == responses_.end() || !found->second.terminal_) {
        return false;
    }
    retained_body_bytes_ -= found->second.body_.size();
    body_budget_->release(found->second.body_.size());
    responses_.erase(found);
    return true;
}

bool http3_client_sans_io_session_engine::cancel_request(std::uint64_t stream_id) noexcept {
    if (feeding_ || connection_failure_.scope_ == http3_connection_error_scope::connection) {
        return false;
    }
    const auto found = responses_.find(stream_id);
    if (found == responses_.end() || found->second.terminal_) {
        return false;
    }
    if (!connection_.retire_client_request(stream_id)) {
        fail_connection(connection_error(http3_connection_error_code::internal_error));
        return false;
    }
    found->second.terminal_ = true;
    found->second.result_ = {http3_client_sans_io_session_status::local_cancelled};
    return true;
}

void http3_client_sans_io_session_engine::fail_connection(result_type failure) noexcept {
    if (connection_failure_.scope_ == http3_connection_error_scope::connection) {
        return;
    }
    connection_failure_ = failure;
    for (auto& [id, response] : responses_) {
        (void)id;
        if (!response.terminal_) {
            response.result_ = failure;
            response.terminal_ = true;
        }
    }
}

std::size_t http3_client_sans_io_session_engine::live_stream_count() const noexcept {
    return responses_.size();
}

std::size_t http3_client_sans_io_session_engine::retained_body_bytes() const noexcept {
    return retained_body_bytes_;
}

http3_client_sans_io_session_engine::result_type http3_client_sans_io_session_engine::stop() noexcept {
    if (feeding_) {
        return {http3_client_sans_io_session_status::invalid_state};
    }
    (void)connection_.retire();
    std::pmr::string(resource_).swap(control_output_);
    fail_connection({http3_client_sans_io_session_status::transport_error,
        http3_connection_error_scope::connection, http3_connection_error_code::no_error});
    return connection_failure_;
}

http3_client_sans_io_session_engine::result_type http3_client_sans_io_session_engine::from_connection(
    http3_connection_result result_value) const noexcept {
    switch (result_value.status_) {
        case http3_connection_status::qpack_blocked:
            return {http3_client_sans_io_session_status::qpack_blocked, result_value.scope_, result_value.code_, result_value.consumed_bytes_};
        case http3_connection_status::push_promise_pending:
            return {http3_client_sans_io_session_status::push_promise_pending, result_value.scope_, result_value.code_, result_value.consumed_bytes_};
        case http3_connection_status::need_more_data:
            return {http3_client_sans_io_session_status::need_more_data, result_value.scope_, result_value.code_, result_value.consumed_bytes_};
        case http3_connection_status::message_end:
            return {http3_client_sans_io_session_status::message_end, result_value.scope_, result_value.code_};
        case http3_connection_status::reset:
            return {http3_client_sans_io_session_status::reset, result_value.scope_, result_value.code_};
        case http3_connection_status::stream_error:
            return {http3_client_sans_io_session_status::stream_error, result_value.scope_, result_value.code_};
        case http3_connection_status::connection_error:
            return {http3_client_sans_io_session_status::connection_error, result_value.scope_, result_value.code_};
    }
    return {};
}

}  // namespace ruvia::detail
