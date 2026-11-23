#include "tls_client_hello.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "binary_text_encoding.h"
#include "decimal_text.h"
#include "ip_address.h"

namespace ruvia {
namespace {

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t value, unsigned int shift) noexcept {
    return (value << shift) | (value >> (32U - shift));
}

[[nodiscard]] constexpr std::uint32_t rotr32(std::uint32_t value, unsigned int shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

class md5_hasher final {
public:
    void update(std::string_view input) noexcept {
        if (input.empty()) {
            return;
        }
        total_bytes_ += input.size();
        const auto* data = reinterpret_cast<const std::uint8_t*>(input.data());
        std::size_t rem = input.size();
        if (buffered_ != 0U) {
            const std::size_t take = (std::min)(rem, 64U - buffered_);
            std::memcpy(block_.data() + buffered_, data, take);
            buffered_ += take;
            data += take;
            rem -= take;
            if (buffered_ == 64U) {
                process_block(block_.data());
                buffered_ = 0;
            }
        }
        while (rem >= 64U) {
            process_block(data);
            data += 64U;
            rem -= 64U;
        }
        if (rem != 0U) {
            std::memcpy(block_.data(), data, rem);
            buffered_ = rem;
        }
    }

    void update_char(char ch) noexcept {
        ++total_bytes_;
        block_[buffered_++] = static_cast<std::uint8_t>(ch);
        if (buffered_ == 64U) {
            process_block(block_.data());
            buffered_ = 0;
        }
    }

    [[nodiscard]] std::array<char, 32> finish_hex() noexcept {
        block_[buffered_++] = 0x80U;
        if (buffered_ > 56U) {
            if (buffered_ < 64U) {
                std::memset(block_.data() + buffered_, 0, 64U - buffered_);
            }
            process_block(block_.data());
            buffered_ = 0;
        }
        if (buffered_ < 56U) {
            std::memset(block_.data() + buffered_, 0, 56U - buffered_);
        }
        const std::uint64_t bit_len = total_bytes_ * 8U;
        for (std::size_t i = 0; i < 8U; ++i) {
            block_[56U + i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
        }
        process_block(block_.data());

        std::array<std::uint8_t, 16> digest{};
        const std::array<std::uint32_t, 4> words = {a0_, b0_, c0_, d0_};
        for (std::size_t i = 0; i < 4U; ++i) {
            digest[i * 4U] = static_cast<std::uint8_t>(words[i] & 0xffU);
            digest[i * 4U + 1U] = static_cast<std::uint8_t>((words[i] >> 8U) & 0xffU);
            digest[i * 4U + 2U] = static_cast<std::uint8_t>((words[i] >> 16U) & 0xffU);
            digest[i * 4U + 3U] = static_cast<std::uint8_t>((words[i] >> 24U) & 0xffU);
        }
        std::array<char, 32> out{};
        (void)encode_lower_hex(out, as_bytes(std::span<const std::uint8_t>(digest)));
        return out;
    }

private:
    void process_block(const std::uint8_t* block) noexcept {
        static constexpr std::array<std::uint32_t, 64> k_table = {0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
        static constexpr std::array<unsigned int, 64> s_table = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16U; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4U]) | (static_cast<std::uint32_t>(block[i * 4U + 1U]) << 8U) | (static_cast<std::uint32_t>(block[i * 4U + 2U]) << 16U) | (static_cast<std::uint32_t>(block[i * 4U + 3U]) << 24U);
        }
        std::uint32_t a = a0_;
        std::uint32_t b = b0_;
        std::uint32_t c = c0_;
        std::uint32_t d = d0_;
        for (std::size_t i = 0; i < 64U; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16U) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32U) {
                f = (d & b) | ((~d) & c);
                g = (5U * i + 1U) % 16U;
            } else if (i < 48U) {
                f = b ^ c ^ d;
                g = (3U * i + 5U) % 16U;
            } else {
                f = c ^ (b | (~d));
                g = (7U * i) % 16U;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl32(a + f + k_table[i] + m[g], s_table[i]);
            a = temp;
        }
        a0_ += a;
        b0_ += b;
        c0_ += c;
        d0_ += d;
    }

    std::uint32_t a0_{0x67452301U};
    std::uint32_t b0_{0xefcdab89U};
    std::uint32_t c0_{0x98badcfeU};
    std::uint32_t d0_{0x10325476U};
    std::array<std::uint8_t, 64> block_{};
    std::size_t buffered_{0};
    std::uint64_t total_bytes_{0};
};

class sha256_hasher final {
public:
    void update(std::string_view input) noexcept {
        if (input.empty()) {
            return;
        }
        total_bytes_ += input.size();
        const auto* data = reinterpret_cast<const std::uint8_t*>(input.data());
        std::size_t rem = input.size();
        if (buffered_ != 0U) {
            const std::size_t take = (std::min)(rem, 64U - buffered_);
            std::memcpy(block_.data() + buffered_, data, take);
            buffered_ += take;
            data += take;
            rem -= take;
            if (buffered_ == 64U) {
                process_block(block_.data());
                buffered_ = 0;
            }
        }
        while (rem >= 64U) {
            process_block(data);
            data += 64U;
            rem -= 64U;
        }
        if (rem != 0U) {
            std::memcpy(block_.data(), data, rem);
            buffered_ = rem;
        }
    }

    void update_char(char ch) noexcept {
        ++total_bytes_;
        block_[buffered_++] = static_cast<std::uint8_t>(ch);
        if (buffered_ == 64U) {
            process_block(block_.data());
            buffered_ = 0;
        }
    }

    [[nodiscard]] std::array<char, 64> finish_hex() noexcept {
        block_[buffered_++] = 0x80U;
        if (buffered_ > 56U) {
            if (buffered_ < 64U) {
                std::memset(block_.data() + buffered_, 0, 64U - buffered_);
            }
            process_block(block_.data());
            buffered_ = 0;
        }
        if (buffered_ < 56U) {
            std::memset(block_.data() + buffered_, 0, 56U - buffered_);
        }
        const std::uint64_t bit_len = total_bytes_ * 8U;
        for (std::size_t i = 0; i < 8U; ++i) {
            block_[63U - i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
        }
        process_block(block_.data());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < 8U; ++i) {
            for (std::size_t b = 0; b < 4U; ++b) {
                digest[i * 4U + b] = static_cast<std::uint8_t>((state_[i] >> ((3U - b) * 8U)) & 0xffU);
            }
        }
        std::array<char, 64> out{};
        (void)encode_lower_hex(out, as_bytes(std::span<const std::uint8_t>(digest)));
        return out;
    }

private:
    void process_block(const std::uint8_t* block) noexcept {
        static constexpr std::array<std::uint32_t, 64> k_table = {0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16U; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4U]) << 24U) | (static_cast<std::uint32_t>(block[i * 4U + 1U]) << 16U) | (static_cast<std::uint32_t>(block[i * 4U + 2U]) << 8U) | static_cast<std::uint32_t>(block[i * 4U + 3U]);
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const std::uint32_t s0 = rotr32(w[i - 15U], 7) ^ rotr32(w[i - 15U], 18) ^ (w[i - 15U] >> 3U);
            const std::uint32_t s1 = rotr32(w[i - 2U], 17) ^ rotr32(w[i - 2U], 19) ^ (w[i - 2U] >> 10U);
            w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t i = 0; i < 64U; ++i) {
            const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + ch + k_table[i] + w[i];
            const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> block_{};
    std::size_t buffered_{0};
    std::uint64_t total_bytes_{0};
};

[[nodiscard]] std::array<char, 32> md5_hex_lower(std::string_view input) noexcept {
    md5_hasher hasher;
    hasher.update(input);
    return hasher.finish_hex();
}

void sink_push(std::string& out, char ch) {
    out.push_back(ch);
}

void sink_push(md5_hasher& out, char ch) noexcept {
    out.update_char(ch);
}

void sink_append(std::string& out, std::string_view sv) {
    out.append(sv);
}

void sink_append(md5_hasher& out, std::string_view sv) noexcept {
    out.update(sv);
}

template <typename sink_type>
void sink_decimal(sink_type& out, std::uint64_t value) {
    std::array<char, 20> buf{};
    std::size_t pos = buf.size();
    do {
        buf[--pos] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    sink_append(out, std::string_view(buf.data() + pos, buf.size() - pos));
}

template <typename sink_type, typename int_type>
void emit_decimal_list(sink_type& out, std::span<const int_type> items) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            sink_push(out, '-');
        }
        sink_decimal(out, static_cast<std::uint64_t>(items[index]));
    }
}

template <typename sink_type>
void emit_ja3_bytes(sink_type& out, std::uint16_t legacy_version, std::span<const std::uint16_t> cipher_suites, std::span<const std::uint16_t> extensions, std::span<const std::uint16_t> supported_groups, std::span<const std::uint8_t> ec_point_formats) {
    sink_decimal(out, legacy_version);
    sink_push(out, ',');
    emit_decimal_list(out, cipher_suites);
    sink_push(out, ',');
    emit_decimal_list(out, extensions);
    sink_push(out, ',');
    emit_decimal_list(out, supported_groups);
    sink_push(out, ',');
    emit_decimal_list(out, ec_point_formats);
}

template <typename sink_type>
void emit_peetprint_bytes(sink_type& out, std::uint16_t legacy_version, std::span<const std::uint16_t> supported_versions, std::span<const std::uint16_t> cipher_suites, std::span<const std::uint16_t> extensions, std::span<const std::uint16_t> supported_groups, std::span<const std::uint8_t> ec_point_formats, std::span<const std::string> alpn, std::span<const std::uint16_t> signature_algorithms) {
    if (!supported_versions.empty()) {
        emit_decimal_list(out, supported_versions);
    } else {
        sink_decimal(out, legacy_version);
    }
    sink_push(out, '|');
    emit_decimal_list(out, cipher_suites);
    sink_push(out, '|');
    emit_decimal_list(out, extensions);
    sink_push(out, '|');
    emit_decimal_list(out, supported_groups);
    sink_push(out, '|');
    emit_decimal_list(out, ec_point_formats);
    sink_push(out, '|');
    for (std::size_t index = 0; index < alpn.size(); ++index) {
        if (index != 0) {
            sink_push(out, '-');
        }
        sink_append(out, alpn[index]);
    }
    sink_push(out, '|');
    emit_decimal_list(out, signature_algorithms);
}

void hash_hex_u16(sha256_hasher& hasher, std::uint16_t value) noexcept {
    const std::array<char, 4> digits = {
        lower_hex_digit(value >> 12U),
        lower_hex_digit(value >> 8U),
        lower_hex_digit(value >> 4U),
        lower_hex_digit(value)
    };
    hasher.update(std::string_view(digits.data(), digits.size()));
}

void append_sorted_u16_hex_list(sha256_hasher& hasher, std::span<const std::uint16_t> items) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            hasher.update_char(',');
        }
        hash_hex_u16(hasher, items[index]);
    }
}

[[nodiscard]] std::string_view ja4_version_code(std::uint16_t version) noexcept {
    switch (version) {
        case 0x0304U:
            return "13";
        case 0x0303U:
            return "12";
        case 0x0302U:
            return "11";
        case 0x0301U:
            return "10";
        case 0x0300U:
            return "s3";
        case 0x0002U:
            return "s2";
        case 0xfeffU:
            return "d1";
        case 0xfefdU:
            return "d2";
        case 0xfefcU:
            return "d3";
        default:
            return "00";
    }
}

[[nodiscard]] constexpr bool is_ascii_alnum(char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

}  // namespace

std::uint16_t tls_client_hello::version() const noexcept {
    if (!supported_versions_.empty()) {
        std::uint16_t highest = 0;
        for (const auto ver : supported_versions_) {
            if (ver > highest) {
                highest = ver;
            }
        }
        return highest;
    }
    return legacy_version_;
}

std::string tls_client_hello::ja3_raw() const {
    std::string out;
    out.reserve(16U + (cipher_suites_.size() + extensions_.size() + supported_groups_.size() + ec_point_formats_.size()) * 6U);
    emit_ja3_bytes(out, legacy_version_, cipher_suites_, extensions_, supported_groups_, ec_point_formats_);
    return out;
}

std::string tls_client_hello::ja3() const {
    md5_hasher hasher;
    emit_ja3_bytes(hasher, legacy_version_, cipher_suites_, extensions_, supported_groups_, ec_point_formats_);
    const auto digest = hasher.finish_hex();
    return std::string(digest.data(), digest.size());
}

std::string tls_client_hello::ja4(bool quic) const {
    std::string out;
    out.reserve(36);

    out.push_back(quic ? 'q' : 't');
    out.append(ja4_version_code(version()));

    const bool has_domain_sni = !server_name_.empty() && !ip_address::try_parse(server_name_).has_value();
    out.push_back(has_domain_sni ? 'd' : 'i');

    const auto append_two_digits = [&out](std::size_t count) {
        const auto clamped = (std::min)(std::size_t{99}, count);
        out.push_back(static_cast<char>('0' + (clamped / 10U)));
        out.push_back(static_cast<char>('0' + (clamped % 10U)));
    };
    append_two_digits(cipher_suites_.size());
    append_two_digits(extensions_.size());

    if (alpn_.empty() || alpn_.front().empty()) {
        out.append("00");
    } else {
        const std::string_view first_alpn = alpn_.front();
        const char first_char = first_alpn.front();
        const char last_char = first_alpn.back();
        if (is_ascii_alnum(first_char) && is_ascii_alnum(last_char)) {
            out.push_back(first_char);
            out.push_back(last_char);
        } else {
            append_lower_hex(out, first_alpn.substr(0, 1));
        }
    }
    out.push_back('_');

    if (cipher_suites_.empty()) {
        out.append("000000000000");
    } else {
        std::array<std::uint16_t, 64> stack_ciphers{};
        std::vector<std::uint16_t> heap_ciphers;
        std::span<std::uint16_t> sorted_ciphers;
        if (cipher_suites_.size() <= stack_ciphers.size()) {
            std::ranges::copy(cipher_suites_, stack_ciphers.data());
            sorted_ciphers = std::span<std::uint16_t>(stack_ciphers.data(), cipher_suites_.size());
        } else {
            heap_ciphers.assign(cipher_suites_.begin(), cipher_suites_.end());
            sorted_ciphers = heap_ciphers;
        }
        std::ranges::sort(sorted_ciphers);
        sha256_hasher hasher;
        append_sorted_u16_hex_list(hasher, sorted_ciphers);
        const auto hash_b = hasher.finish_hex();
        out.append(hash_b.data(), 12U);
    }
    out.push_back('_');

    std::array<std::uint16_t, 64> stack_exts{};
    std::vector<std::uint16_t> heap_exts;
    std::size_t ext_count = 0;
    for (const auto ext : extensions_) {
        if (ext == 0x0000U || ext == 0x0010U) {
            continue;
        }
        if (heap_exts.empty()) {
            if (ext_count < stack_exts.size()) {
                stack_exts[ext_count++] = ext;
                continue;
            }
            heap_exts.reserve(extensions_.size());
            heap_exts.assign(stack_exts.begin(), stack_exts.end());
        }
        heap_exts.push_back(ext);
    }
    std::span<std::uint16_t> filtered_exts = heap_exts.empty() ? std::span<std::uint16_t>(stack_exts.data(), ext_count) : std::span<std::uint16_t>(heap_exts);
    std::ranges::sort(filtered_exts);

    if (filtered_exts.empty() && signature_algorithms_.empty()) {
        out.append("000000000000");
    } else {
        sha256_hasher hasher;
        append_sorted_u16_hex_list(hasher, filtered_exts);
        if (!signature_algorithms_.empty()) {
            hasher.update_char('_');
            append_sorted_u16_hex_list(hasher, signature_algorithms_);
        }
        const auto hash_c = hasher.finish_hex();
        out.append(hash_c.data(), 12U);
    }

    return out;
}

std::string tls_client_hello::peetprint() const {
    std::string out;
    out.reserve(32U + (supported_versions_.size() + cipher_suites_.size() + extensions_.size() + supported_groups_.size() + ec_point_formats_.size() + signature_algorithms_.size()) * 6U);
    emit_peetprint_bytes(out, legacy_version_, supported_versions_, cipher_suites_, extensions_, supported_groups_, ec_point_formats_, alpn_, signature_algorithms_);
    return out;
}

std::string tls_client_hello::peetprint_hash() const {
    md5_hasher hasher;
    emit_peetprint_bytes(hasher, legacy_version_, supported_versions_, cipher_suites_, extensions_, supported_groups_, ec_point_formats_, alpn_, signature_algorithms_);
    const auto digest = hasher.finish_hex();
    return std::string(digest.data(), digest.size());
}

tls_client_hello_fingerprints tls_client_hello::fingerprints(bool quic) const {
    tls_client_hello_fingerprints result;
    result.ja3_raw_ = ja3_raw();
    const auto ja3_md5 = md5_hex_lower(result.ja3_raw_);
    result.ja3_.assign(ja3_md5.data(), ja3_md5.size());
    result.ja4_ = ja4(quic);
    result.peetprint_ = peetprint();
    const auto peet_md5 = md5_hex_lower(result.peetprint_);
    result.peetprint_hash_.assign(peet_md5.data(), peet_md5.size());
    return result;
}

std::optional<tls_client_hello> parse_tls_client_hello(std::span<const std::byte> message) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(message.data());
    std::size_t size = message.size();

    // Unwrap optional 5-byte TLS record header (ContentType::handshake = 0x16).
    if (size >= 9 && data[0] == 0x16U && data[1] == 0x03U) {
        const std::size_t record_len = (static_cast<std::size_t>(data[3]) << 8U) | static_cast<std::size_t>(data[4]);
        if (record_len == size - 5U && data[5] == 0x01U) {
            data += 5U;
            size -= 5U;
        }
    }

    // Handshake message header: msg_type == 1 (ClientHello) + 3-byte length.
    if (size < 4 || data[0] != 0x01U) {
        return std::nullopt;
    }
    const std::size_t body_len = (static_cast<std::size_t>(data[1]) << 16U) | (static_cast<std::size_t>(data[2]) << 8U) | static_cast<std::size_t>(data[3]);
    if (body_len != size - 4U) {
        return std::nullopt;
    }

    const std::uint8_t* const body = data + 4U;
    std::size_t cursor = 0;
    const auto read_u8 = [&](std::uint8_t& out) noexcept {
        if (cursor + 1U > body_len) {
            return false;
        }
        out = body[cursor++];
        return true;
    };
    const auto read_u16 = [&](std::uint16_t& out) noexcept {
        if (cursor + 2U > body_len) {
            return false;
        }
        out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(body[cursor]) << 8U) | body[cursor + 1U]);
        cursor += 2U;
        return true;
    };

    tls_client_hello info;
    if (!read_u16(info.legacy_version_)) {
        return std::nullopt;
    }
    // 32-byte ClientHello random.
    if (cursor + 32U > body_len) {
        return std::nullopt;
    }
    cursor += 32U;

    // Session ID (0..32 bytes).
    std::uint8_t session_id_len = 0;
    if (!read_u8(session_id_len) || session_id_len > 32U || cursor + session_id_len > body_len) {
        return std::nullopt;
    }
    cursor += session_id_len;

    // Cipher suites (2-byte byte length, non-empty, even).
    std::uint16_t cipher_bytes = 0;
    if (!read_u16(cipher_bytes) || cipher_bytes < 2U || (cipher_bytes % 2U) != 0 || cursor + cipher_bytes > body_len) {
        return std::nullopt;
    }
    const std::size_t cipher_count = cipher_bytes / 2U;
    info.cipher_suites_.reserve(cipher_count);
    for (std::size_t i = 0; i < cipher_count; ++i) {
        std::uint16_t suite = 0;
        (void)read_u16(suite);
        if (!is_tls_grease_value(suite)) {
            info.cipher_suites_.push_back(suite);
        }
    }

    // Compression methods (1-byte length, non-empty).
    std::uint8_t comp_len = 0;
    if (!read_u8(comp_len) || comp_len == 0 || cursor + comp_len > body_len) {
        return std::nullopt;
    }
    cursor += comp_len;

    if (cursor == body_len) {
        return info;
    }

    std::uint16_t extensions_len = 0;
    if (!read_u16(extensions_len) || cursor + extensions_len != body_len) {
        return std::nullopt;
    }

    while (cursor < body_len) {
        std::uint16_t ext_type = 0;
        std::uint16_t ext_len = 0;
        if (!read_u16(ext_type) || !read_u16(ext_len) || cursor + ext_len > body_len) {
            return std::nullopt;
        }
        const std::uint8_t* const ext_data = body + cursor;
        cursor += ext_len;

        if (is_tls_grease_value(ext_type)) {
            continue;
        }
        info.extensions_.push_back(ext_type);

        switch (ext_type) {
            case 0x0000U: {  // server_name
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    if (pos + 3U > ext_len) {
                        return std::nullopt;
                    }
                    const std::uint8_t name_type = ext_data[pos];
                    const std::size_t name_len = (static_cast<std::size_t>(ext_data[pos + 1U]) << 8U) | ext_data[pos + 2U];
                    pos += 3U;
                    if (name_len == 0 || pos + name_len > ext_len) {
                        return std::nullopt;
                    }
                    if (name_type == 0 && info.server_name_.empty()) {
                        info.server_name_.assign(reinterpret_cast<const char*>(ext_data + pos), name_len);
                    }
                    pos += name_len;
                }
                break;
            }
            case 0x000aU: {  // supported_groups
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto group = static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(group)) {
                        info.supported_groups_.push_back(group);
                    }
                }
                break;
            }
            case 0x000bU: {  // ec_point_formats
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len == 0 || list_len != ext_len - 1U) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; ++pos) {
                    info.ec_point_formats_.push_back(ext_data[pos]);
                }
                break;
            }
            case 0x000dU: {  // signature_algorithms
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len != ext_len - 2U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 2U; pos < ext_len; pos += 2U) {
                    const auto scheme = static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(scheme)) {
                        info.signature_algorithms_.push_back(scheme);
                    }
                }
                break;
            }
            case 0x0010U: {  // application_layer_protocol_negotiation
                if (ext_len < 2U) {
                    return std::nullopt;
                }
                const std::size_t list_len = (static_cast<std::size_t>(ext_data[0]) << 8U) | ext_data[1];
                if (list_len == 0 || list_len != ext_len - 2U) {
                    return std::nullopt;
                }
                std::size_t pos = 2U;
                while (pos < ext_len) {
                    const std::size_t proto_len = ext_data[pos++];
                    if (proto_len == 0 || pos + proto_len > ext_len) {
                        return std::nullopt;
                    }
                    const bool grease_alpn = proto_len == 2U && is_tls_grease_value(static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]));
                    if (!grease_alpn) {
                        info.alpn_.emplace_back(reinterpret_cast<const char*>(ext_data + pos), proto_len);
                    }
                    pos += proto_len;
                }
                break;
            }
            case 0x002bU: {  // supported_versions
                if (ext_len < 1U) {
                    return std::nullopt;
                }
                const std::size_t list_len = ext_data[0];
                if (list_len < 2U || list_len != ext_len - 1U || (list_len % 2U) != 0) {
                    return std::nullopt;
                }
                for (std::size_t pos = 1U; pos < ext_len; pos += 2U) {
                    const auto ver = static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext_data[pos]) << 8U) | ext_data[pos + 1U]);
                    if (!is_tls_grease_value(ver)) {
                        info.supported_versions_.push_back(ver);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    return info;
}

std::optional<tls_client_hello> parse_tls_client_hello(std::string_view message) {
    return parse_tls_client_hello(std::span<const std::byte>(reinterpret_cast<const std::byte*>(message.data()), message.size()));
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::span<const std::byte> message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::string_view message, bool quic) {
    const auto parsed = parse_tls_client_hello(message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->fingerprints(quic);
}

}  // namespace ruvia
