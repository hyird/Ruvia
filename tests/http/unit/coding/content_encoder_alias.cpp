#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_encoder.h"

#include "poisoned_retirement_resource.h"
#include "test_harness.h"

namespace {

using ruvia::testing::poisoned_retirement_resource;

void check_encoder_alias(ruvia::testing::test_context& ruvia_ctx, ruvia::http_content_coding coding) {
    std::string input(8 * 1024 * 1024 + 37, '\0');
    std::uint64_t random = 0x2b19ab247313fc6dULL;
    for (auto& byte : input) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        byte = static_cast<char>(random);
    }

    for (const std::size_t input_offset : {std::size_t{0}, std::size_t{23}}) {
        poisoned_retirement_resource resource;
        {
            std::pmr::string output(input_offset, 'p', &resource);
            output.append(input);
            const auto padding_offset = output.size();
            output.resize(output.capacity(), '#');
            const auto prefix_size = output.size();
            const auto* original_storage = output.data();
            {
                ruvia::http_content_encoder encoder(coding, &resource);
                encoder.write(std::string_view(output).substr(input_offset, input.size()), output, true);
                encoder.finish(output);
            }
            RUVIA_CHECK(resource.retired(original_storage));
            RUVIA_CHECK_EQ(std::string_view(output).substr(0, input_offset), std::string(input_offset, 'p'));
            RUVIA_CHECK_EQ(std::string_view(output).substr(input_offset, input.size()), std::string_view(input));
            RUVIA_CHECK_EQ(std::string_view(output).substr(padding_offset, prefix_size - padding_offset),
                std::string(prefix_size - padding_offset, '#'));

            const auto decoded = ruvia::decode_http_content(coding,
                std::string_view(output).substr(prefix_size),
                {.max_decoded_bytes_ = input.size(), .resource_ = &resource});
            RUVIA_CHECK(decoded.decoded() != nullptr);
            if (const auto* content = decoded.decoded()) {
                RUVIA_CHECK_EQ(content->bytes(), std::string_view(input));
            }
        }
        RUVIA_CHECK_EQ(resource.live_bytes(), std::size_t{0});
    }
}

}  // namespace

RUVIA_TEST(http_content_encoder_identity_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia_ctx, ruvia::http_content_coding::identity);
}

RUVIA_TEST(http_content_encoder_gzip_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia_ctx, ruvia::http_content_coding::gzip);
}

RUVIA_TEST(http_content_encoder_deflate_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia_ctx, ruvia::http_content_coding::deflate);
}

RUVIA_TEST(http_content_encoder_brotli_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia_ctx, ruvia::http_content_coding::brotli);
}

RUVIA_TEST(http_content_encoder_zstd_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia_ctx, ruvia::http_content_coding::zstd);
}
