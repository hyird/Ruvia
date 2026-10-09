#include <cstdlib>
#include <exception>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "http3/quic_connection_state.h"
#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes_{};
    std::size_t live_blocks_{};
    std::optional<std::size_t> failing_allocation_size_{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (failing_allocation_size_ == size) {
            failing_allocation_size_.reset();
            throw std::bad_alloc();
        }
        void* const memory = std::pmr::new_delete_resource()->allocate(size, alignment);
        live_bytes_ += size;
        ++live_blocks_;
        return memory;
    }

    void do_deallocate(void* memory, std::size_t size, std::size_t alignment) override {
        live_bytes_ -= size;
        --live_blocks_;
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
    std::optional<ruvia::quic_crypto_record_lease> lease_;
    std::size_t retire_calls_{};
    bool drive_called_{};
};

ruvia::quic_tls_drive_result drive_with_held_lease(void* opaque,
    ruvia::quic_tls_handshake& handshake) noexcept {
    auto& driver = *static_cast<held_lease_driver*>(opaque);
    driver.drive_called_ = true;
    auto lease_value = handshake.take_crypto_record();
    if (lease_value) {
        driver.lease_.emplace(std::move(lease_value));
    }
    return {};
}

void retire_held_lease_driver(void* opaque) noexcept {
    auto& driver = *static_cast<held_lease_driver*>(opaque);
    ++driver.retire_calls_;
    driver.lease_.reset();
}

void leave_held_lease_driver(void* opaque) noexcept {
    ++static_cast<held_lease_driver*>(opaque)->retire_calls_;
}

#if defined(__unix__) || defined(__APPLE__)
void exit_on_terminate() noexcept {
    std::_Exit(77);
}
#endif

ruvia::quic_crypto_provider_view provider() {
    return {.random_bytes_ = random_bytes,
        .hkdf_extract_ = hkdf_extract,
        .hkdf_expand_ = hkdf_expand,
        .create_aead_key_ = create_aead,
        .create_header_protection_key_ = create_header,
        .secure_erase_ = secure_erase};
}

ruvia::quic_connection_config config() {
    ruvia::quic_connection_config result;
    result.local_address_.port_ = 443;
    result.peer_address_.port_ = 50000;
    const std::array dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array scid{std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
        std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
    result.destination_connection_id_ = ruvia::quic_connection_id(dcid);
    result.source_connection_id_ = ruvia::quic_connection_id(scid);
    result.limits_.max_crypto_buffer_size_ = 8;
    return result;
}

std::span<const std::byte> bytes(const char* value, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(value), size};
}

void test_repeated_records_and_levels(ruvia::testing::test_context& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        RUVIA_CHECK(!state_value.take_crypto(ruvia::quic_encryption_level::initial));
        state_value.append_crypto(ruvia::quic_encryption_level::initial, bytes("abcd", 4));
        state_value.append_crypto(ruvia::quic_encryption_level::handshake, bytes("xy", 2));
        auto lease_value = state_value.take_crypto(ruvia::quic_encryption_level::handshake);
        bool second_lease_rejected{};
        try {
            (void)state_value.take_crypto(ruvia::quic_encryption_level::initial);
        } catch (const std::logic_error&) {
            second_lease_rejected = true;
        }
        RUVIA_CHECK(second_lease_rejected);
        RUVIA_CHECK(lease_value.level() == ruvia::quic_encryption_level::handshake);
        RUVIA_CHECK_EQ(lease_value.bytes().size(), std::size_t{2});
        lease_value.consume(1);
        state_value.append_crypto(ruvia::quic_encryption_level::handshake, bytes("z", 1));
        RUVIA_CHECK_EQ(lease_value.bytes().size(), std::size_t{1});
        RUVIA_CHECK_EQ(lease_value.bytes()[0], std::byte{'y'});
        lease_value.release();
        auto tail = state_value.take_crypto(ruvia::quic_encryption_level::handshake);
        RUVIA_CHECK_EQ(tail.bytes().size(), std::size_t{1});
        RUVIA_CHECK_EQ(tail.bytes()[0], std::byte{'y'});
        tail.consume(1);
        auto later = state_value.take_crypto(ruvia::quic_encryption_level::handshake);
        RUVIA_CHECK_EQ(later.bytes().size(), std::size_t{1});
        RUVIA_CHECK_EQ(later.bytes()[0], std::byte{'z'});
        later.consume(1);
        auto initial_value = state_value.take_crypto(ruvia::quic_encryption_level::initial);
        RUVIA_CHECK_EQ(initial_value.bytes().size(), std::size_t{4});
        initial_value.consume(4);
        RUVIA_CHECK_EQ(state_value.retained_crypto_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(state_value.crypto_record_count(ruvia::quic_encryption_level::initial), std::size_t{0});
        RUVIA_CHECK_EQ(state_value.crypto_record_count(ruvia::quic_encryption_level::handshake), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

void test_budget_and_transactional_allocation_failure(ruvia::testing::test_context& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        state_value.append_crypto(ruvia::quic_encryption_level::application, bytes("12345678", 8));
        bool budget_rejected{};
        try {
            state_value.append_crypto(ruvia::quic_encryption_level::application, bytes("9", 1));
        } catch (const ruvia::quic_error& error) {
            budget_rejected = error.code() == ruvia::quic_error_code::resource_limit;
        }
        RUVIA_CHECK(budget_rejected);
        auto lease_value = state_value.take_crypto(ruvia::quic_encryption_level::application);
        lease_value.consume(8);

        const auto blocks_before = resource.live_blocks_;
        // Fail the one-byte record payload after node construction, without
        // depending on the STL's debug metadata allocation sequence.
        resource.failing_allocation_size_ = 1;
        bool allocation_failed{};
        try {
            state_value.append_crypto(ruvia::quic_encryption_level::application, bytes("x", 1));
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK_EQ(state_value.retained_crypto_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(state_value.crypto_record_count(ruvia::quic_encryption_level::application), std::size_t{0});
        RUVIA_CHECK_EQ(resource.live_blocks_, blocks_before);
    }
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

void test_move_and_cold_discard(ruvia::testing::test_context& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        state_value.append_crypto(ruvia::quic_encryption_level::initial, bytes("hello", 5));
        {
            auto first = state_value.take_crypto(ruvia::quic_encryption_level::initial);
            auto moved = std::move(first);
            RUVIA_CHECK(!first);
            RUVIA_CHECK_EQ(moved.bytes().size(), std::size_t{5});
        }
        auto reacquired = state_value.take_crypto(ruvia::quic_encryption_level::initial);
        RUVIA_CHECK_EQ(reacquired.bytes().size(), std::size_t{5});
        reacquired.consume(5);
    }
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
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
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.context_ = &driver, .drive_ = drive_with_held_lease, .retire_ = retire_held_lease_driver},
            &resource, {});
        state_value.append_crypto(ruvia::quic_encryption_level::initial, bytes("deferred", 8));
        state_value.tls_driver_active_ = true;
        const auto result_value = state_value.tls_driver_.drive_(state_value.tls_driver_.context_,
            state_value.tls_handshake());
        state_value.tls_driver_active_ = false;
        RUVIA_CHECK(result_value.progress_ == ruvia::quic_tls_progress::need_input);
        RUVIA_CHECK(driver.drive_called_);
        RUVIA_CHECK(driver.lease_.has_value());
        RUVIA_CHECK(state_value.crypto_lease_active_);
        const auto allocated_with_lease = resource.live_blocks_;
        RUVIA_CHECK(allocated_with_lease > 0);

        state_value.retire();
        RUVIA_CHECK_EQ(driver.retire_calls_, std::size_t{1});
        RUVIA_CHECK(!driver.lease_.has_value());
        RUVIA_CHECK(!state_value.crypto_lease_active_);
        RUVIA_CHECK(state_value.connection_ == nullptr);
        RUVIA_CHECK_EQ(state_value.info().state_, ruvia::quic_connection_state::retired);
        state_value.retire();
        RUVIA_CHECK_EQ(driver.retire_calls_, std::size_t{1});
        RUVIA_CHECK(resource.live_blocks_ > 0);
    }
    RUVIA_CHECK_EQ(driver.retire_calls_, std::size_t{1});
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});

    held_lease_driver cold_driver;
    {
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.context_ = &cold_driver, .drive_ = drive_with_held_lease, .retire_ = retire_held_lease_driver},
            &resource, {});
        RUVIA_CHECK(!cold_driver.drive_called_);
    }
    RUVIA_CHECK_EQ(cold_driver.retire_calls_, std::size_t{1});
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
#if defined(__unix__) || defined(__APPLE__)
    const auto child_value = fork();
    RUVIA_CHECK(child_value >= 0);
    if (child_value == 0) {
        std::set_terminate(exit_on_terminate);
        counting_resource child_resource;
        held_lease_driver violating_driver;
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.context_ = &violating_driver, .drive_ = drive_with_held_lease, .retire_ = leave_held_lease_driver},
            &child_resource, {});
        state_value.append_crypto(ruvia::quic_encryption_level::initial, bytes("deferred", 8));
        state_value.tls_driver_active_ = true;
        (void)state_value.tls_driver_.drive_(state_value.tls_driver_.context_, state_value.tls_handshake());
        state_value.tls_driver_active_ = false;
        if (!violating_driver.lease_.has_value()) {
            std::_Exit(1);
        }
        state_value.retire();
        std::_Exit(0);
    }
    if (child_value > 0) {
        int status{};
        RUVIA_CHECK_EQ(waitpid(child_value, &status, 0), child_value);
        RUVIA_CHECK(WIFEXITED(status));
        RUVIA_CHECK_EQ(WEXITSTATUS(status), 77);
    }
#endif
}
