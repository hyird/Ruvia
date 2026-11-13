#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_encoder.h"

namespace {

void check_encoder_alias(ruvia::http_content_coding coding) {
    std::string input(8 * 1024 * 1024 + 37, '\0');
    std::uint64_t random = 0x2b19ab247313fc6dULL;
    for (auto& byte : input) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        byte = static_cast<char>(random);
    }

    for (const std::size_t input_offset : {std::size_t{0}, std::size_t{23}}) {
        {
            std::string output(input_offset, 'p');
            output.append(input);
            const auto padding_offset = output.size();
            output.resize(output.capacity(), '#');
            const auto prefix_size = output.size();
            {
                ruvia::http_content_encoder encoder(coding);
                encoder.write(std::string_view(output).substr(input_offset, input.size()), output, true);
                encoder.finish(output);
            }
            EXPECT_EQ(std::string_view(output).substr(0, input_offset), std::string(input_offset, 'p'));
            EXPECT_EQ(std::string_view(output).substr(input_offset, input.size()), std::string_view(input));
            EXPECT_EQ(std::string_view(output).substr(padding_offset, prefix_size - padding_offset), std::string(prefix_size - padding_offset, '#'));

            const auto decoded = ruvia::decode_http_content(std::array{coding},
                std::string_view(output).substr(prefix_size),
                {.max_decoded_bytes_ = input.size()});
            EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
            if (const auto* content = std::get_if<std::string>(&decoded)) {
                EXPECT_EQ(std::string_view(*content), std::string_view(input));
            }
        }
    }
}

}  // namespace

TEST(Ruvia, http_content_encoder_identity_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::identity);
}

TEST(Ruvia, http_content_encoder_gzip_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::gzip);
}

TEST(Ruvia, http_content_encoder_deflate_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::deflate);
}

TEST(Ruvia, http_content_encoder_brotli_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::brotli);
}

TEST(Ruvia, http_content_encoder_zstd_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::zstd);
}
