#pragma once

#include <cstddef>
#include <limits>

namespace ruvia::detail {

class Http2StreamBodyAccounting final {
public:
    [[nodiscard]] bool setContentLength(std::size_t value) noexcept {
        if (hasContentLength_ && contentLength_ != value) {
            return false;
        }
        contentLength_ = value;
        hasContentLength_ = true;
        return true;
    }

    [[nodiscard]] bool hasContentLength() const noexcept {
        return hasContentLength_;
    }

    [[nodiscard]] std::size_t contentLength() const noexcept {
        return contentLength_;
    }

    void setReceivedBytes(std::size_t value) noexcept {
        receivedBytes_ = value;
    }

    [[nodiscard]] bool addReceivedBytes(std::size_t value) noexcept {
        if (value > std::numeric_limits<std::size_t>::max() - receivedBytes_) {
            return false;
        }
        receivedBytes_ += value;
        return true;
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

    [[nodiscard]] bool exceedsContentLength() const noexcept {
        return hasContentLength_ && receivedBytes_ > contentLength_;
    }

    [[nodiscard]] bool lengthComplete() const noexcept {
        return !hasContentLength_ || receivedBytes_ == contentLength_;
    }

private:
    std::size_t contentLength_{0};
    std::size_t receivedBytes_{0};
    bool hasContentLength_ : 1 {false};
};

}  // namespace ruvia::detail
