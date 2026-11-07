#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_native_path.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_file.h"

#include "server/http_native_file.h"

// The one reader for file response bodies (a whole file, a byte range, or
// multipart/byteranges parts interleaved with in-memory boundaries) on every
// transport. Only a single large range on Windows plain TCP leaves through a
// kernel TransmitFile instead (http1_file_response_body).
//
// Opening and blocking reads run on the worker's blocking pool, never on the
// worker. Each round trip reads ahead up to response_file_read_ahead_bytes;
// Linux tries later reads with RWF_NOWAIT first. The owned read state moves
// through the pool, without copying its buffered bytes. A worker without a
// pool (application::blocking_pool(nullptr)) cannot produce a file body: the
// read fails with blocking_status::unavailable on every platform.
//
// A response framed from an open handle (context::file, static_root) is read
// through a duplicate of that handle, so the path is never resolved again.
//
// Every file range is re-validated against its snapshot identity on the read
// that completes it, so the final bytes of a range are never released for a
// file that changed while being sent. The first file range is validated
// before anything is yielded, so a transport can answer a failure with an
// error response while its head is still unsent.

namespace ruvia::detail {

// Bytes read per blocking round trip. Kept within the process resource's
// pooled block range, so the buffer is recycled across responses.
inline constexpr std::size_t response_file_read_ahead_bytes = 64 * 1024;

// Where a file body's blocking reads run -- the worker's blocking pool -- and
// what bounds or abandons them.
struct response_file_io final {
    worker_handle worker_;
    stop_token stop_;
    // Bounds each read the response waits on. Servers pass write_timeout_: the
    // next write waits for the read. An abandoned read keeps running on the
    // pool and frees its own handle and buffer; the response does not wait.
    std::optional<std::chrono::milliseconds> timeout_;
};

// Why a file body could not be produced.
class response_file_failure final {
public:
    // The read ran: the open, read or snapshot validation failed with `error`.
    [[nodiscard]] static response_file_failure read_failed(std::error_code error);
    // The read never completed: the pool refused, timed out or abandoned it.
    [[nodiscard]] static response_file_failure read_not_completed(blocking_status status);
    // The read threw on the pool thread.
    [[nodiscard]] static response_file_failure read_threw(std::exception_ptr exception) noexcept;

    // The error a response whose head was not committed is answered with:
    // 503 file_read_unavailable when the read never completed, otherwise 500
    // file_read_failed. Neither names the file or the system error.
    [[nodiscard]] std::exception_ptr response_error() const;

    // The fault for the connection failure channel, or null for an expected
    // outcome: a file removed, replaced or changed since it was framed, a
    // full pool queue (counted by blocking_pool_stats), or shutdown.
    [[nodiscard]] const std::exception_ptr& fault() const& noexcept RUVIA_LIFETIMEBOUND {
        return fault_;
    }
    const std::exception_ptr& fault() const&& = delete;

private:
    response_file_failure(bool unavailable, std::exception_ptr fault) noexcept
        : fault_(std::move(fault)),
          unavailable_(unavailable) {}

    std::exception_ptr fault_;
    bool unavailable_;
};

class response_body_chunk final {
public:
    enum class kind_type : std::uint8_t {
        data,
        end,
        failed,
    };

    [[nodiscard]] static constexpr response_body_chunk data(std::string_view bytes) noexcept {
        return response_body_chunk(kind_type::data, bytes);
    }

    [[nodiscard]] static constexpr response_body_chunk end() noexcept {
        return response_body_chunk(kind_type::end, {});
    }

    [[nodiscard]] static constexpr response_body_chunk failed() noexcept {
        return response_body_chunk(kind_type::failed, {});
    }

    [[nodiscard]] constexpr kind_type kind() const noexcept {
        return kind_;
    }

    // Valid until the next call to response_file_source::next() or
    // release_buffer().
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    constexpr response_body_chunk(kind_type kind, std::string_view bytes) noexcept
        : bytes_(bytes),
          kind_(kind) {}

    std::string_view bytes_;
    kind_type kind_;
};

class response_file_source final {
public:
    // `response` is borrowed until the last chunk was consumed.
    response_file_source(const http_response& response, response_file_io io);

    response_file_source(const response_file_source&) = delete;
    response_file_source& operator=(const response_file_source&) = delete;
    response_file_source(response_file_source&&) = delete;
    response_file_source& operator=(response_file_source&&) = delete;

    // Opens and validates the first file range without reading it or
    // allocating a read buffer: one pool round trip that lets a transport
    // commit its head before any flow-control credit exists. Idempotent.
    [[nodiscard]] task<bool> open_first();

    // Yields the body in order: in-memory segments as borrowed views, file
    // ranges as read-ahead buffers of at most `max_bytes` (nonzero). `failed`
    // means a file could not be opened or read, ended early, changed
    // identity, or the read did not complete; failure() says why. Once the
    // head is committed the connection (HTTP/1) or stream (HTTP/2) must end.
    [[nodiscard]] task<response_body_chunk> next(std::size_t max_bytes = response_file_read_ahead_bytes);

    // Returns the read buffer once the last chunk's bytes were consumed, so a
    // body waiting for flow-control credit holds none; the next file chunk
    // allocates again.
    void release_buffer() noexcept;

    // Whether the next chunk next() yields will be read from a file, so a
    // flow-controlled transport can wait for credit before reading it.
    [[nodiscard]] bool file_bytes_pending() const noexcept;

    [[nodiscard]] const response_file_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return failure_ ? &*failure_ : nullptr;
    }
    const response_file_failure* failure() const&& = delete;

private:
    // Owned by exactly one thread at a time: it moves into the blocking
    // callable and back, so an abandoned read frees it on the pool thread
    // through the synchronized process resource.
    class read_buffer final {
    public:
        read_buffer() noexcept = default;
        read_buffer(read_buffer&& other) noexcept;
        read_buffer& operator=(read_buffer&& other) noexcept;
        ~read_buffer();

        [[nodiscard]] char* data();
        void reset() noexcept;

    private:
        char* data_{};
    };

    struct read_state final {
        read_state();

        http_native_path_string_type path_;
        std::uint64_t size_{};
        http_response_file_identity identity_{http_response_file_identity::unchecked()};
        // A duplicate of the response's handle, or empty until the pool opens
        // path_ for a body framed without one.
        native_file_handle input_;
        read_buffer buffer_;
        std::uint64_t position_{};
        std::size_t filled_{};
        std::error_code error_{};
        // input_ matched identity_ and size_ when it was first used.
        bool validated_{false};
#if defined(__linux__)
        bool cached_read_supported_{true};
#endif
    };

    static void read_ahead(read_state& state, std::size_t bytes, bool completes_range);
    [[nodiscard]] task<bool> fill(std::size_t bytes, bool completes_range);
    [[nodiscard]] bool settle_read();
    void select_file(http_response_file_view file);

    const http_response& response_;
    response_file_io io_;
    read_state state_;
    std::optional<response_file_failure> failure_;
    const http_native_path_char_type* current_path_{};
    std::uint64_t range_remaining_{};
    std::size_t next_segment_{};
    std::size_t segment_count_{};
    bool first_file_opened_{false};
};

}  // namespace ruvia::detail
