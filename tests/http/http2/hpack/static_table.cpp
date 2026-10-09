#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/hpack.h"

#include "test_harness.h"

RUVIA_TEST(hpack_encoder_uses_canonical_wire_indices_for_every_static_entry) {
    ruvia::hpack_decoder decoder;
    // RFC 7541 Appendix A defines the 61 one-based static table indices.
    for (std::uint32_t index = 1; index <= 61; ++index) {
        const char indexed_field = static_cast<char>(0x80U | index);
        std::pmr::string encoded(std::pmr::get_default_resource());
        std::size_t fields_value = 0;
        const auto decoded = decoder.decode(std::string_view(&indexed_field, 1),
            [&](std::string_view name, std::string_view value) {
                ++fields_value;
                ruvia::hpack_encoder::encode_header(encoded, name, value);
                return true;
            });
        RUVIA_CHECK_EQ(encoded.size(), std::size_t{1});
        RUVIA_CHECK_EQ(encoded, std::string_view(&indexed_field, 1));
        RUVIA_CHECK(decoded.decoded());
        RUVIA_CHECK_EQ(fields_value, std::size_t{1});
    }
}
