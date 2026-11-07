#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/hpack.h"

TEST(Ruvia, hpack_encoder_uses_canonical_wire_indices_for_every_static_entry) {
    ruvia::hpack_decoder decoder;
    // RFC 7541 Appendix A defines the 61 one-based static table indices.
    for (std::uint32_t index = 1; index <= 61; ++index) {
        const char indexed_field = static_cast<char>(0x80U | index);
        std::pmr::string encoded(std::pmr::get_default_resource());
        std::string name;
        std::size_t fields_value = 0;
        const auto decoded = decoder.decode(std::string_view(&indexed_field, 1),
            [&](std::string_view field_name, std::string_view value) {
                ++fields_value;
                name.assign(field_name);
                ruvia::hpack_encoder::encode_header(encoded, field_name, value);
                return true;
            });
        EXPECT_TRUE(!decoded.has_value());
        EXPECT_EQ(fields_value, std::size_t{1});
        // Credentials are never indexed (RFC 7541 section 7.1.3), so their
        // static entries are referenced by name only.
        if (name == "authorization" || name == "proxy-authorization" || name == "cookie" || name == "set-cookie") {
            continue;
        }
        EXPECT_EQ(encoded, std::string_view(&indexed_field, 1));
    }
}
