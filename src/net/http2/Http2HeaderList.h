#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "../../http/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {

struct Http2StoredHeaderView final {
    std::string_view name;
    std::string_view value;
    RequestHeaderKind kind{RequestHeaderKind::kOther};
};

class Http2HeaderList final {
public:
    explicit Http2HeaderList(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : overflowStorage_(resource), overflowFields_(resource) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    [[nodiscard]] bool full() const noexcept {
        return count_ == kMaxRequestHeaders;
    }

    [[nodiscard]] Http2StoredHeaderView at(std::size_t index) const noexcept {
        const auto& field = index < kInlineHeaderFields
            ? inlineFields_[index]
            : overflowFields_[index - kInlineHeaderFields];
        return Http2StoredHeaderView{
            .name = view(field.nameOffset, field.nameSize),
            .value = view(field.valueOffset, field.valueSize),
            .kind = field.kind};
    }

    [[nodiscard]] bool append(
        std::string_view name,
        std::string_view value,
        RequestHeaderKind kind) {
        if (full() ||
            name.size() > kMaxStoredHeaderViewSize ||
            value.size() > kMaxStoredHeaderViewSize ||
            storageSize_ > kMaxStoredHeaderViewSize - name.size() ||
            storageSize_ + name.size() > kMaxStoredHeaderViewSize - value.size()) {
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

        const auto field = HeaderField{
            .nameOffset = nameOffset,
            .nameSize = static_cast<std::uint32_t>(name.size()),
            .valueOffset = valueOffset,
            .valueSize = static_cast<std::uint32_t>(value.size()),
            .kind = kind};

        if (count_ < kInlineHeaderFields) {
            inlineFields_[count_] = field;
        } else {
            if (overflowFields_.empty()) {
                overflowFields_.reserve(kMaxRequestHeaders - kInlineHeaderFields);
            }
            overflowFields_.push_back(field);
        }
        ++count_;
        return true;
    }

private:
    static constexpr std::size_t kInlineHeaderFields = 16;
    static constexpr std::size_t kInlineHeaderStorageBytes = 512;
    static constexpr std::size_t kMaxStoredHeaderViewSize =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

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
