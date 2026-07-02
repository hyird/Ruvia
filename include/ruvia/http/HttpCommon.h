#pragma once

#include "ruvia/memory/PmrResource.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia {

enum class HttpMethod {
    kGet,
    kPost,
    kPut,
    kDelete,
    kPatch,
    kHead,
    kOptions,
    kConnect,
    kUnknown
};

inline constexpr HttpMethod Get = HttpMethod::kGet;
inline constexpr HttpMethod Post = HttpMethod::kPost;
inline constexpr HttpMethod Put = HttpMethod::kPut;
inline constexpr HttpMethod Delete = HttpMethod::kDelete;
inline constexpr HttpMethod Patch = HttpMethod::kPatch;
inline constexpr HttpMethod Head = HttpMethod::kHead;
inline constexpr HttpMethod Options = HttpMethod::kOptions;
inline constexpr HttpMethod Connect = HttpMethod::kConnect;

enum class RequestBodyMode {
    kBuffered,
    kStream
};

enum class ResponseBodyMode {
    kBuffered,
    kStream,
    kSse,
    // The handler returns an HttpResponse but may instead stream via the
    // response writer; whichever it does is honored at runtime (content
    // negotiation, e.g. buffered JSON vs an SSE stream on one route).
    kDynamic,
    kWebSocket
};

inline constexpr std::size_t kMaxRequestHeaders = 64;

inline constexpr std::size_t kMaxRouteParams = 16;

struct HttpHeaderView {
    std::string_view name;
    std::string_view value;
};

struct MultipartPart {
    std::string_view name;
    std::string_view filename;
    std::string_view contentType;
    std::string_view body;
};

struct RequestNameValueView final {
    std::string_view name;
    std::string_view value;
};

class RequestNameValueList final {
public:
    using value_type = RequestNameValueView;
    using iterator = std::pmr::vector<RequestNameValueView>::iterator;
    using const_iterator = std::pmr::vector<RequestNameValueView>::const_iterator;

    explicit RequestNameValueList(std::pmr::memory_resource* resource = nullptr)
        : items_(detail::pmrResourceOrDefault(resource)) {}

    RequestNameValueList(const RequestNameValueList&) = delete;
    RequestNameValueList& operator=(const RequestNameValueList&) = delete;
    RequestNameValueList(RequestNameValueList&&) noexcept = default;
    RequestNameValueList& operator=(RequestNameValueList&&) noexcept = default;

    [[nodiscard]] iterator begin() noexcept {
        return items_.begin();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return items_.begin();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return items_.cbegin();
    }

    [[nodiscard]] iterator end() noexcept {
        return items_.end();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return items_.end();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return items_.cend();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] RequestNameValueView* data() noexcept {
        return items_.data();
    }

    [[nodiscard]] const RequestNameValueView* data() const noexcept {
        return items_.data();
    }

    [[nodiscard]] RequestNameValueView& operator[](std::size_t index) noexcept {
        return items_[index];
    }

    [[nodiscard]] const RequestNameValueView& operator[](std::size_t index) const noexcept {
        return items_[index];
    }

    [[nodiscard]] std::string_view operator[](std::string_view name) const noexcept {
        return get(name).value_or(std::string_view{});
    }

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (it->name == name) {
                return it->value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool has(std::string_view name) const noexcept {
        return get(name).has_value();
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        std::size_t result = 0;
        for (const auto& item : items_) {
            if (item.name == name) {
                ++result;
            }
        }
        return result;
    }

    [[nodiscard]] std::pmr::vector<std::string_view> values(std::string_view name) const {
        std::pmr::vector<std::string_view> result(items_.get_allocator().resource());
        result.reserve(count(name));
        for (const auto& item : items_) {
            if (item.name == name) {
                result.push_back(item.value);
            }
        }
        return result;
    }

    [[nodiscard]] std::pmr::vector<std::string_view> getAll(std::string_view name) const {
        return values(name);
    }

    [[nodiscard]] std::span<const RequestNameValueView> entries() const noexcept {
        return span();
    }

    [[nodiscard]] std::pmr::vector<std::string_view> keys() const {
        std::pmr::vector<std::string_view> result(items_.get_allocator().resource());
        result.reserve(items_.size());
        for (const auto& item : items_) {
            result.push_back(item.name);
        }
        return result;
    }

    [[nodiscard]] std::pmr::vector<std::string_view> values() const {
        std::pmr::vector<std::string_view> result(items_.get_allocator().resource());
        result.reserve(items_.size());
        for (const auto& item : items_) {
            result.push_back(item.value);
        }
        return result;
    }

    [[nodiscard]] std::span<const RequestNameValueView> span() const noexcept {
        return std::span<const RequestNameValueView>(items_.data(), items_.size());
    }

    void reserve(std::size_t count) {
        items_.reserve(count);
    }

    void push_back(RequestNameValueView value) {
        items_.push_back(value);
    }

    template <typename... Args>
    RequestNameValueView& emplace_back(Args&&... args) {
        return items_.emplace_back(std::forward<Args>(args)...);
    }

private:
    std::pmr::vector<RequestNameValueView> items_;
};

class RequestValueGroup final {
public:
    RequestValueGroup(std::pmr::memory_resource* resource, std::string_view name)
        : name_(name),
          values_(detail::pmrResourceOrDefault(resource)) {}

    RequestValueGroup(const RequestValueGroup&) = delete;
    RequestValueGroup& operator=(const RequestValueGroup&) = delete;
    RequestValueGroup(RequestValueGroup&&) noexcept = default;
    RequestValueGroup& operator=(RequestValueGroup&&) noexcept = default;

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::span<const std::string_view> values() const noexcept {
        return std::span<const std::string_view>(values_.data(), values_.size());
    }

    [[nodiscard]] std::optional<std::string_view> first() const noexcept {
        if (values_.empty()) {
            return std::nullopt;
        }
        return values_.front();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

    void add(std::string_view value) {
        values_.push_back(value);
    }

private:
    std::string_view name_;
    std::pmr::vector<std::string_view> values_;
};

class RequestValueGroupList final {
public:
    using value_type = RequestValueGroup;
    using iterator = std::pmr::vector<RequestValueGroup>::iterator;
    using const_iterator = std::pmr::vector<RequestValueGroup>::const_iterator;

    explicit RequestValueGroupList(std::pmr::memory_resource* resource = nullptr)
        : groups_(detail::pmrResourceOrDefault(resource)) {}

    RequestValueGroupList(const RequestValueGroupList&) = delete;
    RequestValueGroupList& operator=(const RequestValueGroupList&) = delete;
    RequestValueGroupList(RequestValueGroupList&&) noexcept = default;
    RequestValueGroupList& operator=(RequestValueGroupList&&) noexcept = default;

    [[nodiscard]] iterator begin() noexcept {
        return groups_.begin();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return groups_.begin();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return groups_.cbegin();
    }

    [[nodiscard]] iterator end() noexcept {
        return groups_.end();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return groups_.end();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return groups_.cend();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return groups_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return groups_.empty();
    }

    [[nodiscard]] RequestValueGroup* data() noexcept {
        return groups_.data();
    }

    [[nodiscard]] const RequestValueGroup* data() const noexcept {
        return groups_.data();
    }

    [[nodiscard]] RequestValueGroup& operator[](std::size_t index) noexcept {
        return groups_[index];
    }

    [[nodiscard]] const RequestValueGroup& operator[](std::size_t index) const noexcept {
        return groups_[index];
    }

    [[nodiscard]] std::span<const std::string_view> operator[](std::string_view name) const noexcept {
        return values(name);
    }

    [[nodiscard]] const RequestValueGroup* group(std::string_view name) const noexcept {
        for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
            if (it->name() == name) {
                return &*it;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<std::span<const std::string_view>> get(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        if (requestGroup == nullptr) {
            return std::nullopt;
        }
        return requestGroup->values();
    }

    [[nodiscard]] bool has(std::string_view name) const noexcept {
        return group(name) != nullptr;
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        return requestGroup == nullptr ? 0 : requestGroup->size();
    }

    [[nodiscard]] std::optional<std::string_view> first(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        return requestGroup == nullptr ? std::nullopt : requestGroup->first();
    }

    [[nodiscard]] std::span<const std::string_view> values(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        if (requestGroup == nullptr) {
            return {};
        }
        return requestGroup->values();
    }

    [[nodiscard]] std::span<const std::string_view> getAll(std::string_view name) const noexcept {
        return values(name);
    }

    [[nodiscard]] std::span<const RequestValueGroup> entries() const noexcept {
        return span();
    }

    [[nodiscard]] std::pmr::vector<std::string_view> keys() const {
        std::pmr::vector<std::string_view> result(groups_.get_allocator().resource());
        result.reserve(groups_.size());
        for (const auto& group : groups_) {
            result.push_back(group.name());
        }
        return result;
    }

    [[nodiscard]] std::pmr::vector<std::span<const std::string_view>> values() const {
        std::pmr::vector<std::span<const std::string_view>> result(groups_.get_allocator().resource());
        result.reserve(groups_.size());
        for (const auto& group : groups_) {
            result.push_back(group.values());
        }
        return result;
    }

    [[nodiscard]] std::span<const RequestValueGroup> span() const noexcept {
        return std::span<const RequestValueGroup>(groups_.data(), groups_.size());
    }

    void reserve(std::size_t count) {
        groups_.reserve(count);
    }

    void push_back(RequestValueGroup value) {
        groups_.push_back(std::move(value));
    }

    template <typename... Args>
    RequestValueGroup& emplace_back(Args&&... args) {
        return groups_.emplace_back(std::forward<Args>(args)...);
    }

private:
    std::pmr::vector<RequestValueGroup> groups_;
};

class RequestValue final {
public:
    enum class DecodeMode : std::uint8_t {
        kNone,
        kPercent,
        kForm
    };

    RequestValue() = default;
    RequestValue(
        std::optional<std::string_view> value,
        std::pmr::memory_resource* resource = nullptr,
        DecodeMode decodeMode = DecodeMode::kNone) noexcept
        : RequestValue(
              detail::ResolvedPmrResourceTag{},
              value,
              detail::pmrResourceOrDefault(resource),
              decodeMode) {}

    [[nodiscard]] bool exists() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return exists();
    }

    [[nodiscard]] bool has_value() const noexcept {
        return exists();
    }

    [[nodiscard]] std::string_view value_or(std::string_view fallback) const noexcept {
        return value_.has_value() ? *value_ : fallback;
    }

    [[nodiscard]] std::optional<std::string_view> toStringView() const noexcept {
        return value_;
    }

    [[nodiscard]] std::optional<std::pmr::string> toString() const;
    [[nodiscard]] std::optional<bool> toBool() const noexcept;
    [[nodiscard]] std::optional<int> toInt() const noexcept;
    [[nodiscard]] std::optional<unsigned int> toUInt() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> toInt32() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> toUInt32() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> toInt64() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> toUInt64() const noexcept;

private:
    RequestValue(
        detail::ResolvedPmrResourceTag,
        std::optional<std::string_view> value,
        std::pmr::memory_resource* resource,
        DecodeMode decodeMode) noexcept
        : value_(value),
          resource_(resource),
          decodeMode_(decodeMode) {}

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    std::optional<std::string_view> value_;
    std::pmr::memory_resource* resource_{nullptr};
    DecodeMode decodeMode_{DecodeMode::kNone};
};

using QueryValue = RequestValue;

HttpMethod parseMethod(std::string_view method);
std::string_view methodName(HttpMethod method);
[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpStatusText(std::string_view value) noexcept;

}  // namespace ruvia
