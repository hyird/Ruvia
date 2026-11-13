#include <stdexcept>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_output_operation.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "integration/operation_lane_lease.h"
namespace ruvia::detail {
struct http_client_upload_end_input final {
    http_client_response pin_;
    std::vector<http_header> fields_;
};

task<http_client_response> http_client_response_handoff::receive(
    http_client_response pin, std::string_view head_message, operation_options operation) {
    return ::ruvia::make_scoped_operation(scope_, receive_owned(std::move(pin), head_message, std::move(operation)));
}

task<http_client_response> http_client_response_handoff::receive_owned(
    http_client_response pin, std::string_view head_message, operation_options operation) {
    // `pin` keeps the response state, and with it this handoff, alive.
    auto activity = claim_operation_lane(active_, "HTTP response receive is already active");
    if (taken_) {
        throw std::logic_error("HTTP response has already been transferred");
    }
    const operation_deadline redirect_deadline(
        operation.timeout_.has_value()
            ? operation.timeout_
            : (pin.state_->upload_ && pin.state_->upload_->redirect_request_ &&
                          pin.state_->pool_ != nullptr
                      ? std::optional<operation_timeout>(
                            pin.state_->pool_->config().request_timeout_)
                      : std::nullopt));
    {
        const http_client_operation_interruption interruption(
            *pin.state_, http_client_operation_lane::read, operation);
        co_await pin.state_->await_head(head_message);
    }
    taken_ = true;
    pin.consumer_ = true;
    if (pin.state_->upload_ && pin.state_->upload_->redirect_request_) {
        auto redirect_request = std::move(*pin.state_->upload_->redirect_request_);
        pin.state_->upload_->redirect_request_.reset();
        co_return co_await http_client_request::follow_redirects(
            std::move(redirect_request), std::move(pin), redirect_deadline, true);
    }
    co_return std::move(pin);
}

void http_client_response_handoff::release(http_client_response& owner) noexcept {
    scope_.close();
    if (taken_) {
        owner.consumer_ = false;
    }
}
}  // namespace ruvia::detail
namespace ruvia {
namespace {
void require_writable(detail::http_client_response_state& state_value) {
    state_value.require_owner_worker("HTTP exchange must run on its owner worker");
    if (!state_value.upload_ || state_value.upload_->output_.stopped_ || state_value.upload_->output_.ended_ || state_value.upload_->output_.end_requested_) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload is no longer writable");
    }
}
struct upload_output_policy final {
    static detail::http_client_output_queue& output(detail::http_client_response_state& state_value) noexcept {
        return state_value.upload_->output_;
    }
    static void require_write(detail::http_client_response_state& state_value) {
        auto& queue = output(state_value);
        if (queue.stopped_ || queue.end_requested_) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload stopped");
        }
    }
    static bool begin_finish(detail::http_client_response_state& state_value, detail::http_client_upload_end_input& input) {
        require_write(state_value);
        state_value.upload_->trailers_ = std::move(input.fields_);
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::http_client_response_state& state_value) {
        if (state_value.upload_->failure_) {
            std::rethrow_exception(state_value.upload_->failure_);
        }
        if (state_value.failure_) {
            std::rethrow_exception(state_value.failure_);
        }
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload stopped");
    }
};
}  // namespace

http_client_exchange::http_client_exchange(http_client_response response) noexcept
    : response_(std::move(response)),
      body_(response_.state_) {}
http_client_exchange::http_client_exchange(http_client_exchange&& other) noexcept
    : response_(std::move(other.response_)),
      body_(response_.state_) {
    other.body_.state_ = nullptr;
}
http_client_exchange& http_client_exchange::operator=(http_client_exchange&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
        body_.state_ = response_.state_;
        other.body_.state_ = nullptr;
    }
    return *this;
}
http_client_exchange::~http_client_exchange() {
    release();
}
void http_client_exchange::release() noexcept {
    auto* state_value = response_.state_;
    if (state_value == nullptr) {
        return;
    }
    auto& upload = *state_value->upload_;
    const bool abandon_upload = !upload.output_.ended_ && !upload.output_.stopped_;
    upload.output_.output_scope_.close();
    upload.response_.release(response_);
    if (abandon_upload && upload.response_.taken() &&
        !state_value->complete_ && state_value->pool_ != nullptr) {
        // Keep the retained consumer's read scope open to observe cancellation.
        state_value->pool_->abandon_response(*state_value);
    } else if (!upload.output_.ended_) {
        upload.output_.stop();
    }
    response_.release();
    body_.state_ = nullptr;
}

task<void> http_client_request_body_writer::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()), std::move(operation));
}
task<void> http_client_request_body_writer::write(std::string_view bytes_value, operation_options operation) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::string(bytes_value), std::move(operation)}));
}
task<void> http_client_request_body_writer::write_owned(std::string&& bytes_value, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::move(bytes_value), std::move(operation)}));
}
task<void> http_client_request_body_writer::end(std::span<const http_header_view> fields_value, operation_options operation) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    http_request_trailers validation;
    if (validation.append(fields_value).has_value()) {
        throw std::invalid_argument("invalid request trailer section");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::finish_client_output<upload_output_policy>(state_, detail::http_client_upload_end_input{http_client_response(state_), std::move(validation).take_fields()}, std::move(operation)));
}
bool http_client_request_body_writer::complete() const noexcept {
    return state_ == nullptr || state_->upload_->output_.ended_;
}
bool http_client_request_body_writer::writable() const noexcept {
    if (state_ == nullptr || !state_->upload_) {
        return false;
    }
    const auto& output = state_->upload_->output_;
    return !output.stopped_ && !output.ended_ && !output.end_requested_;
}
void http_client_request_body_writer::abandon() & noexcept {
    if (state_ == nullptr || !state_->upload_ || state_->upload_->output_.ended_ || state_->upload_->output_.stopped_) {
        return;
    }
    state_->upload_->abandoned_ = true;
    state_->upload_->output_.stop();
}

task<http_client_response> http_client_exchange::response(operation_options operation) & {
    auto* state_value = response_.state_;
    if (state_value == nullptr || state_value->upload_->response_.taken()) {
        throw std::logic_error("HTTP exchange response has already been transferred");
    }
    state_value->require_owner_worker("HTTP exchange must run on its owner worker");
    return state_value->upload_->response_.receive(
        http_client_response(state_value), "HTTP request failed before response head", std::move(operation));
}
}  // namespace ruvia
