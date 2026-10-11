#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_file.h"

#include "server/http_file_open.h"

// The one reader for buffered file response bodies (a whole file, a byte
// range, or multipart/byteranges parts interleaved with in-memory boundaries)
// used over TLS or without a native sendfile. Plain TCP keeps
// sendfile/TransmitFile.
//
// Opening, seeking and reading run on the server's blocking pool: each round
// trip reads ahead up to response_file_read_ahead_bytes (several transport
// chunks) and moves the owned read state through the pool -- the bytes are
// never copied out of the read buffer. When the deployment disabled the pool
// (application::get_blocking_pool(nullptr)), the same reads run inline on the
// worker, exactly as response compression does without a pool.
//
// Every file range is re-validated against its snapshot identity on the read
// that completes it, so the final bytes of a range are never released for a
// file that changed while being sent.

namespace ruvia::detail {

// Bytes read per blocking round trip. Kept within the process resource's
// pooled block range, so the buffer is recycled across responses.
inline constexpr std::size_t response_file_read_ahead_bytes = 64 * 1024;

// Where a file body's blocking reads run and what may abandon them.
struct response_file_io final {
    blocking_pool* pool_{};
    worker_handle worker_;
    stop_token stop_;
};

class response_body_chunk final {
public:
    enum class kind_type : std::uint8_t {
        data,
        end,
        failed,
    };

    [[nodiscard]] static constexpr response_body_chunk data(std::string_view bytes, bool last) noexcept {
        return response_body_chunk(kind_type::data, bytes, last);
    }

    [[nodiscard]] static constexpr response_body_chunk end() noexcept {
        return response_body_chunk(kind_type::end, {}, true);
    }

    [[nodiscard]] static constexpr response_body_chunk failed() noexcept {
        return response_body_chunk(kind_type::failed, {}, false);
    }

    [[nodiscard]] constexpr kind_type kind() const noexcept {
        return kind_;
    }

    // Valid until the next call to response_file_source::next().
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

    // No body bytes follow this chunk.
    [[nodiscard]] constexpr bool last() const noexcept {
        return last_;
    }

private:
    constexpr response_body_chunk(kind_type kind, std::string_view bytes, bool last) noexcept
        : bytes_(bytes),
          kind_(kind),
          last_(last) {}

    std::string_view bytes_;
    kind_type kind_;
    bool last_;
};

class response_file_source final {
public:
    // `response` is borrowed until the last chunk was consumed.
    response_file_source(const http_response& response, response_file_io io);

    response_file_source(const response_file_source&) = delete;
    response_file_source& operator=(const response_file_source&) = delete;
    response_file_source(response_file_source&&) = delete;
    response_file_source& operator=(response_file_source&&) = delete;

    // Yields the body in order: in-memory segments as borrowed views, file
    // ranges as read-ahead buffers. `failed` means the file could not be
    // opened/read, ended early, changed identity, or the read was abandoned
    // (pool rejection, stop); a protocol that already committed its head must
    // abort the response.
    [[nodiscard]] task<response_body_chunk> next();

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

    private:
        char* data_{};
    };

    struct read_state final {
        std::pmr::basic_string<std::filesystem::path::value_type> path_;
        std::uint64_t size_{};
        http_response_file_identity identity_{http_response_file_identity::unchecked()};
        std::optional<response_file_input> input_;
        read_buffer buffer_;
        std::uint64_t position_{};
        std::size_t filled_{};
        std::error_code error_{};
        bool reopen_{true};
        bool reposition_{true};
    };

    static void read_ahead(read_state& state, std::size_t bytes, bool completes_range);
    [[nodiscard]] task<bool> fill(std::size_t bytes, bool completes_range);
    void select_file(http_response_file_view file);

    const http_response& response_;
    response_file_io io_;
    read_state state_;
    const std::filesystem::path::value_type* current_path_{};
    std::uint64_t range_remaining_{};
    std::size_t next_segment_{};
    std::size_t current_segment_{};
    std::size_t segment_count_{};
    std::size_t last_segment_{};
    bool lost_{false};
};

}  // namespace ruvia::detail
