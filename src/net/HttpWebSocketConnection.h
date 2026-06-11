#pragma once

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ConnectionScanner.h"
#include "HttpWebSocketUtils.h"
#include "../AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
class WebSocketConnection final {
public:
    WebSocketConnection(
        Stream& stream,
        std::pmr::memory_resource* resource,
        ConnectionScanner::Entry& scannerEntry,
        WebSocketHeartbeatOptions heartbeatOptions,
        std::size_t maxMessageBytes,
        std::string_view initialBytes)
        : stream_(stream),
          scannerEntry_(scannerEntry),
          heartbeatOptions_(heartbeatOptions),
          maxMessageBytes_(maxMessageBytes),
          buffer_(resource == nullptr ? ProcessMemory::instance().upstreamResource() : resource),
          fragmentedMessage_(buffer_.get_allocator().resource()),
          backgroundWriteTimer_(stream.get_executor()) {
        backgroundWriteTimer_.expires_at((asio::steady_timer::time_point::max)());
        buffer_.append(initialBytes.data(), initialBytes.size());
        scannerEntry_.webSocketTarget = this;
        scannerEntry_.webSocketTick = &WebSocketConnection::heartbeatTickThunk;
    }

    ~WebSocketConnection() {
        if (scannerEntry_.webSocketTarget == this) {
            scannerEntry_.webSocketTarget = nullptr;
            scannerEntry_.webSocketTick = nullptr;
        }
    }

    [[nodiscard]] static Task<std::optional<WebSocketMessage>> readThunk(void* target) {
        return static_cast<WebSocketConnection*>(target)->read();
    }

    static Task<void> writeThunk(void* target, WebSocketOpcode opcode, std::string_view payload) {
        return static_cast<WebSocketConnection*>(target)->write(opcode, payload);
    }

    static Task<void> closeThunk(void* target, std::uint16_t code, std::string_view reason) {
        return static_cast<WebSocketConnection*>(target)->close(code, reason);
    }

    static bool heartbeatTickThunk(void* target, std::int64_t now) noexcept {
        return static_cast<WebSocketConnection*>(target)->heartbeatTick(now);
    }

    [[nodiscard]] Task<std::optional<WebSocketMessage>> read() {
        for (;;) {
            auto frame = co_await readFrame();
            if (!frame) {
                co_return std::nullopt;
            }
            if (frame->opcode == WebSocketOpcode::kPing) {
                co_await write(WebSocketOpcode::kPong, frame->payload);
                continue;
            }
            if (frame->opcode == WebSocketOpcode::kPong) {
                awaitingPong_ = false;
                continue;
            }
            if (frame->opcode == WebSocketOpcode::kClose) {
                if (!closeSent_) {
                    closeSent_ = true;
                    co_await write(WebSocketOpcode::kClose, frame->payload);
                }
                co_return std::nullopt;
            }
            if (frame->continuation) {
                if (!fragmented_) {
                    throw std::invalid_argument("unexpected websocket continuation frame");
                }
                if (maxMessageBytes_ != 0 &&
                    (frame->payload.size() > maxMessageBytes_ || fragmentedMessage_.size() > maxMessageBytes_ - frame->payload.size())) {
                    throw std::invalid_argument("websocket message is too large");
                }
                fragmentedMessage_.append(frame->payload.data(), frame->payload.size());
                if (!frame->fin) {
                    continue;
                }
                if (fragmentedOpcode_ == WebSocketOpcode::kText && !isValidUtf8(fragmentedMessage_)) {
                    co_await close(1007, "invalid utf-8");
                    co_return std::nullopt;
                }
                fragmented_ = false;
                co_return WebSocketMessage{
                    .opcode = fragmentedOpcode_,
                    .payload = std::string_view(fragmentedMessage_.data(), fragmentedMessage_.size())};
            }
            if (frame->opcode == WebSocketOpcode::kText || frame->opcode == WebSocketOpcode::kBinary) {
                if (fragmented_) {
                    throw std::invalid_argument("invalid websocket fragmented message");
                }
                if (frame->fin) {
                    if (frame->opcode == WebSocketOpcode::kText && !isValidUtf8(frame->payload)) {
                        co_await close(1007, "invalid utf-8");
                        co_return std::nullopt;
                    }
                    co_return WebSocketMessage{.opcode = frame->opcode, .payload = frame->payload};
                }
                fragmented_ = true;
                fragmentedOpcode_ = frame->opcode;
                fragmentedMessage_.assign(frame->payload.data(), frame->payload.size());
                continue;
            }
        }
    }

    Task<void> write(WebSocketOpcode opcode, std::string_view payload) {
        if (closeSent_ && opcode != WebSocketOpcode::kClose) {
            co_return;
        }
        if ((opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong || opcode == WebSocketOpcode::kClose) &&
            payload.size() > 125) {
            throw std::invalid_argument("websocket control frame is too large");
        }

        while (heartbeatWriteActive_) {
            (void)co_await asyncError([this](auto handler) mutable {
                backgroundWriteTimer_.async_wait(std::move(handler));
            });
        }

        if (writeActive_) {
            throw std::logic_error("concurrent websocket writes are not supported");
        }

        writeActive_ = true;
        try {
            co_await writeFrameNow(opcode, payload);
        } catch (...) {
            writeActive_ = false;
            throw;
        }
        writeActive_ = false;
    }

    Task<void> close(std::uint16_t code, std::string_view reason) {
        if (closeSent_) {
            co_return;
        }
        if (!isValidWebSocketCloseCode(code) || !isValidUtf8(reason)) {
            throw std::invalid_argument("invalid websocket close payload");
        }
        closeSent_ = true;
        std::pmr::string payload(buffer_.get_allocator().resource());
        payload.push_back(static_cast<char>((code >> 8) & 0xFF));
        payload.push_back(static_cast<char>(code & 0xFF));
        payload.append(reason.data(), reason.size());
        co_await write(WebSocketOpcode::kClose, payload);
    }

    Task<void> detachAndDrainBackgroundWrites() {
        if (scannerEntry_.webSocketTarget == this) {
            scannerEntry_.webSocketTarget = nullptr;
            scannerEntry_.webSocketTick = nullptr;
        }
        closeSent_ = true;
        while (backgroundWriteCount_ > 0) {
            (void)co_await asyncError([this](auto handler) mutable {
                backgroundWriteTimer_.async_wait(std::move(handler));
            });
        }
    }

private:
    struct Frame final {
        WebSocketOpcode opcode{WebSocketOpcode::kText};
        std::string_view payload;
        bool fin{true};
        bool continuation{false};
    };

    void completeBackgroundWrite() noexcept {
        if (backgroundWriteCount_ > 0) {
            --backgroundWriteCount_;
        }
        std::error_code ignored;
        backgroundWriteTimer_.cancel(ignored);
    }

    bool heartbeatTick(std::int64_t now) noexcept {
        const auto pingInterval = heartbeatOptions_.pingInterval.count();
        if (pingInterval <= 0 || closeSent_) {
            return false;
        }

        auto pongTimeout = heartbeatOptions_.pongTimeout.count();
        if (pongTimeout <= 0) {
            pongTimeout = pingInterval;
        }
        if (awaitingPong_) {
            return now - heartbeatPingSentMs_ >= pongTimeout;
        }
        if (now - scannerEntry_.lastActiveMs < pingInterval) {
            return false;
        }

        if (writeActive_) {
            return false;
        }

        awaitingPong_ = true;
        heartbeatPingSentMs_ = now;
        writeActive_ = true;
        heartbeatWriteActive_ = true;
        ++backgroundWriteCount_;
        try {
            asio::async_write(stream_, asio::buffer(heartbeatPingFrame_), [this](std::error_code ec, std::size_t) noexcept {
                if (ec) {
                    closeSent_ = true;
                } else {
                    scannerEntry_.touch();
                }
                heartbeatWriteActive_ = false;
                writeActive_ = false;
                completeBackgroundWrite();
            });
        } catch (...) {
            heartbeatWriteActive_ = false;
            writeActive_ = false;
            completeBackgroundWrite();
            return true;
        }
        return false;
    }

    Task<void> writeFrameNow(WebSocketOpcode opcode, std::string_view payload) {
        if ((opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary) &&
            maxMessageBytes_ != 0 && payload.size() > maxMessageBytes_) {
            throw std::invalid_argument("websocket message is too large");
        }
        std::array<unsigned char, 10> header{};
        std::size_t headerSize = 0;
        header[headerSize++] = static_cast<unsigned char>(0x80U | static_cast<std::uint8_t>(opcode));
        if (payload.size() <= 125) {
            header[headerSize++] = static_cast<unsigned char>(payload.size());
        } else if (payload.size() <= 0xFFFF) {
            header[headerSize++] = 126;
            header[headerSize++] = static_cast<unsigned char>((payload.size() >> 8) & 0xFF);
            header[headerSize++] = static_cast<unsigned char>(payload.size() & 0xFF);
        } else {
            header[headerSize++] = 127;
            const auto size = static_cast<std::uint64_t>(payload.size());
            for (int shift = 56; shift >= 0; shift -= 8) {
                header[headerSize++] = static_cast<unsigned char>((size >> shift) & 0xFF);
            }
        }
        std::array<asio::const_buffer, 2> buffers{
            asio::buffer(header.data(), headerSize),
            asio::buffer(payload.data(), payload.size())};
        const auto ec = co_await asyncError([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        if (ec) {
            throw std::invalid_argument("failed to write websocket frame");
        }
        scannerEntry_.touch();
    }

    Task<void> ensure(std::size_t bytes) {
        while (buffer_.size() - offset_ < bytes) {
            const auto oldSize = buffer_.size();
            buffer_.resize(oldSize + 4096);
            const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
                [this, oldSize](auto handler) mutable {
                    stream_.async_read_some(
                        asio::buffer(buffer_.data() + oldSize, buffer_.size() - oldSize),
                        std::move(handler));
                });
            if (ec || bytesRead == 0) {
                buffer_.resize(oldSize);
                throw std::invalid_argument("websocket connection closed");
            }
            buffer_.resize(oldSize + bytesRead);
            scannerEntry_.touch();
        }
    }

    void compactConsumedFrame() {
        if (pendingCompactUntil_ == 0) {
            return;
        }
        if (pendingCompactUntil_ >= buffer_.size()) {
            buffer_.clear();
        } else {
            buffer_.erase(0, pendingCompactUntil_);
        }
        offset_ = 0;
        pendingCompactUntil_ = 0;
    }

    [[nodiscard]] Task<std::optional<Frame>> readFrame() {
        compactConsumedFrame();
        co_await ensure(2);
        const auto first = static_cast<unsigned char>(buffer_[offset_]);
        const auto second = static_cast<unsigned char>(buffer_[offset_ + 1]);
        const bool fin = (first & 0x80U) != 0;
        const bool rsv1 = (first & 0x40U) != 0;
        const bool rsvOther = (first & 0x30U) != 0;
        const auto rawOpcode = static_cast<std::uint8_t>(first & 0x0FU);
        const bool masked = (second & 0x80U) != 0;
        std::uint64_t length = second & 0x7FU;
        std::size_t headerSize = 2;

        if (rsv1 || rsvOther || !masked || (rawOpcode >= 0x3 && rawOpcode <= 0x7) || rawOpcode >= 0xB) {
            throw std::invalid_argument("invalid websocket frame");
        }
        if (length == 126) {
            co_await ensure(headerSize + 2);
            length = (static_cast<std::uint64_t>(static_cast<unsigned char>(buffer_[offset_ + headerSize])) << 8) |
                static_cast<unsigned char>(buffer_[offset_ + headerSize + 1]);
            headerSize += 2;
        } else if (length == 127) {
            co_await ensure(headerSize + 8);
            if ((static_cast<unsigned char>(buffer_[offset_ + headerSize]) & 0x80U) != 0) {
                throw std::invalid_argument("invalid websocket frame length");
            }
            length = 0;
            for (std::size_t i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<unsigned char>(buffer_[offset_ + headerSize + i]);
            }
            headerSize += 8;
        }
        const bool continuation = rawOpcode == 0x0;
        const auto opcode = continuation ? WebSocketOpcode::kText : static_cast<WebSocketOpcode>(rawOpcode);
        const bool control = opcode == WebSocketOpcode::kClose || opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong;
        if (control && (!fin || length > 125)) {
            throw std::invalid_argument("invalid websocket control frame");
        }
        if (maxMessageBytes_ != 0 && length > maxMessageBytes_) {
            throw std::invalid_argument("websocket message is too large");
        }
        co_await ensure(headerSize + 4 + static_cast<std::size_t>(length));

        const auto maskOffset = offset_ + headerSize;
        const auto payloadOffset = maskOffset + 4;
        const auto payloadSize = static_cast<std::size_t>(length);
        auto* payload = buffer_.data() + payloadOffset;
        for (std::size_t i = 0; i < payloadSize; ++i) {
            payload[i] = static_cast<char>(
                static_cast<unsigned char>(payload[i]) ^
                static_cast<unsigned char>(buffer_[maskOffset + (i % 4)]));
        }
        const auto payloadView = std::string_view(payload, payloadSize);
        if (opcode == WebSocketOpcode::kClose) {
            if (payloadView.size() == 1) {
                throw std::invalid_argument("invalid websocket close payload");
            }
            if (payloadView.size() >= 2) {
                const auto code = static_cast<std::uint16_t>(
                    (static_cast<unsigned char>(payloadView[0]) << 8) |
                    static_cast<unsigned char>(payloadView[1]));
                if (!isValidWebSocketCloseCode(code) || !isValidUtf8(payloadView.substr(2))) {
                    throw std::invalid_argument("invalid websocket close payload");
                }
            }
        }
        offset_ = payloadOffset + static_cast<std::size_t>(length);
        pendingCompactUntil_ = offset_;
        co_return Frame{
            .opcode = opcode,
            .payload = payloadView,
            .fin = fin,
            .continuation = continuation};
    }

    Stream& stream_;
    ConnectionScanner::Entry& scannerEntry_;
    WebSocketHeartbeatOptions heartbeatOptions_{};
    std::size_t maxMessageBytes_{kDefaultMaxWebSocketMessageBytes};
    std::pmr::string buffer_;
    std::pmr::string fragmentedMessage_;
    asio::steady_timer backgroundWriteTimer_;
    std::array<unsigned char, 2> heartbeatPingFrame_{static_cast<unsigned char>(0x80U | static_cast<std::uint8_t>(WebSocketOpcode::kPing)), 0U};
    std::size_t offset_{0};
    std::size_t pendingCompactUntil_{0};
    std::size_t backgroundWriteCount_{0};
    bool writeActive_{false};
    bool heartbeatWriteActive_{false};
    bool closeSent_{false};
    bool awaitingPong_{false};
    std::int64_t heartbeatPingSentMs_{0};
    bool fragmented_{false};
    WebSocketOpcode fragmentedOpcode_{WebSocketOpcode::kText};
};

}  // namespace ruvia::detail
