#pragma once

#ifndef RUVIA_ENABLE_HTTP_CLIENT

#include <asio/io_context.hpp>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/app/Task.h"

namespace ruvia::detail {

struct HttpClientDefinition {};

class HttpClientRegistry final {
public:
    HttpClientRegistry(
        asio::io_context&,
        std::pmr::memory_resource*,
        std::span<const HttpClientDefinition>) {}

    HttpClientRegistry(const HttpClientRegistry&) = delete;
    HttpClientRegistry& operator=(const HttpClientRegistry&) = delete;

    Task<void> connect() { co_return; }
    void closeNow() noexcept {}
    [[nodiscard]] bool empty() const noexcept { return true; }
    [[nodiscard]] bool hasAnyTimeout() const noexcept { return false; }
};

}  // namespace ruvia::detail

#else  // RUVIA_ENABLE_HTTP_CLIENT

#include <asio/io_context.hpp>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

class HttpClientPool;

class HttpClientRegistry final {
public:
    HttpClientRegistry(
        asio::io_context& ioContext,
        std::pmr::memory_resource* resource,
        std::span<const HttpClientDefinition> clients);
    ~HttpClientRegistry();

    HttpClientRegistry(const HttpClientRegistry&) = delete;
    HttpClientRegistry& operator=(const HttpClientRegistry&) = delete;

    Task<void> connect();
    void closeNow() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    [[nodiscard]] HttpClientPool* get(std::string_view alias = kDefaultHttpClientAlias) const;

private:
    struct Entry final {
        std::pmr::string alias;
        std::unique_ptr<HttpClientPool> pool;
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> pools_;
    HttpClientPool* defaultPool_{nullptr};
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
