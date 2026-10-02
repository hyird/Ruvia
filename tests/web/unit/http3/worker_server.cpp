#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/CallbackRef.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/http3/Http3ServerConnectionChannel.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"
#include "ruvia/web/detail/http3/Http3WorkerServer.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

#include "memory_resource_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"

namespace ruvia::detail {

struct http3_worker_server_test_access final {
    [[nodiscard]] static bool pump_once(Http3WorkerServer& server) noexcept {
        return server.pump();
    }

    [[nodiscard]] static bool retirement_join_started(
        const Http3WorkerServer& server, Http3ServerConnectionChannel::Identity identity) noexcept {
        for (const auto& slot : server.slots_) {
            if (slot.identity == identity) {
                return slot.retirementTaskStarted && slot.connection != nullptr &&
                       slot.connection->joinStarted_ && !slot.connection->joinCompleted_;
            }
        }
        return false;
    }

    [[nodiscard]] static bool transport_retired_consumed(
        const Http3WorkerServer& server, Http3ServerConnectionChannel::Identity identity) noexcept {
        for (const auto& slot : server.slots_) {
            if (slot.identity == identity) {
                return slot.transportRetired;
            }
        }
        return false;
    }

    [[nodiscard]] static bool retirement_task_started(
        const Http3WorkerServer& server, Http3ServerConnectionChannel::Identity identity) noexcept {
        for (const auto& slot : server.slots_) {
            if (slot.identity == identity) {
                return slot.retirementTaskStarted;
            }
        }
        return false;
    }

    [[nodiscard]] static bool join_completed(
        const Http3WorkerServer& server, Http3ServerConnectionChannel::Identity identity) noexcept {
        for (const auto& slot : server.slots_) {
            if (slot.identity == identity) {
                return slot.workerRetirementComplete && slot.connection == nullptr;
            }
        }
        return false;
    }
};

}  // namespace ruvia::detail

namespace {

using channel_type = ruvia::detail::Http3ServerConnectionChannel;
using worker_server_type = ruvia::detail::Http3WorkerServer;
using mailbox_type = ruvia::detail::Http3StreamMailbox;
using control_type = ruvia::detail::Http3StreamControl;
using message_id = ruvia::detail::Http3StreamMessageId;

struct handler_state final {
    explicit handler_state(const ruvia::WorkerHandle& worker)
        : started_(worker),
          release_(worker) {}

    ruvia::WorkerSignal started_;
    ruvia::WorkerSignal release_;
};

ruvia::Task<ruvia::HttpResponse> held_handler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<handler_state*>(raw);
    state.started_.notify();
    co_await state.release_.wait();
    co_return context.text("retired");
}

struct routes final {
    explicit routes(handler_state& state) {
        implementation_.registerRoute(ruvia::HttpKnownMethod::kGet, routing_test::path("/held"),
            ruvia::detail::RouteHandler(&state, &held_handler),
            ruvia::detail::RequestBodyMode::kBuffered,
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
        implementation_.finalize();
    }

    ruvia::detail::Router router_;
    ruvia::detail::RouterImpl& implementation_{ruvia::detail::RouterImpl::from(router_)};
};

struct wake final {
    explicit wake(ruvia::WorkerHandle worker)
        : signal_(worker) {}

    static void notify(void* raw) noexcept {
        auto& event = *static_cast<wake*>(raw);
        if (event.server_ != nullptr) {
            (void)event.server_->notification().notify();
        }
        event.signal_.notify();
    }

    [[nodiscard]] channel_type::Notification notification() noexcept {
        return {.context = this, .notify = &notify};
    }

    ruvia::WorkerSignal signal_;
    worker_server_type* server_{};
};

std::string request_wire(std::pmr::memory_resource* resource) {
    const auto fields = ruvia::encodeHttp3ClientRequestHead({.method = "GET",
                                                                .scheme = "https",
                                                                .authority = "example.test",
                                                                .path = "/held"},
        {}, resource);
    if (!fields) {
        throw std::runtime_error("HTTP/3 request field section encoding failed");
    }
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto header_size = ruvia::encodeHttp3FrameHeader(header,
        static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        fields->fieldSection.size());
    if (!header_size) {
        throw std::runtime_error("HTTP/3 request frame encoding failed");
    }
    std::string wire(header.data(), *header_size);
    wire.append(fields->fieldSection.data(), fields->fieldSection.size());
    return wire;
}

ruvia::Task<void> exercise_worker_server_retirement(
    ruvia::WorkerRuntimeContext& runtime, std::pmr::memory_resource& upstream,
    ruvia::testing::TestContext& ruvia_ctx) {
    const auto worker = runtime.handle();
    ruvia::WorkerMemory worker_memory(upstream);
    ruvia::StopSource stop_source;
    const auto stop_token = stop_source.token();
    handler_state handler(worker);
    routes route_set(handler);
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::WorkerCapabilities capabilities(runtime.ioContext(), worker,
        worker_memory.resource(), {}, {});
    ruvia::ConnectionScanner scanner(worker, {});
    std::atomic<std::size_t> active_connections{};
    std::atomic<std::size_t> refused_connections{};
    mailbox_type requests(8, 8, 8, worker_memory.resource());
    wake network_wake(worker);
    wake worker_wake(worker);
    channel_type active_channel(network_wake.notification(), worker_wake.notification());
    channel_type drain_channel(network_wake.notification(), worker_wake.notification());
    const std::array<channel_type*, 2> channels{&active_channel, &drain_channel};

    worker_server_type server(runtime, worker, worker_memory,
        route_set.implementation_.routeTable(), capabilities, scanner,
        runtime.ioContext().get_executor(), options, stop_token, 2, 8,
        active_connections, refused_connections);
    worker_wake.server_ = &server;
    RUVIA_CHECK(server.stageInstall({.requestMailbox = &requests,
        .channels = channels,
        .networkWake = network_wake.notification()}));
    RUVIA_CHECK(server.install());
    ruvia::TaskScope run_tasks(worker, {.resource = worker_memory.resource()});
    run_tasks.spawn(server.run());

    co_await network_wake.signal_.wait();
    channel_type::Identity active_identity;
    channel_type::Identity drain_identity;
    RUVIA_CHECK(active_channel.peekGrant(active_identity) == channel_type::Status::kReceived);
    RUVIA_CHECK(drain_channel.peekGrant(drain_identity) == channel_type::Status::kReceived);
    RUVIA_CHECK(active_channel.commitAccepted(active_identity,
                    {.remoteAddress = "127.0.0.1", .remotePort = 43210}) ==
                channel_type::Status::kPublished);
    RUVIA_CHECK(drain_channel.commitAccepted(drain_identity,
                    {.remoteAddress = "127.0.0.1", .remotePort = 43211}) ==
                channel_type::Status::kPublished);

    std::array<channel_type::AttachResult, 2> attach_results{
        channel_type::AttachResult(std::in_place_type<channel_type::AttachAck>),
        channel_type::AttachResult(std::in_place_type<channel_type::AttachAck>)};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        auto status = channels[i]->receiveAttachResult(attach_results[i]);
        while (status == channel_type::Status::kEmpty) {
            co_await network_wake.signal_.wait();
            status = channels[i]->receiveAttachResult(attach_results[i]);
        }
        RUVIA_CHECK(status == channel_type::Status::kReceived);
        RUVIA_CHECK(std::holds_alternative<channel_type::AttachAck>(attach_results[i]));
    }

    const auto wire = request_wire(worker_memory.resource());
    const message_id id{active_identity.epoch, active_identity.connectionGeneration, 0};
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    RUVIA_CHECK(requests.trySend(id, bytes) == mailbox_type::SendResult::kSent);
    RUVIA_CHECK(requests.trySendControl(
                    {control_type::Kind::kStreamFin, id, static_cast<std::uint64_t>(wire.size())}) ==
                mailbox_type::ControlResult::kSent);
    RUVIA_CHECK(server.notification().notify() != ruvia::WorkerNotificationStatus::kClosed);
    co_await handler.started_.wait();
    RUVIA_CHECK_EQ(active_connections.load(std::memory_order_relaxed), std::size_t{2});

    RUVIA_CHECK(active_channel.publishAdmissionSealed(active_identity, 1, 0) ==
                channel_type::Status::kPublished);
    RUVIA_CHECK(drain_channel.publishAdmissionSealed(drain_identity, 0, 0) ==
                channel_type::Status::kPublished);
    channel_type::DrainComplete drain_complete;
    auto drain_status = drain_channel.receiveDrainComplete(drain_complete);
    while (drain_status == channel_type::Status::kEmpty) {
        co_await network_wake.signal_.wait();
        drain_status = drain_channel.receiveDrainComplete(drain_complete);
    }
    RUVIA_CHECK(drain_status == channel_type::Status::kReceived);
    RUVIA_CHECK(drain_complete.identity == drain_identity);

    server.requestStop();
    std::array<channel_type::TransportIntent, 2> close_intents{};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        auto status = channels[i]->receiveIntent(close_intents[i]);
        while (status == channel_type::Status::kEmpty) {
            co_await network_wake.signal_.wait();
            status = channels[i]->receiveIntent(close_intents[i]);
        }
        RUVIA_CHECK(status == channel_type::Status::kReceived);
        RUVIA_CHECK(close_intents[i].token.kind ==
                    ruvia::detail::Http3ServerConnection::TransportIntentKind::kConnectionClose);
        const auto identity = i == 0 ? active_identity : drain_identity;
        RUVIA_CHECK(channels[i]->publishTransportRetired(identity) ==
                    channel_type::Status::kPublished);
        RUVIA_CHECK(channels[i]->acknowledgeIntentAfterHandoff(identity,
                        close_intents[i].token,
                        channel_type::IntentSettlement::kTransportRetiredSuperseded) ==
                    channel_type::Status::kPublished);
    }
    channel_type::DrainComplete late_drain_complete;
    RUVIA_CHECK(drain_channel.receiveDrainComplete(late_drain_complete) ==
                channel_type::Status::kReceived);
    RUVIA_CHECK(late_drain_complete.identity == drain_identity);

    RUVIA_CHECK(ruvia::detail::http3_worker_server_test_access::pump_once(server));
    RUVIA_CHECK(ruvia::detail::http3_worker_server_test_access::transport_retired_consumed(
        server, active_identity));
    RUVIA_CHECK(ruvia::detail::http3_worker_server_test_access::retirement_task_started(
        server, active_identity));
    RUVIA_CHECK(ruvia::detail::http3_worker_server_test_access::retirement_join_started(
        server, active_identity));
    RUVIA_CHECK(!ruvia::detail::http3_worker_server_test_access::join_completed(
        server, active_identity));
    RUVIA_CHECK_EQ(active_connections.load(std::memory_order_relaxed), std::size_t{2});

    RUVIA_CHECK(active_channel.closeNetworkPublications(active_identity) ==
                channel_type::Status::kPublished);
    RUVIA_CHECK(drain_channel.closeNetworkPublications(drain_identity) ==
                channel_type::Status::kPublished);
    channel_type::WorkerFinalized drain_worker_finalized;
    auto finalized_status = drain_channel.receiveWorkerFinalized(drain_worker_finalized);
    while (finalized_status == channel_type::Status::kEmpty) {
        co_await network_wake.signal_.wait();
        finalized_status = drain_channel.receiveWorkerFinalized(drain_worker_finalized);
    }
    RUVIA_CHECK(finalized_status == channel_type::Status::kReceived);
    RUVIA_CHECK(drain_worker_finalized.identity == drain_identity);
    RUVIA_CHECK(drain_channel.acknowledgeWorkerFinalized(drain_identity) ==
                channel_type::Status::kPublished);
    RUVIA_CHECK(drain_channel.publishNetworkFinalized(drain_identity) ==
                channel_type::Status::kPublished);

    handler.release_.notify();
    channel_type::WorkerFinalized active_worker_finalized;
    auto active_finalized_status = active_channel.receiveWorkerFinalized(active_worker_finalized);
    while (active_finalized_status == channel_type::Status::kEmpty) {
        co_await network_wake.signal_.wait();
        active_finalized_status = active_channel.receiveWorkerFinalized(active_worker_finalized);
    }
    RUVIA_CHECK(active_finalized_status == channel_type::Status::kReceived);
    RUVIA_CHECK(active_worker_finalized.identity == active_identity);
    RUVIA_CHECK(ruvia::detail::http3_worker_server_test_access::join_completed(
        server, active_identity));
    RUVIA_CHECK(active_channel.acknowledgeWorkerFinalized(active_identity) ==
                channel_type::Status::kPublished);
    RUVIA_CHECK(active_channel.publishNetworkFinalized(active_identity) ==
                channel_type::Status::kPublished);

    while (!active_channel.readyToDestroy() || !drain_channel.readyToDestroy()) {
        co_await worker_wake.signal_.wait();
    }
    co_await run_tasks.join();
    RUVIA_CHECK(server.drained());
    RUVIA_CHECK_EQ(active_connections.load(std::memory_order_relaxed), std::size_t{0});
    RUVIA_CHECK(requests.stop());
    static_cast<void>(ruvia_ctx);
}

ruvia::Task<void> close_runtime_after(ruvia::WorkerRuntimeContext& runtime,
    ruvia::Task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        runtime.close();
        throw;
    }
    runtime.close();
}

}  // namespace

RUVIA_TEST(http3_worker_server_joins_retired_connection_while_handler_is_active) {
    asio::io_context io;
    ruvia::WorkerRuntimeContext runtime(io, 32);
    ruvia::test::CountingMemoryResource upstream;
    std::promise<std::exception_ptr> operation_exit_promise;
    auto operation_exit = operation_exit_promise.get_future();
    asio::co_spawn(io,
        ruvia::asAwaitable(close_runtime_after(runtime,
            exercise_worker_server_retirement(runtime, upstream, ruvia_ctx))),
        [&operation_exit_promise](std::exception_ptr failure) {
            operation_exit_promise.set_value(std::move(failure));
        });
    std::promise<std::exception_ptr> runtime_exit_promise;
    auto runtime_exit = runtime_exit_promise.get_future();
    std::thread worker_thread([&] {
        std::exception_ptr failure;
        try {
            runtime.run();
        } catch (...) {
            failure = std::current_exception();
        }
        runtime_exit_promise.set_value(std::move(failure));
    });
    const auto operation_failure = operation_exit.get();
    const auto runtime_failure = runtime_exit.get();
    worker_thread.join();
    RUVIA_CHECK(operation_failure == nullptr);
    RUVIA_CHECK(runtime_failure == nullptr);
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
