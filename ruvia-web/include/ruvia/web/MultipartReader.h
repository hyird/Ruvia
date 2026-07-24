#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/MultipartParser.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/ScopedOperation.h"

#include <memory_resource>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ruvia {

class MultipartReader final {
public:
    MultipartReader(BodyReader& bodyReader, MultipartBoundary boundary, std::pmr::memory_resource* resource)
        : bodyReader_(bodyReader),
          parser_(std::move(boundary), resource) {}

    MultipartReader(const MultipartReader&) = delete;
    MultipartReader& operator=(const MultipartReader&) = delete;
    MultipartReader(MultipartReader&&) = delete;
    MultipartReader& operator=(MultipartReader&&) = delete;

    /// Returns one typed chunk of the current multipart part. All views in the
    /// returned value remain valid only until the next read() call.
    [[nodiscard]] ScopedOperation<std::optional<MultipartStreamPart>> read();

private:
    enum class State : std::uint8_t {
        kReady,
        kReading,
        kFinished,
        kFailed,
    };

    class ReadGuard final {
    public:
        explicit ReadGuard(State& state) noexcept
            : state_(state) {}

        ~ReadGuard() {
            if (!committed_) {
                state_ = State::kFailed;
            }
        }

        void commit(State state) noexcept {
            state_ = state;
            committed_ = true;
        }

    private:
        State& state_;
        bool committed_{false};
    };

    BodyReader& bodyReader_;
    MultipartParser parser_;
    State state_{State::kReady};
    detail::ScopedOperationScope operationScope_;

    [[nodiscard]] Task<std::optional<MultipartStreamPart>> readTask();
};

}  // namespace ruvia
