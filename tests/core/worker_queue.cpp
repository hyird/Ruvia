#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_runtime_context.h"

#include "test_harness.h"

RUVIA_TEST(worker_queue_yields_to_io_context_between_batches) {
    struct reposting_task final {
        const ruvia::worker_handle* worker_;
        const bool* foreign_ran_;
        unsigned* runs_;
        void operator()() const {
            ++*runs_;
            if (!*foreign_ran_ && *runs_ < 1000) {
                static_cast<void>(worker_->post(*this));
            }
        }
    };
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 1);
    const auto worker_value = runtime.handle();
    bool foreign_ran{};
    unsigned runs{};
    RUVIA_CHECK(worker_value.post(reposting_task{&worker_value, &foreign_ran, &runs}).accepted());
    asio::post(context, [&foreign_ran] { foreign_ran = true; });
    runtime.run();
    // The repost lands behind the plain handler queued after the first batch.
    RUVIA_CHECK(foreign_ran);
    RUVIA_CHECK_EQ(runs, 2u);
}

RUVIA_TEST(worker_cancellation_registration_retires_before_slot_reuse) {
    struct owner final {
        std::uint64_t identity_{};
        unsigned accepted_{};
        unsigned stale_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++accepted_;
            } else {
                ++stale_;
            }
        }
    } state;
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 1);
    auto target = ruvia::make_worker_cancellation_target(state, runtime.handle());
    using registration = ruvia::worker_cancellation_registration<ruvia::worker_cancellation_target<owner>>;
    std::optional<registration> current;
    ruvia::stop_source old_stop;
    ruvia::stop_source new_stop;
    std::uint64_t old_id{};
    bool identity_cleared{};
    RUVIA_CHECK(runtime.handle().post([&] {
                                    current.emplace(target, state.identity_);
                                    old_id = current->id();
                                    current->arm(old_stop.token());
                                    // Foreign cancellation is queued behind this owner turn.
                                    std::jthread requester([&] { old_stop.request_stop(); });
                                    requester.join();
                                    current.reset();
                                    identity_cleared = state.identity_ == 0;
                                    current.emplace(target, state.identity_);
                                    current->arm(new_stop.token());
                                })
            .accepted());
    runtime.run();
    RUVIA_CHECK(identity_cleared);
    RUVIA_CHECK(state.identity_ != 0 && state.identity_ != old_id);
    RUVIA_CHECK_EQ(state.accepted_, 0u);
    RUVIA_CHECK_EQ(state.stale_, 1u);
    RUVIA_CHECK(runtime.handle().post([&] {
                                    new_stop.request_stop();
                                    current.reset();
                                    target->detach(state);
                                })
            .accepted());
    context.restart();
    runtime.run();
    RUVIA_CHECK_EQ(state.accepted_, 1u);
    RUVIA_CHECK_EQ(state.identity_, std::uint64_t{0});
}

RUVIA_TEST(worker_cancellation_registration_handles_precancel_and_exception_unwind) {
    struct owner final {
        std::uint64_t identity_{};
        unsigned cancelled_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++cancelled_;
            }
        }
    } state;
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 1);
    auto target = ruvia::make_worker_cancellation_target(state, runtime.handle());
    ruvia::stop_source source;
    source.request_stop();
    bool unwound = false;
    RUVIA_CHECK(runtime.handle().post([&] {
                                    try {
                                        ruvia::worker_cancellation_registration registration(target, state.identity_);
                                        registration.arm(source.token());
                                        throw std::runtime_error("operation failed");
                                    } catch (const std::runtime_error&) {
                                        unwound = state.identity_ == 0;
                                    }
                                    target->detach(state);
                                })
            .accepted());
    runtime.run();
    RUVIA_CHECK(unwound);
    RUVIA_CHECK_EQ(state.cancelled_, 1u);
}
