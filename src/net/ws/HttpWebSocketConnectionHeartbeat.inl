#pragma once

namespace ruvia::detail {

template <typename Stream>
void WebSocketConnection<Stream>::completeBackgroundWrite() noexcept {
    if (backgroundWriteCount_ > 0) {
        --backgroundWriteCount_;
    }
    std::error_code ignored;
    backgroundWriteTimer_.cancel(ignored);
}

template <typename Stream>
bool WebSocketConnection<Stream>::heartbeatTick(std::int64_t now) noexcept {
    switch (webSocketHeartbeatDecision(
        heartbeatOptions_,
        closeSent_,
        awaitingPong_,
        writeActive_,
        scannerEntry_.lastActiveMs,
        heartbeatPingSentMs_,
        now)) {
        case WebSocketHeartbeatDecision::kIdle:
            return false;
        case WebSocketHeartbeatDecision::kTimeout:
            return true;
        case WebSocketHeartbeatDecision::kSendPing:
            break;
    }

    awaitingPong_ = true;
    heartbeatPingSentMs_ = now;
    writeActive_ = true;
    heartbeatWriteActive_ = true;
    ++backgroundWriteCount_;
    try {
        static constexpr std::array<unsigned char, 2> kHeartbeatPingFrame{
            static_cast<unsigned char>(0x80U | static_cast<std::uint8_t>(WebSocketOpcode::kPing)),
            0U};
        asio::async_write(stream_, asio::buffer(kHeartbeatPingFrame), [this](std::error_code ec, std::size_t) noexcept {
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

}  // namespace ruvia::detail
