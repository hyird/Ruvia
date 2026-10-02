#include <cstdlib>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ruvia/http/detail/http3/quic_connection_state.h"

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes{};
    std::size_t live_blocks{};
    std::size_t allocations_before_failure{std::numeric_limits<std::size_t>::max()};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (allocations_before_failure == 0) {
            allocations_before_failure = std::numeric_limits<std::size_t>::max();
            throw std::bad_alloc();
        }
        if (allocations_before_failure != std::numeric_limits<std::size_t>::max()) {
            --allocations_before_failure;
        }
        void* const memory = std::pmr::new_delete_resource()->allocate(size, alignment);
        live_bytes += size;
        ++live_blocks;
        return memory;
    }

    void do_deallocate(void* memory, std::size_t size, std::size_t alignment) override {
        live_bytes -= size;
        --live_blocks;
        std::pmr::new_delete_resource()->deallocate(memory, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

void random_bytes(void*, std::span<std::byte>) {}
void hkdf_extract(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
void hkdf_expand(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
ruvia::quic_aead_key create_aead(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    return {};
}
ruvia::quic_header_protection_key create_header(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte>) {
    return {};
}
void secure_erase(void*, std::span<std::byte>) noexcept {}
ruvia::quic_tls_drive_result drive_tls(void*, ruvia::quic_tls_handshake&) noexcept {
    return {};
}
void retire_tls(void*) noexcept {}

struct held_lease_driver {
    std::optional<ruvia::quic_crypto_record_lease> lease;
    std::size_t retire_calls{};
    bool drive_called{};
};

ruvia::quic_tls_drive_result drive_with_held_lease(void* opaque,
    ruvia::quic_tls_handshake& handshake) noexcept {
    auto& driver = *static_cast<held_lease_driver*>(opaque);
    driver.drive_called = true;
    auto lease = handshake.take_crypto_record();
    if (lease) {
        driver.lease.emplace(std::move(lease));
    }
    return {};
}

void retire_held_lease_driver(void* opaque) noexcept {
    auto& driver = *static_cast<held_lease_driver*>(opaque);
    ++driver.retire_calls;
    driver.lease.reset();
}

void leave_held_lease_driver(void* opaque) noexcept {
    ++static_cast<held_lease_driver*>(opaque)->retire_calls;
}

#if defined(__unix__) || defined(__APPLE__)
void exit_on_terminate() noexcept {
    std::_Exit(77);
}
#endif

ruvia::quic_crypto_provider_view provider() {
    return {.random_bytes = random_bytes,
        .hkdf_extract = hkdf_extract,
        .hkdf_expand = hkdf_expand,
        .create_aead_key = create_aead,
        .create_header_protection_key = create_header,
        .secure_erase = secure_erase};
}

ruvia::quic_connection_config config() {
    ruvia::quic_connection_config result;
    result.local_address.port = 443;
    result.peer_address.port = 50000;
    const std::array dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array scid{std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
        std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
    result.destination_connection_id = ruvia::quic_connection_id(dcid);
    result.source_connection_id = ruvia::quic_connection_id(scid);
    result.limits.max_crypto_buffer_size = 8;
    return result;
}

std::span<const std::byte> bytes(const char* value, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(value), size};
}

void test_repeated_records_and_levels(ruvia::testing::TestContext& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        RUVIA_CHECK(!state.take_crypto(ruvia::quic_encryption_level::initial));
        state.append_crypto(ruvia::quic_encryption_level::initial, bytes("abcd", 4));
        state.append_crypto(ruvia::quic_encryption_level::handshake, bytes("xy", 2));
        auto lease = state.take_crypto(ruvia::quic_encryption_level::handshake);
        bool second_lease_rejected{};
        try {
            (void)state.take_crypto(ruvia::quic_encryption_level::initial);
        } catch (const std::logic_error&) {
            second_lease_rejected = true;
        }
        RUVIA_CHECK(second_lease_rejected);
        RUVIA_CHECK(lease.level() == ruvia::quic_encryption_level::handshake);
        RUVIA_CHECK_EQ(lease.bytes().size(), std::size_t{2});
        lease.consume(1);
        state.append_crypto(ruvia::quic_encryption_level::handshake, bytes("z", 1));
        RUVIA_CHECK_EQ(lease.bytes().size(), std::size_t{1});
        RUVIA_CHECK_EQ(lease.bytes()[0], std::byte{'y'});
        lease.release();
        auto tail = state.take_crypto(ruvia::quic_encryption_level::handshake);
        RUVIA_CHECK_EQ(tail.bytes().size(), std::size_t{1});
        RUVIA_CHECK_EQ(tail.bytes()[0], std::byte{'y'});
        tail.consume(1);
        auto later = state.take_crypto(ruvia::quic_encryption_level::handshake);
        RUVIA_CHECK_EQ(later.bytes().size(), std::size_t{1});
        RUVIA_CHECK_EQ(later.bytes()[0], std::byte{'z'});
        later.consume(1);
        auto initial = state.take_crypto(ruvia::quic_encryption_level::initial);
        RUVIA_CHECK_EQ(initial.bytes().size(), std::size_t{4});
        initial.consume(4);
        RUVIA_CHECK_EQ(state.retained_crypto_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(state.crypto_record_count(ruvia::quic_encryption_level::initial), std::size_t{0});
        RUVIA_CHECK_EQ(state.crypto_record_count(ruvia::quic_encryption_level::handshake), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}

void test_budget_and_transactional_allocation_failure(ruvia::testing::TestContext& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        state.append_crypto(ruvia::quic_encryption_level::application, bytes("12345678", 8));
        bool budget_rejected{};
        try {
            state.append_crypto(ruvia::quic_encryption_level::application, bytes("9", 1));
        } catch (const ruvia::quic_error& error) {
            budget_rejected = error.code() == ruvia::quic_error_code::resource_limit;
        }
        RUVIA_CHECK(budget_rejected);
        auto lease = state.take_crypto(ruvia::quic_encryption_level::application);
        lease.consume(8);

        const auto blocks_before = resource.live_blocks;
        resource.allocations_before_failure = 1;
        bool allocation_failed{};
        try {
            state.append_crypto(ruvia::quic_encryption_level::application, bytes("x", 1));
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK_EQ(state.retained_crypto_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(state.crypto_record_count(ruvia::quic_encryption_level::application), std::size_t{0});
        RUVIA_CHECK_EQ(resource.live_blocks, blocks_before);
    }
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}

void test_move_and_cold_discard(ruvia::testing::TestContext& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        state.append_crypto(ruvia::quic_encryption_level::initial, bytes("hello", 5));
        {
            auto first = state.take_crypto(ruvia::quic_encryption_level::initial);
            auto moved = std::move(first);
            RUVIA_CHECK(!first);
            RUVIA_CHECK_EQ(moved.bytes().size(), std::size_t{5});
        }
        auto reacquired = state.take_crypto(ruvia::quic_encryption_level::initial);
        RUVIA_CHECK_EQ(reacquired.bytes().size(), std::size_t{5});
        reacquired.consume(5);
    }
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}

}  // namespace

RUVIA_TEST(quic_crypto_record_lease_preserves_partial_records_across_levels) {
    test_repeated_records_and_levels(ruvia_ctx);
}

RUVIA_TEST(quic_crypto_record_lease_rolls_back_budget_and_allocation_failures) {
    test_budget_and_transactional_allocation_failure(ruvia_ctx);
}

RUVIA_TEST(quic_crypto_record_lease_moves_and_releases_on_cold_discard) {
    test_move_and_cold_discard(ruvia_ctx);
}

RUVIA_TEST(quic_tls_driver_retirement_releases_held_lease_once_and_cold_retires) {
    counting_resource resource;
    held_lease_driver driver;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.context = &driver, .drive = drive_with_held_lease, .retire = retire_held_lease_driver},
            &resource, {});
        state.append_crypto(ruvia::quic_encryption_level::initial, bytes("deferred", 8));
        state.tls_driver_active_ = true;
        const auto result = state.tls_driver_.drive(state.tls_driver_.context,
            state.tls_handshake());
        state.tls_driver_active_ = false;
        RUVIA_CHECK(result.progress == ruvia::quic_tls_progress::need_input);
        RUVIA_CHECK(driver.drive_called);
        RUVIA_CHECK(driver.lease.has_value());
        RUVIA_CHECK(state.crypto_lease_active_);
        const auto allocated_with_lease = resource.live_blocks;
        RUVIA_CHECK(allocated_with_lease > 0);

        state.retire();
        RUVIA_CHECK_EQ(driver.retire_calls, std::size_t{1});
        RUVIA_CHECK(!driver.lease.has_value());
        RUVIA_CHECK(!state.crypto_lease_active_);
        RUVIA_CHECK(state.connection_ == nullptr);
        RUVIA_CHECK_EQ(state.info().state, ruvia::quic_connection_state::retired);
        state.retire();
        RUVIA_CHECK_EQ(driver.retire_calls, std::size_t{1});
        RUVIA_CHECK(resource.live_blocks > 0);
    }
    RUVIA_CHECK_EQ(driver.retire_calls, std::size_t{1});
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});

    held_lease_driver cold_driver;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.context = &cold_driver, .drive = drive_with_held_lease, .retire = retire_held_lease_driver},
            &resource, {});
        RUVIA_CHECK(!cold_driver.drive_called);
    }
    RUVIA_CHECK_EQ(cold_driver.retire_calls, std::size_t{1});
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
#if defined(__unix__) || defined(__APPLE__)
    const auto child = fork();
    RUVIA_CHECK(child >= 0);
    if (child == 0) {
        std::set_terminate(exit_on_terminate);
        counting_resource child_resource;
        held_lease_driver violating_driver;
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.context = &violating_driver, .drive = drive_with_held_lease, .retire = leave_held_lease_driver},
            &child_resource, {});
        state.append_crypto(ruvia::quic_encryption_level::initial, bytes("no-release", 10));
        state.tls_driver_active_ = true;
        (void)state.tls_driver_.drive(state.tls_driver_.context, state.tls_handshake());
        state.tls_driver_active_ = false;
        state.retire();
        std::_Exit(0);
    }
    if (child > 0) {
        int status{};
        RUVIA_CHECK_EQ(waitpid(child, &status, 0), child);
        RUVIA_CHECK(WIFEXITED(status));
        RUVIA_CHECK_EQ(WEXITSTATUS(status), 77);
    }
#endif
}
