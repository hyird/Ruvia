#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace ruvia::detail {

class HttpClientPool;
class HttpClientResponseState;
class HttpClientResultBudgetDomain;

class HttpClientResponseMemoryDomain final {
public:
    class Deleter final {
    public:
        void operator()(HttpClientResponseMemoryDomain* domain) const noexcept;
    };

    using Owner = std::unique_ptr<HttpClientResponseMemoryDomain, Deleter>;

    [[nodiscard]] static Owner create(const WorkerHandle& worker,
        const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain);
    // Bind an accounting/custom upstream once; it must outlive the domain and
    // every state pin. Production defaults to the process resource.
    [[nodiscard]] static Owner create(const WorkerHandle& worker,
        const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain,
        std::pmr::memory_resource& upstream);

    HttpClientResponseMemoryDomain(const HttpClientResponseMemoryDomain&) = delete;
    HttpClientResponseMemoryDomain& operator=(const HttpClientResponseMemoryDomain&) = delete;

    [[nodiscard]] const WorkerHandle& worker() const& noexcept {
        return worker_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept {
        return memory_.resource();
    }

    [[nodiscard]] HttpClientResponseState* createState(HttpClientPool& pool);
    void destroyState(HttpClientResponseState* state) noexcept;
    void detachTransportBindings(HttpClientPool& pool) noexcept;

private:
    enum class Phase : unsigned char { kPrepared,
        kActive,
        kRetired };

    HttpClientResponseMemoryDomain(const WorkerHandle& worker,
        const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain,
        std::pmr::memory_resource& upstream);
    ~HttpClientResponseMemoryDomain();

    void requireCurrent() const noexcept;
    void attachState(HttpClientResponseState& state) noexcept;
    void unlinkState(HttpClientResponseState& state) noexcept;
    void retain() noexcept;
    void release() noexcept;
    void releaseRoot() noexcept;

    WorkerMemory memory_;
    WorkerHandle worker_;
    std::shared_ptr<HttpClientResultBudgetDomain> resultBudgetDomain_;
    HttpClientResponseState* stateHead_{};
    std::size_t references_{1};
    Phase phase_{Phase::kPrepared};

    friend class Deleter;
    friend class HttpClientResponseState;
};

}  // namespace ruvia::detail
