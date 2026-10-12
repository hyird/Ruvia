#include "ruvia/core/error_text.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/error.hpp>

#include "test_harness.h"

namespace {

[[nodiscard]] bool is_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead < 0x80U) {
            length = 1;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
        } else {
            return false;
        }
        if (length > text.size() - index) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

}  // namespace

RUVIA_TEST(error_code_text_keeps_portable_category_messages) {
    const auto timed_out = std::make_error_code(std::errc::timed_out);
    RUVIA_CHECK_EQ(ruvia::error_code_text(timed_out), timed_out.message());

    const std::system_error generic(timed_out, "upstream");
    RUVIA_CHECK_EQ(ruvia::system_error_text(generic), std::string(generic.what()));
}

RUVIA_TEST(error_code_text_describes_system_codes_in_utf8) {
    for (const auto& error : {std::error_code(1, std::system_category()),
             std::error_code(0x7FFFFFF0, std::system_category()),
             std::error_code(asio::error::connection_reset), std::error_code(asio::error::host_not_found)}) {
        const auto text = ruvia::error_code_text(error);
        RUVIA_CHECK(!text.empty());
        RUVIA_CHECK(is_utf8(text));
    }

    // The context before the OS message is kept.
    const std::system_error bind(std::error_code(1, std::system_category()), "bind");
    const auto text = ruvia::system_error_text(bind);
    RUVIA_CHECK(text.starts_with("bind: "));
    RUVIA_CHECK(text.size() > std::string_view("bind: ").size());
    RUVIA_CHECK(is_utf8(text));
}
