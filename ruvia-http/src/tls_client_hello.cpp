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

[[nodiscard]] std::array<char, 32> md5_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
    static constexpr std::array<unsigned int, 64> s_table = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    std::uint32_t a0 = 0x67452301U;
    std::uint32_t b0 = 0xefcdab89U;
    std::uint32_t c0 = 0x98badcfeU;
    std::uint32_t d0 = 0x10325476U;

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4]) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8U) | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16U) | (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24U);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl32(a + f + k_table[i] + m[g], s_table[i]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 8 + i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    std::array<std::uint8_t, 16> digest{};
    const std::array<std::uint32_t, 4> words = {a0, b0, c0, d0};
    for (std::size_t i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(words[i] & 0xffU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 8U) & 0xffU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 16U) & 0xffU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>((words[i] >> 24U) & 0xffU);
    }

    std::array<char, 32> out{};
    http_encode_lower_hex(out.data(), digest);
    return out;
}

[[nodiscard]] std::array<char, 64> sha256_hex_lower(std::string_view input) noexcept {
    static constexpr std::array<std::uint32_t, 64> k_table = {0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    const auto process_block = [&](const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) | static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
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
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    const auto* const bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t offset = 0;
    while (input.size() - offset >= 64) {
        process_block(bytes + offset);
        offset += 64;
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t rem = input.size() - offset;
    if (rem != 0) {
        std::memcpy(tail.data(), bytes + offset, rem);
    }
    tail[rem] = 0x80U;
    const std::size_t padded_len = rem < 56 ? 64 : 128;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_len - 1 - i] = static_cast<std::uint8_t>((bit_len >> (i * 8U)) & 0xffU);
    }
    process_block(tail.data());
    if (padded_len == 128) {
        process_block(tail.data() + 64);
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t b = 0; b < 4; ++b) {
            digest[i * 4 + b] = static_cast<std::uint8_t>((state[i] >> ((3U - b) * 8U)) & 0xffU);
        }
    }
    std::array<char, 64> out{};
    http_encode_lower_hex(out.data(), digest);
    return out;
}

template <typename int_type>
void append_decimal_list(std::string& out, std::span<const int_type> items) {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        http_append_unsigned(out, static_cast<std::uint64_t>(items[index]));
    }
}

void append_hex_u16(std::string& out, std::uint16_t value) {
    const char digits[]{http_lower_hex_digit(value >> 12U), http_lower_hex_digit(value >> 8U), http_lower_hex_digit(value >> 4U), http_lower_hex_digit(value)};
    out.append(digits, sizeof(digits));
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

std::uint16_t tls_client_hello_info::version() const noexcept {
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

std::string tls_client_hello_info::ja3_raw() const {
    std::string out;
    http_append_unsigned(out, legacy_version_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back(',');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back(',');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    return out;
}

std::string tls_client_hello_info::ja3() const {
    const auto raw = ja3_raw();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

std::string tls_client_hello_info::ja4(bool quic) const {
    std::string out;
    out.reserve(36);

    out.push_back(quic ? 'q' : 't');
    out.append(ja4_version_code(version()));

    const bool has_domain_sni = !server_name_.empty() && !ip_address::parse(server_name_).has_value();
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
            http_append_lower_hex(out, first_alpn.substr(0, 1));
        }
    }
    out.push_back('_');

    if (cipher_suites_.empty()) {
        out.append("000000000000");
    } else {
        std::vector<std::uint16_t> sorted_ciphers(cipher_suites_);
        std::ranges::sort(sorted_ciphers);
        std::string cipher_list;
        cipher_list.reserve(sorted_ciphers.size() * 5U);
        for (std::size_t index = 0; index < sorted_ciphers.size(); ++index) {
            if (index != 0) {
                cipher_list.push_back(',');
            }
            append_hex_u16(cipher_list, sorted_ciphers[index]);
        }
        const auto hash_b = sha256_hex_lower(cipher_list);
        out.append(hash_b.data(), 12U);
    }
    out.push_back('_');

    std::vector<std::uint16_t> filtered_exts;
    filtered_exts.reserve(extensions_.size());
    for (const auto ext : extensions_) {
        if (ext != 0x0000U && ext != 0x0010U) {
            filtered_exts.push_back(ext);
        }
    }
    std::ranges::sort(filtered_exts);

    if (filtered_exts.empty() && signature_algorithms_.empty()) {
        out.append("000000000000");
    } else {
        std::string ext_sig_list;
        for (std::size_t index = 0; index < filtered_exts.size(); ++index) {
            if (index != 0) {
                ext_sig_list.push_back(',');
            }
            append_hex_u16(ext_sig_list, filtered_exts[index]);
        }
        if (!signature_algorithms_.empty()) {
            ext_sig_list.push_back('_');
            for (std::size_t index = 0; index < signature_algorithms_.size(); ++index) {
                if (index != 0) {
                    ext_sig_list.push_back(',');
                }
                append_hex_u16(ext_sig_list, signature_algorithms_[index]);
            }
        }
        const auto hash_c = sha256_hex_lower(ext_sig_list);
        out.append(hash_c.data(), 12U);
    }

    return out;
}

std::string tls_client_hello_info::peetprint() const {
    std::string out;
    if (!supported_versions_.empty()) {
        append_decimal_list<std::uint16_t>(out, supported_versions_);
    } else {
        http_append_unsigned(out, legacy_version_);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, cipher_suites_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, extensions_);
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, supported_groups_);
    out.push_back('|');
    append_decimal_list<std::uint8_t>(out, ec_point_formats_);
    out.push_back('|');
    for (std::size_t index = 0; index < alpn_.size(); ++index) {
        if (index != 0) {
            out.push_back('-');
        }
        out.append(alpn_[index]);
    }
    out.push_back('|');
    append_decimal_list<std::uint16_t>(out, signature_algorithms_);
    return out;
}

std::string tls_client_hello_info::peetprint_hash() const {
    const auto raw = peetprint();
    const auto digest = md5_hex_lower(raw);
    return std::string(digest.data(), digest.size());
}

tls_client_hello_fingerprints tls_client_hello_info::fingerprints(bool quic) const {
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

std::optional<tls_client_hello_info> parse_tls_client_hello(std::span<const std::byte> message) {
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

    tls_client_hello_info info;
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

std::optional<tls_client_hello_info> parse_tls_client_hello(std::string_view message) {
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
