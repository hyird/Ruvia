#pragma once
#include <chrono>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/web/HttpTunnel.h"
#include "ruvia/web/detail/http/HttpStreamReadResult.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/HttpTunnelAccess.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/util/CallableRef.h"
namespace ruvia::detail {
template <typename Transport>
class HttpTunnelSession final {
public:
    HttpTunnelSession(Transport transport, const WorkerHandle& worker, std::pmr::memory_resource& resource,
        std::string_view pending = {})
        : transport_(std::move(transport)),
          resource_(resource),
          pending_(pending, &resource),
          facade_(HttpTunnelAccess::make(resource, worker, this, &readThunk, &writeThunk, &finishThunk, &abortThunk)) {
        if constexpr (requires(Transport& transport) { transport.readDatagramInput(); transport.datagramConfig(); transport.sendDatagram(std::span<const std::byte>{}); }) {
            HttpTunnelAccess::bindDatagrams(facade_, [](void* raw) -> Task<std::optional<HttpDatagramInput>> {
                    auto& owner=*static_cast<HttpTunnelSession*>(raw);
                    auto input=co_await owner.transport_.readDatagramInput();
                    if(!input){ owner.receiveEnded_=true;
}
                    co_return input; }, [](void* raw, std::span<const std::byte> bytes) {
                    auto& owner=*static_cast<HttpTunnelSession*>(raw);
                    if(owner.ended_){ throw std::logic_error("HTTP Datagram send direction has ended");
}
                    owner.transport_.sendDatagram(bytes); }, [](void* raw) { return static_cast<HttpTunnelSession*>(raw)->transport_.datagramConfig(); });
        }
    }
    [[nodiscard]] HttpTunnel& tunnel() noexcept {
        return facade_;
    }
    void abort() noexcept {
        aborted_ = true;
        transport_.abort();
    }
    [[nodiscard]] Task<void> finish() {
        if (!ended_) {
            const auto error = co_await transport_.writeBytes({}, HttpStreamEnd::kEnd);
            if (error) {
                throw std::system_error(error, "HTTP tunnel finish");
            }
            ended_ = true;
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }
    [[nodiscard]] Task<void> drain() {
        while (!receiveEnded_) {
            std::pmr::string discarded(&resource_);
            const auto read = co_await transport_.readMore(discarded);
            if (const auto* failed = read.failure()) {
                throw std::system_error(failed->errorCode(), "HTTP tunnel drain");
            }
            receiveEnded_ = read.end() != nullptr;
        }
    }
    [[nodiscard]] Task<void> join() {
        if (HttpTunnelAccess::hasRunningOperations(facade_)) {
            abort();
        }
        co_await HttpTunnelAccess::closeAndJoin(facade_);
    }

private:
    static Task<std::optional<std::pmr::string>> readThunk(void* raw) {
        auto& owner = *static_cast<HttpTunnelSession*>(raw);
        if (!owner.pending_.empty()) {
            co_return std::pmr::string(std::move(owner.pending_), &owner.resource_);
        }
        std::pmr::string bytes(&owner.resource_);
        const auto result = co_await owner.transport_.readMore(bytes);
        if (const auto* failed = result.failure()) {
            throw std::system_error(failed->errorCode(), "HTTP tunnel read");
        }
        if (result.end() != nullptr) {
            owner.receiveEnded_ = true;
            co_return std::nullopt;
        }
        co_return std::move(bytes);
    }
    static Task<void> writeThunk(void* raw, std::string_view bytes) {
        auto& owner = *static_cast<HttpTunnelSession*>(raw);
        if (owner.ended_) {
            throw std::logic_error("HTTP tunnel send direction has ended");
        }
        const auto error = co_await owner.transport_.writeBytes(bytes, HttpStreamEnd::kKeepOpen);
        if (error) {
            throw std::system_error(error, "HTTP tunnel write");
        }
    }
    static Task<void> finishThunk(void* raw) {
        return static_cast<HttpTunnelSession*>(raw)->finish();
    }
    static void abortThunk(void* raw) noexcept {
        static_cast<HttpTunnelSession*>(raw)->abort();
    }
    Transport transport_;
    std::pmr::memory_resource& resource_;
    std::pmr::string pending_;
    bool ended_{false};
    bool receiveEnded_{false};
    bool aborted_{false};
    HttpTunnel facade_;
};

template <typename Transport>
[[nodiscard]] Task<void> invokeTunnelHandler(HttpTunnelSession<Transport>& session,
    ConnectionScanner::Entry& entry, const CallableRef<void, Context&>& handler, Context& context) {
    ContextTunnelBinding binding(context, session.tunnel());
    entry.setPhase(ConnectionScanner::Phase::kLongLived);
    co_await handler(context);
}

template <typename Transport>
[[nodiscard]] Task<void> finishTunnelSession(HttpTunnelSession<Transport>& session,
    std::exception_ptr failure, const ConnectionFailureSink& sink, std::string_view remote,
    ConnectionScanner::Entry& scanner, std::chrono::milliseconds timeout) {
    if (failure != nullptr) {
        session.abort();
    }
    co_await session.join();
    if (failure == nullptr && !session.aborted()) {
        struct DrainDeadline final {
            HttpTunnelSession<Transport>& session;
            std::int64_t deadline;
            ConnectionScanner::PeriodicCheckRegistration registration;
            static void tick(void* raw, std::int64_t now) noexcept {
                auto& owner = *static_cast<DrainDeadline*>(raw);
                if (now >= owner.deadline) {
                    owner.registration.reset();
                    owner.session.abort();
                }
            }
        } deadline{session, 0, {}};
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        deadline.deadline = timeout.count() > (std::numeric_limits<std::int64_t>::max)() - now
                                ? (std::numeric_limits<std::int64_t>::max)()
                                : now + timeout.count();
        scanner.registerPeriodicCheck(deadline.registration, &deadline, &DrainDeadline::tick);
        try {
            co_await session.finish();
            co_await session.drain();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (failure != nullptr) {
        session.abort();
        sink.invoke(remote, failure);
    }
}
}  // namespace ruvia::detail
