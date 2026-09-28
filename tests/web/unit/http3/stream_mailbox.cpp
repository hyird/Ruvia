#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <semaphore>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/web/detail/http3/Http3StreamMailbox.h"

#include "test_harness.h"

namespace {
using Mailbox = ruvia::detail::Http3StreamMailbox;
using Id = ruvia::detail::Http3StreamMessageId;
using Control = ruvia::detail::Http3StreamControl;
using CapacityNotifier = ruvia::detail::Http3StreamMailboxCapacityNotifier;

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocated{};
    std::size_t freed{};
    std::size_t allocationCalls{};
    std::size_t allowedCalls{std::numeric_limits<std::size_t>::max()};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes >= 32 && allocationCalls++ >= allowedCalls) {
            throw std::bad_alloc();
        }
        void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        allocated += bytes;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        freed += bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::array<std::byte, 4> payload(std::uint32_t n) {
    return {std::byte(n & 0xff), std::byte((n >> 8) & 0xff), std::byte{0x5a}, std::byte{0xa5}};
}
}  // namespace

RUVIA_TEST(http3StreamMailboxCapacityWaitDistinguishesSlotsBlocksControlAndStop) {
    using Interest = Mailbox::CapacityInterest;
    using Wait = Mailbox::CapacityWaitResult;
    CountingResource memory;
    unsigned notifications = 0;
    {
        Mailbox mailbox(1, 1, 1, &memory, {.context = &notifications, .notify = [](void* state) noexcept { ++*static_cast<unsigned*>(state); }});
        const auto allocationCalls = memory.allocationCalls;
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kData) == Wait::kReady);
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 0}, payload(1)) == Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kData) == Wait::kArmed);
        Mailbox::BorrowedBlock block;
        RUVIA_CHECK(mailbox.tryReceive(block));
        RUVIA_CHECK_EQ(notifications, 1U);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kData) == Wait::kArmed);
        Control control{.kind = Control::Kind::kWritable};
        RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kSent);
        RUVIA_CHECK(mailbox.tryReceiveControl(control));
        RUVIA_CHECK_EQ(notifications, 1U);
        block.release();
        RUVIA_CHECK_EQ(notifications, 2U);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kData) == Wait::kReady);
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 4}, payload(2)) == Mailbox::SendResult::kSent);
        RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kSent);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kControl) == Wait::kArmed);
        RUVIA_CHECK(mailbox.tryReceive(block));
        block.release();
        RUVIA_CHECK_EQ(notifications, 2U);
        RUVIA_CHECK(mailbox.tryReceiveControl(control));
        RUVIA_CHECK_EQ(notifications, 3U);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kControl) == Wait::kReady);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kData) == Wait::kReady);
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 8}, payload(3)) == Mailbox::SendResult::kSent);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kData) == Wait::kArmed);
        RUVIA_CHECK(mailbox.stop());
        RUVIA_CHECK_EQ(notifications, 4U);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kData) == Wait::kStopped);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kControl) == Wait::kStopped);
        RUVIA_CHECK(mailbox.stop());
        RUVIA_CHECK(mailbox.tryReceive(block));
        block.release();
        RUVIA_CHECK_EQ(notifications, 4U);
        RUVIA_CHECK(mailbox.armCapacityWait(static_cast<Interest>(0xff)) == Wait::kInvalidInterest);
        RUVIA_CHECK_EQ(memory.allocationCalls, allocationCalls);
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    Mailbox manual(1, 1, 1);
    RUVIA_CHECK(manual.armCapacityWait(Interest::kData) == Wait::kUnavailable);
    RUVIA_CHECK(manual.armCapacityWait(Interest::kControl) == Wait::kUnavailable);
    RUVIA_CHECK(manual.armCapacityWait(Interest::kAny) == Wait::kUnavailable);
}

RUVIA_TEST(http3StreamMailboxAnyCapacityWaitWakesForEitherLaneAndConsumesOneRegistration) {
    using Interest = Mailbox::CapacityInterest;
    using Wait = Mailbox::CapacityWaitResult;
    unsigned notifications = 0;
    const auto notifier = CapacityNotifier{
        .context = &notifications,
        .notify = [](void* state) noexcept { ++*static_cast<unsigned*>(state); }};
    CountingResource memory;
    {
        Mailbox mailbox(2, 1, 1, &memory, notifier);
        const auto allocationCalls = memory.allocationCalls;
        Control control{.kind = Control::Kind::kWritable};
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 0}, payload(1)) == Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kSent);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kAny) == Wait::kArmed);

        // DATA capacity opens while the CONTROL queue remains full.
        Mailbox::BorrowedBlock block;
        RUVIA_CHECK(mailbox.tryReceive(block));
        RUVIA_CHECK_EQ(notifications, 1U);
        RUVIA_CHECK(mailbox.tryReceiveControl(control));
        block.release();
        RUVIA_CHECK(mailbox.drainReturns() == 1U);
        RUVIA_CHECK_EQ(notifications, 1U);
        RUVIA_CHECK(mailbox.stop());
        RUVIA_CHECK_EQ(memory.allocationCalls, allocationCalls);
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    {
        Mailbox mailbox(2, 1, 1, std::pmr::get_default_resource(), notifier);
        Control control{.kind = Control::Kind::kWritable};
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 0}, payload(2)) == Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kSent);
        Mailbox::BorrowedBlock block;
        RUVIA_CHECK(mailbox.tryReceive(block));
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kAny) == Wait::kReady);
        RUVIA_CHECK_EQ(notifications, 1U);
        block.release();
        RUVIA_CHECK(mailbox.tryReceiveControl(control));
        RUVIA_CHECK(mailbox.drainReturns() == 1U);
        RUVIA_CHECK(mailbox.stop());
    }
    {
        Mailbox mailbox(1, 1, 1, std::pmr::get_default_resource(), notifier);
        Control control{.kind = Control::Kind::kWritable};
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 0}, payload(3)) == Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kSent);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kAny) == Wait::kArmed);
        RUVIA_CHECK(mailbox.tryReceiveControl(control));
        RUVIA_CHECK_EQ(notifications, 2U);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kAny) == Wait::kReady);
        Mailbox::BorrowedBlock block;
        RUVIA_CHECK(mailbox.tryReceive(block));
        block.release();
        RUVIA_CHECK(mailbox.drainReturns() == 1U);
        RUVIA_CHECK_EQ(notifications, 2U);
        RUVIA_CHECK(mailbox.stop());
    }
    {
        Mailbox mailbox(1, 1, 1, std::pmr::get_default_resource(), notifier);
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 0}, payload(3)) == Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(mailbox.trySendControl({.kind = Control::Kind::kWritable}) ==
                    Mailbox::ControlResult::kSent);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kAny) == Wait::kArmed);
        RUVIA_CHECK(mailbox.stop());
        RUVIA_CHECK_EQ(notifications, 3U);
        RUVIA_CHECK(mailbox.armCapacityWait(Interest::kAny) == Wait::kStopped);
    }
}

RUVIA_TEST(http3StreamMailboxCapacityWakeAllowsProducerToSleepAcrossRepeatedReturns) {
    using Wait = Mailbox::CapacityWaitResult;
    CountingResource memory;
    std::counting_semaphore<512> capacityReady(0);
    std::counting_semaphore<128> dataReady(0);
    std::binary_semaphore allowFirstReturn(0);
    std::binary_semaphore producerEnteringWait(0);
    std::atomic<bool> ok{true};
    {
        Mailbox mailbox(1, 1, 1, &memory, {.context = &capacityReady, .notify = [](void* state) noexcept { static_cast<std::counting_semaphore<512>*>(state)->release(); }});
        const auto initialCalls = memory.allocationCalls;
        std::jthread consumer([&] {
            for (unsigned i = 0; i < 64; ++i) {
                if (!dataReady.try_acquire_for(std::chrono::seconds(1))) {
                    ok = false;
                    break;
                }
                Mailbox::BorrowedBlock block;
                if (!mailbox.tryReceive(block)) {
                    ok = false;
                    break;
                }
                const auto expected = payload(i);
                if (block.id().streamId != i || !std::equal(block.bytes().begin(), block.bytes().end(), expected.begin(), expected.end())) {
                    ok = false;
                }
                if (i == 0 &&
                    (!allowFirstReturn.try_acquire_for(std::chrono::seconds(1)) ||
                        !producerEnteringWait.try_acquire_for(std::chrono::seconds(1)))) {
                    ok = false;
                    break;
                }
                if (i == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                (void)mailbox.finishDrain();
            }
            if (!ok) {
                (void)mailbox.stop();
            }
        });
        bool firstWait = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (unsigned i = 0; i < 64 && ok; ++i) {
            bool sent = false;
            while (ok && std::chrono::steady_clock::now() < deadline) {
                const auto result = mailbox.trySend(Id{1, 2, i}, payload(i));
                if (result == Mailbox::SendResult::kSent || result == Mailbox::SendResult::kSentNotifyPeer) {
                    sent = true;
                    dataReady.release();
                    break;
                }
                if (result != Mailbox::SendResult::kFull && result != Mailbox::SendResult::kNoBlock) {
                    ok = false;
                    break;
                }
                const auto wait = mailbox.armCapacityWait(Mailbox::CapacityInterest::kData);
                if (wait == Wait::kReady) {
                    continue;
                }
                if (wait != Wait::kArmed) {
                    ok = false;
                    break;
                }
                if (firstWait) {
                    firstWait = false;
                    producerEnteringWait.release();
                    allowFirstReturn.release();
                }
                if (!capacityReady.try_acquire_for(std::chrono::seconds(1))) {
                    ok = false;
                    break;
                }
            }
            if (!sent) {
                ok = false;
            }
        }
        if (!ok) {
            (void)mailbox.stop();
        }
        consumer.join();
        (void)mailbox.drainReturns();
        RUVIA_CHECK(mailbox.stop());
        RUVIA_CHECK(ok && !firstWait);
        RUVIA_CHECK_EQ(memory.allocationCalls, initialCalls);
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
}

RUVIA_TEST(http3StreamMailboxCapacityArmRacesWithReturnAndStopWithoutLosingWake) {
    using Wait = Mailbox::CapacityWaitResult;
    for (unsigned scenario = 0; scenario < 64; ++scenario) {
        std::binary_semaphore start(0);
        std::counting_semaphore<4> wake(0);
        Mailbox mailbox(1, 1, 1, nullptr, {.context = &wake, .notify = [](void* state) noexcept { static_cast<std::counting_semaphore<4>*>(state)->release(); }});
        const bool control = (scenario & 1U) != 0;
        const bool stop = (scenario & 2U) != 0;
        (void)mailbox.trySend(Id{1, 2, 0}, payload(0));
        (void)mailbox.trySendControl({.kind = Control::Kind::kWritable});
        std::atomic<bool> consumed{false};
        std::jthread consumer([&] {
            if (!start.try_acquire_for(std::chrono::seconds(1))) {
                return;
            }
            if (stop) {
                consumed = mailbox.stop();
            } else if (control) {
                Control event;
                consumed = mailbox.tryReceiveControl(event);
            } else {
                Mailbox::BorrowedBlock block;
                consumed = mailbox.tryReceive(block);
            }
        });
        start.release();
        const auto result = mailbox.armCapacityWait(Mailbox::CapacityInterest::kAny);
        if (result == Wait::kArmed) {
            RUVIA_CHECK(wake.try_acquire_for(std::chrono::seconds(1)));
        } else {
            RUVIA_CHECK(result == Wait::kReady || result == Wait::kStopped);
        }
        consumer.join();
        RUVIA_CHECK(consumed);
        RUVIA_CHECK(mailbox.stop());
    }
}

RUVIA_TEST(http3StreamMailboxConstructionFailureReturnsEarlierAllocations) {
    CountingResource baseline;
    {
        Mailbox mailbox(2, 1, 1, &baseline);
    }
    RUVIA_CHECK(baseline.allocationCalls > 0);
    RUVIA_CHECK(baseline.allocated == baseline.freed);
    for (std::size_t successfulCalls = 0; successfulCalls < baseline.allocationCalls;
        ++successfulCalls) {
        CountingResource memory;
        memory.allowedCalls = successfulCalls;
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { Mailbox mailbox(2, 1, 1, &memory); }));
        RUVIA_CHECK(memory.allocated == memory.freed);
    }
}

RUVIA_TEST(http3StreamMailboxReusesBoundedBlocksAndReturnsStorageAtRetirement) {
    CountingResource memory;
    {
        Mailbox mailbox(2, 1, 1, &memory);
        const auto setupBytes = memory.allocated;
        RUVIA_CHECK(setupBytes > 0);
        Mailbox::BorrowedBlock retained;
        Mailbox::BorrowedBlock current;
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 3}, payload(1)) == Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(mailbox.tryReceive(retained));
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 4}, payload(2)) == Mailbox::SendResult::kSent);
        RUVIA_CHECK(mailbox.tryReceive(current));
        RUVIA_CHECK(retained.bytes()[0] == payload(1)[0]);
        current.release();
        retained.release();
        RUVIA_CHECK(mailbox.drainReturns() == 2);
        for (unsigned i = 0; i < 300; ++i) {
            const auto result = mailbox.trySend(Id{1, 2, i}, payload(i));
            RUVIA_CHECK(result == Mailbox::SendResult::kSent ||
                        result == Mailbox::SendResult::kSentNotifyPeer);
            RUVIA_CHECK(mailbox.tryReceive(current));
            RUVIA_CHECK(current.bytes()[0] == payload(i)[0]);
            current.release();
            RUVIA_CHECK(mailbox.drainReturns() == 1);
        }
        RUVIA_CHECK(memory.allocated == setupBytes);
        RUVIA_CHECK(memory.freed == 0);
        RUVIA_CHECK(mailbox.stop());
    }
    RUVIA_CHECK(memory.allocated == memory.freed);
}

RUVIA_TEST(http3StreamMailboxWritableReservationAbortAndPartialCommit) {
    CountingResource memory;
    {
        Mailbox mailbox(1, 1, 2, &memory);
        Mailbox::DataReservation reservation;
        const Id reservedId{0x1234, 0x5678, 0x9abc};
        RUVIA_CHECK(mailbox.reserveData(reservedId, reservation) ==
                    Mailbox::ReservationResult::kReserved);
        RUVIA_CHECK(reservation);
        RUVIA_CHECK_EQ(reservation.writableBytes().size(), Mailbox::kMaxBlockBytes);
        std::fill(reservation.writableBytes().begin(), reservation.writableBytes().end(),
            std::byte{0x7a});

        // An outstanding preread pins its DATA slot, but not the independent CONTROL lane.
        RUVIA_CHECK(mailbox.trySend(Id{1, 2, 3}, payload(4)) ==
                    Mailbox::SendResult::kReservationActive);
        Control event{.kind = Control::Kind::kWritable,
            .id = Id{4, 5, 6},
            .value = 7};
        RUVIA_CHECK(mailbox.trySendControl(event) == Mailbox::ControlResult::kSentNotifyPeer);
        Control receivedControl;
        RUVIA_CHECK(mailbox.tryReceiveControl(receivedControl));
        RUVIA_CHECK(receivedControl.id.connectionGeneration == 5);
        Mailbox::BorrowedBlock unpublished;
        RUVIA_CHECK(!mailbox.tryReceive(unpublished));
        RUVIA_CHECK(!mailbox.finishDrain());

        // SSL_read_ex returning WANT_READ/WANT_WRITE or zero bytes uses this abort path.
        reservation.abort();
        RUVIA_CHECK(!reservation);
        RUVIA_CHECK(mailbox.drainReturns() == 0);
        RUVIA_CHECK(mailbox.reserveData(reservedId, reservation) ==
                    Mailbox::ReservationResult::kReserved);
        auto writable = reservation.writableBytes();
        writable[0] = std::byte{0x11};
        writable[1] = std::byte{0x22};
        writable[2] = std::byte{0x33};
        writable[3] = std::byte{0x44};
        writable[4] = std::byte{0x55};
        RUVIA_CHECK(reservation.commit(5) == Mailbox::CommitResult::kSentNotifyPeer);
        RUVIA_CHECK(!reservation);

        Mailbox::BorrowedBlock block;
        RUVIA_CHECK(mailbox.tryReceive(block));
        RUVIA_CHECK(block.id().epoch == reservedId.epoch);
        RUVIA_CHECK(block.id().connectionGeneration == reservedId.connectionGeneration);
        RUVIA_CHECK(block.id().streamId == reservedId.streamId);
        RUVIA_CHECK_EQ(block.bytes().size(), std::size_t{5});
        RUVIA_CHECK(block.bytes()[0] == std::byte{0x11});
        RUVIA_CHECK(block.bytes()[4] == std::byte{0x55});
        block.release();
        RUVIA_CHECK(mailbox.drainReturns() == 1);
        RUVIA_CHECK(!mailbox.finishDrain());
        RUVIA_CHECK(mailbox.stop());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
}

RUVIA_TEST(http3StreamMailboxReservationRejectsZeroAndOversizedCommits) {
    Mailbox mailbox(1, 1, 1);
    Mailbox::DataReservation reservation;
    RUVIA_CHECK(mailbox.reserveData(Id{1, 2, 3}, reservation) ==
                Mailbox::ReservationResult::kReserved);
    RUVIA_CHECK(reservation.commit(0) == Mailbox::CommitResult::kZeroBytes);
    RUVIA_CHECK(!reservation);
    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(!mailbox.tryReceive(block));

    RUVIA_CHECK(mailbox.reserveData(Id{4, 5, 6}, reservation) ==
                Mailbox::ReservationResult::kReserved);
    RUVIA_CHECK(reservation.commit(Mailbox::kMaxBlockBytes + 1) ==
                Mailbox::CommitResult::kTooLarge);
    RUVIA_CHECK(!reservation);
    RUVIA_CHECK(!mailbox.tryReceive(block));

    RUVIA_CHECK(mailbox.reserveData(Id{7, 8, 9}, reservation) ==
                Mailbox::ReservationResult::kReserved);
    reservation.abort();
    RUVIA_CHECK(mailbox.stop());
}

RUVIA_TEST(http3StreamMailboxReservationDistinguishesSlotAndBackingBlockCredits) {
    Mailbox mailbox(1, 1, 1);
    RUVIA_CHECK(mailbox.trySend(Id{1, 2, 3}, payload(1)) ==
                Mailbox::SendResult::kSentNotifyPeer);
    Mailbox::DataReservation reservation;
    RUVIA_CHECK(mailbox.reserveData(Id{4, 5, 6}, reservation) ==
                Mailbox::ReservationResult::kFull);
    RUVIA_CHECK(!reservation);

    Mailbox::BorrowedBlock borrowed;
    RUVIA_CHECK(mailbox.tryReceive(borrowed));
    RUVIA_CHECK(mailbox.reserveData(Id{4, 5, 6}, reservation) ==
                Mailbox::ReservationResult::kNoBlock);
    borrowed.release();
    RUVIA_CHECK(mailbox.drainReturns() == 1);
    RUVIA_CHECK(mailbox.reserveData(Id{4, 5, 6}, reservation) ==
                Mailbox::ReservationResult::kReserved);
    reservation.abort();
    RUVIA_CHECK(mailbox.stop());
}

RUVIA_TEST(http3StreamMailboxReservationMoveAndDestructionAbortByRAII) {
    Mailbox mailbox(1, 1, 1);
    std::byte* reservedAddress{};
    {
        Mailbox::DataReservation original;
        RUVIA_CHECK(mailbox.reserveData(Id{1, 2, 3}, original) ==
                    Mailbox::ReservationResult::kReserved);
        reservedAddress = original.writableBytes().data();
        Mailbox::DataReservation moved(std::move(original));
        RUVIA_CHECK(!original);
        RUVIA_CHECK(moved);
    }

    Mailbox::DataReservation retry;
    RUVIA_CHECK(mailbox.reserveData(Id{4, 5, 6}, retry) ==
                Mailbox::ReservationResult::kReserved);
    RUVIA_CHECK(retry.writableBytes().data() == reservedAddress);
    retry.abort();
    RUVIA_CHECK(mailbox.stop());
}

RUVIA_TEST(http3StreamMailboxReservationAbortPreservesReturnedBlockOrder) {
    Mailbox mailbox(2, 2, 1);
    RUVIA_CHECK(mailbox.trySend(Id{1, 2, 3}, payload(1)) ==
                Mailbox::SendResult::kSentNotifyPeer);
    Mailbox::BorrowedBlock returned;
    RUVIA_CHECK(mailbox.tryReceive(returned));

    Mailbox::DataReservation reservation;
    RUVIA_CHECK(mailbox.reserveData(Id{4, 5, 6}, reservation) ==
                Mailbox::ReservationResult::kReserved);
    const auto reservedAddress = reservation.writableBytes().data();
    returned.release();
    RUVIA_CHECK(mailbox.drainReturns() == 0);
    reservation.abort();
    RUVIA_CHECK(mailbox.drainReturns() == 1);

    Mailbox::DataReservation next;
    RUVIA_CHECK(mailbox.reserveData(Id{7, 8, 9}, next) ==
                Mailbox::ReservationResult::kReserved);
    RUVIA_CHECK(next.writableBytes().data() != reservedAddress);
    const auto commit = next.commit(1);
    RUVIA_CHECK(commit == Mailbox::CommitResult::kSent ||
                commit == Mailbox::CommitResult::kSentNotifyPeer);
    RUVIA_CHECK(mailbox.reserveData(Id{10, 11, 12}, next) ==
                Mailbox::ReservationResult::kReserved);
    RUVIA_CHECK(next.writableBytes().data() == reservedAddress);
    next.abort();
    RUVIA_CHECK(mailbox.stop());
}

RUVIA_TEST(http3StreamMailboxReservationKeepsStopNonQuiescentAndWakesWaiter) {
    using Wait = Mailbox::CapacityWaitResult;
    unsigned notifications = 0;
    const auto notifier = CapacityNotifier{
        .context = &notifications,
        .notify = [](void* state) noexcept { ++*static_cast<unsigned*>(state); }};
    Mailbox mailbox(1, 1, 1, nullptr, notifier);
    Control event{.kind = Control::Kind::kWritable};
    RUVIA_CHECK(mailbox.trySendControl(event) == Mailbox::ControlResult::kSentNotifyPeer);
    Control received;
    RUVIA_CHECK(mailbox.tryReceiveControl(received));
    RUVIA_CHECK(!mailbox.finishDrain());
    RUVIA_CHECK(mailbox.trySendControl(event) == Mailbox::ControlResult::kSentNotifyPeer);
    RUVIA_CHECK(mailbox.armCapacityWait(Mailbox::CapacityInterest::kControl) == Wait::kArmed);

    Mailbox::DataReservation reservation;
    RUVIA_CHECK(mailbox.reserveData(Id{9, 10, 11}, reservation) ==
                Mailbox::ReservationResult::kReserved);
    std::binary_semaphore enterStop(0);
    bool stopWasQuiescent = true;
    std::thread stopper([&] {
        enterStop.acquire();
        stopWasQuiescent = mailbox.stop();
    });
    enterStop.release();
    stopper.join();

    RUVIA_CHECK(!stopWasQuiescent);
    RUVIA_CHECK(mailbox.stopped());
    RUVIA_CHECK(!mailbox.publishersQuiescent());
    RUVIA_CHECK_EQ(notifications, 1U);
    reservation.abort();
    RUVIA_CHECK(mailbox.publishersQuiescent());
    RUVIA_CHECK(mailbox.stop());
    RUVIA_CHECK_EQ(notifications, 1U);
    RUVIA_CHECK(mailbox.tryReceiveControl(received));
    RUVIA_CHECK(!mailbox.finishDrain());
}

RUVIA_TEST(http3StreamMailboxReservationCyclesReturnCreditsWithoutAllocation) {
    CountingResource memory;
    {
        Mailbox mailbox(1, 1, 1, &memory);
        const auto setupCalls = memory.allocationCalls;
        Mailbox::DataReservation reservation;
        Mailbox::BorrowedBlock block;
        for (std::uint64_t i = 0; i < 200; ++i) {
            const Id id{0xabc, 0xdef, i};
            RUVIA_CHECK(mailbox.reserveData(id, reservation) ==
                        Mailbox::ReservationResult::kReserved);
            auto writable = reservation.writableBytes();
            writable[0] = std::byte(i & 0xff);
            if ((i & 1U) == 0) {
                reservation.abort();
            } else {
                const auto result = reservation.commit(1);
                RUVIA_CHECK(result == Mailbox::CommitResult::kSent ||
                            result == Mailbox::CommitResult::kSentNotifyPeer);
                RUVIA_CHECK(mailbox.tryReceive(block));
                RUVIA_CHECK(block.id().epoch == id.epoch);
                RUVIA_CHECK(block.id().connectionGeneration == id.connectionGeneration);
                RUVIA_CHECK(block.id().streamId == id.streamId);
                RUVIA_CHECK(block.bytes()[0] == std::byte(i & 0xff));
                block.release();
                RUVIA_CHECK(mailbox.drainReturns() == 1);
                (void)mailbox.finishDrain();
            }
            RUVIA_CHECK(!reservation);
        }
        RUVIA_CHECK_EQ(memory.allocationCalls, setupCalls);
        RUVIA_CHECK(mailbox.stop());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
}

RUVIA_TEST(http3StreamMailboxCreditsBorrowLifetimeAndReuse) {
    Mailbox mailbox(1, 1, 1);
    const auto input = payload(1);
    std::array<std::byte, Mailbox::kMaxBlockBytes + 1> oversized{};
    RUVIA_CHECK(mailbox.trySend(Id{}, oversized) == Mailbox::SendResult::kTooLarge);
    RUVIA_CHECK(mailbox.trySend(Id{7, 8, 9}, input) == Mailbox::SendResult::kSentNotifyPeer);
    RUVIA_CHECK(mailbox.trySend(Id{7, 8, 10}, input) == Mailbox::SendResult::kFull);
    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(mailbox.tryReceive(block));
    RUVIA_CHECK(block.id().epoch == 7 && block.id().connectionGeneration == 8 && block.id().streamId == 9);
    RUVIA_CHECK(block.bytes().size() == input.size());
    RUVIA_CHECK(block.bytes()[1] == input[1]);
    RUVIA_CHECK(mailbox.trySend(Id{}, input) == Mailbox::SendResult::kNoBlock);
    block.release();
    RUVIA_CHECK(!block);
    RUVIA_CHECK(block.bytes().empty());
    RUVIA_CHECK(mailbox.drainReturns() == 1);
    RUVIA_CHECK(!mailbox.finishDrain());
    RUVIA_CHECK(mailbox.trySend(Id{8, 8, 9}, input) == Mailbox::SendResult::kSentNotifyPeer);

    Mailbox::BorrowedBlock latest;
    RUVIA_CHECK(mailbox.tryReceive(latest));
    RUVIA_CHECK(latest.id().epoch == 8);
    latest.release();
    RUVIA_CHECK(mailbox.drainReturns() == 1);
}

RUVIA_TEST(http3StreamMailboxQueueFullControlStopAndLateEpoch) {
    Mailbox mailbox(3, 1, 1);
    const auto bytes = payload(42);
    RUVIA_CHECK(mailbox.trySend(Id{2, 3, 4}, bytes) == Mailbox::SendResult::kSentNotifyPeer);
    RUVIA_CHECK(mailbox.trySend(Id{2, 3, 5}, bytes) == Mailbox::SendResult::kFull);
    Control event{Control::Kind::kStreamFin, Id{9, 10, 11}, 12};
    RUVIA_CHECK(mailbox.trySendControl(event) == Mailbox::ControlResult::kSent);
    const Control unconsumed{Control::Kind::kWritable, Id{1, 2, 3}, 99};
    RUVIA_CHECK(mailbox.trySendControl(unconsumed) == Mailbox::ControlResult::kFull);
    Control received;
    RUVIA_CHECK(mailbox.tryReceiveControl(received));
    RUVIA_CHECK(received.kind == Control::Kind::kStreamFin && received.id.epoch == 9 && received.value == 12);
    RUVIA_CHECK(mailbox.trySendControl(unconsumed) == Mailbox::ControlResult::kSent);
    RUVIA_CHECK(mailbox.stop());
    RUVIA_CHECK(mailbox.stopped());
    RUVIA_CHECK(mailbox.publishersQuiescent());
    RUVIA_CHECK(mailbox.trySend(Id{}, bytes) == Mailbox::SendResult::kStopped);
    RUVIA_CHECK(mailbox.trySendControl(event) == Mailbox::ControlResult::kStopped);
    Mailbox::BorrowedBlock stale;
    RUVIA_CHECK(mailbox.tryReceive(stale));
    RUVIA_CHECK(stale.id().epoch == 2);  // Caller recognizes stale epoch; ownership still returns normally.
    stale.release();
    RUVIA_CHECK(mailbox.drainReturns() == 1);
    RUVIA_CHECK(mailbox.tryReceiveControl(received));  // queued controls remain drainable after stop.
    RUVIA_CHECK(received.kind == Control::Kind::kWritable && received.value == 99);
    RUVIA_CHECK(!mailbox.finishDrain());
}

RUVIA_TEST(http3StreamMailboxKeepsResetCodeSeparateFromFinValue) {
    Mailbox mailbox(1, 1, 2);
    const Control reset{.kind = Control::Kind::kStreamReset,
        .id = {3, 4, 8},
        .value = 0,
        .streamResetErrorCode = ruvia::Http3ConnectionErrorCode::kMessageError};
    RUVIA_CHECK(mailbox.trySendControl(reset) == Mailbox::ControlResult::kSentNotifyPeer);
    Control received;
    RUVIA_CHECK(mailbox.tryReceiveControl(received));
    RUVIA_CHECK(received.kind == Control::Kind::kStreamReset);
    RUVIA_CHECK_EQ(received.value, std::uint64_t{0});
    RUVIA_CHECK(received.streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(!mailbox.finishDrain());

    const Control fin{.kind = Control::Kind::kStreamFin,
        .id = {3, 4, 8},
        .value = 37};
    RUVIA_CHECK(mailbox.trySendControl(fin) == Mailbox::ControlResult::kSentNotifyPeer);
    RUVIA_CHECK(mailbox.tryReceiveControl(received));
    RUVIA_CHECK(received.kind == Control::Kind::kStreamFin);
    RUVIA_CHECK_EQ(received.value, std::uint64_t{37});
    RUVIA_CHECK(received.streamResetErrorCode == ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    RUVIA_CHECK(!mailbox.finishDrain());
    RUVIA_CHECK(mailbox.stop());
}

RUVIA_TEST(http3StreamMailboxCoalescedWakeAndDrainRecheck) {
    Mailbox mailbox(8, 8, 2);
    const auto bytes = payload(17);
    RUVIA_CHECK(mailbox.trySend(Id{1, 1, 1}, bytes) == Mailbox::SendResult::kSentNotifyPeer);
    for (std::uint64_t id = 2; id <= 8; ++id) {
        RUVIA_CHECK(mailbox.trySend(Id{1, 1, id}, bytes) == Mailbox::SendResult::kSent);
    }
    const Control control{Control::Kind::kStreamFin, Id{1, 1, 9}, 0};
    RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kSent);
    RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kSent);
    RUVIA_CHECK(mailbox.trySendControl(control) == Mailbox::ControlResult::kFull);

    // The consumer has work at the drain boundary: clear then recheck must request an immediate drain.
    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(mailbox.tryReceive(block));
    block.release();
    RUVIA_CHECK(mailbox.finishDrain());
    while (mailbox.tryReceive(block)) {
        block.release();
    }
    Control received;
    while (mailbox.tryReceiveControl(received)) {
        RUVIA_CHECK(received.kind == Control::Kind::kStreamFin);
    }
    RUVIA_CHECK(!mailbox.finishDrain());
}

RUVIA_TEST(http3StreamMailboxStopFencesInflightPublisher) {
    Mailbox mailbox(4, 2, 1);
    std::array<std::byte, Mailbox::kMaxBlockBytes> bytes{};
    std::atomic<bool> go{};
    Mailbox::SendResult result{};
    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        result = mailbox.trySend(Id{1, 2, 3}, bytes);
    });
    go.store(true, std::memory_order_release);
    (void)mailbox.stop();
    producer.join();
    RUVIA_CHECK(result == Mailbox::SendResult::kStopped || result == Mailbox::SendResult::kSent ||
                result == Mailbox::SendResult::kSentNotifyPeer);
    RUVIA_CHECK(mailbox.publishersQuiescent());
}

RUVIA_TEST(http3StreamMailboxConcurrentMultipleBatchesAndAccounting) {
    constexpr std::uint32_t count = 3000;
    Mailbox mailbox(8, 5, 2);
    std::atomic<std::uint32_t> received{};
    std::thread producer([&] {
        for (std::uint32_t i = 0; i < count;) {
            (void)mailbox.drainReturns();
            const auto bytes = payload(i);
            const auto result = mailbox.trySend(Id{1, 2, i}, bytes);
            if (result == Mailbox::SendResult::kSent || result == Mailbox::SendResult::kSentNotifyPeer) {
                ++i;
            } else {
                RUVIA_CHECK(result == Mailbox::SendResult::kFull || result == Mailbox::SendResult::kNoBlock);
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&] {
        Mailbox::BorrowedBlock block;
        for (std::uint32_t i = 0; i < count;) {
            if (!mailbox.tryReceive(block)) {
                // Mirrors the event-loop boundary: clear the notification and recheck both lanes.
                (void)mailbox.finishDrain();
                std::this_thread::yield();
                continue;
            }
            RUVIA_CHECK(block.id().streamId == i);
            RUVIA_CHECK(block.bytes().size() == 4);
            RUVIA_CHECK(block.bytes()[0] == std::byte(i & 0xff));
            ++i;
            received.fetch_add(1, std::memory_order_relaxed);
            block.release();
        }
    });
    producer.join();
    consumer.join();
    RUVIA_CHECK(received.load(std::memory_order_relaxed) == count);
    (void)mailbox.drainReturns();
    Mailbox::BorrowedBlock recycled;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const auto sent = mailbox.trySend(Id{3, 4, i}, payload(i));
        RUVIA_CHECK(sent == Mailbox::SendResult::kSent || sent == Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(mailbox.tryReceive(recycled));
        recycled.release();
        (void)mailbox.drainReturns();
    }
}
