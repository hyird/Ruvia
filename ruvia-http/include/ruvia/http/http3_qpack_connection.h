#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"

namespace ruvia {

enum class http3_qpack_connection_error : std::uint8_t {
    decompression_failed,
    encoder_stream_error,
    decoder_stream_error,
    closed_critical_stream,
    limit,
    invalid_stream_id,
};

struct http3_qpack_decoder_config final {
    std::size_t max_table_capacity_{4096};
    std::size_t max_blocked_streams_{16};
    http3_field_section_limits fields_{};
    std::size_t max_pending_output_bytes_{256 * 1024};
};

struct http3_qpack_encoder_config final {
    // Values received in the peer's SETTINGS.
    std::size_t max_table_capacity_{4096};
    std::size_t max_blocked_streams_{16};
    http3_field_section_limits fields_{};
    // Local policy may choose a smaller table without changing Required Insert
    // Count wrapping, which always uses the peer's advertised maximum.
    std::optional<std::size_t> table_capacity_{};
    std::size_t max_outstanding_sections_{4096};
    std::size_t max_pending_output_bytes_{256 * 1024};
};

enum class http3_qpack_decode_status : std::uint8_t { decoded,
    blocked,
    callback_stopped };
struct http3_qpack_decode_result final {
    http3_qpack_decode_status status_{http3_qpack_decode_status::decoded};
    std::size_t fields_{0};
};

// RFC 9204 receive context, shared by all streams in one HTTP/3 connection.
// Blocking retains only the stream ID: the driver must retain the complete
// field section and retry after consume_encoder() advances insert_count().
// Callback views expire on return; no callback may reenter this context.
// Both contexts own all storage in resource, which must outlive them. They
// are worker-affine and do not perform transport I/O. Critical-stream output
// excludes the stream-type prefix. Consume output only after it is transmitted.
class http3_qpack_decoder final {
public:
    http3_qpack_decoder(http3_qpack_decoder_config config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~http3_qpack_decoder();
    http3_qpack_decoder(const http3_qpack_decoder&) = delete;
    http3_qpack_decoder& operator=(const http3_qpack_decoder&) = delete;
    [[nodiscard]] std::variant<std::monostate, http3_qpack_connection_error> consume_encoder(
        std::span<const char> bytes, bool fin = false);
    [[nodiscard]] std::variant<http3_qpack_decode_result, http3_qpack_connection_error> decode(
        std::uint64_t stream_id, std::span<const char> section,
        http3_field_section_callback_type callback, void* context);
    // Cancels a blocked or outstanding field section; emits Stream Cancellation.
    [[nodiscard]] std::variant<std::monostate, http3_qpack_connection_error> cancel(std::uint64_t stream_id);
    [[nodiscard]] std::span<const char> pending_decoder_output() const& noexcept;
    std::span<const char> pending_decoder_output() const&& = delete;
    [[nodiscard]] bool consume_decoder_output(std::size_t bytes) noexcept;
    [[nodiscard]] std::uint64_t insert_count() const noexcept;
    [[nodiscard]] std::size_t blocked_stream_count() const noexcept;

private:
    struct impl_type;
    std::pmr::memory_resource* resource_;
    impl_type* impl_;
};

// Dynamic references are pinned until peer acknowledgment/cancellation. When
// capacity, blocked-stream allowance, or eviction safety prevents insertion,
// encoding falls back to valid static/literal representations. A limit result
// is recoverable; encoding may already have inserted entries and queued valid
// encoder-stream instructions, so the caller must continue draining pending
// encoder output. An exception propagates and latches decoder_stream_error as a
// terminal connection failure. After an exception, do not send any remaining
// pending encoder output or retry/recreate the encoder on the same connection.
// The returned field-section vector uses result_resource (or the encoder's
// resource when null); that resource must outlive the returned vector. An
// exception, including result allocation failure, leaves the encoder terminal.
class http3_qpack_encoder final {
public:
    http3_qpack_encoder(http3_qpack_encoder_config config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~http3_qpack_encoder();
    http3_qpack_encoder(const http3_qpack_encoder&) = delete;
    http3_qpack_encoder& operator=(const http3_qpack_encoder&) = delete;
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_qpack_connection_error> encode(
        std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields,
        http3_field_section_limits limits = {}, std::pmr::memory_resource* result_resource = nullptr);
    [[nodiscard]] std::variant<std::monostate, http3_qpack_connection_error> consume_decoder(
        std::span<const char> bytes, bool fin = false);
    [[nodiscard]] std::span<const char> pending_encoder_output() const& noexcept;
    std::span<const char> pending_encoder_output() const&& = delete;
    [[nodiscard]] bool consume_encoder_output(std::size_t bytes) noexcept;
    [[nodiscard]] std::uint64_t insert_count() const noexcept;
    [[nodiscard]] std::uint64_t known_received_count() const noexcept;

private:
    struct impl_type;
    std::pmr::memory_resource* resource_;
    impl_type* impl_;
};
}  // namespace ruvia
