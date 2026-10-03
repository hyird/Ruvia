#include "ruvia/http/detail/http3/quic_crypto_bridge.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/http3/quic_connection_state.h"

#include "test_harness.h"

namespace {

class faulting_resource final : public std::pmr::memory_resource {
public:
    explicit faulting_resource(std::pmr::memory_resource* upstream)
        : upstream_(upstream) {}

    void fail_after_slot_allocations(std::size_t successful_allocations) noexcept {
        allocations_until_failure_ = successful_allocations + 1;
    }

    std::size_t allocations() const noexcept {
        return allocations_;
    }
    std::size_t deallocations() const noexcept {
        return deallocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Key moves may allocate debug iterator proxies in noexcept STL code.
        // Count only the native slots whose partial construction is under test.
        const bool key_slot =
            (bytes == sizeof(ruvia::detail::quic_aead_key_slot) && alignment == alignof(ruvia::detail::quic_aead_key_slot)) ||
            (bytes == sizeof(ruvia::detail::quic_header_key_slot) && alignment == alignof(ruvia::detail::quic_header_key_slot));
        if (key_slot && allocations_until_failure_ != 0 && --allocations_until_failure_ == 0) {
            throw std::bad_alloc();
        }
        ++allocations_;
        return upstream_->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations_;
        upstream_->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t allocations_until_failure_{};
    std::size_t allocations_{};
    std::size_t deallocations_{};
};

struct callback_context {
    bool reject_open{};
    bool throw_open{};
    bool empty_aead{};
    bool empty_header{};
    std::size_t throw_aead_on_call{};
    std::size_t aead_factory_calls{};
    std::size_t erase_calls{};
    std::size_t erased_bytes{};
    std::size_t fail_slot_allocation_after{};
    std::size_t header_factory_calls{};
    faulting_resource* slot_resource{};
    std::size_t destroyed{};
};

struct primitive_state {
    callback_context* owner{};
};

void destroy_primitive(void* opaque) noexcept {
    std::unique_ptr<primitive_state> state(static_cast<primitive_state*>(opaque));
    ++state->owner->destroyed;
}

void seal_primitive(void*, std::span<const std::byte, 12>, std::span<const std::byte>,
    std::span<const std::byte> plaintext, std::span<std::byte> output) {
    if (output.size() < plaintext.size()) {
        throw std::invalid_argument("short seal output");
    }
    std::ranges::copy(plaintext, output.begin());
    std::ranges::fill(output.subspan(plaintext.size()), std::byte{0x6d});
}

ruvia::quic_aead_key_operations::open_result open_primitive(
    void* opaque, std::span<const std::byte, 12>, std::span<const std::byte>,
    std::span<const std::byte> ciphertext, std::span<std::byte> plaintext) {
    const auto& state = *static_cast<primitive_state*>(opaque);
    if (state.owner->throw_open) {
        throw std::runtime_error("injected AEAD backend failure");
    }
    if (state.owner->reject_open) {
        return {.value = ruvia::quic_aead_key_operations::open_result::status::rejected};
    }
    if (ciphertext.size() < 16 || plaintext.size() != ciphertext.size() - 16) {
        throw std::invalid_argument("malformed test ciphertext");
    }
    std::ranges::copy(ciphertext.first(plaintext.size()), plaintext.begin());
    return {.value = ruvia::quic_aead_key_operations::open_result::status::authenticated,
        .plaintext_size = plaintext.size()};
}

ruvia::quic_aead_key make_key(callback_context& context) {
    auto state = std::make_unique<primitive_state>();
    state->owner = &context;
    return ruvia::quic_aead_key::adopt(state.release(),
        {.destroy = destroy_primitive, .seal = seal_primitive, .open = open_primitive});
}

void random_failure(void*, std::span<std::byte>) {
    throw std::runtime_error("injected entropy backend failure");
}

void noop_hkdf_extract(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
void noop_hkdf_expand(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
ruvia::quic_aead_key unused_aead(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    throw std::logic_error("unexpected key creation");
}
ruvia::quic_header_protection_key unused_header(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte>) {
    throw std::logic_error("unexpected header-key creation");
}
void erase_nothing(void*, std::span<std::byte>) noexcept {}
ruvia::quic_tls_drive_result tls_driver(void*, ruvia::quic_tls_handshake&) noexcept {
    return {};
}
void retire_tls(void*) noexcept {}

void fill_random(void*, std::span<std::byte> output) {
    std::ranges::fill(output, std::byte{0x31});
}

void fill_hkdf(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte> output) {
    std::ranges::fill(output, std::byte{0x42});
}

void fill_expand(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte> output) {
    std::ranges::fill(output, std::byte{0x53});
}

struct header_primitive_state {
    callback_context* owner{};
};

void destroy_header_primitive(void* opaque) noexcept {
    std::unique_ptr<header_primitive_state> state(static_cast<header_primitive_state*>(opaque));
    ++state->owner->destroyed;
}

void mask_primitive(void*, std::span<const std::byte, 16>, std::span<std::byte, 5> output) {
    std::ranges::fill(output, std::byte{0x64});
}

ruvia::quic_header_protection_key make_header_key(callback_context& context) {
    auto state = std::make_unique<header_primitive_state>();
    state->owner = &context;
    return ruvia::quic_header_protection_key::adopt(state.release(),
        {.destroy = destroy_header_primitive, .mask = mask_primitive});
}

ruvia::quic_aead_key client_aead(void* opaque, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    auto& context = *static_cast<callback_context*>(opaque);
    ++context.aead_factory_calls;
    if (context.throw_aead_on_call == context.aead_factory_calls) {
        throw std::runtime_error("injected client AEAD factory failure");
    }
    return context.empty_aead ? ruvia::quic_aead_key{} : make_key(context);
}

ruvia::quic_header_protection_key client_header(void* opaque,
    ruvia::quic_cipher_suite, std::span<const std::byte>) {
    auto& context = *static_cast<callback_context*>(opaque);
    ++context.header_factory_calls;
    if (context.fail_slot_allocation_after != 0 && context.slot_resource &&
        context.header_factory_calls == 2) {
        context.slot_resource->fail_after_slot_allocations(context.fail_slot_allocation_after);
        context.fail_slot_allocation_after = 0;
    }
    return context.empty_header ? ruvia::quic_header_protection_key{} : make_header_key(context);
}

void erase_client_secret(void* opaque, std::span<std::byte> bytes) noexcept {
    auto& context = *static_cast<callback_context*>(opaque);
    ++context.erase_calls;
    context.erased_bytes += bytes.size();
    std::ranges::fill(bytes, std::byte{});
}

ruvia::quic_crypto_provider_view client_crypto(callback_context& context) {
    return {.context = &context,
        .random_bytes = fill_random,
        .hkdf_extract = fill_hkdf,
        .hkdf_expand = fill_expand,
        .create_aead_key = client_aead,
        .create_header_protection_key = client_header,
        .secure_erase = erase_client_secret};
}

struct initial_tls_context {
    std::array<std::byte, 12> client_hello{};
    bool called{};
    ruvia::quic_encryption_level observed_read_level{ruvia::quic_encryption_level::initial};
    ruvia::quic_operation_status submit_status{ruvia::quic_operation_status::would_block};
    ruvia::quic_tls_alert failure_alert{ruvia::quic_tls_alert::handshake_failure};
    std::size_t retire_calls{};
    bool fail_drive{};
};

ruvia::quic_tls_drive_result drive_initial_client_hello(void* opaque,
    ruvia::quic_tls_handshake& handshake) noexcept {
    auto& context = *static_cast<initial_tls_context*>(opaque);
    context.called = true;
    context.observed_read_level = handshake.current_read_level();
    context.client_hello.fill(std::byte{0x79});
    context.submit_status = handshake.submit_crypto(
        ruvia::quic_encryption_level::initial, context.client_hello);
    context.client_hello.fill(std::byte{0x00});
    if (context.fail_drive) {
        handshake.fail(context.failure_alert);
        return {.progress = ruvia::quic_tls_progress::failed, .alert = context.failure_alert};
    }
    return {.progress = ruvia::quic_tls_progress::progress};
}

void retire_initial_tls(void* opaque) noexcept {
    ++static_cast<initial_tls_context*>(opaque)->retire_calls;
}

int test_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid,
    ngtcp2_stateless_reset_token* token, size_t cidlen, void*) {
    std::array<uint8_t, NGTCP2_MAX_CIDLEN> bytes{};
    bytes.fill(0x25);
    ngtcp2_cid_init(cid, bytes.data(), cidlen);
    std::ranges::fill(token->data, 0x36);
    return 0;
}

int test_get_path_challenge(ngtcp2_conn*, ngtcp2_path_challenge_data* challenge, void*) {
    std::ranges::fill(challenge->data, 0x47);
    return 0;
}

struct initial_state_owner {
    initial_state_owner(initial_tls_context& tls_context,
        std::pmr::memory_resource* resource)
        : state(make_client_config(), client_crypto(crypto_context),
              {.context = &tls_context, .drive = drive_initial_client_hello, .retire = retire_initial_tls}, resource, {}) {}

    static ruvia::quic_connection_config make_client_config() {
        ruvia::quic_connection_config config{};
        config.local_address.port = 1;
        config.peer_address.port = 2;
        const std::array<std::byte, 8> dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
            std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
        const std::array<std::byte, 8> scid{std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
            std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
        config.destination_connection_id = ruvia::quic_connection_id(dcid);
        config.source_connection_id = ruvia::quic_connection_id(scid);
        return config;
    }

    callback_context crypto_context;
    ruvia::detail::quic_connection_state state;
};

struct native_client_fixture {
    int initialize(ruvia::testing::TestContext& ruvia_ctx,
        ruvia::detail::quic_connection_state& state) {
        ngtcp2_callbacks callbacks{};
        ruvia::detail::fill_quic_crypto_callbacks(callbacks);
        callbacks.get_new_connection_id2 = test_get_new_connection_id;
        callbacks.get_path_challenge_data2 = test_get_path_challenge;

        ngtcp2_settings settings{};
        ngtcp2_settings_default(&settings);
        settings.initial_ts = 100;
        ruvia::detail::initialize_quic_random_context(settings.rand_ctx, state);

        ngtcp2_transport_params params{};
        ngtcp2_transport_params_default(&params);
        const auto dcid = state.config_.destination_connection_id.view();
        const auto scid = state.config_.source_connection_id->view();
        ngtcp2_cid native_dcid{};
        ngtcp2_cid native_scid{};
        ngtcp2_cid_init(&native_dcid, reinterpret_cast<const uint8_t*>(dcid.data()), dcid.size());
        ngtcp2_cid_init(&native_scid, reinterpret_cast<const uint8_t*>(scid.data()), scid.size());

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(4433);
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(4434);
        remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ngtcp2_path_storage_init(&path, reinterpret_cast<ngtcp2_sockaddr*>(&local), sizeof(local),
            reinterpret_cast<ngtcp2_sockaddr*>(&remote), sizeof(remote), nullptr);

        const int result = ngtcp2_conn_client_new(&state.connection_, &native_dcid, &native_scid,
            &path.path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params, nullptr, &state);
        ruvia::testing::reportCheck(ruvia_ctx, result != 0, __FILE__, __LINE__,
            "ngtcp2_conn_client_new returns 0");
        if (result == 0) {
            ruvia::detail::encode_quic_local_transport_parameters(state);
        }
        return result;
    }

    ngtcp2_path_storage path{};
};

ruvia::quic_connection_config make_config() {
    ruvia::quic_connection_config config{};
    config.local_address.port = 1;
    config.peer_address.port = 2;
    const std::array<std::byte, 8> dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array<std::byte, 8> scid{std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
        std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
    config.destination_connection_id = ruvia::quic_connection_id(dcid);
    config.source_connection_id = ruvia::quic_connection_id(scid);
    return config;
}

struct state_owner {
    state_owner(callback_context& crypto_context, std::pmr::memory_resource* resource)
        : state(make_config(),
              {.context = &crypto_context,
                  .random_bytes = random_failure,
                  .hkdf_extract = noop_hkdf_extract,
                  .hkdf_expand = noop_hkdf_expand,
                  .create_aead_key = unused_aead,
                  .create_header_protection_key = unused_header,
                  .secure_erase = erase_nothing},
              {.drive = tls_driver, .retire = retire_tls}, resource, {}) {}

    ruvia::detail::quic_connection_state state;
};

RUVIA_TEST(quic_ngtcp2_crypto_callback_table_contains_core_owned_crypto_callbacks) {
    ngtcp2_callbacks callbacks{};
    ruvia::detail::fill_quic_crypto_callbacks(callbacks);
    RUVIA_CHECK(callbacks.client_initial != nullptr);
    RUVIA_CHECK(callbacks.recv_client_initial != nullptr);
    RUVIA_CHECK(callbacks.recv_crypto_data != nullptr);
    RUVIA_CHECK(callbacks.encrypt != nullptr);
    RUVIA_CHECK(callbacks.decrypt != nullptr);
    RUVIA_CHECK(callbacks.hp_mask != nullptr);
    RUVIA_CHECK(callbacks.update_key != nullptr);
    RUVIA_CHECK(callbacks.delete_crypto_aead_ctx != nullptr);
    RUVIA_CHECK(callbacks.delete_crypto_cipher_ctx != nullptr);
    RUVIA_CHECK(callbacks.recv_retry != nullptr);
    RUVIA_CHECK(callbacks.version_negotiation != nullptr);
    RUVIA_CHECK(callbacks.tls_early_data_rejected == nullptr);
}

RUVIA_TEST(quic_client_initial_callback_drives_initial_tls_and_ngtcp2_copies_client_hello) {
    initial_tls_context tls_context;
    std::pmr::synchronized_pool_resource resource;
    initial_state_owner owner(tls_context, &resource);
    native_client_fixture native;
    RUVIA_CHECK_EQ(native.initialize(ruvia_ctx, owner.state), 0);

    std::array<std::uint8_t, 1400> packet{};
    ngtcp2_pkt_info packet_info{};
    const auto written = ngtcp2_conn_write_pkt(owner.state.connection_, &native.path.path,
        &packet_info, packet.data(), packet.size(), 200);
    RUVIA_CHECK(written > 0);
    RUVIA_CHECK(tls_context.called);
    RUVIA_CHECK_EQ(tls_context.submit_status, ruvia::quic_operation_status::accepted);
    const auto packet_end = packet.begin() + static_cast<std::size_t>(written > 0 ? written : 0);
    RUVIA_CHECK(std::count(packet.begin(), packet_end, std::uint8_t{0x79}) >= 12);

    owner.state.current_read_level_ = ruvia::quic_encryption_level::application;
    const std::uint8_t late_initial_data[]{0x01, 0x02};
    ngtcp2_callbacks callbacks{};
    ruvia::detail::fill_quic_crypto_callbacks(callbacks);
    RUVIA_CHECK_EQ(callbacks.recv_crypto_data(owner.state.connection_,
                       NGTCP2_ENCRYPTION_LEVEL_INITIAL, 0, late_initial_data,
                       std::size(late_initial_data), &owner.state),
        0);
    RUVIA_CHECK_EQ(owner.state.current_read_level_, ruvia::quic_encryption_level::application);
    RUVIA_CHECK_EQ(tls_context.observed_read_level, ruvia::quic_encryption_level::application);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)owner.state.tls_handshake().take_crypto_record();
    }));
    RUVIA_CHECK_EQ(owner.state.crypto_record_count(ruvia::quic_encryption_level::initial),
        std::size_t{1});
}

RUVIA_TEST(quic_tls_driver_failure_waits_for_native_unwind_before_retirement) {
    initial_tls_context tls_context;
    tls_context.fail_drive = true;
    std::pmr::synchronized_pool_resource resource;
    {
        initial_state_owner owner(tls_context, &resource);
        native_client_fixture native;
        RUVIA_CHECK_EQ(native.initialize(ruvia_ctx, owner.state), 0);
        std::array<std::uint8_t, 1400> packet{};
        ngtcp2_pkt_info packet_info{};
        const auto written = ngtcp2_conn_write_pkt(owner.state.connection_, &native.path.path,
            &packet_info, packet.data(), packet.size(), 200);
        RUVIA_CHECK(written < 0);
        RUVIA_CHECK(owner.state.failed());
        RUVIA_CHECK_EQ(tls_context.retire_calls, std::size_t{0});
    }
    RUVIA_CHECK_EQ(tls_context.retire_calls, std::size_t{1});
}

RUVIA_TEST(quic_initial_key_install_rolls_back_empty_factories_and_native_slot_allocation_failures) {
    for (std::size_t failure_mode = 0; failure_mode < 4; ++failure_mode) {
        std::pmr::synchronized_pool_resource upstream;
        faulting_resource resource(&upstream);
        initial_tls_context tls_context;
        {
            initial_state_owner owner(tls_context, &resource);
            native_client_fixture native;
            RUVIA_CHECK_EQ(native.initialize(ruvia_ctx, owner.state), 0);
            auto& provider = owner.crypto_context;
            if (failure_mode == 0) {
                provider.empty_aead = true;
            } else if (failure_mode == 1) {
                provider.empty_header = true;
            } else {
                provider.slot_resource = &resource;
                provider.fail_slot_allocation_after = failure_mode;
            }

            std::array<std::uint8_t, 1400> packet{};
            ngtcp2_pkt_info packet_info{};
            const auto written = ngtcp2_conn_write_pkt(owner.state.connection_, &native.path.path,
                &packet_info, packet.data(), packet.size(), 200);
            RUVIA_CHECK(written < 0);
            RUVIA_CHECK(owner.state.failed());
            RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.state.rethrow_failure(); }));
            RUVIA_CHECK(!tls_context.called);
            RUVIA_CHECK_EQ(provider.destroyed, failure_mode < 2 ? std::size_t{2} : std::size_t{4});
        }
        RUVIA_CHECK_EQ(resource.allocations(), resource.deallocations());
    }
}

RUVIA_TEST(quic_c_abi_crypto_callbacks_latch_provider_failure_without_unwinding) {
    callback_context context;
    std::pmr::synchronized_pool_resource resource;
    state_owner owner(context, &resource);
    ngtcp2_callbacks callbacks{};
    ruvia::detail::fill_quic_crypto_callbacks(callbacks);
    std::array<std::uint8_t, 8> random_output{};
    ngtcp2_rand_ctx random_context{};
    ruvia::detail::initialize_quic_random_context(random_context, owner.state);
    callbacks.rand(random_output.data(), random_output.size(), &random_context);
    RUVIA_CHECK(std::ranges::all_of(random_output,
        [](std::uint8_t value) { return value == 0; }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.state.rethrow_failure(); }));

    callback_context initial_context;
    state_owner initial_owner(initial_context, &resource);
    RUVIA_CHECK_EQ(callbacks.client_initial(nullptr, &initial_owner.state),
        NGTCP2_ERR_CALLBACK_FAILURE);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { initial_owner.state.rethrow_failure(); }));
    RUVIA_CHECK_EQ(callbacks.version_negotiation(nullptr, 0xfaceU, nullptr,
                       &initial_owner.state),
        NGTCP2_ERR_VERSION_NEGOTIATION_FAILURE);
}

RUVIA_TEST(quic_decrypt_callback_distinguishes_authentication_rejection_from_provider_failure) {
    callback_context context;
    std::pmr::synchronized_pool_resource resource;
    state_owner owner(context, &resource);
    auto key = make_key(context);
    ruvia::detail::quic_aead_key_slot slot{&owner.state, &resource, std::move(key),
        ruvia::detail::quic_cipher_suite_parameters_for(
            ruvia::quic_cipher_suite::aes_128_gcm_sha256),
        0, 0};
    ngtcp2_callbacks callbacks{};
    ruvia::detail::fill_quic_crypto_callbacks(callbacks);
    ngtcp2_crypto_aead_ctx aead_context{.native_handle = &slot};
    std::array<std::uint8_t, 17> ciphertext{};
    const std::array<std::uint8_t, 1> input{0x42};
    std::array<std::uint8_t, 1> plaintext{};
    std::array<std::uint8_t, 12> nonce{};
    slot.parameters.max_encryptions = 1;
    auto result = callbacks.encrypt(ciphertext.data(), nullptr, &aead_context,
        input.data(), input.size(), nonce.data(), nonce.size(), nullptr, 0);
    RUVIA_CHECK_EQ(result, 0);
    RUVIA_CHECK_EQ(ciphertext[0], input[0]);
    RUVIA_CHECK_EQ(ciphertext[16], std::uint8_t{0x6d});
    RUVIA_CHECK_EQ(callbacks.encrypt(ciphertext.data(), nullptr, &aead_context,
                       input.data(), input.size(), nonce.data(), nonce.size(), nullptr, 0),
        NGTCP2_ERR_AEAD_LIMIT_REACHED);
    context.reject_open = true;
    result = callbacks.decrypt(plaintext.data(), nullptr, &aead_context,
        ciphertext.data(), ciphertext.size(), nonce.data(), nonce.size(), nullptr, 0);
    RUVIA_CHECK_EQ(result, NGTCP2_ERR_DECRYPT);
    RUVIA_CHECK_EQ(slot.decryption_failures, std::uint64_t{1});

    context.reject_open = false;
    context.throw_open = true;
    result = callbacks.decrypt(plaintext.data(), nullptr, &aead_context,
        ciphertext.data(), ciphertext.size(), nonce.data(), nonce.size(), nullptr, 0);
    RUVIA_CHECK_EQ(result, NGTCP2_ERR_CALLBACK_FAILURE);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.state.rethrow_failure(); }));
    aead_context.native_handle = nullptr;
}

RUVIA_TEST(quic_native_key_update_installs_both_direction_slots_and_erases_partial_outputs_on_failure) {
    std::pmr::synchronized_pool_resource resource;
    initial_tls_context tls_context;
    native_client_fixture native;
    initial_state_owner owner(tls_context, &resource);
    if (native.initialize(ruvia_ctx, owner.state) != 0) {
        return;
    }
    owner.state.installed_cipher_suite_ = ruvia::quic_cipher_suite::aes_128_gcm_sha256;

    std::array<std::uint8_t, 32> current_rx{};
    std::array<std::uint8_t, 32> current_tx{};
    std::array<std::uint8_t, 32> rx_secret{};
    std::array<std::uint8_t, 32> tx_secret{};
    std::array<std::uint8_t, 12> rx_iv{};
    std::array<std::uint8_t, 12> tx_iv{};
    ngtcp2_crypto_aead_ctx rx_context{};
    ngtcp2_crypto_aead_ctx tx_context{};
    ngtcp2_callbacks callbacks{};
    ruvia::detail::fill_quic_crypto_callbacks(callbacks);
    RUVIA_CHECK_EQ(callbacks.update_key(owner.state.connection_, rx_secret.data(), tx_secret.data(),
                       &rx_context, rx_iv.data(), &tx_context, tx_iv.data(),
                       current_rx.data(), current_tx.data(), current_rx.size(), &owner.state),
        0);
    RUVIA_CHECK(rx_context.native_handle != nullptr);
    RUVIA_CHECK(tx_context.native_handle != nullptr);
    RUVIA_CHECK(std::ranges::all_of(rx_secret, [](std::uint8_t value) { return value == 0x53; }));
    RUVIA_CHECK(std::ranges::all_of(tx_secret, [](std::uint8_t value) { return value == 0x53; }));
    RUVIA_CHECK(std::ranges::all_of(rx_iv, [](std::uint8_t value) { return value == 0x53; }));
    RUVIA_CHECK(std::ranges::all_of(tx_iv, [](std::uint8_t value) { return value == 0x53; }));
    callbacks.delete_crypto_aead_ctx(nullptr, &rx_context, nullptr);
    callbacks.delete_crypto_aead_ctx(nullptr, &tx_context, nullptr);
    RUVIA_CHECK_EQ(owner.crypto_context.destroyed, std::size_t{2});

    initial_tls_context failing_tls_context;
    native_client_fixture failing_native;
    initial_state_owner failing_owner(failing_tls_context, &resource);
    if (failing_native.initialize(ruvia_ctx, failing_owner.state) != 0) {
        return;
    }
    failing_owner.state.installed_cipher_suite_ = ruvia::quic_cipher_suite::aes_128_gcm_sha256;
    failing_owner.crypto_context.throw_aead_on_call = 2;
    rx_secret.fill(0xa1);
    tx_secret.fill(0xa2);
    rx_iv.fill(0xa3);
    tx_iv.fill(0xa4);
    rx_context.native_handle = reinterpret_cast<void*>(std::uintptr_t{1});
    tx_context.native_handle = reinterpret_cast<void*>(std::uintptr_t{2});
    RUVIA_CHECK_EQ(callbacks.update_key(failing_owner.state.connection_, rx_secret.data(), tx_secret.data(),
                       &rx_context, rx_iv.data(), &tx_context, tx_iv.data(),
                       current_rx.data(), current_tx.data(), current_rx.size(), &failing_owner.state),
        NGTCP2_ERR_CALLBACK_FAILURE);
    RUVIA_CHECK(rx_context.native_handle == nullptr);
    RUVIA_CHECK(tx_context.native_handle == nullptr);
    RUVIA_CHECK(std::ranges::all_of(rx_secret, [](std::uint8_t value) { return value == 0; }));
    RUVIA_CHECK(std::ranges::all_of(tx_secret, [](std::uint8_t value) { return value == 0; }));
    RUVIA_CHECK(std::ranges::all_of(rx_iv, [](std::uint8_t value) { return value == 0; }));
    RUVIA_CHECK(std::ranges::all_of(tx_iv, [](std::uint8_t value) { return value == 0; }));
    RUVIA_CHECK_EQ(failing_owner.crypto_context.destroyed, std::size_t{1});
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { failing_owner.state.rethrow_failure(); }));
}

RUVIA_TEST(quic_tls_failure_after_completion_fails_connection_without_overwriting_latched_transport_reason) {
    callback_context context;
    std::pmr::synchronized_pool_resource resource;
    state_owner owner(context, &resource);
    constexpr std::array<std::byte, 2> alpn{std::byte{'h'}, std::byte{'3'}};
    owner.state.latch_close_reason({.kind = ruvia::quic_close_kind::transport,
        .code = NGTCP2_TRANSPORT_PARAMETER_ERROR,
        .frame_type = 0,
        .reason = "invalid peer parameters"});
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        owner.state.tls_handshake().complete({});
    }));
    owner.state.tls_handshake().fail(ruvia::quic_tls_alert::internal_error);
    RUVIA_CHECK(!owner.state.failed());
    owner.state.tls_driver_active_ = true;
    owner.state.tls_handshake().complete({.negotiated_alpn = alpn,
        .cipher_suite = ruvia::quic_cipher_suite::aes_128_gcm_sha256});
    owner.state.tls_driver_active_ = false;
    RUVIA_CHECK(owner.state.info().tls_handshake_complete);
    RUVIA_CHECK(!owner.state.failed());

    owner.state.tls_driver_active_ = true;
    owner.state.tls_handshake().fail(ruvia::quic_tls_alert::internal_error);
    owner.state.tls_driver_active_ = false;
    RUVIA_CHECK(owner.state.failed());
    RUVIA_CHECK_EQ(owner.state.info().state, ruvia::quic_connection_state::failed);
    RUVIA_CHECK_EQ(owner.state.close_reason().code,
        static_cast<std::uint64_t>(NGTCP2_TRANSPORT_PARAMETER_ERROR));
}

RUVIA_TEST(quic_native_key_slot_typed_owner_is_released_once_by_ngtcp_delete_callback) {
    callback_context context;
    std::pmr::synchronized_pool_resource resource;
    state_owner owner(context, &resource);
    auto key = make_key(context);
    std::pmr::polymorphic_allocator<ruvia::detail::quic_aead_key_slot> allocator(&resource);
    auto* slot = allocator.allocate(1);
    std::construct_at(slot, ruvia::detail::quic_aead_key_slot{
                                &owner.state, &resource, std::move(key),
                                ruvia::detail::quic_cipher_suite_parameters_for(
                                    ruvia::quic_cipher_suite::aes_128_gcm_sha256),
                                0, 0});
    ngtcp2_crypto_aead_ctx context_view{.native_handle = slot};
    ngtcp2_callbacks callbacks{};
    ruvia::detail::fill_quic_crypto_callbacks(callbacks);
    callbacks.delete_crypto_aead_ctx(nullptr, &context_view, nullptr);
    RUVIA_CHECK(context_view.native_handle == nullptr);
    RUVIA_CHECK_EQ(context.destroyed, std::size_t{1});
}

}  // namespace
