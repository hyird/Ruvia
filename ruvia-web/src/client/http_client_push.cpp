#include "ruvia/web/http_client_push.h"

#include <stdexcept>
#include <utility>

#include "client/http_client_pool.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"

namespace ruvia {
http_client_push::http_client_push(http_client_response response) noexcept
    : response_(std::move(response)) {}
http_client_push::http_client_push(http_client_push&& other) noexcept
    : response_(std::move(other.response_)) {}
http_client_push& http_client_push::operator=(http_client_push&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
    }
    return *this;
}
http_client_push::~http_client_push() {
    release();
}
void http_client_push::release() noexcept {
    if (auto* state_value = response_.state_; state_value != nullptr) {
        if (!state_value->memory_domain().worker().is_current()) {
            std::terminate();
        }
        state_value->push_response_scope_.close();
        if (state_value->push_response_taken_) {
            response_.consumer_ = false;
        }
    }
    response_.release();
}
const http_push_request& http_client_push::request() const& {
    const auto* state_value = response_.state_;
    if (state_value == nullptr || !state_value->promised_request_) {
        throw std::logic_error("push owner has moved");
    }
    if (!state_value->memory_domain().worker().is_current()) {
        throw std::logic_error("push requires its owner worker");
    }
    return *state_value->promised_request_;
}
scoped_operation<http_client_response> http_client_push::response() & {
    auto* state_value = response_.state_;
    (void)request();
    if (state_value->push_response_taken_ || state_value->push_response_scope_.has_pending_operations()) {
        throw std::logic_error("push response has already been transferred or is active");
    }
    return ::ruvia::make_scoped_operation(state_value->push_response_scope_, receive_owned(http_client_response(state_value)));
}
task<http_client_response> http_client_push::receive_owned(http_client_response pin) {
    auto& state = *pin.state_;
    co_await state.await_head("HTTP push failed before its response head");
    state.push_response_taken_ = true;
    pin.consumer_ = true;
    co_return std::move(pin);
}
}  // namespace ruvia
