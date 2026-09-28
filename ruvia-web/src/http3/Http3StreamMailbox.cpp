#include "ruvia/web/detail/http3/Http3StreamMailbox.h"

#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

Http3StreamMailbox::DataReservation::DataReservation(Http3StreamMailbox* owner,
    std::uint32_t index, Http3StreamMessageId id) noexcept
    : owner_(owner),
      index_(index),
      id_(id) {}

Http3StreamMailbox::DataReservation::~DataReservation() {
    abort();
}

Http3StreamMailbox::DataReservation::DataReservation(DataReservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      index_(other.index_),
      id_(other.id_) {}

Http3StreamMailbox::DataReservation& Http3StreamMailbox::DataReservation::operator=(
    DataReservation&& other) noexcept {
    if (this != &other) {
        abort();
        owner_ = std::exchange(other.owner_, nullptr);
        index_ = other.index_;
        id_ = other.id_;
    }
    return *this;
}

std::span<std::byte> Http3StreamMailbox::DataReservation::writableBytes() const noexcept {
    return owner_ == nullptr ? std::span<std::byte>{}
                             : std::span<std::byte>{owner_->blocks_[index_].bytes.data(),
                                   kMaxBlockBytes};
}

Http3StreamMailbox::CommitResult Http3StreamMailbox::DataReservation::commit(
    std::size_t size) noexcept {
    if (owner_ == nullptr) {
        return CommitResult::kInactive;
    }
    if (size == 0) {
        abort();
        return CommitResult::kZeroBytes;
    }
    if (size > kMaxBlockBytes) {
        abort();
        return CommitResult::kTooLarge;
    }
    auto* owner = std::exchange(owner_, nullptr);
    return owner->commitDataReservation(index_, id_, size);
}

void Http3StreamMailbox::DataReservation::abort() noexcept {
    if (owner_ != nullptr) {
        auto* owner = std::exchange(owner_, nullptr);
        owner->abortDataReservation(index_);
    }
}

Http3StreamMailbox::BorrowedBlock::BorrowedBlock(Http3StreamMailbox* owner,
    std::uint32_t index, std::size_t size, Http3StreamMessageId id) noexcept
    : owner_(owner),
      index_(index),
      size_(size),
      id_(id) {}

Http3StreamMailbox::BorrowedBlock::~BorrowedBlock() {
    release();
}

Http3StreamMailbox::BorrowedBlock::BorrowedBlock(BorrowedBlock&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      index_(other.index_),
      size_(other.size_),
      id_(other.id_) {}

Http3StreamMailbox::BorrowedBlock& Http3StreamMailbox::BorrowedBlock::operator=(
    BorrowedBlock&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        index_ = other.index_;
        size_ = other.size_;
        id_ = other.id_;
    }
    return *this;
}

std::span<const std::byte> Http3StreamMailbox::BorrowedBlock::bytes() const noexcept {
    return owner_ == nullptr ? std::span<const std::byte>{}
                             : std::span<const std::byte>{owner_->blocks_[index_].bytes.data(), size_};
}

void Http3StreamMailbox::BorrowedBlock::release() noexcept {
    if (owner_ != nullptr) {
        auto* owner = std::exchange(owner_, nullptr);
        owner->returnBlock(index_);
        owner->outstandingBorrows_.fetch_sub(1, std::memory_order_release);
    }
}

Http3StreamMailbox::Http3StreamMailbox(std::uint32_t blockCount, std::uint32_t dataSlots,
    std::uint32_t controlSlots, std::pmr::memory_resource* resource,
    Http3StreamMailboxCapacityNotifier capacityNotifier)
    : blockCount_(blockCount),
      dataCapacity_(dataSlots),
      controlCapacity_(controlSlots),
      resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      capacityNotifier_(capacityNotifier),
      blocks_(resource_),
      dataSlots_(resource_),
      controlSlots_(resource_),
      freeBlocks_(resource_),
      returns_(resource_),
      freeCount_(blockCount) {
    if (blockCount == 0 || dataSlots == 0 || controlSlots == 0) {
        throw std::invalid_argument("HTTP/3 mailbox capacities must be nonzero");
    }
    blocks_.resize(blockCount_);
    dataSlots_.resize(dataCapacity_);
    controlSlots_.resize(controlCapacity_);
    freeBlocks_.resize(blockCount_);
    returns_.resize(blockCount_);
    for (std::uint32_t i = 0; i < blockCount_; ++i) {
        freeBlocks_[i] = blockCount_ - i - 1;
    }
}

Http3StreamMailbox::~Http3StreamMailbox() {
    (void)stop();
    if (!publishersQuiescent() || outstandingBorrows_.load(std::memory_order_acquire) != 0) {
        std::terminate();
    }
}

Http3StreamMailbox::SendResult Http3StreamMailbox::trySend(Http3StreamMessageId id,
    std::span<const std::byte> bytes) noexcept {
    if (dataReservationActive_) {
        return SendResult::kReservationActive;
    }
    if (!beginPublish()) {
        return SendResult::kStopped;
    }
    SendResult result = SendResult::kSent;
    if (bytes.size() > kMaxBlockBytes) {
        result = SendResult::kTooLarge;
    } else {
        const auto read = dataRead_.load(std::memory_order_acquire);
        if (dataWrite_ - read >= dataCapacity_) {
            result = SendResult::kFull;
        } else if (freeCount_ == 0) {
            result = SendResult::kNoBlock;
        } else {
            const std::uint32_t index = freeBlocks_[--freeCount_];
            if (!bytes.empty()) {
                std::memcpy(blocks_[index].bytes.data(), bytes.data(), bytes.size());
            }
            auto& slot = dataSlots_[dataWrite_ % dataCapacity_];
            slot.block = index;
            slot.size = static_cast<std::uint32_t>(bytes.size());
            slot.id = id;
            ++dataWrite_;
            dataPublished_.store(dataWrite_, std::memory_order_release);
            if (claimNotification()) {
                result = SendResult::kSentNotifyPeer;
            }
        }
    }
    endPublish();
    return result;
}

Http3StreamMailbox::ReservationResult Http3StreamMailbox::reserveData(
    Http3StreamMessageId id, DataReservation& reservation) noexcept {
    if (reservation.owner_ != nullptr || dataReservationActive_) {
        return ReservationResult::kReservationActive;
    }
    if (!beginPublish()) {
        return ReservationResult::kStopped;
    }

    const auto read = dataRead_.load(std::memory_order_acquire);
    if (dataWrite_ - read >= dataCapacity_) {
        endPublish();
        return ReservationResult::kFull;
    }
    if (freeCount_ == 0) {
        endPublish();
        return ReservationResult::kNoBlock;
    }

    const std::uint32_t index = freeBlocks_[--freeCount_];
    dataReservationActive_ = true;
    reservation = DataReservation(this, index, id);
    return ReservationResult::kReserved;
}

Http3StreamMailbox::ControlResult Http3StreamMailbox::trySendControl(
    const Http3StreamControl& event) noexcept {
    if (!beginPublish()) {
        return ControlResult::kStopped;
    }
    ControlResult result = ControlResult::kSent;
    const auto read = controlRead_.load(std::memory_order_acquire);
    if (controlWrite_ - read >= controlCapacity_) {
        result = ControlResult::kFull;
    } else {
        controlSlots_[controlWrite_ % controlCapacity_] = event;
        ++controlWrite_;
        controlPublished_.store(controlWrite_, std::memory_order_release);
        if (claimNotification()) {
            result = ControlResult::kSentNotifyPeer;
        }
    }
    endPublish();
    return result;
}

bool Http3StreamMailbox::tryReceive(BorrowedBlock& block) noexcept {
    block.release();
    const auto published = dataPublished_.load(std::memory_order_acquire);
    if (dataReadLocal_ == published) {
        return false;
    }
    const auto& slot = dataSlots_[dataReadLocal_ % dataCapacity_];
    outstandingBorrows_.fetch_add(1, std::memory_order_relaxed);
    block = BorrowedBlock(this, slot.block, slot.size, slot.id);
    ++dataReadLocal_;
    dataRead_.store(dataReadLocal_, std::memory_order_release);
    notifyCapacity(kDataCapacity);
    return true;
}

bool Http3StreamMailbox::tryReceiveControl(Http3StreamControl& event) noexcept {
    const auto published = controlPublished_.load(std::memory_order_acquire);
    if (controlReadLocal_ == published) {
        return false;
    }
    event = controlSlots_[controlReadLocal_ % controlCapacity_];
    ++controlReadLocal_;
    controlRead_.store(controlReadLocal_, std::memory_order_release);
    notifyCapacity(kControlCapacity);
    return true;
}

Http3StreamMailbox::CapacityWaitResult Http3StreamMailbox::armCapacityWait(
    CapacityInterest interest) noexcept {
    std::uint8_t mask;
    switch (interest) {
        case CapacityInterest::kData:
            mask = kDataCapacity;
            break;
        case CapacityInterest::kControl:
            mask = kControlCapacity;
            break;
        case CapacityInterest::kAny:
            mask = kDataCapacity | kControlCapacity;
            break;
        default:
            return CapacityWaitResult::kInvalidInterest;
    }
    if (dataReservationActive_ && (mask & kDataCapacity) != 0) {
        return CapacityWaitResult::kUnavailable;
    }
    if (stopped()) {
        return CapacityWaitResult::kStopped;
    }
    if (capacityNotifier_.notify == nullptr) {
        return CapacityWaitResult::kUnavailable;
    }

    // Each registration gets a generation. The consumer can consume this whole
    // registration, but a stale CAS can never erase a subsequently armed one.
    auto state = capacityWait_.load(std::memory_order_acquire);
    std::uint64_t registration;
    for (;;) {
        registration = ((state & ~kCapacityInterestMask) + kCapacityWaitGenerationStep) | mask;
        if (capacityWait_.compare_exchange_weak(state, registration,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    if ((mask & kDataCapacity) != 0) {
        (void)drainReturns();
    }
    const bool dataReady = (mask & kDataCapacity) != 0 && freeCount_ != 0 &&
                           dataWrite_ - dataRead_.load(std::memory_order_acquire) < dataCapacity_;
    const bool controlReady = (mask & kControlCapacity) != 0 &&
                              controlWrite_ - controlRead_.load(std::memory_order_acquire) < controlCapacity_;
    const bool ready = dataReady || controlReady;
    const bool closed = stopped();
    if (ready || closed) {
        clearCapacityWait(registration);
        return closed ? CapacityWaitResult::kStopped : CapacityWaitResult::kReady;
    }
    return CapacityWaitResult::kArmed;
}

void Http3StreamMailbox::clearCapacityWait(std::uint64_t registration) noexcept {
    auto expected = registration;
    (void)capacityWait_.compare_exchange_strong(expected,
        registration & ~kCapacityInterestMask, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

void Http3StreamMailbox::notifyCapacity(std::uint8_t mask) noexcept {
    if (capacityNotifier_.notify == nullptr) {
        return;
    }

    // This RMW pairs with armCapacityWait() even when no matching waiter exists.
    // If arming follows this operation, the producer's recheck observes the
    // preceding capacity publication; if arming precedes it, this side consumes
    // the registration and queues its wakeup. Keep both sides acq_rel RMWs.
    auto state = capacityWait_.fetch_add(0, std::memory_order_acq_rel);
    for (;;) {
        if (((state & kCapacityInterestMask) & mask) == 0) {
            return;
        }
        if (capacityWait_.compare_exchange_weak(state,
                state & ~kCapacityInterestMask, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Consume the entire registration: Any is one waiter, not one per lane.
            capacityNotifier_.notify(capacityNotifier_.context);
            return;
        }
    }
}

std::uint32_t Http3StreamMailbox::drainReturns() noexcept {
    if (dataReservationActive_) {
        return 0;
    }
    const auto published = returnPublished_.load(std::memory_order_acquire);
    std::uint32_t drained = 0;
    while (returnReadLocal_ != published) {
        freeBlocks_[freeCount_++] = returns_[returnReadLocal_ % blockCount_];
        ++returnReadLocal_;
        ++drained;
    }
    returnRead_.store(returnReadLocal_, std::memory_order_release);
    return drained;
}

Http3StreamMailbox::CommitResult Http3StreamMailbox::commitDataReservation(
    std::uint32_t index, Http3StreamMessageId id, std::size_t size) noexcept {
    auto& slot = dataSlots_[dataWrite_ % dataCapacity_];
    slot.block = index;
    slot.size = static_cast<std::uint32_t>(size);
    slot.id = id;
    ++dataWrite_;
    dataPublished_.store(dataWrite_, std::memory_order_release);
    dataReservationActive_ = false;
    const auto result = claimNotification() ? CommitResult::kSentNotifyPeer
                                            : CommitResult::kSent;
    endPublish();
    return result;
}

void Http3StreamMailbox::abortDataReservation(std::uint32_t index) noexcept {
    freeBlocks_[freeCount_++] = index;
    dataReservationActive_ = false;
    endPublish();
}

void Http3StreamMailbox::returnBlock(std::uint32_t index) noexcept {
    const auto read = returnRead_.load(std::memory_order_acquire);
    if (returnWrite_ - read >= blockCount_) {
        std::terminate();
    }
    returns_[returnWrite_ % blockCount_] = index;
    ++returnWrite_;
    returnPublished_.store(returnWrite_, std::memory_order_release);
    notifyCapacity(kDataCapacity);
}

bool Http3StreamMailbox::beginPublish() noexcept {
    constexpr std::uint64_t kClosed = std::uint64_t{1} << 63;
    auto state = publishState_.load(std::memory_order_acquire);
    for (;;) {
        if ((state & kClosed) != 0) {
            return false;
        }
        if (publishState_.compare_exchange_weak(state, state + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void Http3StreamMailbox::endPublish() noexcept {
    publishState_.fetch_sub(1, std::memory_order_release);
}

bool Http3StreamMailbox::stop() noexcept {
    constexpr std::uint64_t kClosed = std::uint64_t{1} << 63;
    publishState_.fetch_or(kClosed, std::memory_order_acq_rel);
    notifyCapacity(kDataCapacity | kControlCapacity);
    return publishersQuiescent();
}
bool Http3StreamMailbox::stopped() const noexcept {
    constexpr std::uint64_t kClosed = std::uint64_t{1} << 63;
    return (publishState_.load(std::memory_order_acquire) & kClosed) != 0;
}
bool Http3StreamMailbox::publishersQuiescent() const noexcept {
    constexpr std::uint64_t kClosed = std::uint64_t{1} << 63;
    const auto state = publishState_.load(std::memory_order_acquire);
    return (state & kClosed) != 0 && (state & ~kClosed) == 0;
}
bool Http3StreamMailbox::finishDrain() noexcept {
    // Read-modify-write pairs with the producer's release exchange. If a
    // publication claimed an already-pending wakeup, its slot publication
    // happens-before the recheck; a plain store(false) would not establish it.
    (void)notification_.exchange(false, std::memory_order_acq_rel);
    return hasPending();
}
std::uint32_t Http3StreamMailbox::blockCapacity() const noexcept {
    return blockCount_;
}
bool Http3StreamMailbox::claimNotification() noexcept {
    return !notification_.exchange(true, std::memory_order_acq_rel);
}
bool Http3StreamMailbox::hasPending() const noexcept {
    return dataReadLocal_ != dataPublished_.load(std::memory_order_acquire) ||
           controlReadLocal_ != controlPublished_.load(std::memory_order_acquire);
}

}  // namespace ruvia::detail
