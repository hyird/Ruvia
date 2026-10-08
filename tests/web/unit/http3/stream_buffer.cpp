#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <utility>

#include "http3/http3_stream_buffer.h"
#include "test_harness.h"

namespace {
using buffer = ruvia::detail::http3_stream_buffer;
using Id = ruvia::detail::http3_stream_id;
using Control = ruvia::detail::http3_stream_control;
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

RUVIA_TEST(http3_stream_buffer_construction_failure_returns_earlier_allocations) {
    CountingResource baseline;
    {
        buffer buffer(2, 1, 1, &baseline);
    }
    RUVIA_CHECK(baseline.allocationCalls > 0);
    RUVIA_CHECK(baseline.allocated == baseline.freed);
    for (std::size_t successfulCalls = 0; successfulCalls < baseline.allocationCalls;
        ++successfulCalls) {
        CountingResource memory;
        memory.allowedCalls = successfulCalls;
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { buffer buffer(2, 1, 1, &memory); }));
        RUVIA_CHECK(memory.allocated == memory.freed);
    }
}

RUVIA_TEST(http3_stream_buffer_reuses_bounded_blocks_and_returns_storage_at_retirement) {
    CountingResource memory;
    {
        buffer buffer(2, 1, 1, &memory);
        const auto setupBytes = memory.allocated;
        RUVIA_CHECK(setupBytes > 0);
        buffer::borrowed_block retained;
        buffer::borrowed_block current;
        RUVIA_CHECK(buffer.try_send(Id{1, 2, 3}, payload(1)) == buffer::send_result::sent);
        RUVIA_CHECK(buffer.try_receive(retained));
        RUVIA_CHECK(buffer.try_send(Id{1, 2, 4}, payload(2)) == buffer::send_result::sent);
        RUVIA_CHECK(buffer.try_receive(current));
        RUVIA_CHECK(retained.bytes()[0] == payload(1)[0]);
        current.release();
        retained.release();
        for (unsigned i = 0; i < 300; ++i) {
            const auto result = buffer.try_send(Id{1, 2, i}, payload(i));
            RUVIA_CHECK(result == buffer::send_result::sent);
            RUVIA_CHECK(buffer.try_receive(current));
            RUVIA_CHECK(current.bytes()[0] == payload(i)[0]);
            current.release();
        }
        RUVIA_CHECK(memory.allocated == setupBytes);
        RUVIA_CHECK(memory.freed == 0);
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK(memory.allocated == memory.freed);
}

RUVIA_TEST(http3_stream_buffer_writable_reservation_abort_and_partial_commit) {
    CountingResource memory;
    {
        buffer buffer(1, 1, 2, &memory);
        buffer::data_reservation reservation;
        const Id reservedId{0x1234, 0x5678, 0x9abc};
        RUVIA_CHECK(buffer.reserve_data(reservedId, reservation) ==
                    buffer::reservation_result::reserved);
        RUVIA_CHECK(reservation);
        RUVIA_CHECK_EQ(reservation.writable_bytes().size(), buffer::max_block_bytes);
        std::fill(reservation.writable_bytes().begin(), reservation.writable_bytes().end(),
            std::byte{0x7a});

        // An outstanding preread pins its DATA slot, but not the independent CONTROL lane.
        RUVIA_CHECK(buffer.try_send(Id{1, 2, 3}, payload(4)) ==
                    buffer::send_result::reservation_active);
        Control event{.kind = Control::kind::writable,
            .id = Id{4, 5, 6},
            .value = 7};
        RUVIA_CHECK(buffer.try_send_control(event) == buffer::control_result::sent);
        Control receivedControl;
        RUVIA_CHECK(buffer.try_receive_control(receivedControl));
        RUVIA_CHECK(receivedControl.id.connection_generation == 5);
        buffer::borrowed_block unpublished;
        RUVIA_CHECK(!buffer.try_receive(unpublished));
        RUVIA_CHECK(!buffer.has_pending());

        // SSL_read_ex returning WANT_READ/WANT_WRITE or zero bytes uses this abort path.
        reservation.abort();
        RUVIA_CHECK(!reservation);
        RUVIA_CHECK(buffer.reserve_data(reservedId, reservation) ==
                    buffer::reservation_result::reserved);
        auto writable = reservation.writable_bytes();
        writable[0] = std::byte{0x11};
        writable[1] = std::byte{0x22};
        writable[2] = std::byte{0x33};
        writable[3] = std::byte{0x44};
        writable[4] = std::byte{0x55};
        RUVIA_CHECK(reservation.commit(5) == buffer::commit_result::sent);
        RUVIA_CHECK(!reservation);

        buffer::borrowed_block block;
        RUVIA_CHECK(buffer.try_receive(block));
        RUVIA_CHECK(block.id().epoch == reservedId.epoch);
        RUVIA_CHECK(block.id().connection_generation == reservedId.connection_generation);
        RUVIA_CHECK(block.id().stream_id == reservedId.stream_id);
        RUVIA_CHECK_EQ(block.bytes().size(), std::size_t{5});
        RUVIA_CHECK(block.bytes()[0] == std::byte{0x11});
        RUVIA_CHECK(block.bytes()[4] == std::byte{0x55});
        block.release();
        RUVIA_CHECK(!buffer.has_pending());
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
}

RUVIA_TEST(http3_stream_buffer_reservation_rejects_zero_and_oversized_commits) {
    buffer buffer(1, 1, 1);
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(Id{1, 2, 3}, reservation) ==
                buffer::reservation_result::reserved);
    RUVIA_CHECK(reservation.commit(0) == buffer::commit_result::zero_bytes);
    RUVIA_CHECK(!reservation);
    buffer::borrowed_block block;
    RUVIA_CHECK(!buffer.try_receive(block));

    RUVIA_CHECK(buffer.reserve_data(Id{4, 5, 6}, reservation) ==
                buffer::reservation_result::reserved);
    RUVIA_CHECK(reservation.commit(buffer::max_block_bytes + 1) ==
                buffer::commit_result::too_large);
    RUVIA_CHECK(!reservation);
    RUVIA_CHECK(!buffer.try_receive(block));

    RUVIA_CHECK(buffer.reserve_data(Id{7, 8, 9}, reservation) ==
                buffer::reservation_result::reserved);
    reservation.abort();
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_reservation_distinguishes_slot_and_backing_block_credits) {
    buffer buffer(1, 1, 1);
    RUVIA_CHECK(buffer.try_send(Id{1, 2, 3}, payload(1)) ==
                buffer::send_result::sent);
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(Id{4, 5, 6}, reservation) ==
                buffer::reservation_result::full);
    RUVIA_CHECK(!reservation);

    buffer::borrowed_block borrowed;
    RUVIA_CHECK(buffer.try_receive(borrowed));
    RUVIA_CHECK(buffer.reserve_data(Id{4, 5, 6}, reservation) ==
                buffer::reservation_result::no_block);
    borrowed.release();
    RUVIA_CHECK(buffer.reserve_data(Id{4, 5, 6}, reservation) ==
                buffer::reservation_result::reserved);
    reservation.abort();
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_reservation_move_and_destruction_abort_by_r_a_i_i) {
    buffer buffer(1, 1, 1);
    std::byte* reservedAddress{};
    {
        buffer::data_reservation original;
        RUVIA_CHECK(buffer.reserve_data(Id{1, 2, 3}, original) ==
                    buffer::reservation_result::reserved);
        reservedAddress = original.writable_bytes().data();
        buffer::data_reservation moved(std::move(original));
        RUVIA_CHECK(!original);
        RUVIA_CHECK(moved);
    }

    buffer::data_reservation retry;
    RUVIA_CHECK(buffer.reserve_data(Id{4, 5, 6}, retry) ==
                buffer::reservation_result::reserved);
    RUVIA_CHECK(retry.writable_bytes().data() == reservedAddress);
    retry.abort();
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_reservation_cycles_return_credits_without_allocation) {
    CountingResource memory;
    {
        buffer buffer(1, 1, 1, &memory);
        const auto setupCalls = memory.allocationCalls;
        buffer::data_reservation reservation;
        buffer::borrowed_block block;
        for (std::uint64_t i = 0; i < 200; ++i) {
            const Id id{0xabc, 0xdef, i};
            RUVIA_CHECK(buffer.reserve_data(id, reservation) ==
                        buffer::reservation_result::reserved);
            auto writable = reservation.writable_bytes();
            writable[0] = std::byte(i & 0xff);
            if ((i & 1U) == 0) {
                reservation.abort();
            } else {
                const auto result = reservation.commit(1);
                RUVIA_CHECK(result == buffer::commit_result::sent);
                RUVIA_CHECK(buffer.try_receive(block));
                RUVIA_CHECK(block.id().epoch == id.epoch);
                RUVIA_CHECK(block.id().connection_generation == id.connection_generation);
                RUVIA_CHECK(block.id().stream_id == id.stream_id);
                RUVIA_CHECK(block.bytes()[0] == std::byte(i & 0xff));
                block.release();
                RUVIA_CHECK(!buffer.has_pending());
            }
            RUVIA_CHECK(!reservation);
        }
        RUVIA_CHECK_EQ(memory.allocationCalls, setupCalls);
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
}

RUVIA_TEST(http3_stream_buffer_credits_borrow_lifetime_and_reuse) {
    buffer buffer(1, 1, 1);
    const auto input = payload(1);
    std::array<std::byte, buffer::max_block_bytes + 1> oversized{};
    RUVIA_CHECK(buffer.try_send(Id{}, oversized) == buffer::send_result::too_large);
    RUVIA_CHECK(buffer.try_send(Id{7, 8, 9}, input) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_send(Id{7, 8, 10}, input) == buffer::send_result::full);
    buffer::borrowed_block block;
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(block.id().epoch == 7 && block.id().connection_generation == 8 && block.id().stream_id == 9);
    RUVIA_CHECK(block.bytes().size() == input.size());
    RUVIA_CHECK(block.bytes()[1] == input[1]);
    RUVIA_CHECK(buffer.try_send(Id{}, input) == buffer::send_result::no_block);
    block.release();
    RUVIA_CHECK(!block);
    RUVIA_CHECK(block.bytes().empty());
    RUVIA_CHECK(!buffer.has_pending());
    RUVIA_CHECK(buffer.try_send(Id{8, 8, 9}, input) == buffer::send_result::sent);

    buffer::borrowed_block latest;
    RUVIA_CHECK(buffer.try_receive(latest));
    RUVIA_CHECK(latest.id().epoch == 8);
    latest.release();
}

RUVIA_TEST(http3_stream_buffer_queue_full_control_stop_and_late_epoch) {
    buffer buffer(3, 1, 1);
    const auto bytes = payload(42);
    RUVIA_CHECK(buffer.try_send(Id{2, 3, 4}, bytes) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_send(Id{2, 3, 5}, bytes) == buffer::send_result::full);
    Control event{Control::kind::stream_fin, Id{9, 10, 11}, 12};
    RUVIA_CHECK(buffer.try_send_control(event) == buffer::control_result::sent);
    const Control unconsumed{Control::kind::writable, Id{1, 2, 3}, 99};
    RUVIA_CHECK(buffer.try_send_control(unconsumed) == buffer::control_result::full);
    Control received;
    RUVIA_CHECK(buffer.try_receive_control(received));
    RUVIA_CHECK(received.kind == Control::kind::stream_fin && received.id.epoch == 9 && received.value == 12);
    RUVIA_CHECK(buffer.try_send_control(unconsumed) == buffer::control_result::sent);
    RUVIA_CHECK(buffer.stop());
    RUVIA_CHECK(buffer.stopped());
    RUVIA_CHECK(buffer.quiescent());
    RUVIA_CHECK(buffer.try_send(Id{}, bytes) == buffer::send_result::stopped);
    RUVIA_CHECK(buffer.try_send_control(event) == buffer::control_result::stopped);
    buffer::borrowed_block stale;
    RUVIA_CHECK(buffer.try_receive(stale));
    RUVIA_CHECK(stale.id().epoch == 2);  // Caller recognizes stale epoch; ownership still returns normally.
    stale.release();
    RUVIA_CHECK(buffer.try_receive_control(received));  // queued controls remain drainable after stop.
    RUVIA_CHECK(received.kind == Control::kind::writable && received.value == 99);
    RUVIA_CHECK(!buffer.has_pending());
}

RUVIA_TEST(http3_stream_buffer_keeps_reset_code_separate_from_fin_value) {
    buffer buffer(1, 1, 2);
    const Control reset{.kind = Control::kind::stream_reset,
        .id = {3, 4, 8},
        .value = 0,
        .stream_reset_error_code = ruvia::Http3ConnectionErrorCode::kMessageError};
    RUVIA_CHECK(buffer.try_send_control(reset) == buffer::control_result::sent);
    Control received;
    RUVIA_CHECK(buffer.try_receive_control(received));
    RUVIA_CHECK(received.kind == Control::kind::stream_reset);
    RUVIA_CHECK_EQ(received.value, std::uint64_t{0});
    RUVIA_CHECK(received.stream_reset_error_code == ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(!buffer.has_pending());

    const Control fin{.kind = Control::kind::stream_fin,
        .id = {3, 4, 8},
        .value = 37};
    RUVIA_CHECK(buffer.try_send_control(fin) == buffer::control_result::sent);
    RUVIA_CHECK(buffer.try_receive_control(received));
    RUVIA_CHECK(received.kind == Control::kind::stream_fin);
    RUVIA_CHECK_EQ(received.value, std::uint64_t{37});
    RUVIA_CHECK(received.stream_reset_error_code == ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    RUVIA_CHECK(!buffer.has_pending());
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_pending_snapshot_observes_both_lanes_without_consuming_work) {
    buffer buffer(8, 8, 2);
    const auto bytes = payload(17);
    RUVIA_CHECK(buffer.try_send(Id{1, 1, 1}, bytes) == buffer::send_result::sent);
    for (std::uint64_t id = 2; id <= 8; ++id) {
        RUVIA_CHECK(buffer.try_send(Id{1, 1, id}, bytes) == buffer::send_result::sent);
    }
    const Control control{Control::kind::stream_fin, Id{1, 1, 9}, 0};
    RUVIA_CHECK(buffer.try_send_control(control) == buffer::control_result::sent);
    RUVIA_CHECK(buffer.try_send_control(control) == buffer::control_result::sent);
    RUVIA_CHECK(buffer.try_send_control(control) == buffer::control_result::full);

    // Observing queued work does not consume DATA or CONTROL publication.
    buffer::borrowed_block block;
    RUVIA_CHECK(buffer.try_receive(block));
    block.release();
    RUVIA_CHECK(buffer.has_pending());
    RUVIA_CHECK(buffer.has_pending());
    while (buffer.try_receive(block)) {
        block.release();
    }
    RUVIA_CHECK(buffer.has_pending());
    Control received;
    while (buffer.try_receive_control(received)) {
        RUVIA_CHECK(received.kind == Control::kind::stream_fin);
    }
    RUVIA_CHECK(!buffer.has_pending());
}

namespace {
struct local_events final {
    std::uint64_t ready_data{};
    std::uint64_t ready_control{};
    std::uint64_t capacity_data{};
    std::uint64_t capacity_control{};

    static void ready(void* context, std::uint8_t lanes) noexcept {
        auto& events = *static_cast<local_events*>(context);
        events.ready_data += (lanes & buffer::data_lane) != 0;
        events.ready_control += (lanes & buffer::control_lane) != 0;
    }
    static void capacity(void* context, std::uint8_t lanes) noexcept {
        auto& events = *static_cast<local_events*>(context);
        events.capacity_data += (lanes & buffer::data_lane) != 0;
        events.capacity_control += (lanes & buffer::control_lane) != 0;
    }
    buffer::local_notifications notifications() noexcept {
        return {{this, ready}, {this, capacity}};
    }
};
}  // namespace

RUVIA_TEST(http3_stream_buffer_signals_each_local_lane_and_immediately_reclaims_blocks) {
    CountingResource memory;
    local_events events;
    {
        buffer buffer(1, 1, 1, &memory, events.notifications());
        const auto allocations = memory.allocationCalls;
        buffer::data_reservation reservation;
        std::byte* reused{};
        for (unsigned attempt = 0; attempt != 8; ++attempt) {
            RUVIA_CHECK(buffer.reserve_data(Id{1, 2, 3}, reservation) == buffer::reservation_result::reserved);
            auto* bytes = reservation.writable_bytes().data();
            RUVIA_CHECK(attempt == 0 || bytes == reused);
            reused = bytes;
            reservation.abort();
            RUVIA_CHECK_EQ(events.capacity_data, 0U);
            RUVIA_CHECK_EQ(events.ready_data, 0U);
        }
        RUVIA_CHECK(buffer.try_send(Id{1, 2, 3}, payload(1)) == buffer::send_result::sent);
        RUVIA_CHECK_EQ(events.ready_data, 1U);
        RUVIA_CHECK(buffer.try_send(Id{}, payload(2)) == buffer::send_result::full);
        RUVIA_CHECK_EQ(events.ready_data, 1U);
        Control control{.kind = Control::kind::writable};
        RUVIA_CHECK(buffer.try_send_control(control) == buffer::control_result::sent);
        RUVIA_CHECK_EQ(events.ready_control, 1U);
        buffer::borrowed_block block;
        RUVIA_CHECK(buffer.try_receive(block));
        RUVIA_CHECK_EQ(events.capacity_data, 1U);
        RUVIA_CHECK(buffer.try_send(Id{}, payload(2)) == buffer::send_result::no_block);
        RUVIA_CHECK(buffer.try_receive_control(control));
        RUVIA_CHECK_EQ(events.capacity_control, 1U);
        block.release();
        RUVIA_CHECK_EQ(events.capacity_data, 2U);
        block.release();
        RUVIA_CHECK_EQ(events.capacity_data, 2U);
        RUVIA_CHECK(buffer.try_send(Id{}, payload(2)) == buffer::send_result::sent);
        RUVIA_CHECK(buffer.stop());
        RUVIA_CHECK(buffer.try_receive(block));
        RUVIA_CHECK(!buffer.quiescent());
        RUVIA_CHECK(!buffer.stop());
        block.release();
        RUVIA_CHECK(buffer.quiescent());
        RUVIA_CHECK_EQ(events.capacity_data, 2U);
        RUVIA_CHECK_EQ(memory.allocationCalls, allocations);
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
}

RUVIA_TEST(http3_stream_buffer_reclaims_out_of_order_borrows_during_writable_reservation) {
    buffer buffer(3, 1, 1);
    buffer::borrowed_block first;
    buffer::borrowed_block second;
    RUVIA_CHECK(buffer.try_send(Id{1, 2, 3}, payload(1)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(first));
    RUVIA_CHECK(buffer.try_send(Id{1, 2, 4}, payload(2)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(second));
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(Id{1, 2, 5}, reservation) == buffer::reservation_result::reserved);
    reservation.writable_bytes()[0] = std::byte{3};
    second.release();
    RUVIA_CHECK_EQ(first.bytes()[0], std::byte{1});
    RUVIA_CHECK(reservation.commit(1) == buffer::commit_result::sent);
    buffer::borrowed_block third;
    RUVIA_CHECK(buffer.try_receive(third));
    // The released later borrow is usable without an owner return-drain phase,
    // even though the earlier borrow and committed reservation remain alive.
    RUVIA_CHECK(buffer.reserve_data(Id{1, 2, 6}, reservation) == buffer::reservation_result::reserved);
    reservation.writable_bytes()[0] = std::byte{4};
    RUVIA_CHECK(reservation.commit(1) == buffer::commit_result::sent);
    buffer::borrowed_block fourth;
    RUVIA_CHECK(buffer.try_receive(fourth));
    RUVIA_CHECK_EQ(first.bytes()[0], std::byte{1});
    RUVIA_CHECK_EQ(third.bytes()[0], std::byte{3});
    RUVIA_CHECK_EQ(fourth.bytes()[0], std::byte{4});
    RUVIA_CHECK(!buffer.stop());
    fourth.release();
    first.release();
    third.release();
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_critical_destination_cannot_be_mistaken_for_request_data) {
    buffer buffer(1, 1, 1);
    const ruvia::detail::http3_critical_stream_id critical{7, 8, ruvia::http3_critical_stream_output::stream_kind::qpack_decoder};
    RUVIA_CHECK(buffer.try_send_critical(critical, payload(9)) == buffer::send_result::sent);
    buffer::borrowed_block block;
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(block.critical() != nullptr);
    RUVIA_CHECK_EQ(block.critical()->epoch, 7U);
    RUVIA_CHECK_EQ(block.critical()->connection_generation, 8U);
    RUVIA_CHECK(block.critical()->kind == critical.kind);
    RUVIA_CHECK_EQ(block.bytes()[0], std::byte{9});
    block.release();
    RUVIA_CHECK(buffer.try_send(Id{1, 2, 3, 4}, payload(5)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(block.critical() == nullptr);
    RUVIA_CHECK_EQ(block.id().push_id, std::optional<std::uint64_t>{4});
}

RUVIA_TEST(http3_stream_buffer_stop_preserves_admitted_reservation_and_external_borrow_contract) {
    local_events events;
    buffer buffer(1, 1, 1);
    buffer.set_local_notifications(events.notifications());
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(Id{1, 2, 3}, reservation) == buffer::reservation_result::reserved);
    reservation.writable_bytes()[0] = std::byte{7};
    RUVIA_CHECK(!buffer.stop());
    RUVIA_CHECK(buffer.stopped());
    RUVIA_CHECK(!buffer.quiescent());
    RUVIA_CHECK(buffer.try_send_control({}) == buffer::control_result::stopped);
    RUVIA_CHECK(reservation.commit(1) == buffer::commit_result::sent);
    RUVIA_CHECK(buffer.quiescent());
    RUVIA_CHECK_EQ(events.ready_data, 1U);
    buffer::borrowed_block block;
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK_EQ(block.bytes()[0], std::byte{7});
    RUVIA_CHECK(!buffer.quiescent());
    RUVIA_CHECK(buffer.try_send(Id{}, payload(1)) == buffer::send_result::stopped);
    buffer::data_reservation rejected;
    RUVIA_CHECK(buffer.reserve_data(Id{}, rejected) == buffer::reservation_result::stopped);
    block.release();
    RUVIA_CHECK(buffer.stop());
    RUVIA_CHECK_EQ(events.capacity_data, 0U);
}

RUVIA_TEST(http3_stream_buffer_move_assignment_releases_previous_linear_borrows) {
    buffer buffer(2, 1, 1);
    buffer::borrowed_block first;
    buffer::borrowed_block second;
    RUVIA_CHECK(buffer.try_send(Id{1, 2, 3}, payload(1)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(first));
    RUVIA_CHECK(buffer.try_send(Id{1, 2, 4}, payload(2)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(second));
    first = std::move(second);
    RUVIA_CHECK(!second);
    RUVIA_CHECK_EQ(first.bytes()[0], std::byte{2});
    RUVIA_CHECK(buffer.try_send(Id{1, 2, 5}, payload(3)) == buffer::send_result::sent);
    first.release();
    RUVIA_CHECK(buffer.try_receive(first));
    RUVIA_CHECK_EQ(first.bytes()[0], std::byte{3});
}
