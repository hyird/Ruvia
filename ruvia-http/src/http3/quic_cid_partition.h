#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "ruvia/http/quic_crypto_provider.h"

namespace ruvia::detail {

inline constexpr std::size_t quic_server_connection_id_size = 16;

inline void validate_quic_cid_partition(quic_cid_partition partition) {
    if (partition.count_ == 0 || partition.index_ >= partition.count_) {
        throw std::invalid_argument("invalid QUIC CID partition: require index < count and count > 0");
    }
}

inline std::uint32_t quic_cid_partition_word(std::span<const std::byte> cid) noexcept {
    std::uint32_t word{};
    for (const auto byte : cid.first(cid.size() < 4 ? cid.size() : 4)) {
        word = (word << 8) | std::to_integer<std::uint8_t>(byte);
    }
    return word;
}

inline std::uint32_t quic_connection_id_partition(std::span<const std::byte> cid,
    std::uint32_t count) noexcept {
    return count == 1 ? 0 : quic_cid_partition_word(cid) % count;
}

inline void generate_quic_server_connection_id(quic_crypto_provider_view crypto,
    std::span<std::byte> output, quic_cid_partition partition) {
    validate_quic_cid_partition(partition);
    if (partition.count_ != 1 && output.size() != quic_server_connection_id_size) {
        throw std::invalid_argument("QUIC server connection IDs must contain 16 bytes");
    }
    crypto.random_bytes_(crypto.context_, output);
    if (partition.count_ == 1) {
        return;
    }
    // Select a representable routing word in constant time, not by retrying
    // random CIDs. The remaining 96 bits retain all their random entropy.
    const auto buckets = (std::uint64_t{1} << 32) / partition.count_;
    auto word = static_cast<std::uint32_t>(
        (quic_cid_partition_word(output) % buckets) * partition.count_ + partition.index_);
    for (std::size_t i = 4; i != 0; --i) {
        output[i - 1] = static_cast<std::byte>(word & 0xffU);
        word >>= 8;
    }
}

}  // namespace ruvia::detail
