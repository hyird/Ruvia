#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerNotification.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3ServerBodyBudget.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/http3/Http3ServerConnectionChannel.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"
#include "ruvia/web/detail/http3/Http3WorkerMailboxScheduler.h"

namespace ruvia::detail {

class RouteTable;
class WorkerCapabilities;
struct HttpServerOptions;

// Worker-affine half of the production HTTP/3 bridge. One instance serves all
// QUIC connections pinned to this business worker through one request mailbox,
// one response mailbox, and fixed startup-allocated scheduler/channel slots.
class Http3WorkerServer final {
public:
    struct Install final {
        Http3StreamMailbox* requestMailbox{};
        std::span<Http3ServerConnectionChannel* const> channels{};
        Http3ServerConnectionChannel::Notification networkWake{};
    };

    Http3WorkerServer(ruvia::WorkerRuntimeContext& runtime, const WorkerHandle& worker,
        WorkerMemory& memory, const RouteTable& routes, WorkerCapabilities& capabilities,
        ConnectionScanner& connectionScanner, asio::any_io_executor executor,
        const HttpServerOptions& options, const StopToken& stopToken,
        std::size_t maxConnections, std::uint32_t mailboxCapacity,
        std::atomic<std::size_t>& activeConnections,
        std::atomic<std::size_t>& refusedConnections);
    ~Http3WorkerServer();

    Http3WorkerServer(const Http3WorkerServer&) = delete;
    Http3WorkerServer& operator=(const Http3WorkerServer&) = delete;

    [[nodiscard]] WorkerNotification& notification() noexcept {
        return notification_;
    }
    [[nodiscard]] Http3StreamMailbox& responseMailbox() noexcept {
        return responseMailbox_;
    }

    // Startup-thread staging followed by worker-owner activation. The network
    // owner keeps every supplied object stable through the finalization handshake.
    [[nodiscard]] bool stageInstall(Install link) noexcept;
    [[nodiscard]] bool install() noexcept;
    [[nodiscard]] Task<void> run();
    void requestStop() noexcept;
    // Worker-owner rollback when no run coroutine was launched (including a
    // successful install followed by TaskScope::spawn failure). Must run on the
    // worker and before run() starts; it closes mailboxes and retires local state.
    void abandonBeforeLaunch() noexcept;
    [[nodiscard]] bool installed() const noexcept {
        return installed_;
    }
    [[nodiscard]] bool drained() const noexcept {
        return drained_.load(std::memory_order_acquire);
    }

private:
    struct Slot final {
        explicit Slot(std::pmr::memory_resource* resource)
            : remoteAddress(resource),
              clientCertificateSubject(resource),
              connection(nullptr,
                  PmrObjectDeleter<Http3ServerConnection>{resource}) {}

        Http3ServerConnectionChannel* channel{};
        Http3WorkerMailboxScheduler::Registration registration{};
        Http3ServerConnectionChannel::Identity identity{};
        Http3ServerConnectionChannel::Identity lastIdentity{};
        std::pmr::string remoteAddress;
        std::pmr::string clientCertificateSubject;
        std::uint16_t remotePort{};
        std::unique_ptr<Http3ServerConnection,
            PmrObjectDeleter<Http3ServerConnection>>
            connection;
        bool reserved{};
        bool attached{};
        bool rejected{};
        bool revokeAcknowledged{};
        bool retirementStarted{};
        bool retirementTaskStarted{};
        bool transportRetired{};
        bool workerRetirementComplete{};
        bool admissionSealed{};
        std::size_t expectedAdmittedRequests{};
        bool drainCompletePublished{};
        bool workerPublicationsClosed{};
        bool workerFinalized{};
    };

    static void capacityWake(void* context) noexcept;
    static void activationWake(void* context) noexcept;
    [[nodiscard]] bool pump() noexcept;
    [[nodiscard]] bool pumpChannels() noexcept;
    [[nodiscard]] bool pumpInput() noexcept;
    [[nodiscard]] bool pumpScheduler() noexcept;
    [[nodiscard]] bool publishDrainCompletions() noexcept;
    [[nodiscard]] Slot* findSlot(Http3StreamMessageId id) noexcept;
    [[nodiscard]] Slot* findSlot(
        Http3ServerConnectionChannel::Identity identity) noexcept;
    [[nodiscard]] bool constructConnection(
        Slot& slot, const Http3ServerConnectionChannel::Bind& bind) noexcept;
    void beginSlotRetirement(Slot& slot) noexcept;
    [[nodiscard]] bool finishTerminalSlots() noexcept;
    [[nodiscard]] bool finalizeWorkerPublications(Slot& slot) noexcept;
    void finishStoppedSlots() noexcept;

    const WorkerHandle& worker_;
    WorkerMemory& memory_;
    const RouteTable& routes_;
    WorkerCapabilities& capabilities_;
    ConnectionScanner& connectionScanner_;
    asio::any_io_executor executor_;
    const HttpServerOptions& options_;
    const StopToken& stopToken_;
    std::atomic<std::size_t>& activeConnections_;
    std::atomic<std::size_t>& refusedConnections_;
    WorkerNotification notification_;
    Http3WorkerMailboxCapacitySignal capacitySignal_;
    Http3StreamMailbox responseMailbox_;
    std::optional<Http3WorkerMailboxScheduler> scheduler_;
    Http3ServerBodyBudget bodyBudget_;
    TaskScope retirementTasks_;
    std::pmr::vector<Slot> slots_;
    Http3StreamMailbox* requestMailbox_{};
    Http3ServerConnectionChannel::Notification networkWake_{};
    std::size_t maxConnections_{};
    std::uint64_t epoch_{1};
    std::uint64_t nextConnectionGeneration_{1};
    bool staged_{};
    bool installed_{};
    bool runStarted_{};
    bool stopping_{};
    std::atomic<bool> drained_{};
};

}  // namespace ruvia::detail
