#include "ruvia/web/multipart_reader.h"

#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/bytes.h"
#include "ruvia/core/task.h"

namespace ruvia {

scoped_operation<std::optional<multipart_stream_part>> multipart_reader::read() & {
    registration_.require_active();
    if (operation_scope_.has_pending_operations()) {
        throw std::logic_error("multipart body read is already in progress");
    }
    return ::ruvia::make_scoped_operation(operation_scope_, read_task());
}

body_reader& multipart_reader::get_body_reader() const {
    registration_.require_active();
    return *body_reader_;
}

void multipart_reader::expire_capability(void* target) noexcept {
    auto& reader_value = *static_cast<multipart_reader*>(target);
    reader_value.operation_scope_.close();
    reader_value.body_reader_ = nullptr;
    reader_value.state_.emplace<expired_state_type>();
}

task<std::optional<multipart_stream_part>> multipart_reader::read_task() {
    if (std::holds_alternative<finished_state_type>(state_)) {
        co_return std::nullopt;
    }
    if (std::holds_alternative<failed_state_type>(state_)) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    if (!std::holds_alternative<receiving_state_type>(state_)) {
        throw std::logic_error("multipart body lifetime has expired");
    }

    read_guard_type read_guard_value(*this);
    for (;;) {
        auto result_value = std::get<receiving_state_type>(state_).parser_.poll();
        if (const auto* part = result_value.part()) {
            read_guard_value.commit();
            co_return *part;
        }
        if (result_value.done() != nullptr) {
            // RFC 2046 permits an epilogue after the closing delimiter. It is
            // semantically ignored but the HTTP body still has to be consumed
            // before the connection can be reused.
            while (co_await get_body_reader().read()) {
            }
            state_.emplace<finished_state_type>();
            read_guard_value.commit();
            co_return std::nullopt;
        }
        if (result_value.need_input() != nullptr) {
            auto chunk = co_await get_body_reader().read();
            if (!chunk) {
                std::get<receiving_state_type>(state_).parser_.finish_input();
            } else {
                std::get<receiving_state_type>(state_).parser_.feed(as_chars(*chunk));
            }
            continue;
        }
        if (const auto* failure = result_value.failure()) {
            throw failure->protocol_error();
        }
        throw std::logic_error("unexpected multipart poll result");
    }
}

}  // namespace ruvia
