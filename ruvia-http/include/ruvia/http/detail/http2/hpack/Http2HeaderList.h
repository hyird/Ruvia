#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

struct Http2StoredHeaderView final {
    std::string_view name;
    std::string_view value;
    RequestHeaderKind kind{RequestHeaderKind::kOther};
};

class Http2HeaderList final {
public:
    struct Checkpoint final {
        std::size_t storageSize;
        std::size_t overflowStorageSize;
        std::size_t overflowFieldCount;
        std::uint8_t count;
        bool usingOverflowStorage;
    };

    explicit Http2HeaderList(std::pmr::memory_resource* resource = nullptr)
        : Http2HeaderList(HttpResolvedPmrResourceTag{}, httpPmrResourceOrDefault(resource)) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    [[nodiscard]] bool full() const noexcept {
        return count_ == kMaxHttpHeaderFields;
    }

    void swap(Http2HeaderList& other) noexcept {
        if (overflowStorage_.get_allocator().resource() != other.overflowStorage_.get_allocator().resource()) {
            std::terminate();
        }
        using std::swap;
        swap(inlineStorage_, other.inlineStorage_);
        overflowStorage_.swap(other.overflowStorage_);
        swap(inlineFields_, other.inlineFields_);
        overflowFields_.swap(other.overflowFields_);
        swap(storageSize_, other.storageSize_);
        swap(count_, other.count_);
        swap(usingOverflowStorage_, other.usingOverflowStorage_);
    }

    [[nodiscard]] Checkpoint checkpoint() const noexcept {
        return Checkpoint{storageSize_, overflowStorage_.size(), overflowFields_.size(), count_, usingOverflowStorage_};
    }

    void rollback(Checkpoint checkpoint) noexcept {
        if (checkpoint.storageSize > storageSize_ || checkpoint.overflowStorageSize > overflowStorage_.size() || checkpoint.overflowFieldCount > overflowFields_.size() || checkpoint.count > count_) {
            std::terminate();
        }
        overflowStorage_.resize(checkpoint.overflowStorageSize);
        overflowFields_.resize(checkpoint.overflowFieldCount);
        storageSize_ = checkpoint.storageSize;
        count_ = checkpoint.count;
        usingOverflowStorage_ = checkpoint.usingOverflowStorage;
    }

    [[nodiscard]] Http2StoredHeaderView at(std::size_t index) const& noexcept {
        const auto& field = index < kInlineHeaderFields ? inlineFields_[index] : overflowFields_[index - kInlineHeaderFields];
        return Http2StoredHeaderView{.name = view(field.nameOffset, field.nameSize), .value = view(field.valueOffset, field.valueSize), .kind = field.kind};
    }
    [[nodiscard]] Http2StoredHeaderView at(std::size_t) const&& = delete;

    [[nodiscard]] bool append(std::string_view name, std::string_view value, RequestHeaderKind kind) {
        if (full() || name.size() > kMaxStoredHeaderViewSize || value.size() > kMaxStoredHeaderViewSize || storageSize_ > kMaxStoredHeaderViewSize - name.size() || storageSize_ + name.size() > kMaxStoredHeaderViewSize - value.size()) {
            return false;
        }

        const auto fieldBytes = name.size() + value.size();
        if (!usingOverflowStorage_ && fieldBytes > kInlineHeaderStorageBytes - storageSize_) {
            ensureOverflowStorage(fieldBytes);
        }

        const auto nameOffset = static_cast<std::uint32_t>(storageSize_);
        appendBytes(name);
        const auto valueOffset = static_cast<std::uint32_t>(storageSize_);
        appendBytes(value);

        const auto field = HeaderField{.nameOffset = nameOffset, .nameSize = static_cast<std::uint32_t>(name.size()), .valueOffset = valueOffset, .valueSize = static_cast<std::uint32_t>(value.size()), .kind = kind};

        if (count_ < kInlineHeaderFields) {
            inlineFields_[count_] = field;
        } else {
            if (overflowFields_.empty()) {
                overflowFields_.reserve(kMaxHttpHeaderFields - kInlineHeaderFields);
            }
            overflowFields_.push_back(field);
        }
        ++count_;
        return true;
    }

private:
    Http2HeaderList(HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : overflowStorage_(resource),
          overflowFields_(resource) {}

    static constexpr std::size_t kInlineHeaderFields = 16;
    static constexpr std::size_t kInlineHeaderStorageBytes = 512;
    static constexpr std::size_t kMaxStoredHeaderViewSize = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

    struct HeaderField final {
        std::uint32_t nameOffset{0};
        std::uint32_t nameSize{0};
        std::uint32_t valueOffset{0};
        std::uint32_t valueSize{0};
        RequestHeaderKind kind{RequestHeaderKind::kOther};
    };

    [[nodiscard]] std::string_view view(std::uint32_t offset, std::uint32_t size) const noexcept {
        return std::string_view(storageData() + offset, size);
    }

    [[nodiscard]] const char* storageData() const noexcept {
        return usingOverflowStorage_ ? overflowStorage_.data() : inlineStorage_.data();
    }

    void appendBytes(std::string_view value) {
        if (value.empty()) {
            return;
        }
        if (!usingOverflowStorage_) {
            std::memcpy(inlineStorage_.data() + storageSize_, value.data(), value.size());
            storageSize_ += value.size();
            return;
        }
        overflowStorage_.append(value.data(), value.size());
        storageSize_ += value.size();
    }

    void ensureOverflowStorage(std::size_t additionalBytes) {
        if (usingOverflowStorage_) {
            return;
        }
        overflowStorage_.reserve(storageSize_ + additionalBytes);
        overflowStorage_.append(inlineStorage_.data(), storageSize_);
        usingOverflowStorage_ = true;
    }

    std::array<char, kInlineHeaderStorageBytes> inlineStorage_{};
    std::pmr::string overflowStorage_;
    std::array<HeaderField, kInlineHeaderFields> inlineFields_{};
    std::pmr::vector<HeaderField> overflowFields_;
    std::size_t storageSize_{0};
    std::uint8_t count_{0};
    bool usingOverflowStorage_{false};
};

}  // namespace ruvia::detail
