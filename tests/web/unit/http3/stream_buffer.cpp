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
using id_type = ruvia::detail::http3_stream_id;
using control_type = ruvia::detail::http3_stream_control;
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocated_{};
    std::size_t freed_{};
    std::size_t allocation_calls_{};
    std::size_t allowed_calls_{std::numeric_limits<std::size_t>::max()};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value >= 32 && allocation_calls_++ >= allowed_calls_) {
            throw std::bad_alloc();
        }
        void* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        allocated_ += bytes_value;
        return result_value;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        freed_ += bytes_value;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
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
    counting_resource baseline;
    {
        buffer buffer(2, 1, 1, &baseline);
    }
    RUVIA_CHECK(baseline.allocation_calls_ > 0);
    RUVIA_CHECK(baseline.allocated_ == baseline.freed_);
    for (std::size_t successful_calls = 0; successful_calls < baseline.allocation_calls_;
        ++successful_calls) {
        counting_resource memory;
        memory.allowed_calls_ = successful_calls;
        RUVIA_CHECK(ruvia::testing::throws_on([&] { buffer buffer(2, 1, 1, &memory); }));
        RUVIA_CHECK(memory.allocated_ == memory.freed_);
    }
}

RUVIA_TEST(http3_stream_buffer_reuses_bounded_blocks_and_returns_storage_at_retirement) {
    counting_resource memory;
    {
        buffer buffer(2, 1, 1, &memory);
        const auto setup_bytes = memory.allocated_;
        RUVIA_CHECK(setup_bytes > 0);
        buffer::borrowed_block retained;
        buffer::borrowed_block current;
        RUVIA_CHECK(buffer.try_send(id_type{1, 2, 3}, payload(1)) == buffer::send_result::sent);
        RUVIA_CHECK(buffer.try_receive(retained));
        RUVIA_CHECK(buffer.try_send(id_type{1, 2, 4}, payload(2)) == buffer::send_result::sent);
        RUVIA_CHECK(buffer.try_receive(current));
        RUVIA_CHECK(retained.bytes()[0] == payload(1)[0]);
        current.release();
        retained.release();
        for (unsigned i = 0; i < 300; ++i) {
            const auto result_value = buffer.try_send(id_type{1, 2, i}, payload(i));
            RUVIA_CHECK(result_value == buffer::send_result::sent);
            RUVIA_CHECK(buffer.try_receive(current));
            RUVIA_CHECK(current.bytes()[0] == payload(i)[0]);
            current.release();
        }
        RUVIA_CHECK(memory.allocated_ == setup_bytes);
        RUVIA_CHECK(memory.freed_ == 0);
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK(memory.allocated_ == memory.freed_);
}

RUVIA_TEST(http3_stream_buffer_writable_reservation_abort_and_partial_commit) {
    counting_resource memory;
    {
        buffer buffer(1, 1, 2, &memory);
        buffer::data_reservation reservation;
        const id_type reserved_id{0x1234, 0x5678, 0x9abc};
        RUVIA_CHECK(buffer.reserve_data(reserved_id, reservation) ==
                    buffer::reservation_result::reserved);
        RUVIA_CHECK(reservation);
        RUVIA_CHECK_EQ(reservation.writable_bytes().size(), buffer::max_block_bytes);
        std::fill(reservation.writable_bytes().begin(), reservation.writable_bytes().end(),
            std::byte{0x7a});

        // An outstanding preread pins its DATA slot, but not the independent CONTROL lane.
        RUVIA_CHECK(buffer.try_send(id_type{1, 2, 3}, payload(4)) ==
                    buffer::send_result::reservation_active);
        control_type event{.kind_ = control_type::kind::writable,
            .id_ = id_type{4, 5, 6},
            .value_ = 7};
        RUVIA_CHECK(buffer.try_send_control(event) == buffer::control_result::sent);
        control_type received_control;
        RUVIA_CHECK(buffer.try_receive_control(received_control));
        RUVIA_CHECK(received_control.id_.connection_generation_ == 5);
        buffer::borrowed_block unpublished;
        RUVIA_CHECK(!buffer.try_receive(unpublished));
        RUVIA_CHECK(!buffer.has_pending());

        // SSL_read_ex returning WANT_READ/WANT_WRITE or zero bytes uses this abort path.
        reservation.abort();
        RUVIA_CHECK(!reservation);
        RUVIA_CHECK(buffer.reserve_data(reserved_id, reservation) ==
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
        RUVIA_CHECK(block.id().epoch_ == reserved_id.epoch_);
        RUVIA_CHECK(block.id().connection_generation_ == reserved_id.connection_generation_);
        RUVIA_CHECK(block.id().stream_id_ == reserved_id.stream_id_);
        RUVIA_CHECK_EQ(block.bytes().size(), std::size_t{5});
        RUVIA_CHECK(block.bytes()[0] == std::byte{0x11});
        RUVIA_CHECK(block.bytes()[4] == std::byte{0x55});
        block.release();
        RUVIA_CHECK(!buffer.has_pending());
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK_EQ(memory.allocated_, memory.freed_);
}

RUVIA_TEST(http3_stream_buffer_reservation_rejects_zero_and_oversized_commits) {
    buffer buffer(1, 1, 1);
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(id_type{1, 2, 3}, reservation) ==
                buffer::reservation_result::reserved);
    RUVIA_CHECK(reservation.commit(0) == buffer::commit_result::zero_bytes);
    RUVIA_CHECK(!reservation);
    buffer::borrowed_block block;
    RUVIA_CHECK(!buffer.try_receive(block));

    RUVIA_CHECK(buffer.reserve_data(id_type{4, 5, 6}, reservation) ==
                buffer::reservation_result::reserved);
    RUVIA_CHECK(reservation.commit(buffer::max_block_bytes + 1) ==
                buffer::commit_result::too_large);
    RUVIA_CHECK(!reservation);
    RUVIA_CHECK(!buffer.try_receive(block));

    RUVIA_CHECK(buffer.reserve_data(id_type{7, 8, 9}, reservation) ==
                buffer::reservation_result::reserved);
    reservation.abort();
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_reservation_distinguishes_slot_and_backing_block_credits) {
    buffer buffer(1, 1, 1);
    RUVIA_CHECK(buffer.try_send(id_type{1, 2, 3}, payload(1)) ==
                buffer::send_result::sent);
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(id_type{4, 5, 6}, reservation) ==
                buffer::reservation_result::full);
    RUVIA_CHECK(!reservation);

    buffer::borrowed_block borrowed;
    RUVIA_CHECK(buffer.try_receive(borrowed));
    RUVIA_CHECK(buffer.reserve_data(id_type{4, 5, 6}, reservation) ==
                buffer::reservation_result::no_block);
    borrowed.release();
    RUVIA_CHECK(buffer.reserve_data(id_type{4, 5, 6}, reservation) ==
                buffer::reservation_result::reserved);
    reservation.abort();
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_reservation_move_and_destruction_abort_by_r_a_i_i) {
    buffer buffer(1, 1, 1);
    std::byte* reserved_address{};
    {
        buffer::data_reservation original;
        RUVIA_CHECK(buffer.reserve_data(id_type{1, 2, 3}, original) ==
                    buffer::reservation_result::reserved);
        reserved_address = original.writable_bytes().data();
        buffer::data_reservation moved(std::move(original));
        RUVIA_CHECK(!original);
        RUVIA_CHECK(moved);
    }

    buffer::data_reservation retry;
    RUVIA_CHECK(buffer.reserve_data(id_type{4, 5, 6}, retry) ==
                buffer::reservation_result::reserved);
    RUVIA_CHECK(retry.writable_bytes().data() == reserved_address);
    retry.abort();
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_reservation_cycles_return_credits_without_allocation) {
    counting_resource memory;
    {
        buffer buffer(1, 1, 1, &memory);
        const auto setup_calls = memory.allocation_calls_;
        buffer::data_reservation reservation;
        buffer::borrowed_block block;
        for (std::uint64_t i = 0; i < 200; ++i) {
            const id_type id{0xabc, 0xdef, i};
            RUVIA_CHECK(buffer.reserve_data(id, reservation) ==
                        buffer::reservation_result::reserved);
            auto writable = reservation.writable_bytes();
            writable[0] = std::byte(i & 0xff);
            if ((i & 1U) == 0) {
                reservation.abort();
            } else {
                const auto result_value = reservation.commit(1);
                RUVIA_CHECK(result_value == buffer::commit_result::sent);
                RUVIA_CHECK(buffer.try_receive(block));
                RUVIA_CHECK(block.id().epoch_ == id.epoch_);
                RUVIA_CHECK(block.id().connection_generation_ == id.connection_generation_);
                RUVIA_CHECK(block.id().stream_id_ == id.stream_id_);
                RUVIA_CHECK(block.bytes()[0] == std::byte(i & 0xff));
                block.release();
                RUVIA_CHECK(!buffer.has_pending());
            }
            RUVIA_CHECK(!reservation);
        }
        RUVIA_CHECK_EQ(memory.allocation_calls_, setup_calls);
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK_EQ(memory.allocated_, memory.freed_);
}

RUVIA_TEST(http3_stream_buffer_credits_borrow_lifetime_and_reuse) {
    buffer buffer(1, 1, 1);
    const auto input = payload(1);
    std::array<std::byte, buffer::max_block_bytes + 1> oversized{};
    RUVIA_CHECK(buffer.try_send(id_type{}, oversized) == buffer::send_result::too_large);
    RUVIA_CHECK(buffer.try_send(id_type{7, 8, 9}, input) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_send(id_type{7, 8, 10}, input) == buffer::send_result::full);
    buffer::borrowed_block block;
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(block.id().epoch_ == 7 && block.id().connection_generation_ == 8 && block.id().stream_id_ == 9);
    RUVIA_CHECK(block.bytes().size() == input.size());
    RUVIA_CHECK(block.bytes()[1] == input[1]);
    RUVIA_CHECK(buffer.try_send(id_type{}, input) == buffer::send_result::no_block);
    block.release();
    RUVIA_CHECK(!block);
    RUVIA_CHECK(block.bytes().empty());
    RUVIA_CHECK(!buffer.has_pending());
    RUVIA_CHECK(buffer.try_send(id_type{8, 8, 9}, input) == buffer::send_result::sent);

    buffer::borrowed_block latest;
    RUVIA_CHECK(buffer.try_receive(latest));
    RUVIA_CHECK(latest.id().epoch_ == 8);
    latest.release();
}

RUVIA_TEST(http3_stream_buffer_queue_full_control_stop_and_late_epoch) {
    buffer buffer(3, 1, 1);
    const auto bytes_value = payload(42);
    RUVIA_CHECK(buffer.try_send(id_type{2, 3, 4}, bytes_value) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_send(id_type{2, 3, 5}, bytes_value) == buffer::send_result::full);
    control_type event{control_type::kind::stream_fin, id_type{9, 10, 11}, 12};
    RUVIA_CHECK(buffer.try_send_control(event) == buffer::control_result::sent);
    const control_type unconsumed{control_type::kind::writable, id_type{1, 2, 3}, 99};
    RUVIA_CHECK(buffer.try_send_control(unconsumed) == buffer::control_result::full);
    control_type received;
    RUVIA_CHECK(buffer.try_receive_control(received));
    RUVIA_CHECK(received.kind_ == control_type::kind::stream_fin && received.id_.epoch_ == 9 && received.value_ == 12);
    RUVIA_CHECK(buffer.try_send_control(unconsumed) == buffer::control_result::sent);
    RUVIA_CHECK(buffer.stop());
    RUVIA_CHECK(buffer.stopped());
    RUVIA_CHECK(buffer.quiescent());
    RUVIA_CHECK(buffer.try_send(id_type{}, bytes_value) == buffer::send_result::stopped);
    RUVIA_CHECK(buffer.try_send_control(event) == buffer::control_result::stopped);
    buffer::borrowed_block stale;
    RUVIA_CHECK(buffer.try_receive(stale));
    RUVIA_CHECK(stale.id().epoch_ == 2);  // Caller recognizes stale epoch; ownership still returns normally.
    stale.release();
    RUVIA_CHECK(buffer.try_receive_control(received));  // queued controls remain drainable after stop.
    RUVIA_CHECK(received.kind_ == control_type::kind::writable && received.value_ == 99);
    RUVIA_CHECK(!buffer.has_pending());
}

RUVIA_TEST(http3_stream_buffer_keeps_reset_code_separate_from_fin_value) {
    buffer buffer(1, 1, 2);
    const control_type reset{.kind_ = control_type::kind::stream_reset,
        .id_ = {3, 4, 8},
        .value_ = 0,
        .stream_reset_error_code_ = ruvia::http3_connection_error_code::message_error};
    RUVIA_CHECK(buffer.try_send_control(reset) == buffer::control_result::sent);
    control_type received;
    RUVIA_CHECK(buffer.try_receive_control(received));
    RUVIA_CHECK(received.kind_ == control_type::kind::stream_reset);
    RUVIA_CHECK_EQ(received.value_, std::uint64_t{0});
    RUVIA_CHECK(received.stream_reset_error_code_ == ruvia::http3_connection_error_code::message_error);
    RUVIA_CHECK(!buffer.has_pending());

    const control_type fin{.kind_ = control_type::kind::stream_fin,
        .id_ = {3, 4, 8},
        .value_ = 37};
    RUVIA_CHECK(buffer.try_send_control(fin) == buffer::control_result::sent);
    RUVIA_CHECK(buffer.try_receive_control(received));
    RUVIA_CHECK(received.kind_ == control_type::kind::stream_fin);
    RUVIA_CHECK_EQ(received.value_, std::uint64_t{37});
    RUVIA_CHECK(received.stream_reset_error_code_ == ruvia::http3_connection_error_code::request_cancelled);
    RUVIA_CHECK(!buffer.has_pending());
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_stream_buffer_pending_snapshot_observes_both_lanes_without_consuming_work) {
    buffer buffer(8, 8, 2);
    const auto bytes_value = payload(17);
    RUVIA_CHECK(buffer.try_send(id_type{1, 1, 1}, bytes_value) == buffer::send_result::sent);
    for (std::uint64_t id = 2; id <= 8; ++id) {
        RUVIA_CHECK(buffer.try_send(id_type{1, 1, id}, bytes_value) == buffer::send_result::sent);
    }
    const control_type control{control_type::kind::stream_fin, id_type{1, 1, 9}, 0};
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
    control_type received;
    while (buffer.try_receive_control(received)) {
        RUVIA_CHECK(received.kind_ == control_type::kind::stream_fin);
    }
    RUVIA_CHECK(!buffer.has_pending());
}

namespace {
struct local_events final {
    std::uint64_t ready_data_{};
    std::uint64_t ready_control_{};
    std::uint64_t capacity_data_{};
    std::uint64_t capacity_control_{};

    static void ready(void* context_value, std::uint8_t lanes) noexcept {
        auto& events_value = *static_cast<local_events*>(context_value);
        events_value.ready_data_ += (lanes & buffer::data_lane) != 0;
        events_value.ready_control_ += (lanes & buffer::control_lane) != 0;
    }
    static void capacity(void* context_value, std::uint8_t lanes) noexcept {
        auto& events_value = *static_cast<local_events*>(context_value);
        events_value.capacity_data_ += (lanes & buffer::data_lane) != 0;
        events_value.capacity_control_ += (lanes & buffer::control_lane) != 0;
    }
    buffer::local_notifications notifications() noexcept {
        return {{this, ready}, {this, capacity}};
    }
};
}  // namespace

RUVIA_TEST(http3_stream_buffer_signals_each_local_lane_and_immediately_reclaims_blocks) {
    counting_resource memory;
    local_events events;
    {
        buffer buffer(1, 1, 1, &memory, events.notifications());
        const auto allocations = memory.allocation_calls_;
        buffer::data_reservation reservation;
        std::byte* reused{};
        for (unsigned attempt_value = 0; attempt_value != 8; ++attempt_value) {
            RUVIA_CHECK(buffer.reserve_data(id_type{1, 2, 3}, reservation) == buffer::reservation_result::reserved);
            auto* bytes_value = reservation.writable_bytes().data();
            RUVIA_CHECK(attempt_value == 0 || bytes_value == reused);
            reused = bytes_value;
            reservation.abort();
            RUVIA_CHECK_EQ(events.capacity_data_, 0U);
            RUVIA_CHECK_EQ(events.ready_data_, 0U);
        }
        RUVIA_CHECK(buffer.try_send(id_type{1, 2, 3}, payload(1)) == buffer::send_result::sent);
        RUVIA_CHECK_EQ(events.ready_data_, 1U);
        RUVIA_CHECK(buffer.try_send(id_type{}, payload(2)) == buffer::send_result::full);
        RUVIA_CHECK_EQ(events.ready_data_, 1U);
        control_type control{.kind_ = control_type::kind::writable};
        RUVIA_CHECK(buffer.try_send_control(control) == buffer::control_result::sent);
        RUVIA_CHECK_EQ(events.ready_control_, 1U);
        buffer::borrowed_block block;
        RUVIA_CHECK(buffer.try_receive(block));
        RUVIA_CHECK_EQ(events.capacity_data_, 1U);
        RUVIA_CHECK(buffer.try_send(id_type{}, payload(2)) == buffer::send_result::no_block);
        RUVIA_CHECK(buffer.try_receive_control(control));
        RUVIA_CHECK_EQ(events.capacity_control_, 1U);
        block.release();
        RUVIA_CHECK_EQ(events.capacity_data_, 2U);
        block.release();
        RUVIA_CHECK_EQ(events.capacity_data_, 2U);
        RUVIA_CHECK(buffer.try_send(id_type{}, payload(2)) == buffer::send_result::sent);
        RUVIA_CHECK(buffer.stop());
        RUVIA_CHECK(buffer.try_receive(block));
        RUVIA_CHECK(!buffer.quiescent());
        RUVIA_CHECK(!buffer.stop());
        block.release();
        RUVIA_CHECK(buffer.quiescent());
        RUVIA_CHECK_EQ(events.capacity_data_, 2U);
        RUVIA_CHECK_EQ(memory.allocation_calls_, allocations);
    }
    RUVIA_CHECK_EQ(memory.allocated_, memory.freed_);
}

RUVIA_TEST(http3_stream_buffer_reclaims_out_of_order_borrows_during_writable_reservation) {
    buffer buffer(3, 1, 1);
    buffer::borrowed_block first;
    buffer::borrowed_block second;
    RUVIA_CHECK(buffer.try_send(id_type{1, 2, 3}, payload(1)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(first));
    RUVIA_CHECK(buffer.try_send(id_type{1, 2, 4}, payload(2)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(second));
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(id_type{1, 2, 5}, reservation) == buffer::reservation_result::reserved);
    reservation.writable_bytes()[0] = std::byte{3};
    second.release();
    RUVIA_CHECK_EQ(first.bytes()[0], std::byte{1});
    RUVIA_CHECK(reservation.commit(1) == buffer::commit_result::sent);
    buffer::borrowed_block third;
    RUVIA_CHECK(buffer.try_receive(third));
    // The released later borrow is usable without an owner return-drain phase,
    // even though the earlier borrow and committed reservation remain alive.
    RUVIA_CHECK(buffer.reserve_data(id_type{1, 2, 6}, reservation) == buffer::reservation_result::reserved);
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
    RUVIA_CHECK_EQ(block.critical()->epoch_, 7U);
    RUVIA_CHECK_EQ(block.critical()->connection_generation_, 8U);
    RUVIA_CHECK(block.critical()->kind_ == critical.kind_);
    RUVIA_CHECK_EQ(block.bytes()[0], std::byte{9});
    block.release();
    RUVIA_CHECK(buffer.try_send(id_type{1, 2, 3, 4}, payload(5)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(block.critical() == nullptr);
    RUVIA_CHECK_EQ(block.id().push_id_, std::optional<std::uint64_t>{4});
}

RUVIA_TEST(http3_stream_buffer_stop_preserves_admitted_reservation_and_external_borrow_contract) {
    local_events events;
    buffer buffer(1, 1, 1);
    buffer.set_local_notifications(events.notifications());
    buffer::data_reservation reservation;
    RUVIA_CHECK(buffer.reserve_data(id_type{1, 2, 3}, reservation) == buffer::reservation_result::reserved);
    reservation.writable_bytes()[0] = std::byte{7};
    RUVIA_CHECK(!buffer.stop());
    RUVIA_CHECK(buffer.stopped());
    RUVIA_CHECK(!buffer.quiescent());
    RUVIA_CHECK(buffer.try_send_control({}) == buffer::control_result::stopped);
    RUVIA_CHECK(reservation.commit(1) == buffer::commit_result::sent);
    RUVIA_CHECK(buffer.quiescent());
    RUVIA_CHECK_EQ(events.ready_data_, 1U);
    buffer::borrowed_block block;
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK_EQ(block.bytes()[0], std::byte{7});
    RUVIA_CHECK(!buffer.quiescent());
    RUVIA_CHECK(buffer.try_send(id_type{}, payload(1)) == buffer::send_result::stopped);
    buffer::data_reservation rejected;
    RUVIA_CHECK(buffer.reserve_data(id_type{}, rejected) == buffer::reservation_result::stopped);
    block.release();
    RUVIA_CHECK(buffer.stop());
    RUVIA_CHECK_EQ(events.capacity_data_, 0U);
}

RUVIA_TEST(http3_stream_buffer_move_assignment_releases_previous_linear_borrows) {
    buffer buffer(2, 1, 1);
    buffer::borrowed_block first;
    buffer::borrowed_block second;
    RUVIA_CHECK(buffer.try_send(id_type{1, 2, 3}, payload(1)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(first));
    RUVIA_CHECK(buffer.try_send(id_type{1, 2, 4}, payload(2)) == buffer::send_result::sent);
    RUVIA_CHECK(buffer.try_receive(second));
    first = std::move(second);
    RUVIA_CHECK(!second);
    RUVIA_CHECK_EQ(first.bytes()[0], std::byte{2});
    RUVIA_CHECK(buffer.try_send(id_type{1, 2, 5}, payload(3)) == buffer::send_result::sent);
    first.release();
    RUVIA_CHECK(buffer.try_receive(first));
    RUVIA_CHECK_EQ(first.bytes()[0], std::byte{3});
}
