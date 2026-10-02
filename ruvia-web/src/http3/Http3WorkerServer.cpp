#include "ruvia/web/detail/http3/Http3WorkerServer.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include <asio/post.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t kWorkerBodyBudgetBytes = std::size_t{64} * 1024 * 1024;
constexpr std::size_t kPumpBudget = 256;
constexpr std::size_t kMailboxPumpBudget = 64;

Http3StreamMailboxCapacityNotifier mailboxCapacityNotifier(
    Http3WorkerMailboxCapacitySignal& signal) noexcept {
    return signal.notifier();
}

}  // namespace

Http3WorkerServer::Http3WorkerServer(ruvia::WorkerRuntimeContext& runtime,
    const WorkerHandle& worker, WorkerMemory& memory, const RouteTable& routes,
    WorkerCapabilities& capabilities, ConnectionScanner& connectionScanner,
    asio::any_io_executor executor, const HttpServerOptions& options,
    const StopToken& stopToken, std::size_t maxConnections,
    std::uint32_t mailboxCapacity, std::atomic<std::size_t>& activeConnections,
    std::atomic<std::size_t>& refusedConnections)
    : worker_(worker),
      memory_(memory),
      routes_(routes),
      capabilities_(capabilities),
      connectionScanner_(connectionScanner),
      executor_(std::move(executor)),
      options_(options),
      stopToken_(stopToken),
      activeConnections_(activeConnections),
      refusedConnections_(refusedConnections),
      notification_(runtime),
      capacitySignal_({this, &capacityWake}),
      responseMailbox_(mailboxCapacity, mailboxCapacity, mailboxCapacity,
          memory.resource(), mailboxCapacityNotifier(capacitySignal_)),
      bodyBudget_(kWorkerBodyBudgetBytes),
      retirementTasks_(worker_, {.resource = memory.resource()}),
      slots_(memory.resource()),
      pendingInput_(memory.resource()),
      maxConnections_(maxConnections) {
    slots_.reserve(maxConnections_);
    pendingInput_.resize(mailboxCapacity);
    for (std::size_t i = 0; i < maxConnections_; ++i) {
        slots_.emplace_back(memory.resource());
    }
}

Http3WorkerServer::~Http3WorkerServer() {
    if (!drained_.load(std::memory_order_acquire) && installed_) {
        std::terminate();
    }
}

void Http3WorkerServer::capacityWake(void* context) noexcept {
    auto& owner = *static_cast<Http3WorkerServer*>(context);
    (void)owner.notification_.notify();
}

void Http3WorkerServer::activationWake(void* context) noexcept {
    if (context == nullptr) {
        std::terminate();
    }
    auto& owner = *static_cast<Http3WorkerServer*>(context);
    if (!owner.worker_.isCurrent()) {
        std::terminate();
    }
    if (owner.notification_.notify() == ruvia::WorkerNotificationStatus::kClosed &&
        !owner.stopping_) {
        // An activation may only be dropped once the owning worker is already
        // stopping and has explicitly closed its latched wake channel.
        std::terminate();
    }
}

bool Http3WorkerServer::stageInstall(Install link) noexcept {
    if (staged_ || link.requestMailbox == nullptr ||
        link.channels.size() != slots_.size() || link.requestMailbox->blockCapacity() > pendingInput_.size() || link.networkWake.context == nullptr ||
        link.networkWake.notify == nullptr) {
        return false;
    }
    requestMailbox_ = link.requestMailbox;
    networkWake_ = link.networkWake;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (link.channels[i] == nullptr) {
            return false;
        }
        slots_[i].channel = link.channels[i];
    }
    staged_ = true;
    return true;
}

bool Http3WorkerServer::install() noexcept {
    if (!worker_.isCurrent() || !staged_ || installed_ || stopping_ ||
        drained_.load(std::memory_order_acquire)) {
        return false;
    }
    try {
        scheduler_.emplace(worker_, maxConnections_, memory_.resource(), &capacitySignal_,
            Http3WorkerMailboxScheduler::kDefaultControlBurstLimit,
            Http3WorkerMailboxWakeRef{
                .context = this,
                .notify = &activationWake,
            });
    } catch (...) {
        return false;
    }
    if (!scheduler_->bindMailbox(responseMailbox_)) {
        scheduler_.reset();
        return false;
    }
    installed_ = true;
    (void)notification_.notify();
    return true;
}

Task<void> Http3WorkerServer::run() {
    if (!worker_.isCurrent() || runStarted_ || !installed_) {
        std::terminate();
    }
    runStarted_ = true;
    while (!drained_.load(std::memory_order_acquire)) {
        bool progress = false;
        bool exhaustedPumpBudget = true;
        for (std::size_t pass = 0; pass < kPumpBudget; ++pass) {
            if (!pump()) {
                exhaustedPumpBudget = false;
                break;
            }
            progress = true;
        }
        if (stopping_) {
            finishStoppedSlots();
            const bool anyLive = std::ranges::any_of(slots_, [](const Slot& slot) {
                return slot.connection != nullptr || slot.reserved;
            });
            if (!anyLive) {
                if (requestMailbox_ != nullptr) {
                    (void)requestMailbox_->stop();
                }
                (void)responseMailbox_.stop();
                notification_.close();
                co_await retirementTasks_.join();
                scheduler_.reset();
                drained_.store(true, std::memory_order_release);
                co_return;
            }
        }
        if (exhaustedPumpBudget) {
            // A full pump turn must yield to other worker work before retrying;
            // bounded inner loops alone do not make this coroutine cooperative.
            const auto yielded = co_await ruvia::asyncAsio([this](auto completion) {
                asio::post(executor_, [completion = std::move(completion)]() mutable {
                    completion(asio::error_code{});
                });
            });
            if (yielded.errorCode()) {
                stopping_ = true;
            }
            continue;
        }
        if (!progress) {
            switch (scheduler_->armCapacityWait().status) {
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kReady:
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kWakePending:
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kRecoveryPending:
                    continue;
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kStopped:
                    stopping_ = true;
                    continue;
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kArmed:
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kNoBlockedWork:
                    break;
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kUnavailable:
                case Http3WorkerMailboxScheduler::CapacityArmStatus::kWrongWorker:
                    std::terminate();
            }
            const auto waited = co_await notification_.wait();
            if (waited == WorkerNotificationWaitStatus::kClosed) {
                stopping_ = true;
            } else {
                scheduler_->notifyTransportCapacity();
            }
        }
    }
}

void Http3WorkerServer::requestStop() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    if (stopping_) {
        return;
    }
    stopping_ = true;
    for (auto& pending : pendingInput_) {
        pending.block.release();
    }
    pendingInputCount_ = 0;
    for (auto& slot : slots_) {
        if (slot.channel != nullptr) {
            const auto sealed = slot.channel->stopGrantPublication();
            if (sealed != Http3ServerConnectionChannel::Status::kPublished &&
                sealed != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
            }
        }
    }
    if (!installed_) {
        if (requestMailbox_ != nullptr) {
            (void)requestMailbox_->stop();
        }
        (void)responseMailbox_.stop();
        notification_.close();
        if (!runStarted_) {
            drained_.store(true, std::memory_order_release);
        }
        return;
    }
    for (auto& slot : slots_) {
        if (slot.connection != nullptr) {
            (void)slot.connection->requestStop();
            beginSlotRetirement(slot);
        }
    }
    (void)notification_.notify();
}

void Http3WorkerServer::abandonBeforeLaunch() noexcept {
    if (runStarted_) {
        std::terminate();
    }
    if (drained_.load(std::memory_order_acquire)) {
        return;
    }
    if (installed_ && !worker_.isCurrent()) {
        std::terminate();
    }
    stopping_ = true;
    for (auto& slot : slots_) {
        if (slot.channel != nullptr) {
            const auto sealed = slot.channel->stopGrantPublication();
            if (sealed != Http3ServerConnectionChannel::Status::kPublished &&
                sealed != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
            }
        }
    }
    if (requestMailbox_ != nullptr) {
        (void)requestMailbox_->stop();
    }
    (void)responseMailbox_.stop();
    notification_.close();
    // install() may already have succeeded when TaskScope::spawn fails. No run
    // coroutine exists in this path, so discard its scheduler locally instead
    // of leaving installed state that the destructor would consider live.
    scheduler_.reset();
    drained_.store(true, std::memory_order_release);
}

bool Http3WorkerServer::pump() noexcept {
    if (!worker_.isCurrent() || !installed_ || !scheduler_) {
        return false;
    }
    bool progress = false;
    progress = pumpChannels() || progress;
    progress = pumpInput() || progress;
    for (auto& slot : slots_) {
        if (!slot.connection || slot.retirementStarted || !slot.channel) {
            continue;
        }
        Http3ServerConnectionChannel::Datagram datagram;
        for (std::size_t pass = 0; pass < 16; ++pass) {
            if (slot.channel->receiveRequestDatagram(datagram) != Http3ServerConnectionChannel::Status::kReceived) {
                break;
            }
            slot.connection->receiveDatagram(std::span(datagram.bytes).first(datagram.size));
            progress = true;
        }
    }
    for (auto& slot : slots_) {
        if (slot.connection && !slot.retirementStarted) {
            progress = slot.connection->resumeQpackInput() || progress;
        }
    }
    progress = pumpScheduler() || progress;
    progress = publishDrainCompletions() || progress;
    if (scheduler_->takeLocalWakeObligation()) {
        progress = true;
    }
    finishStoppedSlots();
    return progress;
}

bool Http3WorkerServer::pumpChannels() noexcept {
    if (!scheduler_) {
        std::terminate();
    }
    bool progress = false;
    for (auto& slot : slots_) {
        if (slot.channel == nullptr) {
            continue;
        }
        if (!slot.reserved && !stopping_) {
            if (nextConnectionGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
                stopping_ = true;
                continue;
            }
            const auto published = slot.channel->reserveAndPublishGrant(
                *scheduler_, epoch_, nextConnectionGeneration_++);
            if (published.status == Http3ServerConnectionChannel::Status::kPublished) {
                slot.registration = published.registration;
                slot.identity = published.identity;
                slot.lastIdentity = published.identity;
                slot.reserved = true;
                progress = true;
            }
        }

        if (slot.reserved && slot.connection != nullptr && !slot.admissionSealed &&
            !slot.transportRetired) {
            Http3ServerConnectionChannel::AdmissionSealed sealed;
            const auto status = slot.channel->receiveAdmissionSealed(sealed);
            if (status == Http3ServerConnectionChannel::Status::kReceived) {
                if (sealed.identity != slot.identity) {
                    std::terminate();
                }
                slot.admissionSealed = true;
                slot.expectedAdmittedRequests = sealed.expectedAdmittedRequests;
                progress = true;
            } else if (status != Http3ServerConnectionChannel::Status::kEmpty) {
                std::terminate();
            }
        }

        if (slot.reserved && slot.connection == nullptr && !slot.rejected &&
            !slot.revokeAcknowledged && !slot.workerRetirementComplete) {
            Http3ServerConnectionChannel::Bind bind;
            const auto bindStatus = slot.channel->receiveBind(bind);
            if (bindStatus == Http3ServerConnectionChannel::Status::kReceived) {
                progress = true;
                if (bind.identity != slot.identity) {
                    std::terminate();
                }
                if (stopping_ || !constructConnection(slot, bind)) {
                    const auto reason = stopping_
                                            ? Http3ServerConnectionChannel::RejectReason::kStopping
                                            : Http3ServerConnectionChannel::RejectReason::kConstructionFailed;
                    if (slot.channel->reject(*scheduler_, slot.registration, reason) !=
                        Http3ServerConnectionChannel::Status::kPublished) {
                        std::terminate();
                    }
                    slot.rejected = true;
                    refusedConnections_.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (bindStatus != Http3ServerConnectionChannel::Status::kEmpty) {
                // A committed bind must either be consumed or remain pending;
                // silently treating WrongState as empty strands the accepted
                // transport and prevents the attach/retirement handshake.
                std::terminate();
            }
        }

        Http3ServerConnectionChannel::TransportIntentAck intentAck;
        while (slot.channel->receiveIntentAck(intentAck) ==
               Http3ServerConnectionChannel::Status::kReceived) {
            progress = true;
            if (!scheduler_->acknowledgeIntent(slot.registration.token, intentAck.token, intentAck.pushStream)) {
                std::terminate();
            }
        }

        Http3ServerConnectionChannel::TransportRetired retired;
        if (slot.channel->receiveTransportRetired(retired) ==
            Http3ServerConnectionChannel::Status::kReceived) {
            progress = true;
            if (retired.identity != slot.identity) {
                std::terminate();
            }
            slot.transportRetired = true;
            if (slot.connection != nullptr) {
                if (!slot.connection->confirmTransportRetired({
                        .epoch = retired.identity.epoch,
                        .connectionGeneration = retired.identity.connectionGeneration,
                    })) {
                    std::terminate();
                }
                beginSlotRetirement(slot);
            } else if (slot.rejected) {
                slot.workerRetirementComplete = true;
            } else {
                std::terminate();
            }
        }

        Http3ServerConnectionChannel::NetworkFinalized finalized;
        if (slot.channel->receiveNetworkFinalized(finalized) ==
            Http3ServerConnectionChannel::Status::kReceived) {
            progress = true;
            if (finalized.identity != slot.identity ||
                slot.channel->acknowledgeNetworkFinalized(finalized.identity) !=
                    Http3ServerConnectionChannel::Status::kPublished) {
                std::terminate();
            }
            slot.reserved = false;
            slot.attached = false;
            slot.rejected = false;
            slot.revokeAcknowledged = false;
            slot.retirementStarted = false;
            slot.retirementTaskStarted = false;
            slot.transportRetired = false;
            slot.workerRetirementComplete = false;
            slot.admissionSealed = false;
            slot.expectedAdmittedRequests = 0;
            slot.drainCompletePublished = false;
            slot.workerPublicationsClosed = false;
            slot.workerFinalized = false;
            slot.identity = {};
            slot.registration = {};
        }

        if (!slot.reserved && stopping_ && slot.lastIdentity.epoch != 0 &&
            !slot.workerPublicationsClosed) {
            const auto closed = slot.channel->closeWorkerPublications(slot.lastIdentity);
            if (closed == Http3ServerConnectionChannel::Status::kPublished) {
                slot.workerPublicationsClosed = true;
                progress = true;
            } else if (closed != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
            }
        }

        Http3ServerConnectionChannel::Identity revoked;
        if (slot.reserved && slot.channel->receiveRevoke(revoked) ==
                                 Http3ServerConnectionChannel::Status::kReceived) {
            progress = true;
            if (revoked != slot.identity ||
                slot.channel->acknowledgeRevoke(*scheduler_, slot.registration) !=
                    Http3ServerConnectionChannel::Status::kPublished) {
                std::terminate();
            }
            slot.revokeAcknowledged = true;
        }
    }
    return finishTerminalSlots() || progress;
}

bool Http3WorkerServer::pumpInput() noexcept {
    if (requestMailbox_ == nullptr) {
        return false;
    }
    bool progress = false;
    const auto sameStream = [](const Http3StreamMessageId& a, const Http3StreamMessageId& b) noexcept {
        return a.epoch == b.epoch && a.connectionGeneration == b.connectionGeneration && a.streamId == b.streamId;
    };
    const auto consume = [this, &progress](Http3StreamMailbox::BorrowedBlock& block) noexcept {
        auto* slot = findSlot(block.id());
        if (stopping_ || slot == nullptr || slot->connection == nullptr) {
            block.release();
            progress = true;
            return;
        }
        if (!slot->connection->canAcceptInput(block.id().streamId, block.bytes().size())) {
            return;
        }
        const auto result = slot->connection->acceptData(block);
        block.release();
        progress = true;
        if (result.connectionCloseRequired) {
            (void)slot->connection->requestStop();
            beginSlotRetirement(*slot);
        }
    };
    for (auto& pending : pendingInput_) {
        if (pendingInputCount_ == 0) {
            break;
        }
        if (!pending.block) {
            continue;
        }
        const bool earlier = std::ranges::any_of(pendingInput_, [&](const PendingInput& other) {
            return other.block && other.sequence < pending.sequence && sameStream(other.block.id(), pending.block.id());
        });
        if (!earlier) {
            consume(pending.block);
            if (!pending.block) {
                --pendingInputCount_;
            }
        }
    }
    for (std::size_t count = 0; count < kMailboxPumpBudget;) {
        bool received = false;
        Http3StreamControl control;
        if (requestMailbox_->tryReceiveControl(control)) {
            received = true;
            progress = true;
            ++count;
            if (auto* slot = findSlot(control.id); slot != nullptr && slot->connection != nullptr) {
                const auto result = slot->connection->acceptControl(control);
                if (result.connectionCloseRequired) {
                    (void)slot->connection->requestStop();
                    beginSlotRetirement(*slot);
                }
            }
        }
        Http3StreamMailbox::BorrowedBlock block;
        if (count < kMailboxPumpBudget && requestMailbox_->tryReceive(block)) {
            received = true;
            progress = true;
            ++count;
            const bool earlier = pendingInputCount_ != 0 && std::ranges::any_of(pendingInput_, [&](const PendingInput& pending) {
                return pending.block && sameStream(pending.block.id(), block.id());
            });
            if (!earlier) {
                consume(block);
            }
            if (block) {
                auto free = std::ranges::find_if(pendingInput_, [](const PendingInput& pending) { return !pending.block; });
                if (free == pendingInput_.end() || nextInputSequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
                    std::terminate();
                }
                free->block = std::move(block);
                free->sequence = nextInputSequence_++;
                ++pendingInputCount_;
            }
        }
        if (!received) {
            break;
        }
    }
    // If the budget was exhausted, the run loop immediately takes another
    // bounded turn (and yields after its outer budget); queued work is not
    // dependent on a fresh producer notification.
    return requestMailbox_->finishDrain() || progress;
}

bool Http3WorkerServer::pumpScheduler() noexcept {
    if (!scheduler_) {
        return false;
    }
    bool progress = false;
    for (std::size_t count = 0; count < kPumpBudget; ++count) {
        const auto step = scheduler_->step();
        if (step.kind == Http3WorkerMailboxScheduler::StepKind::kIdle) {
            break;
        }
        if (step.kind == Http3WorkerMailboxScheduler::StepKind::kWrongWorker) {
            std::terminate();
        }
        progress = true;
        if (step.notifyPeer) {
            networkWake_.notify(networkWake_.context);
        }
        if (step.kind != Http3WorkerMailboxScheduler::StepKind::kTransportIntent) {
            continue;
        }
        const auto found = std::ranges::find_if(slots_, [&step](const Slot& item) {
            return item.registration.token == step.connection;
        });
        if (found == slots_.end() || found->channel == nullptr) {
            std::terminate();
        }
        const auto published = found->channel->publishIntent(found->identity, step.intent);
        if (published == Http3ServerConnectionChannel::Status::kFull) {
            if (!scheduler_->parkIntentForControlCapacity(
                    step.connection, step.intent.token)) {
                std::terminate();
            }
        } else if (published != Http3ServerConnectionChannel::Status::kPublished) {
            std::terminate();
        }
    }
    return progress;
}

bool Http3WorkerServer::publishDrainCompletions() noexcept {
    bool progress = false;
    for (auto& slot : slots_) {
        if (!slot.reserved || !slot.attached || !slot.admissionSealed ||
            slot.drainCompletePublished || slot.connection == nullptr ||
            slot.retirementStarted || slot.transportRetired) {
            continue;
        }
        if (!slot.connection->drainReady(slot.expectedAdmittedRequests)) {
            continue;
        }
        const auto status = slot.channel->publishDrainComplete(slot.identity);
        if (status == Http3ServerConnectionChannel::Status::kPublished) {
            slot.drainCompletePublished = true;
            progress = true;
        } else if (status != Http3ServerConnectionChannel::Status::kWrongState) {
            std::terminate();
        }
    }
    return progress;
}

Http3WorkerServer::Slot* Http3WorkerServer::findSlot(Http3StreamMessageId id) noexcept {
    const auto found = std::ranges::find_if(slots_, [&id](const Slot& slot) {
        return slot.attached && slot.identity.epoch == id.epoch &&
               slot.identity.connectionGeneration == id.connectionGeneration;
    });
    return found == slots_.end() ? nullptr : &*found;
}

Http3WorkerServer::Slot* Http3WorkerServer::findSlot(
    Http3ServerConnectionChannel::Identity identity) noexcept {
    const auto found = std::ranges::find_if(slots_, [&identity](const Slot& slot) {
        return slot.reserved && slot.identity == identity;
    });
    return found == slots_.end() ? nullptr : &*found;
}

bool Http3WorkerServer::constructConnection(
    Slot& slot, const Http3ServerConnectionChannel::Bind& bind) noexcept {
    if (!slot.reserved || slot.connection != nullptr || bind.identity != slot.identity ||
        bind.metadata.remoteAddress.empty() || bind.metadata.remotePort == 0 ||
        !options_.maxConnections ||
        activeConnections_.load(std::memory_order_relaxed) >= *options_.maxConnections) {
        return false;
    }
    try {
        slot.remoteAddress = bind.metadata.remoteAddress;
        slot.clientCertificateSubject = bind.metadata.clientCertificateSubject;
        slot.remotePort = bind.metadata.remotePort;
        auto services = capabilities_.contextServices(stopToken_).withTlsTransport(slot.remoteAddress, slot.clientCertificateSubject, slot.remotePort);
        const auto maxRequests = options_.maxRequestsPerConnection.value_or(0);
        if (maxRequests == 0) {
            return false;
        }
        const auto maxTrackedStreams = http3WorkerTrackedStreamCapacity(maxRequests);
        Http3ServerConnectionConfig config{
            .epoch = bind.identity.epoch,
            .connectionGeneration = bind.identity.connectionGeneration,
            .session = {
                .maxBufferedBodyBytes = options_.maxBufferedBodyBytes,
                .maxStreamBodyBytes = options_.maxStreamBodyBytes,
                .maxLiveStreams = maxTrackedStreams,
                .maxBufferedBytesInFlight = kWorkerBodyBudgetBytes,
                .connection = {.maxActiveStreams = maxTrackedStreams, .qpackMaxTableCapacity = static_cast<std::size_t>(bind.settings.qpackMaxTableCapacity), .qpackBlockedStreams = static_cast<std::size_t>(bind.settings.qpackBlockedStreams), .enableConnectProtocol = bind.settings.enableConnectProtocol, .enableDatagrams = bind.settings.h3Datagram},
                .maxQuicDatagramPayloadBytes = bind.maxQuicDatagramPayloadBytes,
            },
            .maxTrackedStreams = maxTrackedStreams,
            .connectionScanner = &connectionScanner_,
            .executor = executor_,
            .datagramOutput = {.context = &slot, .send = [](void* raw, std::uint64_t streamId, std::span<const std::byte> bytes) {
                                   auto& output_slot = *static_cast<Slot*>(raw);
                                   if (!output_slot.channel || output_slot.retirementStarted || output_slot.transportRetired || output_slot.workerPublicationsClosed) {
                                       throw std::runtime_error("HTTP Datagram connection is closed");
                                   }
                                   static_cast<void>(output_slot.channel->publishResponseDatagram(output_slot.identity, streamId, bytes));
                               }},
        };
        slot.connection = makePmrObject<Http3ServerConnection>(memory_.resource(), routes_,
            memory_, services, options_, responseMailbox_, slot.registration.activation,
            bodyBudget_, config);
        if (!scheduler_ || slot.channel->attach(*scheduler_, slot.registration, *slot.connection) !=
                               Http3ServerConnectionChannel::Status::kPublished) {
            // The just-created owner has not been attached or retired; discarding
            // it would violate its transport-retirement contract.
            std::terminate();
        }
        slot.attached = true;
        activeConnections_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (...) {
        if (slot.connection != nullptr) {
            std::terminate();
        }
        return false;
    }
}

void Http3WorkerServer::beginSlotRetirement(Slot& slot) noexcept {
    if (slot.connection == nullptr || slot.retirementStarted) {
        return;
    }
    slot.retirementStarted = true;
    (void)slot.connection->requestStop();
    if (!scheduler_ || !scheduler_->beginRetirement(slot.registration.token)) {
        std::terminate();
    }
}

bool Http3WorkerServer::finishTerminalSlots() noexcept {
    bool progress = false;
    for (auto& slot : slots_) {
        if (slot.channel == nullptr || !slot.reserved) {
            continue;
        }
        if (slot.revokeAcknowledged) {
            const auto status = slot.channel->closeWorkerPublications(slot.identity);
            if (status == Http3ServerConnectionChannel::Status::kPublished) {
                slot.reserved = false;
                slot.revokeAcknowledged = false;
                slot.workerPublicationsClosed = true;
                slot.registration = {};
                slot.identity = {};
                progress = true;
            } else if (status != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
            }
            continue;
        }
        if (slot.rejected && slot.transportRetired) {
            slot.workerRetirementComplete = true;
        }
        progress = finalizeWorkerPublications(slot) || progress;
    }
    return progress;
}

bool Http3WorkerServer::finalizeWorkerPublications(Slot& slot) noexcept {
    if (!slot.reserved || !slot.workerRetirementComplete) {
        return false;
    }
    bool progress = false;
    if (!slot.workerPublicationsClosed) {
        const auto closed = slot.channel->closeWorkerPublications(slot.identity);
        if (closed == Http3ServerConnectionChannel::Status::kPublished) {
            slot.workerPublicationsClosed = true;
            progress = true;
        } else if (closed != Http3ServerConnectionChannel::Status::kWrongState) {
            std::terminate();
        }
    }
    if (slot.workerPublicationsClosed && !slot.workerFinalized) {
        const auto finalized = slot.channel->publishWorkerFinalized(slot.identity);
        if (finalized == Http3ServerConnectionChannel::Status::kPublished) {
            slot.workerFinalized = true;
            progress = true;
        } else if (finalized != Http3ServerConnectionChannel::Status::kWrongState) {
            std::terminate();
        }
    }
    return progress;
}

Task<void> Http3WorkerServer::join_retired_slot(Slot& slot) {
    co_await slot.connection->join();
    if (!scheduler_ || !scheduler_->retire(slot.registration.token)) {
        std::terminate();
    }
    slot.connection.reset();
    activeConnections_.fetch_sub(1, std::memory_order_relaxed);
    slot.transportRetired = false;
    slot.workerRetirementComplete = true;
    (void)notification_.notify();
}

void Http3WorkerServer::finishStoppedSlots() noexcept {
    if (!scheduler_) {
        return;
    }
    for (auto& slot : slots_) {
        if (slot.connection == nullptr || !slot.retirementStarted ||
            !slot.transportRetired || slot.retirementTaskStarted ||
            slot.connection->pendingTransportIntentCount() != 0) {
            continue;
        }
        slot.retirementTaskStarted = true;
        try {
            retirementTasks_.spawn(join_retired_slot(slot));
        } catch (...) {
            std::terminate();
        }
    }
}

}  // namespace ruvia::detail
