#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/static_files.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// Inline buffered-response compression on one worker, whose encoders are
// reused across responses. This executable owns its application, so the
// app-wide compression policy cannot change the bodies of other suites.

namespace {

[[maybe_unused]] const bool compression_configured = [] {
    ruvia::app().compression({.min_bytes_ = 16});
    return true;
}();

[[nodiscard]] std::string make_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 3U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 6U) + ((i / 48U) % 5U));
    }
    return body;
}

// Both stay below the default 64 KiB synchronous limit, so neither is offloaded.
const std::string small_body = make_body(3'000, 1);
const std::string large_body = make_body(60'000, 2);

[[nodiscard]] const std::filesystem::path& file_dir() {
    static const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ruvia_response_compression_files";
    return dir;
}

std::unique_ptr<ruvia::static_root> resident_root;
std::unique_ptr<ruvia::static_root> coded_root;

class response_compression_routes final {
    RUVIA_ROUTES_BEGIN(response_compression_routes)
    RUVIA_GET("/compression-small", small);
    RUVIA_GET("/compression-large", large);
    RUVIA_GET("/compression-tiny", tiny);
    RUVIA_GET("/compression-digest", digest);
    RUVIA_GET("/compression-file", file);
    RUVIA_GET("/compression-static", static_file);
    RUVIA_GET("/compression-coded-static", coded_static_file);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> small(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(small_body));
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(large_body));
    }

    ruvia::task<ruvia::http_response> tiny(ruvia::context& context_value) {
        co_return context_value.text("hello");
    }

    ruvia::task<ruvia::http_response> digest(ruvia::context& context_value) {
        const auto field = context_value.req().query("field").value_or("");
        context_value.header(field, "sha-256=:AAAA:");
        co_return context_value.text(std::string_view(small_body));
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        static const std::filesystem::path path = file_dir() / "file.txt";
        co_return context_value.file({.path_ = path});
    }

    ruvia::task<ruvia::http_response> static_file(ruvia::context& context_value) {
        co_return context_value.static_file(*resident_root, {.relative_path_ = "resident.txt"});
    }

    ruvia::task<ruvia::http_response> coded_static_file(ruvia::context& context_value) {
        co_return context_value.static_file(*coded_root, {.relative_path_ = "asset.txt"});
    }
};

// Sends one request and returns every byte the server wrote before closing,
// or nullopt when the connection is still open after 5 s.
[[nodiscard]] std::optional<std::string> fetch_response(
    const asio::ip::tcp::endpoint& server_endpoint, std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(server_endpoint);
    asio::write(socket, asio::buffer(request));
    std::string output;
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output),
        [&closed](const std::error_code&, std::size_t) { closed = true; });
    io.run_for(std::chrono::seconds(5));
    if (!closed) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] std::string lower_head(std::string_view response) {
    std::string head(response.substr(0, response.find("\r\n\r\n")));
    std::ranges::transform(head, head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head;
}

// The response head's field lines without Date, which may tick between two
// requests.
[[nodiscard]] std::vector<std::string> head_fields(std::string_view response) {
    std::vector<std::string> fields;
    const auto head = lower_head(response);
    for (auto begin = head.find("\r\n"); begin != std::string::npos;) {
        begin += 2;
        const auto end = head.find("\r\n", begin);
        auto line = head.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.starts_with("date:")) {
            fields.push_back(std::move(line));
        }
        begin = end;
    }
    std::ranges::sort(fields);
    return fields;
}

[[nodiscard]] bool has_field(const std::vector<std::string>& fields, std::string_view prefix) {
    return std::ranges::any_of(fields, [prefix](const std::string& line) { return line.starts_with(prefix); });
}

[[nodiscard]] std::string expected_encoding(ruvia::http_content_coding coding, std::string_view body) {
    const auto result_value = ruvia::encode_http_content(coding, body, {.max_encoded_bytes_ = body.size() - 1});
    return result_value.has_value() ? std::string(*result_value) : std::string();
}

[[nodiscard]] std::string_view response_body(std::string_view response) {
    const auto separator = response.find("\r\n\r\n");
    return separator == std::string_view::npos ? std::string_view{} : response.substr(separator + 4);
}

[[nodiscard]] std::string get_request(std::string_view target, std::string_view fields) {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" + std::string(fields) +
           "Connection: close\r\n\r\n";
}

}  // namespace

RUVIA_TEST(http1_inline_compression_reuses_worker_encoders_with_identical_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    // Each encoder serves several responses of alternating sizes in a row.
    for (int round = 0; round < 2; ++round) {
        for (const auto coding : codings) {
            for (const bool large : {true, false, true}) {
                const std::string_view path = large ? "/compression-large" : "/compression-small";
                const std::string_view body = large ? large_body : small_body;
                const auto token = ruvia::http_content_coding_token(coding);
                const auto output = fetch_response(server.endpoint(),
                    "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: " +
                        std::string(token) + "\r\nConnection: close\r\n\r\n");
                RUVIA_CHECK(output.has_value());
                const auto text = output.value_or("");
                RUVIA_CHECK(text.starts_with("HTTP/1.1 200 "));
                RUVIA_CHECK(lower_head(text).find("\r\ncontent-encoding: " + std::string(token) + "\r\n") !=
                            std::string::npos);
                const auto separator = text.find("\r\n\r\n");
                RUVIA_CHECK(separator != std::string::npos);
                if (separator != std::string::npos) {
                    const auto expected = expected_encoding(coding, body);
                    RUVIA_CHECK(!expected.empty());
                    RUVIA_CHECK(std::string_view(text).substr(separator + 4) == expected);
                }
            }
        }
    }
    server.finish();
}

// context::file and static_file serve fixed file representations: a small
// file's bytes travel in memory on GET but still are not runtime-compressed,
// so GET and HEAD carry the same identity fields (RFC 9110 9.3.2).
RUVIA_TEST(http1_file_representations_are_not_runtime_compressed) {
    namespace fs = std::filesystem;
    std::error_code ignored;
    fs::remove_all(file_dir(), ignored);
    fs::create_directories(file_dir());
    for (const auto* name : {"file.txt", "resident.txt"}) {
        std::ofstream(file_dir() / name, std::ios::binary) << small_body;
    }
    resident_root = std::make_unique<ruvia::static_root>(file_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 16 * 1024,
            .resident_budget_bytes_ = 16 * 1024,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::string_view path : {"/compression-file", "/compression-static"}) {
        const auto fetch = [&](std::string_view method) {
            return fetch_response(server.endpoint(), std::string(method) + " " + std::string(path) +
                                                         " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: "
                                                         "gzip\r\nConnection: close\r\n\r\n")
                .value_or("");
        };
        const auto get = fetch("GET");
        const auto head = fetch("HEAD");
        RUVIA_CHECK(get.starts_with("HTTP/1.1 200 "));
        RUVIA_CHECK(head.starts_with("HTTP/1.1 200 "));
        const auto get_fields = head_fields(get);
        RUVIA_CHECK(get_fields == head_fields(head));
        RUVIA_CHECK(!has_field(get_fields, "content-encoding:"));
        RUVIA_CHECK(has_field(get_fields, "content-length: " + std::to_string(small_body.size())));
        RUVIA_CHECK(has_field(get_fields, "etag: \""));
        // static_file negotiates indexed precompressed variants; context::file
        // has no coding dimension at all.
        RUVIA_CHECK_EQ(has_field(get_fields, "vary: accept-encoding"), path == "/compression-static");
        const auto separator = get.find("\r\n\r\n");
        RUVIA_CHECK(separator != std::string::npos);
        if (separator != std::string::npos) {
            RUVIA_CHECK(std::string_view(get).substr(separator + 4) == small_body);
        }
    }
    server.finish();
    resident_root.reset();
    fs::remove_all(file_dir(), ignored);
}

// RFC 9530 §2/§3: Content-Digest and Repr-Digest are computed over the content
// as sent, including its content coding, so the framework must not encode a
// body whose integrity fields the application already computed.
RUVIA_TEST(http1_compression_keeps_responses_with_integrity_fields_identity) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::string_view field : {"Content-Digest", "Repr-Digest", "Content-MD5"}) {
        const auto text = fetch_response(server.endpoint(),
            get_request("/compression-digest?field=" + std::string(field), "Accept-Encoding: gzip\r\n"))
                              .value_or("");
        RUVIA_CHECK(text.starts_with("HTTP/1.1 200 "));
        const auto fields = head_fields(text);
        RUVIA_CHECK(!has_field(fields, "content-encoding:"));
        RUVIA_CHECK(has_field(fields, "content-length: " + std::to_string(small_body.size())));
        RUVIA_CHECK(response_body(text) == small_body);
    }
    server.finish();
}

// RFC 9110 §12.5.3: a client that refuses identity accepts a listed coding, so a
// body below min_bytes_ is encoded instead of answered with 406.
RUVIA_TEST(http1_compression_encodes_small_bodies_when_identity_is_refused) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto refused = fetch_response(server.endpoint(),
        get_request("/compression-tiny", "Accept-Encoding: gzip, identity;q=0\r\n"))
                             .value_or("");
    RUVIA_CHECK(refused.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(has_field(head_fields(refused), "content-encoding: gzip"));
    const auto decoded = ruvia::decode_http_content(ruvia::http_content_coding::gzip, response_body(refused),
        {.max_decoded_bytes_ = 64, .resource_ = std::pmr::get_default_resource()});
    const auto* plain = std::get_if<std::pmr::string>(&decoded);
    RUVIA_CHECK(plain != nullptr && std::string_view(*plain) == "hello");

    // With identity acceptable, the size threshold still keeps it identity.
    const auto accepted =
        fetch_response(server.endpoint(), get_request("/compression-tiny", "Accept-Encoding: gzip\r\n")).value_or("");
    RUVIA_CHECK(accepted.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(!has_field(head_fields(accepted), "content-encoding:"));
    RUVIA_CHECK(response_body(accepted) == "hello");
    server.finish();
}

// RFC 9110 §8.4 / §14.6: a multipart/byteranges body cannot carry the coding of
// the representation it slices, so several ranges of a precompressed variant
// are answered with the full coded representation; one range stays a 206.
RUVIA_TEST(http1_precompressed_variant_answers_multiple_ranges_in_full) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_response_compression_coded";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    const std::string sidecar = "gz-sidecar-bytes";
    std::ofstream(dir / "asset.txt", std::ios::binary) << "plain-asset-body";
    std::ofstream(dir / "asset.txt.gz", std::ios::binary) << sidecar;
    coded_root = std::make_unique<ruvia::static_root>(dir,
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .range_requests_ = ruvia::static_range_request_policy::honor,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto multi = fetch_response(server.endpoint(),
        get_request("/compression-coded-static", "Accept-Encoding: gzip\r\nRange: bytes=0-1,4-5\r\n"))
                           .value_or("");
    RUVIA_CHECK(multi.starts_with("HTTP/1.1 200 "));
    const auto multi_fields = head_fields(multi);
    RUVIA_CHECK(has_field(multi_fields, "content-encoding: gzip"));
    RUVIA_CHECK(!has_field(multi_fields, "content-type: multipart/byteranges"));
    RUVIA_CHECK(response_body(multi) == sidecar);

    const auto single = fetch_response(server.endpoint(),
        get_request("/compression-coded-static", "Accept-Encoding: gzip\r\nRange: bytes=0-1\r\n"))
                            .value_or("");
    RUVIA_CHECK(single.starts_with("HTTP/1.1 206 "));
    const auto single_fields = head_fields(single);
    RUVIA_CHECK(has_field(single_fields, "content-encoding: gzip"));
    RUVIA_CHECK(has_field(single_fields, "content-range: bytes 0-1/" + std::to_string(sidecar.size())));
    RUVIA_CHECK(response_body(single) == "gz");

    // The identity representation still answers several ranges with multipart.
    const auto identity = fetch_response(server.endpoint(),
        get_request("/compression-coded-static", "Accept-Encoding: identity\r\nRange: bytes=0-1,4-5\r\n"))
                              .value_or("");
    RUVIA_CHECK(identity.starts_with("HTTP/1.1 206 "));
    RUVIA_CHECK(has_field(head_fields(identity), "content-type: multipart/byteranges"));
    RUVIA_CHECK(identity.find("\r\nContent-Encoding:") == std::string::npos);
    server.finish();
    coded_root.reset();
    fs::remove_all(dir, ignored);
}
