#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia::detail {

class http_client_pool;
class http_client_response_state;
class http_client_result_budget_domain;

class http_client_response_memory_domain final {
public:
    class deleter_type final {
    public:
        void operator()(http_client_response_memory_domain* domain) const noexcept;
    };

    using owner = std::unique_ptr<http_client_response_memory_domain, deleter_type>;

    [[nodiscard]] static owner create(const worker_handle& worker,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    // Bind an accounting/custom upstream once; it must outlive the domain and
    // every state pin. Production defaults to the process resource.
    [[nodiscard]] static owner create(const worker_handle& worker,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain,
        std::pmr::memory_resource& upstream);

    http_client_response_memory_domain(const http_client_response_memory_domain&) = delete;
    http_client_response_memory_domain& operator=(const http_client_response_memory_domain&) = delete;

    [[nodiscard]] const worker_handle& worker() const& noexcept {
        return worker_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept {
        return &receive_resource_;
    }

    [[nodiscard]] http_client_response_state* create_state(http_client_pool& pool);
    void destroy_state(http_client_response_state* state) noexcept;
    void detach_transport_bindings(http_client_pool& pool) noexcept;

private:
    enum class phase_type : unsigned char { prepared,
        active,
        retired };

    http_client_response_memory_domain(const worker_handle& worker_value,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain,
        std::pmr::memory_resource& upstream);
    ~http_client_response_memory_domain();

    void require_current() const noexcept;
    void attach_state(http_client_response_state& state) noexcept;
    void unlink_state(http_client_response_state& state) noexcept;
    void retain() noexcept;
    void release() noexcept;
    void release_root() noexcept;

    class receive_resource final : public std::pmr::memory_resource {
    public:
        receive_resource(std::pmr::memory_resource& upstream, http_client_result_budget_domain& budget)
            : upstream_(upstream),
              budget_(budget) {}

    private:
        void* do_allocate(std::size_t bytes, std::size_t alignment) override;
        void do_deallocate(void* allocation, std::size_t bytes, std::size_t alignment) override;
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
        std::pmr::memory_resource& upstream_;
        http_client_result_budget_domain& budget_;
    };

    worker_memory memory_;
    worker_handle worker_;
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    mutable receive_resource receive_resource_;
    http_client_response_state* state_head_{};
    std::size_t references_{1};
    phase_type phase_{phase_type::prepared};

    friend class deleter_type;
    friend class http_client_response_state;
};

}  // namespace ruvia::detail
