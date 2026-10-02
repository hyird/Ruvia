#include <memory_resource>
#include <string>

#include "ruvia/http/HttpRequestTrailers.h"

#include "test_harness.h"

namespace {
class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t deallocations{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace

RUVIA_TEST(request_trailers_own_fields_preserve_duplicates_and_bound_section_size) {
    CountingResource resource;
    {
        ruvia::HttpRequestTrailers trailers(&resource);
        std::string source(512, 'a');
        RUVIA_CHECK(trailers.append("X-Checksum", source).has_value());
        source.assign("mutated");
        RUVIA_CHECK_EQ(trailers.field("x-checksum").value_or(""), std::string(512, 'a'));
        RUVIA_CHECK(trailers.appendHttp1("x-checksum: final\r\nx-extra: yes\r\n\r\n").has_value());
        RUVIA_CHECK_EQ(trailers.field("X-Checksum").value_or(""), "final");
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
        RUVIA_CHECK(!trailers.append("Content-Length", "3"));
        RUVIA_CHECK(!trailers.append("bad name", "x"));
        RUVIA_CHECK(!trailers.append("x-ok", "bad\r\nvalue"));
        RUVIA_CHECK(!trailers.append("x-large", std::string(65536, 'x')));
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}
