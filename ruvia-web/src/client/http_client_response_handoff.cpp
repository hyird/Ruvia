#include "client/http_client_response_handoff.h"

#include <stdexcept>
#include <utility>

#include "client/http_client_response_state.h"
#include "util/operation_lane_lease.h"

namespace ruvia::detail {

task<http_client_response> http_client_response_handoff::receive(
    http_client_response pin, std::string_view head_message) {
    return ::ruvia::make_scoped_operation(scope_, receive_owned(std::move(pin), head_message));
}

task<http_client_response> http_client_response_handoff::receive_owned(
    http_client_response pin, std::string_view head_message) {
    // `pin` keeps the response state, and with it this handoff, alive.
    auto activity = claim_operation_lane(active_, "HTTP response receive is already active");
    if (taken_) {
        throw std::logic_error("HTTP response has already been transferred");
    }
    co_await pin.state_->await_head(head_message);
    taken_ = true;
    pin.consumer_ = true;
    co_return std::move(pin);
}

void http_client_response_handoff::release(http_client_response& owner) noexcept {
    scope_.close();
    if (taken_) {
        owner.consumer_ = false;
    }
}

}  // namespace ruvia::detail
