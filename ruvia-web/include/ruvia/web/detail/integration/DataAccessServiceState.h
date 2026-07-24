#pragma once

#include <atomic>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <utility>
#include <vector>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerPostCounters.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/DataAccess.h"
#include "ruvia/web/detail/integration/DataAccessState.h"

// The lifecycle behind DataAccessService: one connect, then jobs on the bound
// worker, then one close. DataAccessService and DataAccessContext are thin
// forwarders over this state; every phase transition, submission decision and
// counter lives here.

namespace ruvia::detail {

struct DbDefinition;
struct RedisDefinition;

class DataAccessServiceState final : public std::enable_shared_from_this<DataAccessServiceState> {
public:
    using Job = MoveOnlyFunction<Task<void>(DataAccessContext&)>;

    DataAccessServiceState(EventLoop loop, DataAccessOptions options);
    ~DataAccessServiceState();

    DataAccessServiceState(const DataAccessServiceState&) = delete;
    DataAccessServiceState& operator=(const DataAccessServiceState&) = delete;

    void bindStop();

    [[nodiscard]] std::future<void> scheduleConnect();
    [[nodiscard]] Task<void> connectOnWorker();

    [[nodiscard]] DataAccessPostResult post(Job task);

    void requestClose() noexcept;
    void closeOnWorker() noexcept;

    [[nodiscard]] DataAccessStats stats() const noexcept;

    void requireConnectedOnWorker() const;

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept {
        return memory_.resource();
    }

    [[nodiscard]] StopToken stopToken() const noexcept {
        return stopSource_.token();
    }

    [[nodiscard]] DataAccessState& access() noexcept {
        return access_;
    }

private:
    // Holds the outstanding-job count taken at submission time, so a post that
    // is accepted but never runs still gives the count back.
    class JobReservation final {
    public:
        explicit JobReservation(std::shared_ptr<DataAccessServiceState> state) noexcept
            : state_(std::move(state)) {}

        ~JobReservation() {
            if (state_ != nullptr) {
                state_->abandonJob();
            }
        }

        JobReservation(const JobReservation&) = delete;
        JobReservation& operator=(const JobReservation&) = delete;
        JobReservation(JobReservation&& other) noexcept
            : state_(std::move(other.state_)) {}
        JobReservation& operator=(JobReservation&&) = delete;

        [[nodiscard]] std::shared_ptr<DataAccessServiceState> release() noexcept {
            return std::exchange(state_, nullptr);
        }

    private:
        std::shared_ptr<DataAccessServiceState> state_;
    };

    void closeSubmissions() noexcept;
    void startJob(Job task);
    [[nodiscard]] static Task<void> runJob(Job task, std::shared_ptr<DataAccessServiceState> state);
    void completeJob(TaskCompletionResult<void> result);
    void abandonJob() noexcept;

    enum class Phase : unsigned char {
        kFresh,
        kConnectScheduled,
        kConnecting,
        kConnected,
        kClosed,
    };

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    std::pmr::vector<DbDefinition> databaseDefinitions_;
    std::pmr::vector<RedisDefinition> redisDefinitions_;
    ConnectionScanner scanner_;
    DataAccessState access_;
    StopSource stopSource_;
    // Published once by bindStop() and retained until state destruction. The
    // listener only weakly references this state, so retaining it forms no
    // cycle; avoiding callback-side reset also closes the register-vs-stop
    // publication race during DataAccessService construction.
    EventLoopStopRegistration stopRegistration_;
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler_;
    mutable std::mutex submitMutex_;
    bool accepting_{true};
    std::atomic<Phase> phase_{Phase::kFresh};
    std::atomic_size_t outstanding_{0};
    WorkerPostCounters postCounters_;
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failed_{0};
};

}  // namespace ruvia::detail
