#pragma once

namespace ruvia {

// Read-only request provenance. The transport bit is set only from the local
// QUIC stack's receive callback; a request header can never set it.
class http3_early_data_info final {
public:
    constexpr http3_early_data_info() noexcept = default;
    constexpr http3_early_data_info(bool received_from_early_data,
        bool upstream_declared_early_data) noexcept
        : received_from_early_data_(received_from_early_data),
          upstream_declared_early_data_(upstream_declared_early_data) {}

    [[nodiscard]] constexpr bool received_from_early_data() const noexcept {
        return received_from_early_data_;
    }
    [[nodiscard]] constexpr bool upstream_declared_early_data() const noexcept {
        return upstream_declared_early_data_;
    }

private:
    bool received_from_early_data_{};
    bool upstream_declared_early_data_{};
};

}  // namespace ruvia
