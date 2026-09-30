#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/MultipartParser.h"
#include "ruvia/web/Streaming.h"

namespace ruvia {

class MultipartReader final : private detail::ScopedCapabilityNode {
public:
    MultipartReader(BodyReader& bodyReader, MultipartParseOptions options)
        : bodyReader_(nullptr),
          state_(std::in_place_type<ExpiredState>) {
        if (bodyReader.operationScope_.active()) {
            state_.emplace<ReceivingState>(options);
            bodyReader_ = &bodyReader;
            bind(bodyReader.operationScope_, &MultipartReader::expireCapability);
        }
    }

    MultipartReader(const MultipartReader&) = delete;
    MultipartReader& operator=(const MultipartReader&) = delete;
    MultipartReader(MultipartReader&&) = delete;
    MultipartReader& operator=(MultipartReader&&) = delete;

    /// Returns one typed chunk of the current multipart part. All returned views
    /// remain valid only until the next read() call, parent body-reader expiry, or
    /// this reader's destruction.
    [[nodiscard]] ScopedOperation<std::optional<MultipartStreamPart>> read() &;
    ScopedOperation<std::optional<MultipartStreamPart>> read() && = delete;

private:
    struct ReceivingState final {
        explicit ReceivingState(MultipartParseOptions options)
            : parser(options) {}

        MultipartParser parser;
    };
    struct FinishedState final {};
    struct FailedState final {};
    struct ExpiredState final {};

    using State = std::variant<ReceivingState, FinishedState, FailedState, ExpiredState>;

    class ReadGuard final {
    public:
        explicit ReadGuard(MultipartReader& reader) noexcept
            : reader_(reader) {}

        ~ReadGuard() {
            if (!committed_) {
                reader_.state_.emplace<FailedState>();
            }
        }

        void commit() noexcept {
            committed_ = true;
        }

    private:
        MultipartReader& reader_;
        bool committed_{false};
    };

    [[nodiscard]] BodyReader& bodyReader() const;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    BodyReader* bodyReader_;
    State state_;
    detail::ScopedOperationScope operationScope_;

    [[nodiscard]] Task<std::optional<MultipartStreamPart>> readTask();
};

}  // namespace ruvia
