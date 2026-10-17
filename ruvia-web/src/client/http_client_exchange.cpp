#include "ruvia/web/http_client_exchange.h"

#include <stdexcept>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_output_operation.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "util/operation_lane_lease.h"

namespace ruvia::detail {
struct http_client_upload_end_input final {
    http_client_response pin_;
    std::pmr::vector<http_header> fields_;
};
}  // namespace ruvia::detail
namespace ruvia {
namespace {
void require_writable(detail::http_client_response_state& state_value) {
    state_value.require_owner_worker("HTTP exchange must run on its owner worker");
    if (!state_value.upload_ || state_value.upload_->output_.stopped_ || state_value.upload_->output_.ended_ || state_value.upload_->output_.end_requested_) {
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP request upload is no longer writable");
    }
}
struct upload_output_policy final {
    static constexpr bool validate_chunk = true;
    static detail::http_client_output_queue& output(detail::http_client_response_state& state_value) noexcept {
        return state_value.upload_->output_;
    }
    static void require_write(detail::http_client_response_state& state_value) {
        auto& queue = output(state_value);
        if (queue.stopped_ || queue.end_requested_) {
            throw http_client_error(http_client_error::code_type::cancelled, "HTTP request upload stopped");
        }
    }
    static void require_empty_chunk(detail::http_client_output_queue& queue) {
        if (queue.chunk_ready_) {
            throw std::logic_error("HTTP upload queue still owns a chunk");
        }
    }
    static bool begin_finish(detail::http_client_response_state& state_value, detail::http_client_upload_end_input& input) {
        require_write(state_value);
        state_value.upload_->trailers_ = std::move(input.fields_);
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::http_client_response_state& state_value) {
        if (state_value.failure_) {
            std::rethrow_exception(state_value.failure_);
        }
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP request upload stopped");
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
    upload.response_scope_.close();
    if (upload.response_taken_) {
        response_.consumer_ = false;
    }
    if (abandon_upload && upload.response_taken_ && !state_value->complete_ &&
        state_value->transport_ == detail::http_client_response_transport::http1 &&
        state_value->pool_ != nullptr) {
        // Stopping the writer alone cannot interrupt a pending response read.
        // Keep the retained consumer's read scope open to observe cancellation.
        state_value->pool_->abandon_response(*state_value);
    } else if (!upload.output_.ended_) {
        upload.output_.stop();
    }
    response_.release();
    body_.state_ = nullptr;
}

scoped_operation<void> http_client_request_body_writer::write(std::span<const std::byte> bytes_value) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()));
}
scoped_operation<void> http_client_request_body_writer::write(std::string_view bytes_value) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::pmr::string(bytes_value, state_->resource_)}));
}
scoped_operation<void> http_client_request_body_writer::end(std::span<const http_header_view> fields_value) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    http_request_trailers validation(state_->resource_);
    for (const auto& field : fields_value) {
        std::pmr::string name(field.name(), state_->resource_);
        for (auto& ch : name) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        if ((validation.append(name, field.value()).index() != 0)) {
            throw std::invalid_argument("invalid request trailer section");
        }
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::finish_client_output<upload_output_policy>(state_, detail::http_client_upload_end_input{http_client_response(state_), std::move(validation).take_fields()}));
}
bool http_client_request_body_writer::complete() const noexcept {
    return state_ == nullptr || state_->upload_->output_.ended_;
}

scoped_operation<http_client_response> http_client_exchange::response() & {
    auto* state_value = response_.state_;
    if (state_value == nullptr || state_value->upload_->response_taken_) {
        throw std::logic_error("HTTP exchange response has already been transferred");
    }
    state_value->require_owner_worker("HTTP exchange must run on its owner worker");
    return ::ruvia::make_scoped_operation(state_value->upload_->response_scope_, receive_owned(http_client_response(state_value)));
}
task<http_client_response> http_client_exchange::receive_owned(http_client_response pin) {
    auto& state_value = *pin.state_;
    auto activity = detail::claim_operation_lane(state_value.upload_->response_active_, "HTTP exchange response operation is already active");
    if (state_value.upload_->response_taken_) {
        throw std::logic_error("HTTP exchange response has already been transferred");
    }
    co_await state_value.await_head("HTTP request failed before response head");
    state_value.upload_->response_taken_ = true;
    pin.consumer_ = true;
    co_return std::move(pin);
}
}  // namespace ruvia
