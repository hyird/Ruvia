#pragma once

#include "ruvia/http/http_push.h"
#include "ruvia/web/http_client_response.h"
namespace ruvia {
namespace detail {
class http_client_pool;
}
// An accepted promise owns its copied request and response storage on the
// client worker. Destruction cancels unfinished push. Like an ordinary response,
// it can survive client shutdown but must retire before the event_loop.
class http_client_push final {
public:
    http_client_push(const http_client_push&) = delete;
    http_client_push& operator=(const http_client_push&) = delete;
    http_client_push(http_client_push&&) noexcept;
    http_client_push& operator=(http_client_push&&) noexcept;
    ~http_client_push();
    [[nodiscard]] const http_push_request& request() const&;
    const http_push_request& request() const&& = delete;
    // Transfers the response once, when the operation is started. Promise
    // metadata remains available through this push until it is destroyed.
    [[nodiscard]] scoped_operation<http_client_response> response() &;
    scoped_operation<http_client_response> response() && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_push(http_client_response response) noexcept;
    void release() noexcept;
    static task<http_client_response> receive_owned(http_client_response pin);
    http_client_response response_;
};
}  // namespace ruvia
