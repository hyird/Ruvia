#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/http/quic_connection.h"
#include "ruvia/web/detail/http3/openssl_quic_crypto_provider.h"
#include "ruvia/web/detail/http3/openssl_quic_tls_session.h"

#include "test_harness.h"

namespace {

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t live_bytes{};

    void* do_allocate(std::size_t size, std::size_t alignment) override {
        void* const pointer = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++allocations;
        live_bytes += size;
        return pointer;
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++deallocations;
        live_bytes -= size;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct captured_key final {
    ruvia::quic_cipher_suite suite{};
    ruvia::quic_crypto_direction direction{};
    std::size_t id{};
    std::size_t endpoint{};
    std::array<std::byte, 32> bytes{};
    std::size_t size{};
};

struct observed_provider;

struct provider_endpoint final {
    observed_provider* owner{};
    std::size_t id{};
};

struct observed_aead_use final {
    std::size_t key_id{};
    ruvia::quic_crypto_direction direction{};
    bool seal{};
    bool short_header{};
    bool key_phase{};
    bool operation_succeeded{};
    bool authenticated{};
    bool contains_expected_payload{};
};

struct observed_aead_state final {
    observed_provider* owner{};
    std::pmr::memory_resource* resource{};
    ruvia::quic_aead_key inner;
    std::size_t endpoint{};
    std::size_t key_id{};
    ruvia::quic_crypto_direction direction{};
};

struct captured_bytes final {
    std::array<std::byte, 64> bytes{};
    std::size_t size{};

    std::span<const std::byte> view() const noexcept {
        return std::span<const std::byte>(bytes).first(size);
    }

    bool assign(std::span<const std::byte> value) noexcept {
        if (value.size() > bytes.size()) {
            return false;
        }
        std::copy(value.begin(), value.end(), bytes.begin());
        size = value.size();
        return true;
    }
};

struct captured_kdf_event final {
    bool extract{};
    std::size_t endpoint{};
    ruvia::quic_cipher_suite suite{};
    captured_bytes salt;
    captured_bytes input;
    captured_bytes secret;
    captured_bytes info;
    captured_bytes output;
    bool valid{true};
};

struct observed_provider final {
    struct endpoint_events final {
        std::array<observed_aead_use, 1024> uses{};
        std::size_t use_count{};
        std::size_t use_overflows{};
        std::array<std::size_t, 64> destroyed_keys{};
        std::size_t destroyed_count{};
        std::size_t destroy_overflows{};
    };

    observed_provider(ruvia::quic_crypto_provider_view provider,
        std::pmr::memory_resource* memory_resource)
        : inner(provider),
          resource(memory_resource),
          endpoints{{{this, 0}, {this, 1}}} {}

    ~observed_provider() noexcept {
        const auto erase = [this](auto& captures) {
            for (auto& capture : captures) {
                inner.secure_erase(inner.context, capture.bytes);
            }
        };
        erase(aead_keys);
        erase(header_keys);
        for (auto& expected : expected_payloads) {
            inner.secure_erase(inner.context, expected.bytes);
        }
        for (auto& event : kdf_events) {
            inner.secure_erase(inner.context, event.salt.bytes);
            inner.secure_erase(inner.context, event.input.bytes);
            inner.secure_erase(inner.context, event.secret.bytes);
            inner.secure_erase(inner.context, event.info.bytes);
            inner.secure_erase(inner.context, event.output.bytes);
        }
    }

    ruvia::quic_crypto_provider_view inner;
    std::pmr::memory_resource* resource{};
    std::array<provider_endpoint, 2> endpoints{};
    std::array<endpoint_events, 2> endpoint_events_by_id{};
    std::array<std::size_t, 2> next_key_id{};
    std::array<captured_bytes, 2> expected_payloads{};
    std::size_t expected_payload_overflows{};
    std::array<captured_kdf_event, 256> kdf_events{};
    std::size_t kdf_count{};
    std::size_t kdf_overflows{};
    std::array<captured_key, 64> aead_keys{};
    std::array<captured_key, 64> header_keys{};
    std::size_t aead_count{};
    std::size_t aead_overflows{};
    std::size_t aead_attempts{};
    std::size_t throw_aead_at{std::numeric_limits<std::size_t>::max()};
    std::size_t header_count{};
    std::size_t header_overflows{};

    void expect_payload(std::size_t endpoint, std::span<const std::byte> payload) noexcept {
        const bool stored = expected_payloads[endpoint].assign(payload);
        if (!stored) {
            ++expected_payload_overflows;
        }
    }

    observed_aead_use make_use(const observed_aead_state& key, bool seal,
        bool operation_succeeded, bool authenticated,
        std::span<const std::byte> associated_data, std::span<const std::byte> plaintext) const noexcept {
        const bool short_header = !associated_data.empty() &&
                                  (std::to_integer<unsigned char>(associated_data.front()) & 0xc0) == 0x40;
        const bool key_phase = short_header &&
                               (std::to_integer<unsigned char>(associated_data.front()) & 0x04) != 0;
        const auto expected = expected_payloads[key.endpoint].view();
        const bool plaintext_is_trusted = seal ? operation_succeeded : authenticated;
        const bool contains_payload = plaintext_is_trusted && !expected.empty() &&
                                      plaintext.size() >= expected.size() &&
                                      std::search(plaintext.begin(), plaintext.end(), expected.begin(), expected.end()) != plaintext.end();
        return {key.key_id, key.direction, seal, short_header, key_phase,
            operation_succeeded, authenticated, contains_payload};
    }

    void record_use(const observed_aead_state& key, observed_aead_use use) noexcept {
        auto& endpoint = endpoint_events_by_id[key.endpoint];
        if (endpoint.use_count == endpoint.uses.size()) {
            ++endpoint.use_overflows;
            return;
        }
        endpoint.uses[endpoint.use_count++] = use;
    }

    static void destroy_aead(void* opaque) noexcept {
        auto* const key = static_cast<observed_aead_state*>(opaque);
        auto& events = key->owner->endpoint_events_by_id[key->endpoint];
        if (events.destroyed_count < events.destroyed_keys.size()) {
            events.destroyed_keys[events.destroyed_count++] = key->key_id;
        } else {
            ++events.destroy_overflows;
        }
        auto* const memory_resource = key->resource;
        std::destroy_at(key);
        std::pmr::polymorphic_allocator<observed_aead_state> allocator(memory_resource);
        allocator.deallocate(key, 1);
    }

    static void seal_aead(void* opaque, std::span<const std::byte, 12> nonce,
        std::span<const std::byte> associated_data, std::span<const std::byte> plaintext,
        std::span<std::byte> ciphertext_and_tag) {
        auto& key = *static_cast<observed_aead_state*>(opaque);
        const auto use = key.owner->make_use(key, true, true, false, associated_data, plaintext);
        key.inner.seal(nonce, associated_data, plaintext, ciphertext_and_tag);
        key.owner->record_use(key, use);
    }

    static ruvia::quic_aead_key_operations::open_result open_aead(void* opaque,
        std::span<const std::byte, 12> nonce, std::span<const std::byte> associated_data,
        std::span<const std::byte> ciphertext_and_tag, std::span<std::byte> plaintext) {
        auto& key = *static_cast<observed_aead_state*>(opaque);
        const auto result = key.inner.open(nonce, associated_data, ciphertext_and_tag, plaintext);
        const bool authenticated =
            result.value == ruvia::quic_aead_key_operations::open_result::status::authenticated;
        key.owner->record_use(key, key.owner->make_use(
                                       key, false, true, authenticated, associated_data, plaintext));
        return result;
    }

    ruvia::quic_crypto_provider_view view(std::size_t endpoint_id = 0) noexcept {
        return {
            .context = &endpoints[endpoint_id],
            .random_bytes = [](void* opaque, std::span<std::byte> output) {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner;
                self.inner.random_bytes(self.inner.context, output); },
            .hkdf_extract = [](void* opaque, ruvia::quic_cipher_suite suite,
                                std::span<const std::byte> salt, std::span<const std::byte> input,
                                std::span<std::byte> output) {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner;
                self.inner.hkdf_extract(self.inner.context, suite, salt, input, output);
                if (self.kdf_count == self.kdf_events.size()) {
                    ++self.kdf_overflows;
                    return;
                }
                auto& event = self.kdf_events[self.kdf_count++];
                event.extract = true;
                event.endpoint = static_cast<provider_endpoint*>(opaque)->id;
                event.suite = suite;
                event.valid = event.salt.assign(salt) && event.input.assign(input) &&
                              event.output.assign(output); },
            .hkdf_expand = [](void* opaque, ruvia::quic_cipher_suite suite,
                               std::span<const std::byte> secret, std::span<const std::byte> info,
                               std::span<std::byte> output) {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner;
                self.inner.hkdf_expand(self.inner.context, suite, secret, info, output);
                if (self.kdf_count == self.kdf_events.size()) {
                    ++self.kdf_overflows;
                    return;
                }
                auto& event = self.kdf_events[self.kdf_count++];
                event.endpoint = static_cast<provider_endpoint*>(opaque)->id;
                event.suite = suite;
                event.valid = event.secret.assign(secret) && event.info.assign(info) &&
                              event.output.assign(output); },
            .create_aead_key = [](void* opaque, ruvia::quic_cipher_suite suite,
                                   ruvia::quic_crypto_direction direction,
                                   std::span<const std::byte> key_bytes) {
                auto& endpoint = *static_cast<provider_endpoint*>(opaque);
                auto& self = *endpoint.owner;
                if (self.aead_attempts++ == self.throw_aead_at) {
                    throw std::runtime_error("injected QUIC AEAD key factory failure");
                }
                const auto key_id = self.next_key_id[endpoint.id]++;
                if (self.aead_count < self.aead_keys.size() && key_bytes.size() <= self.aead_keys[0].bytes.size()) {
                    auto& capture = self.aead_keys[self.aead_count++];
                    capture.suite = suite;
                    capture.direction = direction;
                    capture.id = key_id;
                    capture.endpoint = endpoint.id;
                    capture.size = key_bytes.size();
                    std::copy(key_bytes.begin(), key_bytes.end(), capture.bytes.begin());
                } else {
                    ++self.aead_overflows;
                }
                auto inner = self.inner.create_aead_key(self.inner.context, suite, direction, key_bytes);
                std::pmr::polymorphic_allocator<observed_aead_state> allocator(self.resource);
                auto* const state = allocator.allocate(1);
                try {
                    std::construct_at(state, observed_aead_state{&self, self.resource,
                        std::move(inner), endpoint.id, key_id, direction});
                } catch (...) {
                    allocator.deallocate(state, 1);
                    throw;
                }
                return ruvia::quic_aead_key::adopt(state, {
                    .destroy = destroy_aead,
                    .seal = seal_aead,
                    .open = open_aead,
                }); },
            .create_header_protection_key = [](void* opaque, ruvia::quic_cipher_suite suite,
                                                std::span<const std::byte> key_bytes) {
                auto& endpoint = *static_cast<provider_endpoint*>(opaque);
                auto& self = *endpoint.owner;
                if (self.header_count < self.header_keys.size() && key_bytes.size() <= self.header_keys[0].bytes.size()) {
                    auto& capture = self.header_keys[self.header_count++];
                    capture.suite = suite;
                    capture.id = self.header_count - 1;
                    capture.endpoint = endpoint.id;
                    capture.size = key_bytes.size();
                    std::copy(key_bytes.begin(), key_bytes.end(), capture.bytes.begin());
                } else {
                    ++self.header_overflows;
                }
                return self.inner.create_header_protection_key(self.inner.context, suite, key_bytes); },
            .secure_erase = [](void* opaque, std::span<std::byte> bytes) noexcept {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner;
                self.inner.secure_erase(self.inner.context, bytes); },
        };
    }
};

bool all_aead_keys_destroyed_once(const observed_provider& provider) {
    if (provider.aead_overflows != 0) {
        return false;
    }
    for (std::size_t index = 0; index < provider.aead_count; ++index) {
        const auto& key = provider.aead_keys[index];
        const auto& endpoint = provider.endpoint_events_by_id[key.endpoint];
        const auto destroys = std::count(endpoint.destroyed_keys.begin(),
            endpoint.destroyed_keys.begin() + static_cast<std::ptrdiff_t>(endpoint.destroyed_count),
            key.id);
        if (destroys != 1) {
            return false;
        }
    }
    return true;
}

bool contains_key(const captured_key* keys, std::size_t count,
    std::span<const std::byte> expected, std::optional<ruvia::quic_crypto_direction> direction = {}) {
    for (std::size_t index = 0; index < count; ++index) {
        const auto& candidate = keys[index];
        if (candidate.size == expected.size() &&
            (!direction || candidate.direction == *direction) &&
            std::equal(expected.begin(), expected.end(), candidate.bytes.begin())) {
            return true;
        }
    }
    return false;
}

std::vector<std::byte> tls13_label(std::string_view label, std::size_t size);

bool contains_extract(const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::span<const std::byte> salt, std::span<const std::byte> input,
    std::span<const std::byte> output) {
    for (std::size_t index = 0; index < provider.kdf_count; ++index) {
        const auto& event = provider.kdf_events[index];
        if (event.valid && event.extract && event.suite == suite &&
            std::ranges::equal(event.salt.view(), salt) &&
            std::ranges::equal(event.input.view(), input) &&
            std::ranges::equal(event.output.view(), output)) {
            return true;
        }
    }
    return false;
}

bool contains_expand(const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::span<const std::byte> secret, std::string_view label, std::size_t output_size,
    std::span<const std::byte> expected_output = {}) {
    const auto expected_info = tls13_label(label, output_size);
    for (std::size_t index = 0; index < provider.kdf_count; ++index) {
        const auto& event = provider.kdf_events[index];
        if (!event.valid || event.extract || event.suite != suite ||
            event.secret.size != secret.size() || event.output.size != output_size ||
            !std::ranges::equal(event.secret.view(), secret) ||
            !std::ranges::equal(event.info.view(), expected_info)) {
            continue;
        }
        if (expected_output.empty() || std::ranges::equal(event.output.view(), expected_output)) {
            return true;
        }
    }
    return false;
}

std::optional<std::span<const std::byte>> find_expand_output(
    const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::span<const std::byte> secret, std::string_view label, std::size_t output_size) {
    const auto expected_info = tls13_label(label, output_size);
    for (std::size_t index = 0; index < provider.kdf_count; ++index) {
        const auto& event = provider.kdf_events[index];
        if (event.valid && !event.extract && event.suite == suite &&
            event.secret.size == secret.size() && event.output.size == output_size &&
            std::ranges::equal(event.secret.view(), secret) &&
            std::ranges::equal(event.info.view(), expected_info)) {
            return event.output.view();
        }
    }
    return std::nullopt;
}

const captured_key* find_aead_capture(const observed_provider& provider, std::size_t id,
    std::size_t endpoint, ruvia::quic_crypto_direction direction) {
    for (std::size_t index = 0; index < provider.aead_count; ++index) {
        const auto& key = provider.aead_keys[index];
        if (key.id == id && key.endpoint == endpoint && key.direction == direction) {
            return &key;
        }
    }
    return nullptr;
}

std::optional<std::span<const std::byte>> secret_for_aead_capture(
    const observed_provider& provider, const captured_key& key) {
    for (std::size_t index = 0; index < provider.kdf_count; ++index) {
        const auto& event = provider.kdf_events[index];
        if (event.valid && !event.extract && event.suite == key.suite &&
            std::ranges::equal(event.info.view(), tls13_label("quic key", key.size)) &&
            event.output.size == key.size &&
            std::ranges::equal(event.output.view(),
                std::span<const std::byte>(key.bytes).first(key.size))) {
            return event.secret.view();
        }
    }
    return std::nullopt;
}

bool is_secret(const captured_bytes& secret, std::span<const std::byte> first,
    std::span<const std::byte> second) {
    return std::ranges::equal(secret.view(), first) || std::ranges::equal(secret.view(), second);
}

bool contains_expand_shape(const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::string_view label, std::size_t secret_size, std::size_t output_size,
    std::span<const std::byte> initial_client_secret,
    std::span<const std::byte> initial_server_secret) {
    const auto expected_info = tls13_label(label, output_size);
    for (std::size_t index = 0; index < provider.kdf_count; ++index) {
        const auto& event = provider.kdf_events[index];
        if (event.valid && !event.extract && event.suite == suite &&
            event.secret.size == secret_size && event.output.size == output_size &&
            !is_secret(event.secret, initial_client_secret, initial_server_secret) &&
            std::ranges::equal(event.info.view(), expected_info)) {
            return true;
        }
    }
    return false;
}

bool contains_expand_key_capture(const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::string_view label, std::size_t secret_size, const captured_key* keys, std::size_t key_count,
    std::span<const std::byte> initial_client_secret,
    std::span<const std::byte> initial_server_secret) {
    for (std::size_t index = 0; index < provider.kdf_count; ++index) {
        const auto& event = provider.kdf_events[index];
        if (!event.valid || event.extract || event.suite != suite ||
            event.secret.size != secret_size ||
            is_secret(event.secret, initial_client_secret, initial_server_secret) ||
            !std::ranges::equal(event.info.view(), tls13_label(label, event.output.size))) {
            continue;
        }
        if (contains_key(keys, key_count, event.output.view())) {
            return true;
        }
    }
    return false;
}

unsigned hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned>(value - 'a' + 10);
    }
    throw std::invalid_argument("invalid hex QUIC test vector");
}

std::vector<std::byte> hex_bytes(std::string_view value) {
    std::vector<std::byte> result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index + 1 < value.size(); index += 2) {
        result.push_back(static_cast<std::byte>((hex_digit(value[index]) << 4) | hex_digit(value[index + 1])));
    }
    return result;
}

std::vector<std::byte> tls13_label(std::string_view label, std::size_t size) {
    constexpr std::string_view prefix = "tls13 ";
    std::vector<std::byte> info;
    info.push_back(static_cast<std::byte>((size >> 8) & 0xff));
    info.push_back(static_cast<std::byte>(size & 0xff));
    info.push_back(static_cast<std::byte>(prefix.size() + label.size()));
    for (const char value : prefix) {
        info.push_back(static_cast<std::byte>(value));
    }
    for (const char value : label) {
        info.push_back(static_cast<std::byte>(value));
    }
    info.push_back(std::byte{0});
    return info;
}

std::array<std::byte, 16> derive_client_initial_key(ruvia::quic_crypto_provider_view crypto,
    std::span<const std::byte> destination_connection_id) {
    const auto salt = hex_bytes("38762cf7f55934b34d179ae6a4c80cadccbb7f0a");
    std::array<std::byte, 32> initial_secret{};
    std::array<std::byte, 32> client_secret{};
    std::array<std::byte, 16> key{};
    crypto.hkdf_extract(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        salt, destination_connection_id, initial_secret);
    const auto client_label = tls13_label("client in", client_secret.size());
    crypto.hkdf_expand(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        initial_secret, client_label, client_secret);
    const auto key_label = tls13_label("quic key", key.size());
    crypto.hkdf_expand(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        client_secret, key_label, key);
    return key;
}

using ssl_ctx_owner = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using ssl_owner = std::unique_ptr<SSL, decltype(&SSL_free)>;
using key_owner = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using certificate_owner = std::unique_ptr<X509, decltype(&X509_free)>;

struct retaining_tls_driver final {
    std::optional<ruvia::quic_crypto_record_lease> record;

    static ruvia::quic_tls_drive_result drive(void* context,
        ruvia::quic_tls_handshake& handshake) noexcept {
        auto& self = *static_cast<retaining_tls_driver*>(context);
        try {
            if (!self.record) {
                auto incoming = handshake.take_crypto_record();
                if (incoming) {
                    self.record.emplace(std::move(incoming));
                }
            }
            return {ruvia::quic_tls_progress::need_input, ruvia::quic_tls_alert::internal_error};
        } catch (...) {
            handshake.fail(ruvia::quic_tls_alert::internal_error);
            return {ruvia::quic_tls_progress::failed, ruvia::quic_tls_alert::internal_error};
        }
    }

    static void retire(void* context) noexcept {
        static_cast<retaining_tls_driver*>(context)->release();
    }

    ruvia::quic_tls_driver_view view() noexcept {
        return {.context = this, .drive = drive, .retire = retire};
    }

    void release() noexcept {
        record.reset();
    }
};

certificate_owner make_certificate(EVP_PKEY* key) {
    certificate_owner certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) == nullptr ||
        X509_set_pubkey(certificate.get(), key) != 1) {
        throw std::runtime_error("failed to construct QUIC test certificate");
    }
    X509_NAME* const subject = X509_get_subject_name(certificate.get());
    constexpr char common_name[] = "localhost";
    if (!subject || X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1) {
        throw std::runtime_error("failed to set QUIC test certificate subject");
    }
    X509V3_CTX extensions;
    X509V3_set_ctx(&extensions, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
        X509V3_EXT_conf_nid(nullptr, &extensions, NID_subject_alt_name,
            const_cast<char*>("DNS:localhost")),
        X509_EXTENSION_free);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> constraints(
        X509V3_EXT_conf_nid(nullptr, &extensions, NID_basic_constraints,
            const_cast<char*>("critical,CA:TRUE")),
        X509_EXTENSION_free);
    if (!san || !constraints || X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
        X509_add_ext(certificate.get(), constraints.get(), -1) != 1 ||
        X509_sign(certificate.get(), key, EVP_sha256()) <= 0) {
        throw std::runtime_error("failed to sign QUIC test certificate");
    }
    return certificate;
}

int select_h3_alpn(SSL*, const unsigned char** output, unsigned char* output_length,
    const unsigned char* input, unsigned int input_length, void* argument) noexcept {
    const auto expected = *static_cast<const std::string_view*>(argument);
    for (unsigned int offset = 0; offset < input_length;) {
        const auto length = input[offset++];
        if (length > input_length - offset) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (length == expected.size() &&
            std::memcmp(input + offset, expected.data(), length) == 0) {
            *output = input + offset;
            *output_length = length;
            return SSL_TLSEXT_ERR_OK;
        }
        offset += length;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

ruvia::quic_address address(std::uint16_t port) {
    ruvia::quic_address result;
    result.bytes[0] = std::byte{127};
    result.bytes[3] = std::byte{1};
    result.port = port;
    result.family = ruvia::quic_address_family::ipv4;
    return result;
}

ruvia::quic_connection_id connection_id(std::array<unsigned char, 8> bytes) {
    return ruvia::quic_connection_id(std::as_bytes(std::span(bytes)));
}

bool read_quic_varint(std::span<const std::byte> bytes, std::size_t& offset,
    std::uint64_t& value) noexcept {
    if (offset >= bytes.size()) {
        return false;
    }
    const auto first = std::to_integer<std::uint8_t>(bytes[offset++]);
    const auto width = std::size_t{1} << (first >> 6);
    if (width > bytes.size() - offset + 1) {
        return false;
    }
    value = first & 0x3f;
    for (std::size_t index = 1; index < width; ++index) {
        value = (value << 8) | std::to_integer<std::uint8_t>(bytes[offset++]);
    }
    return true;
}

bool initial_has_destination_and_token(std::span<const std::byte> packet,
    std::span<const std::byte> expected_destination, std::span<const std::byte> expected_token) noexcept {
    if (packet.size() < 7 ||
        (std::to_integer<std::uint8_t>(packet[0]) & 0xf0) != 0xc0 ||
        packet[1] != std::byte{0} || packet[2] != std::byte{0} ||
        packet[3] != std::byte{0} || packet[4] != std::byte{1}) {
        return false;
    }
    std::size_t offset = 5;
    const auto destination_size = std::to_integer<std::uint8_t>(packet[offset++]);
    if (destination_size != expected_destination.size() || destination_size > packet.size() - offset ||
        !std::equal(expected_destination.begin(), expected_destination.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset))) {
        return false;
    }
    offset += destination_size;
    if (offset >= packet.size()) {
        return false;
    }
    const auto source_size = std::to_integer<std::uint8_t>(packet[offset++]);
    if (source_size > packet.size() - offset) {
        return false;
    }
    offset += source_size;
    std::uint64_t token_size{};
    if (!read_quic_varint(packet, offset, token_size) || token_size != expected_token.size() ||
        token_size > packet.size() - offset) {
        return false;
    }
    return std::equal(expected_token.begin(), expected_token.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset));
}

struct packet_pair final {
    counting_resource resource;
    ruvia::detail::openssl_quic_crypto_provider crypto{&resource};
    observed_provider observed{crypto.view(), &resource};
    ssl_ctx_owner client_context{SSL_CTX_new(TLS_method()), SSL_CTX_free};
    ssl_ctx_owner server_context{SSL_CTX_new(TLS_method()), SSL_CTX_free};
    std::optional<ruvia::detail::openssl_quic_tls_session> client_tls;
    std::optional<ruvia::detail::openssl_quic_tls_session> server_tls;
    retaining_tls_driver server_lease_driver;
    std::optional<ruvia::quic_connection> client;
    std::optional<ruvia::quic_connection> server;
    ruvia::quic_address client_address{address(43001)};
    ruvia::quic_address server_address{address(4433)};
    std::array<std::byte, 2048> datagram{};
    ruvia::quic_connection_id initial_destination_id{connection_id({0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08})};
    ruvia::quic_connection_id client_source_id{connection_id({1, 2, 3, 4, 5, 6, 7, 8})};
    ruvia::quic_connection_id server_source_id{connection_id({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88})};
    std::string_view server_alpn_{"h3"};
    std::size_t crypto_live_bytes{};
    std::size_t crypto_live_allocations{};
    bool datagrams_enabled{};
    bool client_receive_crypto_failure{};
    bool server_receive_crypto_failure{};
    ruvia::quic_timestamp now{};

    explicit packet_pair(std::string_view cipher = "TLS_AES_128_GCM_SHA256",
        std::string_view server_alpn = "h3", bool trust_server = true,
        bool enable_datagrams = false, std::string_view client_host = "localhost",
        bool offer_alpn = true, bool invalid_local_parameters = false,
        bool empty_client_source_id = false, bool retain_server_record = false,
        bool advertise_server_datagrams = true)
        : datagrams_enabled(enable_datagrams) {
        crypto_live_bytes = resource.live_bytes;
        crypto_live_allocations = resource.allocations - resource.deallocations;
        server_alpn_ = server_alpn;
        if (!client_context || !server_context) {
            throw std::runtime_error("failed to create QUIC test TLS contexts");
        }
        if (SSL_CTX_set_min_proto_version(client_context.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(client_context.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_min_proto_version(server_context.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(server_context.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_ciphersuites(client_context.get(), std::string(cipher).c_str()) != 1 ||
            SSL_CTX_set_ciphersuites(server_context.get(), std::string(cipher).c_str()) != 1) {
            throw std::runtime_error("failed to configure QUIC test TLS contexts");
        }
        SSL_CTX_set_verify(client_context.get(), SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_verify(server_context.get(), SSL_VERIFY_NONE, nullptr);

        key_owner key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free);
        if (!key) {
            throw std::runtime_error("failed to generate QUIC test certificate key");
        }
        auto certificate = make_certificate(key.get());
        if (SSL_CTX_use_certificate(server_context.get(), certificate.get()) != 1 ||
            SSL_CTX_use_PrivateKey(server_context.get(), key.get()) != 1 ||
            SSL_CTX_check_private_key(server_context.get()) != 1) {
            throw std::runtime_error("failed to install QUIC test certificate");
        }
        if (trust_server && X509_STORE_add_cert(SSL_CTX_get_cert_store(client_context.get()),
                                certificate.get()) != 1) {
            throw std::runtime_error("failed to trust QUIC test certificate");
        }
        SSL_CTX_set_num_tickets(server_context.get(), 2);
        SSL_CTX_set_alpn_select_cb(server_context.get(), select_h3_alpn, &server_alpn_);

        constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
        client_tls.emplace(client_context.get(), ruvia::quic_role::client,
            offer_alpn ? std::span<const unsigned char>(alpn) : std::span<const unsigned char>{},
            client_host, &resource);
        server_tls.emplace(server_context.get(), ruvia::quic_role::server,
            std::span<const unsigned char>{}, std::string_view{}, &resource);

        ruvia::quic_connection_config client_config;
        client_config.role = ruvia::quic_role::client;
        client_config.local_address = client_address;
        client_config.peer_address = server_address;
        client_config.destination_connection_id = initial_destination_id;
        client_config.source_connection_id = empty_client_source_id
                                                 ? ruvia::quic_connection_id{}
                                                 : client_source_id;
        if (datagrams_enabled) {
            client_config.local_transport_parameters.max_datagram_frame_size = 1200;
        }
        client.emplace(client_config, observed.view(0), client_tls->driver_view(),
            &resource, now);

        ruvia::quic_connection_config server_config;
        server_config.role = ruvia::quic_role::server;
        server_config.local_address = server_address;
        server_config.peer_address = client_address;
        server_config.destination_connection_id = client_config.source_connection_id.value();
        server_config.source_connection_id = server_source_id;
        server_config.original_destination_connection_id = initial_destination_id;
        if (datagrams_enabled && advertise_server_datagrams) {
            server_config.local_transport_parameters.max_datagram_frame_size = 1200;
        }
        if (invalid_local_parameters) {
            server_config.local_transport_parameters.max_udp_payload_size = 1199;
        }
        server.emplace(server_config, observed.view(1),
            retain_server_record ? server_lease_driver.view() : server_tls->driver_view(),
            &resource, now);
    }

    packet_pair(const packet_pair&) = delete;
    packet_pair& operator=(const packet_pair&) = delete;

    ~packet_pair() {
        retire_connections();
    }

    void retire_connections() noexcept {
        if (client_tls) {
            client_tls->stop();
        }
        if (server_tls) {
            server_tls->stop();
        }
        client.reset();
        server.reset();
        client_tls.reset();
        server_tls.reset();
    }

    bool transfer_client_to_server() {
        ruvia::quic_packet_result packet;
        try {
            packet = client->write_packet(datagram, now);
        } catch (const ruvia::quic_error&) {
            client_receive_crypto_failure = true;
            return false;
        }
        if (packet.size != 0) {
            const ruvia::quic_datagram_view input{
                .bytes = std::span<const std::byte>(datagram).first(packet.size),
                .local = server_address,
                .peer = client_address,
            };
            try {
                (void)server->receive(input, now);
            } catch (const ruvia::quic_error&) {
                server_receive_crypto_failure = true;
            }
            return true;
        }
        return false;
    }

    bool transfer_server_to_client() {
        ruvia::quic_packet_result packet;
        try {
            packet = server->write_packet(datagram, now);
        } catch (const ruvia::quic_error&) {
            server_receive_crypto_failure = true;
            return false;
        }
        if (packet.size != 0) {
            const ruvia::quic_datagram_view input{
                .bytes = std::span<const std::byte>(datagram).first(packet.size),
                .local = client_address,
                .peer = server_address,
            };
            try {
                (void)client->receive(input, now);
            } catch (const ruvia::quic_error&) {
                client_receive_crypto_failure = true;
            }
            return true;
        }
        return false;
    }

    bool resources_released() const noexcept {
        return resource.live_bytes == crypto_live_bytes &&
               resource.allocations - resource.deallocations == crypto_live_allocations;
    }

    bool terminal() noexcept {
        const auto client_info = client->info();
        const auto server_info = server->info();
        return (client_info.confirmed && server_info.confirmed) ||
               client_info.state == ruvia::quic_connection_state::failed ||
               server_info.state == ruvia::quic_connection_state::failed ||
               client->tls_handshake().failed() || server->tls_handshake().failed() ||
               client_receive_crypto_failure || server_receive_crypto_failure;
    }

    bool drive_until_terminal() {
        for (std::size_t attempt = 0; attempt < 4000 && !terminal(); ++attempt) {
            transfer_client_to_server();
            transfer_server_to_client();
            now += std::chrono::milliseconds(1);
        }
        return terminal();
    }
};

}  // namespace

RUVIA_TEST(openssl_quic_tls_session_binds_stable_handshake_and_releases_crypto_keys) {
    counting_resource resource;
    {
        ruvia::detail::openssl_quic_crypto_provider crypto(&resource);
        auto crypto_view = crypto.view();
        ssl_ctx_owner tls_context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
        RUVIA_CHECK(tls_context != nullptr);
        RUVIA_CHECK(SSL_CTX_set_min_proto_version(tls_context.get(), TLS1_3_VERSION) == 1);
        RUVIA_CHECK(SSL_CTX_set_max_proto_version(tls_context.get(), TLS1_3_VERSION) == 1);
        SSL_CTX_set_verify(tls_context.get(), SSL_VERIFY_NONE, nullptr);
        constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
        ruvia::detail::openssl_quic_tls_session session(
            tls_context.get(), ruvia::quic_role::client, alpn, {}, &resource);
        ruvia::quic_connection_config config;
        config.role = ruvia::quic_role::client;
        config.local_address = address(43001);
        config.peer_address = address(4433);
        ruvia::quic_connection connection(config, crypto_view, session.driver_view(), &resource,
            std::chrono::steady_clock::now());
        std::array<std::byte, 1500> initial_packet{};
        const auto result = connection.write_packet(initial_packet, std::chrono::steady_clock::now());
        RUVIA_CHECK(result.size != 0);
        RUVIA_CHECK(!connection.tls_handshake().failed());
        session.stop();
    }
    RUVIA_CHECK(resource.allocations == resource.deallocations);
}

RUVIA_TEST(openssl_quic_tls_session_cold_unstarted_owner_can_be_discarded) {
    counting_resource resource;
    {
        ruvia::detail::openssl_quic_crypto_provider crypto(&resource);
        ssl_ctx_owner tls_context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
        RUVIA_CHECK(tls_context != nullptr);
        constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
        ruvia::detail::openssl_quic_tls_session session(
            tls_context.get(), ruvia::quic_role::client, alpn, "localhost", &resource);
    }
    RUVIA_CHECK(resource.allocations == resource.deallocations);
    RUVIA_CHECK(resource.live_bytes == 0);
}

RUVIA_TEST(openssl_quic_tls_session_rejects_early_data_and_latches_callback_failure) {
    ssl_ctx_owner tls_context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    RUVIA_CHECK(tls_context != nullptr);
    SSL_CTX_set_verify(tls_context.get(), SSL_VERIFY_NONE, nullptr);
    constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
    ruvia::detail::openssl_quic_tls_session session(
        tls_context.get(), ruvia::quic_role::client, alpn);
    ruvia::detail::openssl_quic_crypto_provider crypto(std::pmr::get_default_resource());
    ruvia::quic_connection_config config;
    config.role = ruvia::quic_role::client;
    config.local_address = address(43001);
    config.peer_address = address(4433);
    ruvia::quic_connection connection(config, crypto.view(), session.driver_view(),
        std::pmr::get_default_resource(), std::chrono::steady_clock::now());

    constexpr std::array<unsigned char, 32> secret{};
    RUVIA_CHECK(ruvia::detail::openssl_quic_tls_session::yield_secret(nullptr,
                    OSSL_RECORD_PROTECTION_LEVEL_EARLY, 0, secret.data(), secret.size(), &session) == 0);
    const auto first = session.drive(connection.tls_handshake());
    const auto second = session.drive(connection.tls_handshake());
    RUVIA_CHECK(first.progress == ruvia::quic_tls_progress::failed);
    RUVIA_CHECK(first.alert == ruvia::quic_tls_alert::internal_error);
    RUVIA_CHECK(second.progress == ruvia::quic_tls_progress::failed);
    RUVIA_CHECK(second.alert == first.alert);
    session.stop();
}

RUVIA_TEST(openssl_quic_public_packet_pair_completes_all_tls13_suites_and_retains_metadata) {
    constexpr std::array suites{
        std::pair{"TLS_AES_128_GCM_SHA256", ruvia::quic_cipher_suite::aes_128_gcm_sha256},
        std::pair{"TLS_AES_256_GCM_SHA384", ruvia::quic_cipher_suite::aes_256_gcm_sha384},
        std::pair{"TLS_CHACHA20_POLY1305_SHA256", ruvia::quic_cipher_suite::chacha20_poly1305_sha256},
    };
    for (const auto& [name, expected_suite] : suites) {
        packet_pair pair(name);
        RUVIA_CHECK(pair.client->update_key(pair.now) ==
                    ruvia::quic_operation_status::would_block);
        RUVIA_CHECK(pair.drive_until_terminal());
        const auto client_info = pair.client->info();
        const auto server_info = pair.server->info();
        RUVIA_CHECK(client_info.confirmed);
        RUVIA_CHECK(server_info.confirmed);
        RUVIA_CHECK(client_info.tls_handshake_complete);
        RUVIA_CHECK(server_info.tls_handshake_complete);
        RUVIA_CHECK(client_info.quic_handshake_complete);
        RUVIA_CHECK(server_info.quic_handshake_complete);
        RUVIA_CHECK(pair.client->tls_handshake().info().cipher_suite == expected_suite);
        RUVIA_CHECK(pair.server->tls_handshake().info().cipher_suite == expected_suite);
        const auto client_initial_key = std::array<std::byte, 16>{
            std::byte{0x1f}, std::byte{0x36}, std::byte{0x96}, std::byte{0x13}, std::byte{0xdd}, std::byte{0x76},
            std::byte{0xd5}, std::byte{0x46}, std::byte{0x77}, std::byte{0x30}, std::byte{0xef}, std::byte{0xcb},
            std::byte{0xe3}, std::byte{0xb1}, std::byte{0xa2}, std::byte{0x2d}};
        const auto server_initial_key = std::array<std::byte, 16>{
            std::byte{0xcf}, std::byte{0x3a}, std::byte{0x53}, std::byte{0x31}, std::byte{0x65}, std::byte{0x3c},
            std::byte{0x36}, std::byte{0x4c}, std::byte{0x88}, std::byte{0xf0}, std::byte{0xf3}, std::byte{0x79},
            std::byte{0xb6}, std::byte{0x06}, std::byte{0x7e}, std::byte{0x37}};
        const auto client_initial_hp = std::array<std::byte, 16>{
            std::byte{0x9f}, std::byte{0x50}, std::byte{0x44}, std::byte{0x9e}, std::byte{0x04}, std::byte{0xa0},
            std::byte{0xe8}, std::byte{0x10}, std::byte{0x28}, std::byte{0x3a}, std::byte{0x1e}, std::byte{0x99},
            std::byte{0x33}, std::byte{0xad}, std::byte{0xed}, std::byte{0xd2}};
        const auto server_initial_hp = std::array<std::byte, 16>{
            std::byte{0xc2}, std::byte{0x06}, std::byte{0xb8}, std::byte{0xd9}, std::byte{0xb9}, std::byte{0xf0},
            std::byte{0xf3}, std::byte{0x76}, std::byte{0x44}, std::byte{0x43}, std::byte{0x0b}, std::byte{0x49},
            std::byte{0x0e}, std::byte{0xea}, std::byte{0xa3}, std::byte{0x14}};
        const auto initial_salt = hex_bytes("38762cf7f55934b34d179ae6a4c80cadccbb7f0a");
        const auto expected_initial_secret = hex_bytes(
            "7db5df06e7a69e432496adedb00851923595221596ae2ae9fb8115c1e9ed0a44");
        const auto expected_client_initial_secret = hex_bytes(
            "c00cf151ca5be075ed0ebfb5c80323c42d6b7db67881289af4008f1f6c357aea");
        const auto expected_server_initial_secret = hex_bytes(
            "3c199828fd139efd216c155ad844cc81fb82fa8d7446fa7d78be803acdda951b");
        RUVIA_CHECK(pair.observed.kdf_overflows == 0);
        RUVIA_CHECK(pair.observed.expected_payload_overflows == 0);
        RUVIA_CHECK(std::all_of(pair.observed.kdf_events.begin(),
            pair.observed.kdf_events.begin() + static_cast<std::ptrdiff_t>(pair.observed.kdf_count),
            [](const captured_kdf_event& event) { return event.valid; }));
        RUVIA_CHECK(contains_extract(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            initial_salt, pair.initial_destination_id.view(), expected_initial_secret));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_initial_secret, "client in", 32, expected_client_initial_secret));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_initial_secret, "server in", 32, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_initial_secret, "quic key", 16, client_initial_key));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_initial_secret, "quic iv", 12, hex_bytes("fa044b2f42a3fd3b46fb255c")));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_initial_secret, "quic hp", 16, client_initial_hp));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_initial_secret, "quic key", 16, server_initial_key));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_initial_secret, "quic iv", 12, hex_bytes("0ac1493ca1905853b0bba03e")));
        RUVIA_CHECK(contains_expand(pair.observed, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_initial_secret, "quic hp", 16, server_initial_hp));
        RUVIA_CHECK(contains_key(pair.observed.aead_keys.data(), pair.observed.aead_count,
            client_initial_key, ruvia::quic_crypto_direction::write));
        RUVIA_CHECK(contains_key(pair.observed.aead_keys.data(), pair.observed.aead_count,
            server_initial_key, ruvia::quic_crypto_direction::read));
        RUVIA_CHECK(contains_key(pair.observed.header_keys.data(), pair.observed.header_count,
            client_initial_hp));
        RUVIA_CHECK(contains_key(pair.observed.header_keys.data(), pair.observed.header_count,
            server_initial_hp));
        const auto [hash_size, key_size] = expected_suite == ruvia::quic_cipher_suite::aes_128_gcm_sha256
                                               ? std::pair<std::size_t, std::size_t>{32, 16}
                                           : expected_suite == ruvia::quic_cipher_suite::aes_256_gcm_sha384
                                               ? std::pair<std::size_t, std::size_t>{48, 32}
                                               : std::pair<std::size_t, std::size_t>{32, 32};
        RUVIA_CHECK(contains_expand_shape(pair.observed, expected_suite, "quic key",
            hash_size, key_size, expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_shape(pair.observed, expected_suite, "quic iv",
            hash_size, 12, expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_shape(pair.observed, expected_suite, "quic hp",
            hash_size, key_size, expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_key_capture(pair.observed, expected_suite, "quic key",
            hash_size, pair.observed.aead_keys.data(), pair.observed.aead_count,
            expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_key_capture(pair.observed, expected_suite, "quic hp",
            hash_size, pair.observed.header_keys.data(), pair.observed.header_count,
            expected_client_initial_secret, expected_server_initial_secret));
        const auto alpn = pair.client->tls_handshake().info().negotiated_alpn;
        RUVIA_CHECK(alpn.size() == 2);
        RUVIA_CHECK(std::memcmp(alpn.data(), "h3", 2) == 0);
        const auto local_parameters = pair.client->tls_handshake().local_transport_parameters();
        std::vector<std::byte> retained_parameters(local_parameters.begin(), local_parameters.end());

        for (int operation = 0; operation < 8; ++operation) {
            pair.transfer_client_to_server();
            pair.transfer_server_to_client();
            pair.now += std::chrono::milliseconds(1);
            const auto current_parameters = pair.client->tls_handshake().local_transport_parameters();
            RUVIA_CHECK(std::equal(retained_parameters.begin(), retained_parameters.end(),
                current_parameters.begin(), current_parameters.end()));
            (void)pair.client_tls->drive(pair.client->tls_handshake());
            (void)pair.server_tls->drive(pair.server->tls_handshake());
            RUVIA_CHECK(!pair.client->tls_handshake().failed());
            RUVIA_CHECK(!pair.server->tls_handshake().failed());
        }
        const auto header_key_count = pair.observed.header_count;
        std::size_t prior_aead_key_count = pair.observed.aead_count;
        auto& client_crypto = pair.observed.endpoint_events_by_id[0];
        auto& server_crypto = pair.observed.endpoint_events_by_id[1];
        std::optional<std::size_t> previous_write_key_id;
        bool previous_key_phase{};
        for (std::size_t index = 0; index < client_crypto.use_count; ++index) {
            const auto& use = client_crypto.uses[index];
            if (use.seal && use.short_header) {
                previous_write_key_id = use.key_id;
                previous_key_phase = use.key_phase;
            }
        }
        RUVIA_CHECK(previous_write_key_id.has_value());
        std::size_t previous_use_count = client_crypto.use_count;
        constexpr auto three_initial_ptos = 3 * (std::chrono::milliseconds(333) +
                                                    std::chrono::milliseconds(25));
        for (int generation = 0; generation < 3; ++generation) {
            RUVIA_CHECK(pair.client->info().confirmed);
            const auto update_time = pair.now;
            RUVIA_CHECK(pair.client->update_key(update_time) ==
                        ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(pair.observed.aead_count == prior_aead_key_count);

            const auto opened = pair.client->open_stream(false);
            RUVIA_CHECK(opened.status == ruvia::quic_operation_status::accepted);
            const std::array<std::byte, 8> payload{
                std::byte{'k'}, std::byte{'u'}, std::byte{'-'},
                static_cast<std::byte>('0' + generation), std::byte{'-'},
                static_cast<std::byte>('a' + generation), std::byte{'c'}, std::byte{'k'}};
            pair.observed.expect_payload(0, payload);
            pair.observed.expect_payload(1, payload);
            const auto server_use_start = server_crypto.use_count;
            const auto write = pair.client->write_stream(opened.stream_id, payload, true);
            RUVIA_CHECK(write.status == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(write.accepted == payload.size());

            bool received_fin{};
            bool blocked_before_ack{};
            std::vector<std::byte> received_payload;
            std::array<std::byte, 32> received{};
            for (int attempt = 0; attempt < 128 && !received_fin; ++attempt) {
                (void)pair.transfer_client_to_server();
                (void)pair.server->accept_streams();
                for (;;) {
                    const auto read = pair.server->read_stream(opened.stream_id, received);
                    if (read.status == ruvia::quic_stream_read_status::data) {
                        received_payload.insert(received_payload.end(), received.begin(),
                            received.begin() + static_cast<std::ptrdiff_t>(read.size));
                        if (!blocked_before_ack) {
                            RUVIA_CHECK(pair.client->update_key(pair.now) ==
                                        ruvia::quic_operation_status::would_block);
                            blocked_before_ack = true;
                        }
                    } else if (read.status == ruvia::quic_stream_read_status::fin) {
                        received_fin = true;
                        break;
                    } else {
                        break;
                    }
                }
                if (!received_fin) {
                    (void)pair.transfer_server_to_client();
                    pair.now += std::chrono::milliseconds(1);
                    (void)pair.server->handle_expiry(pair.now);
                    (void)pair.client->handle_expiry(pair.now);
                }
            }
            RUVIA_CHECK(received_payload.size() == payload.size());
            RUVIA_CHECK(std::equal(payload.begin(), payload.end(), received_payload.begin()));
            RUVIA_CHECK(received_fin);
            RUVIA_CHECK(blocked_before_ack);

            bool ack_delivered{};
            for (int attempt = 0; attempt < 32; ++attempt) {
                ack_delivered = pair.transfer_server_to_client() || ack_delivered;
                (void)pair.transfer_client_to_server();
                pair.now += std::chrono::milliseconds(1);
                (void)pair.server->handle_expiry(pair.now);
                (void)pair.client->handle_expiry(pair.now);
            }
            RUVIA_CHECK(ack_delivered);
            RUVIA_CHECK(pair.client->update_key(pair.now) ==
                        ruvia::quic_operation_status::would_block);
            if (generation < 2) {
                // Three initial PTOs include the default 333 ms initial RTT and 25 ms ACK delay.
                pair.now += three_initial_ptos + std::chrono::milliseconds(26);
            }
            const auto payload_use = std::find_if(
                client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(previous_use_count),
                client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count),
                [](const observed_aead_use& use) {
                    return use.seal && use.contains_expected_payload;
                });
            RUVIA_CHECK(payload_use !=
                        client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count));
            if (payload_use !=
                client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count)) {
                RUVIA_CHECK(payload_use->key_phase != previous_key_phase);
                const auto* previous_key = find_aead_capture(pair.observed,
                    *previous_write_key_id, 0, ruvia::quic_crypto_direction::write);
                const auto* current_key = find_aead_capture(pair.observed,
                    payload_use->key_id, 0, ruvia::quic_crypto_direction::write);
                RUVIA_CHECK(previous_key != nullptr);
                RUVIA_CHECK(current_key != nullptr);
                if (previous_key && current_key) {
                    const auto previous_secret = secret_for_aead_capture(pair.observed, *previous_key);
                    const auto current_secret = secret_for_aead_capture(pair.observed, *current_key);
                    RUVIA_CHECK(previous_secret.has_value());
                    RUVIA_CHECK(current_secret.has_value());
                    if (previous_secret && current_secret) {
                        const auto updated_secret = find_expand_output(pair.observed,
                            expected_suite, *previous_secret, "quic ku", hash_size);
                        RUVIA_CHECK(updated_secret.has_value());
                        if (updated_secret) {
                            RUVIA_CHECK(std::ranges::equal(*updated_secret, *current_secret));
                            RUVIA_CHECK(contains_expand(pair.observed, expected_suite,
                                *updated_secret, "quic key", current_key->size,
                                std::span<const std::byte>(current_key->bytes).first(current_key->size)));
                            RUVIA_CHECK(contains_expand(pair.observed, expected_suite,
                                *updated_secret, "quic iv", 12));
                        }
                    }
                }
                RUVIA_CHECK(std::any_of(
                    server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_use_start),
                    server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count),
                    [](const observed_aead_use& use) {
                        return !use.seal && use.authenticated && use.contains_expected_payload;
                    }));
                previous_write_key_id = payload_use->key_id;
                previous_key_phase = payload_use->key_phase;
                previous_use_count = client_crypto.use_count;
            }
            pair.observed.expect_payload(0, std::span<const std::byte>{});
            pair.observed.expect_payload(1, std::span<const std::byte>{});
            RUVIA_CHECK(pair.observed.aead_count > prior_aead_key_count);
            prior_aead_key_count = pair.observed.aead_count;
            RUVIA_CHECK(pair.observed.header_count == header_key_count);
            RUVIA_CHECK(pair.observed.aead_overflows == 0);
            RUVIA_CHECK(pair.observed.header_overflows == 0);
            RUVIA_CHECK(client_crypto.use_overflows == 0);
            RUVIA_CHECK(server_crypto.use_overflows == 0);
            RUVIA_CHECK(client_crypto.destroy_overflows == 0);
            RUVIA_CHECK(server_crypto.destroy_overflows == 0);
        }
        RUVIA_CHECK(pair.client->info().confirmed);
        RUVIA_CHECK(pair.server->info().confirmed);
        RUVIA_CHECK(pair.resource.live_bytes >= pair.crypto_live_bytes);
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
        RUVIA_CHECK(all_aead_keys_destroyed_once(pair.observed));
        RUVIA_CHECK(pair.observed.kdf_overflows == 0);
        RUVIA_CHECK(pair.observed.header_overflows == 0);
        RUVIA_CHECK(pair.observed.expected_payload_overflows == 0);
        RUVIA_CHECK(pair.observed.endpoint_events_by_id[0].use_overflows == 0);
        RUVIA_CHECK(pair.observed.endpoint_events_by_id[1].use_overflows == 0);
        RUVIA_CHECK(pair.observed.endpoint_events_by_id[0].destroy_overflows == 0);
        RUVIA_CHECK(pair.observed.endpoint_events_by_id[1].destroy_overflows == 0);
    }
}

RUVIA_TEST(openssl_quic_client_accepts_rfc9001_retry_and_rederives_initial_keys) {
    constexpr std::array<unsigned char, 36> retry_packet{
        0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08,
        0xf0, 0x67, 0xa5, 0x50, 0x2a, 0x42, 0x62, 0xb5,
        0x74, 0x6f, 0x6b, 0x65, 0x6e,
        0x04, 0xa2, 0x65, 0xba, 0x2e, 0xff, 0x4d, 0x82,
        0x90, 0x58, 0xfb, 0x3f, 0x0f, 0x24, 0x96, 0xba};
    auto retry_integrity_key_bytes = hex_bytes("be0c690b9f66575a1d766b54e368c84e");
    const auto retry_integrity_nonce_bytes = hex_bytes("461599d35d632bf2239825bb");
    const auto retry_dcid = hex_bytes("f067a5502a4262b5");
    constexpr std::array<std::byte, 5> retry_token{
        std::byte{'t'}, std::byte{'o'}, std::byte{'k'}, std::byte{'e'}, std::byte{'n'}};

    packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false,
        "localhost", true, false, true);
    const auto provider = pair.crypto.view();
    const auto first_initial = pair.client->write_packet(pair.datagram, pair.now);
    RUVIA_CHECK(first_initial.size != 0);
    auto& client_crypto = pair.observed.endpoint_events_by_id[0];
    const auto initial_use_count = client_crypto.use_count;
    const auto initial_key_count = pair.observed.aead_count;
    auto retry_integrity_key = provider.create_aead_key(
        provider.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        ruvia::quic_crypto_direction::write, retry_integrity_key_bytes);
    std::array<std::byte, 29> retry_pseudo_packet{};
    retry_pseudo_packet[0] = static_cast<std::byte>(pair.initial_destination_id.size());
    std::copy(pair.initial_destination_id.view().begin(), pair.initial_destination_id.view().end(),
        retry_pseudo_packet.begin() + 1);
    std::copy_n(std::as_bytes(std::span(retry_packet)).begin(), 20,
        retry_pseudo_packet.begin() + 9);
    std::array<std::byte, 12> retry_integrity_nonce{};
    std::copy(retry_integrity_nonce_bytes.begin(), retry_integrity_nonce_bytes.end(),
        retry_integrity_nonce.begin());
    std::array<std::byte, 16> retry_integrity_tag{};
    retry_integrity_key.seal(retry_integrity_nonce, retry_pseudo_packet,
        std::span<const std::byte>{}, retry_integrity_tag);
    provider.secure_erase(provider.context, retry_integrity_key_bytes);
    RUVIA_CHECK(std::equal(retry_integrity_tag.begin(), retry_integrity_tag.end(),
        std::as_bytes(std::span(retry_packet)).end() - 16));
    retry_integrity_key = {};

    const ruvia::quic_datagram_view retry_datagram{
        .bytes = std::as_bytes(std::span(retry_packet)),
        .local = pair.client_address,
        .peer = pair.server_address,
    };
    (void)pair.client->receive(retry_datagram, pair.now);
    pair.now += std::chrono::milliseconds(1);
    const auto retry_initial = pair.client->write_packet(pair.datagram, pair.now);
    RUVIA_CHECK(retry_initial.status == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(retry_initial.size != 0);
    const auto expected_client_key = derive_client_initial_key(provider, retry_dcid);
    const auto retry_key_capture = std::find_if(
        pair.observed.aead_keys.begin() + static_cast<std::ptrdiff_t>(initial_key_count),
        pair.observed.aead_keys.begin() + static_cast<std::ptrdiff_t>(pair.observed.aead_count),
        [&](const captured_key& key) {
            return key.endpoint == 0 && key.direction == ruvia::quic_crypto_direction::write &&
                   key.size == expected_client_key.size() &&
                   std::equal(expected_client_key.begin(), expected_client_key.end(), key.bytes.begin());
        });
    RUVIA_CHECK(retry_key_capture !=
                pair.observed.aead_keys.begin() + static_cast<std::ptrdiff_t>(pair.observed.aead_count));
    RUVIA_CHECK(initial_has_destination_and_token(
        std::span<const std::byte>(pair.datagram).first(retry_initial.size), retry_dcid, retry_token));
    if (retry_key_capture !=
        pair.observed.aead_keys.begin() + static_cast<std::ptrdiff_t>(pair.observed.aead_count)) {
        RUVIA_CHECK(std::any_of(
            client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(initial_use_count),
            client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count),
            [&](const observed_aead_use& use) {
                return use.seal && use.operation_succeeded && use.key_id == retry_key_capture->id;
            }));
    }

    packet_pair bad_tag_pair("TLS_AES_128_GCM_SHA256", "h3", true, false,
        "localhost", true, false, true);
    const auto bad_initial = bad_tag_pair.client->write_packet(bad_tag_pair.datagram, bad_tag_pair.now);
    RUVIA_CHECK(bad_initial.size != 0);
    auto bad_retry = retry_packet;
    bad_retry.back() ^= 1;
    const ruvia::quic_datagram_view bad_retry_datagram{
        .bytes = std::as_bytes(std::span(bad_retry)),
        .local = bad_tag_pair.client_address,
        .peer = bad_tag_pair.server_address,
    };
    const auto bad_retry_key_count = bad_tag_pair.observed.aead_count;
    auto& bad_client_crypto = bad_tag_pair.observed.endpoint_events_by_id[0];
    const auto bad_retry_use_count = bad_client_crypto.use_count;
    (void)bad_tag_pair.client->receive(bad_retry_datagram, bad_tag_pair.now);
    if (const auto expiry = bad_tag_pair.client->next_expiry()) {
        bad_tag_pair.now = std::max(bad_tag_pair.now, *expiry);
        (void)bad_tag_pair.client->handle_expiry(bad_tag_pair.now);
    } else {
        bad_tag_pair.now += std::chrono::milliseconds(1);
    }
    const auto rejected_retry_output = bad_tag_pair.client->write_packet(
        bad_tag_pair.datagram, bad_tag_pair.now);
    RUVIA_CHECK(rejected_retry_output.size != 0);
    RUVIA_CHECK(initial_has_destination_and_token(
        std::span<const std::byte>(bad_tag_pair.datagram).first(rejected_retry_output.size),
        bad_tag_pair.initial_destination_id.view(), std::span<const std::byte>{}));
    const auto original_client_key = derive_client_initial_key(
        bad_tag_pair.crypto.view(), bad_tag_pair.initial_destination_id.view());
    RUVIA_CHECK(contains_key(bad_tag_pair.observed.aead_keys.data(),
        bad_tag_pair.observed.aead_count, original_client_key,
        ruvia::quic_crypto_direction::write));
    RUVIA_CHECK(!std::any_of(
        bad_tag_pair.observed.aead_keys.begin() + static_cast<std::ptrdiff_t>(bad_retry_key_count),
        bad_tag_pair.observed.aead_keys.begin() + static_cast<std::ptrdiff_t>(bad_tag_pair.observed.aead_count),
        [&](const captured_key& key) {
            return key.endpoint == 0 && key.direction == ruvia::quic_crypto_direction::write &&
                   key.size == expected_client_key.size() &&
                   std::equal(expected_client_key.begin(), expected_client_key.end(), key.bytes.begin());
        }));
    RUVIA_CHECK(std::none_of(
        bad_client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(bad_retry_use_count),
        bad_client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(bad_client_crypto.use_count),
        [](const observed_aead_use& use) {
            return !use.seal && use.authenticated;
        }));
    bad_tag_pair.retire_connections();
    RUVIA_CHECK(bad_tag_pair.resources_released());
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_accepts_reordered_old_key_phase_packets) {
    packet_pair pair;
    RUVIA_CHECK(pair.drive_until_terminal());
    const auto old_stream = pair.client->open_stream(false);
    RUVIA_CHECK(old_stream.status == ruvia::quic_operation_status::accepted);
    constexpr std::array<std::byte, 9> old_payload{
        std::byte{'o'}, std::byte{'l'}, std::byte{'d'}, std::byte{'-'},
        std::byte{'p'}, std::byte{'h'}, std::byte{'a'}, std::byte{'s'}, std::byte{'e'}};
    pair.observed.expect_payload(0, old_payload);
    pair.observed.expect_payload(1, old_payload);
    const auto old_write = pair.client->write_stream(old_stream.stream_id, old_payload, true);
    RUVIA_CHECK(old_write.status == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(old_write.accepted == old_payload.size());
    auto& client_crypto = pair.observed.endpoint_events_by_id[0];
    auto& server_crypto = pair.observed.endpoint_events_by_id[1];
    const auto old_use_start = client_crypto.use_count;
    std::array<std::byte, 2048> old_packet{};
    const auto old_result = pair.client->write_packet(old_packet, pair.now);
    RUVIA_CHECK(old_result.size != 0);
    const auto old_client_use = std::find_if(
        client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(old_use_start),
        client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count),
        [](const observed_aead_use& use) {
            return use.seal && use.short_header && use.contains_expected_payload;
        });
    RUVIA_CHECK(old_client_use !=
                client_crypto.uses.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count));
    if (old_client_use == client_crypto.uses.begin() +
                              static_cast<std::ptrdiff_t>(client_crypto.use_count)) {
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
        return;
    }
    const auto old_client_key_id = old_client_use->key_id;
    const bool old_phase = old_client_use->key_phase;
    pair.observed.expect_payload(0, std::span<const std::byte>{});
    pair.observed.expect_payload(1, std::span<const std::byte>{});

    RUVIA_CHECK(pair.client->update_key(pair.now) ==
                ruvia::quic_operation_status::accepted);
    const auto new_stream = pair.client->open_stream(false);
    RUVIA_CHECK(new_stream.status == ruvia::quic_operation_status::accepted);
    constexpr std::array<std::byte, 10> new_payload{
        std::byte{'n'}, std::byte{'e'}, std::byte{'w'}, std::byte{'-'},
        std::byte{'p'}, std::byte{'h'}, std::byte{'a'}, std::byte{'s'},
        std::byte{'e'}, std::byte{'!'}};
    pair.observed.expect_payload(0, new_payload);
    pair.observed.expect_payload(1, new_payload);
    const auto new_write = pair.client->write_stream(new_stream.stream_id, new_payload, true);
    RUVIA_CHECK(new_write.status == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(new_write.accepted == new_payload.size());
    const auto new_server_use_start = server_crypto.use_count;
    std::vector<std::byte> new_received;
    std::array<std::byte, 32> read_buffer{};
    bool new_fin{};
    for (int attempt = 0; attempt < 128 && !new_fin; ++attempt) {
        (void)pair.transfer_client_to_server();
        (void)pair.server->accept_streams();
        for (;;) {
            const auto result = pair.server->read_stream(new_stream.stream_id, read_buffer);
            if (result.status == ruvia::quic_stream_read_status::data) {
                new_received.insert(new_received.end(), read_buffer.begin(),
                    read_buffer.begin() + static_cast<std::ptrdiff_t>(result.size));
            } else if (result.status == ruvia::quic_stream_read_status::fin) {
                new_fin = true;
                break;
            } else {
                break;
            }
        }
        if (!new_fin) {
            pair.now += std::chrono::milliseconds(1);
            (void)pair.server->handle_expiry(pair.now);
            (void)pair.client->handle_expiry(pair.now);
        }
    }
    RUVIA_CHECK(new_fin);
    RUVIA_CHECK(std::equal(new_payload.begin(), new_payload.end(),
        new_received.begin(), new_received.end()));
    const auto new_server_use = std::find_if(
        server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(new_server_use_start),
        server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count),
        [](const observed_aead_use& use) {
            return !use.seal && use.authenticated && use.contains_expected_payload;
        });
    RUVIA_CHECK(new_server_use !=
                server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count));
    RUVIA_CHECK(new_server_use !=
                    server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count) &&
                new_server_use->key_phase != old_phase);
    pair.observed.expect_payload(0, std::span<const std::byte>{});
    pair.observed.expect_payload(1, old_payload);

    const ruvia::quic_datagram_view delayed_input{
        .bytes = std::span<const std::byte>(old_packet).first(old_result.size),
        .local = pair.server_address,
        .peer = pair.client_address,
    };
    (void)pair.server->receive(delayed_input, pair.now);
    (void)pair.server->accept_streams();
    std::vector<std::byte> old_received;
    bool old_fin{};
    for (;;) {
        const auto result = pair.server->read_stream(old_stream.stream_id, read_buffer);
        if (result.status == ruvia::quic_stream_read_status::data) {
            old_received.insert(old_received.end(), read_buffer.begin(),
                read_buffer.begin() + static_cast<std::ptrdiff_t>(result.size));
        } else if (result.status == ruvia::quic_stream_read_status::fin) {
            old_fin = true;
            break;
        } else {
            break;
        }
    }
    RUVIA_CHECK(old_fin);
    RUVIA_CHECK(std::equal(old_payload.begin(), old_payload.end(),
        old_received.begin(), old_received.end()));
    const auto old_server_use = std::find_if(
        server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(new_server_use_start),
        server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count),
        [old_phase](const observed_aead_use& use) {
            return !use.seal && use.authenticated && use.contains_expected_payload &&
                   use.key_phase == old_phase;
        });
    RUVIA_CHECK(old_server_use !=
                server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count));
    RUVIA_CHECK(old_server_use !=
                    server_crypto.uses.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count) &&
                old_server_use->key_phase == old_phase);
    const auto* old_client_key = find_aead_capture(pair.observed,
        old_client_key_id, 0, ruvia::quic_crypto_direction::write);
    if (old_server_use != server_crypto.uses.begin() +
                              static_cast<std::ptrdiff_t>(server_crypto.use_count)) {
        const auto* old_server_key = find_aead_capture(pair.observed,
            old_server_use->key_id, 1, ruvia::quic_crypto_direction::read);
        RUVIA_CHECK(old_client_key != nullptr);
        RUVIA_CHECK(old_server_key != nullptr);
        if (old_client_key && old_server_key) {
            RUVIA_CHECK(old_client_key->size == old_server_key->size);
            RUVIA_CHECK(std::equal(old_client_key->bytes.begin(),
                old_client_key->bytes.begin() + static_cast<std::ptrdiff_t>(old_client_key->size),
                old_server_key->bytes.begin()));
        }
    }
    RUVIA_CHECK(pair.server->info().state != ruvia::quic_connection_state::failed);
    RUVIA_CHECK(!pair.server->tls_handshake().failed());
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_rejects_untrusted_certificate_and_alpn_mismatch) {
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", false);
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client->info().state == ruvia::quic_connection_state::failed ||
                    pair.client->tls_handshake().failed());
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h2", true);
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client->info().state == ruvia::quic_connection_state::failed ||
                    pair.server->info().state == ruvia::quic_connection_state::failed ||
                    pair.client->tls_handshake().failed() || pair.server->tls_handshake().failed() ||
                    pair.client_receive_crypto_failure || pair.server_receive_crypto_failure);
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "wrong.example");
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client->info().state == ruvia::quic_connection_state::failed ||
                    pair.client->tls_handshake().failed() || pair.client_receive_crypto_failure);
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", false);
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client->info().state == ruvia::quic_connection_state::failed ||
                    pair.server->info().state == ruvia::quic_connection_state::failed ||
                    pair.client->tls_handshake().failed() || pair.server->tls_handshake().failed() ||
                    pair.client_receive_crypto_failure || pair.server_receive_crypto_failure);
    }
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true, true);
    }));
}

RUVIA_TEST(openssl_quic_public_packet_pair_closes_with_a_retained_crypto_record_lease) {
    packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false,
        "localhost", true, false, false, true);
    pair.transfer_client_to_server();
    RUVIA_CHECK(pair.server_lease_driver.record.has_value());
    RUVIA_CHECK(static_cast<bool>(*pair.server_lease_driver.record));
    RUVIA_CHECK(!pair.server_lease_driver.record->bytes().empty());

    constexpr std::array<char, 8> reason{'c', 'a', 'n', 'c', 'e', 'l', 'l', 'e'};
    (void)pair.server->close({.kind = ruvia::quic_close_kind::application,
        .code = 0x100,
        .frame_type = 0,
        .reason = reason});
    pair.server_tls->stop();
    RUVIA_CHECK(static_cast<bool>(*pair.server_lease_driver.record));
    pair.client_tls->stop();
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_propagates_second_aead_factory_failure_without_leaking) {
    packet_pair pair;
    const auto now = std::chrono::steady_clock::now();
    const auto packet = pair.client->write_packet(pair.datagram, now);
    RUVIA_CHECK(packet.size != 0);
    pair.observed.throw_aead_at = pair.observed.aead_attempts + 1;
    const ruvia::quic_datagram_view input{
        .bytes = std::span<const std::byte>(pair.datagram).first(packet.size),
        .local = pair.server_address,
        .peer = pair.client_address,
    };
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)pair.server->receive(input, pair.now); }));
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_preserves_stream_flow_control_fin_and_ack_storage) {
    packet_pair pair;
    RUVIA_CHECK(pair.drive_until_terminal());
    const auto stable_bytes = pair.resource.live_bytes;
    const auto opened = pair.client->open_stream(false);
    RUVIA_CHECK(opened.status == ruvia::quic_operation_status::accepted);

    std::vector<std::byte> request(200003, std::byte{'q'});
    std::vector<std::byte> received;
    received.reserve(request.size());
    std::array<std::byte, 2048> block{};
    const auto first_write = pair.client->write_stream(opened.stream_id, request, false);
    RUVIA_CHECK(first_write.status == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(first_write.accepted != 0);
    std::size_t submitted = first_write.accepted;
    const auto pre_ack_bytes = pair.resource.live_bytes;
    RUVIA_CHECK(pre_ack_bytes > stable_bytes);
    bool peer_fin{};
    for (int attempt = 0; attempt < 4000 && !peer_fin; ++attempt) {
        if (submitted < request.size()) {
            const auto result = pair.client->write_stream(opened.stream_id,
                std::span<const std::byte>(request).subspan(submitted), true);
            if (result.status == ruvia::quic_operation_status::accepted) {
                submitted += result.accepted;
            } else {
                RUVIA_CHECK(result.status == ruvia::quic_operation_status::would_block);
            }
        }
        pair.transfer_client_to_server();
        pair.transfer_server_to_client();
        pair.now += std::chrono::milliseconds(1);
        (void)pair.server->accept_streams();
        const auto result = pair.server->read_stream(opened.stream_id, block);
        if (result.status == ruvia::quic_stream_read_status::data) {
            received.insert(received.end(), block.begin(), block.begin() + result.size);
        } else if (result.status == ruvia::quic_stream_read_status::fin) {
            peer_fin = true;
        }
    }
    RUVIA_CHECK(submitted == request.size());
    RUVIA_CHECK(peer_fin);
    RUVIA_CHECK(received == request);
    const auto held_bytes = pair.resource.live_bytes;
    RUVIA_CHECK(held_bytes >= stable_bytes);
    RUVIA_CHECK(pair.resource.allocations - pair.resource.deallocations >=
                pair.crypto_live_allocations);

    constexpr std::array<std::byte, 19> response{
        std::byte{'r'}, std::byte{'e'}, std::byte{'p'}, std::byte{'l'}, std::byte{'y'},
        std::byte{' '}, std::byte{'a'}, std::byte{'f'}, std::byte{'t'}, std::byte{'e'},
        std::byte{'r'}, std::byte{' '}, std::byte{'F'}, std::byte{'I'}, std::byte{'N'},
        std::byte{'!'}, std::byte{'!'}, std::byte{'!'}, std::byte{'!'}};
    const auto server_write = pair.server->write_stream(opened.stream_id, response, true);
    RUVIA_CHECK(server_write.status == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(server_write.accepted == response.size());
    std::vector<std::byte> client_received;
    bool client_fin{};
    for (int attempt = 0; attempt < 1000 && !client_fin; ++attempt) {
        pair.transfer_server_to_client();
        pair.transfer_client_to_server();
        pair.now += std::chrono::milliseconds(1);
        const auto result = pair.client->read_stream(opened.stream_id, block);
        if (result.status == ruvia::quic_stream_read_status::data) {
            client_received.insert(client_received.end(), block.begin(), block.begin() + result.size);
        } else if (result.status == ruvia::quic_stream_read_status::fin) {
            client_fin = true;
        }
    }
    RUVIA_CHECK(client_fin);
    RUVIA_CHECK(std::equal(client_received.begin(), client_received.end(), response.begin(), response.end()));
    (void)pair.client->retire_completed_stream(opened.stream_id);
    (void)pair.server->retire_completed_stream(opened.stream_id);
    for (int attempt = 0; attempt < 32; ++attempt) {
        pair.transfer_client_to_server();
        pair.transfer_server_to_client();
        pair.now += std::chrono::milliseconds(1);
    }
    RUVIA_CHECK(pair.resource.live_bytes < pre_ack_bytes);
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_negotiates_and_bounds_datagrams) {
    {
        packet_pair unavailable;
        RUVIA_CHECK(unavailable.drive_until_terminal());
        RUVIA_CHECK(unavailable.client->max_datagram_payload_size() == 0);
        RUVIA_CHECK(unavailable.server->max_datagram_payload_size() == 0);
        RUVIA_CHECK(unavailable.client->write_datagram({}) == ruvia::quic_datagram_write_status::unavailable);
    }
    packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, true);
    RUVIA_CHECK(pair.drive_until_terminal());
    RUVIA_CHECK(pair.client->max_datagram_payload_size() > 0);
    RUVIA_CHECK(pair.server->max_datagram_payload_size() > 0);
    const std::array<std::byte, 7> payload{
        std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'},
        std::byte{'g'}, std::byte{'r'}, std::byte{'m'}};
    RUVIA_CHECK(pair.client->write_datagram(payload) == ruvia::quic_datagram_write_status::queued);
    std::array<std::byte, 64> output{};
    ruvia::quic_datagram_result result;
    for (int attempt = 0; attempt < 1000 && result.status != ruvia::quic_datagram_status::received; ++attempt) {
        pair.transfer_client_to_server();
        pair.transfer_server_to_client();
        pair.now += std::chrono::milliseconds(1);
        result = pair.server->read_datagram(output);
    }
    RUVIA_CHECK(result.status == ruvia::quic_datagram_status::received);
    RUVIA_CHECK(result.size == payload.size());
    RUVIA_CHECK(std::equal(payload.begin(), payload.end(), output.begin()));
    std::vector<std::byte> oversized(1200, std::byte{'x'});
    RUVIA_CHECK(pair.client->write_datagram(oversized) == ruvia::quic_datagram_write_status::too_large);
    for (std::size_t index = 0; index < 16; ++index) {
        RUVIA_CHECK(pair.client->write_datagram(payload) == ruvia::quic_datagram_write_status::queued);
    }
    RUVIA_CHECK(pair.client->write_datagram(payload) == ruvia::quic_datagram_write_status::dropped);
    for (std::size_t index = 0; index < 16; ++index) {
        result = {};
        for (int attempt = 0; attempt < 1000 && result.status != ruvia::quic_datagram_status::received; ++attempt) {
            pair.transfer_client_to_server();
            pair.transfer_server_to_client();
            pair.now += std::chrono::milliseconds(1);
            result = pair.server->read_datagram(output);
        }
        RUVIA_CHECK(result.status == ruvia::quic_datagram_status::received);
        RUVIA_CHECK(result.size == payload.size());
    }
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}
