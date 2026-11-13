#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

class http_client;
class worker_handle;
#ifdef RUVIA_ENABLE_DATABASE
class db_client;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_client;
#endif

namespace detail {

// Caller-owned standalone clients a router lends to the requests it
// dispatches. Every entry borrows its client, which must run on the router's
// worker and outlive every dispatch; aliases are unique per client kind. The
// set is sealed with its router, so request-time lookup only reads it.
class borrowed_clients final {
public:
    borrowed_clients();
    borrowed_clients(const borrowed_clients&) = delete;
    borrowed_clients& operator=(const borrowed_clients&) = delete;

    [[nodiscard]] bool empty() const noexcept;

    void bind_http_client(const worker_handle& worker, std::string_view alias, const http_client& client);
    [[nodiscard]] const http_client* http_clients(std::string_view alias) const noexcept;

#ifdef RUVIA_ENABLE_DATABASE
    void bind_database(const worker_handle& worker, std::string_view alias, const db_client& client);
    [[nodiscard]] const db_client* database(std::string_view alias) const noexcept;
#endif

#ifdef RUVIA_ENABLE_REDIS
    void bind_redis(const worker_handle& worker, std::string_view alias, const redis_client& client);
    [[nodiscard]] const redis_client* redis(std::string_view alias) const noexcept;
#endif

private:
    template <typename client_type>
    struct alias_entry final {
        std::pmr::string alias_;
        const client_type* client_;
    };

    std::pmr::vector<alias_entry<http_client>> http_clients_;
#ifdef RUVIA_ENABLE_DATABASE
    std::pmr::vector<alias_entry<db_client>> databases_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<alias_entry<redis_client>> redis_;
#endif
};

}  // namespace detail
}  // namespace ruvia
