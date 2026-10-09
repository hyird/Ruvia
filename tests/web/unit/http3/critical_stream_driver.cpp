#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_local_critical_streams.h"

#include "http3/http3_critical_stream_driver.h"
#include "test_harness.h"

namespace {

using driver_type = ruvia::detail::http3_critical_stream_driver;
using stream_open_type = ruvia::quic_stream_open_result;
using stream_write_type = ruvia::quic_stream_write_result;
using operation_status_type = ruvia::quic_operation_status;

struct fake_quic final {
    std::array<std::string, 3> accepted_{};
    std::array<const char*, 3> retry_address_{};
    std::array<std::size_t, 3> retry_size_{};
    std::array<int, 3> writes_{};
    int opens_{};
    bool no_credit_{true};
    bool fail_decoder_{false};

    [[nodiscard]] stream_open_type open(driver_type::kind_type kind) {
        ++opens_;
        if (kind == driver_type::kind_type::qpack_encoder && no_credit_) {
            no_credit_ = false;
            return {.status_ = operation_status_type::would_block};
        }
        return {.status_ = operation_status_type::accepted,
            .stream_id_ = 2 + 4 * static_cast<std::uint64_t>(kind)};
    }

    [[nodiscard]] stream_write_type write(std::uint64_t id, std::span<const char> bytes_value) {
        const auto index = static_cast<std::size_t>((id - 2) / 4);
        if (index >= accepted_.size()) {
            return {.status_ = operation_status_type::closing};
        }
        ++writes_[index];
        if (index == 0 && writes_[index] == 1) {
            retry_address_[index] = bytes_value.data();
            retry_size_[index] = bytes_value.size();
            return {.status_ = operation_status_type::would_block};
        }
        if (index == 0 && writes_[index] == 2 &&
            (bytes_value.data() != retry_address_[index] || bytes_value.size() != retry_size_[index])) {
            return {.status_ = operation_status_type::closing};
        }
        if (index == 2 && fail_decoder_) {
            return {.status_ = operation_status_type::closing};
        }
        const auto count = index == 0 && writes_[index] == 2 ? std::size_t{1} : bytes_value.size();
        accepted_[index].append(bytes_value.data(), count);
        return {.status_ = operation_status_type::accepted, .accepted_ = count};
    }
};

}  // namespace

RUVIA_TEST(http3_critical_stream_driver_retries_credit_and_want_without_concluding_streams) {
    const auto prefixes = ruvia::http3_local_critical_streams::create();
    RUVIA_CHECK((prefixes.index() == 0));
    if ((prefixes.index() != 0)) {
        return;
    }
    driver_type driver(std::get<0>(prefixes));
    fake_quic quic;
    RUVIA_CHECK(!driver.complete());
    bool wrote_before_credit = false;
    RUVIA_CHECK(driver.drive(
                    [](driver_type::kind_type) {
                        return stream_open_type{.status_ = operation_status_type::would_block};
                    },
                    [&](std::uint64_t, std::span<const char>) {
                        wrote_before_credit = true;
                        return stream_write_type{.status_ = operation_status_type::closing};
                    }) == driver_type::result_type::blocked);
    RUVIA_CHECK(!wrote_before_credit);
    auto open = [&](driver_type::kind_type kind) { return quic.open(kind); };
    auto write = [&](std::uint64_t id, std::span<const char> bytes_value) {
        return quic.write(id, bytes_value);
    };
    for (int i = 0; i < 5; ++i) {
        if (driver.drive(open, write) == driver_type::result_type::ready) {
            break;
        }
    }
    RUVIA_CHECK(driver.drive(open, write) == driver_type::result_type::ready);
    RUVIA_CHECK(driver.complete());
    RUVIA_CHECK_EQ(quic.accepted_[0], std::string(std::get<0>(prefixes).control_prefix().data(),
                                          std::get<0>(prefixes).control_prefix().size()));
    RUVIA_CHECK_EQ(quic.accepted_[1], std::string(std::get<0>(prefixes).qpack_encoder_prefix().data(),
                                          std::get<0>(prefixes).qpack_encoder_prefix().size()));
    RUVIA_CHECK_EQ(quic.accepted_[2], std::string(std::get<0>(prefixes).qpack_decoder_prefix().data(),
                                          std::get<0>(prefixes).qpack_decoder_prefix().size()));
    RUVIA_CHECK(driver.queue_goaway(12));
    RUVIA_CHECK(!driver.complete());
    RUVIA_CHECK(!driver.queue_goaway(16));
    RUVIA_CHECK(driver.drive(open, write) == driver_type::result_type::ready);
    RUVIA_CHECK(driver.complete());
    const auto control_bytes = std::span<const char>(quic.accepted_[0].data(),
        quic.accepted_[0].size());
    const auto goaway = control_bytes.subspan(std::get<0>(prefixes).control_prefix().size());
    const auto decoded_goaway = ruvia::decode_http3_frame(goaway);
    RUVIA_CHECK((decoded_goaway.index() == 0) && std::get<0>(decoded_goaway).type_ ==
                                                     static_cast<std::uint64_t>(ruvia::http3_frame_type::goaway));
    if ((decoded_goaway.index() == 0)) {
        const auto identifier = ruvia::decode_http3_var_int(std::get<0>(decoded_goaway).payload_);
        RUVIA_CHECK((identifier.index() == 0) && std::get<0>(identifier).value_ == 12);
        RUVIA_CHECK_EQ(std::get<0>(decoded_goaway).encoded_bytes_, goaway.size());
    }
    RUVIA_CHECK(driver.stream_id(driver_type::kind_type::control) == 2);
    RUVIA_CHECK(driver.stream_id(driver_type::kind_type::qpack_encoder) == 6);
    RUVIA_CHECK(driver.stream_id(driver_type::kind_type::qpack_decoder) == 10);
    RUVIA_CHECK_EQ(quic.opens_, 4);
}

RUVIA_TEST(http3_critical_stream_driver_latches_failed_critical_stream) {
    const auto prefixes = ruvia::http3_local_critical_streams::create();
    RUVIA_CHECK((prefixes.index() == 0));
    if ((prefixes.index() != 0)) {
        return;
    }
    driver_type driver(std::get<0>(prefixes));
    fake_quic quic;
    quic.fail_decoder_ = true;
    auto open = [&](driver_type::kind_type kind) { return quic.open(kind); };
    auto write = [&](std::uint64_t id, std::span<const char> bytes_value) {
        return quic.write(id, bytes_value);
    };
    RUVIA_CHECK(driver.drive(open, write) == driver_type::result_type::fatal);
    const auto previous_opens = quic.opens_;
    RUVIA_CHECK(driver.drive(open, write) == driver_type::result_type::fatal);
    RUVIA_CHECK_EQ(quic.opens_, previous_opens);
}
