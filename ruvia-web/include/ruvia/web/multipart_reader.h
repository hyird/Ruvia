#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/http/multipart_parser.h"
#include "ruvia/web/streaming.h"

namespace ruvia {

class multipart_reader final {
public:
    multipart_reader(body_reader& body_reader_value, multipart_parse_options options)
        : body_reader_(nullptr),
          state_(std::in_place_type<expired_state_type>) {
        if (body_reader_value.operation_scope_.active()) {
            state_.emplace<receiving_state_type>(options);
            body_reader_ = &body_reader_value;
            registration_.bind(body_reader_value.operation_scope_, this, &multipart_reader::expire_capability);
        }
    }

    multipart_reader(const multipart_reader&) = delete;
    multipart_reader& operator=(const multipart_reader&) = delete;
    multipart_reader(multipart_reader&&) = delete;
    multipart_reader& operator=(multipart_reader&&) = delete;

    /// Returns one typed chunk of the current multipart part. All returned views
    /// remain valid only until the next read() call, parent body-reader expiry, or
    /// this reader's destruction.
    [[nodiscard]] scoped_operation<std::optional<multipart_stream_part>> read() &;
    scoped_operation<std::optional<multipart_stream_part>> read() && = delete;

private:
    struct receiving_state_type final {
        explicit receiving_state_type(multipart_parse_options options)
            : parser_(options) {}

        multipart_parser parser_;
    };
    struct finished_state_type final {};
    struct failed_state_type final {};
    struct expired_state_type final {};

    using state_type = std::variant<receiving_state_type, finished_state_type, failed_state_type, expired_state_type>;

    class read_guard_type final {
    public:
        explicit read_guard_type(multipart_reader& reader_value) noexcept
            : reader_(reader_value) {}

        ~read_guard_type() {
            if (!committed_) {
                reader_.state_.emplace<failed_state_type>();
            }
        }

        void commit() noexcept {
            committed_ = true;
        }

    private:
        multipart_reader& reader_;
        bool committed_{false};
    };

    [[nodiscard]] body_reader& get_body_reader() const;
    static void expire_capability(void* target) noexcept;

    body_reader* body_reader_;
    state_type state_;
    ::ruvia::operation_scope operation_scope_;
    scoped_capability_registration registration_;

    [[nodiscard]] task<std::optional<multipart_stream_part>> read_task();
};

}  // namespace ruvia
