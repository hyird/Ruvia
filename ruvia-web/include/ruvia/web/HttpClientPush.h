#pragma once

#include "ruvia/http/HttpPush.h"
#include "ruvia/web/HttpClientResponse.h"
namespace ruvia {
namespace detail {
class HttpClientPool;
}
// An accepted promise owns its copied request and response storage on the
// client worker. Destruction cancels unfinished push. Like an ordinary response,
// it can survive client shutdown but must retire before the EventLoop.
class HttpClientPush final {
public:
    HttpClientPush(const HttpClientPush&) = delete;
    HttpClientPush& operator=(const HttpClientPush&) = delete;
    HttpClientPush(HttpClientPush&&) noexcept;
    HttpClientPush& operator=(HttpClientPush&&) noexcept;
    ~HttpClientPush();
    [[nodiscard]] const HttpPushRequest& request() const&;
    const HttpPushRequest& request() const&& = delete;
    // Transfers the response once, when the operation is started. Promise
    // metadata remains available through this push until it is destroyed.
    [[nodiscard]] ScopedOperation<HttpClientResponse> response() &;
    ScopedOperation<HttpClientResponse> response() && = delete;

private:
    friend class detail::HttpClientPool;
    explicit HttpClientPush(HttpClientResponse response) noexcept;
    void release() noexcept;
    static Task<HttpClientResponse> receiveOwned(HttpClientResponse pin);
    HttpClientResponse response_;
};
}  // namespace ruvia
