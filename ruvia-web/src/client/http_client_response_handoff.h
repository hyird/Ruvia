#pragma once

#include <string_view>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/web/http_client.h"

namespace ruvia::detail {

// Transfers the response of an exchange or a push to its consumer once, after
// the final head arrived. Embedded in the address-stable response state; one
// started receive runs at a time, and the owner retiring closes the cold ones.
class http_client_response_handoff final {
public:
    [[nodiscard]] bool taken() const noexcept {
        return taken_;
    }
    [[nodiscard]] bool has_pending_receive() const noexcept {
        return scope_.has_pending_operations();
    }
    // A cold receive of `pin`'s response, bound to this handoff's scope;
    // `head_message` (static text) reports a failure before the head.
    [[nodiscard]] task<http_client_response> receive(http_client_response pin, std::string_view head_message);
    // The owner retires: its pending receives close, and once the response was
    // transferred the owner's handle no longer consumes it.
    void release(http_client_response& owner) noexcept;

private:
    [[nodiscard]] task<http_client_response> receive_owned(http_client_response pin, std::string_view head_message);

    ::ruvia::operation_scope scope_;
    bool taken_{false};
    bool active_{false};
};

}  // namespace ruvia::detail
