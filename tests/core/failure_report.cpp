#include "ruvia/core/failure_report.h"

#include <gtest/gtest.h>

#include <exception>
#include <stdexcept>
#include <string>

TEST(Ruvia, equivalent_failures_compare_type_and_message) {
    const auto first = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    const auto same = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    // Same length, different bytes: a dangling what() comparison could see the
    // same freed storage for both and call them equal.
    const auto other = std::make_exception_ptr(std::runtime_error("redis connect timed in!"));
    const auto other_type = std::make_exception_ptr(std::logic_error("redis connect timed out"));
    const auto non_standard = std::make_exception_ptr(42);

    EXPECT_TRUE(ruvia::equivalent_failures(first, first));
    EXPECT_TRUE(ruvia::equivalent_failures(first, same));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, other));
    EXPECT_TRUE(!ruvia::equivalent_failures(other, first));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, other_type));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, non_standard));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, nullptr));
    EXPECT_TRUE(ruvia::equivalent_failures(non_standard, non_standard));
}

TEST(Ruvia, equivalent_failures_distinguish_long_messages) {
    // Messages too long for any small-buffer storage, differing only at the end.
    const std::string prefix(512, 'x');
    const auto left = std::make_exception_ptr(std::runtime_error(prefix + "a"));
    const auto right = std::make_exception_ptr(std::runtime_error(prefix + "b"));
    EXPECT_TRUE(!ruvia::equivalent_failures(left, right));
    EXPECT_TRUE(ruvia::equivalent_failures(left, std::make_exception_ptr(std::runtime_error(prefix + "a"))));
}

#include <cstddef>
#include <string_view>
#include <vector>

TEST(Ruvia, byte_views_preserve_octets_including_embedded_nul) {
    const char raw[] = {'a', '\0', '\xff'};
    const auto text = std::string_view(raw, sizeof(raw));
    const auto bytes_value = ruvia::as_bytes(text);
    EXPECT_EQ(bytes_value.size(), std::size_t{3});
    EXPECT_TRUE(bytes_value[0] == std::byte{'a'});
    EXPECT_TRUE(bytes_value[1] == std::byte{0});
    EXPECT_TRUE(bytes_value[2] == std::byte{0xff});
    EXPECT_TRUE(ruvia::as_chars(bytes_value).data() == text.data());
    EXPECT_EQ(ruvia::as_chars(bytes_value).size(), text.size());
}

TEST(Ruvia, empty_byte_views_round_trip) {
    EXPECT_TRUE(ruvia::as_bytes({}).empty());
    EXPECT_TRUE(ruvia::as_chars({}).empty());
}

TEST(Ruvia, byte_views_accept_named_owners_and_preserve_storage_addresses) {
    const std::string owned_text("abc", 3);
    const std::vector<std::byte> owned_bytes{std::byte{'x'}, std::byte{0}, std::byte{'z'}};
    const auto text_bytes = ruvia::as_bytes(owned_text);
    const auto chars_value = ruvia::as_chars(owned_bytes);
    EXPECT_TRUE(reinterpret_cast<const char*>(text_bytes.data()) == owned_text.data());
    EXPECT_TRUE(chars_value.data() == reinterpret_cast<const char*>(owned_bytes.data()));
    EXPECT_EQ(chars_value, std::string_view("x\0z", 3));
}

#include "ruvia/core/socket.h"

TEST(Ruvia, ip_address_parsing_handles_ipv4_ipv6_and_view_bounds) {
    const auto v4 = ruvia::parse_ip_address("192.0.2.1");
    EXPECT_TRUE(v4.has_value() && v4->is_v4());
    const auto v6 = ruvia::parse_ip_address("2001:0db8:1234:5678:90ab:cdef:1234:5678");
    EXPECT_TRUE(v6.has_value() && v6->is_v6());
    const auto scoped = ruvia::parse_ip_address("fe80::1%12");
    EXPECT_TRUE(scoped.has_value() && scoped->is_v6() && scoped->to_v6().scope_id() == 12);
    constexpr std::string_view storage = "192.0.2.1suffix";
    EXPECT_TRUE(ruvia::parse_ip_address(storage.substr(0, 9)).has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address(storage).has_value());
}

TEST(Ruvia, ip_address_parsing_rejects_malformed_and_embedded_nul_input) {
    EXPECT_TRUE(!ruvia::parse_ip_address(std::string_view{}).has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address("not an address").has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address(std::string_view("192.0.2.1\0suffix", 16)).has_value());
}

TEST(Ruvia, ip_address_parsing_rejects_ports_brackets_and_partial_or_padded_ipv4) {
    for (const std::string_view text : {"192.0.2.1:80", "[::1]:80", "[::1]", "192.0.2", "01.2.3.4", "192.0.2.256",
             "192.0.2.1.", ".192.0.2.1", "192.0.2.1%1", "fe80::1%", "fe80::1%1%2", "::1 ", "1",
             "::ffff:01.2.3.4", "::ffff:192.0.2", "fe80::1%12:80", "fe80::1%12 ", "fe80::1%1]:80"}) {
        EXPECT_TRUE(!ruvia::parse_ip_address(text).has_value());
    }
    EXPECT_TRUE(ruvia::parse_ip_address("0.0.0.0").has_value());
    EXPECT_TRUE(ruvia::parse_ip_address("255.255.255.255").has_value());
    const auto mapped = ruvia::parse_ip_address("::ffff:192.0.2.1");
    EXPECT_TRUE(mapped.has_value() && mapped->is_v6());
}

TEST(Ruvia, ip_address_parsing_handles_long_input_storage) {
    const auto result_value = ruvia::parse_ip_address("fe80::1%" + std::string(1024, 'x'));
    // An unknown zone name is a platform decision; the storage must round-trip either way.
    EXPECT_TRUE(!result_value.has_value() || result_value->is_v6());
}

TEST(Ruvia, ip_address_parsing_rejects_invalid_ipv6_zone_identifiers) {
    for (const std::string_view text : {
             "fe80::1%0",
             "fe80::1%01",
             "fe80::1%-1",
             "fe80::1%+1",
             "fe80::1%12abc",
             "fe80::1%4294967296",
             "fe80::1%99999999999999999999",
             "fe80::1%nonexistent_ruvia_zone_id",
         }) {
        EXPECT_TRUE(!ruvia::parse_ip_address(text).has_value());
    }
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <system_error>
#include <thread>

#include <asio/error.hpp>

#include "ruvia/core/task.h"

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

TEST(Ruvia, error_code_text_keeps_portable_category_messages) {
    const auto timed_out = std::make_error_code(std::errc::timed_out);
    EXPECT_EQ(std::string_view(ruvia::error_code_text(timed_out)), timed_out.message());

    const std::system_error generic(timed_out, "upstream");
    EXPECT_EQ(ruvia::system_error_text(generic), std::string_view(generic.what()));
}

TEST(Ruvia, error_code_text_describes_system_codes_in_utf8) {
    for (const auto& error : {std::error_code(1, std::system_category()),
             std::error_code(0x7FFFFFF0, std::system_category()),
             std::error_code(asio::error::connection_reset), std::error_code(asio::error::host_not_found)}) {
        const auto text = ruvia::error_code_text(error);
        EXPECT_TRUE(!text.empty());
        EXPECT_TRUE(is_utf8(text));
    }

    // The context before the OS message is kept.
    const std::system_error bind(std::error_code(1, std::system_category()), "bind");
    const auto text = ruvia::system_error_text(bind);
    EXPECT_TRUE(text.starts_with("bind: "));
    EXPECT_TRUE(text.size() > std::string_view("bind: ").size());
    EXPECT_TRUE(is_utf8(text));
}

namespace {

struct alignas(4096) retained_process_value final {
    std::array<std::byte, 4096> bytes_;
};

}  // namespace

TEST(Ruvia, collect_thread_heap_preserves_live_storage) {
    std::string retained(30000, 'r');
    const auto* const retained_address = retained.data();
    std::vector<std::byte> small(17, std::byte{0x33});
    {
        std::vector<std::byte> scratch(1024, std::byte{0xaa});
        (void)scratch;
    }

    ruvia::collect_thread_heap();
    ruvia::collect_thread_heap();

    EXPECT_TRUE(retained.data() == retained_address);
    EXPECT_EQ(retained.size(), std::size_t{30000});
    EXPECT_EQ(retained.find_first_not_of('r'), std::string::npos);
    EXPECT_TRUE(std::ranges::all_of(small, [](std::byte value) {
        return value == std::byte{0x33};
    }));
}

TEST(Ruvia, storage_survives_its_allocating_thread) {
    constexpr std::size_t text_bytes = 2 * 1024 * 1024;
    std::optional<std::string> text;
    std::optional<std::vector<retained_process_value>> values;
    std::thread([&] {
        text.emplace(text_bytes, 's');
        values.emplace(3);
        for (std::size_t index = 0; index < values->size(); ++index) {
            (*values)[index].bytes_.fill(static_cast<std::byte>(index + 1));
        }
    }).join();

    for (std::size_t index = 0; index < values->size(); ++index) {
        const auto& value = (*values)[index];
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&value) % alignof(retained_process_value), std::uintptr_t{0});
        EXPECT_TRUE(std::ranges::all_of(value.bytes_, [index](std::byte byte) {
            return byte == static_cast<std::byte>(index + 1);
        }));
    }
    EXPECT_EQ(text->size(), text_bytes);
    EXPECT_EQ(text->find_first_not_of('s'), std::string::npos);

    bool preserved_after_growth = false;
    std::thread([&] {
        values->reserve(values->capacity() + 1);
        text->append(text->capacity() + 1, 't');
        preserved_after_growth = text->find_first_not_of('s') == text_bytes &&
                                 text->find_first_not_of('t', text_bytes) == std::string::npos;
        for (std::size_t index = 0; index < values->size(); ++index) {
            preserved_after_growth = preserved_after_growth &&
                                     std::ranges::all_of((*values)[index].bytes_, [index](std::byte byte) {
                                         return byte == static_cast<std::byte>(index + 1);
                                     });
        }
        text.reset();
        values.reset();
    }).join();
    EXPECT_TRUE(preserved_after_growth);
}
