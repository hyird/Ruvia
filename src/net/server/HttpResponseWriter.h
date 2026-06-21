#pragma once

#include <array>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio.hpp>

#include "HttpFileFallback.h"
#include "HttpFileZeroCopy.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

constexpr std::size_t kResponseHeadStackBytes = 512;
constexpr std::size_t kResponseHeadRetainedHeapBytes = 4 * 1024;

class ResponseHeadBuffer final {
public:
    explicit ResponseHeadBuffer(std::pmr::polymorphic_allocator<char> allocator) : heap_(allocator) {}

    void reset() noexcept;
    void append(std::string_view value);
    void append(char value);
    void appendUnsigned(std::uint64_t value);
    void reserveAdditional(std::size_t size);
    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] bool canAppendOnStack(std::size_t size) const noexcept;

    // Bulk fast path: returns a raw cursor when `bound` bytes are guaranteed to
    // fit in the stack buffer, so callers can emit without per-append checks.
    [[nodiscard]] char* stackCursor(std::size_t bound) noexcept {
        if (overflowed_ || bound > stack_.size() - used_) {
            return nullptr;
        }
        return stack_.data() + used_;
    }

    void commitStack(const char* end) noexcept {
        used_ = static_cast<std::size_t>(end - stack_.data());
    }

private:
    std::array<char, kResponseHeadStackBytes> stack_{};
    std::pmr::string heap_;
    std::size_t used_{0};
    bool overflowed_{false};
};

struct ResponseWritePolicy {
    bool bodyAllowed{true};
    bool autoContentLengthAllowed{true};
    bool explicitContentLengthAllowed{true};
    bool transferEncodingAllowed{true};
};

[[nodiscard]] ResponseWritePolicy responseWritePolicy(std::uint16_t statusCode) noexcept;
[[nodiscard]] inline bool responseBodyFramingHeaderForbidden(
    std::uint32_t knownBit,
    bool explicitContentLengthAllowed,
    bool transferEncodingAllowed) noexcept {
    return (!explicitContentLengthAllowed && knownBit == HttpResponse::kKnownHeaderContentLength) ||
        (!transferEncodingAllowed && knownBit == HttpResponse::kKnownHeaderTransferEncoding);
}

[[nodiscard]] inline bool responseHasForbiddenBodyFramingHeader(
    std::uint32_t knownBits,
    bool explicitContentLengthAllowed,
    bool transferEncodingAllowed) noexcept {
    return ((knownBits & HttpResponse::kKnownHeaderContentLength) != 0 &&
               responseBodyFramingHeaderForbidden(
                   HttpResponse::kKnownHeaderContentLength,
                   explicitContentLengthAllowed,
                   transferEncodingAllowed)) ||
        ((knownBits & HttpResponse::kKnownHeaderTransferEncoding) != 0 &&
            responseBodyFramingHeaderForbidden(
                HttpResponse::kKnownHeaderTransferEncoding,
                explicitContentLengthAllowed,
                transferEncodingAllowed));
}

[[nodiscard]] inline bool responseHttp2ConnectionHeaderForbidden(
    std::uint32_t knownBit,
    std::string_view name) noexcept {
    if (knownBit == HttpResponse::kKnownHeaderConnection ||
        knownBit == HttpResponse::kKnownHeaderTransferEncoding) {
        return true;
    }
    if (knownBit != 0) {
        return false;
    }
    return httpAsciiEqualsIgnoreCase(name, "connection") ||
        httpAsciiEqualsIgnoreCase(name, "keep-alive") ||
        httpAsciiEqualsIgnoreCase(name, "proxy-connection") ||
        httpAsciiEqualsIgnoreCase(name, "transfer-encoding") ||
        httpAsciiEqualsIgnoreCase(name, "upgrade");
}

void appendResponseHead(
    const HttpResponse& response,
    ResponseHeadBuffer& head,
    ResponseWritePolicy policy,
    bool suppressAutoContentLength = false);
template <typename Stream>
Task<void> writeResponse(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer* reusableHead,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    bool skipBody,
    std::error_code& ec) {
    ResponseHeadBuffer localHead(memory.allocator<char>());
    auto& head = reusableHead == nullptr ? localHead : *reusableHead;
    head.reset();
    const auto policy = responseWritePolicy(response.statusCode());
    appendResponseHead(response, head, policy);
    if (response.hasFileBody()) {
        const auto& fileBody = response.fileBody();
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        if (ec || skipBody || !policy.bodyAllowed || fileBody.length == 0) {
            co_return;
        }

        if constexpr (std::is_same_v<std::remove_cvref_t<Stream>, asio::ip::tcp::socket>) {
            co_await writeFileZeroCopy(stream, fileBody, ec);
            if (ec != asio::error::operation_not_supported) {
                co_return;
            }
        }

        co_await writeFileFallback(stream, memory, fileChunkBuffer, fileBody, ec);
        co_return;
    }

    const auto body = skipBody || !policy.bodyAllowed ? std::string_view{} : response.bodyBytes();
    if (body.empty()) {
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        co_return;
    }
    if (head.canAppendOnStack(body.size())) {
        head.append(body);
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        co_return;
    }
    const auto headView = head.view();
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(headView), asio::buffer(body)};
    ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
}

}  // namespace ruvia::detail
