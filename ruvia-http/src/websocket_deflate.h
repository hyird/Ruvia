#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "http_protocol.h"
#include "websocket_connection.h"

namespace ruvia::detail {

// The receive storage of one connection beyond its input buffer, charged to its
// owner's websocket_receive_storage_budget before it grows and settled on the
// capacity it actually holds. Without a budget it grows uncharged.
class websocket_receive_storage final {
public:
    enum class slot : std::uint8_t {
        reassembly,
        inflate,
    };

    explicit websocket_receive_storage(websocket_receive_storage_budget budget) noexcept :
        budget_(budget) {}

    // Grows `target` geometrically to hold `needed` bytes, keeping its
    // contents. false: the budget refused the growth, or the allocator's
    // rounding exceeded the budget and the grown storage was discarded.
    [[nodiscard]] bool reserve(slot owner, std::string& target, std::size_t needed) {
        if (needed <= target.capacity()) {
            return true;
        }
        const auto doubled = target.capacity() > target.max_size() / 2 ? target.max_size() : target.capacity() * 2;
        const auto capacity = (std::max)(needed, doubled);
        if (budget_.resize_ == nullptr) {
            target.reserve(capacity);
            return true;
        }
        if (!charge(held_total() - held(owner) + capacity)) {
            return false;
        }
        try {
            target.reserve(capacity);
        } catch (...) {
            settle(owner, target);
            throw;
        }
        held(owner) = heap_bytes(target);
        if (!charge(held_total())) {
            std::string{}.swap(target);
            settle(owner, target);
            return false;
        }
        return true;
    }

    // Reports what `target` holds after it released storage.
    void settle(slot owner, const std::string& target) noexcept {
        if (budget_.resize_ == nullptr) {
            return;
        }
        held(owner) = heap_bytes(target);
        if (held_total() != charged_) {
            // Shrinking never exceeds the budget.
            static_cast<void>(budget_.resize_(budget_.context_, held_total()));
            charged_ = held_total();
        }
    }

private:
    [[nodiscard]] static std::size_t heap_bytes(const std::string& value) noexcept {
        static const std::size_t sso_capacity = std::string{}.capacity();
        return value.capacity() > sso_capacity ? value.capacity() : 0;
    }
    [[nodiscard]] std::size_t& held(slot owner) noexcept {
        return held_[static_cast<std::size_t>(owner)];
    }
    [[nodiscard]] std::size_t held_total() const noexcept {
        return held_[0] + held_[1];
    }
    [[nodiscard]] bool charge(std::size_t total) noexcept {
        if (!budget_.resize_(budget_.context_, total)) {
            return false;
        }
        charged_ = total;
        return true;
    }

    websocket_receive_storage_budget budget_;
    std::array<std::size_t, 2> held_{};
    std::size_t charged_{0};
};

// RFC 7692 codec state of one connection. A direction negotiated without
// context takeover starts every message from an empty dictionary, so it uses
// the borrowed shared codec set when the owner supplies one and then holds no
// codec memory. Every other direction owns a private zlib stream,
// created on its first compressed message and kept,
// with its dictionary, until the connection ends. The owner validates the
// deflater (validate_websocket_deflater_config) and windows (8-15).
class websocket_deflate final {
public:
    websocket_deflate(websocket_deflate_codecs* shared, websocket_deflater_config deflater, bool send_takeover, bool receive_takeover, int send_window, int receive_window) noexcept;
    ~websocket_deflate();

    websocket_deflate(const websocket_deflate&) = delete;
    websocket_deflate& operator=(const websocket_deflate&) = delete;

    // Appends the compressed message to `out`. false reports a codec failure;
    // allocation exceptions propagate.
    bool compress(std::string_view input, std::string& out);

    // Compresses one fragment of a message sent as several frames. The whole
    // message is one DEFLATE stream, so it uses a private codec even where
    // whole messages borrow the shared one, and starts from an empty
    // dictionary at its first fragment unless context takeover applies.
    bool compress_fragment(std::string_view input, bool first, bool final, std::string& out);

    websocket_inflate_result decompress(std::string_view input, std::string& out, protocol_byte_limit message_limit, websocket_receive_storage& storage);

    // A trial that is not sent must not enter the peer's future dictionary.
    // Starting the next compressed message with an empty sender dictionary is
    // legal even when the receiver retains its prior dictionary (RFC 7692).
    // The shared codecs already reset after every message.
    void discard_compression();

private:
    websocket_deflate_codecs* shared_;
    std::unique_ptr<zlib_deflate_stream> deflater_;
    std::unique_ptr<zlib_inflate_stream> inflater_;
    websocket_deflater_config deflater_config_;
    int send_window_;
    int receive_window_;
    bool send_takeover_;
    bool receive_takeover_;
};

}  // namespace ruvia::detail
