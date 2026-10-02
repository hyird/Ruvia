#pragma once

#include <memory_resource>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <asio.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/http/Http1InterimResponseWriter.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/detail/http/context/HttpInterimResponseOutput.h"

namespace ruvia::detail {
template <typename Stream>
class Http1InterimResponseSink final {
public:
    Http1InterimResponseSink(Stream& stream, std::pmr::memory_resource* resource)
        : stream_(stream),
          resource_(resource),
          output_(resource, this, write) {}
    [[nodiscard]] HttpInterimResponseOutput& output() noexcept {
        return output_;
    }

private:
    static Task<void> write(void* raw, const HttpInterimResponseHead& response) {
        auto& self = *static_cast<Http1InterimResponseSink*>(raw);
        std::pmr::vector<char> scratch(kMaxHttpHeaderBytes + 1024, self.resource_);
        const auto encoded = Http1InterimResponseWriter{}.prepare(response, scratch);
        const auto* prepared = encoded.prepared();
        if (prepared == nullptr) {
            throw std::invalid_argument("invalid or oversized interim response head");
        }
        if (prepared->connectionDisposition() != Http1InterimConnectionDisposition::kUnchanged) {
            throw std::invalid_argument("application interim response cannot close the exchange");
        }
        const auto completion = co_await asyncAsio<std::size_t>([&self, bytes = prepared->head()](auto handler) {
            asio::async_write(self.stream_, asio::buffer(bytes), std::move(handler));
        });
        if (completion.errorCode()) {
            throw std::system_error(completion.errorCode());
        }
    }
    Stream& stream_;
    std::pmr::memory_resource* resource_;
    HttpInterimResponseOutput output_;
};
}  // namespace ruvia::detail
