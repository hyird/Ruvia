#include <memory_resource>
#include <string>

#include "ruvia/http/http_request_trailers.h"

#include "test_harness.h"

namespace {
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace

RUVIA_TEST(request_trailers_own_fields_preserve_duplicates_and_bound_section_size) {
    counting_resource resource;
    {
        ruvia::http_request_trailers trailers(&resource);
        std::string source_value(512, 'a');
        RUVIA_CHECK((trailers.append("X-Checksum", source_value).index() == 0));
        source_value.assign("mutated");
        RUVIA_CHECK_EQ(trailers.field("x-checksum").value_or(""), std::string(512, 'a'));
        RUVIA_CHECK((trailers.append_http1("x-checksum: final\r\nx-extra: yes\r\n\r\n").index() == 0));
        RUVIA_CHECK_EQ(trailers.field("X-Checksum").value_or(""), "final");
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
        RUVIA_CHECK((trailers.append("Content-Length", "3").index() != 0));
        RUVIA_CHECK((trailers.append("bad name", "x").index() != 0));
        RUVIA_CHECK((trailers.append("x-ok", "bad\r\nvalue").index() != 0));
        RUVIA_CHECK((trailers.append("x-large", std::string(65536, 'x')).index() != 0));
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}
