#include <array>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

using ruvia::decode_http_content;
using ruvia::encode_http_content;
using ruvia::http_content_coding;
using ruvia::http_content_encode_context;

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t live_bytes() const noexcept {
        return live_bytes_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        live_bytes_ += bytes_value;
        return allocation;
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        live_bytes_ -= bytes_value;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
    std::size_t live_bytes_{0};
};

constexpr std::array compressing_codings{http_content_coding::gzip, http_content_coding::deflate,
    http_content_coding::brotli, http_content_coding::zstd};

// Compressible but not trivially repetitive, so encoders emit several output
// blocks for the larger sizes.
[[nodiscard]] std::string sample_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 1U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 7U) + ((i / 64U) % 3U));
    }
    return body;
}

[[nodiscard]] std::string one_shot(http_content_coding coding, std::string_view input, std::size_t cap) {
    const auto result_value = encode_http_content(coding, input, {.max_encoded_bytes_ = cap});
    return result_value.has_value() ? std::string(*result_value) : std::string();
}

[[nodiscard]] std::optional<std::string> encode_with(http_content_encode_context& context,
    std::string_view input, std::size_t cap, std::pmr::memory_resource* output_resource = nullptr) {
    auto result_value = context.encode(input, {.max_encoded_bytes_ = cap, .resource_ = output_resource});
    if (!result_value.has_value()) {
        return std::nullopt;
    }
    return std::string(*result_value);
}

}  // namespace

RUVIA_TEST(http_content_encode_context_reuse_matches_one_shot_bytes) {
    constexpr std::array sizes{std::size_t{0}, std::size_t{1}, std::size_t{1000}, std::size_t{8192},
        std::size_t{70'000}, std::size_t{300'000}};
    for (const auto coding : compressing_codings) {
        counting_resource state_resource;
        http_content_encode_context context(coding, &state_resource);
        RUVIA_CHECK(context.coding() == coding);
        unsigned seed = 0;
        // Twice through, so every size also follows a larger representation.
        for (int round = 0; round < 2; ++round) {
            for (const auto size : sizes) {
                const auto input = sample_body(size, ++seed);
                const auto expected = one_shot(coding, input, input.size() + 1024);
                const auto actual = encode_with(context, input, input.size() + 1024);
                RUVIA_CHECK(actual.has_value());
                RUVIA_CHECK(actual.value_or(std::string()) == expected);
                const auto decoded =
                    decode_http_content(coding, actual.value_or(std::string()), {.max_decoded_bytes_ = size});
                RUVIA_CHECK(std::holds_alternative<std::pmr::string>(decoded));
                if (const auto* content = std::get_if<std::pmr::string>(&decoded)) {
                    RUVIA_CHECK(std::string_view(*content) == input);
                }
            }
        }
    }
}

RUVIA_TEST(http_content_encode_context_keeps_state_apart_from_output) {
    const auto input = sample_body(20'000, 7);
    for (const auto coding : compressing_codings) {
        counting_resource state_resource;
        counting_resource output_resource;
        {
            http_content_encode_context context(coding, &state_resource);
            RUVIA_CHECK_EQ(state_resource.allocations(), 0U);

            std::optional<std::string> first;
            {
                auto result_value =
                    context.encode(input, {.max_encoded_bytes_ = input.size(), .resource_ = &output_resource});
                RUVIA_CHECK(result_value.has_value());
                RUVIA_CHECK(output_resource.live_bytes() != 0);
                if (result_value.has_value()) {
                    first.emplace(*result_value);
                }
            }
            RUVIA_CHECK_EQ(output_resource.live_bytes(), 0U);
            RUVIA_CHECK(state_resource.allocations() != 0);

            const auto warm_allocations = state_resource.allocations();
            RUVIA_CHECK(encode_with(context, input, input.size(), &output_resource) == first);
            if (coding == http_content_coding::brotli) {
                // Brotli cannot reset, so its state is per call and released.
                RUVIA_CHECK_EQ(state_resource.live_bytes(), 0U);
            } else {
                // A reset stream allocates nothing for the next representation.
                RUVIA_CHECK_EQ(state_resource.allocations(), warm_allocations);
                RUVIA_CHECK(state_resource.live_bytes() != 0);
            }
        }
        RUVIA_CHECK_EQ(state_resource.live_bytes(), 0U);
    }
}

RUVIA_TEST(http_content_encode_context_resets_after_a_failed_encode) {
    const auto failed_input = sample_body(100'000, 11);
    const auto next_input = sample_body(5'000, 12);
    for (const auto coding : compressing_codings) {
        http_content_encode_context context(coding, std::pmr::new_delete_resource());
        const auto expected = one_shot(coding, next_input, next_input.size());

        // Stop mid-stream on the output cap.
        const auto capped = context.encode(failed_input, {.max_encoded_bytes_ = 64});
        RUVIA_CHECK(!capped.has_value());
        RUVIA_CHECK(encode_with(context, next_input, next_input.size()) == expected);

        // Stop mid-stream on an output allocation failure.
        failing_memory_resource output_resource;
        output_resource.fail_after(0);
        bool threw = false;
        try {
            static_cast<void>(context.encode(
                failed_input, {.max_encoded_bytes_ = failed_input.size(), .resource_ = &output_resource}));
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(output_resource.live_allocations(), 0U);
        RUVIA_CHECK(encode_with(context, next_input, next_input.size()) == expected);
    }
}

RUVIA_TEST(http_content_encode_context_recovers_from_state_allocation_failure) {
    const auto input = sample_body(3'000, 21);
    for (const auto coding : compressing_codings) {
        failing_memory_resource state_resource;
        {
            http_content_encode_context context(coding, &state_resource);
            state_resource.fail_after(0);
            bool threw = false;
            try {
                static_cast<void>(context.encode(input, {.max_encoded_bytes_ = input.size()}));
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            RUVIA_CHECK(threw);
            RUVIA_CHECK_EQ(state_resource.live_allocations(), 0U);

            state_resource.allow_allocations();
            RUVIA_CHECK(encode_with(context, input, input.size()) == one_shot(coding, input, input.size()));
        }
        RUVIA_CHECK_EQ(state_resource.live_allocations(), 0U);
    }
}

RUVIA_TEST(http_content_encode_context_identity_follows_the_one_shot_contract) {
    counting_resource state_resource;
    http_content_encode_context context(http_content_coding::identity, &state_resource);
    RUVIA_CHECK(encode_with(context, "plain", 5) == std::optional<std::string>("plain"));
    const auto oversized = context.encode("plain", {.max_encoded_bytes_ = 4});
    RUVIA_CHECK(!oversized.has_value());
    RUVIA_CHECK_EQ(state_resource.allocations(), 0U);
}

RUVIA_TEST(http_content_encode_context_reuses_after_zero_and_exact_budgets) {
    const auto input = sample_body(8'000, 31);
    for (const auto coding : compressing_codings) {
        http_content_encode_context context(coding, std::pmr::new_delete_resource());
        const auto expected = encode_http_content(coding, input, {.max_encoded_bytes_ = input.size()});
        RUVIA_CHECK(expected.has_value());
        if (!expected.has_value()) {
            continue;
        }
        RUVIA_CHECK(!context.encode({}, {.max_encoded_bytes_ = 0}).has_value());
        RUVIA_CHECK(!context.encode(input, {.max_encoded_bytes_ = 0}).has_value());
        const auto exact = context.encode(input, {.max_encoded_bytes_ = expected->size()});
        RUVIA_CHECK(exact.has_value() && *exact == *expected);
        RUVIA_CHECK(!context.encode(input, {.max_encoded_bytes_ = expected->size() - 1}).has_value());
        RUVIA_CHECK(encode_with(context, input, input.size()) == std::string(*expected));
    }
}

RUVIA_TEST(http_content_encode_context_unknown_coding_throws_without_allocating_state) {
    counting_resource resource;
    http_content_encode_context context(static_cast<http_content_coding>(255), &resource);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            static_cast<void>(context.encode("plain", {.max_encoded_bytes_ = 0}));
        } catch (const ruvia::http_content_encode_error&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    RUVIA_CHECK_EQ(resource.allocations(), 0U);
}
