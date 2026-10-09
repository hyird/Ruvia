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

#include "http3/http3_quic_client_tls_context.h"
#include "http3/openssl_quic_crypto_provider.h"
#include "http3/openssl_quic_tls_session.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace {

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations_{};
    std::size_t deallocations_{};
    std::size_t live_bytes_{};

    void* do_allocate(std::size_t size, std::size_t alignment) override {
        void* const pointer = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++allocations_;
        live_bytes_ += size;
        return pointer;
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++deallocations_;
        live_bytes_ -= size;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct captured_key final {
    ruvia::quic_cipher_suite suite_{};
    ruvia::quic_crypto_direction direction_{};
    std::size_t id_{};
    std::size_t endpoint_{};
    std::array<std::byte, 32> bytes_{};
    std::size_t size_{};
};

struct observed_provider;

struct provider_endpoint final {
    observed_provider* owner_{};
    std::size_t id_{};
};

struct observed_aead_use final {
    std::size_t key_id_{};
    ruvia::quic_crypto_direction direction_{};
    bool seal_{};
    bool short_header_{};
    bool key_phase_{};
    bool operation_succeeded_{};
    bool authenticated_{};
    bool contains_expected_payload_{};
};

struct observed_aead_state final {
    observed_provider* owner_{};
    std::pmr::memory_resource* resource_{};
    ruvia::quic_aead_key inner_;
    std::size_t endpoint_{};
    std::size_t key_id_{};
    ruvia::quic_crypto_direction direction_{};
};

struct captured_bytes final {
    std::array<std::byte, 64> bytes_{};
    std::size_t size_{};

    std::span<const std::byte> view() const noexcept {
        return std::span<const std::byte>(bytes_).first(size_);
    }

    bool assign(std::span<const std::byte> value) noexcept {
        if (value.size() > bytes_.size()) {
            return false;
        }
        std::copy(value.begin(), value.end(), bytes_.begin());
        size_ = value.size();
        return true;
    }
};

struct captured_kdf_event final {
    bool extract_{};
    std::size_t endpoint_{};
    ruvia::quic_cipher_suite suite_{};
    captured_bytes salt_;
    captured_bytes input_;
    captured_bytes secret_;
    captured_bytes info_;
    captured_bytes output_;
    bool valid_{true};
};

struct observed_provider final {
    struct endpoint_events final {
        std::array<observed_aead_use, 1024> uses_{};
        std::size_t use_count_{};
        std::size_t use_overflows_{};
        std::array<std::size_t, 64> destroyed_keys_{};
        std::size_t destroyed_count_{};
        std::size_t destroy_overflows_{};
    };

    observed_provider(ruvia::quic_crypto_provider_view provider,
        std::pmr::memory_resource* memory_resource)
        : inner_(provider),
          resource_(memory_resource),
          endpoints_{{{this, 0}, {this, 1}}} {}

    ~observed_provider() noexcept {
        const auto erase = [this](auto& captures) {
            for (auto& capture : captures) {
                inner_.secure_erase_(inner_.context_, capture.bytes_);
            }
        };
        erase(aead_keys_);
        erase(header_keys_);
        for (auto& expected : expected_payloads_) {
            inner_.secure_erase_(inner_.context_, expected.bytes_);
        }
        for (auto& event : kdf_events_) {
            inner_.secure_erase_(inner_.context_, event.salt_.bytes_);
            inner_.secure_erase_(inner_.context_, event.input_.bytes_);
            inner_.secure_erase_(inner_.context_, event.secret_.bytes_);
            inner_.secure_erase_(inner_.context_, event.info_.bytes_);
            inner_.secure_erase_(inner_.context_, event.output_.bytes_);
        }
    }

    ruvia::quic_crypto_provider_view inner_;
    std::pmr::memory_resource* resource_{};
    std::array<provider_endpoint, 2> endpoints_{};
    std::array<endpoint_events, 2> endpoint_events_by_id_{};
    std::array<std::size_t, 2> next_key_id_{};
    std::array<captured_bytes, 2> expected_payloads_{};
    std::size_t expected_payload_overflows_{};
    std::array<captured_kdf_event, 256> kdf_events_{};
    std::size_t kdf_count_{};
    std::size_t kdf_overflows_{};
    std::array<captured_key, 64> aead_keys_{};
    std::array<captured_key, 64> header_keys_{};
    std::size_t aead_count_{};
    std::size_t aead_overflows_{};
    std::size_t aead_attempts_{};
    std::size_t throw_aead_at_{std::numeric_limits<std::size_t>::max()};
    std::size_t header_count_{};
    std::size_t header_overflows_{};

    void expect_payload(std::size_t endpoint, std::span<const std::byte> payload_value) noexcept {
        const bool stored = expected_payloads_[endpoint].assign(payload_value);
        if (!stored) {
            ++expected_payload_overflows_;
        }
    }

    observed_aead_use make_use(const observed_aead_state& key, bool seal,
        bool operation_succeeded, bool authenticated,
        std::span<const std::byte> associated_data, std::span<const std::byte> plaintext) const noexcept {
        const bool short_header = !associated_data.empty() &&
                                  (std::to_integer<unsigned char>(associated_data.front()) & 0xc0) == 0x40;
        const bool key_phase = short_header &&
                               (std::to_integer<unsigned char>(associated_data.front()) & 0x04) != 0;
        const auto expected = expected_payloads_[key.endpoint_].view();
        const bool plaintext_is_trusted = seal ? operation_succeeded : authenticated;
        const bool contains_payload = plaintext_is_trusted && !expected.empty() &&
                                      plaintext.size() >= expected.size() &&
                                      std::search(plaintext.begin(), plaintext.end(), expected.begin(), expected.end()) != plaintext.end();
        return {key.key_id_, key.direction_, seal, short_header, key_phase,
            operation_succeeded, authenticated, contains_payload};
    }

    void record_use(const observed_aead_state& key, observed_aead_use use) noexcept {
        auto& endpoint = endpoint_events_by_id_[key.endpoint_];
        if (endpoint.use_count_ == endpoint.uses_.size()) {
            ++endpoint.use_overflows_;
            return;
        }
        endpoint.uses_[endpoint.use_count_++] = use;
    }

    static void destroy_aead(void* opaque) noexcept {
        auto* const key = static_cast<observed_aead_state*>(opaque);
        auto& events_value = key->owner_->endpoint_events_by_id_[key->endpoint_];
        if (events_value.destroyed_count_ < events_value.destroyed_keys_.size()) {
            events_value.destroyed_keys_[events_value.destroyed_count_++] = key->key_id_;
        } else {
            ++events_value.destroy_overflows_;
        }
        auto* const memory_resource = key->resource_;
        std::destroy_at(key);
        std::pmr::polymorphic_allocator<observed_aead_state> allocator(memory_resource);
        allocator.deallocate(key, 1);
    }

    static void seal_aead(void* opaque, std::span<const std::byte, 12> nonce,
        std::span<const std::byte> associated_data, std::span<const std::byte> plaintext,
        std::span<std::byte> ciphertext_and_tag) {
        auto& key = *static_cast<observed_aead_state*>(opaque);
        const auto use = key.owner_->make_use(key, true, true, false, associated_data, plaintext);
        key.inner_.seal(nonce, associated_data, plaintext, ciphertext_and_tag);
        key.owner_->record_use(key, use);
    }

    static ruvia::quic_aead_key_operations::open_result open_aead(void* opaque,
        std::span<const std::byte, 12> nonce, std::span<const std::byte> associated_data,
        std::span<const std::byte> ciphertext_and_tag, std::span<std::byte> plaintext) {
        auto& key = *static_cast<observed_aead_state*>(opaque);
        const auto result_value = key.inner_.open(nonce, associated_data, ciphertext_and_tag, plaintext);
        const bool authenticated =
            result_value.value_ == ruvia::quic_aead_key_operations::open_result::status::authenticated;
        key.owner_->record_use(key, key.owner_->make_use(
                                        key, false, true, authenticated, associated_data, plaintext));
        return result_value;
    }

    ruvia::quic_crypto_provider_view view(std::size_t endpoint_id = 0) noexcept {
        return {
            .context_ = &endpoints_[endpoint_id],
            .random_bytes_ = [](void* opaque, std::span<std::byte> output) {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner_;
                self.inner_.random_bytes_(self.inner_.context_, output); },
            .hkdf_extract_ = [](void* opaque, ruvia::quic_cipher_suite suite,
                                 std::span<const std::byte> salt, std::span<const std::byte> input,
                                 std::span<std::byte> output) {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner_;
                self.inner_.hkdf_extract_(self.inner_.context_, suite, salt, input, output);
                if (self.kdf_count_ == self.kdf_events_.size()) {
                    ++self.kdf_overflows_;
                    return;
                }
                auto& event = self.kdf_events_[self.kdf_count_++];
                event.extract_ = true;
                event.endpoint_ = static_cast<provider_endpoint*>(opaque)->id_;
                event.suite_ = suite;
                event.valid_ = event.salt_.assign(salt) && event.input_.assign(input) &&
                              event.output_.assign(output); },
            .hkdf_expand_ = [](void* opaque, ruvia::quic_cipher_suite suite,
                                std::span<const std::byte> secret, std::span<const std::byte> info,
                                std::span<std::byte> output) {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner_;
                self.inner_.hkdf_expand_(self.inner_.context_, suite, secret, info, output);
                if (self.kdf_count_ == self.kdf_events_.size()) {
                    ++self.kdf_overflows_;
                    return;
                }
                auto& event = self.kdf_events_[self.kdf_count_++];
                event.endpoint_ = static_cast<provider_endpoint*>(opaque)->id_;
                event.suite_ = suite;
                event.valid_ = event.secret_.assign(secret) && event.info_.assign(info) &&
                              event.output_.assign(output); },
            .create_aead_key_ = [](void* opaque, ruvia::quic_cipher_suite suite,
                                    ruvia::quic_crypto_direction direction,
                                    std::span<const std::byte> key_bytes) {
                auto& endpoint = *static_cast<provider_endpoint*>(opaque);
                auto& self = *endpoint.owner_;
                if (self.aead_attempts_++ == self.throw_aead_at_) {
                    throw std::runtime_error("injected QUIC AEAD key factory failure");
                }
                const auto key_id = self.next_key_id_[endpoint.id_]++;
                if (self.aead_count_ < self.aead_keys_.size() && key_bytes.size() <= self.aead_keys_[0].bytes_.size()) {
                    auto& capture_value = self.aead_keys_[self.aead_count_++];
                    capture_value.suite_ = suite;
                    capture_value.direction_ = direction;
                    capture_value.id_ = key_id;
                    capture_value.endpoint_ = endpoint.id_;
                    capture_value.size_ = key_bytes.size();
                    std::copy(key_bytes.begin(), key_bytes.end(), capture_value.bytes_.begin());
                } else {
                    ++self.aead_overflows_;
                }
                auto inner = self.inner_.create_aead_key_(self.inner_.context_, suite, direction, key_bytes);
                std::pmr::polymorphic_allocator<observed_aead_state> allocator(self.resource_);
                auto* const state_value = allocator.allocate(1);
                try {
                    std::construct_at(state_value, observed_aead_state{&self, self.resource_,
                        std::move(inner), endpoint.id_, key_id, direction});
                } catch (...) {
                    allocator.deallocate(state_value, 1);
                    throw;
                }
                return ruvia::quic_aead_key::adopt(state_value, {
                    .destroy_ = destroy_aead,
                    .seal_ = seal_aead,
                    .open_ = open_aead,
                }); },
            .create_header_protection_key_ = [](void* opaque, ruvia::quic_cipher_suite suite,
                                                 std::span<const std::byte> key_bytes) {
                auto& endpoint = *static_cast<provider_endpoint*>(opaque);
                auto& self = *endpoint.owner_;
                if (self.header_count_ < self.header_keys_.size() && key_bytes.size() <= self.header_keys_[0].bytes_.size()) {
                    auto& capture_value = self.header_keys_[self.header_count_++];
                    capture_value.suite_ = suite;
                    capture_value.id_ = self.header_count_ - 1;
                    capture_value.endpoint_ = endpoint.id_;
                    capture_value.size_ = key_bytes.size();
                    std::copy(key_bytes.begin(), key_bytes.end(), capture_value.bytes_.begin());
                } else {
                    ++self.header_overflows_;
                }
                return self.inner_.create_header_protection_key_(self.inner_.context_, suite, key_bytes); },
            .secure_erase_ = [](void* opaque, std::span<std::byte> bytes_value) noexcept {
                auto& self = *static_cast<provider_endpoint*>(opaque)->owner_;
                self.inner_.secure_erase_(self.inner_.context_, bytes_value); },
        };
    }
};

bool all_aead_keys_destroyed_once(const observed_provider& provider) {
    if (provider.aead_overflows_ != 0) {
        return false;
    }
    for (std::size_t index = 0; index < provider.aead_count_; ++index) {
        const auto& key = provider.aead_keys_[index];
        const auto& endpoint = provider.endpoint_events_by_id_[key.endpoint_];
        const auto destroys = std::count(endpoint.destroyed_keys_.begin(),
            endpoint.destroyed_keys_.begin() + static_cast<std::ptrdiff_t>(endpoint.destroyed_count_),
            key.id_);
        if (destroys != 1) {
            return false;
        }
    }
    return true;
}

bool contains_key(const captured_key* keys, std::size_t count,
    std::span<const std::byte> expected, std::optional<ruvia::quic_crypto_direction> direction = {}) {
    for (std::size_t index = 0; index < count; ++index) {
        const auto& candidate_value = keys[index];
        if (candidate_value.size_ == expected.size() &&
            (!direction || candidate_value.direction_ == *direction) &&
            std::equal(expected.begin(), expected.end(), candidate_value.bytes_.begin())) {
            return true;
        }
    }
    return false;
}

std::vector<std::byte> tls13_label(std::string_view label, std::size_t size);

bool contains_extract(const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::span<const std::byte> salt, std::span<const std::byte> input,
    std::span<const std::byte> output) {
    for (std::size_t index = 0; index < provider.kdf_count_; ++index) {
        const auto& event = provider.kdf_events_[index];
        if (event.valid_ && event.extract_ && event.suite_ == suite &&
            std::ranges::equal(event.salt_.view(), salt) &&
            std::ranges::equal(event.input_.view(), input) &&
            std::ranges::equal(event.output_.view(), output)) {
            return true;
        }
    }
    return false;
}

bool contains_expand(const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::span<const std::byte> secret, std::string_view label, std::size_t output_size,
    std::span<const std::byte> expected_output = {}) {
    const auto expected_info = tls13_label(label, output_size);
    for (std::size_t index = 0; index < provider.kdf_count_; ++index) {
        const auto& event = provider.kdf_events_[index];
        if (!event.valid_ || event.extract_ || event.suite_ != suite ||
            event.secret_.size_ != secret.size() || event.output_.size_ != output_size ||
            !std::ranges::equal(event.secret_.view(), secret) ||
            !std::ranges::equal(event.info_.view(), expected_info)) {
            continue;
        }
        if (expected_output.empty() || std::ranges::equal(event.output_.view(), expected_output)) {
            return true;
        }
    }
    return false;
}

std::optional<std::span<const std::byte>> find_expand_output(
    const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::span<const std::byte> secret, std::string_view label, std::size_t output_size) {
    const auto expected_info = tls13_label(label, output_size);
    for (std::size_t index = 0; index < provider.kdf_count_; ++index) {
        const auto& event = provider.kdf_events_[index];
        if (event.valid_ && !event.extract_ && event.suite_ == suite &&
            event.secret_.size_ == secret.size() && event.output_.size_ == output_size &&
            std::ranges::equal(event.secret_.view(), secret) &&
            std::ranges::equal(event.info_.view(), expected_info)) {
            return event.output_.view();
        }
    }
    return std::nullopt;
}

const captured_key* find_aead_capture(const observed_provider& provider, std::size_t id,
    std::size_t endpoint, ruvia::quic_crypto_direction direction) {
    for (std::size_t index = 0; index < provider.aead_count_; ++index) {
        const auto& key = provider.aead_keys_[index];
        if (key.id_ == id && key.endpoint_ == endpoint && key.direction_ == direction) {
            return &key;
        }
    }
    return nullptr;
}

std::optional<std::span<const std::byte>> secret_for_aead_capture(
    const observed_provider& provider, const captured_key& key) {
    for (std::size_t index = 0; index < provider.kdf_count_; ++index) {
        const auto& event = provider.kdf_events_[index];
        if (event.valid_ && !event.extract_ && event.suite_ == key.suite_ &&
            std::ranges::equal(event.info_.view(), tls13_label("quic key", key.size_)) &&
            event.output_.size_ == key.size_ &&
            std::ranges::equal(event.output_.view(),
                std::span<const std::byte>(key.bytes_).first(key.size_))) {
            return event.secret_.view();
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
    for (std::size_t index = 0; index < provider.kdf_count_; ++index) {
        const auto& event = provider.kdf_events_[index];
        if (event.valid_ && !event.extract_ && event.suite_ == suite &&
            event.secret_.size_ == secret_size && event.output_.size_ == output_size &&
            !is_secret(event.secret_, initial_client_secret, initial_server_secret) &&
            std::ranges::equal(event.info_.view(), expected_info)) {
            return true;
        }
    }
    return false;
}

bool contains_expand_key_capture(const observed_provider& provider, ruvia::quic_cipher_suite suite,
    std::string_view label, std::size_t secret_size, const captured_key* keys, std::size_t key_count,
    std::span<const std::byte> initial_client_secret,
    std::span<const std::byte> initial_server_secret) {
    for (std::size_t index = 0; index < provider.kdf_count_; ++index) {
        const auto& event = provider.kdf_events_[index];
        if (!event.valid_ || event.extract_ || event.suite_ != suite ||
            event.secret_.size_ != secret_size ||
            is_secret(event.secret_, initial_client_secret, initial_server_secret) ||
            !std::ranges::equal(event.info_.view(), tls13_label(label, event.output_.size_))) {
            continue;
        }
        if (contains_key(keys, key_count, event.output_.view())) {
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
    crypto.hkdf_extract_(crypto.context_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        salt, destination_connection_id, initial_secret);
    const auto client_label = tls13_label("client in", client_secret.size());
    crypto.hkdf_expand_(crypto.context_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        initial_secret, client_label, client_secret);
    const auto key_label = tls13_label("quic key", key.size());
    crypto.hkdf_expand_(crypto.context_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        client_secret, key_label, key);
    return key;
}

using ssl_ctx_owner = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using ssl_owner = std::unique_ptr<SSL, decltype(&SSL_free)>;
using key_owner = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using certificate_owner = std::unique_ptr<X509, decltype(&X509_free)>;

struct retaining_tls_driver final {
    std::optional<ruvia::quic_crypto_record_lease> record_;

    static ruvia::quic_tls_drive_result drive(void* context_value,
        ruvia::quic_tls_handshake& handshake) noexcept {
        auto& self = *static_cast<retaining_tls_driver*>(context_value);
        try {
            if (!self.record_) {
                auto incoming = handshake.take_crypto_record();
                if (incoming) {
                    self.record_.emplace(std::move(incoming));
                }
            }
            return {ruvia::quic_tls_progress::need_input, ruvia::quic_tls_alert::internal_error};
        } catch (...) {
            handshake.fail(ruvia::quic_tls_alert::internal_error);
            return {ruvia::quic_tls_progress::failed, ruvia::quic_tls_alert::internal_error};
        }
    }

    static void retire(void* context_value) noexcept {
        static_cast<retaining_tls_driver*>(context_value)->release();
    }

    ruvia::quic_tls_driver_view view() noexcept {
        return {.context_ = this, .drive_ = drive, .retire_ = retire};
    }

    void release() noexcept {
        record_.reset();
    }
};

certificate_owner make_certificate(EVP_PKEY* key) {
    certificate_owner certificate(X509_new_ex(nullptr, nullptr), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) == nullptr ||
        X509_set_pubkey(certificate.get(), key) != 1) {
        throw std::runtime_error("failed to construct QUIC test certificate");
    }
    const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    constexpr char common_name[] = "localhost";
    if (!subject || X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
        X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
        X509_set_issuer_name(certificate.get(), subject.get()) != 1) {
        throw std::runtime_error("failed to set QUIC test certificate subject");
    }
    X509V3_CTX extensions;
    X509V3_set_ctx(&extensions, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
        X509V3_EXT_nconf_nid(nullptr, &extensions, NID_subject_alt_name,
            const_cast<char*>("DNS:localhost")),
        X509_EXTENSION_free);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> constraints(
        X509V3_EXT_nconf_nid(nullptr, &extensions, NID_basic_constraints,
            const_cast<char*>("critical,CA:TRUE")),
        X509_EXTENSION_free);
    if (!san || !constraints || X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
        X509_add_ext(certificate.get(), constraints.get(), -1) != 1 ||
        ruvia::test::sign_tls_certificate(certificate.get(), key) <= 0) {
        throw std::runtime_error("failed to sign QUIC test certificate");
    }
    return certificate;
}

int allow_early_data(SSL*, void* argument) noexcept {
    return *static_cast<const bool*>(argument) ? 1 : 0;
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
    result.bytes_[0] = std::byte{127};
    result.bytes_[3] = std::byte{1};
    result.port_ = port;
    result.family_ = ruvia::quic_address_family::ipv4;
    return result;
}

ruvia::quic_connection_id connection_id(std::array<unsigned char, 8> bytes_value) {
    return ruvia::quic_connection_id(std::as_bytes(std::span(bytes_value)));
}

bool read_quic_varint(std::span<const std::byte> bytes_value, std::size_t& offset,
    std::uint64_t& value) noexcept {
    if (offset >= bytes_value.size()) {
        return false;
    }
    const auto first = std::to_integer<std::uint8_t>(bytes_value[offset++]);
    const auto width = std::size_t{1} << (first >> 6);
    if (width > bytes_value.size() - offset + 1) {
        return false;
    }
    value = first & 0x3f;
    for (std::size_t index = 1; index < width; ++index) {
        value = (value << 8) | std::to_integer<std::uint8_t>(bytes_value[offset++]);
    }
    return true;
}

bool has_version_information(std::span<const std::byte> parameters,
    std::uint32_t chosen_version) noexcept {
    std::size_t offset{};
    while (offset < parameters.size()) {
        std::uint64_t identifier{};
        std::uint64_t length{};
        if (!read_quic_varint(parameters, offset, identifier) ||
            !read_quic_varint(parameters, offset, length) || length > parameters.size() - offset) {
            return false;
        }
        const auto value = parameters.subspan(offset, static_cast<std::size_t>(length));
        if (identifier == 0x11) {
            if (value.size() < 4 || (value.size() - 4) % 4 != 0) {
                return false;
            }
            const auto encoded = (std::uint32_t(std::to_integer<std::uint8_t>(value[0])) << 24) |
                                 (std::uint32_t(std::to_integer<std::uint8_t>(value[1])) << 16) |
                                 (std::uint32_t(std::to_integer<std::uint8_t>(value[2])) << 8) |
                                 std::uint32_t(std::to_integer<std::uint8_t>(value[3]));
            return encoded == chosen_version;
        }
        offset += static_cast<std::size_t>(length);
    }
    return false;
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
    counting_resource resource_;
    ruvia::detail::openssl_quic_crypto_provider crypto_{&resource_};
    observed_provider observed_{crypto_.view(), &resource_};
    ssl_ctx_owner client_context_{SSL_CTX_new(TLS_method()), SSL_CTX_free};
    ssl_ctx_owner server_context_{SSL_CTX_new(TLS_method()), SSL_CTX_free};
    std::optional<ruvia::detail::openssl_quic_tls_session> client_tls_;
    std::optional<ruvia::detail::openssl_quic_tls_session> server_tls_;
    retaining_tls_driver server_lease_driver_;
    std::optional<ruvia::quic_connection> client_;
    std::optional<ruvia::quic_connection> server_;
    ruvia::quic_address client_address_{address(43001)};
    ruvia::quic_address server_address_{address(4433)};
    std::array<std::byte, 2048> datagram_{};
    ruvia::quic_connection_id initial_destination_id_{connection_id({0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08})};
    ruvia::quic_connection_id client_source_id_{connection_id({1, 2, 3, 4, 5, 6, 7, 8})};
    ruvia::quic_connection_id server_source_id_{connection_id({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88})};
    std::string_view server_alpn_{"h3"};
    bool server_accept_early_data_{};
    std::size_t crypto_live_bytes_{};
    std::size_t crypto_live_allocations_{};
    bool datagrams_enabled_{};
    bool client_receive_crypto_failure_{};
    bool server_receive_crypto_failure_{};
    ruvia::quic_timestamp now_{};

    explicit packet_pair(std::string_view cipher = "TLS_AES_128_GCM_SHA256",
        std::string_view server_alpn = "h3", bool trust_server = true,
        bool enable_datagrams = false, std::string_view client_host = "localhost",
        bool offer_alpn = true, bool invalid_local_parameters = false,
        bool empty_client_source_id = false, bool retain_server_record = false,
        bool advertise_server_datagrams = true,
        ruvia::quic_version version = ruvia::quic_version::v1,
        ruvia::quic_version server_preferred_version = ruvia::quic_version::v1,
        bool allow_active_migration = false, SSL_CTX* reused_client_context = nullptr,
        SSL_CTX* reused_server_context = nullptr, SSL_SESSION* resumption_session = nullptr,
        std::span<const std::byte> early_transport_parameters = {},
        bool enable_early_data = false, bool advertise_early_data = false,
        bool enable_server_early_data = false)
        : datagrams_enabled_(enable_datagrams) {
        crypto_live_bytes_ = resource_.live_bytes_;
        crypto_live_allocations_ = resource_.allocations_ - resource_.deallocations_;
        server_alpn_ = server_alpn;
        if (reused_client_context) {
            if (SSL_CTX_up_ref(reused_client_context) != 1) {
                throw std::runtime_error("failed to retain client QUIC TLS context");
            }
            client_context_.reset(reused_client_context);
        }
        if (reused_server_context) {
            if (SSL_CTX_up_ref(reused_server_context) != 1) {
                throw std::runtime_error("failed to retain server QUIC TLS context");
            }
            server_context_.reset(reused_server_context);
        }
        if (!client_context_ || !server_context_) {
            throw std::runtime_error("failed to create QUIC test TLS contexts");
        }
        if (SSL_CTX_set_min_proto_version(client_context_.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(client_context_.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_min_proto_version(server_context_.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(server_context_.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_ciphersuites(client_context_.get(), std::string(cipher).c_str()) != 1 ||
            SSL_CTX_set_ciphersuites(server_context_.get(), std::string(cipher).c_str()) != 1) {
            throw std::runtime_error("failed to configure QUIC test TLS contexts");
        }
        SSL_CTX_set_verify(client_context_.get(), SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_verify(server_context_.get(), SSL_VERIFY_NONE, nullptr);

        key_owner key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free);
        if (!key) {
            throw std::runtime_error("failed to generate QUIC test certificate key");
        }
        auto certificate = make_certificate(key.get());
        if (!reused_server_context &&
            (SSL_CTX_use_certificate(server_context_.get(), certificate.get()) != 1 ||
                SSL_CTX_use_PrivateKey(server_context_.get(), key.get()) != 1 ||
                SSL_CTX_check_private_key(server_context_.get()) != 1)) {
            throw std::runtime_error("failed to install QUIC test certificate");
        }
        if (!reused_client_context && trust_server &&
            X509_STORE_add_cert(SSL_CTX_get_cert_store(client_context_.get()), certificate.get()) != 1) {
            throw std::runtime_error("failed to trust QUIC test certificate");
        }
        if (!reused_server_context) {
            SSL_CTX_set_num_tickets(server_context_.get(), 2);
            SSL_CTX_set_alpn_select_cb(server_context_.get(), select_h3_alpn, &server_alpn_);
            SSL_CTX_set_allow_early_data_cb(server_context_.get(), allow_early_data,
                &server_accept_early_data_);
        }
        if (advertise_early_data &&
            (SSL_CTX_set_max_early_data(server_context_.get(), 16384) != 1 ||
                SSL_CTX_set_recv_max_early_data(server_context_.get(), 16384) != 1)) {
            throw std::runtime_error("failed to configure test server early-data allowance");
        }

        constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
        client_tls_.emplace(client_context_.get(), ruvia::quic_role::client,
            offer_alpn ? std::span<const unsigned char>(alpn) : std::span<const unsigned char>{},
            client_host, &resource_, resumption_session, enable_early_data);
        server_tls_.emplace(server_context_.get(), ruvia::quic_role::server,
            std::span<const unsigned char>{}, std::string_view{}, &resource_, nullptr,
            enable_server_early_data);

        ruvia::quic_connection_config client_config;
        client_config.role_ = ruvia::quic_role::client;
        client_config.version_ = version;
        client_config.preferred_version_ = version;
        client_config.local_address_ = client_address_;
        client_config.peer_address_ = server_address_;
        client_config.destination_connection_id_ = initial_destination_id_;
        client_config.source_connection_id_ = empty_client_source_id
                                                  ? ruvia::quic_connection_id{}
                                                  : client_source_id_;
        if (datagrams_enabled_) {
            client_config.local_transport_parameters_.max_datagram_frame_size_ = 1200;
        }
        client_.emplace(client_config, observed_.view(0), client_tls_->driver_view(),
            &resource_, now_, early_transport_parameters);

        ruvia::quic_connection_config server_config;
        server_config.role_ = ruvia::quic_role::server;
        server_config.version_ = version;
        server_config.preferred_version_ = server_preferred_version;
        server_config.local_address_ = server_address_;
        server_config.peer_address_ = client_address_;
        server_config.destination_connection_id_ = client_config.source_connection_id_.value();
        server_config.source_connection_id_ = server_source_id_;
        server_config.original_destination_connection_id_ = initial_destination_id_;
        server_config.local_transport_parameters_.disable_active_migration_ = !allow_active_migration;
        if (datagrams_enabled_ && advertise_server_datagrams) {
            server_config.local_transport_parameters_.max_datagram_frame_size_ = 1200;
        }
        if (invalid_local_parameters) {
            server_config.local_transport_parameters_.max_udp_payload_size_ = 1199;
        }
        server_.emplace(server_config, observed_.view(1),
            retain_server_record ? server_lease_driver_.view() : server_tls_->driver_view(),
            &resource_, now_);
    }

    packet_pair(const packet_pair&) = delete;
    packet_pair& operator=(const packet_pair&) = delete;

    ~packet_pair() {
        retire_connections();
    }

    void retire_connections() noexcept {
        if (client_tls_) {
            client_tls_->stop();
        }
        if (server_tls_) {
            server_tls_->stop();
        }
        client_.reset();
        server_.reset();
        client_tls_.reset();
        server_tls_.reset();
    }

    bool transfer_client_to_server() {
        ruvia::quic_packet_result packet;
        try {
            packet = client_->write_packet(datagram_, now_);
        } catch (const ruvia::quic_error&) {
            client_receive_crypto_failure_ = true;
            return false;
        }
        if (packet.size_ != 0) {
            const ruvia::quic_datagram_view input{
                .bytes_ = std::span<const std::byte>(datagram_).first(packet.size_),
                .local_ = packet.peer_,
                .peer_ = packet.local_,
            };
            try {
                (void)server_->receive(input, now_);
            } catch (const ruvia::quic_error&) {
                server_receive_crypto_failure_ = true;
            }
            return true;
        }
        return false;
    }

    bool transfer_server_to_client() {
        ruvia::quic_packet_result packet;
        try {
            packet = server_->write_packet(datagram_, now_);
        } catch (const ruvia::quic_error&) {
            server_receive_crypto_failure_ = true;
            return false;
        }
        if (packet.size_ != 0) {
            const ruvia::quic_datagram_view input{
                .bytes_ = std::span<const std::byte>(datagram_).first(packet.size_),
                .local_ = packet.peer_,
                .peer_ = packet.local_,
            };
            try {
                (void)client_->receive(input, now_);
            } catch (const ruvia::quic_error&) {
                client_receive_crypto_failure_ = true;
            }
            return true;
        }
        return false;
    }

    bool resources_released() const noexcept {
        return resource_.live_bytes_ == crypto_live_bytes_ &&
               resource_.allocations_ - resource_.deallocations_ == crypto_live_allocations_;
    }

    bool terminal() noexcept {
        const auto client_info = client_->info();
        const auto server_info = server_->info();
        return (client_info.confirmed_ && server_info.confirmed_) ||
               client_info.state_ == ruvia::quic_connection_state::failed ||
               server_info.state_ == ruvia::quic_connection_state::failed ||
               client_->tls_handshake().failed() || server_->tls_handshake().failed() ||
               client_receive_crypto_failure_ || server_receive_crypto_failure_;
    }

    bool drive_until_terminal() {
        for (std::size_t attempt_value = 0; attempt_value < 4000 && !terminal(); ++attempt_value) {
            transfer_client_to_server();
            transfer_server_to_client();
            now_ += std::chrono::milliseconds(1);
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
        ruvia::detail::openssl_quic_tls_session session_value(
            tls_context.get(), ruvia::quic_role::client, alpn, {}, &resource);
        ruvia::quic_connection_config config;
        config.role_ = ruvia::quic_role::client;
        config.local_address_ = address(43001);
        config.peer_address_ = address(4433);
        ruvia::quic_connection connection(config, crypto_view, session_value.driver_view(), &resource,
            std::chrono::steady_clock::now());
        std::array<std::byte, 1500> initial_packet{};
        const auto result_value = connection.write_packet(initial_packet, std::chrono::steady_clock::now());
        RUVIA_CHECK(result_value.size_ != 0);
        RUVIA_CHECK(!connection.tls_handshake().failed());
        session_value.stop();
    }
    RUVIA_CHECK(resource.allocations_ == resource.deallocations_);
}

RUVIA_TEST(openssl_quic_tls_session_cold_unstarted_owner_can_be_discarded) {
    counting_resource resource;
    {
        ruvia::detail::openssl_quic_crypto_provider crypto(&resource);
        ssl_ctx_owner tls_context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
        RUVIA_CHECK(tls_context != nullptr);
        constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
        ruvia::detail::openssl_quic_tls_session session_value(
            tls_context.get(), ruvia::quic_role::client, alpn, "localhost", &resource);
    }
    RUVIA_CHECK(resource.allocations_ == resource.deallocations_);
    RUVIA_CHECK(resource.live_bytes_ == 0);
}

RUVIA_TEST(openssl_quic_tls_session_rejects_early_data_and_latches_callback_failure) {
    ssl_ctx_owner tls_context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    RUVIA_CHECK(tls_context != nullptr);
    SSL_CTX_set_verify(tls_context.get(), SSL_VERIFY_NONE, nullptr);
    constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
    ruvia::detail::openssl_quic_tls_session session_value(
        tls_context.get(), ruvia::quic_role::client, alpn);
    ruvia::detail::openssl_quic_crypto_provider crypto(std::pmr::get_default_resource());
    ruvia::quic_connection_config config;
    config.role_ = ruvia::quic_role::client;
    config.local_address_ = address(43001);
    config.peer_address_ = address(4433);
    ruvia::quic_connection connection(config, crypto.view(), session_value.driver_view(),
        std::pmr::get_default_resource(), std::chrono::steady_clock::now());

    constexpr std::array<unsigned char, 32> secret{};
    RUVIA_CHECK(ruvia::detail::openssl_quic_tls_session::yield_secret(nullptr,
                    OSSL_RECORD_PROTECTION_LEVEL_EARLY, 0, secret.data(), secret.size(), &session_value) == 0);
    const auto first = session_value.drive(connection.tls_handshake());
    const auto second = session_value.drive(connection.tls_handshake());
    RUVIA_CHECK(first.progress_ == ruvia::quic_tls_progress::failed);
    RUVIA_CHECK(first.alert_ == ruvia::quic_tls_alert::internal_error);
    RUVIA_CHECK(second.progress_ == ruvia::quic_tls_progress::failed);
    RUVIA_CHECK(second.alert_ == first.alert_);
    session_value.stop();
}

RUVIA_TEST(openssl_quic_public_packet_pair_completes_all_tls13_suites_and_retains_metadata) {
    constexpr std::array suites{
        std::pair{"TLS_AES_128_GCM_SHA256", ruvia::quic_cipher_suite::aes_128_gcm_sha256},
        std::pair{"TLS_AES_256_GCM_SHA384", ruvia::quic_cipher_suite::aes_256_gcm_sha384},
        std::pair{"TLS_CHACHA20_POLY1305_SHA256", ruvia::quic_cipher_suite::chacha20_poly1305_sha256},
    };
    for (const auto& [name, expected_suite] : suites) {
        packet_pair pair(name);
        RUVIA_CHECK(pair.client_->update_key(pair.now_) ==
                    ruvia::quic_operation_status::would_block);
        RUVIA_CHECK(pair.drive_until_terminal());
        const auto client_info = pair.client_->info();
        const auto server_info = pair.server_->info();
        RUVIA_CHECK(client_info.confirmed_);
        RUVIA_CHECK(server_info.confirmed_);
        RUVIA_CHECK(client_info.tls_handshake_complete_);
        RUVIA_CHECK(server_info.tls_handshake_complete_);
        RUVIA_CHECK(client_info.quic_handshake_complete_);
        RUVIA_CHECK(server_info.quic_handshake_complete_);
        RUVIA_CHECK(pair.client_->tls_handshake().info().cipher_suite_ == expected_suite);
        RUVIA_CHECK(pair.server_->tls_handshake().info().cipher_suite_ == expected_suite);
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
        RUVIA_CHECK(pair.observed_.kdf_overflows_ == 0);
        RUVIA_CHECK(pair.observed_.expected_payload_overflows_ == 0);
        RUVIA_CHECK(std::all_of(pair.observed_.kdf_events_.begin(),
            pair.observed_.kdf_events_.begin() + static_cast<std::ptrdiff_t>(pair.observed_.kdf_count_),
            [](const captured_kdf_event& event) { return event.valid_; }));
        RUVIA_CHECK(contains_extract(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            initial_salt, pair.initial_destination_id_.view(), expected_initial_secret));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_initial_secret, "client in", 32, expected_client_initial_secret));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_initial_secret, "server in", 32, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_initial_secret, "quic key", 16, client_initial_key));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_initial_secret, "quic iv", 12, hex_bytes("fa044b2f42a3fd3b46fb255c")));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_initial_secret, "quic hp", 16, client_initial_hp));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_initial_secret, "quic key", 16, server_initial_key));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_initial_secret, "quic iv", 12, hex_bytes("0ac1493ca1905853b0bba03e")));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_initial_secret, "quic hp", 16, server_initial_hp));
        RUVIA_CHECK(contains_key(pair.observed_.aead_keys_.data(), pair.observed_.aead_count_,
            client_initial_key, ruvia::quic_crypto_direction::write));
        RUVIA_CHECK(contains_key(pair.observed_.aead_keys_.data(), pair.observed_.aead_count_,
            server_initial_key, ruvia::quic_crypto_direction::read));
        RUVIA_CHECK(contains_key(pair.observed_.header_keys_.data(), pair.observed_.header_count_,
            client_initial_hp));
        RUVIA_CHECK(contains_key(pair.observed_.header_keys_.data(), pair.observed_.header_count_,
            server_initial_hp));
        const auto [hash_size, key_size] = expected_suite == ruvia::quic_cipher_suite::aes_128_gcm_sha256
                                               ? std::pair<std::size_t, std::size_t>{32, 16}
                                           : expected_suite == ruvia::quic_cipher_suite::aes_256_gcm_sha384
                                               ? std::pair<std::size_t, std::size_t>{48, 32}
                                               : std::pair<std::size_t, std::size_t>{32, 32};
        RUVIA_CHECK(contains_expand_shape(pair.observed_, expected_suite, "quic key",
            hash_size, key_size, expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_shape(pair.observed_, expected_suite, "quic iv",
            hash_size, 12, expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_shape(pair.observed_, expected_suite, "quic hp",
            hash_size, key_size, expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_key_capture(pair.observed_, expected_suite, "quic key",
            hash_size, pair.observed_.aead_keys_.data(), pair.observed_.aead_count_,
            expected_client_initial_secret, expected_server_initial_secret));
        RUVIA_CHECK(contains_expand_key_capture(pair.observed_, expected_suite, "quic hp",
            hash_size, pair.observed_.header_keys_.data(), pair.observed_.header_count_,
            expected_client_initial_secret, expected_server_initial_secret));
        const auto alpn = pair.client_->tls_handshake().info().negotiated_alpn_;
        RUVIA_CHECK(alpn.size() == 2);
        RUVIA_CHECK(std::memcmp(alpn.data(), "h3", 2) == 0);
        const auto local_parameters = pair.client_->tls_handshake().local_transport_parameters();
        std::vector<std::byte> retained_parameters(local_parameters.begin(), local_parameters.end());

        for (int operation = 0; operation < 8; ++operation) {
            pair.transfer_client_to_server();
            pair.transfer_server_to_client();
            pair.now_ += std::chrono::milliseconds(1);
            const auto current_parameters = pair.client_->tls_handshake().local_transport_parameters();
            RUVIA_CHECK(std::equal(retained_parameters.begin(), retained_parameters.end(),
                current_parameters.begin(), current_parameters.end()));
            (void)pair.client_tls_->drive(pair.client_->tls_handshake());
            (void)pair.server_tls_->drive(pair.server_->tls_handshake());
            RUVIA_CHECK(!pair.client_->tls_handshake().failed());
            RUVIA_CHECK(!pair.server_->tls_handshake().failed());
        }
        const auto header_key_count = pair.observed_.header_count_;
        std::size_t prior_aead_key_count = pair.observed_.aead_count_;
        auto& client_crypto = pair.observed_.endpoint_events_by_id_[0];
        auto& server_crypto = pair.observed_.endpoint_events_by_id_[1];
        std::optional<std::size_t> previous_write_key_id;
        bool previous_key_phase{};
        for (std::size_t index = 0; index < client_crypto.use_count_; ++index) {
            const auto& use = client_crypto.uses_[index];
            if (use.seal_ && use.short_header_) {
                previous_write_key_id = use.key_id_;
                previous_key_phase = use.key_phase_;
            }
        }
        RUVIA_CHECK(previous_write_key_id.has_value());
        std::size_t previous_use_count = client_crypto.use_count_;
        constexpr auto three_initial_ptos = 3 * (std::chrono::milliseconds(333) +
                                                    std::chrono::milliseconds(25));
        for (int generation = 0; generation < 3; ++generation) {
            RUVIA_CHECK(pair.client_->info().confirmed_);
            const auto update_time = pair.now_;
            RUVIA_CHECK(pair.client_->update_key(update_time) ==
                        ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(pair.observed_.aead_count_ == prior_aead_key_count);

            const auto opened = pair.client_->open_stream(false);
            RUVIA_CHECK(opened.status_ == ruvia::quic_operation_status::accepted);
            const std::array<std::byte, 8> payload_value{
                std::byte{'k'}, std::byte{'u'}, std::byte{'-'},
                static_cast<std::byte>('0' + generation), std::byte{'-'},
                static_cast<std::byte>('a' + generation), std::byte{'c'}, std::byte{'k'}};
            pair.observed_.expect_payload(0, payload_value);
            pair.observed_.expect_payload(1, payload_value);
            const auto server_use_start = server_crypto.use_count_;
            const auto write = pair.client_->write_stream(opened.stream_id_, payload_value, true);
            RUVIA_CHECK(write.status_ == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(write.accepted_ == payload_value.size());

            bool received_fin{};
            bool blocked_before_ack{};
            std::vector<std::byte> received_payload;
            std::array<std::byte, 32> received_value{};
            for (int attempt_value = 0; attempt_value < 128 && !received_fin; ++attempt_value) {
                (void)pair.transfer_client_to_server();
                (void)pair.server_->accept_streams();
                for (;;) {
                    const auto read = pair.server_->read_stream(opened.stream_id_, received_value);
                    if (read.status_ == ruvia::quic_stream_read_status::data) {
                        received_payload.insert(received_payload.end(), received_value.begin(),
                            received_value.begin() + static_cast<std::ptrdiff_t>(read.size_));
                        if (!blocked_before_ack) {
                            RUVIA_CHECK(pair.client_->update_key(pair.now_) ==
                                        ruvia::quic_operation_status::would_block);
                            blocked_before_ack = true;
                        }
                    } else if (read.status_ == ruvia::quic_stream_read_status::fin) {
                        received_fin = true;
                        break;
                    } else {
                        break;
                    }
                }
                if (!received_fin) {
                    (void)pair.transfer_server_to_client();
                    pair.now_ += std::chrono::milliseconds(1);
                    (void)pair.server_->handle_expiry(pair.now_);
                    (void)pair.client_->handle_expiry(pair.now_);
                }
            }
            RUVIA_CHECK(received_payload.size() == payload_value.size());
            RUVIA_CHECK(std::equal(payload_value.begin(), payload_value.end(), received_payload.begin()));
            RUVIA_CHECK(received_fin);
            RUVIA_CHECK(blocked_before_ack);

            bool ack_delivered{};
            for (int attempt_value = 0; attempt_value < 32; ++attempt_value) {
                ack_delivered = pair.transfer_server_to_client() || ack_delivered;
                (void)pair.transfer_client_to_server();
                pair.now_ += std::chrono::milliseconds(1);
                (void)pair.server_->handle_expiry(pair.now_);
                (void)pair.client_->handle_expiry(pair.now_);
            }
            RUVIA_CHECK(ack_delivered);
            RUVIA_CHECK(pair.client_->update_key(pair.now_) ==
                        ruvia::quic_operation_status::would_block);
            if (generation < 2) {
                // Three initial PTOs include the default 333 ms initial RTT and 25 ms ACK delay.
                pair.now_ += three_initial_ptos + std::chrono::milliseconds(26);
            }
            const auto payload_use = std::find_if(
                client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(previous_use_count),
                client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count_),
                [](const observed_aead_use& use) {
                    return use.seal_ && use.contains_expected_payload_;
                });
            RUVIA_CHECK(payload_use !=
                        client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count_));
            if (payload_use !=
                client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count_)) {
                RUVIA_CHECK(payload_use->key_phase_ != previous_key_phase);
                const auto* previous_key = find_aead_capture(pair.observed_,
                    *previous_write_key_id, 0, ruvia::quic_crypto_direction::write);
                const auto* current_key = find_aead_capture(pair.observed_,
                    payload_use->key_id_, 0, ruvia::quic_crypto_direction::write);
                RUVIA_CHECK(previous_key != nullptr);
                RUVIA_CHECK(current_key != nullptr);
                if (previous_key && current_key) {
                    const auto previous_secret = secret_for_aead_capture(pair.observed_, *previous_key);
                    const auto current_secret = secret_for_aead_capture(pair.observed_, *current_key);
                    RUVIA_CHECK(previous_secret.has_value());
                    RUVIA_CHECK(current_secret.has_value());
                    if (previous_secret && current_secret) {
                        const auto updated_secret = find_expand_output(pair.observed_,
                            expected_suite, *previous_secret, "quic ku", hash_size);
                        RUVIA_CHECK(updated_secret.has_value());
                        if (updated_secret) {
                            RUVIA_CHECK(std::ranges::equal(*updated_secret, *current_secret));
                            RUVIA_CHECK(contains_expand(pair.observed_, expected_suite,
                                *updated_secret, "quic key", current_key->size_,
                                std::span<const std::byte>(current_key->bytes_).first(current_key->size_)));
                            RUVIA_CHECK(contains_expand(pair.observed_, expected_suite,
                                *updated_secret, "quic iv", 12));
                        }
                    }
                }
                RUVIA_CHECK(std::any_of(
                    server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_use_start),
                    server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count_),
                    [](const observed_aead_use& use) {
                        return !use.seal_ && use.authenticated_ && use.contains_expected_payload_;
                    }));
                previous_write_key_id = payload_use->key_id_;
                previous_key_phase = payload_use->key_phase_;
                previous_use_count = client_crypto.use_count_;
            }
            pair.observed_.expect_payload(0, std::span<const std::byte>{});
            pair.observed_.expect_payload(1, std::span<const std::byte>{});
            RUVIA_CHECK(pair.observed_.aead_count_ > prior_aead_key_count);
            prior_aead_key_count = pair.observed_.aead_count_;
            RUVIA_CHECK(pair.observed_.header_count_ == header_key_count);
            RUVIA_CHECK(pair.observed_.aead_overflows_ == 0);
            RUVIA_CHECK(pair.observed_.header_overflows_ == 0);
            RUVIA_CHECK(client_crypto.use_overflows_ == 0);
            RUVIA_CHECK(server_crypto.use_overflows_ == 0);
            RUVIA_CHECK(client_crypto.destroy_overflows_ == 0);
            RUVIA_CHECK(server_crypto.destroy_overflows_ == 0);
        }
        RUVIA_CHECK(pair.client_->info().confirmed_);
        RUVIA_CHECK(pair.server_->info().confirmed_);
        RUVIA_CHECK(pair.resource_.live_bytes_ >= pair.crypto_live_bytes_);
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
        RUVIA_CHECK(all_aead_keys_destroyed_once(pair.observed_));
        RUVIA_CHECK(pair.observed_.kdf_overflows_ == 0);
        RUVIA_CHECK(pair.observed_.header_overflows_ == 0);
        RUVIA_CHECK(pair.observed_.expected_payload_overflows_ == 0);
        RUVIA_CHECK(pair.observed_.endpoint_events_by_id_[0].use_overflows_ == 0);
        RUVIA_CHECK(pair.observed_.endpoint_events_by_id_[1].use_overflows_ == 0);
        RUVIA_CHECK(pair.observed_.endpoint_events_by_id_[0].destroy_overflows_ == 0);
        RUVIA_CHECK(pair.observed_.endpoint_events_by_id_[1].destroy_overflows_ == 0);
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
    const auto provider = pair.crypto_.view();
    const auto first_initial = pair.client_->write_packet(pair.datagram_, pair.now_);
    RUVIA_CHECK(first_initial.size_ != 0);
    auto& client_crypto = pair.observed_.endpoint_events_by_id_[0];
    const auto initial_use_count = client_crypto.use_count_;
    const auto initial_key_count = pair.observed_.aead_count_;
    auto retry_integrity_key = provider.create_aead_key_(
        provider.context_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        ruvia::quic_crypto_direction::write, retry_integrity_key_bytes);
    std::array<std::byte, 29> retry_pseudo_packet{};
    retry_pseudo_packet[0] = static_cast<std::byte>(pair.initial_destination_id_.size());
    std::copy(pair.initial_destination_id_.view().begin(), pair.initial_destination_id_.view().end(),
        retry_pseudo_packet.begin() + 1);
    std::copy_n(std::as_bytes(std::span(retry_packet)).begin(), 20,
        retry_pseudo_packet.begin() + 9);
    std::array<std::byte, 12> retry_integrity_nonce{};
    std::copy(retry_integrity_nonce_bytes.begin(), retry_integrity_nonce_bytes.end(),
        retry_integrity_nonce.begin());
    std::array<std::byte, 16> retry_integrity_tag{};
    retry_integrity_key.seal(retry_integrity_nonce, retry_pseudo_packet,
        std::span<const std::byte>{}, retry_integrity_tag);
    provider.secure_erase_(provider.context_, retry_integrity_key_bytes);
    RUVIA_CHECK(std::equal(retry_integrity_tag.begin(), retry_integrity_tag.end(),
        std::as_bytes(std::span(retry_packet)).end() - 16));
    retry_integrity_key = {};

    const ruvia::quic_datagram_view retry_datagram{
        .bytes_ = std::as_bytes(std::span(retry_packet)),
        .local_ = pair.client_address_,
        .peer_ = pair.server_address_,
    };
    (void)pair.client_->receive(retry_datagram, pair.now_);
    pair.now_ += std::chrono::milliseconds(1);
    const auto retry_initial = pair.client_->write_packet(pair.datagram_, pair.now_);
    RUVIA_CHECK(retry_initial.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(retry_initial.size_ != 0);
    const auto expected_client_key = derive_client_initial_key(provider, retry_dcid);
    const auto retry_key_capture = std::find_if(
        pair.observed_.aead_keys_.begin() + static_cast<std::ptrdiff_t>(initial_key_count),
        pair.observed_.aead_keys_.begin() + static_cast<std::ptrdiff_t>(pair.observed_.aead_count_),
        [&](const captured_key& key) {
            return key.endpoint_ == 0 && key.direction_ == ruvia::quic_crypto_direction::write &&
                   key.size_ == expected_client_key.size() &&
                   std::equal(expected_client_key.begin(), expected_client_key.end(), key.bytes_.begin());
        });
    RUVIA_CHECK(retry_key_capture !=
                pair.observed_.aead_keys_.begin() + static_cast<std::ptrdiff_t>(pair.observed_.aead_count_));
    RUVIA_CHECK(initial_has_destination_and_token(
        std::span<const std::byte>(pair.datagram_).first(retry_initial.size_), retry_dcid, retry_token));
    if (retry_key_capture !=
        pair.observed_.aead_keys_.begin() + static_cast<std::ptrdiff_t>(pair.observed_.aead_count_)) {
        RUVIA_CHECK(std::any_of(
            client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(initial_use_count),
            client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count_),
            [&](const observed_aead_use& use) {
                return use.seal_ && use.operation_succeeded_ && use.key_id_ == retry_key_capture->id_;
            }));
    }

    packet_pair bad_tag_pair("TLS_AES_128_GCM_SHA256", "h3", true, false,
        "localhost", true, false, true);
    const auto bad_initial = bad_tag_pair.client_->write_packet(bad_tag_pair.datagram_, bad_tag_pair.now_);
    RUVIA_CHECK(bad_initial.size_ != 0);
    auto bad_retry = retry_packet;
    bad_retry.back() ^= 1;
    const ruvia::quic_datagram_view bad_retry_datagram{
        .bytes_ = std::as_bytes(std::span(bad_retry)),
        .local_ = bad_tag_pair.client_address_,
        .peer_ = bad_tag_pair.server_address_,
    };
    const auto bad_retry_key_count = bad_tag_pair.observed_.aead_count_;
    auto& bad_client_crypto = bad_tag_pair.observed_.endpoint_events_by_id_[0];
    const auto bad_retry_use_count = bad_client_crypto.use_count_;
    (void)bad_tag_pair.client_->receive(bad_retry_datagram, bad_tag_pair.now_);
    if (const auto expiry = bad_tag_pair.client_->next_expiry()) {
        bad_tag_pair.now_ = std::max(bad_tag_pair.now_, *expiry);
        (void)bad_tag_pair.client_->handle_expiry(bad_tag_pair.now_);
    } else {
        bad_tag_pair.now_ += std::chrono::milliseconds(1);
    }
    const auto rejected_retry_output = bad_tag_pair.client_->write_packet(
        bad_tag_pair.datagram_, bad_tag_pair.now_);
    RUVIA_CHECK(rejected_retry_output.size_ != 0);
    RUVIA_CHECK(initial_has_destination_and_token(
        std::span<const std::byte>(bad_tag_pair.datagram_).first(rejected_retry_output.size_),
        bad_tag_pair.initial_destination_id_.view(), std::span<const std::byte>{}));
    const auto original_client_key = derive_client_initial_key(
        bad_tag_pair.crypto_.view(), bad_tag_pair.initial_destination_id_.view());
    RUVIA_CHECK(contains_key(bad_tag_pair.observed_.aead_keys_.data(),
        bad_tag_pair.observed_.aead_count_, original_client_key,
        ruvia::quic_crypto_direction::write));
    RUVIA_CHECK(!std::any_of(
        bad_tag_pair.observed_.aead_keys_.begin() + static_cast<std::ptrdiff_t>(bad_retry_key_count),
        bad_tag_pair.observed_.aead_keys_.begin() + static_cast<std::ptrdiff_t>(bad_tag_pair.observed_.aead_count_),
        [&](const captured_key& key) {
            return key.endpoint_ == 0 && key.direction_ == ruvia::quic_crypto_direction::write &&
                   key.size_ == expected_client_key.size() &&
                   std::equal(expected_client_key.begin(), expected_client_key.end(), key.bytes_.begin());
        }));
    RUVIA_CHECK(std::none_of(
        bad_client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(bad_retry_use_count),
        bad_client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(bad_client_crypto.use_count_),
        [](const observed_aead_use& use) {
            return !use.seal_ && use.authenticated_;
        }));
    bad_tag_pair.retire_connections();
    RUVIA_CHECK(bad_tag_pair.resources_released());
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_accepts_reordered_old_key_phase_packets) {
    packet_pair pair;
    RUVIA_CHECK(pair.drive_until_terminal());
    const auto old_stream = pair.client_->open_stream(false);
    RUVIA_CHECK(old_stream.status_ == ruvia::quic_operation_status::accepted);
    constexpr std::array<std::byte, 9> old_payload{
        std::byte{'o'}, std::byte{'l'}, std::byte{'d'}, std::byte{'-'},
        std::byte{'p'}, std::byte{'h'}, std::byte{'a'}, std::byte{'s'}, std::byte{'e'}};
    pair.observed_.expect_payload(0, old_payload);
    pair.observed_.expect_payload(1, old_payload);
    const auto old_write = pair.client_->write_stream(old_stream.stream_id_, old_payload, true);
    RUVIA_CHECK(old_write.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(old_write.accepted_ == old_payload.size());
    auto& client_crypto = pair.observed_.endpoint_events_by_id_[0];
    auto& server_crypto = pair.observed_.endpoint_events_by_id_[1];
    const auto old_use_start = client_crypto.use_count_;
    std::array<std::byte, 2048> old_packet{};
    const auto old_result = pair.client_->write_packet(old_packet, pair.now_);
    RUVIA_CHECK(old_result.size_ != 0);
    const auto old_client_use = std::find_if(
        client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(old_use_start),
        client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count_),
        [](const observed_aead_use& use) {
            return use.seal_ && use.short_header_ && use.contains_expected_payload_;
        });
    RUVIA_CHECK(old_client_use !=
                client_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(client_crypto.use_count_));
    if (old_client_use == client_crypto.uses_.begin() +
                              static_cast<std::ptrdiff_t>(client_crypto.use_count_)) {
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
        return;
    }
    const auto old_client_key_id = old_client_use->key_id_;
    const bool old_phase = old_client_use->key_phase_;
    pair.observed_.expect_payload(0, std::span<const std::byte>{});
    pair.observed_.expect_payload(1, std::span<const std::byte>{});

    RUVIA_CHECK(pair.client_->update_key(pair.now_) ==
                ruvia::quic_operation_status::accepted);
    const auto new_stream = pair.client_->open_stream(false);
    RUVIA_CHECK(new_stream.status_ == ruvia::quic_operation_status::accepted);
    constexpr std::array<std::byte, 10> new_payload{
        std::byte{'n'}, std::byte{'e'}, std::byte{'w'}, std::byte{'-'},
        std::byte{'p'}, std::byte{'h'}, std::byte{'a'}, std::byte{'s'},
        std::byte{'e'}, std::byte{'!'}};
    pair.observed_.expect_payload(0, new_payload);
    pair.observed_.expect_payload(1, new_payload);
    const auto new_write = pair.client_->write_stream(new_stream.stream_id_, new_payload, true);
    RUVIA_CHECK(new_write.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(new_write.accepted_ == new_payload.size());
    const auto new_server_use_start = server_crypto.use_count_;
    std::vector<std::byte> new_received;
    std::array<std::byte, 32> read_buffer{};
    bool new_fin{};
    for (int attempt_value = 0; attempt_value < 128 && !new_fin; ++attempt_value) {
        (void)pair.transfer_client_to_server();
        (void)pair.server_->accept_streams();
        for (;;) {
            const auto result_value = pair.server_->read_stream(new_stream.stream_id_, read_buffer);
            if (result_value.status_ == ruvia::quic_stream_read_status::data) {
                new_received.insert(new_received.end(), read_buffer.begin(),
                    read_buffer.begin() + static_cast<std::ptrdiff_t>(result_value.size_));
            } else if (result_value.status_ == ruvia::quic_stream_read_status::fin) {
                new_fin = true;
                break;
            } else {
                break;
            }
        }
        if (!new_fin) {
            pair.now_ += std::chrono::milliseconds(1);
            (void)pair.server_->handle_expiry(pair.now_);
            (void)pair.client_->handle_expiry(pair.now_);
        }
    }
    RUVIA_CHECK(new_fin);
    RUVIA_CHECK(std::equal(new_payload.begin(), new_payload.end(),
        new_received.begin(), new_received.end()));
    const auto new_server_use = std::find_if(
        server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(new_server_use_start),
        server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count_),
        [](const observed_aead_use& use) {
            return !use.seal_ && use.authenticated_ && use.contains_expected_payload_;
        });
    RUVIA_CHECK(new_server_use !=
                server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count_));
    RUVIA_CHECK(new_server_use !=
                    server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count_) &&
                new_server_use->key_phase_ != old_phase);
    pair.observed_.expect_payload(0, std::span<const std::byte>{});
    pair.observed_.expect_payload(1, old_payload);

    const ruvia::quic_datagram_view delayed_input{
        .bytes_ = std::span<const std::byte>(old_packet).first(old_result.size_),
        .local_ = pair.server_address_,
        .peer_ = pair.client_address_,
    };
    (void)pair.server_->receive(delayed_input, pair.now_);
    (void)pair.server_->accept_streams();
    std::vector<std::byte> old_received;
    bool old_fin{};
    for (;;) {
        const auto result_value = pair.server_->read_stream(old_stream.stream_id_, read_buffer);
        if (result_value.status_ == ruvia::quic_stream_read_status::data) {
            old_received.insert(old_received.end(), read_buffer.begin(),
                read_buffer.begin() + static_cast<std::ptrdiff_t>(result_value.size_));
        } else if (result_value.status_ == ruvia::quic_stream_read_status::fin) {
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
        server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(new_server_use_start),
        server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count_),
        [old_phase](const observed_aead_use& use) {
            return !use.seal_ && use.authenticated_ && use.contains_expected_payload_ &&
                   use.key_phase_ == old_phase;
        });
    RUVIA_CHECK(old_server_use !=
                server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count_));
    RUVIA_CHECK(old_server_use !=
                    server_crypto.uses_.begin() + static_cast<std::ptrdiff_t>(server_crypto.use_count_) &&
                old_server_use->key_phase_ == old_phase);
    const auto* old_client_key = find_aead_capture(pair.observed_,
        old_client_key_id, 0, ruvia::quic_crypto_direction::write);
    if (old_server_use != server_crypto.uses_.begin() +
                              static_cast<std::ptrdiff_t>(server_crypto.use_count_)) {
        const auto* old_server_key = find_aead_capture(pair.observed_,
            old_server_use->key_id_, 1, ruvia::quic_crypto_direction::read);
        RUVIA_CHECK(old_client_key != nullptr);
        RUVIA_CHECK(old_server_key != nullptr);
        if (old_client_key && old_server_key) {
            RUVIA_CHECK(old_client_key->size_ == old_server_key->size_);
            RUVIA_CHECK(std::equal(old_client_key->bytes_.begin(),
                old_client_key->bytes_.begin() + static_cast<std::ptrdiff_t>(old_client_key->size_),
                old_server_key->bytes_.begin()));
        }
    }
    RUVIA_CHECK(pair.server_->info().state_ != ruvia::quic_connection_state::failed);
    RUVIA_CHECK(!pair.server_->tls_handshake().failed());
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_rejects_untrusted_certificate_and_alpn_mismatch) {
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", false);
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client_->info().state_ == ruvia::quic_connection_state::failed ||
                    pair.client_->tls_handshake().failed());
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h2", true);
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client_->info().state_ == ruvia::quic_connection_state::failed ||
                    pair.server_->info().state_ == ruvia::quic_connection_state::failed ||
                    pair.client_->tls_handshake().failed() || pair.server_->tls_handshake().failed() ||
                    pair.client_receive_crypto_failure_ || pair.server_receive_crypto_failure_);
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "wrong.example");
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client_->info().state_ == ruvia::quic_connection_state::failed ||
                    pair.client_->tls_handshake().failed() || pair.client_receive_crypto_failure_);
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", false);
        (void)pair.drive_until_terminal();
        RUVIA_CHECK(pair.client_->info().state_ == ruvia::quic_connection_state::failed ||
                    pair.server_->info().state_ == ruvia::quic_connection_state::failed ||
                    pair.client_->tls_handshake().failed() || pair.server_->tls_handshake().failed() ||
                    pair.client_receive_crypto_failure_ || pair.server_receive_crypto_failure_);
    }
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true, true);
    }));
}

RUVIA_TEST(openssl_quic_public_packet_pair_closes_with_a_retained_crypto_record_lease) {
    packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false,
        "localhost", true, false, false, true);
    pair.transfer_client_to_server();
    RUVIA_CHECK(pair.server_lease_driver_.record_.has_value());
    RUVIA_CHECK(static_cast<bool>(*pair.server_lease_driver_.record_));
    RUVIA_CHECK(!pair.server_lease_driver_.record_->bytes().empty());

    constexpr std::array<char, 8> reason{'c', 'a', 'n', 'c', 'e', 'l', 'l', 'e'};
    (void)pair.server_->close({.kind_ = ruvia::quic_close_kind::application,
        .code_ = 0x100,
        .frame_type_ = 0,
        .reason_ = reason});
    pair.server_tls_->stop();
    RUVIA_CHECK(static_cast<bool>(*pair.server_lease_driver_.record_));
    pair.client_tls_->stop();
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_propagates_second_aead_factory_failure_without_leaking) {
    packet_pair pair;
    const auto now = std::chrono::steady_clock::now();
    const auto packet = pair.client_->write_packet(pair.datagram_, now);
    RUVIA_CHECK(packet.size_ != 0);
    pair.observed_.throw_aead_at_ = pair.observed_.aead_attempts_ + 1;
    const ruvia::quic_datagram_view input{
        .bytes_ = std::span<const std::byte>(pair.datagram_).first(packet.size_),
        .local_ = pair.server_address_,
        .peer_ = pair.client_address_,
    };
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)pair.server_->receive(input, pair.now_); }));
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_preserves_stream_flow_control_fin_and_ack_storage) {
    packet_pair pair;
    RUVIA_CHECK(pair.drive_until_terminal());
    const auto stable_bytes = pair.resource_.live_bytes_;
    const auto opened = pair.client_->open_stream(false);
    RUVIA_CHECK(opened.status_ == ruvia::quic_operation_status::accepted);

    std::vector<std::byte> request(200003, std::byte{'q'});
    std::vector<std::byte> received;
    received.reserve(request.size());
    std::array<std::byte, 2048> block{};
    const auto first_write = pair.client_->write_stream(opened.stream_id_, request, false);
    RUVIA_CHECK(first_write.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(first_write.accepted_ != 0);
    std::size_t submitted = first_write.accepted_;
    const auto pre_ack_bytes = pair.resource_.live_bytes_;
    RUVIA_CHECK(pre_ack_bytes > stable_bytes);
    bool peer_fin{};
    for (int attempt_value = 0; attempt_value < 4000 && !peer_fin; ++attempt_value) {
        if (submitted < request.size()) {
            const auto result_value = pair.client_->write_stream(opened.stream_id_,
                std::span<const std::byte>(request).subspan(submitted), true);
            if (result_value.status_ == ruvia::quic_operation_status::accepted) {
                submitted += result_value.accepted_;
            } else {
                RUVIA_CHECK(result_value.status_ == ruvia::quic_operation_status::would_block);
            }
        }
        pair.transfer_client_to_server();
        pair.transfer_server_to_client();
        pair.now_ += std::chrono::milliseconds(1);
        (void)pair.server_->accept_streams();
        const auto result_value = pair.server_->read_stream(opened.stream_id_, block);
        if (result_value.status_ == ruvia::quic_stream_read_status::data) {
            received.insert(received.end(), block.begin(), block.begin() + result_value.size_);
        } else if (result_value.status_ == ruvia::quic_stream_read_status::fin) {
            peer_fin = true;
        }
    }
    RUVIA_CHECK(submitted == request.size());
    RUVIA_CHECK(peer_fin);
    RUVIA_CHECK(received == request);
    const auto held_bytes = pair.resource_.live_bytes_;
    RUVIA_CHECK(held_bytes >= stable_bytes);
    RUVIA_CHECK(pair.resource_.allocations_ - pair.resource_.deallocations_ >=
                pair.crypto_live_allocations_);

    constexpr std::array<std::byte, 19> response{
        std::byte{'r'}, std::byte{'e'}, std::byte{'p'}, std::byte{'l'}, std::byte{'y'},
        std::byte{' '}, std::byte{'a'}, std::byte{'f'}, std::byte{'t'}, std::byte{'e'},
        std::byte{'r'}, std::byte{' '}, std::byte{'F'}, std::byte{'I'}, std::byte{'N'},
        std::byte{'!'}, std::byte{'!'}, std::byte{'!'}, std::byte{'!'}};
    const auto server_write = pair.server_->write_stream(opened.stream_id_, response, true);
    RUVIA_CHECK(server_write.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(server_write.accepted_ == response.size());
    std::vector<std::byte> client_received;
    bool client_fin{};
    for (int attempt_value = 0; attempt_value < 1000 && !client_fin; ++attempt_value) {
        pair.transfer_server_to_client();
        pair.transfer_client_to_server();
        pair.now_ += std::chrono::milliseconds(1);
        const auto result_value = pair.client_->read_stream(opened.stream_id_, block);
        if (result_value.status_ == ruvia::quic_stream_read_status::data) {
            client_received.insert(client_received.end(), block.begin(), block.begin() + result_value.size_);
        } else if (result_value.status_ == ruvia::quic_stream_read_status::fin) {
            client_fin = true;
        }
    }
    RUVIA_CHECK(client_fin);
    RUVIA_CHECK(std::equal(client_received.begin(), client_received.end(), response.begin(), response.end()));
    (void)pair.client_->retire_completed_stream(opened.stream_id_);
    (void)pair.server_->retire_completed_stream(opened.stream_id_);
    for (int attempt_value = 0; attempt_value < 32; ++attempt_value) {
        pair.transfer_client_to_server();
        pair.transfer_server_to_client();
        pair.now_ += std::chrono::milliseconds(1);
    }
    RUVIA_CHECK(pair.resource_.live_bytes_ < pre_ack_bytes);
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_public_packet_pair_negotiates_and_bounds_datagrams) {
    {
        packet_pair unavailable;
        RUVIA_CHECK(unavailable.drive_until_terminal());
        RUVIA_CHECK(unavailable.client_->max_datagram_payload_size() == 0);
        RUVIA_CHECK(unavailable.server_->max_datagram_payload_size() == 0);
        RUVIA_CHECK(unavailable.client_->write_datagram({}) == ruvia::quic_datagram_write_status::unavailable);
    }
    packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, true);
    RUVIA_CHECK(pair.drive_until_terminal());
    RUVIA_CHECK(pair.client_->max_datagram_payload_size() > 0);
    RUVIA_CHECK(pair.server_->max_datagram_payload_size() > 0);
    const std::array<std::byte, 7> payload_value{
        std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'},
        std::byte{'g'}, std::byte{'r'}, std::byte{'m'}};
    RUVIA_CHECK(pair.client_->write_datagram(payload_value) == ruvia::quic_datagram_write_status::queued);
    std::array<std::byte, 64> output{};
    ruvia::quic_datagram_result result;
    for (int attempt_value = 0; attempt_value < 1000 && result.status_ != ruvia::quic_datagram_status::received; ++attempt_value) {
        pair.transfer_client_to_server();
        pair.transfer_server_to_client();
        pair.now_ += std::chrono::milliseconds(1);
        result = pair.server_->read_datagram(output);
    }
    RUVIA_CHECK(result.status_ == ruvia::quic_datagram_status::received);
    RUVIA_CHECK(result.size_ == payload_value.size());
    RUVIA_CHECK(std::equal(payload_value.begin(), payload_value.end(), output.begin()));
    std::vector<std::byte> oversized(1200, std::byte{'x'});
    RUVIA_CHECK(pair.client_->write_datagram(oversized) == ruvia::quic_datagram_write_status::too_large);
    for (std::size_t index = 0; index < 16; ++index) {
        RUVIA_CHECK(pair.client_->write_datagram(payload_value) == ruvia::quic_datagram_write_status::queued);
    }
    RUVIA_CHECK(pair.client_->write_datagram(payload_value) == ruvia::quic_datagram_write_status::dropped);
    for (std::size_t index = 0; index < 16; ++index) {
        result = {};
        for (int attempt_value = 0; attempt_value < 1000 && result.status_ != ruvia::quic_datagram_status::received; ++attempt_value) {
            pair.transfer_client_to_server();
            pair.transfer_server_to_client();
            pair.now_ += std::chrono::milliseconds(1);
            result = pair.server_->read_datagram(output);
        }
        RUVIA_CHECK(result.status_ == ruvia::quic_datagram_status::received);
        RUVIA_CHECK(result.size_ == payload_value.size());
    }
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_v2_retry_vector_is_accepted_through_public_packet_path) {
    packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
        false, true, false, true, ruvia::quic_version::v2, ruvia::quic_version::v2);
    const auto initial_value = pair.client_->write_packet(pair.datagram_, pair.now_);
    RUVIA_CHECK(initial_value.size_ != 0);
    auto retry = hex_bytes(
        "cf6b3343cf0008f067a5502a4262b5746f6b656ec8646ce8bfe33952d955543665dcc7b6");
    auto invalid_retry = retry;
    invalid_retry.back() ^= std::byte{1};
    const ruvia::quic_datagram_view invalid_input{
        .bytes_ = invalid_retry,
        .local_ = pair.client_address_,
        .peer_ = pair.server_address_,
    };
    (void)pair.client_->receive(invalid_input, pair.now_);
    RUVIA_CHECK(pair.client_->info().negotiated_version_ == ruvia::quic_version::v2);
    RUVIA_CHECK(pair.client_->info().state_ == ruvia::quic_connection_state::connecting);
    const ruvia::quic_datagram_view input{
        .bytes_ = retry,
        .local_ = pair.client_address_,
        .peer_ = pair.server_address_,
    };
    const auto received_value = pair.client_->receive(input, pair.now_);
    RUVIA_CHECK(received_value == ruvia::quic_operation_status::accepted ||
                received_value == ruvia::quic_operation_status::need_input);
    const auto retried_initial = pair.client_->write_packet(pair.datagram_, pair.now_);
    RUVIA_CHECK(retried_initial.size_ != 0);
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_client_migration_validates_candidate_and_keeps_connection_live) {
    packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
        false, false, false, true, ruvia::quic_version::v1, ruvia::quic_version::v1, true);
    RUVIA_CHECK(pair.drive_until_terminal());
    const auto candidate_value = address(43002);
    RUVIA_CHECK(pair.server_->start_path_migration(address(43004)).status_ ==
                ruvia::quic_migration_status::rejected);
    const auto migration = pair.client_->start_path_migration(candidate_value);
    RUVIA_CHECK(migration.status_ == ruvia::quic_migration_status::started);
    RUVIA_CHECK(pair.client_->start_path_migration(address(43003)).status_ ==
                ruvia::quic_migration_status::rejected);
    for (std::size_t attempt_value = 0; attempt_value < 1000; ++attempt_value) {
        (void)pair.transfer_client_to_server();
        (void)pair.transfer_server_to_client();
        pair.now_ += std::chrono::milliseconds(1);
        const auto current = pair.client_->path_migration(migration.id_);
        if (!current || current->status_ != ruvia::quic_migration_status::started) {
            break;
        }
    }
    const auto completed = pair.client_->path_migration(migration.id_);
    RUVIA_CHECK(completed.has_value());
    RUVIA_CHECK(completed->status_ == ruvia::quic_migration_status::validated);
    RUVIA_CHECK(pair.client_->info().local_address_.port_ == candidate_value.port_);

    const auto stream = pair.client_->open_stream(false);
    RUVIA_CHECK(stream.status_ == ruvia::quic_operation_status::accepted);
    const std::array payload_value{std::byte{'m'}, std::byte{'o'}, std::byte{'v'}, std::byte{'e'}};
    RUVIA_CHECK(pair.client_->write_stream(stream.stream_id_, payload_value, true).status_ ==
                ruvia::quic_operation_status::accepted);
    std::array<std::byte, 8> received_value{};
    bool got_payload = false;
    for (std::size_t attempt_value = 0; attempt_value < 128 && !got_payload; ++attempt_value) {
        (void)pair.transfer_client_to_server();
        (void)pair.transfer_server_to_client();
        pair.now_ += std::chrono::milliseconds(1);
        (void)pair.server_->accept_streams();
        const auto result_value = pair.server_->read_stream(stream.stream_id_, received_value);
        got_payload = result_value.status_ == ruvia::quic_stream_read_status::data &&
                      result_value.size_ == payload_value.size();
    }
    RUVIA_CHECK(got_payload);
    RUVIA_CHECK(std::equal(payload_value.begin(), payload_value.end(), received_value.begin()));

    const auto previous_local = pair.client_->info().local_address_;
    const auto failed_migration = pair.client_->start_path_migration(address(43003));
    RUVIA_CHECK(failed_migration.status_ == ruvia::quic_migration_status::started);
    for (std::size_t attempt_value = 0; attempt_value < 4000; ++attempt_value) {
        (void)pair.transfer_client_to_server();
        (void)pair.server_->write_packet(pair.datagram_, pair.now_);
        pair.now_ += std::chrono::milliseconds(1);
        (void)pair.client_->handle_expiry(pair.now_);
        (void)pair.server_->handle_expiry(pair.now_);
        const auto current = pair.client_->path_migration(failed_migration.id_);
        if (!current || current->status_ != ruvia::quic_migration_status::started) {
            break;
        }
    }
    const auto failed = pair.client_->path_migration(failed_migration.id_);
    RUVIA_CHECK(failed.has_value());
    RUVIA_CHECK(failed->status_ == ruvia::quic_migration_status::failed);
    RUVIA_CHECK(pair.client_->info().local_address_.port_ == previous_local.port_);
    pair.retire_connections();
    RUVIA_CHECK(pair.resources_released());
}

RUVIA_TEST(openssl_quic_ticket_resumption_sends_and_processes_a_real_early_stream_before_finished) {
    packet_pair first("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
        false, false, false, true, ruvia::quic_version::v1,
        ruvia::quic_version::v1, false, nullptr, nullptr, nullptr, {}, false, true, true);
    RUVIA_CHECK(first.drive_until_terminal());
    RUVIA_CHECK(!first.client_->tls_handshake().failed());
    RUVIA_CHECK(!first.server_->tls_handshake().failed());
    RUVIA_CHECK(first.client_->info().quic_handshake_complete_);
    RUVIA_CHECK(first.server_->info().quic_handshake_complete_);
    for (std::size_t attempt_value = 0; attempt_value < 128 &&
                                        (!first.client_->info().tls_handshake_complete_ ||
                                            !first.server_->info().tls_handshake_complete_);
        ++attempt_value) {
        (void)first.transfer_client_to_server();
        (void)first.transfer_server_to_client();
        first.now_ += std::chrono::milliseconds(1);
    }
    RUVIA_CHECK(first.client_->info().tls_handshake_complete_);
    RUVIA_CHECK(first.server_->info().tls_handshake_complete_);

    std::array<std::byte, 4096> encoded_parameters{};
    const auto parameter_size = first.client_->encode_early_transport_parameters(encoded_parameters);
    for (std::size_t attempt_value = 0; attempt_value < 128; ++attempt_value) {
        (void)first.transfer_server_to_client();
        (void)first.transfer_client_to_server();
        first.now_ += std::chrono::milliseconds(1);
    }
    auto ticket = first.client_tls_->take_resumption_session();
    RUVIA_CHECK(ticket != nullptr);
    RUVIA_CHECK(SSL_SESSION_is_resumable(ticket.get()) == 1);
    RUVIA_CHECK(SSL_SESSION_get_max_early_data(ticket.get()) != 0);
    counting_resource ticket_resource;
    ruvia::detail::http3_quic_client_tls_context tls_context({}, &ticket_resource);
    constexpr ruvia::http3_settings remembered_settings{};
    tls_context.remember_ticket(ticket.get(), "localhost.", ruvia::quic_version::v1,
        std::span<const std::byte>(encoded_parameters).first(parameter_size), remembered_settings);
    ruvia::detail::http3_quic_client_tls_context wrong_host_context({});
    wrong_host_context.remember_ticket(ticket.get(), "localhost", ruvia::quic_version::v1,
        std::span<const std::byte>(encoded_parameters).first(parameter_size), remembered_settings);
    RUVIA_CHECK(!wrong_host_context.take_ticket("otherhost", ruvia::quic_version::v1).has_value());
    ruvia::detail::http3_quic_client_tls_context wrong_version_context({});
    wrong_version_context.remember_ticket(ticket.get(), "localhost", ruvia::quic_version::v1,
        std::span<const std::byte>(encoded_parameters).first(parameter_size), remembered_settings);
    RUVIA_CHECK(!wrong_version_context.take_ticket("localhost", ruvia::quic_version::v2).has_value());
    auto lease_value = tls_context.take_ticket("localhost", ruvia::quic_version::v1);
    RUVIA_CHECK(lease_value.has_value());
    RUVIA_CHECK(lease_value->settings_.has_value());
    RUVIA_CHECK(!tls_context.take_ticket("localhost", ruvia::quic_version::v1).has_value());
    first.server_accept_early_data_ = true;

    packet_pair resumed("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
        false, false, false, true, ruvia::quic_version::v1,
        ruvia::quic_version::v1, false, first.client_context_.get(), first.server_context_.get(),
        lease_value->session_.get(), std::span<const std::byte>(encoded_parameters).first(parameter_size),
        true, false, true);
    const auto opened = resumed.client_->open_stream(false);
    RUVIA_CHECK(opened.status_ == ruvia::quic_operation_status::accepted);
    constexpr std::array payload_value{std::byte{'e'}, std::byte{'a'}, std::byte{'r'},
        std::byte{'l'}, std::byte{'y'}};
    const auto write = resumed.client_->write_stream(opened.stream_id_, payload_value, true);
    RUVIA_CHECK(write.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(write.accepted_ == payload_value.size());

    std::array<std::byte, 16> received_value{};
    bool processed_before_finished{};
    for (std::size_t attempt_value = 0; attempt_value < 128 && !processed_before_finished; ++attempt_value) {
        (void)resumed.transfer_client_to_server();
        (void)resumed.transfer_server_to_client();
        resumed.now_ += std::chrono::milliseconds(1);
        const auto accepted = resumed.server_->accept_streams();
        const bool stream_known = std::ranges::any_of(
            std::span(accepted.streams_).first(accepted.size_), [&](const auto& stream) {
                return stream.stream_id_ == opened.stream_id_;
            });
        if (!stream_known) {
            continue;
        }
        const auto result_value = resumed.server_->read_stream(opened.stream_id_, received_value);
        processed_before_finished = result_value.status_ == ruvia::quic_stream_read_status::data &&
                                    result_value.size_ == payload_value.size();
        if (processed_before_finished) {
            RUVIA_CHECK(!SSL_is_init_finished(resumed.client_tls_->native_handle()));
            RUVIA_CHECK(!SSL_is_init_finished(resumed.server_tls_->native_handle()));
            RUVIA_CHECK(std::equal(payload_value.begin(), payload_value.end(), received_value.begin()));
        }
    }
    RUVIA_CHECK(processed_before_finished);
    RUVIA_CHECK(resumed.drive_until_terminal());
    RUVIA_CHECK(SSL_session_reused(resumed.client_tls_->native_handle()) == 1);
    RUVIA_CHECK(resumed.client_->info().early_data_ == ruvia::quic_early_data_state::accepted);
    RUVIA_CHECK(resumed.server_->info().early_data_ == ruvia::quic_early_data_state::accepted);
    resumed.retire_connections();
    first.retire_connections();
    lease_value.reset();
    RUVIA_CHECK(ticket_resource.allocations_ == ticket_resource.deallocations_);
}

RUVIA_TEST(openssl_quic_ticket_resumption_rejects_early_stream_without_exposing_it) {
    packet_pair first("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
        false, false, false, true, ruvia::quic_version::v1,
        ruvia::quic_version::v1, false, nullptr, nullptr, nullptr, {}, false, true, true);
    RUVIA_CHECK(first.drive_until_terminal());
    for (std::size_t attempt_value = 0; attempt_value < 128; ++attempt_value) {
        (void)first.transfer_server_to_client();
        (void)first.transfer_client_to_server();
        first.now_ += std::chrono::milliseconds(1);
    }
    auto ticket = first.client_tls_->take_resumption_session();
    RUVIA_CHECK(ticket != nullptr);
    std::array<std::byte, 4096> encoded_parameters{};
    const auto parameter_size = first.client_->encode_early_transport_parameters(encoded_parameters);

    packet_pair resumed("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
        false, false, false, true, ruvia::quic_version::v1,
        ruvia::quic_version::v1, false, first.client_context_.get(), first.server_context_.get(),
        ticket.get(), std::span<const std::byte>(encoded_parameters).first(parameter_size),
        true, false, true);
    const auto opened = resumed.client_->open_stream(false);
    RUVIA_CHECK(opened.status_ == ruvia::quic_operation_status::accepted);
    constexpr std::array payload_value{std::byte{'n'}, std::byte{'o'}, std::byte{'p'}, std::byte{'e'}};
    RUVIA_CHECK(resumed.client_->write_stream(opened.stream_id_, payload_value, true).accepted_ == payload_value.size());
    RUVIA_CHECK(resumed.drive_until_terminal());
    RUVIA_CHECK(resumed.client_->info().tls_handshake_complete_);
    RUVIA_CHECK(SSL_session_reused(resumed.client_tls_->native_handle()) == 1);
    RUVIA_CHECK(resumed.client_->info().early_data_ == ruvia::quic_early_data_state::rejected);
    RUVIA_CHECK(resumed.server_->info().early_data_ == ruvia::quic_early_data_state::rejected);
    const auto streams = resumed.server_->accept_streams();
    RUVIA_CHECK(std::ranges::none_of(std::span(streams.streams_).first(streams.size_),
        [&](const auto& stream) { return stream.stream_id_ == opened.stream_id_; }));
    resumed.retire_connections();
    first.retire_connections();
}

RUVIA_TEST(openssl_quic_v1_and_v2_complete_tls_handshakes_and_compatible_v2_negotiation) {
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
            false, false, false, true, ruvia::quic_version::v1, ruvia::quic_version::v1);
        RUVIA_CHECK(pair.drive_until_terminal());
        RUVIA_CHECK(pair.client_->info().confirmed_);
        RUVIA_CHECK(pair.server_->info().confirmed_);
        RUVIA_CHECK(pair.client_->info().negotiated_version_ == ruvia::quic_version::v1);
        RUVIA_CHECK(pair.server_->info().negotiated_version_ == ruvia::quic_version::v1);
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
            false, false, false, true, ruvia::quic_version::v2, ruvia::quic_version::v2);
        RUVIA_CHECK(pair.drive_until_terminal());
        RUVIA_CHECK(pair.client_->info().confirmed_);
        RUVIA_CHECK(pair.server_->info().confirmed_);
        RUVIA_CHECK(pair.client_->info().negotiated_version_ == ruvia::quic_version::v2);
        RUVIA_CHECK(pair.server_->info().negotiated_version_ == ruvia::quic_version::v2);
        RUVIA_CHECK(has_version_information(
            pair.client_->tls_handshake().local_transport_parameters(), 0x6b3343cf));
        RUVIA_CHECK(has_version_information(
            pair.server_->tls_handshake().local_transport_parameters(), 0x6b3343cf));
        const auto initial_salt = hex_bytes("0dede3def700a6db819381be6e269dcbf9bd2ed9");
        const auto expected_initial_secret = hex_bytes(
            "2062e8b3cd8d52092614b8071d0aa1fb7c2e3ac193f78b280e72d8f5751f6aba");
        const auto expected_client_secret = hex_bytes(
            "14ec9d6eb9fd7af83bf5a668bc17a7e283766aade7ecd0891f70f9ff7f4bf47b");
        const auto expected_server_secret = hex_bytes(
            "0263db1782731bf4588e7e4d93b7463907cb8cd8200b5da55a8bd488eafc37c1");
        RUVIA_CHECK(contains_extract(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            initial_salt, pair.initial_destination_id_.view(), expected_initial_secret));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_initial_secret, "client in", 32, expected_client_secret));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_initial_secret, "server in", 32, expected_server_secret));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_secret, "quicv2 key", 16, hex_bytes("8b1a0bc121284290a29e0971b5cd045d")));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_secret, "quicv2 iv", 12, hex_bytes("91f73e2351d8fa91660e909f")));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_client_secret, "quicv2 hp", 16, hex_bytes("45b95e15235d6f45a6b19cbcb0294ba9")));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_secret, "quicv2 key", 16, hex_bytes("82db637861d55e1d011f19ea71d5d2a7")));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_secret, "quicv2 iv", 12, hex_bytes("dd13c276499c0249d3310652")));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            expected_server_secret, "quicv2 hp", 16, hex_bytes("edf6d05c83121201b436e16877593c3a")));
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
            false, false, false, true, ruvia::quic_version::v1, ruvia::quic_version::v2);
        RUVIA_CHECK(pair.drive_until_terminal());
        RUVIA_CHECK(pair.client_->info().confirmed_);
        RUVIA_CHECK(pair.server_->info().confirmed_);
        RUVIA_CHECK(pair.client_->info().negotiated_version_ == ruvia::quic_version::v2);
        RUVIA_CHECK(pair.server_->info().negotiated_version_ == ruvia::quic_version::v2);
        RUVIA_CHECK(has_version_information(
            pair.client_->tls_handshake().local_transport_parameters(), 0x00000001));
        RUVIA_CHECK(has_version_information(
            pair.server_->tls_handshake().local_transport_parameters(), 0x6b3343cf));
        RUVIA_CHECK(contains_expand(pair.observed_, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            hex_bytes("14ec9d6eb9fd7af83bf5a668bc17a7e283766aade7ecd0891f70f9ff7f4bf47b"),
            "quicv2 key", 16, hex_bytes("8b1a0bc121284290a29e0971b5cd045d")));
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
    }
    {
        packet_pair pair("TLS_AES_128_GCM_SHA256", "h3", true, false, "localhost", true,
            false, false, false, true, ruvia::quic_version::v2, ruvia::quic_version::v1);
        RUVIA_CHECK(pair.drive_until_terminal());
        RUVIA_CHECK(pair.client_->info().confirmed_);
        RUVIA_CHECK(pair.server_->info().confirmed_);
        RUVIA_CHECK(pair.client_->info().negotiated_version_ == ruvia::quic_version::v1);
        RUVIA_CHECK(pair.server_->info().negotiated_version_ == ruvia::quic_version::v1);
        RUVIA_CHECK(has_version_information(
            pair.client_->tls_handshake().local_transport_parameters(), 0x6b3343cf));
        RUVIA_CHECK(has_version_information(
            pair.server_->tls_handshake().local_transport_parameters(), 0x00000001));
        pair.retire_connections();
        RUVIA_CHECK(pair.resources_released());
    }
}
