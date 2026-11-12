#include "ruvia/http/url_encoding.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "failing_memory_resource.h"

using ruvia::test::failing_memory_resource;

namespace {
using ruvia::decode_url_component;
using ruvia::has_url_encoding;
using mode_type = ruvia::url_decode_mode;

std::optional<std::pmr::string> url_decode(std::string_view in, ruvia::url_decode_mode mode) {
    std::pmr::string storage;
    const auto decoded = ruvia::decode_url_component(in, storage, mode);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return std::pmr::string(*decoded);
}

}  // namespace

// Percent-encoding: decoding a component or a form field, validating one, and walking encoded
// pairs.

TEST(Ruvia, url_decode_percent) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("hello", m_type::percent).value(), std::pmr::string("hello"));
    EXPECT_EQ(url_decode("%41%42%43", m_type::percent).value(), std::pmr::string("ABC"));
    EXPECT_EQ(url_decode("a%2Fb", m_type::percent).value(), std::pmr::string("a/b"));
    EXPECT_EQ(url_decode("%00", m_type::percent).value(), std::pmr::string(1, '\0'));
    // '+' is literal in percent mode
    EXPECT_EQ(url_decode("a+b", m_type::percent).value(), std::pmr::string("a+b"));
}

TEST(Ruvia, url_decode_form) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("a+b", m_type::form).value(), std::pmr::string("a b"));
    EXPECT_EQ(url_decode("a+b%20c", m_type::form).value(), std::pmr::string("a b c"));
}

TEST(Ruvia, percent_byte_decoding_rejects_out_of_range_and_truncated_triplets) {
    EXPECT_TRUE(ruvia::http_try_decode_percent_byte("a%2Fb", 1) == std::optional<unsigned char>('/'));
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", std::string_view::npos).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", 3).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%2", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%G0", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("x20", 0).has_value());
}

TEST(Ruvia, percent_byte_append_allocation_failure_preserves_output) {
    for (std::size_t spare = 0; spare != 3; ++spare) {
        SCOPED_TRACE(spare);
        failing_memory_resource resource;
        {
            std::pmr::string output(&resource);
            output.reserve(128);
            output.assign(output.capacity() - spare, 'x');
            const std::pmr::string original(output);
            resource.fail_after(0);
            EXPECT_THROW(ruvia::http_append_percent_byte(output, 0xaf), std::bad_alloc);
            EXPECT_EQ(output, std::string_view(original));
            resource.allow_allocations();
            ruvia::http_append_percent_byte(output, 0xaf);
            EXPECT_EQ(output, std::string_view(original + "%AF"));
        }
        EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, url_decode_borrows_unchanged_input_and_decodes_into_storage) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string storage("keep", &resource);
    constexpr std::string_view plain = "value";
    const auto borrowed = ruvia::decode_url_component(plain, storage);
    EXPECT_TRUE(borrowed.has_value() && borrowed->data() == plain.data() && borrowed->size() == plain.size());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    // Malformed percent encoding leaves the storage untouched.
    EXPECT_TRUE(!ruvia::decode_url_component("bad%2", storage).has_value());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    const auto decoded = ruvia::decode_url_component("value%20one", storage);
    EXPECT_TRUE(decoded.has_value() && decoded->data() == storage.data());
    EXPECT_EQ(std::string_view(storage), std::string_view("value one"));
    EXPECT_EQ(storage.get_allocator().resource(), static_cast<std::pmr::memory_resource*>(&resource));

    // The input may be the storage itself.
    storage.assign("a%2Bb+c");
    const auto aliased = ruvia::decode_url_component(storage, storage, ruvia::url_decode_mode::form);
    EXPECT_TRUE(aliased.has_value() && aliased->data() == storage.data());
    EXPECT_EQ(std::string_view(storage), std::string_view("a+b c"));
}

TEST(Ruvia, uri_reference_encoding_borrows_or_writes_storage) {
    std::pmr::string storage("keep");
    constexpr std::string_view canonical = "/a%20b?x=1#frag";
    const auto unchanged = ruvia::percent_encode_uri_reference(canonical, storage);
    EXPECT_TRUE(unchanged.data() == canonical.data() && unchanged.size() == canonical.size());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    const auto encoded = ruvia::percent_encode_uri_reference("/a b?x=%zz", storage);
    EXPECT_TRUE(encoded.data() == storage.data());
    EXPECT_EQ(encoded, std::string_view("/a%20b?x=%25zz"));

    // The reference may be the storage itself.
    storage.assign("/c d");
    EXPECT_EQ(ruvia::percent_encode_uri_reference(storage, storage), std::string_view("/c%20d"));
    EXPECT_EQ(std::string_view(storage), std::string_view("/c%20d"));
}

TEST(Ruvia, uri_reference_encoding_allocation_failure_preserves_output_and_allows_retry) {
    const std::pmr::string input = "prefix/" + std::pmr::string(1024, ' ') + "%2F?x=%yy#first#second";
    std::pmr::string expected = "prefix/";
    for (std::size_t index = 0; index != 1024; ++index) {
        expected += "%20";
    }
    expected += "%2F?x=%25yy#first%23second";
    for (int state = 0; state != 3; ++state) {
        SCOPED_TRACE(state);
        bool saw_failure = false;
        bool succeeded = false;
        for (std::size_t budget = 0; budget != 16 && !succeeded; ++budget) {
            SCOPED_TRACE(budget);
            failing_memory_resource resource;
            {
                std::pmr::string output(&resource);
                if (state == 1) {
                    output.assign(128, 'k');
                } else if (state == 2) {
                    output.assign(input);
                }
                const std::pmr::string original(output);
                const std::string_view reference = state == 2 ? std::string_view(output) : std::string_view(input);
                resource.fail_after_stl_operation(budget);
                try {
                    EXPECT_EQ(ruvia::percent_encode_uri_reference(reference, output), std::string_view(expected));
                    succeeded = true;
                } catch (const std::bad_alloc&) {
                    saw_failure = true;
                    EXPECT_EQ(output, std::string_view(original));
                }
                resource.allow_allocations();
                if (!succeeded) {
                    // Reborrow after failure so even a broken implementation cannot
                    // turn the retry check into a dangling input access.
                    const std::string_view retry = state == 2 ? std::string_view(output) : std::string_view(input);
                    EXPECT_EQ(ruvia::percent_encode_uri_reference(retry, output), std::string_view(expected));
                }
                EXPECT_EQ(output.get_allocator().resource(), &resource);
            }
            EXPECT_EQ(resource.live_allocations(), std::size_t{0});
        }
        EXPECT_TRUE(succeeded);
        EXPECT_EQ(saw_failure, ruvia::test::stl_allocation_failure_injection_enabled());
    }
}

TEST(Ruvia, uri_reference_encoding_reuses_sufficient_destination_capacity) {
    failing_memory_resource resource;
    {
        std::pmr::string output("keep", &resource);
        output.reserve(128);
        resource.fail_after_stl_operation(0);
        EXPECT_EQ(ruvia::percent_encode_uri_reference("/a b%2F?x=%yy#one#two", output),
            std::string_view("/a%20b%2F?x=%25yy#one%23two"));
        resource.allow_allocations();
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, url_decode_invalid) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_TRUE(!url_decode("%", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%4", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%zz", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("ab%2", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%g0", m_type::percent).has_value());
}

TEST(Ruvia, url_decode_form_keeps_lone_percent_literally) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("100%", m_type::form).value_or("<none>"), std::pmr::string("100%"));
    EXPECT_EQ(url_decode("%zz+%41", m_type::form).value_or("<none>"), std::pmr::string("%zz A"));
    EXPECT_EQ(url_decode("%4%41", m_type::form).value_or("<none>"), std::pmr::string("%4A"));
}

TEST(Ruvia, url_decode_all_bytes_and_malformed_suffix) {
    using m_type = ruvia::url_decode_mode;
    constexpr std::string_view hex = "0123456789aBcDeF";
    std::pmr::string encoded;
    std::pmr::string expected;
    for (unsigned int byte = 0; byte < 256; ++byte) {
        encoded.push_back('%');
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 15]);
        expected.push_back(static_cast<char>(byte));
    }
    for (const auto mode : {m_type::percent, m_type::form}) {
        const auto result_value = url_decode(encoded, mode);
        EXPECT_TRUE(result_value.has_value());
        if (result_value) {
            EXPECT_EQ(*result_value, expected);
        }
        for (const std::string_view suffix : {"%", "%0", "%xz"}) {
            const auto suffixed = url_decode(encoded + std::pmr::string(suffix), mode);
            if (mode == m_type::percent) {
                EXPECT_TRUE(!suffixed.has_value());
            } else {
                // WHATWG percent-decode keeps a '%' that starts no valid escape.
                EXPECT_TRUE(suffixed.has_value() && *suffixed == expected + std::pmr::string(suffix));
            }
        }
    }
}

TEST(Ruvia, url_validate_encoding) {
    using ruvia::validate_url_encoding;
    EXPECT_TRUE(validate_url_encoding("plain"));
    EXPECT_TRUE(validate_url_encoding("%41%42"));
    EXPECT_TRUE(!validate_url_encoding("%4"));
    EXPECT_TRUE(!validate_url_encoding("%zz"));
    EXPECT_TRUE(validate_url_encoding(""));
}

TEST(Ruvia, url_uri_paths_equivalent_and_ordered_by_canonical_form) {
    using ruvia::compare_uri_paths;
    using ruvia::uri_paths_equivalent;
    EXPECT_TRUE(uri_paths_equivalent("/users/admin", "/users/%61dmin"));
    EXPECT_TRUE(uri_paths_equivalent("/caf%C3%A9", "/caf%c3%a9"));
    EXPECT_TRUE(uri_paths_equivalent("/a:b", "/a%3Ab"));  // decoded, like route parameters
    EXPECT_TRUE(uri_paths_equivalent("", ""));
    EXPECT_TRUE(!uri_paths_equivalent("/a/b", "/a%2Fb"));  // an encoded slash is no separator
    EXPECT_TRUE(uri_paths_equivalent("/a%2fb", "/a%2Fb"));
    EXPECT_TRUE(!uri_paths_equivalent("/a+b", "/a%20b"));  // '+' is literal in a path
    EXPECT_TRUE(!uri_paths_equivalent("/api", "/apix"));
    EXPECT_TRUE(!uri_paths_equivalent("/%zz", "/%zz"));  // malformed escape
    EXPECT_TRUE(!uri_paths_equivalent("/%2", "/%2"));    // truncated escape

    EXPECT_EQ(compare_uri_paths("%61", "a"), 0);
    EXPECT_TRUE(compare_uri_paths("a", "%62") < 0);
    EXPECT_TRUE(compare_uri_paths("%62", "a") > 0);
    EXPECT_TRUE(compare_uri_paths("ab", "%61") > 0);
    EXPECT_TRUE(compare_uri_paths("a", "a%2F") < 0);
    // A separator orders after every octet, an encoded slash among them.
    EXPECT_TRUE(compare_uri_paths("a/", "a%FF") > 0);
    EXPECT_TRUE(compare_uri_paths("a%2F", "a/") < 0);
}

TEST(Ruvia, url_visit_pairs_count) {
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> pairs;
    (void)ruvia::visit_url_encoded_pairs(
        "x=1&y=2&z=3", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::pmr::string(n), std::pmr::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(3));
    if (pairs.size() == 3) {
        EXPECT_EQ(pairs[0].first, std::pmr::string("x"));
        EXPECT_EQ(pairs[2].second, std::pmr::string("3"));
    }
}

TEST(Ruvia, url_visit_pairs_stops_early) {
    std::size_t visited = 0;
    const bool completed = ruvia::visit_url_encoded_pairs("a=1&b=2&c=3",
        [&visited](std::string_view, std::string_view) {
            return ++visited < 2;
        });
    EXPECT_TRUE(!completed);
    EXPECT_EQ(visited, std::size_t(2));
}

TEST(Ruvia, url_visit_pairs_skips_empty_segments) {
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> pairs;
    // Leading, doubled, and trailing '&' produce empty segments that must NOT yield ("","") pairs.
    (void)ruvia::visit_url_encoded_pairs(
        "&a=1&&b=2&", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::pmr::string(n), std::pmr::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(2));
    if (pairs.size() == 2) {
        EXPECT_EQ(pairs[0].first, std::pmr::string("a"));
        EXPECT_EQ(pairs[1].first, std::pmr::string("b"));
    }
    // A key with an empty value ("k=") is still a real field and must be kept.
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> kept;
    (void)ruvia::visit_url_encoded_pairs("k=&=v", [&](std::string_view n, std::string_view v) {
        kept.emplace_back(std::pmr::string(n), std::pmr::string(v));
    });
    EXPECT_EQ(kept.size(), std::size_t(2));  // "k=" (name k, empty value) and "=v" (empty name, value v)
}

TEST(Ruvia, has_url_encoding_detection) {
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::percent));
    EXPECT_TRUE(!has_url_encoding("abc", mode_type::percent));
    // '+' is a literal in percent mode but triggers decoding in form mode.
    EXPECT_TRUE(!has_url_encoding("a+b", mode_type::percent));
    EXPECT_TRUE(has_url_encoding("a+b", mode_type::form));
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("plain", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::percent));
}

TEST(Ruvia, has_url_encoding_handles_binary_components) {
    std::pmr::string value(129, 'a');
    for (std::size_t position = 0; position < value.size(); ++position) {
        for (unsigned int byte = 0; byte < 256; ++byte) {
            value[position] = static_cast<char>(byte);
            EXPECT_EQ(has_url_encoding(value, mode_type::percent), byte == '%');
            EXPECT_EQ(has_url_encoding(value, mode_type::form), byte == '%' || byte == '+');
        }
        value[position] = 'a';
    }
}

TEST(Ruvia, has_url_encoding_respects_component_bounds) {
    const std::pmr::string storage = "%+" + std::pmr::string(129, 'a') + "+%";
    const auto value = std::string_view(storage).substr(2, 129);
    for (const auto mode : {mode_type::percent, mode_type::form}) {
        EXPECT_TRUE(!has_url_encoding(value, mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(0, 0), mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(value.size()), mode));
    }
    const std::string_view binary("a\0b%00c", 7);
    EXPECT_TRUE(has_url_encoding(binary, mode_type::percent));
    EXPECT_TRUE(has_url_encoding(binary, mode_type::form));
}

TEST(Ruvia, no_encoding_means_decode_borrows_the_input) {
    // Callers size their decoded-storage reservations with has_url_encoding:
    // a component it reports unencoded is returned as the input itself.
    std::pmr::string storage;
    for (const std::string_view value : {std::string_view(""), std::string_view("plain"),
             std::string_view("a/b?c=d"), std::string_view("no-plus")}) {
        for (const auto mode : {mode_type::percent, mode_type::form}) {
            EXPECT_TRUE(!has_url_encoding(value, mode));
            const auto decoded = decode_url_component(value, storage, mode);
            EXPECT_TRUE(decoded.has_value() && decoded->data() == value.data() && decoded->size() == value.size());
        }
    }
    EXPECT_TRUE(storage.empty());
}
