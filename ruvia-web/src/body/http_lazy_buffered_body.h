#pragma once

#include "ruvia/core/memory/pmr_resource.h"

#include "body/http_stream_body_reader.h"

namespace ruvia::detail {

template <typename stream_type>
class lazy_buffered_body final {
public:
    lazy_buffered_body(stream_type& stream, std::pmr::polymorphic_allocator<char> worker_allocator,
        std::pmr::memory_resource* request_resource, std::string_view initial_body_and_pipeline,
        http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
        ruvia::connection_scanner::entry_type& scanner_entry)
        : reader_(
              stream, worker_allocator, initial_body_and_pipeline, body_plan, body_limit_value, scanner_entry),
          body_(pmr_resource_or_default(request_resource)) {}

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept {
        return reader_.consumption();
    }
    [[nodiscard]] const http_request_trailers& trailers() const& noexcept {
        return reader_.trailers();
    }

    void take_pipeline(std::pmr::string& stash) {
        reader_.take_pipeline(stash);
    }

    [[nodiscard]] task<std::string_view> read_all() {
        co_return co_await reader_.read_all(body_);
    }

    task<void> discard() {
        while (co_await reader_.read()) {
        }
    }

private:
    stream_body_reader<stream_type> reader_;
    std::pmr::string body_;
};

}  // namespace ruvia::detail
