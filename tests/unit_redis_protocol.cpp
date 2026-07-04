#include "test_harness.h"

#include <exception>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "redis/core/RedisProtocol.h"

namespace {

using ruvia::detail::appendRespCommand;
using ruvia::detail::respCommandSerializedSize;

std::span<const std::string_view> asSpan(const std::vector<std::string_view>& args) {
    return std::span<const std::string_view>(args.data(), args.size());
}

std::string encode(const std::vector<std::string_view>& args) {
    std::pmr::string out(std::pmr::get_default_resource());
    appendRespCommand(out, asSpan(args));
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(resp_command_encodes_multibulk_form) {
    // RESP2 multi-bulk: *<n> then $<len>\r\n<arg>\r\n per argument.
    RUVIA_CHECK_EQ(encode({"SET", "key", "val"}),
                   std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$3\r\nval\r\n"));
    // A single-argument command and a zero-length argument.
    RUVIA_CHECK_EQ(encode({"PING"}), std::string("*1\r\n$4\r\nPING\r\n"));
    RUVIA_CHECK_EQ(encode({"GET", ""}), std::string("*2\r\n$3\r\nGET\r\n$0\r\n\r\n"));
}

RUVIA_TEST(resp_serialized_size_matches_written_output) {
    // The size hint is used to reserve buffer space, so it must exactly equal the
    // bytes appendRespCommand writes -- including multi-digit length prefixes and
    // the empty-argument case.
    const std::string mid(42, 'y');    // two-digit bulk length
    const std::string big(150, 'x');   // three-digit bulk length
    const std::vector<std::vector<std::string_view>> cases = {
        {"PING"},
        {"SET", "key", "value"},
        {"GET", ""},
        {"MSET", "k1", "v1", "k2", "v2"},
        {"SETEX", "k", mid, big},
    };
    for (const auto& args : cases) {
        std::pmr::string out(std::pmr::get_default_resource());
        const auto span = asSpan(args);
        const auto hinted = respCommandSerializedSize(span);
        appendRespCommand(out, span);
        RUVIA_CHECK_EQ(hinted, out.size());
    }
}

RUVIA_TEST(resp_command_rejects_empty_argument_list) {
    std::pmr::string out(std::pmr::get_default_resource());
    const std::span<const std::string_view> empty;
    bool threw = false;
    try {
        appendRespCommand(out, empty);
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}
