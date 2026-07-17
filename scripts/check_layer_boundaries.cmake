cmake_minimum_required(VERSION 3.25)

get_filename_component(RUVIA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Architectural checks target the complete HTTP/2 connection implementation,
# not one physical translation unit. Keep the implementation partition list in
# one place so receive and local-submit responsibilities can evolve independently.
set(HTTP2_CONNECTION_PRIMARY_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(HTTP2_CONNECTION_IMPLEMENTATION_FILES
    "${HTTP2_CONNECTION_PRIMARY_SOURCE}"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2ConnectionHeaders.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2ConnectionSubmit.cpp")

function(read_http2_connection_implementation output)
    set(content)
    foreach(source IN LISTS HTTP2_CONNECTION_IMPLEMENTATION_FILES)
        if(EXISTS "${source}")
            file(READ "${source}" fragment)
            string(APPEND content "\n${fragment}")
        endif()
    endforeach()
    set(${output} "${content}" PARENT_SCOPE)
endfunction()

function(expand_http2_connection_implementation output)
    set(paths ${ARGN})
    list(FIND paths "${HTTP2_CONNECTION_PRIMARY_SOURCE}" primary_index)
    if(NOT primary_index EQUAL -1)
        list(REMOVE_ITEM paths "${HTTP2_CONNECTION_PRIMARY_SOURCE}")
        list(APPEND paths ${HTTP2_CONNECTION_IMPLEMENTATION_FILES})
    endif()
    list(REMOVE_DUPLICATES paths)
    set(${output} "${paths}" PARENT_SCOPE)
endfunction()

# Pure negative boundaries are declarative: each regex lives beside the planted
# samples that prove it detects its forbidden shape. Complex cross-file positive
# contracts stay below as executable checks.
set(RUVIA_BOUNDARY_RULE_CATALOG
    "${RUVIA_ROOT}/scripts/layer_boundary_rules.json")
if(NOT EXISTS "${RUVIA_BOUNDARY_RULE_CATALOG}")
    message(FATAL_ERROR "boundary rule catalog is missing")
endif()
file(READ "${RUVIA_BOUNDARY_RULE_CATALOG}" boundary_rule_catalog)
string(JSON boundary_rule_count LENGTH "${boundary_rule_catalog}")
if(boundary_rule_count EQUAL 0)
    message(FATAL_ERROR "boundary rule catalog is empty")
endif()
math(EXPR boundary_rule_last "${boundary_rule_count} - 1")
foreach(boundary_rule_index RANGE 0 ${boundary_rule_last})
    string(JSON rule_name GET
        "${boundary_rule_catalog}" ${boundary_rule_index} name)
    string(JSON rule_regex GET
        "${boundary_rule_catalog}" ${boundary_rule_index} regex)
    if(NOT rule_name MATCHES "^RULE_[A-Z0-9_]+$")
        message(FATAL_ERROR
            "boundary rule ${boundary_rule_index} has invalid name ${rule_name}")
    endif()
    if(DEFINED "${rule_name}")
        message(FATAL_ERROR "duplicate boundary rule ${rule_name}")
    endif()
    set("${rule_name}" "${rule_regex}")
endforeach()

function(expect_match label regex sample)
    string(REGEX MATCH "${regex}" detected "${sample}")
    if(detected STREQUAL "")
        message(FATAL_ERROR
            "boundary self-test FAIL: ${label} was not detected\n"
            "sample: ${sample}\nregex: ${regex}")
    endif()
endfunction()

function(example_has_private_include content output)
    string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" includes "${content}")
    set(found FALSE)
    foreach(include_line IN LISTS includes)
        if(NOT include_line MATCHES "#[ \t]*include[ \t]*\"ruvia/")
            set(found TRUE)
            break()
        endif()
    endforeach()
    set(${output} ${found} PARENT_SCOPE)
endfunction()

if(RUVIA_BOUNDARY_SELF_TEST)
    foreach(boundary_rule_index RANGE 0 ${boundary_rule_last})
        string(JSON rule_name GET
            "${boundary_rule_catalog}" ${boundary_rule_index} name)
        string(JSON boundary_sample_count LENGTH
            "${boundary_rule_catalog}" ${boundary_rule_index} samples)
        if(boundary_sample_count EQUAL 0)
            message(FATAL_ERROR
                "boundary self-test FAIL: ${rule_name} has no planted sample")
        endif()
        math(EXPR boundary_sample_last "${boundary_sample_count} - 1")
        foreach(boundary_sample_index RANGE 0 ${boundary_sample_last})
            string(JSON boundary_sample_width LENGTH
                "${boundary_rule_catalog}" ${boundary_rule_index}
                samples ${boundary_sample_index})
            if(NOT boundary_sample_width EQUAL 2)
                message(FATAL_ERROR
                    "boundary self-test FAIL: ${rule_name} sample ${boundary_sample_index} must contain label and planted source")
            endif()
            string(JSON label GET
                "${boundary_rule_catalog}" ${boundary_rule_index}
                samples ${boundary_sample_index} 0)
            string(JSON sample GET
                "${boundary_rule_catalog}" ${boundary_rule_index}
                samples ${boundary_sample_index} 1)
            expect_match("${label}" "${${rule_name}}" "${sample}")
        endforeach()
    endforeach()
    example_has_private_include("#include \"HttpServerInternal.h\"" private_example)
    if(NOT private_example)
        message(FATAL_ERROR
            "boundary self-test FAIL: private example include was not detected")
    endif()
    example_has_private_include("#include \"ruvia/web/Context.h\"" public_example)
    if(public_example)
        message(FATAL_ERROR
            "boundary self-test FAIL: public example include was rejected")
    endif()
    message(STATUS "boundary self-test OK (all negative rules detect planted violations)")
    return()
endif()

set_property(GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED FALSE)

function(boundary_error label details)
    message(STATUS "boundary-check FAIL: ${label}\n    ${details}")
    set_property(GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED TRUE)
endfunction()

function(check_files_no_match label regex)
    set(hit_files)
    expand_http2_connection_implementation(paths ${ARGN})
    foreach(path IN LISTS paths)
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(READ "${path}" content)
            string(REGEX MATCH "${regex}" detected "${content}")
            if(NOT detected STREQUAL "")
                file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
                list(APPEND hit_files "${relative}: ${detected}")
            endif()
        endif()
    endforeach()
    if(hit_files)
        list(JOIN hit_files "\n    " details)
        boundary_error("${label}" "${details}")
    endif()
endfunction()

file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/Channel.h"
    core_channel_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/OneShot.h"
    core_one_shot_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/OperationDeadline.h"
    core_operation_deadline_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/Task.h"
    core_task_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/TaskScope.h"
    core_task_scope_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/src/TaskScope.cpp"
    core_task_scope_implementation)
file(READ "${RUVIA_ROOT}/tests/task_scope.cpp"
    core_task_scope_test_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/TaskPromise.h"
    core_task_promise_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/AsioAwait.h"
    core_task_completion_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/WorkerWaitResult.h"
    core_worker_wait_result_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/WorkerWaitAwaiter.h"
    core_worker_wait_awaiter_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/WorkerTimer.h"
    core_worker_timer_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/WorkerHandle.h"
    core_worker_handle_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/WorkerDispatcher.h"
    core_worker_dispatcher_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/src/WorkerDispatcher.cpp"
    core_worker_dispatcher_implementation)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/WorkerSignal.h"
    core_worker_signal_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/RuntimeLifecycle.h"
    core_runtime_lifecycle_contract)
file(READ "${RUVIA_ROOT}/ruvia-core/src/EventLoopPool.cpp"
    core_runtime_implementation)
file(READ "${RUVIA_ROOT}/tests/runtime_worker.cpp"
    core_runtime_test_contract)
file(READ "${RUVIA_ROOT}/tests/worker_timer.cpp"
    core_worker_timer_test_contract)
file(READ "${RUVIA_ROOT}/tests/worker_dispatch_failure.cpp"
    core_worker_dispatch_failure_test_contract)
file(READ "${RUVIA_ROOT}/tests/operation_deadline.cpp"
    core_operation_deadline_test_contract)
file(READ "${RUVIA_ROOT}/tests/package-consumer/core.cpp"
    core_linear_receiver_package_contract)
if(NOT core_runtime_lifecycle_contract MATCHES
       "class RuntimeLifecycle final" OR
   NOT core_runtime_lifecycle_contract MATCHES
       "compare_exchange_weak" OR
   NOT core_runtime_lifecycle_contract MATCHES
       "expected = State::kStopping" OR
   NOT core_runtime_lifecycle_contract MATCHES
       "State::kStopped" OR
   core_runtime_lifecycle_contract MATCHES
       "exchange[(]State::kStopping" OR
   NOT core_runtime_implementation MATCHES
       "lifecycle[.]requestStop[(][)]" OR
   core_runtime_implementation MATCHES
       "exchange[(]RuntimeState::kStopping" OR
   NOT core_runtime_test_contract MATCHES
       "testLifecycleTransitionsAreMonotonic" OR
   NOT core_runtime_test_contract MATCHES
       "testConcurrentStopHasOneInitiator")
    boundary_error("Runtime lifecycle can regress after terminal stop"
        "start, stop initiation, and stop completion must use the monotonic lifecycle contract, with concurrent stop ownership and terminal-state regression covered by tests")
endif()
if(NOT core_worker_handle_contract MATCHES
       "std::shared_ptr<detail::WorkerDispatcher> dispatcher_" OR
   core_worker_handle_contract MATCHES
       "std::weak_ptr<detail::WorkerDispatcher> dispatcher_" OR
   NOT core_worker_timer_contract MATCHES
       "WorkerTimerRegistration[(]WorkerTimerRegistration&&[)] = delete" OR
   NOT core_worker_timer_contract MATCHES
       "WorkerDispatcher[*] dispatcher_" OR
   NOT core_worker_timer_contract MATCHES
       "std::size_t slot_" OR
   NOT core_worker_timer_contract MATCHES
       "std::uint64_t generation_" OR
   core_worker_timer_contract MATCHES
       "std::shared_ptr|std::weak_ptr|std::atomic" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "struct TimerSlot final" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "std::pmr::vector<TimerSlot> timerSlots" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "if [(]gCurrentWorker == this[)]" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "cancelTimer[(]slot, generation[)]" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "self = shared_from_this[(][)], slot, generation" OR
   core_worker_dispatcher_implementation MATCHES
       "allocate_shared<WorkerTimerState>|WorkerTimerState" OR
   NOT core_worker_dispatcher_contract MATCHES
       "void detachContext[(][)] noexcept" OR
   NOT core_worker_dispatcher_contract MATCHES
       "bool attached[(][)] const noexcept" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "if [(]!impl_->contextAttached[)]" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "abandonedSlots[.]swap[(]impl_->slots[)]" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "abandonedTimers[.]swap[(]impl_->timers[)]" OR
   NOT core_worker_dispatcher_contract MATCHES
       "requestTimerCancellation" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "requestTimerCancellation" OR
   NOT core_worker_timer_test_contract MATCHES
       "detachedTimerCancellationRaceWorks" OR
   NOT core_runtime_implementation MATCHES
       "dispatcher->detachContext[(][)]" OR
   NOT core_runtime_test_contract MATCHES
       "testEscapedWorkerHandleBecomesDetachedEndpoint" OR
   NOT core_runtime_test_contract MATCHES
       "testDetachDestroysAbandonedMailboxTasks")
    boundary_error("WorkerHandle regained weak-lock dispatch or an unretired context endpoint"
        "worker handles must directly own a stable dispatcher endpoint, while every context owner detaches it before context destruction and escaped handles are tested as safe invalid endpoints")
endif()
if(NOT core_worker_signal_contract MATCHES
       "const WorkerHandle[*][ \\t]+worker_" OR
   NOT core_worker_signal_contract MATCHES
       "Awaiter[*][ \\t]+waiters_" OR
   NOT core_worker_signal_contract MATCHES
       "std::size_t scheduledWaiters_" OR
   NOT core_worker_signal_contract MATCHES
       "~Awaiter[(][)]" OR
   NOT core_worker_signal_contract MATCHES
       "worker_[-][>]isCurrent[(][)]" OR
   core_worker_signal_contract MATCHES
       "asio::any_io_executor|template <typename Executor>|using Target = std::variant" OR
   core_worker_signal_contract MATCHES
       "WorkerHandle[ \\t]+worker_" OR
   core_worker_signal_contract MATCHES
       "std::array<std::coroutine_handle|waiter capacity exceeded" OR
   NOT core_runtime_test_contract MATCHES
       "testWorkerSignalIsWorkerAffine" OR
   NOT core_runtime_test_contract MATCHES
       "testWorkerSignalHasNoArbitraryWaiterLimit")
    boundary_error("WorkerSignal weakened worker affinity or intrusive waiter lifetime"
        "each signal must borrow one stable WorkerHandle, guard linked and scheduled coroutine-frame nodes, and avoid handle refcount churn, executor fallback, or an arbitrary concurrency limit")
endif()
if(NOT core_runtime_implementation MATCHES
       "using ContextOwnership" OR
   NOT core_runtime_implementation MATCHES
       "std::variant<std::unique_ptr<asio::io_context>, ExternalContextClaim>" OR
   core_runtime_implementation MATCHES
       "std::optional<ExternalContextClaim>|std::optional<asio::executor_work_guard")
    boundary_error("event loop context ownership regained parallel optional state"
        "owned context and external claim must be exclusive alternatives, and every live loop state must own an engaged work guard")
endif()
if(NOT core_worker_dispatcher_implementation MATCHES
       "impl_->slots\\[insertedIndex\\][.]reset[(][)]" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "impl_->tail = insertedIndex" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "impl_->drainScheduled = false" OR
   NOT core_worker_dispatch_failure_test_contract MATCHES
       "attempt < 2")
    boundary_error("worker mailbox dispatch regained partial commit"
        "drain scheduling failure must roll back the inserted slot, tail, size, and scheduled state before another producer can observe the mailbox")
endif()
if(NOT core_worker_dispatcher_contract MATCHES
       "deferOrTerminate" OR
   NOT core_worker_dispatcher_implementation MATCHES
       "void WorkerDispatcher::deferOrTerminate" OR
   core_worker_dispatcher_implementation MATCHES
       "dispatcher->defer[(][ \\t\\r\\n]*\\[dispatcher, state" OR
   NOT core_runtime_implementation MATCHES
       "dispatcher->deferOrTerminate[(]" OR
   core_runtime_implementation MATCHES
       "dispatcher->defer[(][ \\t\\r\\n]*\\[dispatcher = dispatcher\\].*stopTimers")
    boundary_error("worker control dispatch regained silent failure"
        "timer cancellation and loop stopping must terminate if their required worker dispatch cannot be committed")
endif()
if(NOT core_worker_dispatch_failure_test_contract MATCHES
       "deferOrTerminate" OR
   NOT core_worker_dispatch_failure_test_contract MATCHES
       "set_terminate")
    boundary_error("worker dispatch failure injection coverage was removed"
        "tests must inject both transactional mailbox scheduling failure and required-control-dispatch termination")
endif()
if(NOT core_operation_deadline_contract MATCHES
       "struct Inactive final" OR
   NOT core_operation_deadline_contract MATCHES
       "struct Active final" OR
   NOT core_operation_deadline_contract MATCHES
       "struct Expired final" OR
   NOT core_operation_deadline_contract MATCHES
       "using State = std::variant" OR
   NOT core_operation_deadline_contract MATCHES
       "requires std::is_enum_v<Kind>" OR
   core_operation_deadline_contract MATCHES
       "bool active_|bool expired_|optional<Kind> kind_" OR
   NOT core_operation_deadline_test_contract MATCHES
       "operationDeadlineTransitionsAreExclusive" OR
   NOT core_linear_receiver_package_contract MATCHES
       "HasRvalueOperationDeadlineKind")
    boundary_error("operation deadlines regained parallel lifecycle state"
        "inactive, armed, and expired deadlines must remain exclusive while retaining the cancellation kind through expiry")
endif()
if(NOT core_channel_contract MATCHES
       "struct ChannelOpen final" OR
   NOT core_channel_contract MATCHES
       "struct ChannelClosed final" OR
   NOT core_channel_contract MATCHES
       "struct ChannelWorkerStopping final" OR
   NOT core_channel_contract MATCHES
       "using Lifecycle = std::variant" OR
   NOT core_channel_contract MATCHES
       "emplace<ChannelWorkerStopping>" OR
   NOT core_channel_contract MATCHES
       "emplace<detail::ChannelClosed>" OR
   core_channel_contract MATCHES
       "bool closed|bool workerStopped")
    boundary_error("Channel regained overlapping close and worker-stop flags"
        "open, sender-closed, and worker-stopping must remain exclusive while buffered values stay orthogonal to lifecycle")
endif()
if(NOT core_channel_contract MATCHES
       "ChannelReceiver[(][)][ \t]*=[ \t]*delete" OR
   NOT core_channel_contract MATCHES
       "operator=[(]ChannelReceiver&&[)][ \t]*=[ \t]*delete" OR
   NOT core_one_shot_contract MATCHES
       "OneShotReceiver[(][)][ \t]*=[ \t]*delete" OR
   NOT core_one_shot_contract MATCHES
       "operator=[(]OneShotReceiver&&[)][ \t]*=[ \t]*delete" OR
   NOT core_task_contract MATCHES
       "operator=[(]Task&&[)][ \t]*=[ \t]*delete" OR
   NOT core_linear_receiver_package_contract MATCHES
       "!std::default_initializable<ruvia::ChannelReceiver<int>>" OR
   NOT core_linear_receiver_package_contract MATCHES
       "!std::default_initializable<ruvia::OneShotReceiver<int>>" OR
   NOT core_linear_receiver_package_contract MATCHES
       "!std::assignable_from<ruvia::Task<void>&, ruvia::Task<void>&&>" OR
   NOT core_linear_receiver_package_contract MATCHES
       "!std::assignable_from<ruvia::Task<int>&, ruvia::Task<int>&&>")
    boundary_error("core linear async owners regained invalid default states or destructive reassignment"
        "receivers must be factory-created and Task/receiver handles must be move-construct-only so assignment cannot orphan endpoints or destroy live coroutine frames")
endif()
if(NOT core_task_scope_contract MATCHES
       "struct TaskScopeEmpty final" OR
   NOT core_task_scope_contract MATCHES
       "struct TaskScopeOpen final" OR
   NOT core_task_scope_contract MATCHES
       "class TaskScopeJoining final" OR
   NOT core_task_scope_contract MATCHES
       "struct TaskScopeJoined final" OR
   NOT core_task_scope_contract MATCHES
       "struct TaskScopeSuccess final" OR
   NOT core_task_scope_contract MATCHES
       "class TaskScopeFailure final" OR
   NOT core_task_scope_contract MATCHES
       "using Lifecycle = std::variant" OR
   NOT core_task_scope_contract MATCHES
       "using Outcome = std::variant" OR
   core_task_scope_contract MATCHES
       "firstFailure_|joinContinuation_|joinStarted_" OR
   NOT core_task_scope_implementation MATCHES
       "holds_alternative<TaskScopeOpen>[(]lifecycle_[)]" OR
   NOT core_task_scope_implementation MATCHES
       "holds_alternative<TaskScopeJoining>[(]lifecycle_[)]" OR
   NOT core_task_scope_implementation MATCHES
       "task[.]handle_ == nullptr" OR
   NOT core_task_scope_implementation MATCHES
       "emplace<TaskScopeJoined>" OR
   NOT core_task_scope_test_contract MATCHES
       "emptyTaskRejected" OR
   NOT core_task_scope_test_contract MATCHES
       "completedFailureObserved" OR
   NOT core_linear_receiver_package_contract MATCHES
       "!std::move_constructible<ruvia::TaskScope>")
    boundary_error("TaskScope regained overlapping join/failure state or optional joining"
        "empty, open, joining, and joined must be exclusive; non-empty scopes must join, failures must remain a success/failure outcome, and moved-from Task inputs must be rejected")
endif()
if(NOT core_one_shot_contract MATCHES
       "struct OneShotPending final" OR
   NOT core_one_shot_contract MATCHES
       "class OneShotReady final" OR
   NOT core_one_shot_contract MATCHES
       "struct OneShotConsumed final" OR
   NOT core_one_shot_contract MATCHES
       "struct OneShotReceiverClosed final" OR
   NOT core_one_shot_contract MATCHES
       "struct OneShotWorkerStopping final" OR
   NOT core_one_shot_contract MATCHES
       "using Lifecycle = std::variant" OR
   NOT core_one_shot_contract MATCHES
       "wakeOneShotReceiver" OR
   core_one_shot_contract MATCHES
       "std::optional<T>[ \t]+value|bool completed|bool consumed|bool closed|bool workerStopped")
    boundary_error("OneShot regained parallel lifecycle flags"
        "pending, ready, consumed, receiver-closed, and worker-stopping must remain exclusive states and use the shared completion-then-wake protocol")
endif()
if(NOT core_worker_wait_awaiter_contract MATCHES
       "struct WorkerWaitAwaitPreparing final" OR
   NOT core_worker_wait_awaiter_contract MATCHES
       "class WorkerWaitAwaitSuspended final" OR
   NOT core_worker_wait_awaiter_contract MATCHES
       "class WorkerWaitAwaitReadyBeforeSuspend final" OR
   NOT core_worker_wait_awaiter_contract MATCHES
       "class WorkerWaitAwaitReadyAfterSuspend final" OR
   NOT core_worker_wait_awaiter_contract MATCHES
       "class WorkerWaitAwaitState final" OR
   NOT core_worker_wait_awaiter_contract MATCHES
       "using State = std::variant" OR
   NOT core_worker_wait_awaiter_contract MATCHES
       "WorkerWaitAwaitState[(]WorkerWaitAwaitState&&[)] = delete" OR
   NOT core_channel_contract MATCHES
       "WorkerWaitAwaitState<T> completion" OR
   NOT core_one_shot_contract MATCHES
       "WorkerWaitAwaitState<T> completion" OR
   core_channel_contract MATCHES
       "std::optional<WorkerWaitResult<T>>|bool suspended|bool wakePending|prepareChannelReceiverWake" OR
   core_one_shot_contract MATCHES
       "std::optional<WorkerWaitResult<T>>|bool suspended|bool wakePending|prepareOneShotReceiverWake")
    boundary_error("worker waits regained parallel await-suspension state"
        "Channel and OneShot must share one discriminated preparing/suspended/completed handshake so a racing wake cannot overlap flags or an optional result")
endif()
if(NOT core_worker_timer_contract MATCHES
       "enum class WorkerTimerOutcome" OR
   NOT core_worker_timer_contract MATCHES
       "kExpired" OR
   NOT core_worker_timer_contract MATCHES
       "kCancelled" OR
   NOT core_worker_handle_contract MATCHES
       "move_only_function<void[(]WorkerTimerOutcome[)]>" OR
   NOT core_worker_dispatcher_contract MATCHES
       "move_only_function<void[(]WorkerTimerOutcome[)]>" OR
   core_channel_contract MATCHES
       "[(]bool cancelled[)]" OR
   core_one_shot_contract MATCHES
       "[(]bool cancelled[)]" OR
   NOT core_linear_receiver_package_contract MATCHES
       "!std::convertible_to<" OR
   NOT core_linear_receiver_package_contract MATCHES
       "WorkerTimerOutcome, bool")
    boundary_error("worker timer completion regained a boolean outcome"
        "timer expiry and cancellation must remain named outcomes throughout core and Web runtime callbacks")
endif()
if(NOT core_task_promise_contract MATCHES
       "enum class TaskFrameOwnership" OR
   NOT core_task_promise_contract MATCHES
       "kCold" OR
   NOT core_task_promise_contract MATCHES
       "kStarted" OR
   NOT core_task_promise_contract MATCHES
       "void markStarted[(][)] noexcept" OR
   NOT core_task_promise_contract MATCHES
       "bool started[(][)] const noexcept" OR
   NOT core_task_contract MATCHES
       "!handle[.]done[(][)] && handle[.]promise[(][)][.]started[(][)]" OR
   NOT core_task_contract MATCHES
       "std::terminate[(][)]" OR
   core_task_promise_contract MATCHES
       "kDetached|detachIfStarted" OR
   core_task_contract MATCHES
       "handle_[.]destroy[(][)];[\r\n \t]*handle_ = nullptr" )
    boundary_error("Task regained destructive or detached cancellation"
        "cold Tasks may be discarded, but started Tasks are structured run-to-completion owners and must never destroy or silently detach a suspended coroutine frame")
endif()
if(NOT core_task_promise_contract MATCHES
       "struct TaskPromisePending final" OR
   NOT core_task_promise_contract MATCHES
       "struct TaskPromiseCompleted final" OR
   NOT core_task_promise_contract MATCHES
       "class TaskPromiseValue final" OR
   NOT core_task_promise_contract MATCHES
       "class TaskPromiseFailure final" OR
   NOT core_task_promise_contract MATCHES
       "using State = std::variant" OR
   NOT core_task_promise_contract MATCHES
       "emplace<TaskPromiseValue<T>>" OR
   NOT core_task_promise_contract MATCHES
       "emplace<TaskPromiseCompleted>" OR
   NOT core_task_promise_contract MATCHES
       "emplace<TaskPromiseFailure>" OR
   core_task_promise_contract MATCHES
       "TaskExceptionStorage|hasException_|std::optional<T>[ 	]+value_")
    boundary_error("Task promise regained parallel value/exception state"
        "pending, returned value, completed void, and failure must remain explicit exclusive coroutine states")
endif()
if(NOT core_task_completion_contract MATCHES
       "class TaskCompletionSuccess final" OR
   NOT core_task_completion_contract MATCHES
       "class TaskCompletionFailure final" OR
   NOT core_task_completion_contract MATCHES
       "class TaskCompletionResult final" OR
   NOT core_task_completion_contract MATCHES
       "using Value = std::variant" OR
   NOT core_task_completion_contract MATCHES
       "success[(][)] [&] noexcept" OR
   NOT core_task_completion_contract MATCHES
       "failure[(][)] const [&] noexcept" OR
   NOT core_task_completion_contract MATCHES
       "success[(][)] && = delete" OR
   NOT core_task_completion_contract MATCHES
       "failure[(][)] const && = delete" OR
   core_task_completion_contract MATCHES
       "struct TaskCompletionResult|std::optional<T>[ 	]+value|std::exception_ptr[ 	]+exception[;]" OR
   NOT core_linear_receiver_package_contract MATCHES
       "HasLooseTaskCompletionFields" OR
   NOT core_linear_receiver_package_contract MATCHES
       "HasAnyRvalueTaskCompletionAccessor")
    boundary_error("Task-to-Asio completion regained a loose exception/value tuple"
        "success and failure must remain exclusive alternatives, with payload borrows restricted to live lvalue owners")
endif()
if(NOT core_task_completion_contract MATCHES
       "struct AsioCompletionPending final" OR
   NOT core_task_completion_contract MATCHES
       "class AsioCompletion final" OR
   NOT core_task_completion_contract MATCHES
       "class AsioCompletionAwaiter final" OR
   NOT core_task_completion_contract MATCHES
       "using State = std::variant" OR
   NOT core_task_completion_contract MATCHES
       "errorCode[(][)] const noexcept" OR
   NOT core_task_completion_contract MATCHES
       "result[(][)] const [&] noexcept" OR
   NOT core_task_completion_contract MATCHES
       "result[(][)] const && = delete" OR
   NOT core_task_completion_contract MATCHES
       "takeResult[(][)] &&" OR
   NOT core_task_completion_contract MATCHES
       "asyncAsio" OR
   core_task_completion_contract MATCHES
       "ErrorAwaiter|ErrorResultAwaiter|asyncError|asyncResult|std::pair<std::error_code" OR
   NOT core_linear_receiver_package_contract MATCHES
       "HasRvalueAsioCompletionResult")
    boundary_error("Asio adapter regained split or positional completion results"
        "one typed completion must own its error and optional protocol result while pending/completed remains an exclusive awaiter state")
endif()
if(NOT core_worker_wait_result_contract MATCHES
       "value[(][)] const [&] noexcept" OR
   NOT core_worker_wait_result_contract MATCHES
       "value[(][)] [&] noexcept" OR
   NOT core_worker_wait_result_contract MATCHES
       "value[(][)] const && = delete" OR
   NOT core_worker_wait_result_contract MATCHES
       "value[(][)] && = delete" OR
   NOT core_worker_wait_result_contract MATCHES
       "closed[(][)] const [&] noexcept" OR
   NOT core_worker_wait_result_contract MATCHES
       "workerStopping[(][)] const && = delete" OR
   NOT core_linear_receiver_package_contract MATCHES
       "HasAnyRvalueWorkerWaitAccessor")
    boundary_error("worker wait results expose alternatives from temporary owners"
        "Channel and OneShot completion payload pointers must remain valid only while their lvalue WorkerWaitResult owner lives")
endif()

function(check_files_no_lower_match label regex)
    set(hit_files)
    expand_http2_connection_implementation(paths ${ARGN})
    foreach(path IN LISTS paths)
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(READ "${path}" content)
            string(TOLOWER "${content}" content)
            string(REGEX MATCH "${regex}" detected "${content}")
            if(NOT detected STREQUAL "")
                file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
                list(APPEND hit_files "${relative}: ${detected}")
            endif()
        endif()
    endforeach()
    if(hit_files)
        list(JOIN hit_files "\n    " details)
        boundary_error("${label}" "${details}")
    endif()
endfunction()

foreach(required_dir IN ITEMS ruvia-core ruvia-http ruvia-web)
    if(NOT IS_DIRECTORY "${RUVIA_ROOT}/${required_dir}")
        boundary_error("missing target directory"
            "${required_dir}/ is required; otherwise checks would be vacuous")
    endif()
endforeach()
foreach(required_doc IN ITEMS README.md AGENTS.md)
    if(NOT EXISTS "${RUVIA_ROOT}/${required_doc}")
        boundary_error("missing project boundary document"
            "${required_doc} is required; otherwise checks would be vacuous")
    endif()
endforeach()

file(READ "${RUVIA_ROOT}/CMakeLists.txt" root_cmake)
if(root_cmake MATCHES "${RULE_MONOLITHIC_PACKAGE_EXPORT}")
    boundary_error("package targets were collapsed into one export set"
        "core/http/web must remain independently importable from partial installs")
endif()
if(NOT root_cmake MATCHES "EXPORT[ \t]+ruvia_[$][{]component[}]_targets" OR
   NOT root_cmake MATCHES "FILE[ \t]+ruvia-[$][{]_ruvia_component[}]-targets[.]cmake" OR
   NOT root_cmake MATCHES "include[(].*ruvia-core-targets[.]cmake" OR
   NOT root_cmake MATCHES "include[(].*ruvia-http-targets[.]cmake" OR
   NOT root_cmake MATCHES "include[(].*ruvia-web-targets[.]cmake")
    boundary_error("component-scoped package export contract is incomplete"
        "each target needs its own export and package config must load only the requested closure")
endif()

function(check_target_header_ownership target expected_namespace)
    set(include_root "${RUVIA_ROOT}/${target}/include/ruvia")
    if(NOT IS_DIRECTORY "${include_root}")
        boundary_error("target include root is missing" "${target}/include/ruvia")
        return()
    endif()

    file(GLOB namespace_entries RELATIVE "${include_root}" "${include_root}/*")
    foreach(entry IN LISTS namespace_entries)
        if(NOT entry STREQUAL expected_namespace)
            boundary_error("target public headers escape their namespace"
                "${target}/include/ruvia/${entry} is outside ruvia/${expected_namespace}")
        endif()
    endforeach()

    file(READ "${RUVIA_ROOT}/${target}/CMakeLists.txt" target_cmake)
    string(REGEX MATCHALL "include/ruvia/[A-Za-z0-9_-]+" installed_header_roots "${target_cmake}")
    foreach(header_root IN LISTS installed_header_roots)
        if(NOT header_root STREQUAL "include/ruvia/${expected_namespace}")
            boundary_error("target install list uses another namespace"
                "${target}/CMakeLists.txt: ${header_root}")
        endif()
    endforeach()
endfunction()

check_target_header_ownership(ruvia-core core)
check_target_header_ownership(ruvia-http http)
check_target_header_ownership(ruvia-web web)

if(IS_DIRECTORY "${RUVIA_ROOT}/ruvia-edge")
    boundary_error("ruvia-edge must remain fully removed" "ruvia-edge/ exists")
endif()
foreach(forbidden_web_client_path IN ITEMS
    "ruvia-web/src/client"
    "ruvia-web/include/ruvia/web/detail/client")
    if(EXISTS "${RUVIA_ROOT}/${forbidden_web_client_path}")
        boundary_error("ruvia-web outbound client runtime must remain removed"
            "${forbidden_web_client_path} exists")
    endif()
endforeach()
if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientTypes.h")
    boundary_error("stale split outbound client model header must remain removed"
        "ruvia-http/include/ruvia/http/HttpClientTypes.h exists")
endif()
if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpParser.h")
    boundary_error("generic loose HTTP parser API must remain removed"
        "ruvia-http/include/ruvia/http/HttpParser.h exists")
endif()
set(OBSOLETE_HTTP_BODY_FRAMER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpBodyFramer.h")
set(HTTP1_CHUNK_DECODER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h")
set(HTTP1_CHUNK_SCANNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpChunkParser.h")
if(EXISTS "${OBSOLETE_HTTP_BODY_FRAMER}")
    boundary_error("generic HTTP body framer must remain removed"
        "ruvia-http/include/ruvia/http/detail/HttpBodyFramer.h exists")
endif()
foreach(http1_chunk_contract IN ITEMS
        "${HTTP1_CHUNK_DECODER}"
        "${HTTP1_CHUNK_SCANNER}")
    if(NOT EXISTS "${http1_chunk_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${http1_chunk_contract}")
        boundary_error("typed HTTP/1 chunked contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP1_CHUNK_DECODER}" AND EXISTS "${HTTP1_CHUNK_SCANNER}")
    file(READ "${HTTP1_CHUNK_DECODER}" http1_chunk_decoder)
    file(READ "${HTTP1_CHUNK_SCANNER}" http1_chunk_scanner)
    if(NOT http1_chunk_decoder MATCHES "class Http1ChunkedBodyDecoder final" OR
       NOT http1_chunk_decoder MATCHES "class Http1ChunkDecodeNeedMore final" OR
       NOT http1_chunk_decoder MATCHES "class Http1ChunkDecodeBodyChunk final" OR
       NOT http1_chunk_decoder MATCHES "class Http1ChunkDecodeComplete final" OR
       NOT http1_chunk_decoder MATCHES "enum class Http1ChunkDecodeError" OR
       NOT http1_chunk_decoder MATCHES "class Http1ChunkDecodeFailure final" OR
       NOT http1_chunk_decoder MATCHES "using Value = std::variant" OR
       NOT http1_chunk_decoder MATCHES
           "using State = std::variant<ProgressState, Http1ChunkDecodeError>" OR
       NOT http1_chunk_decoder MATCHES "std::size_t consumedBytes[(][)] const" OR
       NOT http1_chunk_decoder MATCHES "std::string_view bytes[(][)] const" OR
       NOT http1_chunk_decoder MATCHES "std::get_if<Http1ChunkDecodeBodyChunk>" OR
       NOT http1_chunk_decoder MATCHES "std::get_if<Http1ChunkDecodeFailure>" OR
       NOT http1_chunk_decoder MATCHES
           "needMore[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_chunk_decoder MATCHES
           "bodyChunk[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_chunk_decoder MATCHES
           "complete[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_chunk_decoder MATCHES
           "failure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       http1_chunk_decoder MATCHES
           "${RULE_STALE_CHUNK_DECODER_FAILURE_SPLIT}" OR
       http1_chunk_decoder MATCHES "class Http1ChunkDecoder" OR
       http1_chunk_decoder MATCHES
           "throw[ \t]+(std::invalid_argument|HttpProtocolError)")
        boundary_error("HTTP/1 streaming chunk decoder lost field ownership"
            "need-more/body/complete/failure must be discriminated and the outer decoder must be the only state machine")
    endif()
    if(NOT http1_chunk_scanner MATCHES "class HttpChunkScanNeedMore final" OR
       NOT http1_chunk_scanner MATCHES "class HttpChunkScanComplete final" OR
       NOT http1_chunk_scanner MATCHES "class HttpChunkScanFailure final" OR
       NOT http1_chunk_scanner MATCHES "class HttpChunkScanResult final" OR
       NOT http1_chunk_scanner MATCHES "using Value = std::variant" OR
       NOT http1_chunk_scanner MATCHES
           "needMore[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_chunk_scanner MATCHES
           "complete[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_chunk_scanner MATCHES
           "failure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_chunk_scanner MATCHES "std::optional<HttpChunkScanError> validateHttpChunkTrailers" OR
       NOT http1_chunk_scanner MATCHES "HttpChunkScanError error[(][)] const")
        boundary_error("HTTP/1 whole-message chunk scanner lost field ownership"
            "only complete may expose the final consumed boundary and only failure may expose HttpChunkScanError")
    endif()
endif()
foreach(forbidden_dynamic_response_stream_path IN ITEMS
    "ruvia-http/include/ruvia/http/HttpBodyStream.h"
    "ruvia-web/include/ruvia/web/detail/http/HttpBodyStreamAccess.h")
    if(EXISTS "${RUVIA_ROOT}/${forbidden_dynamic_response_stream_path}")
        boundary_error("dynamic response-body streaming bypass must remain removed"
            "${forbidden_dynamic_response_stream_path} exists")
    endif()
endforeach()
foreach(forbidden_http_static_file_path IN ITEMS
    "ruvia-http/include/ruvia/http/detail/FileResponseHelpers.h"
    "ruvia-http/include/ruvia/http/detail/FileResponseResource.h"
    "ruvia-http/include/ruvia/http/detail/server/HttpFileChunkBuffer.h")
    if(EXISTS "${RUVIA_ROOT}/${forbidden_http_static_file_path}")
        boundary_error("static-file product/runtime helper must remain outside ruvia-http"
            "${forbidden_http_static_file_path} exists")
    endif()
endforeach()
if(NOT EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedFraming.h")
    boundary_error("HTTP/1 streaming framer is missing from ruvia-http"
        "ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedFraming.h")
endif()
file(GLOB_RECURSE HTTP_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-http/*.h"
    "${RUVIA_ROOT}/ruvia-http/*.cpp"
    "${RUVIA_ROOT}/ruvia-http/*.inl")
file(GLOB_RECURSE WEB_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-web/*.h"
    "${RUVIA_ROOT}/ruvia-web/*.cpp"
    "${RUVIA_ROOT}/ruvia-web/*.inl")
file(GLOB_RECURSE CORE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/*.h"
    "${RUVIA_ROOT}/ruvia-core/*.cpp"
    "${RUVIA_ROOT}/ruvia-core/*.inl")
check_files_no_match("ruvia-http must not invent a Ruvia Server product identity"
    "${RULE_HTTP_IMPLICIT_SERVER_PRODUCT}" ${HTTP_SOURCE})
check_files_no_match("Context request-field models must remain Web-owned"
    "RequestNameValue(View|List)|RequestQueryValues|RequestValueGroup(List)?" ${HTTP_SOURCE})
check_files_no_match("Web must not directly emit HTTP/1 Connection semantics"
    "http1MarkConnectionClose|[.]header[(][\"]Connection[\"]"
    ${WEB_SOURCE})

file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/ValidatedValues.h"
    web_validated_model_bindings)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    web_validated_model_context)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/controller/ControllerRuntime.h"
    web_validated_model_runtime)
file(READ "${RUVIA_ROOT}/ruvia-web/src/router/Router.cpp"
    web_validated_model_registration)
file(READ "${RUVIA_ROOT}/tests/unit_validator.cpp"
    web_validated_model_test)
file(READ "${RUVIA_ROOT}/tests/unit_routing.cpp"
    web_validated_model_routing_test)
if(NOT web_validated_model_bindings MATCHES
       "struct ValidatedModelBindingNode final" OR
   NOT web_validated_model_bindings MATCHES
       "ValidatedModelBindingNode[*] previous" OR
   NOT web_validated_model_bindings MATCHES
       "ValidatedModelBindingNode[*] head_" OR
   NOT web_validated_model_bindings MATCHES
       "~ValidatedModelBinding[(][)] noexcept" OR
   NOT web_validated_model_bindings MATCHES
       "~ValidatedModelBindings[(][)] noexcept" OR
   NOT web_validated_model_bindings MATCHES
       "if [(]head_ != nullptr[)]" OR
   NOT web_validated_model_bindings MATCHES
       "using ModelT = std::remove_cvref_t<T>" OR
   NOT web_validated_model_bindings MATCHES
       "bind[(]T&&[)] = delete" OR
   NOT web_validated_model_bindings MATCHES
       "bindings_->pop[(]node_[)]" OR
   web_validated_model_bindings MATCHES
       "ValidatedValueStore|std::array|constructPmrObject|destroyPmrObject|memory_resource|void[ \t]*[(][*]destroy" OR
   NOT web_validated_model_context MATCHES
       "ValidatedModelBindings validatedModels_" OR
   NOT web_validated_model_runtime MATCHES
       "BodyT body = co_await parseValidatedBody" OR
   NOT web_validated_model_runtime MATCHES
       "auto binding = bindValidatedModel[(]c, body[)]" OR
   NOT web_validated_model_runtime MATCHES
       "co_await next[(][)]" OR
   NOT web_validated_model_registration MATCHES
       "validateUniqueValidatedModelTypes" OR
   NOT web_validated_model_test MATCHES
       "validated_model_bindings_are_nested_scoped_borrows" OR
   NOT web_validated_model_test MATCHES
       "validated_model_binding_unwinds_on_exception" OR
   NOT web_validated_model_test MATCHES
       "AcceptsRvalueValidatedModel" OR
   NOT web_validated_model_routing_test MATCHES
       "validated_model_binding_spans_next_and_unwinds_before_upstream_resumes")
    boundary_error("validated models regained request-owned erased storage"
        "typed validator coroutine frames must own models, Context must keep only an unbounded intrusive stack of scoped non-owning bindings, and registration must reject duplicate model types before requests run")
endif()

set(WEB_RATE_LIMIT_RULE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/RateLimitRule.h")
set(WEB_RATE_LIMITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/RateLimiter.h")
set(WEB_RATE_LIMIT_DECISION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/RateLimitDecision.h")
set(WEB_RATE_LIMIT_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/RateLimit.cpp")
set(WEB_HTTP_SERVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServer.h")
set(WEB_APP_RUNTIME
    "${RUVIA_ROOT}/ruvia-web/src/app/App.cpp")
foreach(rate_limit_owner IN ITEMS
        "${WEB_RATE_LIMIT_RULE}"
        "${WEB_RATE_LIMITER}"
        "${WEB_HTTP_SERVER}"
        "${WEB_APP_RUNTIME}")
    if(NOT EXISTS "${rate_limit_owner}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${rate_limit_owner}")
        boundary_error("Worker-owned rate-limit chain is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_RATE_LIMIT_RULE}" AND EXISTS "${WEB_RATE_LIMITER}" AND
   EXISTS "${WEB_HTTP_SERVER}" AND EXISTS "${WEB_APP_RUNTIME}")
    file(READ "${WEB_RATE_LIMIT_RULE}" web_rate_limit_rule)
    file(READ "${WEB_RATE_LIMITER}" web_rate_limiter)
    file(READ "${WEB_HTTP_SERVER}" web_http_server)
    file(READ "${WEB_APP_RUNTIME}" web_app_runtime)
    if(web_rate_limit_rule MATCHES "slotCount|shared[ \t]+atomic" OR
       web_rate_limit_rule MATCHES "struct[ \t]+RateLimitRule" OR
       NOT web_rate_limit_rule MATCHES "class RateLimitRule final" OR
       NOT web_rate_limit_rule MATCHES "RateLimitRule fixedWindow" OR
       NOT web_rate_limit_rule MATCHES "enum class RateLimitOverflowPolicy" OR
       NOT web_rate_limit_rule MATCHES "Per-worker")
        boundary_error("RateLimitRule exposes shared-table implementation policy"
            "public rules must be valid fixed windows with independent per-worker admission semantics")
    endif()
    if(web_rate_limiter MATCHES
           "#[ \t]*include[ \t]*[<\"]atomic|std::atomic|compare_exchange|this_thread|yield[(]|normalizeRateLimitRule" OR
       NOT web_rate_limiter MATCHES "std::pmr::vector[<]Slot[>]")
        boundary_error("RateLimiter regained cross-worker synchronization"
            "the request path must mutate only its worker-owned PMR slot table")
    endif()
    if(NOT web_http_server MATCHES "RateLimiter[ \t]+rateLimiter_" OR
       web_http_server MATCHES "RateLimiter[*][ \t]+rateLimiter_" OR
       web_app_runtime MATCHES "unique_ptr[<]RateLimiter|runtime->rateLimiter")
        boundary_error("RateLimiter escaped HttpServer worker ownership"
            "each HttpServer must own its limiter; AppRuntimeGraph must not share one")
    endif()
endif()

set(WEB_AUTO_HTTPS_POLICY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerAutoHttps.h")
set(WEB_AUTO_HTTPS_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_AUTO_HTTPS_TEST
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp")
if(EXISTS "${WEB_AUTO_HTTPS_POLICY}" AND
   EXISTS "${WEB_AUTO_HTTPS_SESSION}" AND
   EXISTS "${WEB_AUTO_HTTPS_TEST}")
    file(READ "${WEB_AUTO_HTTPS_POLICY}" web_auto_https_policy)
    file(READ "${WEB_AUTO_HTTPS_SESSION}" web_auto_https_session)
    file(READ "${WEB_AUTO_HTTPS_TEST}" web_auto_https_test)
    if(web_auto_https_policy MATCHES
           "http1MarkConnectionClose|HttpServerResponseState[.]h|header[(][\"]Connection[\"]" OR
       NOT web_auto_https_session MATCHES
           "makeAutoHttpsRedirectResponse" OR
       NOT web_auto_https_session MATCHES
           "parsed[.]connectionPlan[.]requireClose[(][)]" OR
       NOT web_auto_https_session MATCHES
           "requireHttp1FinalResponseCommit" OR
       NOT web_auto_https_test MATCHES
           "!response[.]header[(][\"]Connection[\"][)][.]has_value[(][)]" OR
       NOT web_auto_https_test MATCHES
           "http1CommitFinalResponse")
        boundary_error("AutoHTTPS regained direct HTTP/1 connection ownership"
            "the Web policy must construct only the redirect, while the HTTP/1 session applies its external close policy through the parsed connection plan and the protocol commit emits Connection")
    endif()
endif()

set(WEB_RATE_LIMIT_HTTP1_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_RATE_LIMIT_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_SERVER_RESPONSE_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h")
if(EXISTS "${WEB_RATE_LIMITER}" AND
   EXISTS "${WEB_RATE_LIMIT_DECISION}" AND
   EXISTS "${WEB_RATE_LIMIT_SOURCE}" AND
   EXISTS "${WEB_RATE_LIMIT_HTTP1_SESSION}" AND
   EXISTS "${WEB_RATE_LIMIT_HTTP2_SESSION}" AND
   EXISTS "${WEB_SERVER_RESPONSE_STATE}")
    file(READ "${WEB_RATE_LIMIT_DECISION}" web_rate_limit_decision)
    file(READ "${WEB_RATE_LIMIT_SOURCE}" web_rate_limit_source)
    file(READ "${WEB_RATE_LIMIT_HTTP1_SESSION}" web_rate_limit_http1_session)
    file(READ "${WEB_RATE_LIMIT_HTTP2_SESSION}" web_rate_limit_http2_session)
    file(READ "${WEB_SERVER_RESPONSE_STATE}" web_server_response_state)
    if(NOT web_rate_limiter MATCHES "class RateLimitAllowed final" OR
       NOT web_rate_limiter MATCHES "class RateLimitRejection final" OR
       NOT web_rate_limiter MATCHES "class RateLimitDecision final" OR
       NOT web_rate_limiter MATCHES
           "std::variant<RateLimitAllowed, RateLimitRejection>" OR
       NOT web_rate_limiter MATCHES "allowed[(][)] const [&]" OR
       NOT web_rate_limiter MATCHES "rejection[(][)] const [&]" OR
       web_rate_limiter MATCHES "struct RateLimitCheck|bool[ 	]+allowed" OR
       NOT web_rate_limit_decision MATCHES "decideRequestRateLimit" OR
       NOT web_rate_limit_decision MATCHES "rateLimitRejectionError" OR
       NOT web_rate_limit_decision MATCHES "applyRateLimitRejectionHeaders" OR
       NOT web_rate_limit_source MATCHES
           "HttpErrorInfo[(]429, [\"]too_many_requests[\"], [\"]rate limit exceeded[\"]" OR
       NOT web_rate_limit_source MATCHES
           "applyRouteRateLimitRejectionHeaders" OR
       NOT web_rate_limit_http1_session MATCHES "decideRequestRateLimit" OR
       NOT web_rate_limit_http1_session MATCHES
           "Http1ClosingRejection::rateLimit" OR
       NOT web_rate_limit_http1_session MATCHES
           "applyRateLimitRejectionHeaders" OR
       NOT web_rate_limit_http2_session MATCHES "decideRequestRateLimit" OR
       NOT web_rate_limit_http2_session MATCHES
           "applyRateLimitRejectionHeaders" OR
       web_rate_limit_http1_session MATCHES
           "HttpErrorInfo[(]429|rate limit exceeded|resetAfterMs" OR
       web_rate_limit_http2_session MATCHES
           "HttpErrorInfo[(]429|rate limit exceeded|resetAfterMs" OR
       web_server_response_state MATCHES
           "Retry-After|setRetryAfterSeconds")
        boundary_error("rate-limit decisions or rejection presentation regressed"
            "the worker limiter must return typed alternatives and one Web policy must map every H1/H2/route rejection to 429 and Retry-After")
    endif()
endif()
check_files_no_match("rate-limit 429 presentation duplicated outside its Web policy"
    "[\"]rate limit exceeded[\"]|[\"]Retry-After[\"]"
    "${WEB_RATE_LIMIT_HTTP1_SESSION}"
    "${WEB_RATE_LIMIT_HTTP2_SESSION}"
    "${WEB_SERVER_RESPONSE_STATE}")

set(WEB_HTTP_SERVER_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpServerLifecycle.cpp")
set(WEB_HTTP_SERVER_ACCEPT
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpServerAccept.cpp")
set(WEB_HTTP1_STREAM_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_CLEARTEXT_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerCleartextHttp2.h")
set(WEB_HTTP_SERVER_WORKER_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWorkerState.h")
set(WEB_HTTP_SERVER_WORKER_COMPLETION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWorkerCompletion.h")
set(WEB_HTTP_SERVER_SHUTDOWN_TEST
    "${RUVIA_ROOT}/tests/server_shutdown_drain.cpp")
set(WEB_HTTP_SERVER_WORKER_COMPLETION_TEST
    "${RUVIA_ROOT}/tests/unit_http_server_worker_completion.cpp")
foreach(worker_lifecycle_file IN ITEMS
        "${WEB_HTTP_SERVER_LIFECYCLE}"
        "${WEB_HTTP_SERVER_ACCEPT}"
        "${WEB_HTTP1_STREAM_SESSION}"
        "${WEB_HTTP2_SESSION}"
        "${WEB_CLEARTEXT_HTTP2_SESSION}"
        "${WEB_HTTP_SERVER_WORKER_STATE}"
        "${WEB_HTTP_SERVER_WORKER_COMPLETION}"
        "${WEB_HTTP_SERVER_SHUTDOWN_TEST}"
        "${WEB_HTTP_SERVER_WORKER_COMPLETION_TEST}")
    if(NOT EXISTS "${worker_lifecycle_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${worker_lifecycle_file}")
        boundary_error("Worker-local shutdown chain is incomplete"
            "${relative} is required")
    endif()
endforeach()
check_files_no_match("request sessions must not poll cross-thread lifecycle atomics"
    "atomic_bool|started_[.]load|lifecycleState_"
    "${WEB_HTTP_SERVER_ACCEPT}"
    "${WEB_HTTP1_STREAM_SESSION}"
    "${WEB_HTTP2_SESSION}"
    "${WEB_CLEARTEXT_HTTP2_SESSION}")
if(EXISTS "${WEB_HTTP_SERVER_ACCEPT}")
    file(READ "${WEB_HTTP_SERVER_ACCEPT}" web_http_server_accept)
    if(NOT web_http_server_accept MATCHES
           "options_[.]maxConnections[.]has_value[(][)]" OR
       NOT web_http_server_accept MATCHES
           "activeConnectionCount_[ 	]*>=[ 	]*[*]options_[.]maxConnections" OR
       web_http_server_accept MATCHES
           "Http(Response|Error)|http1|writeResponse|RequestMemory")
        boundary_error("Connection admission crossed into HTTP response handling"
            "over-budget sockets must close before TLS/HTTP detection without fabricating protocol bytes")
    endif()
endif()
if(EXISTS "${WEB_HTTP_SERVER}" AND EXISTS "${WEB_HTTP_SERVER_LIFECYCLE}")
    file(READ "${WEB_HTTP_SERVER}" web_http_server_lifecycle_model)
    file(READ "${WEB_HTTP_SERVER_LIFECYCLE}" web_http_server_lifecycle)
    file(READ "${WEB_HTTP_SERVER_WORKER_STATE}" web_http_server_worker_state)
    file(READ "${WEB_HTTP_SERVER_WORKER_COMPLETION}"
        web_http_server_worker_completion)
    file(READ "${WEB_HTTP_SERVER_SHUTDOWN_TEST}" web_http_server_shutdown_test)
    file(READ "${WEB_HTTP_SERVER_WORKER_COMPLETION_TEST}"
        web_http_server_worker_completion_test)
    if(NOT web_http_server_lifecycle_model MATCHES
           "RuntimeLifecycle[ \t]+lifecycle_" OR
       NOT web_http_server_lifecycle_model MATCHES
           "HttpServerWorkerState[ \t]+workerState_" OR
       web_http_server_lifecycle_model MATCHES
           "bool[ \t]+(workerRunning_|drainPending_)" OR
       NOT web_http_server_worker_state MATCHES
           "kRunning,[ \t\r\n]+kDraining,[ \t\r\n]+kStopped" OR
       NOT web_http_server_lifecycle_model MATCHES
           "HttpServerWorkerCompletion[ \t]+workerCompletion_" OR
       web_http_server_lifecycle_model MATCHES
           "startupReady_|startupException_|workerException_|startupMutex_|startupCv_" OR
       NOT web_http_server_worker_completion MATCHES
           "class HttpServerWorkerCompletion final" OR
       NOT web_http_server_worker_completion MATCHES
           "using Startup = std::variant" OR
       NOT web_http_server_worker_completion MATCHES
           "markStartupReady[(][)] noexcept" OR
       NOT web_http_server_worker_completion MATCHES
           "markStartupFailed" OR
       NOT web_http_server_worker_completion MATCHES
           "waitForStartup[(][)]" OR
       web_http_server_lifecycle MATCHES
           "resetStartupState|completeStartup|waitForStartupReady" OR
       NOT web_http_server_worker_completion_test MATCHES
           "http_server_worker_completion_is_monotonic" OR
       NOT web_http_server_worker_completion_test MATCHES
           "http_server_worker_completion_propagates_startup_failure" OR
       NOT web_http_server_worker_completion_test MATCHES
           "http_server_worker_completion_keeps_first_terminal_failure" OR
       NOT web_http_server_lifecycle MATCHES "asio::post[(]ioContext_" OR
       NOT web_http_server_lifecycle MATCHES
           "HttpServer::~HttpServer[(][)][ \t\r\n]*[{][ \t\r\n]*stop[(][)][;][ \t\r\n]*try[ \t\r\n]*[{][ \t\r\n]*join[(][)]" OR
       NOT web_http_server_lifecycle MATCHES "finishStopOnContext[(][)]" OR
       NOT web_http_server_shutdown_test MATCHES
           "worker failed during graceful stop")
        boundary_error("HttpServer lifecycle bypasses its monotonic worker state"
            "startup completion must be discriminated and synchronized in one owner; shutdown must use the worker mailbox and distinguish running, draining, and stopped")
    endif()
endif()
file(GLOB_RECURSE EDGE_REFERENCE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/*.h" "${RUVIA_ROOT}/ruvia-core/*.cpp" "${RUVIA_ROOT}/ruvia-core/*.inl"
    "${RUVIA_ROOT}/ruvia-core/*.cmake" "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/*.h" "${RUVIA_ROOT}/ruvia-http/*.cpp" "${RUVIA_ROOT}/ruvia-http/*.inl"
    "${RUVIA_ROOT}/ruvia-http/*.cmake" "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/*.h" "${RUVIA_ROOT}/ruvia-web/*.cpp" "${RUVIA_ROOT}/ruvia-web/*.inl"
    "${RUVIA_ROOT}/ruvia-web/*.cmake" "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/*.h" "${RUVIA_ROOT}/examples/*.cpp" "${RUVIA_ROOT}/examples/*.inl"
    "${RUVIA_ROOT}/examples/*.cmake" "${RUVIA_ROOT}/examples/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl"
    "${RUVIA_ROOT}/tests/*.cmake" "${RUVIA_ROOT}/tests/CMakeLists.txt")
list(APPEND EDGE_REFERENCE_SOURCE
    "${RUVIA_ROOT}/CMakeLists.txt"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")

check_files_no_match("removed edge target is still referenced" "${RULE_EDGE}"
    ${EDGE_REFERENCE_SOURCE})
check_files_no_match("removed mixed-layer error API is still referenced"
    "${RULE_STALE_ERROR_API}" ${EDGE_REFERENCE_SOURCE})
check_files_no_match("ruvia-http must not reference asio" "${RULE_ASIO}"
    ${HTTP_SOURCE})
check_files_no_match("ruvia-http HTTP/2 core must not own Web request-body runtime"
    "${RULE_HTTP2_WEB_RUNTIME_IN_CORE}" ${HTTP_SOURCE})
# A protocol-level ResponseFileBody descriptor is allowed here; opening the
# described path through fstream/open/CreateFile remains a Web runtime concern.
check_files_no_match("ruvia-http must not perform OS file I/O (sans-I/O protocol lib)"
    "${RULE_FILE_IO}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not include core/web headers"
    "${RULE_HTTP_FRAMEWORK_INCLUDE}" ${HTTP_SOURCE})
check_files_no_match("Cookie option tokens must remain typed"
    "std::string_view[ \t]+(sameSite|priority)|std::int64_t[ \t]+maxAge"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Cookies.h")
set(COOKIE_OPTIONS_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Cookies.h")
if(EXISTS "${COOKIE_OPTIONS_HEADER}")
    file(READ "${COOKIE_OPTIONS_HEADER}" cookie_options_header)
    if(NOT cookie_options_header MATCHES
           "std::optional<CookiePrefix>[ \t]+prefix" OR
       NOT cookie_options_header MATCHES
           "std::optional<CookieSameSite>[ \t]+sameSite" OR
       NOT cookie_options_header MATCHES
           "std::optional<CookiePriority>[ \t]+priority")
        boundary_error("Cookie attribute absence lost its optional contract"
            "prefix, SameSite, and Priority absence must not be encoded as fake enum values")
    endif()
endif()
check_files_no_match("Cookie attribute enums restored absence sentinels"
    "CookiePrefix::kNone|CookieSameSite::kUnspecified|CookiePriority::kUnspecified|enum class CookiePrefix[^{]*[{][^}]*kNone|enum class CookieSameSite[^{]*[{][^}]*kUnspecified|enum class CookiePriority[^{]*[{][^}]*kUnspecified"
    "${COOKIE_OPTIONS_HEADER}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/CookieValidation.h"
    "${RUVIA_ROOT}/ruvia-http/src/Cookies.cpp"
    "${RUVIA_ROOT}/tests/unit_cookie_validation.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp")
set(HTTP_COOKIE_VALIDATION
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/CookieValidation.h")
set(HTTP_COOKIE_VALIDATION_TEST
    "${RUVIA_ROOT}/tests/unit_cookie_validation.cpp")
set(HTTP_COOKIE_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${HTTP_COOKIE_VALIDATION}" AND
   EXISTS "${HTTP_COOKIE_VALIDATION_TEST}" AND
   EXISTS "${HTTP_COOKIE_PACKAGE_CONSUMER}")
    file(READ "${HTTP_COOKIE_VALIDATION}" http_cookie_validation)
    file(READ "${HTTP_COOKIE_VALIDATION_TEST}"
        http_cookie_validation_test)
    file(READ "${HTTP_COOKIE_PACKAGE_CONSUMER}"
        http_cookie_package_consumer)
    if(NOT http_cookie_validation MATCHES
           "byte < 0x20 [|][|] byte > 0x7e" OR
       NOT http_cookie_validation MATCHES
           "cookieNameStartsWithIgnoreCase" OR
       NOT http_cookie_validation MATCHES
           "cookieNameStartsWithIgnoreCase[(]name, [\"]__Host-[\"]" OR
       NOT http_cookie_validation MATCHES
           "cookieNameStartsWithIgnoreCase[(]name, [\"]__Secure-[\"]" OR
       NOT http_cookie_validation_test MATCHES
           "cookie_path_octets_follow_set_cookie_grammar" OR
       NOT http_cookie_validation_test MATCHES
           "__SeCuRe-tok" OR
       NOT http_cookie_validation_test MATCHES
           "__hOsT-sid" OR
       NOT http_cookie_package_consumer MATCHES
           "cookieNameStartsWithIgnoreCase")
        boundary_error("Set-Cookie sender accepts values user agents reject"
            "Path must use av-octet and literal __Host-/__Secure- constraints must mirror UA case-insensitive matching")
    endif()
endif()
check_files_no_match("Set-Cookie wire serialization belongs to ruvia-http"
    "\";[ ]+(Path=|Domain=|Max-Age=|Expires=|HttpOnly|Secure|SameSite=|Priority=|Partitioned)"
    ${WEB_SOURCE})
check_files_no_match("ruvia-http client models must not contain runtime configuration"
    "${RULE_HTTP_CLIENT_RUNTIME_CONFIG}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h")
check_files_no_match("stale Fetch-shaped outbound client model must remain removed"
    "${RULE_STALE_HTTP_CLIENT_FETCH_MODEL}" ${EDGE_REFERENCE_SOURCE})
check_files_no_match("HTTP origin must use an immutable typed scheme"
    "${RULE_STALE_HTTP_ORIGIN_SHAPE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h")
check_files_no_match("HTTP origin validity must be established only by its factories"
    "${RULE_STALE_HTTP_ORIGIN_VALIDATION}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpOrigin.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpOrigin.cpp")
check_files_no_match("redirects must use the shared typed authority parser"
    "${RULE_DUPLICATE_HTTP_AUTHORITY_PARSER}"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp")
check_files_no_match("redirect authority validity must not collapse into origin equality"
    "${RULE_COLLAPSED_HTTP_ORIGIN_AUTHORITY}"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp")
check_files_no_match("redirect results must remain public discriminated values"
    "${RULE_STALE_HTTP_CLIENT_REDIRECT_RESULT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/unit_http_client_redirect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp")
check_files_no_match("host comparison must preserve encoded reserved characters"
    "${RULE_STALE_HTTP_HOST_PERCENT_NORMALIZATION}"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpRequestTarget.cpp")
check_files_no_match("public HTTP/1 parsing must keep discriminated outcomes"
    "${RULE_STALE_PUBLIC_HTTP1_PARSE_RESULT}"
    ${HTTP_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/api_surface.cpp"
    "${RUVIA_ROOT}/tests/smoke_http_target.cpp"
    "${RUVIA_ROOT}/tests/guards/cookie_api_guard.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/1 parse state must separate message and required bytes"
    "${RULE_STALE_HTTP1_PARSE_BYTE_OVERLOAD}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h")
file(GLOB_RECURSE HTTP1_PARSE_PHASE_REFERENCE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/examples/*.h" "${RUVIA_ROOT}/examples/*.cpp" "${RUVIA_ROOT}/examples/*.inl"
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl")
list(APPEND HTTP1_PARSE_PHASE_REFERENCE_SOURCE
    ${HTTP_SOURCE}
    ${WEB_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("HTTP/1 request-head and whole-message readiness were conflated again"
    "${RULE_STALE_HTTP1_SERVER_PARSE_PHASE}"
    ${HTTP1_PARSE_PHASE_REFERENCE_SOURCE})
check_files_no_match("loose or protocol-ambiguous HTTP/1 chunked result was restored"
    "${RULE_STALE_HTTP1_CHUNK_RESULT}"
    ${HTTP_SOURCE}
    ${WEB_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt")
check_files_no_match("HTTP byte ranges must use one discriminated resolution"
    "${RULE_STALE_HTTP_BYTE_RANGE_RESULT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpByteRange.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp"
    "${RUVIA_ROOT}/tests/unit_http_byte_range.cpp"
    "${RUVIA_ROOT}/tests/unit_content_range.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP method wire tokens must not collapse back into a closed enum"
    "${RULE_STALE_HTTP_METHOD_DOMAIN}"
    ${EDGE_REFERENCE_SOURCE})
check_files_no_match("valid extension methods must not be rejected by semantic classification"
    "${RULE_METHOD_CLASSIFICATION_REJECTION}"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")

set(HTTP_METHOD_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpKnownMethod.h")
set(HTTP_HEADER_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpHeader.h")
set(HTTP_MULTIPART_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/MultipartParser.h")
set(HTTP_MULTIPART_PART_ACCESS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/MultipartPartAccess.h")
set(HTTP_LEGACY_COMMON_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpCommon.h")
set(HTTP_LEGACY_COMMON_INTERNAL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpCommonInternal.h")
set(HTTP_LEGACY_TYPES_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpTypes.h")
set(HTTP_LEGACY_FRAME_UMBRELLA
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Frame.h")
set(WEB_LEGACY_SERVER_SESSION_UMBRELLA
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerSessionUtils.h")
set(WEB_LEGACY_REQUEST_BODY_UMBRELLA
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpRequestBody.h")
set(WEB_REQUEST_FIELDS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/RequestFields.h")
set(WEB_REQUEST_FIELDS_ACCESS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/RequestFieldsAccess.h")
set(WEB_REQUEST_QUERY_VALUES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/RequestQueryValues.h")
set(WEB_CONTEXT_REQUEST_STORAGE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextRequestStorage.h")
set(CORE_REQUEST_MEMORY
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/memory/MemoryPool.h")
set(HTTP_REQUEST_MODEL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpRequest.h")
set(HTTP1_REQUEST_PARSER
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
set(HTTP2_REQUEST_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h")
set(HTTP2_REQUEST_BUILDER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h")
set(WEB_ROUTER_DISPATCH
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
set(WEB_APP_PUBLIC_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/App.h")
set(WEB_SERVER_CONFIG_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ServerConfig.h")
set(WEB_SERVER_OPTIONS_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerOptions.h")
set(WEB_SERVER_CONFIG_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(WEB_LEGACY_PUBLIC_SERVER_OPTIONS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/HttpServerOptions.h")
foreach(method_contract_file IN ITEMS
        "${HTTP_METHOD_CONTRACT}"
        "${HTTP_REQUEST_MODEL}"
        "${HTTP1_REQUEST_PARSER}"
        "${HTTP2_REQUEST_HEADERS}"
        "${HTTP2_REQUEST_BUILDER}"
        "${WEB_ROUTER_DISPATCH}")
    if(NOT EXISTS "${method_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${method_contract_file}")
        boundary_error("HTTP method token/classification contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(NOT EXISTS "${HTTP_HEADER_CONTRACT}")
    boundary_error("HTTP header contract is incomplete"
        "ruvia-http/include/ruvia/http/HttpHeader.h is required")
else()
    file(READ "${HTTP_HEADER_CONTRACT}" http_header_contract)
    if(NOT http_header_contract MATCHES "class[ \t]+HttpHeaderView[ \t]+final" OR
       NOT http_header_contract MATCHES "kMaxHttpHeaderFields" OR
       NOT http_header_contract MATCHES "isValidHttpHeaderName" OR
       NOT http_header_contract MATCHES "isValidHttpHeaderValue")
        boundary_error("HTTP header contract lost a canonical primitive"
            "HttpHeader.h must own header views, limits and field validation")
    endif()
endif()
if(EXISTS "${HTTP_LEGACY_COMMON_HEADER}" OR
   EXISTS "${HTTP_LEGACY_COMMON_INTERNAL}" OR
   EXISTS "${HTTP_LEGACY_TYPES_HEADER}")
    boundary_error("Generic HTTP aggregation headers were restored"
        "request, response, multipart, method, header and Web field contracts must retain explicit owners")
endif()
if(EXISTS "${HTTP_LEGACY_FRAME_UMBRELLA}")
    boundary_error("Generic HTTP/2 frame aggregation was restored"
        "frame types, byte codec and payload parsing must retain explicit owners")
endif()
check_files_no_match("HTTP/2 code must not depend on a frame aggregation header"
    "Http2Frame[.]h"
    ${EDGE_REFERENCE_SOURCE})
check_files_no_match("App and server runtime must share one compression/CORS model"
    "${RULE_STALE_WEB_SERVER_CONFIG_SPLIT}"
    ${EDGE_REFERENCE_SOURCE})
if(EXISTS "${WEB_LEGACY_PUBLIC_SERVER_OPTIONS}")
    boundary_error("Worker runtime options leaked back into the public Web root"
        "public callers use ServerConfig.h; HttpServerOptions remains under detail/server")
endif()
if(EXISTS "${WEB_APP_PUBLIC_MODEL}" AND
   EXISTS "${WEB_SERVER_CONFIG_MODEL}" AND
   EXISTS "${WEB_SERVER_OPTIONS_MODEL}" AND
   EXISTS "${WEB_SERVER_CONFIG_PACKAGE_CONSUMER}")
    file(READ "${WEB_APP_PUBLIC_MODEL}" web_app_public_model)
    file(READ "${WEB_SERVER_CONFIG_MODEL}" web_server_config_model)
    file(READ "${WEB_SERVER_OPTIONS_MODEL}" web_server_options_model)
    file(READ "${WEB_SERVER_CONFIG_PACKAGE_CONSUMER}"
        web_server_config_package_consumer)
    if(web_app_public_model MATCHES
           "struct[ \t]+(CompressionConfig|CorsConfig)[ \t]+final" OR
       NOT web_server_config_model MATCHES
           "struct[ \t]+CompressionConfig[ \t]+final" OR
       NOT web_server_config_model MATCHES
           "struct[ \t]+CorsConfig[ \t]+final" OR
       NOT web_server_config_model MATCHES
           "std::optional<CorsMaxAge>[ \\t]+maxAge" OR
       NOT web_server_config_model MATCHES
           "using[ \\t]+Topology[ \\t]*=[ \\t]*std::variant" OR
       web_server_config_model MATCHES
           "enum class[ \\t]+Kind[^{]*[{][^}]*kHttpAndHttps" OR
       web_server_config_model MATCHES
           "std::optional<TlsConfig>[ \\t]+tls_" OR
       NOT web_server_config_model MATCHES "AccessLogCallback" OR
       NOT web_server_options_model MATCHES
           "namespace[ \t]+ruvia::detail" OR
       NOT web_server_options_model MATCHES
           "std::optional<CompressionConfig>[ \t]+compression" OR
       NOT web_server_options_model MATCHES
           "std::optional<CorsConfig>[ \t]+cors" OR
       web_server_config_model MATCHES
           "bool[ \t]+enabled" OR
       NOT web_server_config_package_consumer MATCHES
           "HasEmbeddedPolicyEnabledFlag" OR
       NOT web_server_config_package_consumer MATCHES
           "AppSetCompressionFunction" OR
       NOT web_server_config_package_consumer MATCHES
           "AppSetCorsFunction" OR
       NOT web_server_config_package_consumer MATCHES
           "decltype[(]ruvia::CorsConfig[{][}][.]maxAge[)]" OR
       NOT web_server_config_package_consumer MATCHES
           "HasLegacyCorsFields" OR
       NOT web_server_config_package_consumer MATCHES
           "HasLegacyStaticAllowAll" OR
       NOT web_server_config_package_consumer MATCHES
           "HasLegacyStaticFileTypesVector")
        boundary_error("Web server configuration regained parallel public models"
            "ServerConfig.h owns active policy values; detail/server/HttpServerOptions.h uses optional presence for enablement")
    endif()
endif()

set(WEB_SERVER_OPTIONS_VALIDATION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerOptionsValidation.h")
set(WEB_SERVER_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpServerLifecycle.cpp")
file(READ "${WEB_SERVER_OPTIONS_VALIDATION}" web_server_options_validation)
file(READ "${WEB_SERVER_LIFECYCLE}" web_server_lifecycle)
if(NOT web_server_options_validation MATCHES
       "options[.]workerMailboxCapacity" OR
   NOT web_server_options_validation MATCHES
       "options[.]shutdownGracePeriod" OR
   NOT web_server_options_validation MATCHES
       "TLS client certificate CA bundle must not be empty" OR
   NOT web_server_options_validation MATCHES
       "SNI hosts must be unique" OR
   NOT web_server_lifecycle MATCHES
       "validatedHttpServerOptions[(]std::move[(]options[)][)]" OR
   web_server_lifecycle MATCHES
       "ioContext_, options[.]workerMailboxCapacity")
    boundary_error("HttpServer consumed a partially validated runtime configuration"
        "All scalar and nested TLS options must be validated before member initialization")
endif()
if(EXISTS "${WEB_LEGACY_SERVER_SESSION_UMBRELLA}")
    boundary_error("Generic Web server session aggregation was restored"
        "runtime translation units and tests must include their actual session contracts")
endif()
if(EXISTS "${WEB_LEGACY_REQUEST_BODY_UMBRELLA}")
    boundary_error("Generic Web request-body aggregation was restored"
        "streaming and lazy-buffered runtimes must retain explicit owners")
endif()
check_files_no_match("Web runtime must not depend on ownerless aggregation headers"
    "Http(ServerSessionUtils|RequestBody)[.]h"
    ${EDGE_REFERENCE_SOURCE})
if(NOT EXISTS "${HTTP_MULTIPART_CONTRACT}" OR
   NOT EXISTS "${HTTP_MULTIPART_PART_ACCESS}")
    boundary_error("HTTP multipart part ownership is incomplete"
        "MultipartParser.h and detail/MultipartPartAccess.h are required")
else()
    file(READ "${HTTP_MULTIPART_CONTRACT}" http_multipart_contract)
    file(READ "${HTTP_MULTIPART_PART_ACCESS}" http_multipart_part_access)
    if(NOT http_multipart_contract MATCHES "class[ \t]+MultipartPart[ \t]+final" OR
       NOT http_multipart_contract MATCHES "class[ \t]+MultipartParser[ \t]+final" OR
       http_multipart_contract MATCHES
           "struct[ \t]+MultipartPartAccess[ \t]+final" OR
       NOT http_multipart_part_access MATCHES
           "struct[ \t]+MultipartPartAccess[ \t]+final")
        boundary_error("Buffered multipart parts escaped their protocol owner"
            "MultipartParser.h owns the read-only model and detail/MultipartPartAccess.h owns mutation")
    endif()
endif()
if(NOT EXISTS "${WEB_REQUEST_FIELDS}" OR
   NOT EXISTS "${WEB_REQUEST_FIELDS_ACCESS}" OR
   NOT EXISTS "${WEB_REQUEST_QUERY_VALUES}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_STORAGE}")
    boundary_error("Web request-field ownership is incomplete"
        "RequestFields.h, RequestFieldsAccess.h and RequestQueryValues.h are required")
else()
    file(READ "${WEB_REQUEST_FIELDS}" web_request_fields)
    file(READ "${WEB_REQUEST_FIELDS_ACCESS}" web_request_fields_access)
    file(READ "${WEB_REQUEST_QUERY_VALUES}" web_request_query_values)
    file(READ "${WEB_CONTEXT_REQUEST_STORAGE}" web_context_request_storage)
    foreach(request_field_type IN ITEMS
            RequestNameValueView
            RequestNameValueList)
        if(NOT web_request_fields MATCHES
               "class[ \t]+${request_field_type}[ \t]+final")
            boundary_error("Web request-field model lost a canonical type"
                "RequestFields.h must own ${request_field_type}")
        endif()
    endforeach()
    if(NOT web_request_fields MATCHES "pmrResourceOrDefault" OR
       web_request_fields MATCHES "httpPmrResourceOrDefault")
        boundary_error("Web request fields use the wrong memory-resource boundary"
            "RequestFields.h must resolve null resources through ruvia-core")
    endif()
    if(web_request_fields MATCHES
           "RequestValueGroup|RequestQueryValues" OR
       web_request_fields MATCHES
           "struct[ \t]+RequestNameValue(View|List)Access[ \t]+final" OR
       NOT web_request_fields_access MATCHES
           "struct[ \t]+RequestNameValueViewAccess[ \t]+final" OR
       NOT web_request_fields_access MATCHES
           "struct[ \t]+RequestNameValueListAccess[ \t]+final" OR
       web_request_fields_access MATCHES
           "Request(ValueGroup|QueryValues)")
        boundary_error("Web request-field mutation leaked into the public model"
            "only detail/http/RequestFieldsAccess.h may define construction and mutation access")
    endif()
    if(NOT web_request_query_values MATCHES
           "namespace[ \t]+ruvia::detail" OR
       NOT web_request_query_values MATCHES
           "class[ \t]+RequestQueryValues[ \t]+final" OR
       NOT web_request_query_values MATCHES
           "class[ \t]+RequestQueryCache[ \t]+final" OR
       web_request_query_values MATCHES
           "namespace[ \t]+ruvia[ \t]*[{]" OR
       web_request_query_values MATCHES
           "RequestValueGroup(List)?")
        boundary_error("Query multivalue indexing escaped its internal owner"
            "detail/http/RequestQueryValues.h must own the sole private query multivalue index")
    endif()
endif()
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    web_context_query_cache)
file(READ "${RUVIA_ROOT}/ruvia-web/src/http/ContextStorage.cpp"
    web_context_storage_impl)
file(READ "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp"
    web_context_request_impl)
file(READ "${CORE_REQUEST_MEMORY}" core_request_memory)
file(READ "${RUVIA_ROOT}/tests/unit_context_capabilities.cpp"
    context_capability_tests)
file(READ "${RUVIA_ROOT}/tests/unit_context_cookie.cpp"
    context_request_cache_tests)
file(READ "${RUVIA_ROOT}/tests/package-consumer/core.cpp"
    core_package_contract)
if(NOT web_context_query_cache MATCHES
       "RequestStorageOwner[ \t]+requestStorage_" OR
   NOT web_context_request_storage MATCHES
       "class ContextRequestStorage final" OR
   NOT web_context_request_storage MATCHES
       "std::optional<RequestQueryCache>[ \t]+query" OR
   NOT web_context_request_storage MATCHES
       "std::optional<RequestFieldCache>[ \t]+headers" OR
   NOT web_context_request_storage MATCHES
       "std::optional<RequestFieldCache>[ \t]+routeParams" OR
   NOT web_context_request_storage MATCHES
       "bool[ \t]+queryInvalid" OR
   NOT web_context_request_storage MATCHES
       "bool[ \t]+routeParamsInvalid" OR
   NOT web_context_query_cache MATCHES
       "PmrObjectDeleter<detail::ContextRequestStorage>" OR
   NOT web_context_storage_impl MATCHES
       "makePmrObject<detail::ContextRequestStorage>" OR
   NOT web_request_query_values MATCHES
       "vector<std::pmr::string>[ \t]+storage_" OR
   core_request_memory MATCHES
       "CleanupNode|cleanupHead_|void[ \t]*[(][*]destroy|T&[ \t]+emplace[(]" OR
   web_context_request_impl MATCHES "memory_[.]emplace" OR
   web_context_request_impl MATCHES
       "std::stable_sort|appendDecodedValue|decodedValues_" OR
   web_context_request_storage MATCHES
       "std::pmr::(list|forward_list|map|unordered_map)" OR
   NOT web_context_request_impl MATCHES
       "Context::requestQuery[(]std::string_view name[)] const [{][^}]*ensureRequestQuery[(][)]" OR
   NOT web_context_request_impl MATCHES
       "Context::routeParam[(]std::string_view name[)] const [{][^}]*ensureRouteParams[(][)]" OR
   web_context_query_cache MATCHES
       "(RequestQueryCache|RequestNameValueList|pmr::string)[*][ \t]+(requestQueryCache_|requestHeaders_|requestCookies_|routeParams_|decodedBody_)" OR
   web_context_query_cache MATCHES
       "RequestNameValueList[*][ \t]+requestQuery_" OR
   web_context_query_cache MATCHES
       "RequestQueryValues[*][ \t]+requestQueries_" OR
   NOT context_capability_tests MATCHES
       "context_lazy_request_caches_share_one_typed_storage_owner" OR
   NOT context_request_cache_tests MATCHES
       "context_request_query_single_lookup_materializes_one_shared_cache" OR
   NOT context_request_cache_tests MATCHES
       "context_request_param_single_lookup_materializes_one_shared_cache" OR
   NOT context_request_cache_tests MATCHES
       "context_request_query_rejects_and_remembers_malformed_percent_encoding" OR
   NOT context_request_cache_tests MATCHES
       "context_request_param_rejects_and_remembers_malformed_percent_encoding" OR
   NOT core_package_contract MATCHES
       "!HasErasedArenaEmplace<ruvia::RequestMemory>")
    boundary_error("request arena regained erased cleanup or split lazy-cache ownership"
        "RequestMemory must remain a pure arena; query/route scalar and field APIs must reuse one typed cache, remember decode failure, and avoid per-lookup nodes or non-PMR sort scratch")
endif()
if(EXISTS "${HTTP_METHOD_CONTRACT}" AND EXISTS "${HTTP_REQUEST_MODEL}")
    file(READ "${HTTP_METHOD_CONTRACT}" http_method_contract)
    file(READ "${HTTP_REQUEST_MODEL}" http_request_model)
    if(http_method_contract MATCHES
           "inline[ \t]+constexpr[ \t]+HttpKnownMethod[ \t]+(Get|Post|Put|Delete|Patch|Head|Options|Connect)")
        boundary_error("HTTP methods regained namespace-level aliases"
            "public method values must use the scoped HttpKnownMethod vocabulary")
    endif()
    if(NOT http_method_contract MATCHES "enum class HttpKnownMethod" OR
       NOT http_method_contract MATCHES "classifyHttpMethod" OR
       NOT http_method_contract MATCHES "isValidHttpMethodToken" OR
       NOT http_request_model MATCHES "std::string_view method[(][)] const noexcept" OR
       NOT http_request_model MATCHES "HttpKnownMethod knownMethod[(][)] const noexcept")
        boundary_error("HTTP request method lost raw-token/known-class separation"
            "HttpRequest::method must expose the exact token and knownMethod the fixed semantic class")
    endif()
endif()
if(EXISTS "${HTTP1_REQUEST_PARSER}" AND EXISTS "${HTTP2_REQUEST_HEADERS}" AND
   EXISTS "${HTTP2_REQUEST_BUILDER}" AND EXISTS "${WEB_ROUTER_DISPATCH}")
    file(READ "${HTTP1_REQUEST_PARSER}" http1_request_parser)
    file(READ "${HTTP2_REQUEST_HEADERS}" http2_request_headers)
    file(READ "${HTTP2_REQUEST_BUILDER}" http2_request_builder)
    file(READ "${WEB_ROUTER_DISPATCH}" web_router_dispatch)
    if(NOT http1_request_parser MATCHES "HttpRequestAccess::setMethod[(]state[.]request, method[)]" OR
       NOT http2_request_headers MATCHES "!isValidHttpMethodToken[(]value[)]" OR
       NOT http2_request_headers MATCHES "assignRequestMethod[(]value[)]" OR
       NOT http2_request_builder MATCHES "routeMethod" OR
       NOT http2_request_builder MATCHES "setMethod[(]request, method[)]" OR
       NOT web_router_dispatch MATCHES "knownMethod[(][)] == HttpKnownMethod::kUnknown" OR
       NOT web_router_dispatch MATCHES "HttpErrorInfo[(]501")
        boundary_error("HTTP method handling bypasses the shared extensible-token chain"
            "H1/H2 must preserve valid tokens, WS CONNECT may map only route lookup, and Web must render 501")
    endif()
endif()

set(WEB_ROUTE_MODES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteModes.h")
set(WEB_ROUTE_LIMITS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteLimits.h")
set(WEB_ROUTE_RESOLUTION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteResolution.h")
set(WEB_ROUTE_TABLE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h")
set(WEB_STALE_REQUEST_DISPATCHER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RequestDispatcher.h")
set(WEB_CONTROLLER_MACROS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Controller.h")
set(WEB_ROUTE_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_ROUTE_RESPONSE_STREAM_DISPATCH
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h")
set(WEB_ROUTE_WEBSOCKET_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSession.h")
set(WEB_ROUTE_HTTP1_WEBSOCKET
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h")
set(WEB_STALE_STREAM_KIND_ADAPTER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamKindAdapter.h")
set(WEB_ROUTE_RESOLUTION_TEST
    "${RUVIA_ROOT}/tests/unit_route_resolution.cpp")
set(CORE_OWNER_BORROW_TEST
    "${RUVIA_ROOT}/tests/smoke_core_target.cpp")
set(HTTP_OWNER_BORROW_TEST
    "${RUVIA_ROOT}/tests/unit_request_access.cpp")
set(WEB_OWNER_BORROW_TEST
    "${RUVIA_ROOT}/tests/unit_routing.cpp")
set(PUBLIC_OWNER_BORROW_API_SURFACE
    "${RUVIA_ROOT}/examples/api_surface.cpp")
set(HTTP_OWNER_BORROW_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(WEB_OWNER_BORROW_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(route_contract_file IN ITEMS
        "${WEB_ROUTE_MODES}"
        "${WEB_ROUTE_LIMITS}"
        "${WEB_ROUTE_RESOLUTION}"
        "${WEB_ROUTE_TABLE}"
        "${WEB_CONTROLLER_MACROS}"
        "${WEB_ROUTE_HTTP2_SESSION}"
        "${WEB_ROUTE_RESPONSE_STREAM_DISPATCH}"
        "${WEB_ROUTE_WEBSOCKET_SESSION}"
        "${WEB_ROUTE_HTTP1_WEBSOCKET}"
        "${WEB_ROUTE_RESOLUTION_TEST}")
    if(NOT EXISTS "${route_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${route_contract_file}")
        boundary_error("typed Web route contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${CORE_REQUEST_MEMORY}" AND
   EXISTS "${HTTP_REQUEST_MODEL}" AND
   EXISTS "${WEB_CONTROLLER_MACROS}" AND
   EXISTS "${CORE_OWNER_BORROW_TEST}" AND
   EXISTS "${HTTP_OWNER_BORROW_TEST}" AND
   EXISTS "${WEB_OWNER_BORROW_TEST}" AND
   EXISTS "${PUBLIC_OWNER_BORROW_API_SURFACE}" AND
   EXISTS "${HTTP_OWNER_BORROW_PACKAGE_CONSUMER}" AND
   EXISTS "${WEB_OWNER_BORROW_PACKAGE_CONSUMER}")
    file(READ "${CORE_REQUEST_MEMORY}" core_owner_borrow_contract)
    file(READ "${HTTP_REQUEST_MODEL}" http_owner_borrow_contract)
    file(READ "${WEB_CONTROLLER_MACROS}" web_owner_borrow_contract)
    file(READ "${CORE_OWNER_BORROW_TEST}" core_owner_borrow_test)
    file(READ "${HTTP_OWNER_BORROW_TEST}" http_owner_borrow_test)
    file(READ "${WEB_OWNER_BORROW_TEST}" web_owner_borrow_test)
    file(READ "${PUBLIC_OWNER_BORROW_API_SURFACE}"
        public_owner_borrow_api_surface)
    file(READ "${HTTP_OWNER_BORROW_PACKAGE_CONSUMER}"
        http_owner_borrow_package_consumer)
    file(READ "${WEB_OWNER_BORROW_PACKAGE_CONSUMER}"
        web_owner_borrow_package_consumer)
    if(NOT core_owner_borrow_contract MATCHES
           "allocator[(][)] &[ \t]+noexcept" OR
       NOT core_owner_borrow_contract MATCHES
           "allocator[(][)] &&[ \t]*=[ \t]*delete" OR
       NOT core_owner_borrow_contract MATCHES
           "resource[(][)] &[ \t]+noexcept" OR
       NOT core_owner_borrow_contract MATCHES
           "resource[(][)] const &[ \t]+noexcept" OR
       NOT core_owner_borrow_contract MATCHES
           "resource[(][)] &&[ \t]*=[ \t]*delete" OR
       NOT core_owner_borrow_contract MATCHES
           "resource[(][)] const &&[ \t]*=[ \t]*delete")
        boundary_error("RequestMemory exposes its arena from temporary owners"
            "allocator and resource must remain lvalue-only because they point into RequestMemory")
    endif()
    if(NOT http_owner_borrow_contract MATCHES
           "headers[(][)] const &[ \t]+noexcept" OR
       NOT http_owner_borrow_contract MATCHES
           "headers[(][)] const &&[ \t]*=[ \t]*delete")
        boundary_error("HttpRequest exposes its header table from temporary owners"
            "headers must remain lvalue-only because its span points into HttpRequest")
    endif()
    string(REGEX MATCHALL
        "(begin|end)[ \t\r\n]*[(][)][ \t\r\n]*const &[ \t]+noexcept"
        web_owner_borrow_lvalue_iterators
        "${web_owner_borrow_contract}")
    list(LENGTH web_owner_borrow_lvalue_iterators
        web_owner_borrow_lvalue_iterator_count)
    string(REGEX MATCHALL
        "(begin|end)[ \t\r\n]*[(][)][ \t\r\n]*const &&[ \t]*=[ \t]*delete"
        web_owner_borrow_deleted_rvalue_iterators
        "${web_owner_borrow_contract}")
    list(LENGTH web_owner_borrow_deleted_rvalue_iterators
        web_owner_borrow_deleted_rvalue_iterator_count)
    if(web_owner_borrow_lvalue_iterator_count LESS 4 OR
       web_owner_borrow_deleted_rvalue_iterator_count LESS 4)
        boundary_error("route macro lists expose iterators from temporary owners"
            "method and path list iterators must remain lvalue-only")
    endif()
    if(NOT web_owner_borrow_contract MATCHES
           "ruvia/web/detail/BorrowedView[.]h" OR
       NOT web_owner_borrow_contract MATCHES
           "RvalueCharBasicString<Paths>[ 	]*[|][|][ 	]*[.][.][.]" OR
       NOT web_owner_borrow_contract MATCHES
           "RuviaPathList[(]Paths&&[.][.][.][)][ 	]*=[ 	]*delete")
        boundary_error("route macro path lists accept temporary owning strings"
            "RUVIA_ON paths may borrow literals and owning-string lvalues, but must reject owning-string rvalues")
    endif()
    if(NOT core_owner_borrow_test MATCHES
           "static_assert[(]!ExposesRvalueRequestMemoryBorrow<ruvia::RequestMemory>[)]" OR
       NOT core_package_contract MATCHES
           "static_assert[(]!ExposesRvalueRequestMemoryBorrow<ruvia::RequestMemory>[)]" OR
       NOT http_owner_borrow_test MATCHES
           "static_assert[(]!ExposesRvalueHttpRequestHeaders<HttpRequest>[)]" OR
       NOT http_owner_borrow_package_consumer MATCHES
           "static_assert[(]!ExposesRvalueHttpRequestHeaders<ruvia::HttpRequest>[)]" OR
       NOT web_owner_borrow_test MATCHES
           "ExposesRvalueRouteListIterator<ruvia::detail::RuviaMethodList>" OR
       NOT web_owner_borrow_test MATCHES
           "ExposesRvalueRouteListIterator<ruvia::detail::RuviaPathList>" OR
       NOT web_owner_borrow_package_consumer MATCHES
           "ExposesRvalueRouteListIterator<[ \t\r\n]*ruvia::detail::RuviaMethodList>" OR
       NOT web_owner_borrow_package_consumer MATCHES
           "ExposesRvalueRouteListIterator<[ \t\r\n]*ruvia::detail::RuviaPathList>" OR
       NOT public_owner_borrow_api_surface MATCHES
           "static_assert[(]!ExposesRvalueRequestMemoryBorrow<ruvia::RequestMemory>[)]" OR
       NOT public_owner_borrow_api_surface MATCHES
           "static_assert[(]!ExposesRvalueHttpRequestHeaders<ruvia::HttpRequest>[)]" OR
       NOT public_owner_borrow_api_surface MATCHES
           "ExposesRvalueRouteListIterator<[ \t\r\n]*ruvia::detail::RuviaMethodList>" OR
       NOT public_owner_borrow_api_surface MATCHES
           "ExposesRvalueRouteListIterator<[ \t\r\n]*ruvia::detail::RuviaPathList>")
        boundary_error("owner-backed public borrow coverage is incomplete"
            "direct, API-surface, and installed-package probes must reject every confirmed temporary owner")
    endif()
    foreach(route_path_lifetime_coverage IN ITEMS
            "${web_owner_borrow_test}"
            "${web_owner_borrow_package_consumer}"
            "${public_owner_borrow_api_surface}")
        if(NOT route_path_lifetime_coverage MATCHES
               "static_assert[(]!AcceptsTemporaryRoutePath<std::string>[)]" OR
           NOT route_path_lifetime_coverage MATCHES
               "static_assert[(]!AcceptsTemporaryRoutePath<const std::string>[)]" OR
           NOT route_path_lifetime_coverage MATCHES
               "static_assert[(]!AcceptsTemporaryRoutePath<std::pmr::string>[)]")
            boundary_error("route path temporary-string coverage is incomplete"
                "direct, API-surface, and installed-package probes must reject standard and PMR owning-string rvalues")
            break()
        endif()
    endforeach()
endif()
if(EXISTS "${WEB_STALE_STREAM_KIND_ADAPTER}")
    boundary_error("split response route-mode adapter was restored"
        "stream sinks must consume the ResponseStreamKind owned by the typed endpoint")
endif()
if(EXISTS "${WEB_STALE_REQUEST_DISPATCHER}")
    boundary_error("request-time virtual route dispatcher was restored"
        "HTTP/1, HTTP/2, streaming, and WebSocket dispatch must call the concrete RouteTable directly")
endif()
if(EXISTS "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/RouteModes.h")
    boundary_error("route internals escaped detail/router ownership"
        "ruvia-web/include/ruvia/web/RouteModes.h must not restore the removed public-root path")
endif()
if(EXISTS "${WEB_ROUTE_MODES}" AND EXISTS "${WEB_ROUTE_LIMITS}" AND
   EXISTS "${WEB_ROUTE_RESOLUTION}" AND
   EXISTS "${WEB_ROUTE_TABLE}" AND
   EXISTS "${WEB_CONTROLLER_MACROS}" AND EXISTS "${WEB_ROUTE_HTTP2_SESSION}" AND
   EXISTS "${WEB_ROUTE_RESPONSE_STREAM_DISPATCH}" AND
   EXISTS "${WEB_ROUTE_WEBSOCKET_SESSION}" AND
   EXISTS "${WEB_ROUTE_HTTP1_WEBSOCKET}" AND
   EXISTS "${WEB_ROUTE_RESOLUTION_TEST}")
    file(READ "${WEB_ROUTE_MODES}" web_route_modes)
    file(READ "${WEB_ROUTE_LIMITS}" web_route_limits)
    file(READ "${WEB_ROUTE_RESOLUTION}" web_route_resolution)
    file(READ "${WEB_ROUTE_TABLE}" web_route_table)
    file(READ "${WEB_CONTROLLER_MACROS}" web_controller_macros)
    file(READ "${WEB_ROUTE_HTTP2_SESSION}" web_route_http2_session)
    file(READ "${WEB_ROUTE_RESPONSE_STREAM_DISPATCH}"
        web_route_response_stream_dispatch)
    file(READ "${WEB_ROUTE_WEBSOCKET_SESSION}"
        web_route_websocket_session)
    file(READ "${WEB_ROUTE_HTTP1_WEBSOCKET}"
        web_route_http1_websocket)
    file(READ "${WEB_ROUTE_RESOLUTION_TEST}" web_route_resolution_test)
    if(NOT web_route_modes MATCHES "enum class RequestBodyMode" OR
       web_route_modes MATCHES "kMaxRouteParams" OR
       NOT web_route_limits MATCHES
           "inline constexpr std::size_t kMaxRouteParams" OR
       web_route_limits MATCHES "namespace ruvia[ \t\r\n]*[{]" OR
       web_route_modes MATCHES "ResponseBodyMode" OR
       web_route_resolution MATCHES "RouteDisposition" OR
       web_route_table MATCHES "ResponseBodyMode" OR
       web_route_table MATCHES
           "resolve[ \t\r\n]*[(][^)]*RouteMatch[ \t]*&")
        boundary_error("Web routing restored handler/mode or caller-scratch split state"
            "endpoint kind, handler shape, route match, and resolution outcome must each have one owner")
    endif()
    if(NOT web_route_resolution MATCHES "class RouteNotFound final" OR
       NOT web_route_resolution MATCHES
           "class RouteMethodNotAllowed final" OR
       NOT web_route_resolution MATCHES "class ResolvedRoute final" OR
       NOT web_route_resolution MATCHES "using Value = std::variant" OR
       NOT web_route_resolution MATCHES "std::get_if<ResolvedRoute>" OR
       NOT web_route_resolution MATCHES
           "std::get_if<RouteMethodNotAllowed>" OR
       NOT web_route_resolution MATCHES "std::get_if<RouteNotFound>")
        boundary_error("RouteResolution lost its exclusive result alternatives"
            "resolved, 405, and 404 payloads must not be readable through a shared tuple")
    endif()
    if(NOT web_route_table MATCHES
           "class BufferedRouteEndpoint final" OR
       NOT web_route_table MATCHES
           "class ResponseStreamRouteEndpoint final" OR
       NOT web_route_table MATCHES
           "class WebSocketRouteEndpoint final" OR
       NOT web_route_table MATCHES "class RouteEndpoint final" OR
       NOT web_route_table MATCHES
           "std::get_if<BufferedRouteEndpoint>" OR
       NOT web_route_table MATCHES
           "std::get_if<ResponseStreamRouteEndpoint>" OR
       NOT web_route_table MATCHES
           "std::get_if<WebSocketRouteEndpoint>" OR
       NOT web_route_table MATCHES "RouteEndpoint endpoint_" OR
       NOT web_route_table MATCHES "const ResolvedRoute& route")
        boundary_error("route endpoint lost its discriminated handler contract"
            "buffered, response-stream, and WebSocket routes must bind handler and metadata in one alternative")
    endif()
    if(NOT web_route_table MATCHES "class RouteTable final[ \t\r\n]*[{]" OR
       web_route_table MATCHES "${RULE_STALE_REQUEST_DISPATCHER}")
        boundary_error("RouteTable lost its concrete request-time dispatch contract"
            "the startup-frozen route table must not inherit or expose a virtual dispatch interface")
    endif()
    if(NOT web_route_table MATCHES
           "RouteEndpoint& operator=[(]RouteEndpoint&&[)][ \t]*=[ \t]*delete" OR
       NOT web_route_table MATCHES
           "RouteEntry& operator=[(]RouteEntry&&[)][ \t]*=[ \t]*delete" OR
       NOT web_route_table MATCHES
           "RouteTable[(]RouteTable&&[)][ \t]*=[ \t]*delete" OR
       NOT web_route_table MATCHES
           "RouteTable& operator=[(]RouteTable&&[)][ \t]*=[ \t]*delete" OR
       NOT web_route_resolution_test MATCHES
           "!std::is_move_constructible_v<RouteTable>" OR
       NOT web_route_resolution_test MATCHES
           "!std::is_move_assignable_v<RouteEntry>")
        boundary_error("startup route graph regained relocatable identity"
            "the finalized RouteTable must be built at its final address, while endpoint and entry values may be move-constructed into reserved storage but never reassigned across resource domains")
    endif()
    if(NOT web_route_http2_session MATCHES "const RouteTable& routes" OR
       NOT web_route_response_stream_dispatch MATCHES
           "const RouteTable& routes" OR
       NOT web_route_http1_websocket MATCHES "const RouteTable& routes")
        boundary_error("Web runtime bypasses the concrete RouteTable dispatch chain"
            "HTTP/2, response streaming, and WebSocket sessions must receive the startup-frozen RouteTable directly")
    endif()
    if(NOT web_route_http1_websocket MATCHES
           "auto upgradeAndRun" OR
       NOT web_route_http1_websocket MATCHES
           "Context& context" OR
       NOT web_route_http1_websocket MATCHES
           "routes[.]dispatchWebSocket" OR
       NOT web_route_http1_websocket MATCHES
           "makeCallableRef<void, Context&>" OR
       NOT web_route_http2_session MATCHES
           "auto upgradeAndRun" OR
       NOT web_route_http2_session MATCHES
           "Context& context" OR
       NOT web_route_http2_session MATCHES
           "routes[.]dispatchWebSocket" OR
       NOT web_route_http2_session MATCHES
           "makeCallableRef<void, Context&>" OR
       NOT web_route_http1_websocket MATCHES
           "finishWebSocketSession" OR
       NOT web_route_http2_session MATCHES
           "finishWebSocketSession" OR
       NOT web_route_websocket_session MATCHES
           "Task<void> invokeWebSocketHandler" OR
       NOT web_route_websocket_session MATCHES
           "Task<void> finishWebSocketSession" OR
       NOT web_route_websocket_session MATCHES
           "ContextWebSocketBinding webSocketBinding" OR
       web_route_websocket_session MATCHES
           "dispatchWebSocket|const RouteTable&|runWebSocketSession")
        boundary_error("WebSocket middleware no longer owns the upgrade boundary"
            "HTTP/1 and HTTP/2 must dispatch middleware around a terminal upgrade/session action so short-circuit responses remain HTTP")
    endif()
    if(NOT web_controller_macros MATCHES
           "RuviaControllerAccess::addResponseStreamRoute" OR
       NOT web_controller_macros MATCHES
           "RuviaControllerAccess::addSseRoute" OR
       NOT web_controller_macros MATCHES
           "RuviaControllerAccess::addWebSocketRoute" OR
       NOT web_route_resolution_test MATCHES
           "route_endpoint_binds_handler_shape_and_only_relevant_metadata" OR
       NOT web_route_resolution_test MATCHES
           "route_endpoint_rejects_empty_handlers_and_invalid_discriminants" OR
       NOT web_route_resolution_test MATCHES
           "route_resolution_method_not_allowed_vs_not_found")
        boundary_error("typed route contract lacks registration or regression coverage"
            "distinct macro paths and value-level endpoint/resolution tests must remain")
    endif()
endif()
set(WEB_CONTROLLER_DESCRIPTORS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/controller/ControllerDescriptors.h")
set(WEB_CONTROLLER_RUNTIME
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/controller/ControllerRuntime.h")
set(WEB_CONTROLLER_REGISTRY
    "${RUVIA_ROOT}/ruvia-web/src/router/ControllerRegistry.cpp")
set(WEB_CONTROLLER_CMAKE
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt")
foreach(stale_controller_file IN ITEMS
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ControllerDescriptors.h"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ControllerRuntime.h"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ControllerTypes.h"
        "${RUVIA_ROOT}/ruvia-web/src/http/Controller.cpp")
    if(EXISTS "${stale_controller_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${stale_controller_file}")
        boundary_error("Controller internals escaped their detail/router ownership"
            "${relative} must not restore the removed public-root or HTTP-layer path")
    endif()
endforeach()
if(NOT EXISTS "${WEB_CONTROLLER_DESCRIPTORS}" OR
   NOT EXISTS "${WEB_CONTROLLER_RUNTIME}" OR
   NOT EXISTS "${WEB_CONTROLLER_REGISTRY}" OR
   NOT EXISTS "${WEB_CONTROLLER_CMAKE}")
    boundary_error("Controller internal ownership is incomplete"
        "detail/controller headers and src/router/ControllerRegistry.cpp must remain")
elseif(EXISTS "${WEB_CONTROLLER_MACROS}")
    file(READ "${WEB_CONTROLLER_MACROS}" web_controller_macros)
    file(READ "${WEB_CONTROLLER_RUNTIME}" web_controller_runtime)
    file(READ "${WEB_CONTROLLER_CMAKE}" web_controller_cmake)
    if(NOT web_controller_macros MATCHES
           "ruvia/web/detail/controller/ControllerRuntime[.]h" OR
       NOT web_controller_runtime MATCHES
           "ruvia/web/detail/controller/ControllerDescriptors[.]h" OR
       NOT web_controller_cmake MATCHES
           "src/router/ControllerRegistry[.]cpp" OR
       NOT web_controller_macros MATCHES
           "using RuviaControllerAccess" OR
       NOT web_controller_macros MATCHES
           "friend class ::ruvia::detail::ControllerRegistrationAccess<RuviaControllerType>" OR
       NOT web_controller_runtime MATCHES
           "class ControllerRegistrationAccess final" OR
       NOT web_controller_runtime MATCHES "friend ControllerT;" OR
       NOT web_controller_runtime MATCHES
           "ControllerRegistrationAccess<ControllerT>::registerRoutes" OR
       web_controller_runtime MATCHES
           "ruvia(CreateRouteGroup|AddRoute|AddResponseStreamRoute|AddSseRoute|AddWebSocketRoute|MakeMiddleware|MakeMiddlewares)")
        boundary_error("Controller registration escaped its macro-only access chain"
            "generated hooks and route-builder operations must remain private behind ControllerRegistrationAccess")
    endif()
endif()
set(WEB_MIDDLEWARE_PUBLIC
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Middleware.h")
set(WEB_MIDDLEWARE_DESCRIPTOR
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/middleware/MiddlewareDescriptor.h")
set(WEB_MIDDLEWARE_REGISTRATION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/middleware/MiddlewareRegistration.h")
set(WEB_MIDDLEWARE_GUARD
    "${RUVIA_ROOT}/tests/guards/middleware_next_guard.cpp")
set(WEB_MIDDLEWARE_EXAMPLE
    "${RUVIA_ROOT}/examples/middleware_next.cpp")
foreach(stale_middleware_header IN ITEMS
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/MiddlewareDescriptor.h"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/MiddlewareRuntime.h")
    if(EXISTS "${stale_middleware_header}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${stale_middleware_header}")
        boundary_error("middleware registration internals escaped detail ownership"
            "${relative} must not restore the removed public-root path")
    endif()
endforeach()
if(NOT EXISTS "${WEB_MIDDLEWARE_PUBLIC}" OR
   NOT EXISTS "${WEB_MIDDLEWARE_DESCRIPTOR}" OR
   NOT EXISTS "${WEB_MIDDLEWARE_REGISTRATION}" OR
   NOT EXISTS "${WEB_MIDDLEWARE_GUARD}" OR
   NOT EXISTS "${WEB_MIDDLEWARE_EXAMPLE}")
    boundary_error("middleware API/registration split is incomplete"
        "Middleware.h, detail/middleware contracts, and the signature guard must remain")
else()
    file(READ "${WEB_MIDDLEWARE_PUBLIC}" web_middleware_public)
    file(READ "${WEB_MIDDLEWARE_DESCRIPTOR}" web_middleware_descriptor)
    file(READ "${WEB_MIDDLEWARE_REGISTRATION}" web_middleware_registration)
    file(READ "${WEB_MIDDLEWARE_GUARD}" web_middleware_guard)
    file(READ "${WEB_MIDDLEWARE_EXAMPLE}" web_middleware_example)
    if(NOT web_middleware_public MATCHES "class Middleware" OR
       web_middleware_public MATCHES
           "namespace detail|ControllerMiddlewareDescriptor|invokeMiddleware|Context[.]h" OR
       NOT web_middleware_descriptor MATCHES
           "class ControllerMiddlewareDescriptor final" OR
       NOT web_middleware_registration MATCHES
           "concept VoidHandleMiddleware" OR
       NOT web_middleware_registration MATCHES
           "concept ResponseHandleMiddleware" OR
       NOT web_middleware_registration MATCHES
           "makeMiddlewareDescriptor" OR
       NOT web_middleware_guard MATCHES
           "!ruvia::detail::VoidHandleMiddleware<ByValueNextMiddleware>" OR
       NOT web_middleware_guard MATCHES
           "!HasPublicNextRuntimeState<ruvia::Next>" OR
       web_middleware_example MATCHES
           "ruvia::detail::(VoidHandleMiddleware|ResponseHandleMiddleware)|detail/middleware")
        boundary_error("middleware public API and registration implementation were mixed"
            "the public header must contain only the CRTP marker; signature/factory checks belong in detail and guard tests")
    endif()
endif()
set(WEB_NEXT_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Next.h")
if(EXISTS "${WEB_NEXT_CONTRACT}")
    file(READ "${WEB_NEXT_CONTRACT}" web_next_contract)
    if(NOT web_next_contract MATCHES "enum class Invocation" OR
       NOT web_next_contract MATCHES "enum class Phase" OR
       web_next_contract MATCHES "bool[ \t]+(invoked|active|repeated|awaited_)")
        boundary_error("middleware Next regained parallel boolean lifecycle state"
            "Next invocation, scope expiry, and repeated calls must remain typed phases")
    endif()
endif()
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouterInternal.h"
    route_final_storage_internal)
file(READ "${RUVIA_ROOT}/ruvia-web/src/router/RouterBuild.cpp"
    route_final_storage_build)
file(READ "${RUVIA_ROOT}/ruvia-web/src/router/Router.cpp"
    route_final_storage_finalize)
file(READ "${RUVIA_ROOT}/tests/package-consumer/web.cpp"
    route_final_storage_package)
if(NOT route_final_storage_internal MATCHES
       "void buildRouteTable[(]RouteTable& table[)] const" OR
   route_final_storage_internal MATCHES
       "RouteTable buildRouteTable[(][)] const" OR
   NOT route_final_storage_build MATCHES
       "void detail::RouterImpl::buildRouteTable[(]RouteTable& table[)] const" OR
   route_final_storage_build MATCHES
       "RouteTable table[(]resource_[)]" OR
   NOT route_final_storage_finalize MATCHES
       "constructPmrObject<RouteTable>[(]resource_, resource_[)]" OR
   NOT route_final_storage_finalize MATCHES
       "buildRouteTable[(][*]table[)]" OR
   route_final_storage_finalize MATCHES
       "constructPmrObject<RouteTable>[(]resource_, buildRouteTable" OR
   NOT route_final_storage_package MATCHES
       "!std::is_move_constructible_v<[ \n\t]*ruvia::detail::RouteTable>")
    boundary_error("RouteTable is no longer built in final storage"
        "Router finalize must allocate the address-stable table first, build every self-referential index there, and publish it only after successful completion")
endif()
check_files_no_match("Web routing restored split endpoint or resolution APIs"
    "${RULE_STALE_ROUTE_MODE_SPLIT}|${RULE_STALE_ROUTE_RESOLUTION_TUPLE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Controller.h"
    "${WEB_CONTROLLER_DESCRIPTORS}"
    "${WEB_CONTROLLER_RUNTIME}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouterInternal.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterBuild.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterIndex.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterRegistration.cpp")
check_files_no_match("Web routing restored request-time virtual dispatch"
    "${RULE_STALE_REQUEST_DISPATCHER}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSession.h")

set(WEB_CONTEXT_CAPABILITIES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextCapabilities.h")
set(WEB_CONTEXT_SERVICES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextServices.h")
set(WEB_CONTEXT_INTERNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextInternal.h")
set(WEB_CONTEXT_CAPABILITY_CONTEXT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h")
set(WEB_CONTEXT_REQUEST_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
set(WEB_CONTEXT_RUNTIME_FACADES
    "${RUVIA_ROOT}/ruvia-web/src/http/HttpRuntimeFacades.cpp")
set(WEB_CONTEXT_ROUTER_DISPATCH
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
set(WEB_CONTEXT_LAZY_BODY_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h")
set(WEB_CONTEXT_STREAM_BODY_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h")
set(WEB_CONTEXT_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_CONTEXT_CAPABILITY_TEST
    "${RUVIA_ROOT}/tests/unit_context_capabilities.cpp")
set(WEB_CONTEXT_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(context_capability_file IN ITEMS
        "${WEB_CONTEXT_CAPABILITIES}"
        "${WEB_CONTEXT_SERVICES}"
        "${WEB_CONTEXT_INTERNAL}"
        "${WEB_CONTEXT_CAPABILITY_CONTEXT}"
        "${WEB_CONTEXT_REQUEST_SOURCE}"
        "${WEB_CONTEXT_RUNTIME_FACADES}"
        "${WEB_CONTEXT_ROUTER_DISPATCH}"
        "${WEB_CONTEXT_LAZY_BODY_ROUTE}"
        "${WEB_CONTEXT_STREAM_BODY_ROUTE}"
        "${WEB_CONTEXT_HTTP2_SESSION}"
        "${WEB_CONTEXT_CAPABILITY_TEST}"
        "${WEB_CONTEXT_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${context_capability_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${context_capability_file}")
        boundary_error("typed Context capability contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_CONTEXT_CAPABILITIES}" AND
   EXISTS "${WEB_CONTEXT_SERVICES}" AND
   EXISTS "${WEB_CONTEXT_INTERNAL}" AND
   EXISTS "${WEB_CONTEXT_CAPABILITY_CONTEXT}" AND
   EXISTS "${WEB_CONTEXT_REQUEST_SOURCE}" AND
   EXISTS "${WEB_CONTEXT_RUNTIME_FACADES}" AND
   EXISTS "${WEB_CONTEXT_ROUTER_DISPATCH}" AND
   EXISTS "${WEB_CONTEXT_LAZY_BODY_ROUTE}" AND
   EXISTS "${WEB_CONTEXT_STREAM_BODY_ROUTE}" AND
   EXISTS "${WEB_CONTEXT_HTTP2_SESSION}" AND
   EXISTS "${WEB_CONTEXT_CAPABILITY_TEST}" AND
   EXISTS "${WEB_CONTEXT_PACKAGE_CONSUMER}")
    file(READ "${WEB_CONTEXT_CAPABILITIES}" web_context_capabilities)
    file(READ "${WEB_CONTEXT_SERVICES}" web_context_services)
    file(READ "${WEB_CONTEXT_INTERNAL}" web_context_internal)
    file(READ "${WEB_CONTEXT_CAPABILITY_CONTEXT}" web_context_header)
    file(READ "${WEB_CONTEXT_REQUEST_SOURCE}" web_context_request_source)
    file(READ "${WEB_CONTEXT_RUNTIME_FACADES}" web_context_runtime_facades)
    file(READ "${WEB_CONTEXT_ROUTER_DISPATCH}" web_context_router_dispatch)
    file(READ "${WEB_CONTEXT_LAZY_BODY_ROUTE}" web_context_lazy_body_route)
    file(READ "${WEB_CONTEXT_STREAM_BODY_ROUTE}" web_context_stream_body_route)
    file(READ "${WEB_CONTEXT_HTTP2_SESSION}" web_context_http2_session)
    file(READ "${WEB_CONTEXT_CAPABILITY_TEST}" web_context_capability_test)
    file(READ "${WEB_CONTEXT_PACKAGE_CONSUMER}" web_context_package_consumer)
    if(NOT web_context_capabilities MATCHES
           "class ContextBufferedRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "class ContextLazyRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "class ContextStreamingRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "class ContextRequestBodySource final" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextBufferedRequestBodySource>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextLazyRequestBodySource>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextStreamingRequestBodySource>" OR
       NOT web_context_capabilities MATCHES
           "class ContextBufferedResponseOutput final" OR
       NOT web_context_capabilities MATCHES
           "class ContextResponseStreamOutput final" OR
       NOT web_context_capabilities MATCHES
           "class ContextWebSocketOutput final" OR
       NOT web_context_capabilities MATCHES
           "class ContextResponseOutput final" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextBufferedResponseOutput>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextResponseStreamOutput>" OR
       NOT web_context_capabilities MATCHES
           "std::get_if<ContextWebSocketOutput>")
        boundary_error("Context capabilities lost their exclusive alternatives"
            "request body and response output must each be one explicit discriminated value")
    endif()
    string(REGEX MATCHALL
        "(buffered|lazy|streaming|responseStream|webSocket)[ \t\r\n]*[(][)][ \t\r\n]*const[ \t\r\n]*&[ \t\r\n]*noexcept"
        context_capability_lvalue_accessors
        "${web_context_capabilities}")
    list(LENGTH context_capability_lvalue_accessors
        context_capability_lvalue_accessor_count)
    string(REGEX MATCHALL
        "(buffered|lazy|streaming|responseStream|webSocket)[ \t\r\n]*[(][)][ \t\r\n]*const[ \t\r\n]*&&[ \t\r\n]*=[ \t\r\n]*delete"
        context_capability_deleted_rvalue_accessors
        "${web_context_capabilities}")
    list(LENGTH context_capability_deleted_rvalue_accessors
        context_capability_deleted_rvalue_accessor_count)
    if(context_capability_lvalue_accessor_count LESS 6 OR
       context_capability_deleted_rvalue_accessor_count LESS 6)
        boundary_error("Context capability alternatives expose temporary owners"
            "all request-body and response-output alternatives must be lvalue-only")
    endif()
    if(web_context_services MATCHES
           "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}" OR
       web_context_services MATCHES "withWebSocket" OR
       web_context_header MATCHES
           "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}" OR
       web_context_internal MATCHES
           "${RULE_STALE_CONTEXT_CAPABILITY_SPLIT}" OR
       NOT web_context_services MATCHES
           "ContextRequestBodySource requestBodySource_" OR
       NOT web_context_services MATCHES
           "ContextResponseOutput responseOutput_" OR
       NOT web_context_services MATCHES
           "const WorkerHandle[*] worker_" OR
       web_context_services MATCHES
           "(^|[^*])WorkerHandle worker_" OR
       web_context_services MATCHES "constexpr ContextServices" OR
       NOT web_context_header MATCHES
           "const WorkerHandle& worker_" OR
       web_context_header MATCHES
           "std::shared_ptr<[^>]*WorkerHandle|std::weak_ptr<[^>]*WorkerHandle" OR
       NOT web_context_services MATCHES "withLazyRequestBody" OR
       NOT web_context_services MATCHES "withStreamingRequestBody" OR
       NOT web_context_header MATCHES
           "ContextRequestBodySource requestBodySource_" OR
       NOT web_context_header MATCHES
           "ContextResponseOutput responseOutput_" OR
       NOT web_context_internal MATCHES
           "services[.]requestBodySource[(][)]" OR
       NOT web_context_internal MATCHES
           "services[.]responseOutput[(][)]")
        boundary_error("Context restored owning worker-handle copies or invalid capability ownership"
            "request-local ContextServices and Context must borrow the address-stable server WorkerHandle while carrying discriminated protocol capabilities, so derived service values never contend on shared ownership")
    endif()
    if(NOT web_context_request_source MATCHES
           "requestBodySource_[.]lazy[(][)]" OR
       NOT web_context_request_source MATCHES
           "requestBodySource_[.]streaming[(][)]" OR
       NOT web_context_runtime_facades MATCHES
           "responseOutput_[.]responseStream[(][)]" OR
       NOT web_context_runtime_facades MATCHES
           "responseOutput_[.]webSocket[(][)]" OR
       NOT web_context_router_dispatch MATCHES
           "services[.]responseOutput[(][)][.]responseStream[(][)]" OR
       NOT web_context_internal MATCHES
           "class ContextWebSocketBinding final" OR
       NOT web_context_internal MATCHES
           "restoreResponseOutput" OR
       NOT web_context_lazy_body_route MATCHES "withLazyRequestBody" OR
       NOT web_context_stream_body_route MATCHES
           "withStreamingRequestBody" OR
       NOT web_context_http2_session MATCHES "withStreamingRequestBody")
        boundary_error("Context runtime bypasses the typed capability chain"
            "H1, H2, router dispatch, and public Context access must consume the active alternatives")
    endif()
    if(NOT web_context_capability_test MATCHES
           "context_request_body_source_has_one_active_alternative" OR
       NOT web_context_capability_test MATCHES
           "context_response_output_has_one_active_alternative" OR
       NOT web_context_capability_test MATCHES
           "context_copies_typed_capabilities_into_public_facades" OR
       NOT web_context_package_consumer MATCHES
           "HasSplitContextCapabilityAccessors" OR
       NOT web_context_capability_test MATCHES
           "ExposesRvalueRequestBodyAlternative" OR
       NOT web_context_capability_test MATCHES
           "ExposesRvalueResponseOutputAlternative" OR
       NOT web_context_package_consumer MATCHES
           "ExposesRvalueRequestBodyAlternative" OR
       NOT web_context_package_consumer MATCHES
           "ExposesRvalueResponseOutputAlternative" OR
       NOT web_context_package_consumer MATCHES
           "ContextRequestBodySource" OR
       NOT web_context_package_consumer MATCHES "ContextResponseOutput")
        boundary_error("typed Context capabilities lack regression coverage"
            "unit and installed-package tests must pin exclusivity, propagation, temporary-owner safety, and removal of split accessors")
    endif()
endif()

set(WEB_CONTENT_DECODE_SERVER_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerSessionEntry.inl")
if(EXISTS "${WEB_CONTENT_DECODE_SERVER_ENTRY}" AND
   EXISTS "${WEB_CONTEXT_SERVICES}" AND
   EXISTS "${WEB_CONTEXT_INTERNAL}" AND
   EXISTS "${WEB_CONTEXT_REQUEST_SOURCE}")
    file(READ "${WEB_CONTENT_DECODE_SERVER_ENTRY}"
        web_content_decode_server_entry)
    if(NOT web_content_decode_server_entry MATCHES
           "options_[.]maxBufferedBodyBytes" OR
       NOT web_context_services MATCHES "maxDecodedBodyBytes" OR
       NOT web_context_internal MATCHES
           "maxDecodedBodyBytes_[(]services[.]maxDecodedBodyBytes[(][)][)]" OR
       NOT web_context_request_source MATCHES
           "decodeHttpRequestContent" OR
       NOT web_context_request_source MATCHES
           "maxDecodedBodyBytes_")
        boundary_error("Web decoded-body limit is not wired end to end"
            "the configured buffered-body limit must reach the HTTP decoder through ContextServices")
    endif()
endif()

set(WEB_CONN_INFO
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ConnInfo.h")
set(WEB_CONN_CONTEXT_SERVICES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextServices.h")
set(WEB_CONN_CONTEXT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h")
set(WEB_CONN_CONTEXT_INTERNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextInternal.h")
set(WEB_CONN_CONTEXT_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
set(WEB_CONN_SERVER_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerSessionEntry.inl")
set(WEB_CONN_HTTP1_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_CONN_HTTP2_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerCleartextHttp2.h")
set(WEB_CONN_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_CONN_TEST "${RUVIA_ROOT}/tests/unit_conn_info.cpp")
set(WEB_CONN_TLS_TEST "${RUVIA_ROOT}/tests/unit_sansio_tls.cpp")
set(WEB_CONN_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(WEB_CONN_API_SURFACE "${RUVIA_ROOT}/examples/api_surface.cpp")
foreach(conn_info_contract_file IN ITEMS
        "${WEB_CONN_INFO}"
        "${WEB_CONN_CONTEXT_SERVICES}"
        "${WEB_CONN_CONTEXT}"
        "${WEB_CONN_CONTEXT_INTERNAL}"
        "${WEB_CONN_CONTEXT_SOURCE}"
        "${WEB_CONN_SERVER_ENTRY}"
        "${WEB_CONN_HTTP1_SESSION}"
        "${WEB_CONN_HTTP2_ENTRY}"
        "${WEB_CONN_HTTP2_SESSION}"
        "${WEB_CONN_TEST}"
        "${WEB_CONN_TLS_TEST}"
        "${WEB_CONN_PACKAGE_CONSUMER}"
        "${WEB_CONN_API_SURFACE}")
    if(NOT EXISTS "${conn_info_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${conn_info_contract_file}")
        boundary_error("typed connection metadata contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_CONN_INFO}" AND
   EXISTS "${WEB_CONN_CONTEXT_SERVICES}" AND
   EXISTS "${WEB_CONN_CONTEXT}" AND
   EXISTS "${WEB_CONN_CONTEXT_INTERNAL}" AND
   EXISTS "${WEB_CONN_CONTEXT_SOURCE}" AND
   EXISTS "${WEB_CONN_SERVER_ENTRY}" AND
   EXISTS "${WEB_CONN_HTTP1_SESSION}" AND
   EXISTS "${WEB_CONN_HTTP2_ENTRY}" AND
   EXISTS "${WEB_CONN_HTTP2_SESSION}" AND
   EXISTS "${WEB_CONN_TEST}" AND
   EXISTS "${WEB_CONN_TLS_TEST}" AND
   EXISTS "${WEB_CONN_PACKAGE_CONSUMER}" AND
   EXISTS "${WEB_CONN_API_SURFACE}")
    file(READ "${WEB_CONN_INFO}" web_conn_info)
    file(READ "${WEB_CONN_CONTEXT_SERVICES}" web_conn_services)
    file(READ "${WEB_CONN_CONTEXT}" web_conn_context)
    file(READ "${WEB_CONN_CONTEXT_INTERNAL}" web_conn_context_internal)
    file(READ "${WEB_CONN_CONTEXT_SOURCE}" web_conn_context_source)
    file(READ "${WEB_CONN_SERVER_ENTRY}" web_conn_server_entry)
    file(READ "${WEB_CONN_HTTP1_SESSION}" web_conn_http1_session)
    file(READ "${WEB_CONN_HTTP2_ENTRY}" web_conn_http2_entry)
    file(READ "${WEB_CONN_HTTP2_SESSION}" web_conn_http2_session)
    file(READ "${WEB_CONN_TEST}" web_conn_test)
    file(READ "${WEB_CONN_TLS_TEST}" web_conn_tls_test)
    file(READ "${WEB_CONN_PACKAGE_CONSUMER}" web_conn_package_consumer)
    file(READ "${WEB_CONN_API_SURFACE}" web_conn_api_surface)
    if(NOT web_conn_info MATCHES
           "class PlainConnectionTransport final" OR
       NOT web_conn_info MATCHES
           "class TlsConnectionTransport final" OR
       NOT web_conn_info MATCHES "class ConnInfo final" OR
       NOT web_conn_info MATCHES
           "std::variant<PlainConnectionTransport, TlsConnectionTransport>" OR
       NOT web_conn_info MATCHES
           "std::get_if<PlainConnectionTransport>" OR
       NOT web_conn_info MATCHES
           "std::get_if<TlsConnectionTransport>" OR
       NOT web_conn_info MATCHES
           "TlsConnectionTransport[\t\r\n ]+transport" OR
       web_conn_info MATCHES "secure[ \t\r\n]*[(]" OR
       web_conn_info MATCHES
           "std::(function|shared_ptr|unique_ptr)")
        boundary_error("ConnInfo lost its exclusive transport alternatives"
            "plain or TLS must be one allocation-free discriminated value, with certificate identity owned only by TLS")
    endif()
    string(REGEX MATCHALL "const && = delete"
        conn_deleted_rvalue_accessors "${web_conn_info}")
    list(LENGTH conn_deleted_rvalue_accessors
        conn_deleted_rvalue_accessor_count)
    if(conn_deleted_rvalue_accessor_count LESS 2 OR
       NOT web_conn_test MATCHES "ExposesRvalueTransportPointer" OR
       NOT web_conn_package_consumer MATCHES
           "ExposesRvalueTransportPointer" OR
       NOT web_conn_api_surface MATCHES
           "HasRvalueConnInfoTransportAccess")
        boundary_error("ConnInfo exposes alternative pointers from temporaries"
            "plain/tls pointer access must remain lvalue-only in source, installed consumers, and the public API surface")
    endif()
    if(web_conn_services MATCHES "${RULE_STALE_CONN_INFO_SCALARS}" OR
       web_conn_context MATCHES "${RULE_STALE_CONN_INFO_SCALARS}" OR
       web_conn_context_internal MATCHES
           "${RULE_STALE_CONN_INFO_SCALARS}" OR
       web_conn_context_source MATCHES
           "${RULE_STALE_CONN_INFO_SCALARS}" OR
       NOT web_conn_services MATCHES "const ConnInfo& connInfo" OR
       NOT web_conn_services MATCHES "withPlainTransport" OR
       NOT web_conn_services MATCHES "withTlsTransport" OR
       NOT web_conn_services MATCHES "ConnInfo connInfo_" OR
       NOT web_conn_context MATCHES "ConnInfo connInfo_" OR
       NOT web_conn_context_internal MATCHES
           "connInfo_[(]services[.]connInfo[(][)][)]" OR
       NOT web_conn_context_source MATCHES
           "return context[.]connInfo_")
        boundary_error("Context restored split connection metadata"
            "ContextServices, Context, and getConnInfo must pass one ConnInfo value without scalar reconstruction")
    endif()
    string(REGEX MATCHALL
        "std::basic_string<char, Traits, Allocator>&&"
        conn_deleted_rvalue_refinements
        "${web_conn_services}")
    list(LENGTH conn_deleted_rvalue_refinements
        conn_deleted_rvalue_refinement_count)
    if(conn_deleted_rvalue_refinement_count LESS 3 OR
       NOT web_conn_test MATCHES "AcceptsRvaluePlainTransport" OR
       NOT web_conn_test MATCHES "AcceptsRvalueTlsAddress" OR
       NOT web_conn_test MATCHES "AcceptsRvalueTlsCertificate" OR
       NOT web_conn_package_consumer MATCHES
           "AcceptsRvaluePlainTransport" OR
       NOT web_conn_package_consumer MATCHES
           "AcceptsRvalueTlsAddress" OR
       NOT web_conn_package_consumer MATCHES
           "AcceptsRvalueTlsCertificate")
        boundary_error("borrowed connection metadata accepts temporary owners"
            "plain/TLS address and certificate rvalue owning strings must remain deleted in source and installed consumers")
    endif()
    string(REGEX MATCHALL "remote_endpoint[(]" conn_remote_reads
        "${web_conn_server_entry}")
    list(LENGTH conn_remote_reads conn_remote_read_count)
    if(NOT conn_remote_read_count EQUAL 1 OR
       NOT web_conn_server_entry MATCHES "withPlainTransport" OR
       NOT web_conn_server_entry MATCHES "withTlsTransport" OR
       web_conn_http1_session MATCHES "remote_endpoint[(]" OR
       web_conn_http2_entry MATCHES "remote_endpoint[(]" OR
       web_conn_http2_session MATCHES "remote_endpoint[(]" OR
       NOT web_conn_http1_session MATCHES
           "ContextServices baseRouteServices" OR
       NOT web_conn_http1_session MATCHES
           "baseRouteServices[.]connInfo[(][)][.]remote[(][)][.]address[(][)]" OR
       NOT web_conn_http2_entry MATCHES "ContextServices services" OR
       NOT web_conn_http2_session MATCHES
           "const ContextServices& services[(][)]" OR
       NOT web_conn_http2_session MATCHES
           "const auto& baseServices = session[.]services[(][)]")
        boundary_error("server runtimes re-derived connection identity"
            "the accepted socket/handshake must classify one ConnInfo reused by HTTP/1, cleartext HTTP/2, and ALPN HTTP/2")
    endif()
    if(NOT web_conn_test MATCHES
           "conn_info_transport_has_one_active_alternative" OR
       NOT web_conn_test MATCHES
           "context_preserves_typed_connection_info_for_handler" OR
       NOT web_conn_test MATCHES
           "HasBooleanTransportRefinement" OR
       NOT web_conn_tls_test MATCHES "TlsConnectionObservation" OR
       NOT web_conn_tls_test MATCHES "withTlsTransport" OR
       NOT web_conn_tls_test MATCHES "info[.]tls[(][)]" OR
       NOT web_conn_package_consumer MATCHES
           "HasLegacyConnInfoScalarAccessors" OR
       NOT web_conn_package_consumer MATCHES
           "PlainConnectionTransport" OR
       NOT web_conn_package_consumer MATCHES
           "TlsConnectionTransport" OR
       NOT web_conn_api_surface MATCHES
           "HasLegacyConnInfoScalarAccessors")
        boundary_error("typed connection metadata lacks regression coverage"
            "unit, installed-package, and public API checks must pin alternatives, propagation, and removed scalar access")
    endif()
endif()
check_files_no_match("Web connection metadata restored scalar transport state"
    "${RULE_STALE_CONN_INFO_SCALARS}"
    "${WEB_CONN_CONTEXT_SERVICES}"
    "${WEB_CONN_CONTEXT}"
    "${WEB_CONN_CONTEXT_INTERNAL}"
    "${WEB_CONN_CONTEXT_SOURCE}"
    "${WEB_CONN_HTTP1_SESSION}"
    "${WEB_CONN_HTTP2_ENTRY}"
    "${WEB_CONN_HTTP2_SESSION}")

set(HTTP_PROTOCOL_VERSION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpProtocolVersion.h")
set(HTTP_REQUEST_ACCESS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestInternal.h")
set(HTTP1_CLIENT_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
foreach(protocol_version_file IN ITEMS
        "${HTTP_PROTOCOL_VERSION_HEADER}"
        "${HTTP_REQUEST_MODEL}"
        "${HTTP_REQUEST_ACCESS}"
        "${HTTP1_REQUEST_PARSER}"
        "${HTTP2_REQUEST_BUILDER}"
        "${HTTP1_CLIENT_RESPONSE_SOURCE}")
    if(NOT EXISTS "${protocol_version_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${protocol_version_file}")
        boundary_error("typed HTTP protocol-version contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_PROTOCOL_VERSION_HEADER}" AND
   EXISTS "${HTTP_REQUEST_MODEL}" AND
   EXISTS "${HTTP_REQUEST_ACCESS}" AND
   EXISTS "${HTTP1_REQUEST_PARSER}" AND
   EXISTS "${HTTP2_REQUEST_BUILDER}" AND
   EXISTS "${HTTP1_CLIENT_RESPONSE_SOURCE}")
    file(READ "${HTTP_PROTOCOL_VERSION_HEADER}" http_protocol_version_header)
    file(READ "${HTTP_REQUEST_MODEL}" http_protocol_request_model)
    file(READ "${HTTP_REQUEST_ACCESS}" http_protocol_request_access)
    file(READ "${HTTP1_REQUEST_PARSER}" http_protocol_http1_parser)
    file(READ "${HTTP2_REQUEST_BUILDER}" http_protocol_http2_builder)
    file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
        http_protocol_client_model)
    file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientAccess.h"
        http_protocol_client_access)
    file(READ "${HTTP1_CLIENT_RESPONSE_SOURCE}" http_protocol_client_parser)
    file(READ "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt" http_protocol_cmake)
    if(NOT http_protocol_version_header MATCHES "enum class HttpProtocolVersion" OR
       NOT http_protocol_version_header MATCHES "kHttp10" OR
       NOT http_protocol_version_header MATCHES "kHttp11" OR
       NOT http_protocol_version_header MATCHES "kHttp2" OR
       NOT http_protocol_request_model MATCHES "HttpProtocolVersion protocolVersion[(][)] const noexcept" OR
       NOT http_protocol_request_access MATCHES "setProtocolVersion" OR
       NOT http_protocol_http1_parser MATCHES "HttpProtocolVersion::kHttp10" OR
       NOT http_protocol_http1_parser MATCHES "HttpProtocolVersion::kHttp11" OR
       NOT http_protocol_http2_builder MATCHES "HttpProtocolVersion::kHttp2" OR
       NOT http_protocol_client_model MATCHES "HttpProtocolVersion protocolVersion[(][)] const noexcept" OR
       NOT http_protocol_client_model MATCHES
           "HttpClientResponseHead[(][ \t\r\n]*std::uint16_t status" OR
       http_protocol_client_model MATCHES "status_[ \t]*[{][ \t]*0[ \t]*[}]" OR
       NOT http_protocol_client_access MATCHES
           "make[(][ \t\r\n]*std::uint16_t status" OR
       http_protocol_client_access MATCHES "setStatus[(]" OR
       NOT http_protocol_client_access MATCHES "HttpProtocolVersion protocolVersion" OR
       http_protocol_client_access MATCHES "setProtocolVersion" OR
       NOT http_protocol_client_parser MATCHES "make[(]" OR
       NOT http_protocol_client_parser MATCHES
           "parsed[.]statusCode, parsed[.]protocolVersion" OR
       NOT http_protocol_client_parser MATCHES
           "using StatusLineParseResult = std::variant" OR
       NOT http_protocol_client_parser MATCHES
           "using ResponseHeadParseResult = std::variant" OR
       NOT http_protocol_client_parser MATCHES
           "std::get_if<ParsedStatusLine>" OR
       NOT http_protocol_client_parser MATCHES
           "std::get_if<Http1ClientResponseParseError>[(]&parsedHead[)]" OR
       http_protocol_client_parser MATCHES
           "HttpClientResponseHeadAccess::setStatus" OR
       NOT http_protocol_client_parser MATCHES "parsed[.]protocolVersion" OR
       NOT http_protocol_cmake MATCHES "include/ruvia/http/HttpProtocolVersion[.]h")
        boundary_error("HTTP protocol version split back into wire strings or parallel transport state"
            "H1/H2 requests, client responses, and connection state must share HttpProtocolVersion; protocol-specific final-response planners must not redispatch it")
    endif()
endif()
check_files_no_match("HTTP/1 client response status recovered mutation, output parameters, or a zero sentinel"
    "${RULE_STALE_HTTP_CLIENT_RESPONSE_STATUS_PRODUCT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientAccess.h"
    "${HTTP1_CLIENT_RESPONSE_SOURCE}")
file(READ "${RUVIA_ROOT}/tests/unit_request_access.cpp"
    http_protocol_request_tests)
file(READ "${RUVIA_ROOT}/tests/unit_http1_parser.cpp"
    http_protocol_http1_tests)
file(READ "${RUVIA_ROOT}/tests/unit_http2_request_builder.cpp"
    http_protocol_http2_tests)
file(READ "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    http_protocol_client_tests)
if(NOT http_protocol_request_tests MATCHES
       "request_access_protocol_version_is_typed_control_data" OR
   NOT http_protocol_http1_tests MATCHES
       "http1_parser_maps_wire_versions_to_typed_control_data" OR
   NOT http_protocol_http2_tests MATCHES
       "h2_request_builder_uses_connection_protocol_version" OR
   NOT http_protocol_client_tests MATCHES
       "http_client_response_preserves_typed_protocol_version")
    boundary_error("typed HTTP protocol-version coverage is incomplete"
        "request defaults, H1 start-lines, H2 connection version, and client responses all require direct tests")
endif()

set(WEB_ACCESS_LOG_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ServerConfig.h")
set(WEB_ACCESS_LOG_ACCESS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/app/AppAccess.h")
set(WEB_ACCESS_LOG_RECORDER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerAccessLog.h")
set(WEB_ACCESS_LOG_HTTP1
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_ACCESS_LOG_HTTP2
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_ACCESS_LOG_TEST "${RUVIA_ROOT}/tests/unit_access_log.cpp")
set(WEB_ACCESS_LOG_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(WEB_ACCESS_LOG_API_SURFACE "${RUVIA_ROOT}/examples/api_surface.cpp")
foreach(access_log_contract_file IN ITEMS
        "${WEB_ACCESS_LOG_MODEL}"
        "${WEB_ACCESS_LOG_ACCESS}"
        "${WEB_ACCESS_LOG_RECORDER}"
        "${WEB_ACCESS_LOG_HTTP1}"
        "${WEB_ACCESS_LOG_HTTP2}"
        "${WEB_ACCESS_LOG_TEST}"
        "${WEB_ACCESS_LOG_PACKAGE_CONSUMER}"
        "${WEB_ACCESS_LOG_API_SURFACE}")
    if(NOT EXISTS "${access_log_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${access_log_contract_file}")
        boundary_error("typed access-log protocol contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_ACCESS_LOG_MODEL}" AND
   EXISTS "${WEB_ACCESS_LOG_ACCESS}" AND
   EXISTS "${WEB_ACCESS_LOG_RECORDER}" AND
   EXISTS "${WEB_ACCESS_LOG_HTTP1}" AND
   EXISTS "${WEB_ACCESS_LOG_HTTP2}" AND
   EXISTS "${WEB_ACCESS_LOG_TEST}" AND
   EXISTS "${WEB_ACCESS_LOG_PACKAGE_CONSUMER}" AND
   EXISTS "${WEB_ACCESS_LOG_API_SURFACE}")
    file(READ "${WEB_ACCESS_LOG_MODEL}" web_access_log_model)
    file(READ "${WEB_ACCESS_LOG_ACCESS}" web_access_log_access)
    file(READ "${WEB_ACCESS_LOG_RECORDER}" web_access_log_recorder)
    file(READ "${WEB_ACCESS_LOG_HTTP1}" web_access_log_http1)
    file(READ "${WEB_ACCESS_LOG_HTTP2}" web_access_log_http2)
    file(READ "${WEB_ACCESS_LOG_TEST}" web_access_log_test)
    file(READ "${WEB_ACCESS_LOG_PACKAGE_CONSUMER}"
        web_access_log_package_consumer)
    file(READ "${WEB_ACCESS_LOG_API_SURFACE}"
        web_access_log_api_surface)
    if(web_access_log_model MATCHES
           "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}" OR
       web_access_log_access MATCHES
           "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}" OR
       web_access_log_recorder MATCHES
           "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}" OR
       NOT web_access_log_model MATCHES
           "const HttpRequest& request_" OR
       NOT web_access_log_model MATCHES
           "HttpProtocolVersion protocolVersion[(][)] const noexcept" OR
       NOT web_access_log_model MATCHES
           "return request_[.]protocolVersion[(][)]" OR
       NOT web_access_log_model MATCHES
           "return request_[.]method[(][)]" OR
       NOT web_access_log_model MATCHES
           "return request_[.]knownMethod[(][)]" OR
       NOT web_access_log_model MATCHES
           "return request_[.]path[(][)]" OR
       NOT web_access_log_model MATCHES "class AccessLogCallback final" OR
       NOT web_access_log_model MATCHES "AccessLogCallback bind" OR
       NOT web_access_log_access MATCHES
           "const HttpRequest& request" OR
       NOT web_access_log_access MATCHES
           "AccessLogRecord[(][ \t\r\n]*request" OR
       NOT web_access_log_recorder MATCHES
           "AccessLogRecordAccess::make[(][ \t\r\n]*request" OR
       NOT web_access_log_recorder MATCHES "const AccessLogSink&")
        boundary_error("access log restored copied request facts or a protocol boolean"
            "AccessLogRecord must borrow one HttpRequest and derive method/path/version from it")
    endif()
    if(web_access_log_http1 MATCHES
           "recordHttpAccess[(][^;]*(true|false)[)]" OR
       web_access_log_http2 MATCHES
           "recordHttpAccess[(][^;]*(true|false)[)]" OR
       web_access_log_recorder MATCHES
           "HttpProtocolVersion[ \t]+protocolVersion|bool[ \t]+http2")
        boundary_error("server access-log calls re-derived protocol version"
            "recordHttpAccess must consume request.protocolVersion through the record without H1/H2 flags")
    endif()
    if(NOT web_access_log_test MATCHES
           "access_log_record_borrows_one_typed_request" OR
       NOT web_access_log_test MATCHES
           "access_log_preserves_all_protocol_versions_without_transport_bool" OR
       NOT web_access_log_test MATCHES "RecordHttpAccessFunction" OR
       NOT web_access_log_package_consumer MATCHES
           "HasLegacyAccessLogHttp2Flag" OR
       NOT web_access_log_package_consumer MATCHES
           "RecordHttpAccessFunction" OR
       NOT web_access_log_package_consumer MATCHES
           "AppOnAccessFunction" OR
       NOT web_access_log_api_surface MATCHES
           "HasLegacyAccessLogHttp2Flag" OR
       NOT web_access_log_api_surface MATCHES
           "HasCanonicalAccessLogCallback")
        boundary_error("typed access-log protocol contract lacks regression coverage"
            "unit, installed-package, and public API checks must pin request borrowing, all versions, and removed bool access")
    endif()
endif()
check_files_no_match("AccessLogRecord restored copied request or bool protocol state"
    "${RULE_STALE_ACCESS_LOG_PROTOCOL_SPLIT}"
    "${WEB_ACCESS_LOG_MODEL}"
    "${WEB_ACCESS_LOG_ACCESS}"
    "${WEB_ACCESS_LOG_RECORDER}")

file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    http_client_public_model)
if(NOT http_client_public_model MATCHES "enum class HttpScheme" OR
   NOT http_client_public_model MATCHES "class HttpOrigin final" OR
   NOT http_client_public_model MATCHES "basic_string<char, Traits, Allocator>&&" OR
   NOT http_client_public_model MATCHES "class HttpClientRequestContent final" OR
   NOT http_client_public_model MATCHES
       "class HttpClientRequestWithoutContent final" OR
   NOT http_client_public_model MATCHES "class HttpClientRequestBytes final" OR
   NOT http_client_public_model MATCHES "using Content = std::variant" OR
   NOT http_client_public_model MATCHES
       "std::get_if<HttpClientRequestWithoutContent>" OR
   NOT http_client_public_model MATCHES "std::get_if<HttpClientRequestBytes>" OR
   NOT http_client_public_model MATCHES "borrowedBytes" OR
   NOT http_client_public_model MATCHES "class HttpClientResponseHead final" OR
   NOT http_client_public_model MATCHES "struct HttpClientRequest" OR
   NOT http_client_public_model MATCHES "class BorrowedText final" OR
   NOT http_client_public_model MATCHES "BorrowedText[(]String&&[)] = delete" OR
   NOT http_client_public_model MATCHES "operator=[(]String&&[)] = delete" OR
   NOT http_client_public_model MATCHES "BorrowedText method" OR
   NOT http_client_public_model MATCHES "BorrowedText target" OR
   NOT http_client_public_model MATCHES
       "sizeof[(]HttpClientRequest::BorrowedText[)] == sizeof[(]std::string_view[)]" OR
   http_client_public_model MATCHES
       "class HttpClientResponse final|body[(][)] const|body_[;]")
    boundary_error("outbound HTTP public model lost its transport-free typed contract"
        "HttpClient.h must type borrowed request text, distinguish absent/explicit content, and keep response head ownership separate from externally driven response content")
endif()
if(NOT EXISTS "${RUVIA_ROOT}/ruvia-http/src/client/HttpOrigin.cpp")
    boundary_error("outbound origin factory implementation is missing"
        "ruvia-http/src/client/HttpOrigin.cpp")
else()
    file(READ "${RUVIA_ROOT}/ruvia-http/src/client/HttpOrigin.cpp"
        http_client_origin_factory)
    if(NOT http_client_origin_factory MATCHES "isValidHttpHost[(]host[)]")
        boundary_error("outbound origin host validation drifted from request-target grammar"
            "HttpOrigin factories must reuse the shared HTTP uri-host parser")
    endif()
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpRequestTarget.h"
    http_authority_contract)
if(NOT http_authority_contract MATCHES "enum class HttpAuthorityPortKind" OR
   NOT http_authority_contract MATCHES "class HttpAuthorityView final" OR
   NOT http_authority_contract MATCHES "parseHttpAuthority" OR
   NOT http_authority_contract MATCHES "httpUriHostEquals")
    boundary_error("HTTP authority parsing lost its shared typed contract"
        "Host, absolute-form, origin, and redirects must share HttpAuthorityView")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/src/parser/HttpRequestTarget.cpp"
    http_authority_implementation)
if(NOT http_authority_implementation MATCHES "isUnreservedByte" OR
   NOT http_authority_implementation MATCHES "encodedReserved" OR
   NOT http_authority_implementation MATCHES "!isUnreservedByte[(]byte[)]")
    boundary_error("HTTP host comparison lost RFC percent-encoding normalization"
        "only encoded unreserved octets may collapse to raw spelling; encoded reserved octets remain distinct")
endif()
check_files_no_match("ruvia-http must not link/name ruvia-core in CMake"
    "${RULE_HTTP_CORE_LINK}" "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("ruvia-http request model must not contain socket/TLS metadata"
    "${RULE_HTTP_REQUEST_TRANSPORT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpRequest.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestInternal.h")
check_files_no_match("ruvia-http must not own Web application error models or JSON envelopes"
    "${RULE_HTTP_WEB_ERROR}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not own application JSON serialization helpers"
    "${RULE_HTTP_WEB_JSON}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not own static-file product/runtime helpers"
    "${RULE_HTTP_STATIC_FILE_PRODUCT}" ${HTTP_SOURCE})
check_files_no_match("Context must expose only body() for raw response construction"
    "${RULE_CONTEXT_NEW_RESPONSE_ALIAS}"
    ${WEB_SOURCE}
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
set(HTTP_BYTE_RANGE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpByteRange.h")
set(WEB_FILE_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp")
set(HTTP_BYTE_RANGE_TEST
    "${RUVIA_ROOT}/tests/unit_http_byte_range.cpp")
set(HTTP_CONTENT_RANGE_TEST
    "${RUVIA_ROOT}/tests/unit_content_range.cpp")
set(HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(byte_range_contract_file IN ITEMS
        "${HTTP_BYTE_RANGE_HEADER}"
        "${WEB_FILE_RESPONSE_SOURCE}"
        "${HTTP_BYTE_RANGE_TEST}"
        "${HTTP_CONTENT_RANGE_TEST}"
        "${HTTP_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${byte_range_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${byte_range_contract_file}")
        boundary_error("HTTP byte-range resolution contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_BYTE_RANGE_HEADER}" AND
   EXISTS "${WEB_FILE_RESPONSE_SOURCE}" AND
   EXISTS "${HTTP_BYTE_RANGE_TEST}" AND
   EXISTS "${HTTP_CONTENT_RANGE_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP_BYTE_RANGE_HEADER}" http_byte_range_header)
    file(READ "${WEB_FILE_RESPONSE_SOURCE}" web_file_response_source)
    file(READ "${HTTP_BYTE_RANGE_TEST}" http_byte_range_test)
    file(READ "${HTTP_CONTENT_RANGE_TEST}" http_content_range_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_range_package_consumer)
    if(NOT http_byte_range_header MATCHES "class HttpByteRangeIgnored final" OR
       NOT http_byte_range_header MATCHES
           "class HttpByteRangeUnsatisfiable final" OR
       NOT http_byte_range_header MATCHES "class HttpResolvedByteRange final" OR
       NOT http_byte_range_header MATCHES "class HttpByteRangeResolution final" OR
       NOT http_byte_range_header MATCHES "using Value = std::variant" OR
       NOT http_byte_range_header MATCHES "std::get_if<HttpByteRangeIgnored>" OR
       NOT http_byte_range_header MATCHES
           "std::get_if<HttpByteRangeUnsatisfiable>" OR
       NOT http_byte_range_header MATCHES "std::get_if<HttpResolvedByteRange>" OR
       NOT http_byte_range_header MATCHES "resolveHttpByteRange" OR
       NOT http_byte_range_header MATCHES "httpAsciiEqualsIgnoreCase" OR
       NOT http_byte_range_header MATCHES "std::errc::result_out_of_range" OR
       NOT http_byte_range_header MATCHES "representationLength == 0" OR
       NOT http_byte_range_header MATCHES "length_ == 0")
        boundary_error("HTTP byte-range resolver lost its discriminated RFC contract"
            "ignored/unsatisfiable outcomes must be payload-free; only one bounded nonempty range owns slicing coordinates")
    endif()
    if(NOT web_file_response_source MATCHES "resolveHttpByteRange" OR
       NOT web_file_response_source MATCHES "rangeResolution[.]ignored[(][)]" OR
       NOT web_file_response_source MATCHES
           "rangeResolution[.]unsatisfiable[(][)]" OR
       NOT web_file_response_source MATCHES "rangeResolution[.]resolved[(][)]" OR
       NOT web_file_response_source MATCHES "resolved[.]offset[(][)]" OR
       NOT web_file_response_source MATCHES "resolved[.]length[(][)]")
        boundary_error("ruvia-web bypasses the HTTP byte-range resolution"
            "file responses must map the three typed outcomes without reparsing Range")
    endif()
    if(NOT http_byte_range_test MATCHES
           "byte_range_resolution_is_discriminated" OR
       NOT http_byte_range_test MATCHES
           "!std::default_initializable<HttpByteRangeResolution>" OR
       NOT http_byte_range_test MATCHES
           "!HasByteRangeOutcomeField<HttpByteRangeResolution>" OR
       NOT http_byte_range_test MATCHES
           "HasByteRangeOffsetAccessor<HttpResolvedByteRange>" OR
       NOT http_byte_range_test MATCHES
           "byte_range_unit_is_case_insensitive" OR
       NOT http_byte_range_test MATCHES
           "byte_range_huge_decimal_numerals_preserve_semantics" OR
       NOT http_byte_range_test MATCHES
           "byte_range_empty_representation_uses_ignore_policy" OR
       NOT http_content_range_test MATCHES "Bytes=5-9" OR
       NOT http_content_range_test MATCHES "empty[.]txt" OR
       NOT http_range_package_consumer MATCHES "HttpByteRangeResolution" OR
       NOT http_range_package_consumer MATCHES "!HasByteRangeOutcomeField" OR
       NOT http_range_package_consumer MATCHES
           "installedResolvedRange[.]resolved[(][)]->offset[(][)]" OR
       NOT http_range_package_consumer MATCHES
           "installedUnsatisfiableRange[.]unsatisfiable[(][)]")
        boundary_error("HTTP byte-range resolution contract is under-tested"
            "unit, Web integration, and installed-package consumers must pin exclusive outcomes and RFC numeric/unit edges")
    endif()
endif()
check_files_no_match("ruvia-web response stream sink must not serialize HTTP/1 chunk/trailer bytes"
    "${RULE_WEB_HTTP1_STREAM_FRAMING_BYTES}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
check_files_no_match("ruvia-web stream body reader must drive the HTTP chunked decoder"
    "${RULE_WEB_CHUNKED_PROTOCOL_PARSER}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderChunked.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl")
check_files_no_match("ruvia-web buffered multipart facade must use parseMultipartBody"
    "${RULE_WEB_MULTIPART_PROTOCOL_PARSER}"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("multipart parsing must use one typed boundary and chunk lifecycle"
    "${RULE_STALE_MULTIPART_API}" ${EDGE_REFERENCE_SOURCE})
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/MultipartParser.h"
    multipart_public_api)
if(NOT multipart_public_api MATCHES "class MultipartBoundary final" OR
   NOT multipart_public_api MATCHES "std::array<char, kMaxSize>" OR
   NOT multipart_public_api MATCHES "enum class MultipartChunkPhase" OR
   NOT multipart_public_api MATCHES "class MultipartPollNeedInput final" OR
   NOT multipart_public_api MATCHES "class MultipartPollDone final" OR
   NOT multipart_public_api MATCHES "enum class MultipartParseError" OR
   NOT multipart_public_api MATCHES "class MultipartPollFailure final" OR
   NOT multipart_public_api MATCHES "class MultipartPollResult final" OR
   NOT multipart_public_api MATCHES "class MultipartBody final" OR
   NOT multipart_public_api MATCHES "class MultipartBodyParseFailure final" OR
   NOT multipart_public_api MATCHES "class MultipartBodyParseResult final" OR
   NOT multipart_public_api MATCHES "using Value = std::variant" OR
   NOT multipart_public_api MATCHES
       "using State = std::variant<ProgressState, MultipartParseError>" OR
   NOT multipart_public_api MATCHES "std::get_if<MultipartStreamPart>" OR
   NOT multipart_public_api MATCHES "std::get_if<MultipartPollFailure>" OR
   NOT multipart_public_api MATCHES
       "needInput[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_public_api MATCHES
       "part[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_public_api MATCHES
       "done[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_public_api MATCHES
       "failure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_public_api MATCHES
       "HttpProtocolError protocolError[(][)] const noexcept" OR
   multipart_public_api MATCHES "multipartParseErrorMessage" OR
   multipart_public_api MATCHES
       "MultipartParseError error[(][)] const noexcept" OR
   NOT multipart_public_api MATCHES "MultipartParser[(]MultipartBoundary boundary" OR
   NOT multipart_public_api MATCHES "void feed[(]std::string_view chunk[)]" OR
   NOT multipart_public_api MATCHES "void finishInput[(][)] noexcept" OR
   NOT multipart_public_api MATCHES "MultipartBodyParseResult parseMultipartBody" OR
   multipart_public_api MATCHES
       "${RULE_STALE_MULTIPART_NONTERMINAL_FAILURE}")
    boundary_error("multipart public API lost its typed sans-I/O contract"
        "MultipartParser.h must validate boundary ownership once and expose discriminated phase/need-input/part/done results")
endif()
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/MultipartReader.h"
    multipart_web_api)
file(READ "${RUVIA_ROOT}/tests/unit_multipart_reader.cpp"
    multipart_lifetime_test)
file(READ "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    multipart_http_package_test)
file(READ "${RUVIA_ROOT}/tests/package-consumer/web.cpp"
    multipart_web_package_test)
if(NOT multipart_public_api MATCHES
       "MultipartParser[(]const MultipartParser&[)][ \t]*=[ \t]*delete" OR
   NOT multipart_public_api MATCHES
       "MultipartParser[(]MultipartParser&&[)][ \t]*=[ \t]*delete" OR
   NOT multipart_web_api MATCHES
       "MultipartReader[(]const MultipartReader&[)][ \t]*=[ \t]*delete" OR
   NOT multipart_web_api MATCHES
       "MultipartReader[(]MultipartReader&&[)][ \t]*=[ \t]*delete" OR
   NOT multipart_lifetime_test MATCHES
       "!std::is_copy_constructible_v<ruvia::MultipartParser>" OR
   NOT multipart_lifetime_test MATCHES
       "!std::is_move_constructible_v<MultipartReader>" OR
   NOT multipart_http_package_test MATCHES
       "!std::is_move_constructible_v<ruvia::MultipartParser>" OR
   NOT multipart_web_package_test MATCHES
       "!std::is_move_constructible_v<ruvia::MultipartReader>")
    boundary_error("multipart parser state regained cross-object aliasing"
        "the sans-I/O parser and Web stream driver must remain address-stable single-owner states because their views and body source bind to one instance")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    pmr_http_client_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
    pmr_http_redirect_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    pmr_http_client_parser_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpResponse.h"
    pmr_http_response_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseBody.h"
    pmr_http_response_body_api)
file(READ "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp"
    pmr_http_response_impl)
file(READ "${RUVIA_ROOT}/ruvia-http/src/HttpResponseHeadersStorage.cpp"
    pmr_http_response_headers_impl)
file(READ "${RUVIA_ROOT}/tests/unit_http_response.cpp"
    pmr_http_response_test)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/RequestFields.h"
    pmr_request_fields_api)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbMigration.h"
    pmr_db_migration_api)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbQueryResult.h"
    pmr_db_query_result_api)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbTransaction.h"
    pmr_db_transaction_api)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbTypes.h"
    pmr_db_types_api)
file(READ "${RUVIA_ROOT}/ruvia-web/src/db/DbTypes.cpp"
    pmr_db_types_impl)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/db/DbResultAccess.h"
    pmr_db_result_access)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/db/DbValueAccess.h"
    pmr_db_value_access)
file(READ "${RUVIA_ROOT}/tests/unit_db_api_surface.cpp"
    pmr_db_types_test)
file(READ "${RUVIA_ROOT}/examples/api_surface.cpp"
    pmr_db_api_surface)
file(READ "${RUVIA_ROOT}/ruvia-web/src/db/DbHandle.cpp"
    pmr_db_handle_impl)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisTypes.h"
    pmr_redis_types_api)
file(READ "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    pmr_http_package_contract)
file(READ "${RUVIA_ROOT}/tests/package-consumer/web.cpp"
    pmr_web_package_contract)
if(NOT pmr_http_client_api MATCHES
       "operator=[(]HttpClientResponseHead&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_http_redirect_api MATCHES
       "operator=[(]HttpClientRedirectTarget&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_http_redirect_api MATCHES
       "HttpClientRedirectTargetResult&&[)][ \n\t]*=[ \t]*delete" OR
   NOT pmr_http_client_parser_api MATCHES
       "operator=[(]Http1ParsedClientResponseHead&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_http_client_parser_api MATCHES
       "operator=[(]Http1ClientResponseParseResult&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_http_response_api MATCHES
       "operator=[(]HttpResponseHeaders&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_http_response_api MATCHES
       "operator=[(]HttpResponse&& other[)][ \t]*noexcept" OR
   NOT pmr_http_response_body_api MATCHES
       "operator=[(]HttpResponseBody&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_http_response_impl MATCHES
       "std::destroy_at[(]this[)]" OR
   NOT pmr_http_response_impl MATCHES
       "std::construct_at[(]this, std::move[(]other[)][)]" OR
   pmr_http_response_headers_impl MATCHES
       "HttpResponseHeaders::operator=" OR
   NOT pmr_http_response_test MATCHES
       "response_move_assignment_transfers_one_resource_domain" OR
   NOT pmr_request_fields_api MATCHES
       "operator=[(]RequestNameValueList&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_db_migration_api MATCHES
       "operator=[(]DbMigrationReport&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_db_query_result_api MATCHES
       "operator=[(]QueryResult&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_db_query_result_api MATCHES
       "operator=[(]DbStreamResult&&[)][ \t]*=[ \t]*delete" OR
   NOT pmr_db_transaction_api MATCHES
       "operator=[(]DbTransaction&&[)][ \t]*=[ \t]*delete" OR
   pmr_db_types_impl MATCHES
       "QueryResult::operator=" OR
   pmr_db_handle_impl MATCHES
       "(DbStreamResult|DbTransaction)::operator=" OR
   NOT pmr_db_types_api MATCHES
       "operator=[(]DbField&& other[)][ \t]*;" OR
   pmr_db_types_api MATCHES
       "operator=[(]DbField&& other[)][ \t]*noexcept" OR
   NOT pmr_db_types_api MATCHES
       "operator=[(]DbRow&& other[)][ \t]*;" OR
   pmr_db_types_api MATCHES
       "operator=[(]DbRow&& other[)][ \t]*noexcept" OR
   pmr_db_types_impl MATCHES
       "operator=[(]Db(Field|Row)&& other[)][ \t]*noexcept" OR
   NOT pmr_redis_types_api MATCHES
       "operator=[(]RedisKeyValue&&[)][ \t]*=[ \t]*default" OR
   pmr_redis_types_api MATCHES
       "operator=[(]RedisKeyValue&&[)][ \t]*noexcept" OR
   NOT pmr_redis_types_api MATCHES
       "operator=[(]RedisScoredValue&&[)][ \t]*=[ \t]*default" OR
   pmr_redis_types_api MATCHES
       "operator=[(]RedisScoredValue&&[)][ \t]*noexcept" OR
   NOT pmr_redis_types_api MATCHES
       "operator=[(]RedisValue&&[)][ \t]*=[ \t]*default" OR
   pmr_redis_types_api MATCHES
       "operator=[(]RedisValue&&[)][ \t]*noexcept" OR
   NOT pmr_http_package_contract MATCHES
       "HttpClientResponseHead&, ruvia::HttpClientResponseHead&&" OR
   NOT pmr_web_package_contract MATCHES
       "!std::is_nothrow_move_assignable_v<ruvia::DbField>" OR
   NOT pmr_web_package_contract MATCHES
       "!std::is_nothrow_move_assignable_v<ruvia::RedisValue>")
    boundary_error("PMR-owning public values or linear database handles regained unsafe move-assignment guarantees"
        "single-use results and active DB operations must reject reassignment; assignable values must expose allocation failure, and HttpResponse replacement must transfer one resource domain without member-wise PMR assignment")
endif()
if(NOT pmr_db_types_api MATCHES
       "DbValue[(]std::basic_string<char, Traits, Allocator>&&[)] = delete" OR
   NOT pmr_db_types_api MATCHES
       "DbValue[(]const std::basic_string<char, Traits, Allocator>&&[)] = delete" OR
   NOT pmr_db_types_test MATCHES
       "!AcceptsTemporaryDbValueText<std::string>" OR
   NOT pmr_db_types_test MATCHES
       "AcceptsLvalueDbValueText<std::string>" OR
   NOT pmr_db_api_surface MATCHES
       "!AcceptsTemporaryDbValueText<const std::string>" OR
   NOT pmr_web_package_contract MATCHES
       "!AcceptsTemporaryDbValueText<std::string>")
    boundary_error("DbValue regained temporary owning-text borrows"
        "stored query parameters must reject owning-string rvalues while preserving borrowed lvalue input")
endif()
if(NOT pmr_db_migration_api MATCHES "class DbMigration final" OR
   NOT pmr_db_migration_api MATCHES
       "DbMigration[(]String&&, std::string_view[)] = delete" OR
   NOT pmr_db_migration_api MATCHES
       "DbMigration[(]std::string_view, String&&[)] = delete" OR
   NOT pmr_db_migration_api MATCHES
       "std::string_view id[(][)] const noexcept" OR
   NOT pmr_db_migration_api MATCHES
       "std::string_view sql[(][)] const noexcept" OR
   NOT pmr_db_types_test MATCHES
       "!AcceptsAnyTemporaryDbMigrationText<std::pmr::string>" OR
   NOT pmr_db_types_test MATCHES
       "kCompileTimeMigration" OR
   NOT pmr_db_api_surface MATCHES
       "HasDbMigrationTextAccessors<ruvia::DbMigration>" OR
   NOT pmr_web_package_contract MATCHES
       "!AcceptsAnyTemporaryDbMigrationText<const std::string>")
    boundary_error("DbMigration regained mutable or temporary text borrows"
        "migration descriptors must remain immutable constexpr borrowed values that reject owning-string rvalues")
endif()
if(NOT pmr_db_types_api MATCHES
       "using Storage = std::variant<" OR
   NOT pmr_db_types_api MATCHES
       "using Storage = std::variant<OwnedFields, BorrowedFields>" OR
   NOT pmr_db_types_api MATCHES
       "operator=[(]const DbValue&[)][ 	]*=[ 	]*delete" OR
   NOT pmr_db_types_api MATCHES
       "operator=[(]DbValue&&[)][ 	]*=[ 	]*delete" OR
   NOT pmr_db_types_api MATCHES
       "detail::DbValueType type[(][)] const noexcept" OR
   NOT pmr_db_types_api MATCHES
       "OwnedFields& ownedFields[(][)] noexcept" OR
   pmr_db_types_api MATCHES
       "ownsText_|ownsValue_|ownsFields_|ownedFields_|valueView_|refreshView|DbValueType[ 	]+type_|bool[ 	]+isNull_" OR
   pmr_db_types_impl MATCHES
       "ownsText_|ownsValue_|ownsFields_|ownedFields_|valueView_|refreshView" OR
   pmr_db_result_access MATCHES
       "refresh[(]DbRow&|refreshView|ownedFields_" OR
   NOT pmr_db_value_access MATCHES
       "static DbValueType type[(]const DbValue& value[)] noexcept" OR
   NOT pmr_web_package_contract MATCHES
       "!ExposesDbValueInspection<ruvia::DbValue>" OR
   NOT pmr_db_types_test MATCHES
       "db_value_and_result_storage_have_one_live_alternative")
    boundary_error("database values restored parallel tag, payload, or ownership state"
        "DbValue, DbField, and DbRow must own one exact storage alternative; owned rows expose their live vector directly and immutable query parameters reject assignment")
endif()
file(READ "${RUVIA_ROOT}/ruvia-web/src/db/Db.cpp"
    db_mariadb_stream_impl)
file(READ "${RUVIA_ROOT}/ruvia-web/src/db/PgDb.cpp"
    db_postgresql_stream_impl)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/db/DbOperationState.h"
    db_operation_state)
if(NOT pmr_db_query_result_api MATCHES
       "DbOperationState<Lease>[ \n\t]+state_" OR
   NOT pmr_db_transaction_api MATCHES
       "DbOperationState<Lease>[ \n\t]+state_" OR
   NOT db_operation_state MATCHES
       "std::variant<Closed, Active, Operating, Failed>[ \n\t]+state_" OR
   NOT db_operation_state MATCHES
       "database operation is already in progress" OR
   NOT db_operation_state MATCHES
       "Payload payload[(]std::move[(]active->payload[)][)]" OR
   NOT db_operation_state MATCHES
       "std::holds_alternative<Operating>[(]other[.]state_[)]" OR
   NOT db_operation_state MATCHES "std::terminate[(][)]" OR
   NOT pmr_db_query_result_api MATCHES "class OperationGuard final" OR
   NOT pmr_db_transaction_api MATCHES "class OperationGuard final" OR
   NOT pmr_db_handle_impl MATCHES
       "DbStreamResult::OperationGuard::~OperationGuard" OR
   NOT pmr_db_handle_impl MATCHES
       "DbTransaction::OperationGuard::~OperationGuard" OR
   NOT pmr_db_handle_impl MATCHES
       "owner_->state_[.]finishFailed[(][)]" OR
   NOT pmr_db_handle_impl MATCHES
       "Task<void> DbTransaction::commit" OR
   NOT pmr_db_handle_impl MATCHES
       "co_await commitPoolTransaction" OR
   pmr_db_handle_impl MATCHES
       "return (commit|rollback)PoolTransaction" OR
   pmr_db_query_result_api MATCHES "bool[ \t]+active_" OR
   pmr_db_transaction_api MATCHES "bool[ \t]+active_" OR
   NOT pmr_db_types_test MATCHES
       "database_operation_state_rejects_overlap_and_failed_reuse" OR
   NOT pmr_db_types_test MATCHES
       "database_cold_operations_do_not_consume_pool_lease" OR
   NOT db_mariadb_stream_impl MATCHES
       "co_return DbStreamResult[(][)]" OR
   NOT db_postgresql_stream_impl MATCHES
       "DbStreamResult[(]DbPoolRef[{]this[}], slotIndex, nullptr, resource[)]")
    boundary_error("database linear resources regained overlapping operation or lease state"
        "stream results and transactions must admit one lazy operation through Closed/Active/Operating/Failed state, reject overlap and failed reuse, preserve cold-task leases, and keep backend cleanup single-owner")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/MultipartParsing.h"
    multipart_protocol_helpers)
if(NOT multipart_protocol_helpers MATCHES "class HttpMultipartDelimiterNoMatch final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartDelimiterNeedInput final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartPartDelimiter final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartCloseDelimiter final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartDelimiterResult final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartBoundaryNotApplicable final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartBoundaryParseFailure final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartBoundaryParseResult final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartPartHeaderParseFailure final" OR
   NOT multipart_protocol_helpers MATCHES "class HttpMultipartPartHeaderParseResult final" OR
   NOT multipart_protocol_helpers MATCHES "using Value = std::variant" OR
   NOT multipart_protocol_helpers MATCHES "httpMatchMultipartDelimiterLine" OR
   NOT multipart_protocol_helpers MATCHES "std::get_if<HttpMultipartDelimiterNeedInput>" OR
   NOT multipart_protocol_helpers MATCHES
       "noMatch[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_protocol_helpers MATCHES
       "needInput[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_protocol_helpers MATCHES
       "part[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_protocol_helpers MATCHES
       "close[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_protocol_helpers MATCHES
       "headers[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_protocol_helpers MATCHES
       "boundary[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_protocol_helpers MATCHES
       "notApplicable[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_protocol_helpers MATCHES
       "HttpProtocolError protocolError[(][)] const noexcept" OR
   multipart_protocol_helpers MATCHES
       "HttpMultipartBoundaryParseError|HttpMultipartPartHeaderParseError|failure->error[(][)]" OR
   NOT multipart_protocol_helpers MATCHES
       "MultipartParseError parseError[(][)] const noexcept")
    boundary_error("multipart delimiter and Content-Type decisions escaped the HTTP core"
        "ruvia-http must own discriminated boundary/header extraction and an input-aware shared delimiter scanner")
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/src/MultipartReader.cpp"
    multipart_parser_implementation)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/MultipartParser.h"
    multipart_parser_api)
string(FIND "${multipart_parser_implementation}"
    "MultipartPollResult MultipartParser::poll" multipart_poll_offset)
if(multipart_poll_offset EQUAL -1)
    boundary_error("multipart poll implementation is missing"
        "MultipartReader.cpp must implement the typed MultipartPollResult driver")
else()
    string(SUBSTRING "${multipart_parser_implementation}"
        ${multipart_poll_offset} -1 multipart_incremental_implementation)
    if(multipart_incremental_implementation MATCHES
           "throw[ \\t]+std::invalid_argument" OR
       NOT multipart_incremental_implementation MATCHES
           "MultipartPollResult::makeFailure" OR
       NOT multipart_parser_implementation MATCHES
           "MultipartPollResult MultipartParser::fail" OR
       NOT multipart_parser_implementation MATCHES
           "state_ = error" OR
       NOT multipart_parser_implementation MATCHES
           "std::get_if<MultipartParseError>[(]&state_[)]" OR
       NOT multipart_parser_implementation MATCHES
           "multipartParseErrorMessage" OR
       multipart_parser_implementation MATCHES
           "throw[ \t]+std::invalid_argument" OR
       NOT multipart_parser_implementation MATCHES
           "MultipartParser parser[(]" OR
       NOT multipart_parser_implementation MATCHES
           "MultipartBorrowedInput[{]completeBody[}]")
        boundary_error("incremental multipart wire failures bypass typed results"
            "buffered and incremental parsing must share MultipartParser and return typed wire failures")
    endif()
endif()
if(multipart_parser_api MATCHES "StepStatus|stepError" OR
   NOT multipart_parser_api MATCHES
       "using StepResult = std::variant<StepProgress, MultipartParseError>" OR
   multipart_parser_implementation MATCHES
       "StepStatus|stepError|HttpMultipartPartHeaderParseError|failure->error[(][)]" OR
   NOT multipart_parser_implementation MATCHES "failure->parseError[(][)]")
    boundary_error("multipart parser regained duplicate internal error domains"
        "part-header and parser steps must carry the single MultipartParseError domain without translation switches")
endif()
file(READ "${RUVIA_ROOT}/ruvia-web/src/http/MultipartReader.cpp"
    multipart_web_driver)
file(READ "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp"
    multipart_buffered_web_driver)
if(NOT multipart_web_driver MATCHES "parser_[.]finishInput[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]part[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]done[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]needInput[(][)]" OR
   NOT multipart_web_driver MATCHES "result[.]failure[(][)]" OR
   NOT multipart_web_driver MATCHES "failure->protocolError[(][)]" OR
   multipart_web_driver MATCHES "failure->error[(][)]|HttpProtocolError" OR
   NOT multipart_web_driver MATCHES "while [(]co_await bodyReader_[.]read[(][)][)]" OR
   multipart_web_driver MATCHES "bodyEnded_")
    boundary_error("multipart Web facade stopped driving the complete HTTP body lifecycle"
        "the runtime must drive typed results, signal EOF to the protocol parser, and drain RFC 2046 epilogue bytes")
endif()
if(NOT multipart_buffered_web_driver MATCHES
       "parseCompleteMultipartBody" OR
   NOT multipart_buffered_web_driver MATCHES
       "boundary[.]notApplicable[(][)]" OR
   NOT multipart_buffered_web_driver MATCHES "parsed[.]failure[(][)]" OR
   NOT multipart_buffered_web_driver MATCHES
       "failure->protocolError[(][)]" OR
   multipart_buffered_web_driver MATCHES
       "multipartParseErrorMessage|HttpMultipartBoundaryParseError|failure->error[(][)]" OR
   NOT multipart_buffered_web_driver MATCHES "takeParts[(][)]")
    boundary_error("buffered multipart Web facade bypasses the typed parser result"
        "Context request parsing must map one HTTP result and consume its owned part vector")
endif()
file(READ "${RUVIA_ROOT}/tests/unit_multipart.cpp" multipart_unit_test)
file(READ "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    multipart_package_consumer)
file(READ "${RUVIA_ROOT}/examples/api_surface.cpp" multipart_api_surface)
if(NOT multipart_unit_test MATCHES "multipart_parser_commits_an_eof_close_only_after_finish_input" OR
   NOT multipart_unit_test MATCHES "multipart_parser_reports_typed_incomplete_body" OR
   NOT multipart_unit_test MATCHES "multipart_part_header_result_is_discriminated" OR
   NOT multipart_unit_test MATCHES "default_initializable<ruvia::MultipartPollResult>" OR
   NOT multipart_unit_test MATCHES "ruvia::MultipartBodyParseResult" OR
   NOT multipart_unit_test MATCHES "multipart_complete_body_parser_rejects_malformed_body" OR
   NOT multipart_unit_test MATCHES "multipart_complete_body_parser_shares_incremental_limits" OR
   NOT multipart_unit_test MATCHES
       "HasMultipartProtocolError<ruvia::MultipartPollFailure>" OR
   NOT multipart_unit_test MATCHES
       "HasMultipartProtocolError<ruvia::MultipartBodyParseFailure>" OR
   NOT multipart_unit_test MATCHES "feedAfterFailureThrew" OR
   NOT multipart_unit_test MATCHES "const auto repeated = incremental[.]poll[(][)]" OR
   NOT multipart_unit_test MATCHES "HasMultipartLineBytes" OR
   NOT multipart_package_consumer MATCHES "ruvia::MultipartPollResult" OR
   NOT multipart_package_consumer MATCHES "ruvia::MultipartBodyParseResult" OR
   NOT multipart_package_consumer MATCHES
       "HasMultipartProtocolError<ruvia::MultipartPollFailure>" OR
   NOT multipart_package_consumer MATCHES
       "HasMultipartProtocolError<ruvia::MultipartBodyParseFailure>" OR
   NOT multipart_package_consumer MATCHES "repeatedMultipartFailure" OR
   NOT multipart_package_consumer MATCHES "failedMultipartParser[.]feed" OR
   NOT multipart_package_consumer MATCHES "HttpMultipartDelimiterResult" OR
   NOT multipart_unit_test MATCHES
       "!HasAnyRvalueMultipartDelimiterAccessor" OR
   NOT multipart_unit_test MATCHES
       "!HasAnyRvalueMultipartPartHeaderAccessor" OR
   NOT multipart_unit_test MATCHES
       "!HasAnyRvalueMultipartBoundaryAccessor" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueMultipartDelimiterAccessor" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueMultipartPartHeaderAccessor" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueMultipartBoundaryAccessor" OR
   NOT multipart_api_surface MATCHES "HasMultipartPollResultAccessors<ruvia::MultipartPollResult>" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueMultipartPollAccessor<ruvia::MultipartPollResult>" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueHttp1RequestParseAccessor" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueHttp1ClientResponseParseAccessor" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueHttp1ClientRequestPrepareAccessor" OR
   NOT multipart_package_consumer MATCHES
       "!HasAnyRvalueHttp1InterimResponsePrepareAccessor" OR
   NOT multipart_api_surface MATCHES
       "!HasAnyRvalueMultipartPollAccessor<ruvia::MultipartPollResult>" OR
   NOT multipart_api_surface MATCHES
       "!HasAnyRvalueHttp1RequestParseAccessor" OR
   NOT multipart_api_surface MATCHES
       "!HasAnyRvalueHttp1ClientResponseParseAccessor")
    boundary_error("typed multipart result ownership is insufficiently tested"
        "unit, example, and installed-consumer contracts must pin alternatives and reject pointers into temporary parse results")
endif()
check_files_no_match("ruvia-web handshake writers must only submit HTTP-owned parts"
    "${RULE_WEB_HANDSHAKE_PROTOCOL_BYTES}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketHandshake.h")
check_files_no_match("WebSocket server negotiation must remain one immutable HTTP value"
    "${RULE_STALE_WS_SERVER_NEGOTIATION}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("WebSocket deflate negotiation must use exclusive alternatives"
    "${RULE_STALE_WS_DEFLATE_PRODUCT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h")
check_files_no_match("ruvia-web HTTP/2 stream sink must not encode trailer protocol bytes"
    "${RULE_WEB_HTTP2_TRAILER_PROTOCOL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
check_files_no_match("ruvia-web stream state must not validate response trailer protocol fields"
    "${RULE_WEB_TRAILER_PROTOCOL_VALIDATION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h")
check_files_no_match("ruvia-web WebSocket transports must flush opaque HTTP-owned bytes"
    "${RULE_WEB_WS_SPLIT_FRAME_TRANSPORT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
check_files_no_match("WebSocket close and transport end must use one typed protocol plan"
    "${RULE_STALE_WS_CLOSE_CHAIN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h"
    "${RUVIA_ROOT}/tests/unit_ws_connection.cpp")
check_files_no_match("WebSocket events must remain optional and discriminated"
    "${RULE_STALE_WS_EVENT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsEvent.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl")
check_files_no_match("WebSocket core must keep one generic frame submission entry"
    "submit(Message|Ping|Pong)[(]"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl")
check_files_no_match("WebSocket transport end must use only typed disposition"
    "endsTransport[(]|transportEndPending[(]"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl")
check_files_no_match("WebSocket inbound parsing must remain nonthrowing and discriminated"
    "${RULE_STALE_WS_INBOUND_RESULT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketUtils.h"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketValidation.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/tests/unit_websocket_frame.cpp"
    "${RUVIA_ROOT}/tests/unit_websocket_assembler.cpp"
    "${RUVIA_ROOT}/tests/unit_websocket_close.cpp"
    "${RUVIA_ROOT}/tests/unit_ws_connection.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("ruvia-http must not own WebSocket timer/runtime policy"
    "${RULE_HTTP_WS_RUNTIME_POLICY}" ${HTTP_SOURCE})
check_files_no_match("WebSocket liveness must abort its transport, not the scanner owner"
    "${RULE_WS_SCANNER_OWNER_ABORT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionHeartbeat.inl")
check_files_no_match("ruvia-web must drive the HTTP-owned HTTP/1 stream plan"
    "${RULE_WEB_HTTP1_STREAM_PLAN}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
check_files_no_match("ruvia-web response state must use HTTP-owned persistence finalization"
    "${RULE_WEB_HTTP1_RESPONSE_FINALIZATION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h")
check_files_no_match("HTTP/1 connection lifetime must use one typed plan"
    "${RULE_STALE_HTTP1_CONNECTION_LIFETIME}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp"
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP connection fields must use the shared typed state"
    "${RULE_STALE_HTTP_CONNECTION_FIELD_STATE}"
    ${HTTP_SOURCE} ${WEB_SOURCE})
check_files_no_match("HTTP/1 request-body framing must use one typed plan"
    "${RULE_STALE_HTTP1_REQUEST_BODY_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpLazyBufferedBody.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderContentLength.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerRequestState.h"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP content decoding must use one owning typed result"
    "${RULE_STALE_HTTP_CONTENT_DECODE_CHAIN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentCoding.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/RequestBodyDecoding.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientContentEncoding.h"
    "${RUVIA_ROOT}/ruvia-http/src/HttpContentCoding.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("HTTP content encoding must own its result and response lifetime"
    "${RULE_STALE_HTTP_CONTENT_ENCODE_CHAIN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentCoding.h"
    "${RUVIA_ROOT}/ruvia-http/src/HttpContentCoding.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseCompression.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpResponseCompression.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpConnectionState.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpConnectionState.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(HTTP_CONTENT_CODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentCoding.h")
set(WEB_RESPONSE_COMPRESSION_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpResponseCompression.cpp")
set(WEB_RESPONSE_COMPRESSION_TEST
    "${RUVIA_ROOT}/tests/unit_response_compression.cpp")
set(WEB_UNSUPPORTED_CONTENT_CODING_SIGNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/UnsupportedRequestContentCoding.h")
set(WEB_ROUTER_DISPATCH_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
set(WEB_ROUTER_DISPATCH_TEST
    "${RUVIA_ROOT}/tests/unit_routing.cpp")
if(EXISTS "${HTTP_CONTENT_CODING_CONTRACT}" AND
   EXISTS "${WEB_RESPONSE_COMPRESSION_SOURCE}" AND
   EXISTS "${WEB_RESPONSE_COMPRESSION_TEST}" AND
   EXISTS "${WEB_UNSUPPORTED_CONTENT_CODING_SIGNAL}" AND
   EXISTS "${WEB_ROUTER_DISPATCH_SOURCE}" AND
   EXISTS "${WEB_ROUTER_DISPATCH_TEST}")
    file(READ "${HTTP_CONTENT_CODING_CONTRACT}"
        http_content_coding_contract)
    file(READ "${WEB_RESPONSE_COMPRESSION_SOURCE}"
        web_response_compression_source)
    file(READ "${WEB_RESPONSE_COMPRESSION_TEST}"
        web_response_compression_test)
    file(READ "${WEB_UNSUPPORTED_CONTENT_CODING_SIGNAL}"
        web_unsupported_content_coding_signal)
    file(READ "${WEB_ROUTER_DISPATCH_SOURCE}"
        web_router_dispatch_source)
    file(READ "${WEB_ROUTER_DISPATCH_TEST}"
        web_router_dispatch_test)
    if(http_content_coding_contract MATCHES
           "HttpContentCoding::kNone" OR
       NOT http_content_coding_contract MATCHES
           "class HttpUnsupportedContentCoding final" OR
       NOT http_content_coding_contract MATCHES
           "class HttpInvalidContentCodingField final" OR
       NOT http_content_coding_contract MATCHES
           "static constexpr std::uint16_t status" OR
       NOT http_content_coding_contract MATCHES
           "httpSupportedRequestContentCodings" OR
       NOT http_content_coding_contract MATCHES
           "class HttpContentCodingFieldResult final" OR
       NOT http_content_coding_contract MATCHES
           "HttpInvalidContentCodingField> value_" OR
       NOT http_content_coding_contract MATCHES
           "class HttpEncodedContent final" OR
       NOT http_content_coding_contract MATCHES
           "class HttpContentEncodeFailure final" OR
       NOT http_content_coding_contract MATCHES
           "class HttpContentEncodeResult final" OR
       NOT http_content_coding_contract MATCHES
           "std::variant<HttpEncodedContent, HttpContentEncodeFailure>" OR
       NOT web_response_compression_source MATCHES
           "setResponseBodyOwned" OR
       NOT web_response_compression_source MATCHES
           "takeBytes[(][)]" OR
       NOT web_response_compression_test MATCHES
           "responseBody[(]response[)][.]ownedBytes[(][)]")
        boundary_error("HTTP response compression lost encoded-byte ownership"
            "the HTTP encoder must return one owning alternative and Web must move it into HttpResponse")
    endif()
    if(NOT web_response_compression_source MATCHES
           "CacheControlFieldParser" OR
       NOT web_response_compression_source MATCHES
           "for [(]const auto& header : response[.]headers[(][)][)]" OR
       NOT web_response_compression_source MATCHES
           "[.]noTransform" OR
       NOT web_response_compression_test MATCHES
           "compress_ignores_no_transform_inside_quoted_extension" OR
       NOT web_response_compression_test MATCHES
           "compress_respects_no_transform_in_later_cache_control_field")
        boundary_error("Web response compression bypassed Cache-Control parsing"
            "no-transform must be recognized across every field line by the quote-aware HTTP parser")
    endif()
    if(web_unsupported_content_coding_signal MATCHES
           "status_|HttpUnsupportedContentCoding::status|std::uint16_t[ \t]+status" OR
       NOT web_router_dispatch_source MATCHES
           "HttpUnsupportedContentCoding::status[(][)]" OR
       NOT web_router_dispatch_test MATCHES
           "dispatch_rejects_unsupported_request_content_coding_with_advertisement")
        boundary_error("unsupported content-coding status must remain protocol-owned"
            "the Web signal must carry no status copy and Router dispatch must map the protocol status while preserving its 415/Accept-Encoding behavior test")
    endif()
endif()
set(HTTP_RESPONSE_VARY_UTILS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/ResponseHeaderUtils.h")
set(HTTP_RESPONSE_VARY_TEST
    "${RUVIA_ROOT}/tests/unit_vary_headers.cpp")
foreach(http_response_vary_contract IN ITEMS
        "${HTTP_RESPONSE_VARY_UTILS}"
        "${HTTP_RESPONSE_VARY_TEST}")
    if(NOT EXISTS "${http_response_vary_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${http_response_vary_contract}")
        boundary_error("response Vary combination contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_RESPONSE_VARY_UTILS}" AND
   EXISTS "${HTTP_RESPONSE_VARY_TEST}")
    file(READ "${HTTP_RESPONSE_VARY_UTILS}"
        http_response_vary_utils)
    file(READ "${HTTP_RESPONSE_VARY_TEST}"
        http_response_vary_test)
    if(NOT http_response_vary_utils MATCHES
           "responseVaryHasToken" OR
       NOT http_response_vary_utils MATCHES
           "for [(]const auto& header : response[.]headers[(][)][)]" OR
       NOT http_response_vary_test MATCHES
           "vary_add_preserves_repeated_field_lines_in_combined_order" OR
       NOT http_response_vary_test MATCHES
           "vary_wildcard_in_later_repeated_field_line_dominates")
        boundary_error("response Vary mutation lost repeated field semantics"
            "Vary token checks and rewrites must consume every field line in wire order, including a later wildcard")
    endif()
endif()
set(HTTP_REQUEST_CONTENT_DECODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/RequestBodyDecoding.h")
set(HTTP_CLIENT_CONTENT_DECODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientContentEncoding.h")
set(WEB_CONTEXT_REQUEST_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
set(REQUEST_CONTENT_DECODING_TEST
    "${RUVIA_ROOT}/tests/unit_request_body_decoding.cpp")
set(HTTP_CONTENT_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(request_content_decode_contract IN ITEMS
        "${HTTP_REQUEST_CONTENT_DECODING_CONTRACT}"
        "${HTTP_CLIENT_CONTENT_DECODING_CONTRACT}"
        "${WEB_CONTEXT_REQUEST_SOURCE}"
        "${REQUEST_CONTENT_DECODING_TEST}"
        "${HTTP_CONTENT_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${request_content_decode_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${request_content_decode_contract}")
        boundary_error("request content decode role contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_REQUEST_CONTENT_DECODING_CONTRACT}" AND
   EXISTS "${HTTP_CLIENT_CONTENT_DECODING_CONTRACT}" AND
   EXISTS "${WEB_CONTEXT_REQUEST_SOURCE}" AND
   EXISTS "${REQUEST_CONTENT_DECODING_TEST}" AND
   EXISTS "${HTTP_CONTENT_PACKAGE_CONSUMER}")
    file(READ "${HTTP_REQUEST_CONTENT_DECODING_CONTRACT}"
        request_content_decode_contract)
    file(READ "${HTTP_CLIENT_CONTENT_DECODING_CONTRACT}"
        client_content_decode_contract)
    file(READ "${WEB_CONTEXT_REQUEST_SOURCE}"
        web_context_request_source)
    file(READ "${REQUEST_CONTENT_DECODING_TEST}"
        request_content_decode_test)
    file(READ "${HTTP_CONTENT_PACKAGE_CONSUMER}"
        content_decode_package_consumer)
    if(NOT request_content_decode_contract MATCHES
           "class HttpRequestContentDecodeProtocolFailure final" OR
       NOT request_content_decode_contract MATCHES
           "class HttpRequestContentDecoderFailure final" OR
       NOT request_content_decode_contract MATCHES
           "class HttpRequestContentDecodeResult final" OR
       NOT request_content_decode_contract MATCHES
           "HttpProtocolError protocolError[(][)] const noexcept" OR
       NOT request_content_decode_contract MATCHES
           "std::variant<[ \t\r\n]*HttpDecodedContent,[ \t\r\n]*HttpRequestContentDecodeProtocolFailure,[ \t\r\n]*HttpRequestContentDecoderFailure>" OR
       NOT request_content_decode_contract MATCHES
           "HttpRequestContentDecodeResult decodeHttpRequestContent" OR
       NOT request_content_decode_contract MATCHES
           "decoded[(][)] const &&[ \t]*=[ \t]*delete" OR
       NOT request_content_decode_contract MATCHES
           "protocolFailure[(][)] const &&[ \t]*=[ \t]*delete" OR
       NOT request_content_decode_contract MATCHES
           "decoderFailure[(][)] const &&[ \t]*=[ \t]*delete" OR
       request_content_decode_contract MATCHES
           "HttpContentDecodeError error[(][)] const|std::optional<HttpProtocolError>" OR
       client_content_decode_contract MATCHES
           "decodeHttpRequestContent|HttpRequestContentDecodeResult")
        boundary_error("request and client content decoding roles were merged"
            "HTTP request decoding must own protocol status while client decoding keeps the role-neutral result")
    endif()
    if(NOT web_context_request_source MATCHES
           "decodeHttpRequestContent[(]" OR
       NOT web_context_request_source MATCHES
           "decodeResult[.]protocolFailure[(][)]" OR
       NOT web_context_request_source MATCHES
           "decodeResult[.]decoderFailure[(][)]" OR
       NOT web_context_request_source MATCHES
           "failure->protocolError[(][)]" OR
       web_context_request_source MATCHES
           "decodeHttpContent[(]|HttpContentDecodeError::")
        boundary_error("Web request content decoding rebuilt HTTP failure semantics"
            "Context must drive the HTTP-owned request result and only propagate its protocol error")
    endif()
    if(NOT request_content_decode_test MATCHES
           "HttpRequestContentDecodeResult" OR
       NOT request_content_decode_test MATCHES
           "HasRawRequestContentDecodeError" OR
       NOT request_content_decode_test MATCHES
           "web_request_decode_uses_the_configured_buffered_body_limit" OR
       NOT request_content_decode_test MATCHES
           "web_request_decode_rejects_empty_encoded_representation" OR
       NOT request_content_decode_test MATCHES
           "http_request_content_decoder_owns_protocol_failure_status" OR
       NOT content_decode_package_consumer MATCHES
           "HttpRequestContentDecodeResult" OR
       NOT content_decode_package_consumer MATCHES
           "HttpRequestContentDecodeProtocolFailure" OR
       NOT content_decode_package_consumer MATCHES
           "HttpRequestContentDecoderFailure" OR
       NOT content_decode_package_consumer MATCHES
           "decodeHttpRequestContent")
        boundary_error("request content decode role ownership is insufficiently tested"
            "unit and installed-consumer contracts must pin result lifetime and 400/413 protocol mappings")
    endif()
endif()
if(EXISTS
       "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpStreamingDecoder.h")
    boundary_error(
        "HTTP content decoding must have one implementation owner"
        "stale HttpStreamingDecoder.h exists")
endif()
set(WEB_STATIC_FILE_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp")
set(WEB_STATIC_FILE_INDEX_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/StaticFiles.cpp")
set(WEB_STATIC_FILE_INDEX_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/StaticFilesInternal.h")
set(HTTP_RESPONSE_FILE_IDENTITY_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseFileBody.h")
set(WEB_NATIVE_FILE_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpNativeFile.h")
set(WEB_FILE_INPUT_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpFileOpen.h")
set(WEB_STATIC_FILE_REPRESENTATION_TEST
    "${RUVIA_ROOT}/tests/unit_content_range.cpp")
set(WEB_STATIC_FILE_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
check_files_no_match("static file representation must use one typed selection"
    "${RULE_STALE_STATIC_FILE_REPRESENTATION_SPLIT}"
    "${WEB_STATIC_FILE_RESPONSE_SOURCE}"
    "${WEB_STATIC_FILE_INDEX_CONTRACT}")
if(EXISTS "${WEB_STATIC_FILE_RESPONSE_SOURCE}" AND
   EXISTS "${WEB_STATIC_FILE_INDEX_SOURCE}" AND
   EXISTS "${WEB_STATIC_FILE_INDEX_CONTRACT}" AND
   EXISTS "${HTTP_RESPONSE_FILE_IDENTITY_CONTRACT}" AND
   EXISTS "${WEB_NATIVE_FILE_CONTRACT}" AND
   EXISTS "${WEB_FILE_INPUT_CONTRACT}" AND
   EXISTS "${WEB_STATIC_FILE_REPRESENTATION_TEST}" AND
   EXISTS "${WEB_STATIC_FILE_PACKAGE_CONSUMER}")
    file(READ "${WEB_STATIC_FILE_RESPONSE_SOURCE}"
        web_static_file_response_source)
    file(READ "${WEB_STATIC_FILE_INDEX_SOURCE}"
        web_static_file_index_source)
    file(READ "${WEB_STATIC_FILE_INDEX_CONTRACT}"
        web_static_file_index_contract)
    file(READ "${HTTP_RESPONSE_FILE_IDENTITY_CONTRACT}"
        http_response_file_identity_contract)
    file(READ "${WEB_NATIVE_FILE_CONTRACT}"
        web_native_file_contract)
    file(READ "${WEB_FILE_INPUT_CONTRACT}"
        web_file_input_contract)
    file(READ "${WEB_STATIC_FILE_REPRESENTATION_TEST}"
        web_static_file_representation_test)
    file(READ "${WEB_STATIC_FILE_PACKAGE_CONSUMER}"
        web_static_file_package_consumer)
    if(NOT web_static_file_response_source MATCHES
           "class FileResponsePath final" OR
       NOT web_static_file_response_source MATCHES
           "copyingNative[(]" OR
       NOT web_static_file_response_source MATCHES
           "std::filesystem::path path_" OR
       NOT web_static_file_response_source MATCHES
           "setResponseFileBody[(][ \t\r\n]*response,[ \t\r\n]*takePath[(][)]" OR
       NOT web_static_file_response_source MATCHES
           "snapshotResponseFile[(]path[.]c_str[(][)]" OR
       NOT web_static_file_response_source MATCHES
           "servedEntry[.]identity[(][)]" OR
       web_static_file_response_source MATCHES
           "FileResponseBorrowedNativePath|setResponseBorrowedNativeFileBody|FileResponsePath::borrowing" OR
       NOT web_static_file_response_source MATCHES
           "class StaticFileRepresentation final" OR
       NOT web_static_file_response_source MATCHES
           "HttpContentCoding contentCoding_" OR
       NOT web_static_file_response_source MATCHES
           "httpContentCodingToken[(]contentCoding[)]" OR
       NOT web_static_file_index_contract MATCHES
           "class StaticRootEntryView final" OR
       NOT web_static_file_index_contract MATCHES
           "std::optional<StaticRootEntryView>" OR
       NOT web_static_file_index_contract MATCHES
           "ResponseFileIdentity identity" OR
       NOT web_static_file_index_source MATCHES
           "snapshotResponseFile" OR
       NOT http_response_file_identity_contract MATCHES
           "class ResponseFileIdentity final" OR
       NOT http_response_file_identity_contract MATCHES
           "array<std::uint64_t, 4>" OR
       NOT http_response_file_identity_contract MATCHES
           "requiresValidation[(][)] const noexcept" OR
       NOT web_native_file_contract MATCHES
           "snapshotNativeFileHandle" OR
       NOT web_native_file_contract MATCHES
           "snapshot[.]identity != file[.]identity[(][)]" OR
       NOT web_native_file_contract MATCHES
           "snapshot[.]size != file[.]size[(][)]" OR
       NOT web_native_file_contract MATCHES
           "errc::state_not_recoverable" OR
       NOT web_file_input_contract MATCHES
           "openNativeFileForRead[(]file, error_[)]" OR
       NOT web_static_file_index_source MATCHES
           "mime[.]contentType[.]empty[(][)]" OR
       NOT web_static_file_representation_test MATCHES
           "static_file_selects_precompressed_representation_atomically" OR
       NOT web_static_file_representation_test MATCHES
           "static_root_rejects_empty_custom_mime_type" OR
       NOT web_static_file_representation_test MATCHES
           "static_file_response_owns_path_after_handler_local_root_is_destroyed" OR
       NOT web_static_file_representation_test MATCHES
           "static_file_replacement_cannot_reuse_indexed_metadata" OR
       NOT web_static_file_representation_test MATCHES
           "context_file_replacement_cannot_reuse_response_metadata" OR
       NOT web_static_file_package_consumer MATCHES
           "!std::default_initializable<[ \t\r\n]*ruvia::detail::StaticRootEntryView>" OR
       NOT web_static_file_package_consumer MATCHES
           "std::optional<ruvia::detail::StaticRootEntryView>")
        boundary_error("static file representation ownership or identity was split"
            "response paths must be owned and carry a same-handle validated identity while selected entries and HTTP content coding remain typed and atomically tested")
    endif()
endif()
set(WEB_JSON_STRING_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/json/JsonString.h")
set(WEB_JSON_OBJECT_FIELDS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/json/JsonObjectFields.h")
set(WEB_JSON_MODEL_PARSER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/JsonParser.h")
set(WEB_JSON_STRING_TEST
    "${RUVIA_ROOT}/tests/unit_json.cpp")
set(WEB_JSON_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
check_files_no_match("JSON string decoding must return one owning transactional result"
    "${RULE_STALE_JSON_STRING_DECODE_CHAIN}"
    "${WEB_JSON_STRING_CONTRACT}"
    "${WEB_JSON_OBJECT_FIELDS}"
    "${WEB_JSON_MODEL_PARSER}"
    "${RUVIA_ROOT}/ruvia-web/src/auth/JwtJson.cpp")
check_files_no_match("JSON string scanning must return one transactional token"
    "${RULE_STALE_JSON_STRING_SCAN_CHAIN}"
    "${WEB_JSON_STRING_CONTRACT}"
    "${WEB_JSON_OBJECT_FIELDS}"
    "${WEB_JSON_MODEL_PARSER}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/json/JsonSkip.h"
    "${RUVIA_ROOT}/ruvia-web/src/auth/JwtJson.cpp")
check_files_no_match("JSON model parsing must return complete transactional values"
    "${RULE_STALE_JSON_MODEL_PARSE_OUTPUT}"
    "${WEB_JSON_MODEL_PARSER}")
if(EXISTS "${WEB_JSON_STRING_CONTRACT}" AND
   EXISTS "${WEB_JSON_MODEL_PARSER}" AND
   EXISTS "${WEB_JSON_STRING_TEST}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    file(READ "${WEB_JSON_STRING_CONTRACT}"
        web_json_string_contract)
    file(READ "${WEB_JSON_MODEL_PARSER}"
        web_json_model_parser)
    file(READ "${WEB_JSON_STRING_TEST}"
        web_json_string_test)
    file(READ "${WEB_JSON_PACKAGE_CONSUMER}"
        web_json_package_consumer)
    if(NOT web_json_string_contract MATCHES
           "std::optional<std::pmr::string>[ \t\r\n]+decodeJsonString" OR
       NOT web_json_string_contract MATCHES
           "class[ \t]+JsonStringToken[ \t]+final" OR
       NOT web_json_string_contract MATCHES
           "std::optional<JsonStringToken>[ \t\r\n]+parseJsonString" OR
       NOT web_json_string_contract MATCHES
           "auto[ \t]+remaining[ \t]*=[ \t]*input" OR
       NOT web_json_string_contract MATCHES
           "input[ \t]*=[ \t]*remaining" OR
       NOT web_json_model_parser MATCHES
           "value[.]assignOwned[(]std::move[(][*]decoded[)][)]" OR
       NOT web_json_model_parser MATCHES
           "std::optional<SequenceT>[ \t\r\n]+parseJsonSequenceValue" OR
       NOT web_json_string_test MATCHES
           "json_string_value_failure_preserves_input_cursor" OR
       NOT web_json_string_test MATCHES
           "json_string_scan_failure_preserves_input_cursor" OR
       NOT web_json_string_test MATCHES
           "json_sequence_failure_preserves_input_cursor" OR
       NOT web_json_package_consumer MATCHES
           "AcceptsJsonDecodeOutputParameter" OR
       NOT web_json_package_consumer MATCHES
           "AcceptsJsonStringScanOutputParameters" OR
       NOT web_json_package_consumer MATCHES
           "AcceptsJsonValueOutputParameter" OR
       NOT web_json_package_consumer MATCHES
           "AcceptsJsonSequenceOutputParameter" OR
       NOT web_json_package_consumer MATCHES
           "std::optional<std::pmr::string>" OR
       NOT web_json_package_consumer MATCHES
           "std::optional<ruvia::detail::JsonStringToken>" OR
       NOT web_json_package_consumer MATCHES
           "std::optional<ruvia::Array<ruvia::Int32>>")
        boundary_error("JSON string parsing lost transactional ownership"
            "Web JSON scanning and decoding must return complete typed results and commit callers only after success")
    endif()
endif()
set(WEB_MODEL_TYPES_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ModelTypes.h")
set(WEB_MODEL_LIST_TEST
    "${RUVIA_ROOT}/tests/unit_model_list.cpp")
check_files_no_match("model List must own and release its PMR elements"
    "${RULE_STALE_MODEL_LIST_OWNERSHIP}"
    "${WEB_MODEL_TYPES_CONTRACT}")
if(EXISTS "${WEB_MODEL_TYPES_CONTRACT}" AND
   EXISTS "${WEB_MODEL_LIST_TEST}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    file(READ "${WEB_MODEL_TYPES_CONTRACT}"
        web_model_types_contract)
    file(READ "${WEB_MODEL_LIST_TEST}"
        web_model_list_test)
    if(NOT web_model_types_contract MATCHES
           "~List[(][)]" OR
       NOT web_model_types_contract MATCHES
           "for[ \t]*[(]auto[*][ \t]+value[ \t]*:[ \t]*items_[)]" OR
       NOT web_model_types_contract MATCHES
           "destroyPmrObject" OR
       NOT web_model_types_contract MATCHES
           "std::destroy_at[(]&items_[)]" OR
       NOT web_model_types_contract MATCHES
           "std::construct_at[(]&items_[ \t]*,[ \t]*std::move[(]other[.]items_[)]" OR
       NOT web_model_list_test MATCHES
           "model_list_clear_and_destructor_release_owned_elements" OR
       NOT web_model_list_test MATCHES
           "model_list_move_assignment_transfers_element_resource" OR
       NOT web_json_package_consumer MATCHES
           "is_nothrow_move_assignable_v<ruvia::List<ruvia::Int32>>")
        boundary_error("model List lost PMR element ownership"
            "List must destroy every allocated element and move its pointer-table allocator with the owning resource")
    endif()
endif()
set(WEB_MODEL_STRING_TEST
    "${RUVIA_ROOT}/tests/unit_model_string.cpp")
set(WEB_MODEL_VALIDATION_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Validation.h")
check_files_no_match("model String must use one exclusive storage alternative"
    "${RULE_STALE_MODEL_STRING_STORAGE}"
    "${WEB_MODEL_TYPES_CONTRACT}")
if(EXISTS "${WEB_MODEL_TYPES_CONTRACT}" AND
   EXISTS "${WEB_MODEL_STRING_TEST}" AND
   EXISTS "${WEB_MODEL_VALIDATION_CONTRACT}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    file(READ "${WEB_MODEL_STRING_TEST}"
        web_model_string_test)
    file(READ "${WEB_MODEL_VALIDATION_CONTRACT}"
        web_model_validation_contract)
    if(NOT web_model_types_contract MATCHES
           "using[ \t]+Storage[ \t]*=[ \t]*std::variant<std::string_view,[ \t]*std::pmr::string>" OR
       NOT web_model_types_contract MATCHES
           "String[(]const String&[)][ \t]*=[ \t]*delete" OR
       NOT web_model_types_contract MATCHES
           "std::destroy_at[(]&storage_[)]" OR
       NOT web_model_types_contract MATCHES
           "std::construct_at[(]&storage_[ \t]*,[ \t]*std::move[(]other[.]storage_[)]" OR
       NOT web_model_validation_contract MATCHES
           "const auto&[ \t]+ruviaValue[ \t]*=[ \t]*body[.]field[(][)]" OR
       NOT web_model_string_test MATCHES
           "model_string_public_construction_owns_input" OR
       NOT web_model_string_test MATCHES
           "model_string_owned_assignment_is_alias_safe" OR
       NOT web_model_string_test MATCHES
           "model_string_move_assignment_transfers_resource" OR
       NOT web_json_package_consumer MATCHES
           "!std::copy_constructible<ruvia::String>")
        boundary_error("model String lost exclusive move-only ownership"
            "public values must own inputs while parser-only construction may borrow one typed storage alternative")
    endif()
endif()
set(WEB_MODEL_MACROS_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/Macros.h")
set(WEB_MODEL_FIELD_OPS_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/MacroFieldOps.h")
set(WEB_MODEL_MATERIALIZATION_TEST
    "${RUVIA_ROOT}/tests/unit_http_parsing.cpp")
set(WEB_MODEL_OBJECT_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ModelObject.h")
set(WEB_MODEL_API_SURFACE
    "${RUVIA_ROOT}/examples/api_surface.cpp")
set(WEB_MODEL_HEADER_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Model.h")
set(WEB_MODEL_INPUT_VISITORS_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/ModelInputVisitors.h")
set(WEB_MODEL_JSON_WRITER_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/JsonWriter.h")
set(WEB_MODEL_RULES_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/Rules.h")
set(WEB_VALIDATION_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Validation.h")
set(WEB_STALE_REQUEST_OBJECT_VISITORS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/RequestObjectVisitors.h")
if(EXISTS "${WEB_STALE_REQUEST_OBJECT_VISITORS}")
    boundary_error("stale RequestObject visitor header remains"
        "model parsing input visitors must use the detail::ModelInput vocabulary")
endif()
check_files_no_match("generated model factories must publish only materialized models"
    "${RULE_STALE_MODEL_LAZY_PARSE_STATE}"
    "${WEB_MODEL_MACROS_CONTRACT}")
check_files_no_match("generated model fields must remain const-correct"
    "${RULE_STALE_MODEL_MUTABLE_FIELDS}"
    "${WEB_MODEL_FIELD_OPS_CONTRACT}")
check_files_no_match("generated model fields must expose one const getter"
    "${RULE_STALE_MODEL_NONCONST_FIELD_GETTER}"
    "${WEB_MODEL_FIELD_OPS_CONTRACT}")
check_files_no_match("response model JSON writing must use its nominal schema boundary"
    "${RULE_STALE_MODEL_JSON_WRITER_DUCK_TYPING}"
    "${WEB_MODEL_JSON_WRITER_CONTRACT}")
check_files_no_match("generated model fields must use their declared names"
    "${RULE_STALE_MODEL_COMPILE_TIME_GET_ALIAS}"
    "${WEB_MODEL_MACROS_CONTRACT}"
    "${WEB_MODEL_FIELD_OPS_CONTRACT}")
check_files_no_match("model validation must use its detail state accessor"
    "${RULE_STALE_MODEL_VALIDATION_DIRECT_STATE}"
    "${WEB_VALIDATION_CONTRACT}")
check_files_no_match("generated models must not rescan raw bodies through dynamic get"
    "${RULE_STALE_GENERATED_MODEL_DYNAMIC_GET}"
    "${WEB_MODEL_MACROS_CONTRACT}"
    "${WEB_MODEL_OBJECT_CONTRACT}")
check_files_no_match("generated models must not retain transient request sources"
    "${RULE_STALE_GENERATED_MODEL_REQUEST_RETENTION}"
    "${WEB_MODEL_MACROS_CONTRACT}"
    "${WEB_MODEL_FIELD_OPS_CONTRACT}")
check_files_no_match("model parsing input must remain detail-only ModelInput"
    "${RULE_STALE_MODEL_REQUEST_OBJECT}"
    "${WEB_MODEL_HEADER_CONTRACT}"
    "${WEB_MODEL_TYPES_CONTRACT}"
    "${WEB_MODEL_OBJECT_CONTRACT}"
    "${WEB_MODEL_MACROS_CONTRACT}"
    "${WEB_MODEL_FIELD_OPS_CONTRACT}"
    "${WEB_MODEL_INPUT_VISITORS_CONTRACT}"
    "${WEB_MODEL_API_SURFACE}"
    "${WEB_JSON_PACKAGE_CONSUMER}")
if(EXISTS "${WEB_MODEL_MACROS_CONTRACT}" AND
   EXISTS "${WEB_MODEL_FIELD_OPS_CONTRACT}" AND
   EXISTS "${WEB_MODEL_MATERIALIZATION_TEST}" AND
   EXISTS "${WEB_MODEL_OBJECT_CONTRACT}" AND
   EXISTS "${WEB_MODEL_API_SURFACE}" AND
   EXISTS "${WEB_MODEL_HEADER_CONTRACT}" AND
   EXISTS "${WEB_MODEL_TYPES_CONTRACT}" AND
   EXISTS "${WEB_MODEL_INPUT_VISITORS_CONTRACT}" AND
   EXISTS "${WEB_MODEL_JSON_WRITER_CONTRACT}" AND
   EXISTS "${WEB_MODEL_RULES_CONTRACT}" AND
   EXISTS "${WEB_VALIDATION_CONTRACT}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    file(READ "${WEB_MODEL_MACROS_CONTRACT}"
        web_model_macros_contract)
    file(READ "${WEB_MODEL_FIELD_OPS_CONTRACT}"
        web_model_field_ops_contract)
    file(READ "${WEB_MODEL_MATERIALIZATION_TEST}"
        web_model_materialization_test)
    file(READ "${WEB_MODEL_API_SURFACE}"
        web_model_api_surface)
    file(READ "${WEB_MODEL_OBJECT_CONTRACT}"
        web_model_object_contract)
    file(READ "${WEB_MODEL_INPUT_VISITORS_CONTRACT}"
        web_model_input_visitors_contract)
    file(READ "${WEB_MODEL_JSON_WRITER_CONTRACT}"
        web_model_json_writer_contract)
    file(READ "${WEB_MODEL_RULES_CONTRACT}"
        web_model_rules_contract)
    file(READ "${WEB_VALIDATION_CONTRACT}"
        web_validation_contract)
    if(NOT web_model_macros_contract MATCHES
           "ruviaMaterializeInput" OR
       NOT web_model_macros_contract MATCHES
           "class[ \t]+T[ \t]*:[ \t]*private[ \t]+::ruvia::detail::RequestModelSchemaTag" OR
       NOT web_model_macros_contract MATCHES
           "class[ \t]+T[ \t]*:[ \t]*private[ \t]+::ruvia::detail::ResponseModelSchemaTag" OR
       NOT web_model_types_contract MATCHES
           "struct[ \t]+ModelSchemaTag[ \t]*[{][}]" OR
       NOT web_model_types_contract MATCHES
           "struct[ \t]+RequestModelSchemaTag[ \t]*:[ \t]*ModelSchemaTag" OR
       NOT web_model_types_contract MATCHES
           "struct[ \t]+ResponseModelSchemaTag[ \t]*:[ \t]*ModelSchemaTag" OR
       NOT web_model_types_contract MATCHES
           "is_base_of_v<detail::RequestModelSchemaTag,[ \t]*T>" OR
       web_model_types_contract MATCHES
           "void_t<decltype[(]T::ruviaParse(Json|Form)Body" OR
       NOT web_model_macros_contract MATCHES
           "T[ \t]+request[{]ruviaInput[.]resource[(][)][}]" OR
       NOT web_model_macros_contract MATCHES
           "if[ \t]*[(]!request[.]ruviaMaterialize[(]ruviaInput[)][)]" OR
       web_model_macros_contract MATCHES
           "${RULE_STALE_MODEL_LAZY_PARSE_STATE}" OR
       web_model_macros_contract MATCHES
           "${RULE_STALE_GENERATED_MODEL_REQUEST_RETENTION}" OR
       web_model_field_ops_contract MATCHES
           "${RULE_STALE_MODEL_MUTABLE_FIELDS}" OR
       web_model_field_ops_contract MATCHES
           "${RULE_STALE_MODEL_NONCONST_FIELD_GETTER}" OR
       web_model_macros_contract MATCHES
           "${RULE_STALE_MODEL_COMPILE_TIME_GET_ALIAS}" OR
       web_model_field_ops_contract MATCHES
           "${RULE_STALE_MODEL_COMPILE_TIME_GET_ALIAS}" OR
       NOT web_model_macros_contract MATCHES
           "pmrResourceOrDefault[(]resource[)]" OR
       NOT web_model_macros_contract MATCHES
           "memory_resource[*][ \t]+ruviaResource_" OR
       NOT web_model_macros_contract MATCHES
           "ruviaMaterialize[(]const[ \t]+::ruvia::detail::ModelInput&[ \t]+ruviaInput[)]" OR
       NOT web_model_macros_contract MATCHES
           "friend[ \t]+struct[ \t]+::ruvia::JsonBody" OR
       NOT web_model_macros_contract MATCHES
           "friend[ \t]+struct[ \t]+::ruvia::FormBody" OR
       NOT web_model_macros_contract MATCHES
           "friend[ \t]+struct[ \t]+::ruvia::detail::ModelJsonAccess" OR
       NOT web_model_macros_contract MATCHES
           "friend[ \t]+struct[ \t]+::ruvia::detail::ModelValidationAccess" OR
       NOT web_model_rules_contract MATCHES
           "struct[ \t]+ModelValidationAccess[ \t]+final" OR
       NOT web_model_rules_contract MATCHES
           "model[.]template[ \t]+ruviaFieldState<Field>[(][)]" OR
       NOT web_validation_contract MATCHES
           "ModelValidationAccess::fieldState<#field>[(]body[)]" OR
       web_validation_contract MATCHES
           "${RULE_STALE_MODEL_VALIDATION_DIRECT_STATE}" OR
       NOT web_model_json_writer_contract MATCHES
           "struct[ \t]+ModelJsonAccess[ \t]+final" OR
       NOT web_model_json_writer_contract MATCHES
           "isResponseModel<T>" OR
       NOT web_model_json_writer_contract MATCHES
           "ModelJsonAccess::sizeHint[(]value[)]" OR
       NOT web_model_json_writer_contract MATCHES
           "ModelJsonAccess::append[(]output,[ \t]*value[)]" OR
       web_model_json_writer_contract MATCHES
           "${RULE_STALE_MODEL_JSON_WRITER_DUCK_TYPING}" OR
       NOT web_model_object_contract MATCHES
           "enum[ \t]+class[ \t]+ModelInputKind" OR
       NOT web_model_object_contract MATCHES
           "class[ \t]+ModelInput[ \t]+final" OR
       NOT web_model_input_visitors_contract MATCHES
           "visitModelInputJsonFields" OR
       NOT web_model_input_visitors_contract MATCHES
           "visitModelInputFormFields" OR
       NOT web_model_materialization_test MATCHES
           "model_factory_materializes_before_publication" OR
       NOT web_model_materialization_test MATCHES
           "JsonBody<AccessorSurfaceRequest>::parse" OR
       NOT web_model_materialization_test MATCHES
           "ModelValidationAccess::fieldState<\"message\">" OR
       NOT web_model_materialization_test MATCHES
           "messageEnsure[(][)][.]resource[(][)][ \t]*==[ \t]*&modelResource" OR
       NOT web_model_api_surface MATCHES
           "!HasModelDynamicGet<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelTypedDynamicGet<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelCompileTimeGetAlias<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelPublicBodyParseHooks<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelInputAccessor<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelPublicJsonDepthHook<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelPublicFormFieldsHook<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelNonConstMessageGetter<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelPublicJsonWriterHooks<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "!HasModelPublicFieldStateHook<ClonePayload>" OR
       NOT web_model_api_surface MATCHES
           "RequestModelSchemaTag,[ \t]*ClonePayload" OR
       NOT web_model_api_surface MATCHES
           "ResponseModelSchemaTag,[ \t]*SurfaceJsonResponse" OR
       NOT web_model_api_surface MATCHES
           "!ruvia::JsonBody<SurfaceJsonResponse>::value" OR
       NOT web_model_api_surface MATCHES
           "isResponseModel<SurfaceJsonResponse>" OR
       NOT web_model_api_surface MATCHES
           "!ruvia::JsonBody<ModelBodyDuckProbe>::value" OR
       NOT web_model_api_surface MATCHES
           "!ruvia::FormBody<ModelBodyDuckProbe>::value" OR
       NOT web_model_api_surface MATCHES
           "ruvia::detail::ModelInputKind" OR
       NOT web_json_package_consumer MATCHES
           "RUVIA_REQUEST_MODEL[(]InstalledPackageRequest" OR
       NOT web_json_package_consumer MATCHES
           "RUVIA_RESPONSE_MODEL[(]InstalledPackageResponse" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelDynamicGet<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelTypedDynamicGet<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelCompileTimeGetAlias<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelPublicBodyParseHooks<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelInputAccessor<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelPublicJsonDepthHook<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelPublicFormFieldsHook<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelNonConstNameGetter<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelPublicJsonWriterHooks<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!HasGeneratedModelPublicFieldStateHook<InstalledPackageRequest>" OR
       NOT web_json_package_consumer MATCHES
           "!ruvia::JsonBody<InstalledModelBodyDuckProbe>::value" OR
       NOT web_json_package_consumer MATCHES
           "!ruvia::FormBody<InstalledModelBodyDuckProbe>::value" OR
       NOT web_json_package_consumer MATCHES
           "ModelValidationAccess::fieldState<\"name\">" OR
       NOT web_json_package_consumer MATCHES
           "ruvia::toJson" OR
       NOT web_json_package_consumer MATCHES
           "JsonBody<InstalledPackageRequest>::parse" OR
       NOT web_json_package_consumer MATCHES
           "installedModelJson[ \t]*!=[ \t]*R" OR
       NOT web_json_package_consumer MATCHES
           "std::default_initializable<ruvia::detail::ModelInput>" OR
       NOT web_json_package_consumer MATCHES
           "name[(][)][-][>]resource[(][)][ \t]*!=[ \t]*&installedModelResource")
        boundary_error("request/response model schema boundary regressed"
            "request parsing and validation state must remain separate from response JSON writing")
    endif()
endif()
if(EXISTS "${WEB_MODEL_TYPES_CONTRACT}" AND
   EXISTS "${WEB_MODEL_FIELD_OPS_CONTRACT}" AND
   EXISTS "${WEB_MODEL_STRING_TEST}" AND
   EXISTS "${WEB_MODEL_LIST_TEST}" AND
   EXISTS "${WEB_MODEL_MATERIALIZATION_TEST}" AND
   EXISTS "${WEB_MODEL_API_SURFACE}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    if(NOT web_model_types_contract MATCHES
           "view[(][)] const &[ 	]+noexcept" OR
       NOT web_model_types_contract MATCHES
           "view[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_model_types_contract MATCHES
           "data[(][)] const &[ 	]+noexcept" OR
       NOT web_model_types_contract MATCHES
           "data[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_model_types_contract MATCHES
           "operator[ 	]+std::string_view[(][)] const &[ 	]+noexcept" OR
       NOT web_model_types_contract MATCHES
           "operator[ 	]+std::string_view[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_model_types_contract MATCHES
           "front[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_model_types_contract MATCHES
           "void[ 	]+begin[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_model_types_contract MATCHES
           "void[ 	]+end[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_model_types_contract MATCHES
           "emplace[(]Args&&[.][.][.][ 	]+args[)][ 	]+&" OR
       NOT web_model_types_contract MATCHES
           "emplaceMove[(]T&&[ 	]+value[)][ 	]+&")
        boundary_error("model owning values regained temporary borrow access"
            "FixedString, String, and List must expose storage-backed views, pointers, references, iterators, and inserted elements only from stable lvalues")
    endif()
    if(NOT web_model_field_ops_contract MATCHES
           "field[(][)] const &[ 	]*[{]" OR
       NOT web_model_field_ops_contract MATCHES
           "field[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_model_field_ops_contract MATCHES
           "field##Ensure[(][)][ 	]+&[ 	]*[{]" OR
       NOT web_model_field_ops_contract MATCHES
           "field##Ensure[(][)][ 	]+&&[ 	]*=[ 	]*delete" OR
       NOT web_model_field_ops_contract MATCHES
           "field[(]RuviaFieldValueT&&[ 	]+value[)][ 	]+&[ 	]*[{]" OR
       NOT web_model_field_ops_contract MATCHES
           "field[(]RuviaFieldValueT&&[)][ 	]+&&[ 	]*=[ 	]*delete")
        boundary_error("generated models regained temporary member references"
            "request and response field getters, ensure access, and chain setters must share the stable-lvalue contract")
    endif()
    foreach(model_owning_lifetime_coverage IN ITEMS
            "${web_model_string_test}"
            "${web_model_list_test}"
            "${web_model_materialization_test}"
            "${web_model_api_surface}"
            "${web_json_package_consumer}")
        if(model_owning_lifetime_coverage MATCHES
               "ExposesAnyRvalueModelStringBorrow" OR
           model_owning_lifetime_coverage MATCHES
               "ExposesAnyRvalueModelListBorrow" OR
           model_owning_lifetime_coverage MATCHES
               "ExposesAnyRvalueGenerated(Message|Name)Member")
            if(NOT model_owning_lifetime_coverage MATCHES
                   "static_assert[(]!Exposes")
                boundary_error("model temporary-borrow regression coverage is incomplete"
                    "unit, API-surface, and installed-package probes must assert rejection")
                break()
            endif()
        endif()
    endforeach()
    if(NOT web_model_string_test MATCHES
           "static_assert[(]!ExposesRvalueFixedStringView" OR
       NOT web_model_api_surface MATCHES
           "static_assert[(]!ExposesRvalueFixedStringView" OR
       NOT web_json_package_consumer MATCHES
           "static_assert[(]!ExposesRvalueFixedStringView")
        boundary_error("FixedString temporary-view coverage is incomplete"
            "direct, API-surface, and installed-package consumers must reject it")
    endif()
endif()
set(WEB_DIAGNOSTIC_ERROR_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Error.h")
set(WEB_DIAGNOSTIC_ERROR_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/Error.cpp")
set(WEB_DIAGNOSTIC_VALIDATOR_TEST
    "${RUVIA_ROOT}/tests/unit_validator.cpp")
set(WEB_DIAGNOSTIC_ERROR_TEST
    "${RUVIA_ROOT}/tests/unit_error.cpp")
set(WEB_DIAGNOSTIC_REQUEST_GUARD
    "${RUVIA_ROOT}/tests/guards/context_request_header_guard.cpp")
if(EXISTS "${WEB_REQUEST_FIELDS}" AND
   EXISTS "${WEB_MODEL_VALIDATION_CONTRACT}" AND
   EXISTS "${WEB_DIAGNOSTIC_ERROR_CONTRACT}" AND
   EXISTS "${WEB_DIAGNOSTIC_ERROR_SOURCE}" AND
   EXISTS "${WEB_DIAGNOSTIC_VALIDATOR_TEST}" AND
   EXISTS "${WEB_DIAGNOSTIC_ERROR_TEST}" AND
   EXISTS "${WEB_DIAGNOSTIC_REQUEST_GUARD}" AND
   EXISTS "${WEB_MODEL_API_SURFACE}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    file(READ "${WEB_REQUEST_FIELDS}"
        web_diagnostic_request_fields)
    file(READ "${WEB_MODEL_VALIDATION_CONTRACT}"
        web_diagnostic_validation_contract)
    file(READ "${WEB_DIAGNOSTIC_ERROR_CONTRACT}"
        web_diagnostic_error_contract)
    file(READ "${WEB_DIAGNOSTIC_ERROR_SOURCE}"
        web_diagnostic_error_source)
    file(READ "${WEB_DIAGNOSTIC_VALIDATOR_TEST}"
        web_diagnostic_validator_test)
    file(READ "${WEB_DIAGNOSTIC_ERROR_TEST}"
        web_diagnostic_error_test)
    file(READ "${WEB_DIAGNOSTIC_REQUEST_GUARD}"
        web_diagnostic_request_guard)
    if(NOT web_diagnostic_request_fields MATCHES
           "begin[(][)] const &[ 	]+noexcept" OR
       NOT web_diagnostic_request_fields MATCHES
           "begin[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_request_fields MATCHES
           "cbegin[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_request_fields MATCHES
           "end[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_request_fields MATCHES
           "cend[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_request_fields MATCHES
           "data[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_request_fields MATCHES
           "entries[(][)] const &[ 	]+noexcept" OR
       NOT web_diagnostic_request_fields MATCHES
           "entries[(][)] const &&[ 	]*=[ 	]*delete")
        boundary_error("request field collections regained temporary container borrows"
            "RequestNameValueList iterators, pointers, references, and spans must require a stable list lvalue")
    endif()
    if(NOT web_diagnostic_validation_contract MATCHES
           "field[(][)] const &[ 	]+noexcept" OR
       NOT web_diagnostic_validation_contract MATCHES
           "field[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_validation_contract MATCHES
           "code[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_validation_contract MATCHES
           "message[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_validation_contract MATCHES
           "issues[(][)] const &[ 	]+noexcept" OR
       NOT web_diagnostic_validation_contract MATCHES
           "issues[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_validation_contract MATCHES
           "info[(][)] const &[ 	]+noexcept" OR
       NOT web_diagnostic_validation_contract MATCHES
           "info[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_validation_contract MATCHES
           "message = \"is required\"[)][ 	]+&" OR
       NOT web_diagnostic_validation_contract MATCHES
           "message = \"is too short\"[)][ 	]+&" OR
       NOT web_diagnostic_validation_contract MATCHES
           "message = \"is too long\"[)][ 	]+&" OR
       NOT web_diagnostic_validation_contract MATCHES
           "message = \"is out of range\"[)][ 	]+&" OR
       NOT web_diagnostic_validation_contract MATCHES
           "message = \"is not allowed\"[)][ 	]+&")
        boundary_error("validation owning values regained temporary borrows or self references"
            "issues, error metadata, and Validator mutation must share one stable-lvalue contract")
    endif()
    if(NOT web_diagnostic_error_contract MATCHES
           "HttpErrorInfo[ 	]+info[(][)] const &[ 	]+noexcept" OR
       NOT web_diagnostic_error_contract MATCHES
           "HttpErrorInfo[ 	]+info[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_diagnostic_error_source MATCHES
           "HttpError::info[(][)] const &[ 	]+noexcept")
        boundary_error("HttpError regained temporary metadata borrowing"
            "the borrowed HttpErrorInfo projection must require a stable exception lvalue")
    endif()
    foreach(web_diagnostic_lifetime_coverage IN ITEMS
            "${web_diagnostic_validator_test}"
            "${web_diagnostic_error_test}"
            "${web_diagnostic_request_guard}"
            "${web_model_api_surface}"
            "${web_json_package_consumer}")
        if(web_diagnostic_lifetime_coverage MATCHES
               "ExposesAnyRvalue(RequestNameValueList|ValidationIssue|ValidationError)|ExposesRvalue(ValidatorIssues|HttpErrorInfo)|AcceptsAnyRvalueValidatorMutation" AND
           NOT web_diagnostic_lifetime_coverage MATCHES
               "static_assert[(]!Exposes|static_assert[(]!Accepts")
            boundary_error("Web diagnostic temporary-borrow coverage is incomplete"
                "direct, standalone, API-surface, and installed-package probes must assert rejection")
            break()
        endif()
    endforeach()
endif()
set(HTTP_URL_ENCODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/UrlEncoding.h")
set(WEB_FORM_DECODING_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/FormParser.h")
set(WEB_FORM_DECODING_VISITOR
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/RequestFieldVisitors.h")
set(WEB_FORM_DECODING_TEST
    "${RUVIA_ROOT}/tests/unit_form_parser.cpp")
set(WEB_FORM_MODEL_OBJECT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ModelObject.h")
set(WEB_FORM_MODEL_MACROS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/model/MacroFieldOps.h")
set(HTTP_URL_ENCODING_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("URL decoding must return one owning transactional result"
    "${RULE_STALE_URL_DECODE_CHAIN}"
    "${HTTP_URL_ENCODING_CONTRACT}"
    "${WEB_FORM_DECODING_CONTRACT}"
    "${WEB_FORM_DECODING_VISITOR}"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextFileResponse.cpp")
check_files_no_match("form values must return one complete typed result"
    "${RULE_STALE_FORM_VALUE_PARSE_CHAIN}"
    "${WEB_FORM_DECODING_CONTRACT}"
    "${WEB_FORM_MODEL_OBJECT}"
    "${WEB_FORM_MODEL_MACROS}")
if(EXISTS "${HTTP_URL_ENCODING_CONTRACT}" AND
   EXISTS "${WEB_FORM_DECODING_CONTRACT}" AND
   EXISTS "${WEB_FORM_DECODING_TEST}" AND
   EXISTS "${HTTP_URL_ENCODING_PACKAGE_CONSUMER}")
    file(READ "${HTTP_URL_ENCODING_CONTRACT}"
        http_url_encoding_contract)
    file(READ "${WEB_FORM_DECODING_CONTRACT}"
        web_form_decoding_contract)
    file(READ "${WEB_FORM_DECODING_TEST}"
        web_form_decoding_test)
    file(READ "${HTTP_URL_ENCODING_PACKAGE_CONSUMER}"
        http_url_encoding_package_consumer)
    if(NOT http_url_encoding_contract MATCHES
           "std::optional<std::pmr::string>[ \t\r\n]+decodeUrlComponent" OR
       NOT web_form_decoding_contract MATCHES
           "value[.]assignOwned[(]std::move[(][*]decoded(Storage)?[)][)]" OR
       NOT web_form_decoding_test MATCHES
           "form_value_decode_failure_returns_no_partial_value" OR
       NOT http_url_encoding_package_consumer MATCHES
           "AcceptsUrlDecodeOutputParameter" OR
       NOT http_url_encoding_package_consumer MATCHES
           "std::optional<std::pmr::string>")
        boundary_error("URL decoding lost transactional ownership"
            "HTTP must own the decoded optional and Web model fields must commit only after success")
    endif()
endif()
if(EXISTS "${WEB_FORM_DECODING_CONTRACT}" AND
   EXISTS "${WEB_FORM_DECODING_TEST}" AND
   EXISTS "${WEB_FORM_MODEL_MACROS}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    file(READ "${WEB_FORM_MODEL_MACROS}"
        web_form_model_macros)
    file(READ "${WEB_JSON_PACKAGE_CONSUMER}"
        web_form_package_consumer)
    if(NOT web_form_decoding_contract MATCHES
           "enum[ \t]+class[ \t]+FormValueEncoding" OR
       NOT web_form_decoding_contract MATCHES
           "std::optional<T>[ \t\r\n]+parseFormValue" OR
       NOT web_form_model_macros MATCHES
           "FormValueEncoding::kUrlEncoded" OR
       NOT web_form_model_macros MATCHES
           "FormValueEncoding::kDecoded" OR
       NOT web_form_model_macros MATCHES
           "ruviaValue[.]has_value[(][)]" OR
       NOT web_form_decoding_test MATCHES
           "form_value_encoding_is_explicit" OR
       NOT web_form_package_consumer MATCHES
           "AcceptsFormValueOutputParameter" OR
       NOT web_form_package_consumer MATCHES
           "std::optional<ruvia::String>")
        boundary_error("form model parsing lost its typed value path"
            "URL-encoded and decoded form fields must share one explicit optional-value parser")
    endif()
endif()
check_files_no_match("HTTP/1 request-body plans must use exclusive alternatives"
    "${RULE_STALE_HTTP1_REQUEST_BODY_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderContentLength.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerRequestState.h"
    "${RUVIA_ROOT}/tests/unit_http1_parser.cpp"
    "${RUVIA_ROOT}/tests/unit_request_body_decoding.cpp"
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp")
check_files_no_match("HTTP/1 request-body plans must use parser-only constructors"
    "${RULE_STALE_HTTP1_REQUEST_BODY_FACTORIES}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
check_files_no_match("server Expect semantics must use one cross-version typed state"
    "${RULE_STALE_SERVER_EXPECTATION_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpParseError.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpParseError.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HeaderTokenUtils.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP message protocol version must use one typed control datum"
    "${RULE_STALE_HTTP_PROTOCOL_VERSION_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpRequest.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestInternal.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientAccess.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketValidation.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp"
    "${RUVIA_ROOT}/tests/unit_request_access.cpp")
check_files_no_match("HTTP/1 client requests must use one typed writer contract"
    "${RULE_STALE_HTTP1_CLIENT_REQUEST_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_redirect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/tests/smoke_http_target.cpp")
check_files_no_match("outbound request content must use exclusive alternatives"
    "${RULE_STALE_OUTBOUND_REQUEST_CONTENT_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestContent.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/examples/api_surface.cpp")
check_files_no_match("HTTP/1 client response framing must use one typed plan"
    "${RULE_STALE_HTTP1_CLIENT_RESPONSE_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/1 client response plans must use exclusive alternatives"
    "${RULE_STALE_HTTP1_CLIENT_RESPONSE_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/tests/smoke_http_target.cpp")
check_files_no_match("HTTP/1 client response parsing must use the public discriminated API"
    "${RULE_STALE_HTTP1_CLIENT_RESPONSE_PARSER_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientAccess.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/1 request and response must share Content-Length parsing"
    "${RULE_STALE_HTTP_CONTENT_LENGTH_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
check_files_no_match("HTTP/1 request and response must share Transfer-Encoding parsing"
    "${RULE_STALE_HTTP_TRANSFER_ENCODING_SPLIT}"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
check_files_no_match("HTTP client method semantics must remain case-sensitive"
    "${RULE_STALE_HTTP_CLIENT_METHOD_CASE_FOLD}"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp")
check_files_no_match("ruvia-web must not pass loose response-body protocol booleans"
    "${RULE_WEB_RESPONSE_BODY_PROTOCOL_BOOL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/Http2BufferedResponseWrite.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h")

set(HTTP_RESPONSE_BODY_STORAGE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseBody.h")
set(HTTP_RESPONSE_FILE_BODY_VIEW
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseFileBody.h")
set(HTTP_RESPONSE_MODEL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpResponse.h")
set(HTTP_RESPONSE_BODY_ACCESS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseBodyAccess.h")
set(HTTP_RESPONSE_FILE_ACCESS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseFileAccess.h")
set(HTTP_RESPONSE_STORAGE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp")
set(HTTP_RESPONSE_HEADER_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponseHeaderOps.cpp")
set(HTTP_RESPONSE_WRITE_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h")
set(HTTP_RESPONSE_H2_CONNECTION
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(WEB_BUFFERED_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h")
set(WEB_RESPONSE_COMPRESSION
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpResponseCompression.cpp")
set(WEB_RESPONSE_H2_WRITER
    "${RUVIA_ROOT}/ruvia-web/src/server/Http2BufferedResponseWrite.cpp")
set(HTTP_RESPONSE_BODY_TEST
    "${RUVIA_ROOT}/tests/unit_http_response_body.cpp")
set(HTTP_RESPONSE_PUBLIC_TEST
    "${RUVIA_ROOT}/tests/unit_http_response.cpp")
set(HTTP_RESPONSE_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(response_body_contract_file IN ITEMS
        "${HTTP_RESPONSE_BODY_STORAGE}"
        "${HTTP_RESPONSE_FILE_BODY_VIEW}"
        "${HTTP_RESPONSE_MODEL}"
        "${HTTP_RESPONSE_BODY_ACCESS}"
        "${HTTP_RESPONSE_FILE_ACCESS}"
        "${HTTP_RESPONSE_STORAGE_SOURCE}"
        "${HTTP_RESPONSE_HEADER_SOURCE}"
        "${HTTP_RESPONSE_WRITE_PLAN}"
        "${HTTP_RESPONSE_H2_CONNECTION}"
        "${WEB_BUFFERED_RESPONSE_WRITER}"
        "${WEB_RESPONSE_COMPRESSION}"
        "${WEB_RESPONSE_H2_WRITER}"
        "${HTTP_RESPONSE_BODY_TEST}"
        "${HTTP_RESPONSE_PUBLIC_TEST}"
        "${HTTP_RESPONSE_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${response_body_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${response_body_contract_file}")
        boundary_error("typed HttpResponse body contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_RESPONSE_BODY_STORAGE}" AND
   EXISTS "${HTTP_RESPONSE_FILE_BODY_VIEW}" AND
   EXISTS "${HTTP_RESPONSE_MODEL}" AND
   EXISTS "${HTTP_RESPONSE_BODY_ACCESS}" AND
   EXISTS "${HTTP_RESPONSE_FILE_ACCESS}" AND
   EXISTS "${HTTP_RESPONSE_STORAGE_SOURCE}" AND
   EXISTS "${HTTP_RESPONSE_HEADER_SOURCE}" AND
   EXISTS "${HTTP_RESPONSE_WRITE_PLAN}" AND
   EXISTS "${HTTP_RESPONSE_H2_CONNECTION}" AND
   EXISTS "${WEB_BUFFERED_RESPONSE_WRITER}" AND
   EXISTS "${WEB_RESPONSE_COMPRESSION}" AND
   EXISTS "${WEB_RESPONSE_H2_WRITER}" AND
   EXISTS "${HTTP_RESPONSE_BODY_TEST}" AND
   EXISTS "${HTTP_RESPONSE_PUBLIC_TEST}" AND
   EXISTS "${HTTP_RESPONSE_PACKAGE_CONSUMER}")
    file(READ "${HTTP_RESPONSE_BODY_STORAGE}" http_response_body_storage)
    file(READ "${HTTP_RESPONSE_FILE_BODY_VIEW}" http_response_file_body_view)
    file(READ "${HTTP_RESPONSE_MODEL}" http_response_storage_model)
    file(READ "${HTTP_RESPONSE_BODY_ACCESS}" http_response_body_access)
    file(READ "${HTTP_RESPONSE_FILE_ACCESS}" http_response_file_access)
    file(READ "${HTTP_RESPONSE_STORAGE_SOURCE}" http_response_storage_source)
    file(READ "${HTTP_RESPONSE_HEADER_SOURCE}" http_response_header_source)
    file(READ "${HTTP_RESPONSE_WRITE_PLAN}" http_response_storage_write_plan)
    read_http2_connection_implementation(http_response_storage_h2)
    file(READ "${WEB_BUFFERED_RESPONSE_WRITER}" web_buffered_response_writer)
    file(READ "${WEB_RESPONSE_COMPRESSION}" web_response_compression)
    file(READ "${WEB_RESPONSE_H2_WRITER}" web_response_h2_writer)
    file(READ "${HTTP_RESPONSE_BODY_TEST}" http_response_body_test)
    file(READ "${HTTP_RESPONSE_PUBLIC_TEST}" http_response_public_test)
    file(READ "${HTTP_RESPONSE_PACKAGE_CONSUMER}"
        http_response_package_consumer)
    if(NOT http_response_body_storage MATCHES
           "class HttpEmptyResponseBody final" OR
       NOT http_response_body_storage MATCHES
           "class HttpBorrowedResponseBytes final" OR
       NOT http_response_body_storage MATCHES
           "class HttpStaticResponseBytes final" OR
       NOT http_response_body_storage MATCHES
           "class HttpOwnedResponseBytes final" OR
       NOT http_response_body_storage MATCHES
           "class HttpOwnedResponseFile final" OR
       NOT http_response_body_storage MATCHES
           "class HttpBorrowedResponseFile final" OR
       NOT http_response_body_storage MATCHES
           "class HttpResponseBody final" OR
       NOT http_response_body_storage MATCHES "using Value = std::variant" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpEmptyResponseBody>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpBorrowedResponseBytes>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpStaticResponseBytes>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpOwnedResponseBytes>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpOwnedResponseFile>" OR
       NOT http_response_body_storage MATCHES
           "std::get_if<HttpBorrowedResponseFile>" OR
       NOT http_response_body_storage MATCHES
           "std::optional<ResponseFileBody> file[(][)] const [&] noexcept")
        boundary_error("HttpResponse body lost its exclusive storage alternatives"
            "empty, borrowed/static/owned bytes, and owned/borrowed files must remain one discriminated value")
    endif()
    if(NOT http_response_storage_model MATCHES
           "headers[(][)] const [&] noexcept" OR
       NOT http_response_storage_model MATCHES
           "headers[(][)] const && = delete" OR
       NOT http_response_storage_model MATCHES
           "header[ \t\r\n]*[(][^)]*std::string_view[^)]*[)][ \t\r\n]*const && = delete" OR
       NOT http_response_storage_model MATCHES
           "begin[(][)] const [&] noexcept" OR
       NOT http_response_storage_model MATCHES
           "cend[(][)] const && = delete" OR
       NOT http_response_storage_source MATCHES
           "HttpResponse::headers[(][)] const [&] noexcept" OR
       NOT http_response_header_source MATCHES
           "HttpResponse::header[(][^)]*std::string_view[^)]*[)] const [&] noexcept" OR
       NOT http_response_body_storage MATCHES
           "empty[(][)] const [&] noexcept" OR
       NOT http_response_body_storage MATCHES
           "ownedBytes[(][)] const && = delete" OR
       NOT http_response_body_storage MATCHES
           "file[(][)] const && = delete" OR
       NOT http_response_body_storage MATCHES
           "nativePathCStr[(][)] const && = delete" OR
       NOT http_response_body_access MATCHES
           "static const HttpResponseBody& body[ \t\r\n]*[(][ \t\r\n]*const HttpResponse&&[^)]*[)][ \t\r\n]*= delete" OR
       NOT http_response_body_access MATCHES
           "responseBody[ \t\r\n]*[(][ \t\r\n]*const HttpResponse&&[^)]*[)][ \t\r\n]*= delete" OR
       NOT http_response_public_test MATCHES
           "ExposesAnyRvalueResponseView" OR
       NOT http_response_body_test MATCHES
           "ExposesAnyRvalueResponseBodyBorrow" OR
       NOT http_response_body_test MATCHES
           "ExposesRvalueResponseBodyAccess" OR
       NOT http_response_package_consumer MATCHES
           "ExposesAnyRvalueResponseView" OR
       NOT http_response_package_consumer MATCHES
           "ExposesAnyRvalueResponseBodyBorrow" OR
       NOT http_response_package_consumer MATCHES
           "ExposesRvalueResponseBodyAccess")
        boundary_error("HttpResponse owning state exposes borrows from temporary owners"
            "headers, body alternatives, owned bytes/files, and responseBody access must require a live lvalue response")
    endif()
    if(http_response_storage_model MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       http_response_storage_source MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       NOT http_response_storage_model MATCHES
           "detail::HttpResponseBody body_" OR
       NOT http_response_file_body_view MATCHES
           "class ResponseFileBody final" OR
       NOT http_response_file_body_view MATCHES
           "friend class HttpResponseBody" OR
       NOT http_response_file_body_view MATCHES
           "std::uint64_t length[(][)] const noexcept")
        boundary_error("HttpResponse restored enum plus parallel payload storage"
            "the model must own only HttpResponseBody and file descriptors must come from its active file alternative")
    endif()
    if(http_response_body_access MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       http_response_file_access MATCHES
           "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}" OR
       NOT http_response_body_access MATCHES
           "const HttpResponseBody& responseBody" OR
       NOT http_response_storage_write_plan MATCHES
           "responseBody[(]response[)][.]size[(][)]" OR
       NOT http_response_storage_h2 MATCHES
           "const auto& body = responseBody[(]response[)]" OR
       NOT web_buffered_response_writer MATCHES
           "const auto& responseContent = responseBody[(]response[)]" OR
       NOT web_buffered_response_writer MATCHES
           "responseContent[.]file[(][)]" OR
       NOT web_response_compression MATCHES
           "const auto& responseContent = responseBody[(]response[)]" OR
       NOT web_response_h2_writer MATCHES
           "const auto& content = responseBody[(]response[)]")
        boundary_error("response writers bypass the unified body read contract"
            "HTTP planning, H1/H2 drivers, and compression must derive bytes/file/size from responseBody(response)")
    endif()
    if(NOT http_response_body_test MATCHES
           "response_body_has_one_storage_alternative" OR
       NOT http_response_body_test MATCHES
           "response_body_materializes_only_ephemeral_borrow" OR
       NOT http_response_body_test MATCHES
           "response_body_file_view_is_atomic_and_non_default" OR
       NOT http_response_body_test MATCHES
           "response_body_file_transition_validates_before_replacement" OR
       NOT http_response_package_consumer MATCHES
           "const ruvia::detail::HttpResponseBody&" OR
       NOT http_response_package_consumer MATCHES
           "HttpBorrowedResponseFile" OR
       NOT http_response_package_consumer MATCHES "ResponseFileBody")
        boundary_error("typed HttpResponse body lacks regression coverage"
            "unit and installed-package consumers must pin alternatives, materialization, atomic file views, and removed default states")
    endif()
endif()
check_files_no_match("HttpResponse restored split body storage or read side channels"
    "${RULE_STALE_RESPONSE_BODY_STORAGE_SPLIT}"
    "${HTTP_RESPONSE_MODEL}"
    "${HTTP_RESPONSE_BODY_ACCESS}"
    "${HTTP_RESPONSE_FILE_ACCESS}"
    "${HTTP_RESPONSE_STORAGE_SOURCE}"
    "${HTTP_RESPONSE_WRITE_PLAN}"
    "${HTTP_RESPONSE_H2_CONNECTION}"
    "${WEB_BUFFERED_RESPONSE_WRITER}"
    "${WEB_RESPONSE_COMPRESSION}"
    "${WEB_RESPONSE_H2_SESSION}")
check_files_no_match("ruvia-web HTTP/2 runtime must not recompute the response write plan"
    "${RULE_WEB_H2_RESPONSE_PLAN_DUPLICATION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("ruvia-web must not decide HEAD response body semantics"
    "${RULE_WEB_HEAD_BODY_DECISION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
check_files_no_match("HTTP/1 response-head framing must not collapse to a boolean"
    "${RULE_STALE_HTTP1_RESPONSE_HEAD_SCALAR}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHead.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadPolicy.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h"
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
check_files_no_match("HTTP/2 send path must not restore ambiguous retry ownership"
    "${RULE_STALE_H2_SEND_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
check_files_no_match("HTTP/2 local content accounting must use exclusive alternatives"
    "${RULE_STALE_H2_LOCAL_CONTENT_MODE_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalContentState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 local send permission must use one exclusive state"
    "${RULE_STALE_H2_LOCAL_SEND_PRODUCT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalSendState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamLifecycle.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamTable.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
check_files_no_match("HTTP/2 remote receive permission must use one exclusive state"
    "${RULE_STALE_H2_REMOTE_RECEIVE_PRODUCT}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteReceiveState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamLifecycle.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamRequestState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
check_files_no_match("HTTP/2 remote content accounting must use exclusive alternatives"
    "${RULE_STALE_H2_REMOTE_CONTENT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteContentState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("response trailers must remain one terminal section"
    "${RULE_STALE_RESPONSE_TRAILER_SIDE_CHANNEL}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Streaming.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/StreamingInternal.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_streaming.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
check_files_no_match("HTTP/2 response trailers must not have staged per-stream ownership"
    "${RULE_STALE_H2_RESPONSE_TRAILER_STAGING}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamHeaderBlocks.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("HTTP/2 response finish must receive the complete terminal section explicitly"
    "${RULE_IMPLICIT_H2_RESPONSE_FINISH}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("response-stream runtime must consume the typed commit plan"
    "${RULE_STALE_RESPONSE_STREAM_COMMIT_BOOL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
check_files_no_match("HTTP/1 response-stream termination must remain exclusive"
    "bool[ \t]+aborted_"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
check_files_no_match("response-stream status must follow exclusive commit results"
    "${RULE_STALE_RESPONSE_STREAM_STATUS_SPLIT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteStreamState.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterMiddlewareDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("Next continuation state must remain fully typed"
    "${RULE_STALE_NEXT_CONTINUATION_TYPE_ERASURE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Next.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteStreamState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterMiddlewareDispatch.cpp")
check_files_no_match("Router PImpl deleter must not escape the Router private contract"
    "RouterImplDeleter"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Router.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/Router.cpp")
check_files_no_match("App PImpl deleter must not escape the App private contract"
    "AppStateDeleter"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/App.h"
    "${RUVIA_ROOT}/ruvia-web/src/app/App.cpp")
check_files_no_match("Env PImpl deleter must not escape the Env private contract"
    "EnvStateDeleter"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Dotenv.h"
    "${RUVIA_ROOT}/ruvia-web/src/app/Dotenv.cpp")
set(WEB_ENV_CONTRACT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Dotenv.h")
set(WEB_ENV_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/app/Dotenv.cpp")
set(WEB_ENV_TEST
    "${RUVIA_ROOT}/tests/unit_dotenv_parser.cpp")
if(EXISTS "${WEB_ENV_CONTRACT}" AND
   EXISTS "${WEB_ENV_SOURCE}" AND
   EXISTS "${WEB_ENV_TEST}" AND
   EXISTS "${WEB_MODEL_API_SURFACE}" AND
   EXISTS "${WEB_JSON_PACKAGE_CONSUMER}")
    file(READ "${WEB_ENV_CONTRACT}" web_env_contract)
    file(READ "${WEB_ENV_SOURCE}" web_env_source)
    file(READ "${WEB_ENV_TEST}" web_env_test)
    if(NOT web_env_contract MATCHES
           "get[(][ \t\r\n]*std::string_view name[)] const &[ \t]+noexcept" OR
       NOT web_env_contract MATCHES
           "get[(][ \t\r\n]*std::string_view[)] const &&[ \t]*=[ \t]*delete" OR
       NOT web_env_contract MATCHES
           "remove_cvref_t<T>>[ \t\r\n]+get[(]" OR
       NOT web_env_source MATCHES
           "Env::get[(][ \t\r\n]*std::string_view name[)] const &[ \t]+noexcept")
        boundary_error("Env regained temporary owning-value borrowing"
            "both raw and typed get overloads must require a stable Env lvalue")
    endif()
    foreach(web_env_lifetime_coverage IN ITEMS
            "${web_env_test}"
            "${web_model_api_surface}"
            "${web_json_package_consumer}")
        if(NOT web_env_lifetime_coverage MATCHES
               "ExposesAnyRvalueEnvBorrow" OR
           NOT web_env_lifetime_coverage MATCHES
               "static_assert[(]!ExposesAnyRvalueEnvBorrow<ruvia::Env>[)]")
            boundary_error("Env temporary-borrow coverage is incomplete"
                "direct, API-surface, and installed-package probes must reject rvalue get")
            break()
        endif()
    endforeach()
endif()
check_files_no_match("StaticRoot PImpl deleter must not escape the StaticRoot private contract"
    "StaticRootStateDeleter"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/StaticFiles.h"
    "${RUVIA_ROOT}/ruvia-web/src/StaticFiles.cpp")
check_files_no_match("HTTP/1 session completion must not split wire, connection, and buffer state"
    "${RULE_STALE_HTTP1_SESSION_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1SessionRequestCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpConnectionState.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpConnectionState.cpp")
check_files_no_match("HTTP/1 request limit must use one connection-private sequence"
    "${RULE_STALE_HTTP1_REQUEST_SEQUENCE_SCALARS}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1RequestSequence.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl"
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
set(WEB_HTTP1_REQUEST_SEQUENCE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1RequestSequence.h")
set(WEB_HTTP1_REQUEST_SEQUENCE_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
if(EXISTS "${WEB_HTTP1_REQUEST_SEQUENCE}" AND
   EXISTS "${WEB_HTTP1_REQUEST_SEQUENCE_TEST}" AND
   EXISTS "${WEB_SERVER_OPTIONS_MODEL}" AND
   EXISTS "${WEB_APP_PUBLIC_MODEL}" AND
   EXISTS "${WEB_SERVER_CONFIG_PACKAGE_CONSUMER}")
    file(READ "${WEB_HTTP1_REQUEST_SEQUENCE}" web_http1_request_sequence)
    file(READ "${WEB_HTTP1_REQUEST_SEQUENCE_TEST}"
        web_http1_request_sequence_test)
    file(READ "${WEB_SERVER_OPTIONS_MODEL}" web_limit_server_options)
    file(READ "${WEB_APP_PUBLIC_MODEL}" web_limit_app_api)
    file(READ "${WEB_SERVER_CONFIG_PACKAGE_CONSUMER}"
        web_limit_package_consumer)
    if(NOT web_http1_request_sequence MATCHES
           "std::optional<std::size_t>[ 	]+requestsUntilClose_" OR
       NOT web_http1_request_sequence MATCHES
           "Http1RequestSequence[(]std::size_t[)][ 	]*=[ 	]*delete" OR
       NOT web_http1_request_sequence_test MATCHES
           "http1_request_sequence_rejects_configured_zero_budget" OR
       NOT web_limit_server_options MATCHES
           "std::optional<std::size_t>[ 	]+maxConnections" OR
       NOT web_limit_server_options MATCHES
           "std::optional<std::size_t>[ 	]+keepaliveRequests" OR
       NOT web_limit_app_api MATCHES
           "setMaxConnectionsPerWorker[(]std::optional<std::size_t>" OR
       NOT web_limit_app_api MATCHES
           "setKeepaliveRequests[(]std::optional<std::size_t>" OR
       NOT web_limit_package_consumer MATCHES
           "AppSetOptionalSizeFunction")
        boundary_error("Web connection/request limits regained zero sentinels"
            "absence must mean unlimited from App configuration through admission and the connection-private request sequence")
    endif()
endif()
check_files_no_match("response stream must end in Context scope without dummy payload"
    "${RULE_LATE_RESPONSE_STREAM_END}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h")

set(HTTP_RESPONSE_STREAM_COMMIT_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h")
set(HTTP1_RESPONSE_STREAM_COMMIT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP2_RESPONSE_STREAM_COMMIT
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(WEB_ROUTE_TABLE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h")
set(WEB_ROUTE_STREAM_DISPATCH_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
set(WEB_RESPONSE_STREAM_DISPATCH_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h")
set(WEB_RESPONSE_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamState.h")
set(WEB_HTTP1_RESPONSE_STREAM_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
set(WEB_HTTP1_SESSION_REQUEST_COMPLETION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1SessionRequestCompletion.h")
set(WEB_HTTP1_STREAM_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_HTTP2_STREAM_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(RESPONSE_STREAM_STATUS_TEST
    "${RUVIA_ROOT}/tests/unit_response_stream_dispatch.cpp")
set(RESPONSE_STREAM_LIFECYCLE_TEST
    "${RUVIA_ROOT}/tests/unit_streaming.cpp")
set(HTTP1_SESSION_COMPLETION_TEST
    "${RUVIA_ROOT}/tests/unit_connection_read_buffer.cpp")
set(RESPONSE_STREAM_H2_RUNTIME_TEST
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
set(RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(RESPONSE_STREAM_WEB_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(response_stream_status_contract IN ITEMS
        "${HTTP_RESPONSE_STREAM_COMMIT_PLAN}"
        "${HTTP1_RESPONSE_STREAM_COMMIT}"
        "${HTTP2_RESPONSE_STREAM_COMMIT}"
        "${WEB_ROUTE_TABLE}"
        "${WEB_ROUTE_STREAM_DISPATCH_SOURCE}"
        "${WEB_RESPONSE_STREAM_DISPATCH_RESULT}"
        "${WEB_RESPONSE_STREAM_STATE}"
        "${WEB_HTTP1_RESPONSE_STREAM_ROUTE}"
        "${WEB_HTTP1_SESSION_REQUEST_COMPLETION}"
        "${WEB_HTTP1_STREAM_SESSION}"
        "${WEB_HTTP2_STREAM_SESSION}"
        "${RESPONSE_STREAM_STATUS_TEST}"
        "${RESPONSE_STREAM_LIFECYCLE_TEST}"
        "${HTTP1_SESSION_COMPLETION_TEST}"
        "${RESPONSE_STREAM_H2_RUNTIME_TEST}"
        "${RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER}"
        "${RESPONSE_STREAM_WEB_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${response_stream_status_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${response_stream_status_contract}")
        boundary_error("response-stream status contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_RESPONSE_STREAM_COMMIT_PLAN}" AND
   EXISTS "${HTTP1_RESPONSE_STREAM_COMMIT}" AND
   EXISTS "${HTTP2_RESPONSE_STREAM_COMMIT}" AND
   EXISTS "${WEB_ROUTE_TABLE}" AND
   EXISTS "${WEB_ROUTE_STREAM_DISPATCH_SOURCE}" AND
   EXISTS "${WEB_RESPONSE_STREAM_DISPATCH_RESULT}" AND
   EXISTS "${WEB_RESPONSE_STREAM_STATE}" AND
   EXISTS "${WEB_HTTP1_RESPONSE_STREAM_ROUTE}" AND
   EXISTS "${WEB_HTTP1_SESSION_REQUEST_COMPLETION}" AND
   EXISTS "${WEB_HTTP1_STREAM_SESSION}" AND
   EXISTS "${WEB_HTTP2_STREAM_SESSION}" AND
   EXISTS "${RESPONSE_STREAM_STATUS_TEST}" AND
   EXISTS "${RESPONSE_STREAM_LIFECYCLE_TEST}" AND
   EXISTS "${HTTP1_SESSION_COMPLETION_TEST}" AND
   EXISTS "${RESPONSE_STREAM_H2_RUNTIME_TEST}" AND
   EXISTS "${RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER}" AND
   EXISTS "${RESPONSE_STREAM_WEB_PACKAGE_CONSUMER}")
    file(READ "${HTTP_RESPONSE_STREAM_COMMIT_PLAN}"
        response_stream_commit_plan)
    file(READ "${HTTP1_RESPONSE_STREAM_COMMIT}"
        http1_response_stream_commit)
    read_http2_connection_implementation(http2_response_stream_commit)
    file(READ "${WEB_ROUTE_TABLE}"
        web_route_table)
    file(READ "${WEB_ROUTE_STREAM_DISPATCH_SOURCE}"
        web_route_stream_dispatch_source)
    file(READ "${WEB_RESPONSE_STREAM_DISPATCH_RESULT}"
        web_response_stream_dispatch_result)
    file(READ "${WEB_RESPONSE_STREAM_STATE}"
        web_response_stream_state)
    file(READ "${WEB_HTTP1_RESPONSE_STREAM_ROUTE}"
        web_http1_response_stream_route)
    file(READ "${WEB_HTTP1_SESSION_REQUEST_COMPLETION}"
        web_http1_session_request_completion)
    file(READ "${WEB_HTTP1_STREAM_SESSION}"
        web_http1_stream_session)
    file(READ "${WEB_HTTP2_STREAM_SESSION}"
        web_http2_stream_session)
    file(READ "${RESPONSE_STREAM_STATUS_TEST}"
        response_stream_status_test)
    file(READ "${RESPONSE_STREAM_LIFECYCLE_TEST}"
        response_stream_lifecycle_test)
    file(READ "${HTTP1_SESSION_COMPLETION_TEST}"
        http1_session_completion_test)
    file(READ "${RESPONSE_STREAM_H2_RUNTIME_TEST}"
        response_stream_h2_runtime_test)
    file(READ "${RESPONSE_STREAM_HTTP_PACKAGE_CONSUMER}"
        response_stream_http_package_consumer)
    file(READ "${RESPONSE_STREAM_WEB_PACKAGE_CONSUMER}"
        response_stream_web_package_consumer)

    if(NOT response_stream_commit_plan MATCHES
           "std::uint16_t responseStatus[(][)] const noexcept" OR
       NOT response_stream_commit_plan MATCHES
           "ResponseStreamFraming framing[(][)] const noexcept" OR
       NOT response_stream_commit_plan MATCHES
           "HttpKnownMethod requestMethod" OR
       NOT response_stream_commit_plan MATCHES
           "response[.]status[(][)] != commitPlan[.]responseStatus[(][)]" OR
       NOT http1_response_stream_commit MATCHES
           "httpResponseStreamCommitPlan" OR
       NOT http1_response_stream_commit MATCHES
           "plan[.]framing[(][)]" OR
       NOT http1_response_stream_commit MATCHES
           "response[.]status[(][)]" OR
       NOT http2_response_stream_commit MATCHES
           "ResponseStreamFraming::kHttp2Frames" OR
       NOT http2_response_stream_commit MATCHES
           "head[.]status[(][)]")
        boundary_error("response-stream protocol commit lost the final status"
            "the HTTP commit plan must bind response status, framing, method-derived body semantics, and the prepared head")
    endif()

    if(NOT web_route_table MATCHES
           "Task<std::optional<HttpResponse>> dispatchResponseStream" OR
       NOT web_route_table MATCHES
           "Task<std::optional<HttpResponse>> dispatchWebSocket" OR
       web_route_table MATCHES
           "StreamDispatchResult|StreamRouteHandled" OR
       NOT web_route_stream_dispatch_source MATCHES
           "co_return std::nullopt" OR
       NOT web_route_stream_dispatch_source MATCHES
           "responseStreamOutput->writer[(][)][.]end[(][)]" OR
       NOT web_route_stream_dispatch_source MATCHES
           "Context is local to this coroutine" OR
       NOT web_response_stream_dispatch_result MATCHES
           "ResponseStreamPeerAbortedBeforeCommit" OR
       NOT web_response_stream_dispatch_result MATCHES
           "class ResponseStreamCompleted final" OR
       NOT web_response_stream_dispatch_result MATCHES
           "class ResponseStreamPeerAbortedAfterCommit final" OR
       NOT web_response_stream_dispatch_result MATCHES
           "class ResponseStreamFailedAfterCommit final" OR
       NOT web_response_stream_dispatch_result MATCHES
           "class ResponseStreamRouteResponse final" OR
       NOT web_response_stream_dispatch_result MATCHES
           "class ResponseStreamRecoveredFailure final" OR
       NOT web_response_stream_dispatch_result MATCHES
           "committedStatus[(][)] const noexcept" OR
       NOT web_response_stream_dispatch_result MATCHES
           "routeResponse[(][)] [&] noexcept" OR
       NOT web_response_stream_dispatch_result MATCHES
           "recoveredFailure[(][)] [&] noexcept" OR
       web_response_stream_dispatch_result MATCHES
           "ResponseStreamCommittedOutcome|ResponseStreamBufferedOutcome|class ResponseStreamCommitted final|class ResponseStreamBuffered final|makeCommitted[(]|makeBuffered[(]|bool failed[(][)] const noexcept" OR
       NOT web_response_stream_dispatch_result MATCHES
           "committedResponseStreamStatus" OR
       NOT web_response_stream_state MATCHES
           "using State = std::variant<" OR
       NOT web_response_stream_state MATCHES
           "struct Unbound final" OR
       NOT web_response_stream_state MATCHES
           "struct Bound final" OR
       NOT web_response_stream_state MATCHES
           "struct Detached final" OR
       NOT web_response_stream_state MATCHES
           "struct BodyOpen final" OR
       NOT web_response_stream_state MATCHES
           "struct TrailersOnly final" OR
       NOT web_response_stream_state MATCHES
           "struct Ended final" OR
       NOT web_response_stream_state MATCHES
           "struct AbortedBeforeCommit final" OR
       NOT web_response_stream_state MATCHES
           "struct AbortedAfterCommit final" OR
       NOT web_response_stream_state MATCHES
           "bool aborted[(][)] const noexcept" OR
       NOT web_response_stream_state MATCHES
           "void markAborted[(][)] noexcept" OR
       web_response_stream_state MATCHES
           "std::optional<CommittedState>|context_|streamingHead_|committed_|enum class Phase" OR
       NOT web_response_stream_state MATCHES
           "commitPlan[(][)] const [&] noexcept" OR
       NOT web_response_stream_state MATCHES
           "void releaseContext[(][)] noexcept" OR
       NOT web_route_stream_dispatch_source MATCHES
           "class ResponseStreamContextBinding final" OR
       NOT web_route_stream_dispatch_source MATCHES
           "StreamingAccess::releaseContext" OR
       NOT web_http1_response_stream_route MATCHES
           "Task<Http1SessionRequestCompletion>" OR
       NOT web_http1_response_stream_route MATCHES
           "makeCommittedStream" OR
       NOT web_http1_response_stream_route MATCHES
           "result[.]committedStatus[(][)]" OR
       NOT web_http1_response_stream_route MATCHES
           "result[.]completed[(][)]" OR
       NOT web_http1_response_stream_route MATCHES
           "result[.]routeResponse[(][)]" OR
       NOT web_http1_response_stream_route MATCHES
           "result[.]recoveredFailure[(][)]")
        boundary_error("Web response-stream outcomes restored nested discriminators"
            "each terminal must remain one variant alternative; HTTP/1 and HTTP/2 must consume the exact terminal without reconstructing status or recovery state")
    endif()

    if(NOT web_http1_session_request_completion MATCHES
           "class Http1SessionRequestCompletion final" OR
       NOT web_http1_session_request_completion MATCHES
           "class Http1CommittedStreamResponse final" OR
       NOT web_http1_session_request_completion MATCHES
           "class Http1RequestBufferCompletion final" OR
       NOT web_http1_session_request_completion MATCHES
           "Http1RequestBufferDiscarded" OR
       NOT web_http1_session_request_completion MATCHES
           "Http1RequestBufferCompaction" OR
       NOT web_http1_session_request_completion MATCHES
           "Http1RequestBufferPipelineRestore" OR
       NOT web_http1_session_request_completion MATCHES
           "makeBufferedClosing" OR
       NOT web_http1_session_request_completion MATCHES
           "makeBufferedUnrestored" OR
       NOT web_http1_session_request_completion MATCHES
           "makeBufferedPipelineRestore" OR
       NOT web_http1_session_request_completion MATCHES
           "makeCommittedStream" OR
       NOT web_http1_session_request_completion MATCHES
           "connectionPlan[(][)] const noexcept" OR
       NOT web_http1_session_request_completion MATCHES
           "bufferCompletion[(][)] const [&] noexcept" OR
       NOT web_http1_stream_session MATCHES
           "std::optional<Http1SessionRequestCompletion> requestCompletion" OR
       NOT web_http1_stream_session MATCHES
           "committed->status[(][)]" OR
       NOT web_http1_stream_session MATCHES
           "applyReusableHttp1RequestBufferCompletion" OR
       NOT web_http1_stream_session MATCHES
           "Http1ConnectionDisposition::kClose" OR
       NOT web_http2_stream_session MATCHES
           "result[.]committedStatus[(][)]" OR
       NOT web_http2_stream_session MATCHES
           "result[.]failedAfterCommit[(][)]" OR
       web_http2_stream_session MATCHES
           "completed streamed response [(]status 200[)]")
        boundary_error("server runtime re-derived streamed access-log status"
            "H1 must consume one request completion carrying wire status, connection disposition, and buffer cleanup; H2 must log exact committed status before close/reset")
    endif()

    # Every method/target/header view of the request being completed borrows the
    # connection read buffer, and the response is not built nor the access log
    # recorded until after the body route returns. So a body runtime must hand its
    # pipelined suffix to the session rather than shifting the next request into
    # that buffer itself -- doing so logs request N under request N+1's path.
    file(READ "${WEB_CONTEXT_LAZY_BODY_ROUTE}" web_body_route_completion)
    # Shifting bytes to the front of the read buffer means rewriting the fill
    # level, so a mutable usedBytes here is the signature of that regression;
    # reading the buffer to locate the body (by value, const) stays fine.
    if(NOT web_body_route_completion MATCHES "takePipeline" OR
       NOT web_body_route_completion MATCHES "pipelineStash" OR
       web_body_route_completion MATCHES "std::size_t& usedBytes")
        boundary_error("HTTP/1 body route completion wrote the connection read buffer"
            "completeSuccessfulHttpBodyRoute must hand the pipelined suffix to a request-scoped stash; the session installs it after the response is written and the access log is recorded")
    endif()
    check_files_no_match(
        "HTTP/1 body runtime restored an eager read-buffer pipeline shift"
        "restorePipeline"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpLazyBufferedBody.h"
        "${WEB_CONTEXT_LAZY_BODY_ROUTE}"
        "${WEB_CONTEXT_STREAM_BODY_ROUTE}")

    if(NOT response_stream_status_test MATCHES
           "response_stream_dispatch_preserves_exact_committed_status" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_distinguishes_precommit_peer_abort" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_distinguishes_committed_peer_abort" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_end_commits_bodyless_status" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_preserves_committed_failure_status" OR
       NOT response_stream_status_test MATCHES
           "response_stream_dispatch_types_precommit_failure_response" OR
       NOT response_stream_status_test MATCHES
           "makeRecoveredFailure" OR
       NOT response_stream_lifecycle_test MATCHES
           "committedContextReleased" OR
       NOT response_stream_lifecycle_test MATCHES
           "rebindRejected" OR
       NOT response_stream_lifecycle_test MATCHES
           "abortedOpen[.]markAborted[(][)]" OR
       NOT http1_session_completion_test MATCHES
           "http1_session_request_completion_owns_wire_and_buffer_outcome" OR
       NOT http1_session_completion_test MATCHES
           "http1_request_buffer_completion_applies_exactly_one_cleanup" OR
       NOT response_stream_h2_runtime_test MATCHES
           "sansio_driver_h2_stream_trailers_emitted" OR
       NOT response_stream_h2_runtime_test MATCHES
           ":status=207;" OR
       NOT response_stream_h2_runtime_test MATCHES
           "accessObservation[.]status" OR
       NOT response_stream_http_package_consumer MATCHES
           "ResponseStreamCommitPlanner" OR
       NOT response_stream_web_package_consumer MATCHES
           "HasLegacyStreamedPredicate" OR
       NOT response_stream_web_package_consumer MATCHES
           "Http1SessionRequestCompletion" OR
       NOT response_stream_web_package_consumer MATCHES
           "Http1RequestBufferCompaction" OR
       NOT response_stream_web_package_consumer MATCHES
           "dispatchHttpWebSocketRoute")
        boundary_error("response-stream status propagation lacks regression coverage"
            "unit and installed-package checks must pin exact status and exclusive terminal alternatives")
    endif()
endif()
check_files_no_match("buffered response planning must derive body semantics from method and response"
    "${RULE_LOOSE_BUFFERED_RESPONSE_PLAN}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 buffered completion must not reconstruct status from HttpResponse"
    "${RULE_STALE_H2_BUFFERED_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/2 buffered submission must consume the prepared Web plan"
    "${RULE_STALE_H2_UNPREPARED_BUFFERED_HEAD}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/1 buffered completion must own its commit boundary and plan status"
    "${RULE_STALE_H1_BUFFERED_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
check_files_no_match("HTTP file writes must return results instead of error side channels"
    "${RULE_STALE_HTTP_FILE_WRITE_COMPLETION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpFileFallback.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpFileWrite.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h"
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpFileWrite.cpp")

set(WEB_HTTP_FILE_WRITE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpFileWrite.h")
set(WEB_HTTP_FILE_WRITE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpFileWrite.cpp")
set(WEB_HTTP_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h")
if(EXISTS "${WEB_HTTP_FILE_WRITE}" AND
   EXISTS "${WEB_HTTP_FILE_WRITE_SOURCE}" AND
   EXISTS "${WEB_HTTP_RESPONSE_WRITER}")
    file(READ "${WEB_HTTP_FILE_WRITE}" web_http_file_write)
    file(READ "${WEB_HTTP_FILE_WRITE_SOURCE}" web_http_file_write_source)
    file(READ "${WEB_HTTP_RESPONSE_WRITER}" web_http_response_writer)
    if(NOT web_http_file_write MATCHES
           "Task<std::error_code> writeHttpResponseFile" OR
       NOT web_http_file_write MATCHES
           "return writeFileFallback" OR
       NOT web_http_file_write_source MATCHES
           "Task<std::error_code> writeHttpResponseFile" OR
       NOT web_http_file_write_source MATCHES
           "sendfile|TransmitFile|writeFileFallback" OR
       NOT web_http_response_writer MATCHES
           "co_await writeHttpResponseFile" OR
       web_http_response_writer MATCHES
           "HttpFileZeroCopy|writeFileZeroCopy|zeroCopyResult|if constexpr[^{\r\n]*asio::ip::tcp::socket" OR
       web_http_file_write MATCHES
           "HttpFileZeroCopy(Result|Completed|Unavailable|Failed)|std::variant")
        boundary_error("HTTP response file strategy leaked into the generic writer"
            "one file-write driver must own native TCP selection, unsupported-platform fallback, and the final error result")
    endif()
else()
    boundary_error("HTTP response file write driver is incomplete"
        "HttpFileWrite.h, HttpFileWrite.cpp, and HttpResponseWriter.h are required")
endif()
check_files_no_match("database migration must return its owned report"
    "${RULE_STALE_DB_MIGRATION_REPORT_SIDE_CHANNEL}"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbMigration.cpp")
check_files_no_match("database query results must remain direct RAII values"
    "${RULE_STALE_DB_RESULT_MOUNT_PROXY}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbQueryResult.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbHandle.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbTransaction.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/db/DbInternal.h"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbTypes.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbHandle.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbRegistry.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/db/Db.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/db/PgDb.cpp")
check_files_no_match("locally staged Redis transactions must not expose fake DISCARD state"
    "${RULE_STALE_REDIS_TRANSACTION_DISCARD}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisTransaction.h"
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisTransaction.cpp")
set(WEB_REDIS_PIPELINE_API
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisPipeline.h")
set(WEB_REDIS_PIPELINE_IMPL
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPipeline.cpp")
set(WEB_REDIS_TRANSACTION_API
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisTransaction.h")
set(WEB_REDIS_TRANSACTION_IMPL
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisTransaction.cpp")
set(WEB_REDIS_LINEAR_API_TEST
    "${RUVIA_ROOT}/tests/unit_redis_api_surface.cpp")
set(WEB_REDIS_LINEAR_PACKAGE_TEST
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
if(EXISTS "${WEB_REDIS_PIPELINE_API}" AND
   EXISTS "${WEB_REDIS_PIPELINE_IMPL}" AND
   EXISTS "${WEB_REDIS_TRANSACTION_API}" AND
   EXISTS "${WEB_REDIS_TRANSACTION_IMPL}" AND
   EXISTS "${WEB_REDIS_LINEAR_API_TEST}" AND
   EXISTS "${WEB_REDIS_LINEAR_PACKAGE_TEST}")
    file(READ "${WEB_REDIS_PIPELINE_API}" web_redis_pipeline_api)
    file(READ "${WEB_REDIS_PIPELINE_IMPL}" web_redis_pipeline_impl)
    file(READ "${WEB_REDIS_TRANSACTION_API}" web_redis_transaction_api)
    file(READ "${WEB_REDIS_TRANSACTION_IMPL}" web_redis_transaction_impl)
    file(READ "${WEB_REDIS_LINEAR_API_TEST}" web_redis_linear_api_test)
    file(READ "${WEB_REDIS_LINEAR_PACKAGE_TEST}"
        web_redis_linear_package_test)
    if(NOT web_redis_pipeline_api MATCHES "exec[(][)][ \t]*&&" OR
       NOT web_redis_transaction_api MATCHES "exec[(][)][ \t]*&&" OR
       NOT web_redis_pipeline_api MATCHES
           "operator=[(]RedisPipeline&&[)][ \t]*=[ \t]*delete" OR
       NOT web_redis_transaction_api MATCHES
           "operator=[(]RedisTransaction&&[)][ \t]*=[ \t]*delete" OR
       NOT web_redis_pipeline_api MATCHES
           "std::pmr::vector<Command>[ \t]+commands" OR
       NOT web_redis_transaction_api MATCHES
           "std::pmr::vector<RedisPipeline::Command>[ \t]+commands" OR
       NOT web_redis_pipeline_api MATCHES
           "std::variant<Ready, Consumed>[ \t]+state_" OR
       web_redis_pipeline_api MATCHES
           "RedisPool[*][ \t]+pool_|memory_resource[*][ \t]+resource_" OR
       NOT web_redis_pipeline_impl MATCHES
           "emplace<Consumed>" OR
       NOT web_redis_transaction_impl MATCHES
           "pipeline_[.]consumePool[(][)]" OR
       NOT web_redis_linear_api_test MATCHES "HasLvalueRedisExec" OR
       NOT web_redis_linear_api_test MATCHES "HasRvalueRedisExec" OR
       NOT web_redis_linear_package_test MATCHES "HasLvalueRedisExec" OR
       NOT web_redis_linear_package_test MATCHES "HasRvalueRedisExec")
        boundary_error("Redis batch builders regained borrowed lazy execution"
            "pipeline and transaction exec must consume an rvalue, transfer command ownership into the coroutine frame, and invalidate the source")
    endif()
endif()
if(NOT pmr_db_query_result_api MATCHES
       "std::variant<NoRawResult, OwnedRawResult>[ \t]+rawResult_" OR
   pmr_db_query_result_api MATCHES
       "void[*][ \t]+rawResult_|releaseRawResult_" OR
   NOT pmr_db_result_access MATCHES "ownRawResult" OR
   pmr_db_result_access MATCHES "retainRawResult")
    boundary_error("database query result restored split backend ownership"
        "backend result and deleter must be one exclusive OwnedRawResult state")
endif()
check_files_no_match("Context final-response observation must not split or expose provisional storage"
    "${RULE_STALE_CONTEXT_RESPONSE_OBSERVATION_SPLIT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextStorage.cpp")
check_files_no_match("response-side cookie deletion must not read or return request state"
    "${RULE_STALE_DELETE_COOKIE_REQUEST_COUPLING}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")
check_files_no_match("route request metadata must only be read through ContextRequest"
    "${RULE_STALE_FREE_ROUTE_REQUEST_ACCESS}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequestFacade.cpp")
check_files_no_match("request-local data must not expose a misleading whole-request clone"
    "${RULE_STALE_REQUEST_CLONE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("form lookups must share one zero-allocation value facade"
    "${RULE_STALE_FORM_PATH_VALUE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h")
check_files_no_match("named form lookup must return only the Value facade"
    "${RULE_STALE_FORM_ENTRY_LOOKUP}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h")
check_files_no_match("form value lookup must use one get operation"
    "${RULE_STALE_FORM_AT_LOOKUP}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h")
check_files_no_match("SSE public operations must follow the typed facade naming"
    "${RULE_STALE_SSE_PUBLIC_NAMING}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Streaming.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/HttpRuntimeFacades.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/Streaming.cpp")
check_files_no_match("ContextRequest must not expose the borrowed protocol object"
    "${RULE_STALE_CONTEXT_REQUEST_RAW_ESCAPE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequestFacade.cpp")
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/SecurityHeaders.h"
    security_headers_public_contract)
file(READ "${RUVIA_ROOT}/ruvia-web/src/http/SecurityHeaders.cpp"
    security_headers_wire_contract)
file(READ "${RUVIA_ROOT}/tests/unit_security_headers.cpp"
    security_headers_unit_contract)
file(READ "${RUVIA_ROOT}/examples/api_surface.cpp"
    security_headers_api_surface_contract)
file(READ "${RUVIA_ROOT}/tests/package-consumer/web.cpp"
    security_headers_package_contract)
if(security_headers_public_contract MATCHES
       "bool[ \t]+xssProtection" OR
   NOT security_headers_public_contract MATCHES
       "enum class LegacyXssFilterPolicy" OR
   NOT security_headers_public_contract MATCHES
       "LegacyXssFilterPolicy[ \t]+legacyXssFilter" OR
   NOT security_headers_public_contract MATCHES
       "LegacyXssFilterPolicy::kDisable" OR
   NOT security_headers_wire_contract MATCHES
       "case LegacyXssFilterPolicy::kDisable" OR
   NOT security_headers_wire_contract MATCHES
       "setHeader[(]\"X-XSS-Protection\",[ \t]*\"0\"" OR
   NOT security_headers_wire_contract MATCHES
       "case LegacyXssFilterPolicy::kOmitHeader" OR
   NOT security_headers_unit_contract MATCHES
       "security_headers_legacy_xss_filter_policy_is_explicit" OR
   NOT security_headers_api_surface_contract MATCHES
       "HasMisleadingXssProtectionOption" OR
   NOT security_headers_package_contract MATCHES
       "HasMisleadingXssProtectionOption")
    boundary_error("legacy browser XSS filter policy regained inverted boolean semantics"
        "the public typed policy must default to explicit filter disablement, emit X-XSS-Protection: 0, offer only header omission, and remain pinned by source and installed API probes")
endif()
check_files_no_match("Context must expose explicit typed capabilities, not an arbitrary value bag"
    "ContextValueStore|ContextKey|detail/ContextValues[.]h|valuesIf[(]|void[ \t]+set[(]std::string_view|[*][ \t]+get[(]std::string_view"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextStorage.cpp")
if(EXISTS "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/ContextValues.h")
    boundary_error("Context regained an arbitrary request-local value bag"
        "request state must use an explicit typed Context capability with direct storage and lifecycle")
endif()
check_files_no_match("Context layout mutation must not return the assigned value"
    "${RULE_STALE_CONTEXT_LAYOUT_SETTER_RESULT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")
check_files_no_match("HttpResponse body mutation must use one safe public operation"
    "${RULE_STALE_HTTP_RESPONSE_BODY_SETTERS}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpResponse.h"
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp")
check_files_no_match("Context dynamic bodies must be owned without implicit lvalue consumption"
    "${RULE_STALE_CONTEXT_DYNAMIC_BODY_BORROW}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/Context.inl")
check_files_no_match("response headers must remain an owned read-only facade"
    "${RULE_STALE_RESPONSE_HEADERS_STANDALONE_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpResponse.h"
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponseHeadersStorage.cpp")
check_files_no_match("Context cookies must use the response mutation path"
    "${RULE_STALE_CONTEXT_COOKIE_GENERATOR}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")
check_files_no_match("request and session storage must not bypass SSO for obsolete response borrowing"
    "${RULE_STALE_CONTEXT_SSO_BYPASS}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/SessionInternal.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("Context response construction must not restore request-local renderer state"
    "${RULE_STALE_CONTEXT_RENDER_PIPELINE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")
check_files_no_match("validated request models must have an internal-only write path"
    "${RULE_STALE_VALIDATED_MODEL_PUBLIC_WRITE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextRequestModel.inl")
check_files_no_match("request arena objects must not have write-only Context owner pointers"
    "${RULE_STALE_CONTEXT_ARENA_STORAGE_POINTERS}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("request route metadata must not restore synthetic matched-route entries"
    "${RULE_STALE_MATCHED_ROUTES_FACADE}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextInternal.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequestFacade.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp")
check_files_no_match("ContextRequest must not synthesize an absolute URL from transport metadata"
    "${RULE_STALE_CONTEXT_REQUEST_SYNTHETIC_URL}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequestFacade.cpp")
check_files_no_match("query collections must encode absence with an empty span only"
    "${RULE_STALE_OPTIONAL_QUERY_VALUES}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequestFacade.cpp")
check_files_no_match("form parsing policies must remain strongly typed and explicit"
    "${RULE_STALE_PARSE_BODY_BOOLEAN_POLICY}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
check_files_no_match("request blobs must use the shared contentType vocabulary"
    "${RULE_STALE_REQUEST_BLOB_TYPE_NAME}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequestFacade.cpp")

set(DB_MIGRATION_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/db/DbMigration.cpp")
set(DB_MIGRATION_TEST
    "${RUVIA_ROOT}/tests/unit_db_api_surface.cpp")
if(EXISTS "${DB_MIGRATION_SOURCE}" AND EXISTS "${DB_MIGRATION_TEST}")
    file(READ "${DB_MIGRATION_SOURCE}" db_migration_source)
    file(READ "${DB_MIGRATION_TEST}" db_migration_test)
    if(NOT db_migration_source MATCHES
           "Task<DbMigrationReport>[ \t]+run" OR
       NOT db_migration_source MATCHES
           "TaskCompletionResult<DbMigrationReport>" OR
       NOT db_migration_source MATCHES
           "completion[.]failure[(][)]" OR
       NOT db_migration_source MATCHES
           "completion[.]success[(][)]" OR
       db_migration_source MATCHES
           "completion[.](exception|value)" OR
       NOT db_migration_source MATCHES "return std::move[(][*]report[)]" OR
       NOT db_migration_test MATCHES
           "db_migrator_validates_before_opening_connection")
        boundary_error("database migration report ownership is incomplete"
            "the runner must return its report and feature-on tests must cover pre-I/O validation")
    endif()
endif()

set(HTTP_BUFFERED_RESPONSE_WRITE_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h")
set(HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp")
set(HTTP2_BUFFERED_RESPONSE_HEAD_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
set(HTTP2_BUFFERED_RESPONSE_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeaders.h")
set(HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(HTTP2_BUFFERED_RESPONSE_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(WEB_HTTP2_BUFFERED_RESPONSE_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2BufferedResponseWrite.h")
set(WEB_HTTP2_BUFFERED_RESPONSE_WRITER_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/server/Http2BufferedResponseWrite.cpp")
set(WEB_HTTP2_BUFFERED_RESPONSE_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_HTTP1_BUFFERED_RESPONSE_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1BufferedResponseWrite.h")
set(WEB_HTTP1_BUFFERED_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h")
set(WEB_HTTP1_BUFFERED_RESPONSE_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_BUFFERED_RESPONSE_PREPARATION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h")
set(BUFFERED_RESPONSE_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_policy.cpp")
set(BUFFERED_RESPONSE_H1_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
set(BUFFERED_RESPONSE_H1_RESULT_TEST
    "${RUVIA_ROOT}/tests/unit_http1_buffered_response_write.cpp")
set(BUFFERED_RESPONSE_H2_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_http2_response_headers.cpp")
set(BUFFERED_RESPONSE_H2_RESULT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_buffered_response_write.cpp")
set(BUFFERED_RESPONSE_H2_RUNTIME_TEST
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
set(BUFFERED_RESPONSE_H2_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(BUFFERED_RESPONSE_PACKAGE_VERIFY
    "${RUVIA_ROOT}/tests/verify_package_consumers.cmake.in")
foreach(buffered_response_status_contract IN ITEMS
        "${HTTP_BUFFERED_RESPONSE_WRITE_PLAN}"
        "${HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE}"
        "${HTTP2_BUFFERED_RESPONSE_HEAD_PLAN}"
        "${HTTP2_BUFFERED_RESPONSE_HEADERS}"
        "${HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER}"
        "${HTTP2_BUFFERED_RESPONSE_CONNECTION_SOURCE}"
        "${WEB_HTTP2_BUFFERED_RESPONSE_RESULT}"
        "${WEB_HTTP2_BUFFERED_RESPONSE_WRITER_SOURCE}"
        "${WEB_HTTP2_BUFFERED_RESPONSE_SESSION}"
        "${WEB_HTTP1_BUFFERED_RESPONSE_RESULT}"
        "${WEB_HTTP1_BUFFERED_RESPONSE_WRITER}"
        "${WEB_HTTP1_BUFFERED_RESPONSE_SESSION}"
        "${WEB_BUFFERED_RESPONSE_PREPARATION}"
        "${BUFFERED_RESPONSE_PLAN_TEST}"
        "${BUFFERED_RESPONSE_H1_TEST}"
        "${BUFFERED_RESPONSE_H1_RESULT_TEST}"
        "${BUFFERED_RESPONSE_H2_PLAN_TEST}"
        "${BUFFERED_RESPONSE_H2_RESULT_TEST}"
        "${BUFFERED_RESPONSE_H2_RUNTIME_TEST}"
        "${BUFFERED_RESPONSE_H2_CONNECTION_TEST}"
        "${BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER}"
        "${BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER}"
        "${BUFFERED_RESPONSE_PACKAGE_VERIFY}")
    if(NOT EXISTS "${buffered_response_status_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${buffered_response_status_contract}")
        boundary_error("buffered response status contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_BUFFERED_RESPONSE_WRITE_PLAN}" AND
   EXISTS "${HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_HEAD_PLAN}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_HEADERS}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER}" AND
   EXISTS "${HTTP2_BUFFERED_RESPONSE_CONNECTION_SOURCE}" AND
   EXISTS "${WEB_HTTP2_BUFFERED_RESPONSE_RESULT}" AND
   EXISTS "${WEB_HTTP2_BUFFERED_RESPONSE_WRITER_SOURCE}" AND
   EXISTS "${WEB_HTTP2_BUFFERED_RESPONSE_SESSION}" AND
   EXISTS "${WEB_HTTP1_BUFFERED_RESPONSE_RESULT}" AND
   EXISTS "${WEB_HTTP1_BUFFERED_RESPONSE_WRITER}" AND
   EXISTS "${WEB_HTTP1_BUFFERED_RESPONSE_SESSION}" AND
   EXISTS "${WEB_BUFFERED_RESPONSE_PREPARATION}" AND
   EXISTS "${BUFFERED_RESPONSE_PLAN_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H1_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H1_RESULT_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_PLAN_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_RESULT_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_RUNTIME_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_H2_CONNECTION_TEST}" AND
   EXISTS "${BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER}" AND
   EXISTS "${BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER}" AND
   EXISTS "${BUFFERED_RESPONSE_PACKAGE_VERIFY}")
    file(READ "${HTTP_BUFFERED_RESPONSE_WRITE_PLAN}"
        buffered_response_write_plan)
    file(READ "${HTTP1_BUFFERED_RESPONSE_HEAD_SOURCE}"
        buffered_response_h1_source)
    file(READ "${HTTP2_BUFFERED_RESPONSE_HEAD_PLAN}"
        buffered_response_h2_head_plan)
    file(READ "${HTTP2_BUFFERED_RESPONSE_HEADERS}"
        buffered_response_h2_headers)
    file(READ "${HTTP2_BUFFERED_RESPONSE_CONNECTION_HEADER}"
        buffered_response_h2_connection_header)
    read_http2_connection_implementation(
        buffered_response_h2_connection_source)
    file(READ "${WEB_HTTP2_BUFFERED_RESPONSE_RESULT}"
        buffered_response_h2_result)
    file(READ "${WEB_HTTP2_BUFFERED_RESPONSE_WRITER_SOURCE}"
        buffered_response_h2_writer_source)
    file(READ "${WEB_HTTP2_BUFFERED_RESPONSE_SESSION}"
        buffered_response_h2_session)
    file(READ "${WEB_HTTP1_BUFFERED_RESPONSE_RESULT}"
        buffered_response_h1_result)
    file(READ "${WEB_HTTP1_BUFFERED_RESPONSE_WRITER}"
        buffered_response_h1_writer)
    file(READ "${WEB_HTTP1_BUFFERED_RESPONSE_SESSION}"
        buffered_response_h1_session)
    file(READ "${WEB_BUFFERED_RESPONSE_PREPARATION}"
        buffered_response_preparation)
    file(READ "${BUFFERED_RESPONSE_PLAN_TEST}"
        buffered_response_plan_test)
    file(READ "${BUFFERED_RESPONSE_H1_TEST}"
        buffered_response_h1_test)
    file(READ "${BUFFERED_RESPONSE_H1_RESULT_TEST}"
        buffered_response_h1_result_test)
    file(READ "${BUFFERED_RESPONSE_H2_PLAN_TEST}"
        buffered_response_h2_plan_test)
    file(READ "${BUFFERED_RESPONSE_H2_RESULT_TEST}"
        buffered_response_h2_result_test)
    file(READ "${BUFFERED_RESPONSE_H2_RUNTIME_TEST}"
        buffered_response_h2_runtime_test)
    file(READ "${BUFFERED_RESPONSE_H2_CONNECTION_TEST}"
        buffered_response_h2_connection_test)
    file(READ "${BUFFERED_RESPONSE_HTTP_PACKAGE_CONSUMER}"
        buffered_response_http_package_consumer)
    file(READ "${BUFFERED_RESPONSE_WEB_PACKAGE_CONSUMER}"
        buffered_response_web_package_consumer)
    file(READ "${BUFFERED_RESPONSE_PACKAGE_VERIFY}"
        buffered_response_package_verify)

    if(NOT buffered_response_preparation MATCHES
           "HttpBufferedResponseWritePlan prepareBufferedHttpResponse" OR
       buffered_response_preparation MATCHES
           "class HttpBufferedResponsePreparation" OR
       NOT buffered_response_write_plan MATCHES
           "std::uint16_t responseStatus[(][)] const noexcept" OR
       NOT buffered_response_write_plan MATCHES
           "return bodyPlan_[.]responseStatus[(][)]" OR
       NOT buffered_response_write_plan MATCHES
           "HttpKnownMethod requestMethod" OR
       NOT buffered_response_write_plan MATCHES
           "HttpKnownMethod requestMethod[(][)] const noexcept" OR
       NOT buffered_response_write_plan MATCHES
           "matchesResponse" OR
       NOT buffered_response_write_plan MATCHES
           "bufferedRepresentationLength" OR
       NOT buffered_response_h1_source MATCHES
           "response[.]status[(][)] != bodyPlan[.]responseStatus[(][)]" OR
       NOT buffered_response_h1_source MATCHES
           "response plan representation does not match response" OR
       NOT buffered_response_h1_source MATCHES
           "bodyPlan[.]bufferedRepresentationLength[(]response[)]" OR
       NOT buffered_response_h1_source MATCHES
           "httpReasonPhrase[(]responseStatus[)]" OR
       NOT buffered_response_h2_head_plan MATCHES
           "kResponseStatusMismatch" OR
       NOT buffered_response_h2_head_plan MATCHES
           "writePlan[.]responseStatus[(][)] != response[.]status[(][)]" OR
       NOT buffered_response_h2_head_plan MATCHES
           "kResponseRepresentationMismatch" OR
       NOT buffered_response_h2_headers MATCHES
           "plan[.]bodyPlan[(][)][.]responseStatus[(][)]")
        boundary_error("buffered protocol plan lost exact response status ownership"
            "body/write/head plans must bind one status and reject response-plan mismatch before wire mutation")
    endif()

    if(NOT buffered_response_h2_connection_header MATCHES
           "kResponsePlanMismatch" OR
       NOT buffered_response_h2_connection_header MATCHES
           "HttpBufferedResponseWritePlan writePlan" OR
       NOT buffered_response_h2_connection_source MATCHES
           "writePlan[.]requestMethod[(][)] != stream->requestKnownMethod[(][)]" OR
       NOT buffered_response_h2_connection_source MATCHES
           "!writePlan[.]matchesResponse[(]response[)]" OR
       buffered_response_h2_connection_source MATCHES
           "auto[ \t]+writePlan[ \t]*=[ \t\r\n]*httpBufferedResponseWritePlan" OR
       NOT buffered_response_h2_session MATCHES
           "const auto writePlan = prepareBufferedHttpResponse" OR
       buffered_response_h2_session MATCHES
           "responsePreparation|[.]writePlan[(][)]" OR
       NOT buffered_response_h2_session MATCHES
           "bufferedResponseWriter[.]write[\r\n \t]*[(][\r\n \t]*streamId[\r\n \t]*,[\r\n \t]*response[\r\n \t]*,[\r\n \t]*writePlan")
        boundary_error("HTTP/2 buffered submission stopped consuming the prepared response plan"
            "Web preparation must flow into core submission and method/status/representation drift must fail transactionally")
    endif()

    if(NOT buffered_response_h2_result MATCHES
           "class Http2BufferedResponseWriteResult final" OR
       NOT buffered_response_h2_result MATCHES
           "class Http2BufferedResponseWriteCompleted final" OR
       NOT buffered_response_h2_result MATCHES
           "class Http2BufferedResponseWritePeerAbortedBeforeCommit final" OR
       NOT buffered_response_h2_result MATCHES
           "class Http2BufferedResponseWritePeerAbortedAfterCommit final" OR
       NOT buffered_response_h2_result MATCHES
           "class Http2BufferedResponseWriteFailedBeforeCommit final" OR
       NOT buffered_response_h2_result MATCHES
           "class Http2BufferedResponseWriteFailedAfterCommit final" OR
       NOT buffered_response_h2_result MATCHES
           "using Value = std::variant" OR
       NOT buffered_response_h2_result MATCHES
           "completed[(][)] const [&] noexcept" OR
       NOT buffered_response_h2_result MATCHES
           "peerAbortedBeforeCommit[(][)] const && = delete" OR
       NOT buffered_response_h2_result MATCHES
           "peerAbortedAfterCommit[(][)] const && = delete" OR
       NOT buffered_response_h2_result MATCHES
           "failedBeforeCommit[(][)] const && = delete" OR
       NOT buffered_response_h2_result MATCHES
           "failedAfterCommit[(][)] const && = delete" OR
       NOT buffered_response_h2_result MATCHES
           "committedStatus[(][)] const noexcept" OR
       NOT buffered_response_h2_result MATCHES
           "is_trivially_copyable_v<Http2BufferedResponseWriteResult>" OR
       NOT buffered_response_h2_result MATCHES
           "sizeof[(]Http2BufferedResponseWriteResult[)] <= 4" OR
       NOT buffered_response_h2_result MATCHES
           "class Http2BufferedResponseWriter final" OR
       NOT buffered_response_h2_result MATCHES
           "Task<Http2BufferedResponseWriteResult> write" OR
       NOT buffered_response_h2_writer_source MATCHES
           "Task<Http2BufferedResponseWriteResult>" OR
       NOT buffered_response_h2_writer_source MATCHES
           "const auto committedStatus = submittedHead[-][>]responseStatus[(][)]" OR
       NOT buffered_response_h2_writer_source MATCHES
           "Http2BufferedResponseWriteResult::makeCompleted" OR
       NOT buffered_response_h2_writer_source MATCHES
           "Http2BufferedResponseWriteResult::makePeerAbortedBeforeCommit" OR
       NOT buffered_response_h2_writer_source MATCHES
           "Http2BufferedResponseWriteResult::makePeerAbortedAfterCommit" OR
       NOT buffered_response_h2_writer_source MATCHES
           "Http2BufferedResponseWriteResult::makeFailedBeforeCommit" OR
       NOT buffered_response_h2_writer_source MATCHES
           "Http2BufferedResponseWriteResult::makeFailedAfterCommit" OR
       NOT buffered_response_h2_writer_source MATCHES
           "openResponseFileInput" OR
       NOT buffered_response_h2_session MATCHES
           "prepareBufferedHttpResponse" OR
       NOT buffered_response_h2_session MATCHES
           "Http2BufferedResponseWriter bufferedResponseWriter" OR
       NOT buffered_response_h2_session MATCHES
           "bufferedResponseWriter[.]write" OR
       NOT buffered_response_h2_session MATCHES
           "result[.]committedStatus[(][)]" OR
       buffered_response_h2_session MATCHES
           "openResponseFileInput|Http2BufferedDataSubmitResult|auto submitResponse")
        boundary_error("HTTP/2 buffered completion lost its typed terminal result chain"
            "the writer must preserve peer/local and pre/post-commit outcomes while the session consumes its committed status")
    endif()

    if(NOT buffered_response_h1_result MATCHES
           "class Http1BufferedResponseWriteResult final" OR
       NOT buffered_response_h1_result MATCHES
           "class Http1BufferedResponseWriteCompleted final" OR
       NOT buffered_response_h1_result MATCHES
           "class Http1BufferedResponseWriteFailedBeforeCommit final" OR
       NOT buffered_response_h1_result MATCHES
           "class Http1BufferedResponseWriteFailedAfterCommit final" OR
       NOT buffered_response_h1_result MATCHES
           "using Value = std::variant" OR
       NOT buffered_response_h1_result MATCHES
           "completed[(][)] const [&] noexcept" OR
       NOT buffered_response_h1_result MATCHES
           "failedBeforeCommit[(][)] const && = delete" OR
       buffered_response_h1_result MATCHES
           "enum class Http1BufferedResponseWriteOutcome|outcome[(][)] const noexcept|bool completed[(][)] const noexcept" OR
       NOT buffered_response_h1_result MATCHES
           "std::optional<std::uint16_t>[ \t\r\n]+committedStatus[(][)] const noexcept" OR
       NOT buffered_response_h1_result MATCHES
           "is_trivially_copyable_v<Http1BufferedResponseWriteResult>" OR
       NOT buffered_response_h1_result MATCHES
           "sizeof[(]Http1BufferedResponseWriteResult[)] <= 4" OR
       NOT buffered_response_h1_result MATCHES
           "plan[.]responseStatus[(][)]" OR
       NOT buffered_response_h1_writer MATCHES
           "Task<Http1BufferedResponseWriteResult> writeResponseWithScratch" OR
       NOT buffered_response_h1_writer MATCHES
           "asyncAsio<std::size_t>" OR
       NOT buffered_response_h1_writer MATCHES
           "classifyHttp1BufferedResponseWrite" OR
       NOT buffered_response_h1_session MATCHES
           "writeResult[.]completed[(][)]" OR
       NOT buffered_response_h1_session MATCHES
           "writeResult[.]committedStatus[(][)]" OR
       buffered_response_h1_result MATCHES
           "error[(][)]" OR
       buffered_response_h1_session MATCHES
           "response[.]status[(][)][ \t\r\n]*,[ \t\r\n]*requestStart|writeResult[.]failedBeforeCommit[(]|writeResult[.]failedAfterCommit[(]" OR
       NOT buffered_response_web_package_consumer MATCHES
           "HasLegacyHttp1BufferedWriteOutcome" OR
       NOT buffered_response_web_package_consumer MATCHES
           "ExposesAnyRvalueHttp1BufferedWriteAlternative")
        boundary_error("HTTP/1 buffered completion restored a loose write/error/status path"
            "the writer must consume transport errors, classify the complete-head byte boundary, and expose one three-state outcome with derived committed status")
    endif()

    if(NOT buffered_response_plan_test MATCHES
           "AcceptsLooseBufferedResponseBodyPlan" OR
       NOT buffered_response_h1_test MATCHES
           "http1_response_head_rejects_status_plan_mismatch" OR
       NOT buffered_response_h1_test MATCHES
           "http1_response_head_rejects_representation_plan_mismatch" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_write_partial_head_has_no_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_write_body_failure_keeps_committed_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_scatter_write_keeps_committed_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_write_cannot_complete_without_a_full_head" OR
       NOT buffered_response_h1_result_test MATCHES
           "http_response_file_writer_hides_native_capability" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_file_fallback_completion_owns_status" OR
       NOT buffered_response_h1_result_test MATCHES
           "http1_buffered_file_open_failure_preserves_committed_status" OR
       NOT buffered_response_h2_plan_test MATCHES
           "http2_response_head_rejects_status_plan_mismatch" OR
       NOT buffered_response_h2_plan_test MATCHES
           "http2_response_head_rejects_representation_plan_mismatch" OR
       NOT buffered_response_h2_result_test MATCHES
           "http2_buffered_response_write_result_preserves_terminal_cause" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "sansio_driver_h2_buffered_access_uses_only_committed_plan_status" OR
       NOT buffered_response_h2_connection_test MATCHES
           "http2_connection_buffered_response_requires_matching_prepared_plan" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "sansio_driver_h2_buffered_peer_abort_before_commit_has_no_status" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "accessObservation[.]calls, std::size_t[{]1[}]" OR
       NOT buffered_response_h2_runtime_test MATCHES
           "accessObservation[.]calls, std::size_t[{]0[}]" OR
       NOT buffered_response_http_package_consumer MATCHES
           "AcceptsLooseBufferedResponseBodyPlan" OR
       NOT buffered_response_http_package_consumer MATCHES
           "AcceptsUnpreparedBufferedResponseHead" OR
       NOT buffered_response_web_package_consumer MATCHES
           "Http2BufferedResponseWriteResult" OR
       NOT buffered_response_web_package_consumer MATCHES
           "Http1BufferedResponseWriteResult" OR
       NOT buffered_response_web_package_consumer MATCHES
           "writeHttpResponseFile" OR
       NOT buffered_response_web_package_consumer MATCHES
           "prepareBufferedHttpResponse" OR
       NOT buffered_response_web_package_consumer MATCHES
           "HttpBufferedResponseWritePlan" OR
       NOT buffered_response_package_verify MATCHES
           "installed buffered response status ownership" OR
       NOT buffered_response_package_verify MATCHES
           "installed HTTP/2 prepared buffered response plan ownership" OR
       NOT buffered_response_package_verify MATCHES
           "installed HTTP/1 buffered response completion ownership")
        boundary_error("buffered response completion ownership lacks regression coverage"
            "unit, integration, source-boundary, and installed-package checks must pin typed terminal outcomes and committed status")
    endif()
endif()
check_files_no_match("HTTP/2 must not restore split discard state or deprecated priority semantics"
    "${RULE_STALE_H2_FIELD_BLOCK_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2HeaderContinuation.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 CONNECT must use exclusive tunnel alternatives"
    "${RULE_STALE_H2_CONNECT_STATE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2TunnelState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamRequestState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/2 must not restore split preface APIs or wire/accounting knobs"
    "${RULE_STALE_H2_PREFACE_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 client request heads must allocate and submit atomically"
    "${RULE_STALE_H2_CLIENT_STREAM_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 GOAWAY lifecycle must remain typed and core-owned"
    "${RULE_STALE_H2_GOAWAY_API}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/SansIoDriver.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 events must remain optional and discriminated"
    "${RULE_STALE_H2_EVENT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Event.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
check_files_no_match("HTTP/2 feed must remain a direct all-or-nothing ownership enum"
    "${RULE_STALE_H2_FEED_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 request-head submission must remain discriminated"
    "${RULE_STALE_H2_REQUEST_HEAD_SUBMIT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("HTTP/2 response-head submission must remain discriminated"
    "${RULE_STALE_H2_RESPONSE_HEAD_SUBMIT_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp"
    "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
check_files_no_match("HTTP/2 peer-setting application must remain discriminated"
    "${RULE_STALE_H2_PEER_SETTING_APPLY_TUPLE}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2PeerSettings.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("HTTP/2 DATA must keep connection-first receive-window accounting"
    "${RULE_STALE_H2_DATA_FLOW_ACCOUNTING}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2FlowControl.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_flow_control.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("205 Reset Content must not regain a sendable body"
    "${RULE_STALE_205_RESPONSE_BODY}"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadPolicy.h"
    "${RUVIA_ROOT}/tests/unit_response_head_policy.cpp")

set(HTTP_RESPONSE_HEAD_POLICY
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadPolicy.h")
if(EXISTS "${HTTP_RESPONSE_HEAD_POLICY}")
    file(READ "${HTTP_RESPONSE_HEAD_POLICY}" http_response_head_policy)
    if(NOT http_response_head_policy MATCHES "statusCode == 205" OR
       NOT http_response_head_policy MATCHES "ResponseWritePolicy::makeZeroLength")
        boundary_error("205 Reset Content bypasses the shared response policy"
            "205 must suppress content while retaining writer-owned zero-length framing")
    endif()
endif()

set(HTTP1_RESPONSE_HEAD_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ResponseHeadPlan.h")
set(HTTP_RESPONSE_HEAD_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHead.h")
set(HTTP_RESPONSE_STREAM_HEAD
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h")
set(HTTP1_SERVER_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(WEB_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseWriter.h")
set(WEB_RESPONSE_STREAM_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
set(WEB_RESPONSE_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
foreach(http1_response_head_contract_file IN ITEMS
        "${HTTP1_RESPONSE_HEAD_PLAN}"
        "${HTTP_RESPONSE_HEAD_HEADER}"
        "${HTTP_RESPONSE_STREAM_HEAD}"
        "${HTTP1_SERVER_SEMANTICS}"
        "${WEB_RESPONSE_WRITER}"
        "${WEB_RESPONSE_STREAM_SINK}"
        "${WEB_RESPONSE_SESSION}")
    if(NOT EXISTS "${http1_response_head_contract_file}")
        file(RELATIVE_PATH relative
            "${RUVIA_ROOT}" "${http1_response_head_contract_file}")
        boundary_error("typed HTTP/1 response-head call chain is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP1_RESPONSE_HEAD_PLAN}" AND
   EXISTS "${HTTP_RESPONSE_HEAD_HEADER}" AND
   EXISTS "${HTTP_RESPONSE_STREAM_HEAD}" AND
   EXISTS "${HTTP1_SERVER_SEMANTICS}" AND
   EXISTS "${WEB_RESPONSE_WRITER}" AND
   EXISTS "${WEB_RESPONSE_STREAM_SINK}" AND
   EXISTS "${WEB_RESPONSE_SESSION}")
    file(READ "${HTTP1_RESPONSE_HEAD_PLAN}" http1_response_head_plan)
    file(READ "${HTTP_RESPONSE_HEAD_HEADER}" http_response_head_header)
    file(READ "${HTTP_RESPONSE_STREAM_HEAD}" http_response_stream_head)
    file(READ "${HTTP1_SERVER_SEMANTICS}" http1_response_head_semantics)
    file(READ "${WEB_RESPONSE_WRITER}" web_response_writer)
    file(READ "${WEB_RESPONSE_STREAM_SINK}" web_response_stream_sink)
    file(READ "${WEB_RESPONSE_SESSION}" web_response_session)
    set(http1_response_head_missing)
    foreach(http1_head_probe IN ITEMS
            "class Http1BufferedResponseHead final"
            "class Http1ChunkedResponseStreamHead final"
            "class Http1CloseDelimitedResponseStreamHead final"
            "using Framing = std::variant"
            "std::get_if<Http1BufferedResponseHead>"
            "std::get_if<Http1ChunkedResponseStreamHead>"
            "std::get_if<Http1CloseDelimitedResponseStreamHead>"
            "HttpResponseBodyPlan bodyPlan_"
            "std::uint64_t contentLength_"
            "HttpProtocolVersion protocolVersion_"
            "class Http1BufferedResponsePlan final"
            "std::uint16_t responseStatus[(][)] const noexcept"
            "std::uint64_t contentLength[(][)] const noexcept"
            "bool sendBody[(][)] const noexcept"
            "is_trivially_copyable_v<Http1BufferedResponsePlan>"
            "sizeof[(]Http1BufferedResponsePlan[)] == sizeof[(]Http1ResponseHeadPlan[)]"
            "http1BufferedResponsePlan")
        if(NOT http1_response_head_plan MATCHES "${http1_head_probe}")
            list(APPEND http1_response_head_missing
                "plan:${http1_head_probe}")
        endif()
    endforeach()
    if(NOT http_response_head_header MATCHES
           "const Http1ResponseHeadPlan& plan")
        list(APPEND http1_response_head_missing "head-signature")
    endif()
    foreach(http1_stream_probe IN ITEMS
            "writerOwnsHttp1Chunked"
            "response[.]header[(]\"Transfer-Encoding\", std::nullopt[)]"
            "response[.]header[(]\"Content-Length\", std::nullopt[)]")
        if(NOT http_response_stream_head MATCHES "${http1_stream_probe}")
            list(APPEND http1_response_head_missing
                "stream:${http1_stream_probe}")
        endif()
    endforeach()
    foreach(http1_semantics_probe IN ITEMS
            "Http1ResponseHeadPlan responseHeadPlan_"
            "http1ChunkedResponseStreamHeadPlan"
            "http1CloseDelimitedResponseStreamHeadPlan")
        if(NOT http1_response_head_semantics MATCHES "${http1_semantics_probe}")
            list(APPEND http1_response_head_missing
                "semantics:${http1_semantics_probe}")
        endif()
    endforeach()
    if(NOT web_response_writer MATCHES
           "const Http1BufferedResponsePlan& responsePlan" OR
       NOT web_response_writer MATCHES "responsePlan[.]headPlan[(][)]" OR
       web_response_writer MATCHES "http1BufferedResponseHeadPlan")
        list(APPEND http1_response_head_missing "web-buffered-driver")
    endif()
    if(NOT web_response_session MATCHES "http1BufferedResponsePlan" OR
       NOT web_response_session MATCHES
           "const auto writePlan = prepareBufferedHttpResponse" OR
       web_response_session MATCHES
           "responsePreparation|[.]writePlan[(][)]" OR
       NOT web_response_session MATCHES
           "http1BufferedResponsePlan[\r\n \t]*[(][\r\n \t]*writePlan" OR
       NOT web_response_session MATCHES "connectionPlan")
        list(APPEND http1_response_head_missing
            "web-buffered-composition")
    endif()
    if(NOT web_response_stream_sink MATCHES
           "streamHead[.]responseHeadPlan[(][)]")
        list(APPEND http1_response_head_missing "web-stream-driver")
    endif()
    if(http1_response_head_missing)
        string(JOIN ", " http1_response_head_missing_text
            ${http1_response_head_missing})
        boundary_error("HTTP/1 response-head framing escaped its exclusive plan"
            "buffered, chunked-stream, and close-delimited-stream heads must remain exclusive; the prepared HTTP/1 plan owns canonical framing and Web may only drive it; missing ${http1_response_head_missing_text}")
    endif()
endif()

set(HTTP_RESPONSE_HEAD_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp")
if(EXISTS "${HTTP_RESPONSE_HEAD_SOURCE}")
    file(READ "${HTTP_RESPONSE_HEAD_SOURCE}" http1_typed_response_head_source)
    if(NOT http1_typed_response_head_source MATCHES
           "plan[.]chunkedStream[(][)]" OR
       NOT http1_typed_response_head_source MATCHES
           "plan[.]closeDelimitedStream[(][)]" OR
       NOT http1_typed_response_head_source MATCHES
           "plan[.]protocolVersion[(][)]" OR
       NOT http1_typed_response_head_source MATCHES
           "buffered->contentLength[(][)]" OR
       NOT http1_typed_response_head_source MATCHES "HTTP/1[.]0" OR
       NOT http1_typed_response_head_source MATCHES "HTTP/1[.]1" OR
       NOT http1_typed_response_head_source MATCHES
           "kChunkedTransferEncodingHeader" OR
       NOT http1_typed_response_head_source MATCHES
           "knownBit == kResponseHeaderTransferEncoding" OR
       NOT http1_typed_response_head_source MATCHES
           "knownBit == kResponseHeaderContentLength")
        boundary_error("HTTP/1 response-head emitter bypasses its typed framing plan"
            "status-line version, canonical buffered length, chunked ownership, and close-delimited TE/CL filtering must derive from Http1ResponseHeadPlan")
    endif()
endif()
set(HTTP2_RESPONSE_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeaders.h")
set(HTTP2_RESPONSE_HEAD_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
set(HTTP2_RESPONSE_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
check_files_no_match("HTTP/2 response-head encoding must not restore scalar Content-Length ownership"
    "${RULE_STALE_HTTP2_RESPONSE_HEAD_SCALAR}"
    "${HTTP2_RESPONSE_HEADERS}"
    "${HTTP2_RESPONSE_CONNECTION_SOURCE}")
if(NOT EXISTS "${HTTP2_RESPONSE_HEAD_PLAN}" OR
   NOT EXISTS "${HTTP2_RESPONSE_HEADERS}" OR
   NOT EXISTS "${HTTP2_RESPONSE_CONNECTION_SOURCE}")
    boundary_error("typed HTTP/2 response-head plan is incomplete"
        "the plan, HPACK encoder, and connection driver are all required")
else()
    file(READ "${HTTP2_RESPONSE_HEAD_PLAN}" http2_response_head_plan)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_head_encoder)
    read_http2_connection_implementation(http2_response_head_connection)
    set(http2_response_head_missing)
    foreach(http2_head_probe IN ITEMS
            "class Http2ResponseHeadPlan final"
            "std::optional<std::uint64_t>"
            "contentLength[(][)] const noexcept"
            "streamingContentLength[(][)] const noexcept"
            "enum class ContentLengthMode : std::uint8_t"
            "is_trivially_copyable_v<Http2ResponseHeadPlan>"
            "sizeof[(]Http2ResponseHeadPlan[)] <= 24"
            "class Http2ResponseHeadPlanResult final"
            "std::get_if<Http2ResponseHeadPlan>"
            "HttpResponseBodyPlan bodyPlan_"
            "http2BufferedResponseHeadPlan"
            "http2StreamingResponseHeadPlan"
            "http2ConnectResponseHeadPlan")
        if(NOT http2_response_head_plan MATCHES "${http2_head_probe}")
            list(APPEND http2_response_head_missing
                "plan:${http2_head_probe}")
        endif()
    endforeach()
    foreach(http2_encoder_probe IN ITEMS
            "const Http2ResponseHeadPlan& plan"
            "knownBit == kResponseHeaderContentLength"
            "plan[.]contentLength[(][)]")
        if(NOT http2_response_head_encoder MATCHES
               "${http2_encoder_probe}")
            list(APPEND http2_response_head_missing
                "encoder:${http2_encoder_probe}")
        endif()
    endforeach()
    foreach(http2_connection_probe IN ITEMS
            "http2BufferedResponseHeadPlan"
            "http2StreamingResponseHeadPlan"
            "http2ConnectResponseHeadPlan"
            "headPlan->streamingContentLength[(][)]")
        if(NOT http2_response_head_connection MATCHES
               "${http2_connection_probe}")
            list(APPEND http2_response_head_missing
                "connection:${http2_connection_probe}")
        endif()
    endforeach()
    if(http2_response_head_plan MATCHES
           "Http2CanonicalResponseContentLength|Http2ExplicitResponseContentLength|Http2AbsentResponseContentLength|Http2ForbiddenResponseContentLength|using ContentLength = std::variant|canonicalContentLength[(]|explicitContentLength[(]|absentContentLength[(]|forbiddenContentLength[(]" OR
       http2_response_head_encoder MATCHES
           "Http2ExplicitContentLengthStatus|http2ExplicitResponseContentLength|emitAutoContentLength|std::uint64_t[ 	]+autoContentLength" OR
       http2_response_head_connection MATCHES
           "Http2ExplicitContentLengthStatus|http2ExplicitResponseContentLength|emitAutoContentLength")
        list(APPEND http2_response_head_missing "stale-scalar-api")
    endif()
    if(http2_response_head_missing)
        string(JOIN ", " http2_response_head_missing_text
            ${http2_response_head_missing})
        boundary_error("HTTP/2 response-head Content-Length escaped its exclusive plan"
            "HPACK field emission and streaming DATA accounting must consume the same direct execution plan without marker variants; missing ${http2_response_head_missing_text}")
    endif()
endif()
set(HTTP_STATUS_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpStatus.h")
set(HTTP_RESPONSE_MODEL_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpResponse.h")
set(HTTP_RESPONSE_MODEL_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp")
set(HTTP1_INTERIM_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-http/src/server/Http1InterimResponseWriter.cpp")
set(WEB_CONTEXT_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h")
set(WEB_CONTEXT_REQUEST_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequest.h")
set(WEB_VALIDATION_TARGET_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ValidationTypes.h")
set(WEB_CONTEXT_INLINE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/Context.inl")
set(WEB_CONTEXT_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextModel.inl")
set(WEB_CONTEXT_REQUEST_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextRequestModel.inl")
set(WEB_CONTEXT_CMAKE "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt")
set(WEB_CONTEXT_REQUEST_GUARD
    "${RUVIA_ROOT}/tests/guards/context_request_header_guard.cpp")
set(WEB_CONTEXT_REQUEST_INTERNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextRequestInternal.h")
set(WEB_CONTEXT_REQUEST_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequest.cpp")
set(WEB_CONTEXT_REQUEST_FACADE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextRequestFacade.cpp")
set(WEB_CONTEXT_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")
set(WEB_ERROR_NORMALIZE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/HttpErrorNormalize.h")

foreach(stale_context_inline IN ITEMS
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.inl"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextModel.h"
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/ContextRequestModel.h")
    if(EXISTS "${stale_context_inline}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${stale_context_inline}")
        boundary_error("Context implementation escaped detail/http ownership"
            "${relative} must not restore the removed public-root path")
    endif()
endforeach()
if(EXISTS "${WEB_CONTEXT_HEADER}")
    file(READ "${WEB_CONTEXT_HEADER}" web_context_public_header)
    if(NOT web_context_public_header MATCHES
           "ruvia/web/detail/http/Context[.]inl" OR
       NOT web_context_public_header MATCHES
           "ruvia/web/detail/http/ContextModel[.]inl")
        boundary_error("Context public templates depend on caller include order"
            "Context.h must include both detail/http inline implementation headers")
    endif()
endif()
if(NOT EXISTS "${WEB_CONTEXT_REQUEST_HEADER}" OR
   NOT EXISTS "${WEB_CONTEXT_MODEL}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_MODEL}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_FACADE_SOURCE}" OR
   NOT EXISTS "${WEB_CONTEXT_CMAKE}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_GUARD}")
    boundary_error("Context request API ownership is incomplete"
        "ContextRequest.h, its detail/http model templates, install entry, and standalone guard must remain")
elseif(EXISTS "${WEB_CONTEXT_HEADER}")
    file(READ "${WEB_CONTEXT_REQUEST_HEADER}" web_context_request_header)
    file(READ "${WEB_CONTEXT_MODEL}" web_context_model)
    file(READ "${WEB_CONTEXT_REQUEST_MODEL}" web_context_request_model)
    file(READ "${WEB_CONTEXT_REQUEST_FACADE_SOURCE}"
        web_context_request_facade_source)
    file(READ "${WEB_CONTEXT_CMAKE}" web_context_cmake)
    file(READ "${WEB_CONTEXT_REQUEST_GUARD}" web_context_request_guard)
    if(NOT web_context_public_header MATCHES
           "ruvia/web/ContextRequest[.]h" OR
       web_context_public_header MATCHES "class ContextRequest final" OR
       web_context_public_header MATCHES
           "ContextRequest::[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*[(]" OR
       NOT web_context_request_header MATCHES "class ContextRequest final" OR
       NOT web_context_request_header MATCHES
           "ruvia/web/detail/http/ContextRequestModel[.]inl" OR
       web_context_model MATCHES "ContextRequest::" OR
       NOT web_context_request_facade_source MATCHES
           "ContextRequest::method[(][)] const noexcept" OR
       NOT web_context_request_facade_source MATCHES
           "ScopedOperation<std::string_view> ContextRequest::text[(][)] const" OR
       NOT web_context_request_facade_source MATCHES
           "requestHeaderFields[(]const ContextRequest& request[)]" OR
       NOT web_context_cmake MATCHES
           "include/ruvia/web/ContextRequest[.]h" OR
       NOT web_context_cmake MATCHES
           "src/http/ContextRequestFacade[.]cpp" OR
       NOT web_context_request_guard MATCHES
           "#[ \t]*include[ \t]*[<\"]ruvia/web/ContextRequest[.]h" OR
       NOT web_context_request_guard MATCHES
           "&ruvia::ContextRequest::method")
        boundary_error("Context request API collapsed back into the response/state header"
            "Context.h must be an umbrella; request types/templates, linked facade, installation, and standalone compilation have separate ownership")
    endif()
endif()
if(NOT EXISTS "${WEB_CONTEXT_REQUEST_HEADER}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_INTERNAL}")
    boundary_error("Request parse-result construction boundary is incomplete"
        "ContextRequest.h and detail/http/ContextRequestInternal.h are required")
else()
    file(READ "${WEB_CONTEXT_REQUEST_HEADER}" web_request_result_contract)
    file(READ "${WEB_CONTEXT_REQUEST_INTERNAL}" web_request_result_internal)
    if(NOT web_request_result_contract MATCHES
           "friend[ \t]+struct[ \t]+detail::RequestFormDataAccess" OR
       NOT web_request_result_contract MATCHES
           "RequestFormField&[ \t]+operator=[(]RequestFormField&&[)][ \t]*=[ \t]*delete" OR
       NOT web_request_result_contract MATCHES
           "Entry[(]const[ \t]+Entry&[)][ \t]*=[ \t]*delete" OR
       NOT web_request_result_contract MATCHES
           "Entry&[ \t]+operator=[(]Entry&&[)][ \t]*=[ \t]*delete" OR
       NOT web_request_result_contract MATCHES
           "RequestFormData[(]const[ \t]+RequestFormData&[)][ \t]*=[ \t]*delete" OR
       NOT web_request_result_contract MATCHES
           "RequestFormData&[ \t]+operator=[(]RequestFormData&&[)][ \t]*=[ \t]*delete" OR
       NOT web_request_result_contract MATCHES
           "Object[(]const[ \t]+Object&[)][ \t]*=[ \t]*delete" OR
       NOT web_request_result_contract MATCHES
           "Object&[ \t]+operator=[(]Object&&[)][ \t]*=[ \t]*delete" OR
       NOT web_request_result_internal MATCHES
           "struct[ \t]+RequestFormDataAccess[ \t]+final" OR
       NOT web_request_result_internal MATCHES
           "RequestFormData[ \t]+fromFields")
        boundary_error("Request parse results regained unsafe construction, assignment, or pointer-copy semantics"
            "RequestFormField, Entry, RequestFormData, and Object must be move-construct-only; parser construction must use RequestFormDataAccess")
    endif()
endif()
set(WEB_CONTEXT_REQUEST_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(WEB_CONTEXT_REQUEST_API_SURFACE
    "${RUVIA_ROOT}/examples/api_surface.cpp")
if(NOT EXISTS "${WEB_CONTEXT_REQUEST_HEADER}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_GUARD}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_PACKAGE_CONSUMER}" OR
   NOT EXISTS "${WEB_CONTEXT_REQUEST_API_SURFACE}")
    boundary_error("Request form borrow-lifetime coverage is incomplete"
        "the public header, standalone guard, package consumer, and API surface are required")
else()
    file(READ "${WEB_CONTEXT_REQUEST_HEADER}"
        web_request_form_lifetime_contract)
    file(READ "${WEB_CONTEXT_REQUEST_GUARD}"
        web_request_form_lifetime_guard)
    file(READ "${WEB_CONTEXT_REQUEST_PACKAGE_CONSUMER}"
        web_request_form_lifetime_package_consumer)
    file(READ "${WEB_CONTEXT_REQUEST_API_SURFACE}"
        web_request_form_lifetime_api_surface)
    if(NOT web_request_form_lifetime_contract MATCHES
           "name[(][)] const &[ 	]+noexcept" OR
       NOT web_request_form_lifetime_contract MATCHES
           "name[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_request_form_lifetime_contract MATCHES
           "blob[(][)] const &[ 	]+noexcept" OR
       NOT web_request_form_lifetime_contract MATCHES
           "blob[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_request_form_lifetime_contract MATCHES
           "fields[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_request_form_lifetime_contract MATCHES
           "groups[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT web_request_form_lifetime_contract MATCHES
           "Value[ 	]+get[(]std::string_view[)][ 	]+const &&[ 	]*=[ 	]*delete" OR
       NOT web_request_form_lifetime_contract MATCHES
           "Object[ 	]+object[(]std::string_view[)][ 	]+const &&[ 	]*=[ 	]*delete")
        boundary_error("Request form owning values regained temporary borrow access"
            "RequestFormField, Entry, RequestFormData, and Object must expose borrowed data only from stable lvalues")
    endif()
    foreach(web_request_form_lifetime_coverage IN ITEMS
            "${web_request_form_lifetime_guard}"
            "${web_request_form_lifetime_package_consumer}"
            "${web_request_form_lifetime_api_surface}")
        if(NOT web_request_form_lifetime_coverage MATCHES
               "static_assert[(]!ExposesAnyRvalueRequestFormFieldBorrow" OR
           NOT web_request_form_lifetime_coverage MATCHES
               "static_assert[(]!ExposesRvalueRequestFormEntryFields" OR
           NOT web_request_form_lifetime_coverage MATCHES
               "static_assert[(]!ExposesAnyRvalueRequestFormDataBorrow" OR
           NOT web_request_form_lifetime_coverage MATCHES
               "static_assert[(]!ExposesRvalueRequestFormObjectGroups")
            boundary_error("Request form temporary-borrow regression coverage is incomplete"
                "standalone, installed-package, and API-surface consumers must reject every owning rvalue borrow")
            break()
        endif()
    endforeach()
endif()
if(EXISTS "${WEB_CONTEXT_HEADER}" AND
   EXISTS "${WEB_CONTEXT_REQUEST_HEADER}" AND
   EXISTS "${WEB_VALIDATION_TARGET_HEADER}")
    file(READ "${WEB_VALIDATION_TARGET_HEADER}"
        web_validation_target_header)
    set(web_context_public_api
        "${web_context_public_header}\n${web_context_request_header}")
    if(web_context_public_api MATCHES
           "valid[(]std::string_view[ 	]+target[)]" OR
       web_context_public_api MATCHES
           "addValidatedData[(]std::string_view[ 	]+target" OR
       web_validation_target_header MATCHES
           "validationTargetFromName" OR
       web_validation_target_header MATCHES
           "inline[ 	]+constexpr[ 	]+ValidationTarget[ 	]+(Json|Form|Query|Param|Header|Cookie)")
        boundary_error("validation targets regained parallel string APIs"
            "Context validation must use only the scoped ValidationTarget vocabulary")
    endif()
    if(web_context_public_api MATCHES "arrayBuffer[(]")
        boundary_error("binary request bodies regained a parallel name"
            "ContextRequest and RequestBlob must use bytes()")
    endif()
    if(web_context_public_api MATCHES "formData[(]" OR
       web_context_public_api MATCHES "SingleValueSelection")
        boundary_error("form body parsing regained entry-dependent semantics"
            "parseBody() must be the only RequestFormData constructor path and duplicate scalar lookup must select the last value")
    endif()
    if(NOT web_validation_target_header MATCHES
           "enum[ 	]+class[ 	]+ValidationTarget[ 	]*:[ 	]*std::uint8_t")
        boundary_error("validation targets lost their typed contract"
            "Context and controller validation must share ValidationTarget")
    endif()
endif()
if(EXISTS "${WEB_CONTEXT_REQUEST_SOURCE}")
    file(READ "${WEB_CONTEXT_REQUEST_SOURCE}"
        web_context_request_source)
    if(web_context_request_source MATCHES "SingleValueSelection")
        boundary_error("form body runtime regained split selection policy"
            "ContextRequest.cpp must produce one last-wins RequestFormData representation")
    endif()
endif()

foreach(response_status_contract_file IN ITEMS
        "${HTTP_STATUS_HEADER}"
        "${HTTP_RESPONSE_MODEL_HEADER}"
        "${HTTP_RESPONSE_MODEL_SOURCE}"
        "${HTTP_RESPONSE_HEAD_SOURCE}"
        "${HTTP1_INTERIM_RESPONSE_WRITER}"
        "${HTTP2_RESPONSE_HEADERS}"
        "${WEB_CONTEXT_HEADER}"
        "${WEB_CONTEXT_INLINE}"
        "${WEB_CONTEXT_MODEL}"
        "${WEB_CONTEXT_RESPONSE_SOURCE}"
        "${WEB_ERROR_NORMALIZE}")
    if(NOT EXISTS "${response_status_contract_file}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${response_status_contract_file}")
        boundary_error("version-neutral response-status contract is incomplete"
            "${relative} is required")
    endif()
endforeach()

check_files_no_match("generic response model must not own an HTTP/1 reason phrase"
    "${RULE_STALE_RESPONSE_REASON_PHRASE}"
    "${HTTP_STATUS_HEADER}"
    "${HTTP_RESPONSE_MODEL_HEADER}"
    "${HTTP_RESPONSE_MODEL_SOURCE}"
    "${HTTP_RESPONSE_HEAD_SOURCE}"
    "${HTTP1_INTERIM_RESPONSE_WRITER}")
check_files_no_match("Context response helpers must remain status-code only"
    "${RULE_STALE_CONTEXT_REASON_PHRASE}"
    "${WEB_CONTEXT_HEADER}"
    "${WEB_CONTEXT_INLINE}"
    "${WEB_CONTEXT_MODEL}"
    "${WEB_CONTEXT_RESPONSE_SOURCE}")
check_files_no_match("HTTP/2 response encoding must not consume a reason phrase"
    "${RULE_HTTP2_REASON_PHRASE}"
    "${HTTP2_RESPONSE_HEADERS}"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")

if(EXISTS "${HTTP_STATUS_HEADER}" AND
   EXISTS "${HTTP_RESPONSE_MODEL_HEADER}" AND
   EXISTS "${HTTP_RESPONSE_HEAD_SOURCE}" AND
   EXISTS "${HTTP1_INTERIM_RESPONSE_WRITER}" AND
   EXISTS "${HTTP2_RESPONSE_HEADERS}" AND
   EXISTS "${WEB_CONTEXT_HEADER}" AND
   EXISTS "${WEB_ERROR_NORMALIZE}")
    file(READ "${HTTP_STATUS_HEADER}" http_status_header)
    file(READ "${HTTP_RESPONSE_MODEL_HEADER}" http_response_model_header)
    file(READ "${HTTP_RESPONSE_HEAD_SOURCE}" http1_response_head_source)
    file(READ "${HTTP1_INTERIM_RESPONSE_WRITER}" http1_interim_response_writer)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_status_response_headers)
    file(READ "${WEB_CONTEXT_HEADER}" web_context_status_header)
    file(READ "${WEB_ERROR_NORMALIZE}" web_error_status_normalize)
    file(READ "${RUVIA_ROOT}/tests/package-consumer/web.cpp"
        web_context_status_package_verify)
    if(NOT http_status_header MATCHES "httpReasonPhrase" OR
       NOT http_status_header MATCHES "default:[ \t]*return[ \t]*[{][}];" OR
       NOT http_response_model_header MATCHES
           "void status[(]std::uint16_t statusCode[)];" OR
       NOT http1_response_head_source MATCHES
           "httpReasonPhrase[(]responseStatus[)]" OR
       NOT http1_response_head_source MATCHES "sink[.]append[(]' '[)]" OR
       NOT http1_interim_response_writer MATCHES
           "httpReasonPhrase[(]response[.]status[(][)][)]" OR
       NOT http1_interim_response_writer MATCHES
           "[*]cursor[+][+][ \t]*=[ \t]*' '" OR
       NOT http2_status_response_headers MATCHES
           "plan[.]bodyPlan[(][)][.]responseStatus[(][)]" OR
       NOT web_context_status_header MATCHES
           "void status[(]std::uint16_t statusCode[)];" OR
       web_context_status_header MATCHES "ResponseInit|ResponseHeaderInit" OR
       NOT web_context_status_package_verify MATCHES
           "!HasResponseInit<ruvia::Context>" OR
       NOT web_context_status_package_verify MATCHES
           "!HasBuilderMetadataArguments<ruvia::Context>" OR
       NOT web_error_status_normalize MATCHES "statusText = \"HTTP Error\"")
        boundary_error("response status and reason-phrase ownership split again"
            "Context metadata must have one status path, body builders must not accept metadata, H1 derives an optional phrase, and H2 emits :status")
    endif()
endif()

set(HTTP2_CLIENT_RESPONSE_HEADERS_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2ConnectionHeaders.cpp")
set(HTTP1_CLIENT_RESPONSE_STATUS_TEST
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp")
set(HTTP2_CLIENT_RESPONSE_STATUS_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
foreach(http_client_status_contract_file IN ITEMS
        "${HTTP_STATUS_HEADER}"
        "${HTTP1_CLIENT_RESPONSE_SOURCE}"
        "${HTTP2_CLIENT_RESPONSE_HEADERS_SOURCE}"
        "${HTTP1_CLIENT_RESPONSE_STATUS_TEST}"
        "${HTTP2_CLIENT_RESPONSE_STATUS_TEST}")
    if(NOT EXISTS "${http_client_status_contract_file}")
        file(RELATIVE_PATH relative
            "${RUVIA_ROOT}" "${http_client_status_contract_file}")
        boundary_error("cross-version HTTP client status contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP_STATUS_HEADER}" AND
   EXISTS "${HTTP1_CLIENT_RESPONSE_SOURCE}" AND
   EXISTS "${HTTP2_CLIENT_RESPONSE_HEADERS_SOURCE}" AND
   EXISTS "${HTTP1_CLIENT_RESPONSE_STATUS_TEST}" AND
   EXISTS "${HTTP2_CLIENT_RESPONSE_STATUS_TEST}")
    file(READ "${HTTP_STATUS_HEADER}" http_status_code_contract)
    file(READ "${HTTP1_CLIENT_RESPONSE_SOURCE}"
        http1_client_response_status_parser)
    file(READ "${HTTP2_CLIENT_RESPONSE_HEADERS_SOURCE}"
        http2_client_response_status_parser)
    file(READ "${HTTP1_CLIENT_RESPONSE_STATUS_TEST}"
        http1_client_response_status_test)
    file(READ "${HTTP2_CLIENT_RESPONSE_STATUS_TEST}"
        http2_client_response_status_test)
    if(NOT http_status_code_contract MATCHES
           "statusCode >= 100 && statusCode <= 599" OR
       NOT http1_client_response_status_parser MATCHES
           "detail::httpStatusCodeValid" OR
       NOT http2_client_response_status_parser MATCHES
           "#[ \t]*include[ \t]*[<\"]ruvia/http/HttpStatus[.]h[>\"]" OR
       NOT http2_client_response_status_parser MATCHES
           "httpStatusCodeValid[(]status[)]" OR
       NOT http1_client_response_status_test MATCHES
           "http_client_rejects_malformed_status_and_length_fields" OR
       NOT http2_client_response_status_test MATCHES
           "http2_connection_client_rejects_status_outside_http_range")
        boundary_error("HTTP client response status-code space diverged across versions"
            "HTTP/1 and HTTP/2 must consume the shared 100..599 wire contract, with both boundaries pinned by protocol-specific regressions")
    endif()
endif()

check_files_no_match("Context response status recovered a zero sentinel"
    "statusCode[ \t]*(==|!=)[ \t]*0|statusCode[ \t]*=[ \t]*0"
    "${WEB_CONTEXT_HEADER}"
    "${WEB_CONTEXT_INLINE}"
    "${WEB_CONTEXT_MODEL}"
    "${WEB_CONTEXT_RESPONSE_SOURCE}")
check_files_no_match("Context storage re-inferred explicit status 200"
    "response[.]status[(][)][ \t]*==[ \t]*200"
    "${WEB_CONTEXT_RESPONSE_SOURCE}")

if(EXISTS "${WEB_CONTEXT_HEADER}" AND
   EXISTS "${WEB_CONTEXT_RESPONSE_SOURCE}" AND
   EXISTS "${WEB_CONTEXT_INTERNAL}")
    file(READ "${WEB_CONTEXT_HEADER}" web_context_status_header)
    file(READ "${WEB_CONTEXT_RESPONSE_SOURCE}" web_context_response_metadata)
    file(READ "${WEB_CONTEXT_INTERNAL}" web_context_internal_metadata)
    file(READ
        "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextResponseState.h"
        web_context_response_state)
    if(NOT web_context_status_header MATCHES
           "using HeaderOptions = HttpResponse::HeaderOptions" OR
       NOT web_context_status_header MATCHES
           "ContextResponseState responseState_" OR
       NOT web_context_internal_metadata MATCHES
           "responseState_[(]memory[.]resource[(][)][)]" OR
       NOT web_context_response_metadata MATCHES
           "responseState_[.]activeResponse[(][)][.]status[(]statusCode[)]" OR
       NOT web_context_response_metadata MATCHES
           "responseState_[.]activeResponse[(][)][.]header" OR
       NOT web_context_response_state MATCHES
           "std::variant<ContextPendingResponse" OR
       NOT web_context_response_state MATCHES
           "ContextProvisionalResponse" OR
       NOT web_context_response_state MATCHES
           "ContextFinalResponse> value_" OR
       web_context_status_header MATCHES
           "responseMetadata_|response_[{;]|responseFinalized_|responseStatusCode_|responseHeaders_|responseHeaderIndexes_" OR
       web_context_response_metadata MATCHES
           "httpFinalStatusCodeValid|isValidHttpHeaderName|isValidHttpHeaderValue")
        boundary_error("Context response lifecycle lost its single typed owner"
            "Pending, provisional, and final phases must share one ContextResponseState-owned HttpResponse")
    endif()
endif()

set(RESPONSE_STATUS_MODEL_TEST
    "${RUVIA_ROOT}/tests/unit_http_response.cpp")
set(RESPONSE_REASON_PHRASE_TEST
    "${RUVIA_ROOT}/tests/unit_error.cpp")
set(RESPONSE_HEAD_REASON_PHRASE_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
set(RESPONSE_ERROR_LABEL_TEST
    "${RUVIA_ROOT}/tests/unit_error_response.cpp")
set(RESPONSE_API_SURFACE_TEST
    "${RUVIA_ROOT}/examples/api_surface.cpp")
if(EXISTS "${RESPONSE_STATUS_MODEL_TEST}" AND
   EXISTS "${RESPONSE_REASON_PHRASE_TEST}" AND
   EXISTS "${RESPONSE_HEAD_REASON_PHRASE_TEST}" AND
   EXISTS "${RESPONSE_ERROR_LABEL_TEST}" AND
   EXISTS "${RESPONSE_API_SURFACE_TEST}")
    file(READ "${RESPONSE_STATUS_MODEL_TEST}" response_status_model_test)
    file(READ "${RESPONSE_REASON_PHRASE_TEST}" response_reason_phrase_test)
    file(READ "${RESPONSE_HEAD_REASON_PHRASE_TEST}" response_head_reason_phrase_test)
    file(READ "${RESPONSE_ERROR_LABEL_TEST}" response_error_label_test)
    file(READ "${RESPONSE_API_SURFACE_TEST}" response_status_api_surface)
    if(NOT response_status_model_test MATCHES
           "response_status_is_version_neutral_code_only" OR
       NOT response_reason_phrase_test MATCHES
           "http_reason_phrase_does_not_mislabel_extension_statuses" OR
       NOT response_head_reason_phrase_test MATCHES
           "response_head_extension_status_uses_an_empty_reason_phrase" OR
       NOT response_head_reason_phrase_test MATCHES "HTTP/1[.]1 299" OR
       NOT response_error_label_test MATCHES "HTTP Error" OR
       NOT response_status_api_surface MATCHES "HasResponseReasonPhraseSetter")
        boundary_error("response status/reason-phrase regression coverage is incomplete"
            "API shape, unknown-code phrase, H1 empty phrase, and Web-only error label all require direct coverage")
    endif()
endif()

set(HTTP1_RESPONSE_HEAD_POLICY_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_policy.cpp")
set(HTTP1_RESPONSE_STREAM_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp")
set(HTTP_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${RESPONSE_HEAD_REASON_PHRASE_TEST}" AND
   EXISTS "${HTTP1_RESPONSE_HEAD_POLICY_TEST}" AND
   EXISTS "${HTTP1_RESPONSE_STREAM_PLAN_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${RESPONSE_HEAD_REASON_PHRASE_TEST}"
        http1_response_head_wire_test)
    file(READ "${HTTP1_RESPONSE_HEAD_POLICY_TEST}"
        http1_response_head_policy_test)
    file(READ "${HTTP1_RESPONSE_STREAM_PLAN_TEST}"
        http1_response_stream_plan_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}"
        http1_response_head_package_test)
    if(NOT http1_response_head_wire_test MATCHES
           "response_head_close_delimited_stream_rejects_declared_framing" OR
       NOT http1_response_head_wire_test MATCHES
           "http1_buffered_response_plan_owns_request_version_and_length" OR
       NOT http1_response_head_wire_test MATCHES "HTTP/1[.]0 200 OK" OR
       NOT http1_response_head_wire_test MATCHES
           "Transfer-Encoding: chunked" OR
       NOT http1_response_head_wire_test MATCHES
           "Content-Length: 8" OR
       NOT http1_response_head_policy_test MATCHES
           "http1_response_head_framing_is_an_exclusive_plan" OR
       NOT http1_response_head_policy_test MATCHES
           "Http1BufferedResponsePlan" OR
       NOT http1_response_stream_plan_test MATCHES
           "http1_prepared_stream_head_owns_exact_wire_framing" OR
       NOT http1_response_stream_plan_test MATCHES
           "responseHeadPlan[(][)][.]closeDelimitedStream[(][)]" OR
       NOT http1_response_stream_plan_test MATCHES
           "http10Wire[.]starts_with[(]\"HTTP/1[.]0" OR
       NOT http1_response_stream_plan_test MATCHES "failedHttp10" OR
       NOT http1_response_stream_plan_test MATCHES
           "failedHttp10[.]connectionPlan[.]protocolVersion[(][)]" OR
       NOT http1_response_head_package_test MATCHES
           "HasHttp1ResponseHeadAlternatives" OR
       NOT http1_response_head_package_test MATCHES
           "HasHttp1ProtocolVersion" OR
       NOT http1_response_head_package_test MATCHES
           "HasHttp1BufferedPlanContract" OR
       NOT http1_response_head_package_test MATCHES
           "!HasStaleHttp1BufferedWritePlanForwarder" OR
       NOT http1_response_head_package_test MATCHES
           "!HasStaleHttp1ResponseSignal" OR
       NOT http1_response_head_package_test MATCHES
           "!HasStaleHttp1ResponseHeadScalar" OR
       NOT http1_response_head_package_test MATCHES
           "!HasStalePreparedStreamPolicy")
        boundary_error("typed HTTP/1 response-head framing is under-tested"
            "wire tests, parser body-failure tests, prepared-plan tests, and installed consumers must pin exact HTTP/1.0 status-line ownership, canonical buffered length, chunked framing, TE/CL filtering, HEAD metadata, and removal of scalar/version-signal APIs")
    endif()
endif()

set(HTTP2_RESPONSE_HEAD_PLAN_TEST
    "${RUVIA_ROOT}/tests/unit_http2_response_headers.cpp")
set(HTTP2_RESPONSE_HEAD_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
if(EXISTS "${HTTP2_RESPONSE_HEAD_PLAN_TEST}" AND
   EXISTS "${HTTP2_RESPONSE_HEAD_CONNECTION_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_RESPONSE_HEAD_PLAN_TEST}"
        http2_response_head_plan_test)
    file(READ "${HTTP2_RESPONSE_HEAD_CONNECTION_TEST}"
        http2_response_head_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}"
        http2_response_head_package_test)
    if(NOT http2_response_head_plan_test MATCHES
           "http2_response_head_content_length_plan_drives_execution" OR
       NOT http2_response_head_plan_test MATCHES
           "http2_response_headers_canonicalize_valid_explicit_content_length_once" OR
       NOT http2_response_head_plan_test MATCHES
           "http2_response_headers_reject_only_preserved_invalid_content_length" OR
       NOT http2_response_head_plan_test MATCHES
           "!std::is_default_constructible_v<Http2ResponseHeadPlan>" OR
       NOT http2_response_head_connection_test MATCHES
           "http2_connection_streaming_content_length_finish_and_trailers_are_exact" OR
       NOT http2_response_head_package_test MATCHES
           "HasHttp2ResponseHeadExecutionPlan" OR
       NOT http2_response_head_package_test MATCHES
           "!std::default_initializable<[ \t\r\n]*ruvia::detail::Http2ResponseHeadPlan>" OR
       NOT http2_response_head_package_test MATCHES
           "http2BufferedResponseHeadPlan" OR
       NOT http2_response_head_package_test MATCHES
           "http2StreamingResponseHeadPlan" OR
       NOT http2_response_head_package_test MATCHES
           "Http2ResponseHeadPlanError::kInvalidContentLength")
        boundary_error("typed HTTP/2 response-head plan is under-tested"
            "unit, connection, and installed-package tests must pin exclusive length ownership, single parsing, canonical wire bytes, invalid failure, and DATA accounting")
    endif()
endif()

if(EXISTS "${HTTP_RESPONSE_HEAD_SOURCE}" AND EXISTS "${HTTP2_RESPONSE_HEADERS}")
    file(READ "${HTTP_RESPONSE_HEAD_SOURCE}" http_response_head_source)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_headers)
    if(NOT http_response_head_source MATCHES "canonicalContentLength" OR
       NOT http2_response_headers MATCHES "plan[.]contentLength[(][)]")
        boundary_error("205 zero-length canonicalization is incomplete across protocols"
            "HTTP/1 and HTTP/2 writers must replace application framing with length zero")
    endif()
endif()

set(HTTP_FINAL_RESPONSE_CONTROL_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpFinalResponseControlPlan.h")
set(HTTP1_SERVER_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(HTTP2_HEADER_RULES
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2HeaderRules.h")
set(HTTP_RESPONSE_CONTENT_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseContentSemantics.h")
set(HTTP_REQUEST_CONTENT_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestContentSemantics.h")
set(HTTP1_CLIENT_REQUEST_SEMANTICS_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp")
set(HTTP2_CLIENT_REQUEST_SEMANTICS_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2ConnectionSubmit.cpp")
set(HTTP_RESPONSE_WRITE_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h")
if(NOT EXISTS "${HTTP_FINAL_RESPONSE_CONTROL_PLAN}" OR
   NOT EXISTS "${HTTP1_SERVER_SEMANTICS}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}" OR
   NOT EXISTS "${HTTP2_HEADER_RULES}" OR
   NOT EXISTS "${HTTP2_RESPONSE_HEADERS}")
    boundary_error("final response control plan is missing"
        "the protocol-specific results, H1 finalizer, H2 field rules, encoder, and all H2 submit paths are required")
else()
    file(READ "${HTTP_FINAL_RESPONSE_CONTROL_PLAN}" http_final_response_control_plan)
    file(READ "${HTTP1_SERVER_SEMANTICS}" http1_server_semantics)
    read_http2_connection_implementation(http2_connection_source)
    file(READ "${HTTP2_HEADER_RULES}" http2_header_rules)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_headers)
    if(NOT http_final_response_control_plan MATCHES
           "class Http1FinalResponseControl final" OR
       NOT http_final_response_control_plan MATCHES
           "class Http2FinalResponseControl final" OR
       NOT http_final_response_control_plan MATCHES
           "enum class Http1FinalResponseControlPlanError" OR
       NOT http_final_response_control_plan MATCHES
           "enum class Http2FinalResponseControlPlanError" OR
       NOT http_final_response_control_plan MATCHES
           "class Http1FinalResponseControlPlanFailure final" OR
       NOT http_final_response_control_plan MATCHES
           "class Http2FinalResponseControlPlanFailure final" OR
       NOT http_final_response_control_plan MATCHES
           "template <typename Control, typename Failure>" OR
       NOT http_final_response_control_plan MATCHES
           "using Http1FinalResponseControlPlanResult" OR
       NOT http_final_response_control_plan MATCHES
           "using Http2FinalResponseControlPlanResult" OR
       NOT http_final_response_control_plan MATCHES
           "using Value = std::variant<Control, Failure>" OR
       NOT http_final_response_control_plan MATCHES
           "const Control[*] control[(][)] const [&] noexcept" OR
       NOT http_final_response_control_plan MATCHES
           "control[(][)] const && = delete" OR
       NOT http_final_response_control_plan MATCHES
           "http1FinalResponseControlPlan" OR
       NOT http_final_response_control_plan MATCHES
           "http2FinalResponseControlPlan" OR
       NOT http_final_response_control_plan MATCHES
           "is_trivially_copyable_v<[ \t\r\n]*Http1FinalResponseControlPlanResult>" OR
       NOT http_final_response_control_plan MATCHES
           "sizeof[(]Http1FinalResponseControlPlanResult[)] <= 8" OR
       NOT http_final_response_control_plan MATCHES
           "is_trivially_copyable_v<[ \t\r\n]*Http2FinalResponseControlPlanResult>" OR
       NOT http_final_response_control_plan MATCHES
           "sizeof[(]Http2FinalResponseControlPlanResult[)] <= 2" OR
       NOT http_final_response_control_plan MATCHES "statusCode == 426" OR
       NOT http_final_response_control_plan MATCHES "kUpgradeUnavailable" OR
       NOT http_final_response_control_plan MATCHES
           "kConnectionSpecificFieldForbidden" OR
       NOT http2_header_rules MATCHES
           "http2IsForbiddenResponseConnectionField" OR
       NOT http2_response_headers MATCHES
           "const Http2FinalResponseControl& control" OR
       http2_response_headers MATCHES
           "http2ResponseConnectionHeaderForbidden" OR
       http_final_response_control_plan MATCHES
           "HttpFinalResponseControlPlanError|HttpFinalResponseControlPlanFailure|httpFinalResponseControlPlan[(]|HttpProtocolVersion|std::variant<[ \t\r\n]*Http1FinalResponseControl,[ \t\r\n]*Http2FinalResponseControl|http1[(][)] const [&]|http2[(][)] const [&]|class HttpFinalResponseControlPlan final|using Protocol = std::variant|std::get_if<HttpFinalResponseControlPlan>|plan[(][)] const [&] noexcept" OR
       http_final_response_control_plan MATCHES
           "${RULE_STALE_FINAL_RESPONSE_CONTROL_TUPLE}")
        boundary_error("final response status/Upgrade paths have diverged"
            "protocol-specific results must own one control token or typed failure, parsed H1 fields, and RFC 9113 connection-field rejection before encoding")
    endif()
    if(NOT http1_server_semantics MATCHES "class Http1FinalResponseCommitResult final" OR
       NOT http1_server_semantics MATCHES "using Value = std::variant" OR
       NOT http1_server_semantics MATCHES "controlResult[.]failure[(][)]" OR
       NOT http1_server_semantics MATCHES "http1FinalResponseControlPlan" OR
       NOT http1_server_semantics MATCHES "controlResult[.]control[(][)]" OR
       NOT http1_server_semantics MATCHES "connectionOptions[(][)]" OR
       NOT http1_server_semantics MATCHES "upgradeProtocols[(][)]" OR
       http1_server_semantics MATCHES "httpFinalResponseControlPlan|controlResult[.]http1[(][)]|controlResult[.]http2[(][)]|controlResult[.]plan[(][)]|http1ResponseConnectionOptions")
        boundary_error("HTTP/1 final response control was reparsed or reduced to scalars"
            "the typed commit must retain failures and consume the H1 alternative's already parsed Connection and Upgrade states")
    endif()
    string(REGEX MATCHALL "http2FinalResponseControlPlan[(]"
        http2_final_control_calls "${http2_connection_source}")
    list(LENGTH http2_final_control_calls http2_final_control_call_count)
    if(http2_final_control_call_count LESS 3 OR
       NOT http2_connection_source MATCHES "controlResult[.]control[(][)]" OR
       http2_connection_source MATCHES "httpFinalResponseControlPlan|controlResult[.]http1[(][)]|controlResult[.]http2[(][)]|controlResult[.]plan[(][)]" OR
       NOT http2_connection_source MATCHES
           "submitConnectResponseHead[^(]*[(]")
        boundary_error("HTTP/2 final response paths bypassed shared control planning"
            "buffered, streaming, and CONNECT final heads must each obtain the HTTP/2 validation token before HPACK or stream mutation")
    endif()
endif()
set(FINAL_RESPONSE_CONTROL_TEST
    "${RUVIA_ROOT}/tests/unit_final_response_control.cpp")
set(HTTP2_FINAL_RESPONSE_HEADER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_response_headers.cpp")
set(HTTP2_FINAL_RESPONSE_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(HTTP2_CONNECT_RESPONSE_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp")
set(HTTP_FINAL_CONTROL_PACKAGE_TEST
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(final_control_test_file IN ITEMS
        "${FINAL_RESPONSE_CONTROL_TEST}"
        "${HTTP2_FINAL_RESPONSE_HEADER_TEST}"
        "${HTTP2_FINAL_RESPONSE_CONNECTION_TEST}"
        "${HTTP2_CONNECT_RESPONSE_TEST}"
        "${HTTP_FINAL_CONTROL_PACKAGE_TEST}")
    if(NOT EXISTS "${final_control_test_file}")
        file(RELATIVE_PATH relative
            "${RUVIA_ROOT}" "${final_control_test_file}")
        boundary_error("final-response control coverage is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${FINAL_RESPONSE_CONTROL_TEST}" AND
   EXISTS "${HTTP2_FINAL_RESPONSE_HEADER_TEST}" AND
   EXISTS "${HTTP2_FINAL_RESPONSE_CONNECTION_TEST}" AND
   EXISTS "${HTTP2_CONNECT_RESPONSE_TEST}" AND
   EXISTS "${HTTP_FINAL_CONTROL_PACKAGE_TEST}")
    file(READ "${FINAL_RESPONSE_CONTROL_TEST}" final_response_control_test)
    file(READ "${HTTP2_FINAL_RESPONSE_HEADER_TEST}"
        http2_final_response_header_test)
    file(READ "${HTTP2_FINAL_RESPONSE_CONNECTION_TEST}"
        http2_final_response_connection_test)
    file(READ "${HTTP2_CONNECT_RESPONSE_TEST}"
        http2_connect_response_test)
    file(READ "${HTTP_FINAL_CONTROL_PACKAGE_TEST}"
        http_final_control_package_test)
    if(NOT final_response_control_test MATCHES
           "final_response_control_entry_points_own_only_their_protocol" OR
       NOT final_response_control_test MATCHES
           "final_response_control_failure_never_exposes_protocol_alternative" OR
       NOT final_response_control_test MATCHES
           "final_response_control_rejects_every_http2_connection_specific_field" OR
       NOT http2_final_response_header_test MATCHES
           "http2_response_headers_reject_connection_specific_fields_before_hpack" OR
       NOT http2_final_response_connection_test MATCHES
           "http2_connection_rejects_connection_specific_final_heads_transactionally" OR
       NOT http2_connect_response_test MATCHES "invalidConnection" OR
       NOT http_final_control_package_test MATCHES
           "HasFinalResponseControlResult" OR
       NOT http_final_control_package_test MATCHES
           "!HasStaleFinalResponseControlStatus")
        boundary_error("final-response control coverage is incomplete"
            "unit, encoder, buffered/streaming/CONNECT, and installed-consumer tests must pin exclusive result alternatives and pre-HPACK rejection")
    endif()
endif()
check_files_no_match("outbound responses restored the invalid 600..999 status range"
    "statusCode[ ]*>[ ]*999"
    "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextResponse.cpp")

set(HTTP_INTERIM_RESPONSE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpInterimResponse.h")
set(HTTP_INTERIM_RESPONSE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/HttpInterimResponse.cpp")
set(HTTP1_INTERIM_RESPONSE_WRITER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1InterimResponseWriter.h")
set(HTTP1_INTERIM_RESPONSE_WRITER_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/server/Http1InterimResponseWriter.cpp")
set(HTTP_INTERIM_RESPONSE_VALIDATION
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpInterimResponseValidation.h")
set(WEB_CONTINUE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpContinueWriter.h")
set(HTTP2_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(HTTP2_INTERIM_RESPONSE_RECEIVER
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2ConnectionHeaders.cpp")
set(HTTP2_INTERIM_RESPONSE_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
if(NOT EXISTS "${HTTP_INTERIM_RESPONSE_HEADER}" OR
   NOT EXISTS "${HTTP_INTERIM_RESPONSE_SOURCE}" OR
   NOT EXISTS "${HTTP1_INTERIM_RESPONSE_WRITER}" OR
   NOT EXISTS "${HTTP1_INTERIM_RESPONSE_WRITER_SOURCE}" OR
   NOT EXISTS "${HTTP_INTERIM_RESPONSE_VALIDATION}" OR
   NOT EXISTS "${HTTP2_INTERIM_RESPONSE_RECEIVER}" OR
   NOT EXISTS "${HTTP2_INTERIM_RESPONSE_TEST}" OR
   NOT EXISTS "${WEB_CONTINUE_WRITER}")
    boundary_error("typed interim response head is missing"
        "non-switching 1xx needs typed HTTP/1 and HTTP/2 writers plus shared receive validation")
else()
    file(READ "${HTTP_INTERIM_RESPONSE_HEADER}" http_interim_response_header)
    file(READ "${HTTP_INTERIM_RESPONSE_SOURCE}" http_interim_response_source)
    file(READ "${HTTP1_INTERIM_RESPONSE_WRITER}" http1_interim_response_writer)
    file(READ "${HTTP1_INTERIM_RESPONSE_WRITER_SOURCE}" http1_interim_response_writer_source)
    file(READ "${HTTP_INTERIM_RESPONSE_VALIDATION}" http_interim_response_validation)
    file(READ "${WEB_CONTINUE_WRITER}" web_continue_writer)
    file(READ "${HTTP2_CONNECTION_HEADER}" http2_connection_header)
    file(READ "${HTTP2_RESPONSE_HEADERS}" http2_response_headers)
    file(READ "${HTTP2_INTERIM_RESPONSE_RECEIVER}"
        http2_interim_response_receiver)
    file(READ "${HTTP2_INTERIM_RESPONSE_TEST}"
        http2_interim_response_test)
    file(READ "${RUVIA_ROOT}/ruvia-http/src/HttpResponse.cpp" http_response_source)
    if(NOT http_interim_response_header MATCHES "HttpInterimResponseHead" OR
       NOT http_interim_response_header MATCHES "HeaderInit" OR
       NOT http_interim_response_source MATCHES "httpInterimStatusCodeValid" OR
       NOT http_response_source MATCHES "httpFinalStatusCodeValid" OR
       NOT http1_interim_response_writer MATCHES "Http1InterimResponseWriter" OR
       NOT http1_interim_response_writer MATCHES
           "enum class Http1InterimConnectionDisposition" OR
       NOT http1_interim_response_writer MATCHES "connectionDisposition[(][)]" OR
       http1_interim_response_writer MATCHES "requiresFinalConnectionClose|bool[ \t]+requiresFinalConnectionClose_" OR
       NOT http1_interim_response_writer MATCHES
           "bufferTooSmall[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_interim_response_writer MATCHES
           "prepared[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_interim_response_writer MATCHES
           "failure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_interim_response_writer_source MATCHES "kUpgradeConnectionOptionRequired" OR
       NOT http1_interim_response_writer_source MATCHES "headBuffer[.]size" OR
       NOT http_interim_response_validation MATCHES "kContentLengthForbidden" OR
       NOT http_interim_response_validation MATCHES "kTransferEncodingForbidden" OR
       NOT http_interim_response_validation MATCHES "kRepeatedSingleton" OR
       NOT http_interim_response_validation MATCHES
           "class HttpInterimResponseHeaderValidator final" OR
       NOT http_interim_response_validation MATCHES
           "validator[.]validate" OR
       NOT web_continue_writer MATCHES "Http1InterimResponseWriter" OR
       NOT web_continue_writer MATCHES "system_error" OR
       NOT http2_connection_header MATCHES "submitInterimResponseHead" OR
       NOT http2_response_headers MATCHES "appendHttp2InterimResponseHeaders" OR
       NOT http2_response_headers MATCHES "validateHttpInterimResponseHeaders" OR
       NOT http2_response_headers MATCHES "kInvalidHeader" OR
       NOT http2_interim_response_receiver MATCHES
           "HttpInterimResponseHeaderValidator interimHeaders" OR
       NOT http2_interim_response_receiver MATCHES
           "interimHeaders[.]validate" OR
       NOT http2_interim_response_test MATCHES
           "http2_connection_client_rejects_forbidden_interim_fields")
        boundary_error("interim/final response types have drifted"
            "typed 1xx must use one incremental field contract across exact HTTP/1 and HTTP/2 writers and the HTTP/2 client receiver")
    endif()
endif()
set(HTTP_RESPONSE_HEAD_BUFFER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadBuffer.h")
set(HTTP1_CHUNK_HEADER_BUFFER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedFraming.h")
set(HTTP2_OUTPUT_BUFFER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2OutputBuffer.h")
set(HTTP2_CONNECTION_BUFFER_API
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(WEBSOCKET_CLOSE_BUFFER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketUtils.h")
set(HTTP1_CHUNK_HEADER_BUFFER_TEST
    "${RUVIA_ROOT}/tests/unit_http1_chunked_framing.cpp")
set(HTTP_RESPONSE_HEAD_BUFFER_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_buffer.cpp")
set(HTTP2_OUTPUT_BUFFER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_output_buffer.cpp")
set(HTTP2_CONNECTION_BUFFER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(WEBSOCKET_CLOSE_BUFFER_TEST
    "${RUVIA_ROOT}/tests/unit_websocket_close.cpp")
set(PROTOCOL_BUFFER_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(protocol_buffer_contract IN ITEMS
        "${HTTP_RESPONSE_HEAD_BUFFER}"
        "${HTTP1_CHUNK_HEADER_BUFFER}"
        "${HTTP2_OUTPUT_BUFFER}"
        "${HTTP2_CONNECTION_BUFFER_API}"
        "${WEBSOCKET_CLOSE_BUFFER}"
        "${HTTP1_CHUNK_HEADER_BUFFER_TEST}"
        "${HTTP_RESPONSE_HEAD_BUFFER_TEST}"
        "${HTTP2_OUTPUT_BUFFER_TEST}"
        "${HTTP2_CONNECTION_BUFFER_TEST}"
        "${WEBSOCKET_CLOSE_BUFFER_TEST}"
        "${PROTOCOL_BUFFER_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${protocol_buffer_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${protocol_buffer_contract}")
        boundary_error("protocol buffer lifetime contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(NOT EXISTS "${HTTP_RESPONSE_HEAD_BUFFER}")
    boundary_error("HTTP response-head scratch buffer is missing"
        "the HTTP/1 writer needs an owned fixed/heap storage state")
else()
    file(READ "${HTTP_RESPONSE_HEAD_BUFFER}" http_response_head_buffer)
    if(NOT http_response_head_buffer MATCHES
           "std::variant<StackState, HeapState>" OR
       http_response_head_buffer MATCHES "overflowed_")
        boundary_error("HTTP response-head storage lost its exclusive state"
            "fixed and heap storage must use one discriminated authority")
    endif()
endif()
if(EXISTS "${HTTP1_CHUNK_HEADER_BUFFER}" AND
   EXISTS "${HTTP_RESPONSE_HEAD_BUFFER}" AND
   EXISTS "${HTTP2_OUTPUT_BUFFER}" AND
   EXISTS "${HTTP2_CONNECTION_BUFFER_API}" AND
   EXISTS "${WEBSOCKET_CLOSE_BUFFER}" AND
   EXISTS "${HTTP1_CHUNK_HEADER_BUFFER_TEST}" AND
   EXISTS "${HTTP_RESPONSE_HEAD_BUFFER_TEST}" AND
   EXISTS "${HTTP2_OUTPUT_BUFFER_TEST}" AND
   EXISTS "${HTTP2_CONNECTION_BUFFER_TEST}" AND
   EXISTS "${WEBSOCKET_CLOSE_BUFFER_TEST}" AND
   EXISTS "${PROTOCOL_BUFFER_PACKAGE_CONSUMER}")
    file(READ "${HTTP1_CHUNK_HEADER_BUFFER}"
        http1_chunk_header_buffer)
    file(READ "${HTTP2_OUTPUT_BUFFER}" http2_output_buffer)
    file(READ "${HTTP2_CONNECTION_BUFFER_API}"
        http2_connection_buffer_api)
    file(READ "${WEBSOCKET_CLOSE_BUFFER}" websocket_close_buffer)
    file(READ "${HTTP1_CHUNK_HEADER_BUFFER_TEST}"
        http1_chunk_header_buffer_test)
    file(READ "${HTTP_RESPONSE_HEAD_BUFFER_TEST}"
        http_response_head_buffer_test)
    file(READ "${HTTP2_OUTPUT_BUFFER_TEST}"
        http2_output_buffer_test)
    file(READ "${HTTP2_CONNECTION_BUFFER_TEST}"
        http2_connection_buffer_test)
    file(READ "${WEBSOCKET_CLOSE_BUFFER_TEST}"
        websocket_close_buffer_test)
    file(READ "${PROTOCOL_BUFFER_PACKAGE_CONSUMER}"
        protocol_buffer_package_consumer)
    if(NOT http1_chunk_header_buffer MATCHES
           "view[(][)] const &[ 	]+noexcept" OR
       NOT http1_chunk_header_buffer MATCHES
           "view[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT http_response_head_buffer MATCHES
           "view[(][)] const &[ 	]+noexcept" OR
       NOT http_response_head_buffer MATCHES
           "view[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT http_response_head_buffer MATCHES
           "stackCursor[(]std::size_t bound[)] &[ 	]+noexcept" OR
       NOT http_response_head_buffer MATCHES
           "stackCursor[(]std::size_t[)][ 	]*&&[ 	]*=[ 	]*delete" OR
       NOT http2_output_buffer MATCHES
           "pending[(][)] const &[ 	]+noexcept" OR
       NOT http2_output_buffer MATCHES
           "pending[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT http2_connection_buffer_api MATCHES
           "pendingOutput[(][)] const &[ 	]+noexcept" OR
       NOT http2_connection_buffer_api MATCHES
           "pendingOutput[(][)] const &&[ 	]*=[ 	]*delete" OR
       NOT http2_connection_buffer_api MATCHES
           "takeDrainedDataStreams[(][)] &[ 	]+noexcept" OR
       NOT http2_connection_buffer_api MATCHES
           "takeDrainedDataStreams[(][)] &&[ 	]*=[ 	]*delete" OR
       NOT websocket_close_buffer MATCHES
           "bytes[(][)] const &[ 	]+noexcept" OR
       NOT websocket_close_buffer MATCHES
           "bytes[(][)] const &&[ 	]*=[ 	]*delete")
        boundary_error("protocol buffers expose storage from temporary owners"
            "H1, H2, response-head, and WebSocket owned byte buffers must expose borrows only from stable lvalues")
    endif()
    foreach(protocol_buffer_lifetime_test IN ITEMS
            "${http1_chunk_header_buffer_test}"
            "${http_response_head_buffer_test}"
            "${http2_output_buffer_test}"
            "${http2_connection_buffer_test}"
            "${websocket_close_buffer_test}")
        if(NOT protocol_buffer_lifetime_test MATCHES
               "ExposesRvalue(Http1ChunkHeaderView|ResponseHeadBufferStorage|Http2OutputBuffer|Http2ConnectionStorage|EncodedClosePayloadBytes)")
            boundary_error("protocol buffer temporary-owner coverage is incomplete"
                "each owning protocol buffer requires a direct rvalue-borrow probe")
            break()
        endif()
    endforeach()
    foreach(protocol_buffer_probe IN ITEMS
            "ExposesRvalueHttp1ChunkHeaderView"
            "ExposesRvalueResponseHeadBufferStorage"
            "ExposesRvalueHttp2OutputBuffer"
            "ExposesRvalueHttp2ConnectionStorage"
            "ExposesRvalueEncodedClosePayloadBytes")
        if(NOT protocol_buffer_package_consumer MATCHES
               "static_assert[(]!${protocol_buffer_probe}<")
            boundary_error("installed protocol buffer lifetime coverage is incomplete"
                "tests/package-consumer/http.cpp must reject every temporary owner")
            break()
        endif()
    endforeach()
endif()
set(HTTP2_HEADER_LIST_OWNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2HeaderList.h")
set(HTTP2_STREAM_REQUEST_DATA_OWNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamRequestData.h")
set(HTTP2_STREAM_REQUEST_STATE_OWNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamRequestState.h")
set(HTTP2_STREAM_HEADER_BLOCKS_OWNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamHeaderBlocks.h")
set(HTTP2_STREAM_LIFECYCLE_OWNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamLifecycle.h")
set(HTTP2_STREAM_STATE_OWNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(HTTP2_STREAM_TABLE_OWNER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamTable.h")
set(HTTP2_HEADER_LIST_OWNER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_header_list.cpp")
set(HTTP2_STREAM_REQUEST_DATA_OWNER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_stream_request_data.cpp")
set(HTTP2_STREAM_REQUEST_STATE_OWNER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_request_headers.cpp")
set(HTTP2_STREAM_LIFECYCLE_OWNER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_stream_lifecycle.cpp")
set(HTTP2_STREAM_TABLE_OWNER_TEST
    "${RUVIA_ROOT}/tests/unit_http2_stream_table.cpp")
foreach(http2_stream_owner_contract IN ITEMS
        "${HTTP2_HEADER_LIST_OWNER}"
        "${HTTP2_STREAM_REQUEST_DATA_OWNER}"
        "${HTTP2_STREAM_REQUEST_STATE_OWNER}"
        "${HTTP2_STREAM_HEADER_BLOCKS_OWNER}"
        "${HTTP2_STREAM_LIFECYCLE_OWNER}"
        "${HTTP2_STREAM_STATE_OWNER}"
        "${HTTP2_STREAM_TABLE_OWNER}"
        "${HTTP2_CONNECTION_BUFFER_API}"
        "${HTTP2_HEADER_LIST_OWNER_TEST}"
        "${HTTP2_STREAM_REQUEST_DATA_OWNER_TEST}"
        "${HTTP2_STREAM_REQUEST_STATE_OWNER_TEST}"
        "${HTTP2_STREAM_LIFECYCLE_OWNER_TEST}"
        "${HTTP2_STREAM_TABLE_OWNER_TEST}"
        "${HTTP2_CONNECTION_BUFFER_TEST}"
        "${PROTOCOL_BUFFER_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${http2_stream_owner_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${http2_stream_owner_contract}")
        boundary_error("HTTP/2 stream-owner lifetime contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP2_HEADER_LIST_OWNER}" AND
   EXISTS "${HTTP2_STREAM_REQUEST_DATA_OWNER}" AND
   EXISTS "${HTTP2_STREAM_REQUEST_STATE_OWNER}" AND
   EXISTS "${HTTP2_STREAM_HEADER_BLOCKS_OWNER}" AND
   EXISTS "${HTTP2_STREAM_LIFECYCLE_OWNER}" AND
   EXISTS "${HTTP2_STREAM_STATE_OWNER}" AND
   EXISTS "${HTTP2_STREAM_TABLE_OWNER}" AND
   EXISTS "${HTTP2_CONNECTION_BUFFER_API}" AND
   EXISTS "${HTTP2_HEADER_LIST_OWNER_TEST}" AND
   EXISTS "${HTTP2_STREAM_REQUEST_DATA_OWNER_TEST}" AND
   EXISTS "${HTTP2_STREAM_REQUEST_STATE_OWNER_TEST}" AND
   EXISTS "${HTTP2_STREAM_LIFECYCLE_OWNER_TEST}" AND
   EXISTS "${HTTP2_STREAM_TABLE_OWNER_TEST}" AND
   EXISTS "${HTTP2_CONNECTION_BUFFER_TEST}" AND
   EXISTS "${PROTOCOL_BUFFER_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_HEADER_LIST_OWNER}" http2_header_list_owner)
    file(READ "${HTTP2_STREAM_REQUEST_DATA_OWNER}"
        http2_stream_request_data_owner)
    file(READ "${HTTP2_STREAM_REQUEST_STATE_OWNER}"
        http2_stream_request_state_owner)
    file(READ "${HTTP2_STREAM_HEADER_BLOCKS_OWNER}"
        http2_stream_header_blocks_owner)
    file(READ "${HTTP2_STREAM_LIFECYCLE_OWNER}"
        http2_stream_lifecycle_owner)
    file(READ "${HTTP2_STREAM_STATE_OWNER}" http2_stream_state_owner)
    file(READ "${HTTP2_STREAM_TABLE_OWNER}" http2_stream_table_owner)
    file(READ "${HTTP2_CONNECTION_BUFFER_API}"
        http2_stream_connection_owner)
    file(READ "${PROTOCOL_BUFFER_PACKAGE_CONSUMER}"
        http2_stream_owner_package_consumer)
    if(NOT http2_header_list_owner MATCHES
           "at[(][^)]*[)][\\r\\n \\t]*const &[\\r\\n \\t]+noexcept" OR
       NOT http2_header_list_owner MATCHES
           "at[(][^)]*[)][\\r\\n \\t]*const &&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
        boundary_error("HTTP/2 header list exposes views from temporary owners"
            "Http2HeaderList::at must require a stable lvalue")
    endif()
    foreach(http2_request_data_borrow IN ITEMS
            method scheme authority path protocol cookie headerAt)
        if(NOT http2_stream_request_data_owner MATCHES
               "${http2_request_data_borrow}[(][^)]*[)][\\r\\n \\t]*const &[\\r\\n \\t]+noexcept" OR
           NOT http2_stream_request_data_owner MATCHES
               "${http2_request_data_borrow}[(][^)]*[)][\\r\\n \\t]*const &&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
            boundary_error("HTTP/2 request data exposes views from temporary owners"
                "${http2_request_data_borrow} must require a stable lvalue")
            break()
        endif()
    endforeach()
    if(NOT http2_stream_request_state_owner MATCHES
           "responseStatus[(][)][\\r\\n \\t]*const &[\\r\\n \\t]+noexcept" OR
       NOT http2_stream_request_state_owner MATCHES
           "responseStatus[(][)][\\r\\n \\t]*const &&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
        boundary_error("HTTP/2 request state exposes pointers from temporary owners"
            "responseStatus must require a stable lvalue")
    endif()
    foreach(http2_header_block_borrow IN ITEMS request response)
        if(NOT http2_stream_header_blocks_owner MATCHES
               "${http2_header_block_borrow}[(][)][\\r\\n \\t]*&[\\r\\n \\t]+noexcept" OR
           NOT http2_stream_header_blocks_owner MATCHES
               "${http2_header_block_borrow}[(][)][\\r\\n \\t]*&&[\\r\\n \\t]*=[\\r\\n \\t]*delete" OR
           NOT http2_stream_header_blocks_owner MATCHES
               "${http2_header_block_borrow}[(][)][\\r\\n \\t]*const &[\\r\\n \\t]+noexcept" OR
           NOT http2_stream_header_blocks_owner MATCHES
               "${http2_header_block_borrow}[(][)][\\r\\n \\t]*const &&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
            boundary_error("HTTP/2 header blocks expose references from temporary owners"
                "${http2_header_block_borrow} must require a stable lvalue")
            break()
        endif()
    endforeach()
    foreach(http2_lifecycle_borrow IN ITEMS localSend remoteReceive)
        if(NOT http2_stream_lifecycle_owner MATCHES
               "${http2_lifecycle_borrow}[(][)][\\r\\n \\t]*const &[\\r\\n \\t]+noexcept" OR
           NOT http2_stream_lifecycle_owner MATCHES
               "${http2_lifecycle_borrow}[(][)][\\r\\n \\t]*const &&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
            boundary_error("HTTP/2 lifecycle exposes references from temporary owners"
                "${http2_lifecycle_borrow} must require a stable lvalue")
            break()
        endif()
    endforeach()
    foreach(http2_stream_mutable_borrow IN ITEMS
            receiveWindowCredit requestHeaderBlock responseHeaderBlock)
        if(NOT http2_stream_state_owner MATCHES
               "${http2_stream_mutable_borrow}[(][^)]*[)][\\r\\n \\t]*&[\\r\\n \\t]+noexcept" OR
           NOT http2_stream_state_owner MATCHES
               "${http2_stream_mutable_borrow}[(][^)]*[)][\\r\\n \\t]*&&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
            boundary_error("HTTP/2 stream exposes mutable storage from temporary owners"
                "${http2_stream_mutable_borrow} must require a stable lvalue")
            break()
        endif()
    endforeach()
    foreach(http2_stream_const_borrow IN ITEMS
            requestHeaderBlock responseHeaderBlock remoteContent localContent
            localSend remoteReceive requestMethod requestAuthority requestPath
            requestProtocol requestCookie requestHeaderAt requestScheme tunnel
            responseStatus)
        if(NOT http2_stream_state_owner MATCHES
               "${http2_stream_const_borrow}[(][^)]*[)][\\r\\n \\t]*const &[\\r\\n \\t]+noexcept" OR
           NOT http2_stream_state_owner MATCHES
               "${http2_stream_const_borrow}[(][^)]*[)][\\r\\n \\t]*const &&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
            boundary_error("HTTP/2 stream exposes storage from temporary owners"
                "${http2_stream_const_borrow} must require a stable lvalue")
            break()
        endif()
    endforeach()
    if(NOT http2_stream_table_owner MATCHES
           "find[(][^)]*[)][\\r\\n \\t]*&[\\r\\n \\t]+noexcept" OR
       NOT http2_stream_table_owner MATCHES
           "find[(][^)]*[)][\\r\\n \\t]*&&[\\r\\n \\t]*=[\\r\\n \\t]*delete" OR
       NOT http2_stream_table_owner MATCHES
           "find[(][^)]*[)][\\r\\n \\t]*const &[\\r\\n \\t]+noexcept" OR
       NOT http2_stream_table_owner MATCHES
           "find[(][^)]*[)][\\r\\n \\t]*const &&[\\r\\n \\t]*=[\\r\\n \\t]*delete" OR
       NOT http2_stream_table_owner MATCHES
           "create[(][^)]*[)][\\r\\n \\t]*&[\\r\\n \\t]*[{]" OR
       NOT http2_stream_table_owner MATCHES
           "create[(][^)]*[)][\\r\\n \\t]*&&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
        boundary_error("HTTP/2 stream table exposes pointers from temporary owners"
            "find and create must require a stable lvalue")
    endif()
    if(NOT http2_stream_connection_owner MATCHES
           "stream[(][^)]*[)][\\r\\n \\t]*&[\\r\\n \\t]+noexcept" OR
       NOT http2_stream_connection_owner MATCHES
           "stream[(][^)]*[)][\\r\\n \\t]*&&[\\r\\n \\t]*=[\\r\\n \\t]*delete")
        boundary_error("HTTP/2 connection exposes streams from temporary owners"
            "Http2Connection::stream must require a stable lvalue")
    endif()
    foreach(http2_stream_owner_test IN ITEMS
            "${HTTP2_HEADER_LIST_OWNER_TEST}"
            "${HTTP2_STREAM_REQUEST_DATA_OWNER_TEST}"
            "${HTTP2_STREAM_REQUEST_STATE_OWNER_TEST}"
            "${HTTP2_STREAM_LIFECYCLE_OWNER_TEST}"
            "${HTTP2_STREAM_TABLE_OWNER_TEST}"
            "${HTTP2_CONNECTION_BUFFER_TEST}")
        file(READ "${http2_stream_owner_test}" http2_stream_owner_test_content)
        if(NOT http2_stream_owner_test_content MATCHES
               "ExposesRvalueHttp2(HeaderList|StreamRequestData|StreamRequestState|StreamHeaderBlocks|StreamLifecycle|StreamState|StreamTable|Connection)Storage")
            boundary_error("HTTP/2 stream-owner direct coverage is incomplete"
                "each owner layer requires an rvalue-borrow compile probe")
            break()
        endif()
    endforeach()
    foreach(http2_stream_owner_probe IN ITEMS
            "ExposesRvalueHttp2HeaderListStorage"
            "ExposesRvalueHttp2StreamRequestDataStorage"
            "ExposesRvalueHttp2StreamRequestStateStorage"
            "ExposesRvalueHttp2StreamHeaderBlocksStorage"
            "ExposesRvalueHttp2StreamLifecycleStorage"
            "ExposesRvalueHttp2StreamStateStorage"
            "ExposesRvalueHttp2StreamTableStorage"
            "ExposesRvalueHttp2ConnectionStorage")
        if(NOT http2_stream_owner_package_consumer MATCHES
               "static_assert[(]!${http2_stream_owner_probe}<")
            boundary_error("installed HTTP/2 stream-owner coverage is incomplete"
                "tests/package-consumer/http.cpp must reject every temporary owner")
            break()
        endif()
    endforeach()
endif()
check_files_no_match("obsolete untyped informational response submit API was restored"
    "submitInformationalResponseHead"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
check_files_no_match("raw HTTP/1 100 Continue bytes escaped the protocol writer"
    "HTTP/1[.]1 100 Continue"
    ${WEB_SOURCE})
check_files_no_match("obsolete raw HTTP/1 continue response constant was restored"
    "kHttp1ContinueResponse|writeContinue[(]"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpContinueWriter.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl")

set(WEB_BUFFERED_RESPONSE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpBufferedResponse.h")
if(EXISTS "${WEB_BUFFERED_RESPONSE}")
    file(READ "${WEB_BUFFERED_RESPONSE}" web_buffered_response)
    if(NOT web_buffered_response MATCHES "HttpBufferedResponseWritePlan")
        boundary_error("ruvia-web buffered response bypasses the HTTP-owned write plan"
            "HttpBufferedResponse.h must carry HttpBufferedResponseWritePlan")
    endif()
endif()

set(WEB_H2_BUFFERED_WRITER
    "${RUVIA_ROOT}/ruvia-web/src/server/Http2BufferedResponseWrite.cpp")
if(EXISTS "${WEB_H2_BUFFERED_WRITER}")
    file(READ "${WEB_H2_BUFFERED_WRITER}" web_h2_buffered_writer)
    if(NOT web_h2_buffered_writer MATCHES "submittedHead[-][>]sendBody" OR
       NOT web_h2_buffered_writer MATCHES "headResult[.]submitted[(][)]" OR
       NOT web_h2_buffered_writer MATCHES "headResult[.]failure[(][)][-][>]peerClosed[(][)]" OR
       web_h2_buffered_writer MATCHES "Http2ResponseHeadSubmitError::|failure[(][)][-][>]error[(][)]" OR
       web_h2_buffered_writer MATCHES "submittedHead[-][>]plan[(][)]" OR
       NOT web_h2_buffered_writer MATCHES "Http2ErrorCode::kInternalError")
        boundary_error("ruvia-web HTTP/2 runtime bypasses the HTTP-owned send-body verdict"
            "Http2BufferedResponseWrite.cpp must consume only a submitted plan and terminate typed final-head failures")
    endif()
endif()

set(WEB_H2_STREAM_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
if(EXISTS "${WEB_H2_STREAM_SINK}")
    file(READ "${WEB_H2_STREAM_SINK}" web_h2_stream_sink)
    if(NOT web_h2_stream_sink MATCHES "headResult[.]submitted[(][)]" OR
       NOT web_h2_stream_sink MATCHES "headResult[.]failure[(][)][-][>]peerClosed[(][)]" OR
       NOT web_h2_stream_sink MATCHES "headResult[.]failure[(][)][-][>]exception[(][)]" OR
       web_h2_stream_sink MATCHES "Http2ResponseHeadSubmitError::|failure[(][)][-][>]error[(][)]" OR
       NOT web_h2_stream_sink MATCHES
           "markCommitted[(][*]submittedHead[)]")
        boundary_error("ruvia-web HTTP/2 streaming sink bypasses the submitted-head plan"
            "the sink must distinguish typed failure before committing the successful streaming plan")
    endif()
endif()

set(HTTP2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(HTTP2_LOCAL_SEND_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalSendState.h")
set(HTTP2_STREAM_CLOSE_SOURCE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamCloseSource.h")
set(HTTP2_CLOSED_STREAMS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ClosedStreams.h")
set(HTTP2_STREAM_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamLifecycle.h")
set(HTTP2_STREAM_TABLE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamTable.h")
set(HTTP2_LOCAL_CONTENT_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalContentState.h")
set(HTTP2_REMOTE_CONTENT_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteContentState.h")
set(HTTP2_REMOTE_RECEIVE_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteReceiveState.h")
set(HTTP2_REMOTE_RECEIVE_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RemoteReceiveSemantics.h")
set(HTTP2_STALE_BODY_ACCOUNTING
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamBodyAccounting.h")
set(HTTP2_STALE_WEB_RUNTIME_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyState.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2BodyQueue.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamBodyQueue.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamBodyPolicy.h")
set(HTTP2_REQUEST_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h")
set(HTTP2_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(HTTP2_REQUEST_CONTENT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestContent.h")
set(HTTP2_TUNNEL_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2TunnelState.h")
set(HTTP2_LOCAL_SETTINGS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalSettings.h")
set(HTTP2_PEER_SETTINGS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2PeerSettings.h")
if(NOT EXISTS "${HTTP_RESPONSE_CONTENT_SEMANTICS}" OR
   NOT EXISTS "${HTTP_RESPONSE_WRITE_PLAN}" OR
   NOT EXISTS "${HTTP1_CLIENT_RESPONSE_SOURCE}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("shared response-content semantics contract is missing"
        "HTTP/1 client, HTTP/2 client, and response writers must consume one method/status classification")
else()
    file(READ "${HTTP_RESPONSE_CONTENT_SEMANTICS}"
        http_response_content_semantics)
    file(READ "${HTTP_RESPONSE_WRITE_PLAN}" http_response_write_plan)
    file(READ "${HTTP1_CLIENT_RESPONSE_SOURCE}"
        http1_shared_response_semantics)
    read_http2_connection_implementation(http2_shared_response_semantics)
    if(NOT http_response_content_semantics MATCHES
           "enum class HttpResponseContentSemantics : std::uint8_t" OR
       NOT http_response_content_semantics MATCHES "kInformational" OR
       NOT http_response_content_semantics MATCHES "kProtocolSwitch" OR
       NOT http_response_content_semantics MATCHES "kConnectTunnel" OR
       NOT http_response_content_semantics MATCHES "kWithoutContent" OR
       NOT http_response_content_semantics MATCHES "kWithContent" OR
       http_response_content_semantics MATCHES
           "std::variant|std::holds_alternative|HttpInformationalResponseContent|bool withContent[(]" OR
       NOT http_response_content_semantics MATCHES
           "httpResponseContentSemantics" OR
       NOT http1_shared_response_semantics MATCHES
           "detail::httpResponseContentSemantics" OR
       NOT http2_shared_response_semantics MATCHES
           "httpResponseContentSemantics" OR
       NOT http_response_write_plan MATCHES
           "HttpResponseContentSemantics semantics_" OR
       NOT http_response_write_plan MATCHES
           "HttpResponseContentSemantics[ \t\r\n]+contentSemantics[(][)] const noexcept" OR
       NOT http_response_write_plan MATCHES
           "HttpResponseBodyPlan bodyPlan[(][)] const noexcept" OR
       NOT http_response_write_plan MATCHES
           "is_trivially_copyable_v<HttpResponseBodyPlan>" OR
       NOT http_response_write_plan MATCHES
           "sizeof[(]HttpResponseBodyPlan[)] <= 12" OR
       NOT http_response_write_plan MATCHES
           "semantics_[ 	]*!=[ 	]*HttpResponseContentSemantics::kWithContent" OR
       http_response_write_plan MATCHES "bodySuppressed_")
        boundary_error("response content semantics split by protocol direction"
            "informational, switch, CONNECT, without-content, and with-content alternatives must drive H1 client, H2 client, and server body plans as cheap value facts")
    endif()
endif()
if(NOT EXISTS "${HTTP_REQUEST_CONTENT_SEMANTICS}" OR
   NOT EXISTS "${HTTP1_CLIENT_REQUEST_SEMANTICS_SOURCE}" OR
   NOT EXISTS "${HTTP2_CLIENT_REQUEST_SEMANTICS_SOURCE}")
    boundary_error("shared request-content semantics contract is missing"
        "HTTP/1 and HTTP/2 client writers must consume one sender-side method/content classification")
else()
    file(READ "${HTTP_REQUEST_CONTENT_SEMANTICS}"
        http_request_content_semantics)
    file(READ "${HTTP1_CLIENT_REQUEST_SEMANTICS_SOURCE}"
        http1_client_request_semantics)
    file(READ "${HTTP2_CLIENT_REQUEST_SEMANTICS_SOURCE}"
        http2_client_request_semantics)
    if(NOT http_request_content_semantics MATCHES
           "enum class HttpRequestContentSemantics : std::uint8_t" OR
       NOT http_request_content_semantics MATCHES
           "kNoAdditionalRequirements" OR
       NOT http_request_content_semantics MATCHES "kForbidden" OR
       NOT http_request_content_semantics MATCHES "kContentTypeRequired" OR
       NOT http_request_content_semantics MATCHES
           "httpRequestContentSemantics" OR
       NOT http1_client_request_semantics MATCHES
           "detail::httpRequestContentSemantics" OR
       NOT http2_client_request_semantics MATCHES
           "httpRequestContentSemantics")
        boundary_error("request content semantics split by HTTP version"
            "TRACE content prohibition and OPTIONS Content-Type requirements must drive both client serializers")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SEND_STATE}" OR
   NOT EXISTS "${HTTP2_STREAM_CLOSE_SOURCE}" OR
   NOT EXISTS "${HTTP2_CLOSED_STREAMS}" OR
   NOT EXISTS "${HTTP2_STREAM_LIFECYCLE}" OR
   NOT EXISTS "${HTTP2_STREAM_TABLE}" OR
   NOT EXISTS "${HTTP2_STREAM_STATE}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("HTTP/2 local send state is missing"
        "local frame permission must be owned by one installed discriminated state")
else()
    file(READ "${HTTP2_LOCAL_SEND_STATE}" http2_local_send_state)
    file(READ "${HTTP2_STREAM_CLOSE_SOURCE}" http2_stream_close_source)
    file(READ "${HTTP2_CLOSED_STREAMS}" http2_closed_streams)
    file(READ "${HTTP2_STREAM_LIFECYCLE}" http2_stream_lifecycle)
    file(READ "${HTTP2_STREAM_TABLE}" http2_stream_table)
    file(READ "${HTTP2_STREAM_STATE}" http2_local_send_stream_state)
    read_http2_connection_implementation(http2_local_send_connection)
    if(NOT http2_stream_close_source MATCHES
           "enum class Http2StreamCloseSource" OR
       http2_stream_close_source MATCHES "kNone" OR
       NOT http2_stream_close_source MATCHES "kPeerGoaway" OR
       NOT http2_stream_close_source MATCHES
           "http2IsValidStreamCloseSource" OR
       NOT http2_closed_streams MATCHES
           "std::optional<Http2StreamCloseSource>" OR
       NOT http2_closed_streams MATCHES "return std::nullopt" OR
       http2_closed_streams MATCHES "Http2StreamCloseSource::kNone" OR
       NOT http2_local_send_state MATCHES
           "Http2StreamCloseSource[.]h" OR
       NOT http2_local_send_state MATCHES
           "http2IsValidStreamCloseSource[(]source[)]" OR
       NOT http2_local_send_state MATCHES
           "private:[\r\n \t]+friend class Http2StreamLifecycle" OR
       http2_local_send_state MATCHES
           "enum class Http2StreamCloseSource" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalHeadPending final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalRequestContentOpen final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalResponseContentOpen final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalResponseTrailersOnly final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalConnectPending final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalTunnelOpen final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalEndStreamQueued final" OR
       NOT http2_local_send_state MATCHES
           "class Http2LocalEndStreamCommitted final" OR
       NOT http2_local_send_state MATCHES
           "class Http2StreamAborted final" OR
       NOT http2_local_send_state MATCHES "using State = std::variant" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalHeadPending>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalRequestContentOpen>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalResponseContentOpen>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalResponseTrailersOnly>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalConnectPending>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalTunnelOpen>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalEndStreamQueued>" OR
       NOT http2_local_send_state MATCHES
           "std::get_if<Http2LocalEndStreamCommitted>" OR
       NOT http2_local_send_state MATCHES "std::get_if<Http2StreamAborted>" OR
       NOT http2_local_send_state MATCHES
           "Http2StreamCloseSource source[(][)] const noexcept" OR
       NOT http2_stream_lifecycle MATCHES
           "const Http2LocalSendState& localSend[(][)] const & noexcept" OR
       NOT http2_stream_lifecycle MATCHES
           "private:[\r\n \t]+friend class Http2StreamState" OR
       NOT http2_stream_lifecycle MATCHES
           "bool aborted[(][)] const noexcept" OR
       NOT http2_stream_lifecycle MATCHES
           "bool abort[(]Http2StreamCloseSource source[)] noexcept" OR
       NOT http2_stream_lifecycle MATCHES "queued_ = false" OR
       NOT http2_local_send_stream_state MATCHES
           "const Http2LocalSendState& localSend[(][)] const & noexcept" OR
       NOT http2_local_send_stream_state MATCHES
           "bool isAborted[(][)] const noexcept" OR
       NOT http2_local_send_stream_state MATCHES
           "bool abort[(]Http2StreamCloseSource source[)] noexcept" OR
       NOT http2_stream_table MATCHES "void removeAborted" OR
       NOT http2_local_send_connection MATCHES "beginLocalRequestContent" OR
       NOT http2_local_send_connection MATCHES "beginLocalResponseContent" OR
       NOT http2_local_send_connection MATCHES
           "beginLocalResponseTrailersOnly" OR
       NOT http2_local_send_connection MATCHES "beginLocalConnectRequest" OR
       NOT http2_local_send_connection MATCHES "openLocalConnectTunnel" OR
       NOT http2_local_send_connection MATCHES "queueLocalEndStream" OR
       NOT http2_local_send_connection MATCHES "commitLocalEndStream")
        boundary_error("HTTP/2 local send lifecycle lost its discriminated state"
            "head, request/response content, trailers, CONNECT, queued/committed END_STREAM, and whole-stream abort must remain exclusive; only abort owns a real close source and it must atomically clear queue ownership")
    endif()
endif()
if(NOT EXISTS "${HTTP2_REMOTE_RECEIVE_STATE}" OR
   NOT EXISTS "${HTTP2_REMOTE_RECEIVE_SEMANTICS}" OR
   NOT EXISTS "${HTTP2_STREAM_LIFECYCLE}" OR
   NOT EXISTS "${HTTP2_STREAM_STATE}" OR
   NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("HTTP/2 remote receive state is missing"
        "remote HEADERS, content, CONNECT, tunnel, END_STREAM, and abort permission must be one installed discriminated state")
else()
    file(READ "${HTTP2_REMOTE_RECEIVE_STATE}" http2_remote_receive_state)
    file(READ "${HTTP2_REMOTE_RECEIVE_SEMANTICS}"
        http2_remote_receive_semantics)
    file(READ "${HTTP2_STREAM_LIFECYCLE}" http2_remote_receive_lifecycle)
    file(READ "${HTTP2_STREAM_STATE}" http2_remote_receive_stream)
    read_http2_connection_implementation(http2_remote_receive_connection)
    if(NOT http2_remote_receive_state MATCHES
           "private:[\r\n \t]+friend class Http2StreamLifecycle" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteHeadPending final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteHeadEndStreamPending final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteContentOpen final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteConnectPending final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteConnectPendingEndStream final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteConnectRejectedAwaitingEndStream final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteTunnelOpen final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteEndStream final" OR
       NOT http2_remote_receive_state MATCHES
           "class Http2RemoteAborted final" OR
       NOT http2_remote_receive_state MATCHES "using State = std::variant" OR
       NOT http2_remote_receive_state MATCHES
           "std::get_if<Http2RemoteHeadPending>" OR
       NOT http2_remote_receive_state MATCHES
           "std::get_if<Http2RemoteConnectRejectedAwaitingEndStream>" OR
       NOT http2_remote_receive_state MATCHES
           "std::get_if<Http2RemoteEndStream>" OR
       NOT http2_remote_receive_lifecycle MATCHES
           "const Http2RemoteReceiveState& remoteReceive[(][)] const & noexcept" OR
       NOT http2_remote_receive_lifecycle MATCHES "remoteReceive_[.]abort[(][)]" OR
       NOT http2_remote_receive_stream MATCHES
           "const Http2RemoteReceiveState&[ \t\r\n]+remoteReceive[(][)] const & noexcept" OR
       NOT http2_remote_receive_stream MATCHES "finalizeRemoteConnectHead" OR
       NOT http2_remote_receive_stream MATCHES "finishRemotePendingConnect" OR
       NOT http2_remote_receive_stream MATCHES "finishRemoteRejectedConnect" OR
       NOT http2_remote_receive_semantics MATCHES
           "inline bool http2RemoteFinalHeadDecoded" OR
       NOT http2_remote_receive_semantics MATCHES
           "inline bool http2RemotePeerHalfClosed" OR
       NOT http2_remote_receive_connection MATCHES
           "http2RemoteFinalHeadDecoded" OR
       NOT http2_remote_receive_connection MATCHES
           "http2RemotePeerHalfClosed" OR
       NOT http2_remote_receive_connection MATCHES
           "connectRejectedAwaitingEndStream" OR
       NOT http2_remote_receive_connection MATCHES
           "finishRemoteRejectedConnect" OR
       NOT http2_remote_receive_connection MATCHES
           "remote[.]tunnelOpen[(][)]")
        boundary_error("HTTP/2 remote receive lifecycle lost its discriminated state"
            "final-head decoding, content/trailer DATA, CONNECT decisions, tunnel flow control, normal peer half-close, and whole-stream abort must remain exclusive and stream-owned")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_CONTENT_STATE}")
    boundary_error("HTTP/2 local content state is missing"
        "Http2LocalContentState.h must own outbound response length accounting")
elseif(NOT EXISTS "${HTTP2_STREAM_STATE}")
    boundary_error("HTTP/2 stream state is missing"
        "Http2StreamState.h must expose the const local-content contract")
else()
    file(READ "${HTTP2_LOCAL_CONTENT_STATE}" http2_local_content_state)
    file(READ "${HTTP2_STREAM_STATE}" http2_stream_state)
    if(NOT http2_local_content_state MATCHES
           "class Http2LocalContentUnset final" OR
       NOT http2_local_content_state MATCHES
           "class Http2LocalContentForbidden final" OR
       NOT http2_local_content_state MATCHES
           "class Http2LocalContentUnbounded final" OR
       NOT http2_local_content_state MATCHES
           "class Http2LocalContentKnownLength final" OR
       NOT http2_local_content_state MATCHES "using Content = std::variant" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentUnset>" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentForbidden>" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentUnbounded>" OR
       NOT http2_local_content_state MATCHES
           "std::get_if<Http2LocalContentKnownLength>" OR
       NOT http2_local_content_state MATCHES "kNotStarted" OR
       NOT http2_local_content_state MATCHES "if [(]unset[(][)] != nullptr[)]" OR
       NOT http2_stream_state MATCHES
           "const Http2LocalContentState&[ \t\r\n]+localContent[(][)] const & noexcept" OR
       NOT http2_stream_state MATCHES
           "HttpRequestExpectations requestExpectations[(][)] const noexcept" OR
       http2_stream_state MATCHES
           "const HttpRequestExpectations& requestExpectations")
        boundary_error("HTTP/2 local content accounting lost its discriminated state"
            "unset, forbidden, unbounded, and known-length must be exclusive, only known-length may own a declared length, and cheap request expectation facts must remain value-semantic")
    endif()
endif()
if(EXISTS "${HTTP2_STALE_BODY_ACCOUNTING}")
    boundary_error("stale HTTP/2 body accounting header was restored"
        "peer content must be represented only by Http2RemoteContentState.h")
elseif(NOT EXISTS "${HTTP2_REMOTE_CONTENT_STATE}")
    boundary_error("HTTP/2 remote content state is missing"
        "Http2RemoteContentState.h must own peer Content-Length and DATA accounting")
elseif(NOT EXISTS "${HTTP2_REQUEST_HEADERS}" OR
       NOT EXISTS "${HTTP2_STREAM_STATE}" OR
       NOT EXISTS "${HTTP2_CONNECTION_SOURCE}")
    boundary_error("HTTP/2 remote content call chain is incomplete"
        "header decode, DATA preflight, and stream state must consume one remote-content contract")
else()
    file(READ "${HTTP2_REMOTE_CONTENT_STATE}" http2_remote_content_state)
    file(READ "${HTTP2_REQUEST_HEADERS}" http2_request_headers)
    file(READ "${HTTP2_STREAM_STATE}" http2_remote_stream_state)
    read_http2_connection_implementation(http2_remote_content_connection)
    if(NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentAllowedWithoutLength final" OR
       NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentAllowedKnownLength final" OR
       NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentMetadataOnlyWithoutLength final" OR
       NOT http2_remote_content_state MATCHES
           "class Http2RemoteContentMetadataOnlyKnownLength final" OR
       NOT http2_remote_content_state MATCHES "using State = std::variant" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<Http2RemoteContentAllowedWithoutLength>" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<Http2RemoteContentAllowedKnownLength>" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<[\r\n \t]*Http2RemoteContentMetadataOnlyWithoutLength>" OR
       NOT http2_remote_content_state MATCHES
           "std::get_if<[\r\n \t]*Http2RemoteContentMetadataOnlyKnownLength>" OR
       NOT http2_remote_content_state MATCHES "kCounterOverflow" OR
       NOT http2_remote_content_state MATCHES "kDeclaredLengthExceeded" OR
       NOT http2_remote_content_state MATCHES "kContentForbidden" OR
       NOT http2_remote_content_state MATCHES "selectMetadataOnly" OR
       NOT http2_remote_content_state MATCHES "account[(]" OR
       NOT http2_remote_content_state MATCHES "terminalLengthValid" OR
       NOT http2_remote_stream_state MATCHES
           "const Http2RemoteContentState&[ \t\r\n]+remoteContent[(][)] const & noexcept" OR
       NOT http2_remote_stream_state MATCHES "accountRemoteContent" OR
       NOT http2_remote_stream_state MATCHES
           "selectRemoteContentMetadataOnly" OR
       NOT http2_remote_content_connection MATCHES
           "stream->accountRemoteContent[(]data[.]size[(][)][)]" OR
       NOT http2_remote_content_connection MATCHES
           "Http2RemoteContentAccountingResult::kDeclaredLengthExceeded" OR
       NOT http2_remote_content_connection MATCHES
           "Http2RemoteContentAccountingResult::kContentForbidden" OR
       NOT http2_request_headers MATCHES "declareRemoteContentLength")
        boundary_error("HTTP/2 remote content accounting lost its discriminated transaction"
            "content allowance and length must be exclusive, DATA accounting must be atomic, and metadata-only responses must reject payload")
    endif()
endif()
foreach(http2_stale_web_runtime_header IN LISTS
        HTTP2_STALE_WEB_RUNTIME_HEADERS)
    if(EXISTS "${http2_stale_web_runtime_header}")
        file(RELATIVE_PATH relative
            "${RUVIA_ROOT}" "${http2_stale_web_runtime_header}")
        boundary_error("ruvia-http regained Web request-body runtime state"
            "${relative} must remain absent; route storage, buffering, queues, and coroutine wakeups belong to ruvia-web")
    endif()
endforeach()
if(NOT EXISTS "${HTTP2_REQUEST_CONTENT}")
    boundary_error("HTTP/2 request content contract is missing"
        "Http2RequestContent.h must own regular request Content-Length/END_STREAM selection")
else()
    file(READ "${HTTP2_REQUEST_CONTENT}" http2_request_content)
    if(NOT http2_request_content MATCHES
           "class Http2RequestWithoutContent final" OR
       NOT http2_request_content MATCHES
           "class Http2KnownLengthRequestContent final" OR
       NOT http2_request_content MATCHES
           "class Http2StreamingRequestContent final" OR
       NOT http2_request_content MATCHES "using Content = std::variant" OR
       NOT http2_request_content MATCHES
           "std::get_if<Http2RequestWithoutContent>" OR
       NOT http2_request_content MATCHES
           "std::get_if<Http2KnownLengthRequestContent>" OR
       NOT http2_request_content MATCHES
           "std::get_if<Http2StreamingRequestContent>" OR
       NOT http2_request_content MATCHES "knownLengthContent" OR
       NOT http2_request_content MATCHES "streamingContent")
        boundary_error("HTTP/2 request content lost its exclusive alternatives"
            "absent, known-length, and streaming contracts must own only their relevant payload")
    endif()
endif()
if(NOT EXISTS "${HTTP2_TUNNEL_STATE}")
    boundary_error("HTTP/2 CONNECT tunnel state is missing"
        "Http2TunnelState.h must own pending, open, and rejected phases")
else()
    file(READ "${HTTP2_TUNNEL_STATE}" http2_tunnel_state)
    file(READ "${HTTP2_STREAM_STATE}" http2_tunnel_stream_state)
    file(READ
        "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestBuilder.h"
        http2_tunnel_request_builder)
    file(READ
        "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h"
        http2_tunnel_websocket_handshake)
    if(NOT http2_tunnel_state MATCHES "enum class Http2ConnectForm" OR
       NOT http2_tunnel_state MATCHES "class Http2NotConnect final" OR
       NOT http2_tunnel_state MATCHES "class Http2ConnectPending final" OR
       NOT http2_tunnel_state MATCHES "class Http2TunnelOpen final" OR
       NOT http2_tunnel_state MATCHES "class Http2ConnectRejected final" OR
       NOT http2_tunnel_state MATCHES "using State = std::variant" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2NotConnect>" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2ConnectPending>" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2TunnelOpen>" OR
       NOT http2_tunnel_state MATCHES "std::get_if<Http2ConnectRejected>" OR
       NOT http2_tunnel_state MATCHES
           "form != Http2ConnectForm::kStandard" OR
       NOT http2_tunnel_state MATCHES
           "form != Http2ConnectForm::kExtended" OR
       NOT http2_tunnel_stream_state MATCHES
           "const Http2TunnelState& tunnel[(][)] const & noexcept" OR
       NOT http2_tunnel_request_builder MATCHES
           "tunnel[(][)][.]pending[(][)]" OR
       NOT http2_tunnel_websocket_handshake MATCHES
           "http2IsPendingWebSocketConnect")
        boundary_error("HTTP/2 CONNECT tunnel state lost exclusive alternatives"
            "only pending may own standard/extended form; message-content accounting must not reinterpret tunnel bytes")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SETTINGS}")
    boundary_error("HTTP/2 local SETTINGS contract is missing"
        "Http2LocalSettings.h must own wire values and matching receive accounting")
else()
    file(READ "${HTTP2_LOCAL_SETTINGS}" http2_local_settings)
    if(NOT http2_local_settings MATCHES "struct Http2LocalSettings" OR
       NOT http2_local_settings MATCHES "kMaxConcurrentStreams" OR
       NOT http2_local_settings MATCHES "kInitialWindowSize" OR
       NOT http2_local_settings MATCHES "kMaxFrameSize" OR
       NOT http2_local_settings MATCHES "http2WriteLocalSettingsFrame")
        boundary_error("HTTP/2 local SETTINGS has more than one source of truth"
            "one typed contract must drive emitted bytes, capacities, and receive windows")
    endif()
endif()
if(NOT EXISTS "${HTTP2_PEER_SETTINGS}")
    boundary_error("HTTP/2 peer SETTINGS contract is missing"
        "Http2PeerSettings.h must own role-aware peer setting validation")
else()
    file(READ "${HTTP2_PEER_SETTINGS}" http2_peer_settings)
    if(NOT http2_peer_settings MATCHES
           "explicit Http2PeerSettings.*Http2Role localRole" OR
       NOT http2_peer_settings MATCHES
           "localRole_.*Http2Role::kClient.*value == 1")
        boundary_error("HTTP/2 peer SETTINGS validation lost endpoint direction"
            "peer settings must bind Http2Role and reject server ENABLE_PUSH=1 at clients")
    endif()
    if(NOT http2_peer_settings MATCHES "enum class Http2PeerSettingError" OR
       NOT http2_peer_settings MATCHES "class Http2PeerSettingApplied final" OR
       NOT http2_peer_settings MATCHES "class Http2PeerInitialWindowChange final" OR
       NOT http2_peer_settings MATCHES "class Http2PeerSettingFailure final" OR
       NOT http2_peer_settings MATCHES "class Http2PeerSettingApplyResult final" OR
       NOT http2_peer_settings MATCHES "using Value = std::variant" OR
       NOT http2_peer_settings MATCHES "std::get_if<Http2PeerSettingApplied>" OR
       NOT http2_peer_settings MATCHES "std::get_if<Http2PeerInitialWindowChange>" OR
       NOT http2_peer_settings MATCHES "std::get_if<Http2PeerSettingFailure>" OR
       NOT http2_peer_settings MATCHES "makeInitialWindowChange" OR
       NOT http2_peer_settings MATCHES "makeFailure")
        boundary_error("HTTP/2 peer SETTINGS application lost its discriminated result"
            "ordinary settings, initial-window delta, and failure must remain exclusive alternatives")
    endif()
endif()
if(EXISTS "${HTTP2_CONNECTION_SOURCE}")
    read_http2_connection_implementation(http2_connection_source)
    if(NOT http2_connection_source MATCHES
           "localSend[(][)][.]headPending[(][)]" OR
       NOT http2_connection_source MATCHES "requestContentOpen[(][)]" OR
       NOT http2_connection_source MATCHES "responseContentOpen[(][)]" OR
       NOT http2_connection_source MATCHES "tunnelOpen[(][)]" OR
       NOT http2_connection_source MATCHES "Http2DataSubmitStatus::kBackpressured")
        boundary_error("HTTP/2 core does not enforce its typed local send state"
            "head ownership, request/response/tunnel DATA permission, and zero-ownership backpressure must be core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "discardDeferredStreamState\\(streamId\\)" OR
       NOT http2_connection_source MATCHES "first && http2EndsStream\\(endStream\\)")
        boundary_error("HTTP/2 terminal transitions bypass the shared protocol path"
            "reset cleanup and HEADERS-only END_STREAM placement must remain centralized")
    endif()
    if(NOT http2_connection_source MATCHES "Http2HeaderBlockKind::kDiscarded" OR
       NOT http2_connection_source MATCHES "decodeDiscardedHeaderBlock" OR
       NOT http2_connection_source MATCHES "detachActiveHeaderBlock" OR
       NOT http2_connection_source MATCHES
           "kCompressionError, \"field block not decompressed\"")
        boundary_error("HTTP/2 discarded field blocks bypass connection-scoped HPACK"
            "detached continuation state and COMPRESSION_ERROR fallback must remain core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "checkLocalContentAccept" OR
       NOT http2_connection_source MATCHES "acceptLocalContent" OR
       NOT http2_connection_source MATCHES "commitLocalContent" OR
       NOT http2_connection_source MATCHES
           "localContent[(][)][.]lengthComplete[(][)]" OR
       NOT http2_connection_source MATCHES
           "Http2LocalContentCheck::kNotStarted")
        boundary_error("HTTP/2 response Content-Length bypasses stream-owned accounting"
            "head, DATA emission, deferred drain, and finish must share local content state")
    endif()
    if(NOT http2_connection_source MATCHES "declareRemoteContentLength" OR
       NOT http2_connection_source MATCHES
           "remoteContent[(][)][.]allowedKnownLength[(][)]" OR
       NOT http2_connection_source MATCHES
           "remoteContent[(][)][.]terminalLengthValid[(][)]" OR
       NOT http2_connection_source MATCHES
           "selectRemoteContentMetadataOnly")
        boundary_error("HTTP/2 inbound Content-Length bypasses remote content state"
            "response semantics, CONNECT validation, DATA, and terminal HEADERS must share the peer-content contract")
    endif()
    string(REGEX MATCHALL "remoteContent[(][)][.]terminalLengthValid[(][)]"
        http2_remote_terminal_call_sites "${http2_connection_source}")
    list(LENGTH http2_remote_terminal_call_sites
        http2_remote_terminal_call_site_count)
    if(http2_remote_terminal_call_site_count LESS 3)
        boundary_error("HTTP/2 END_STREAM paths split remote length validation"
            "initial HEADERS, DATA, and trailing HEADERS must all consult the active remote-content alternative")
    endif()
    if(NOT http2_connection_source MATCHES "submitRegularRequestHead" OR
       NOT http2_connection_source MATCHES "content[.]withoutContent" OR
       NOT http2_connection_source MATCHES "content[.]knownLengthContent" OR
       NOT http2_connection_source MATCHES "content[.]streamingContent" OR
       NOT http2_connection_source MATCHES "beginLocalContentKnownLength" OR
       NOT http2_connection_source MATCHES "knownLengthContent->length" OR
       NOT http2_connection_source MATCHES "http2IsValidOutboundRegularRequestHead" OR
       NOT http2_connection_source MATCHES
           "method != \"CONNECT\" && isValidHttpMethodToken[(]method[)]")
        boundary_error("HTTP/2 regular request framing bypasses its typed content contract"
            "regular request validation, generated length, END_STREAM, and CONNECT isolation must remain core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "localRequestAdmissionError" OR
       NOT http2_connection_source MATCHES
           "activeLocalRequestStreams_.*peerSettings_\.maxConcurrentStreams" OR
       NOT http2_connection_source MATCHES
           "Http2RequestHeadSubmitResult::makeSubmitted" OR
       NOT http2_connection_source MATCHES
           "Http2RequestHeadSubmitResult::makeFailure" OR
       NOT http2_connection_source MATCHES "activateLocalRequestStream" OR
       NOT http2_connection_source MATCHES "releaseLocalRequestStreamIfClosed")
        boundary_error("HTTP/2 client stream concurrency escaped the protocol core"
            "request submission must atomically discriminate success/failure and retain/release peer concurrency slots")
    endif()
    if(NOT http2_connection_source MATCHES "submitConnectRequestHead" OR
       NOT http2_connection_source MATCHES "submitExtendedConnectRequestHead" OR
       NOT http2_connection_source MATCHES "submitConnectResponseHead" OR
       NOT http2_connection_source MATCHES "beginStandardConnect" OR
       NOT http2_connection_source MATCHES "beginExtendedConnect" OR
       NOT http2_connection_source MATCHES "acceptConnect" OR
       NOT http2_connection_source MATCHES "rejectConnect" OR
       NOT http2_connection_source MATCHES
           "tunnel[(][)][.]pending[(][)]" OR
       NOT http2_connection_source MATCHES "tunnel[(][)][.]open[(][)]" OR
       NOT http2_connection_source MATCHES "Http2Event::tunnelData" OR
       NOT http2_connection_source MATCHES "Http2Event::tunnelEnd" OR
       NOT http2_connection_source MATCHES
           "prefacePhase_ != PrefacePhase::kReady" OR
       NOT http2_connection_source MATCHES
           "remote[.]tunnelOpen[(][)]" OR
       NOT http2_connection_source MATCHES
           "http2RemotePeerHalfClosed")
        boundary_error("HTTP/2 CONNECT bypasses the shared tunnel lifecycle"
            "typed pending/open/rejected transitions, dedicated heads, tunnel events, and peer half-close enforcement must remain core-owned")
    endif()
    if(NOT http2_connection_source MATCHES "Http2Connection::beginConnection" OR
       NOT http2_connection_source MATCHES "Http2LocalSettings::kFrameBytes" OR
       NOT http2_connection_source MATCHES "Http2LocalSettings::kInitialWindowSize" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kConnectionNotStarted" OR
       NOT http2_connection_source MATCHES
           "PrefacePhase::kAwaitingClientMagic" OR
       NOT http2_connection_source MATCHES
           "PrefacePhase::kAwaitingPeerSettings" OR
       NOT http2_connection_source MATCHES "PrefacePhase::kReady" OR
       NOT http2_connection_source MATCHES "SETTINGS ACK before SETTINGS" OR
       NOT http2_connection_source MATCHES
           "connectionSendWindow_.*kHttp2DefaultInitialWindowSize")
        boundary_error("HTTP/2 preface bytes diverge from flow-control accounting"
            "typed role-aware startup, initial non-ACK SETTINGS, and local/peer window ownership must remain centralized")
    endif()
    if(NOT http2_connection_source MATCHES
           "const auto[*]? failure = result[.]failure[(][)]" OR
       NOT http2_connection_source MATCHES
           "http2PeerSettingErrorCode[(]failure->error[(][)][)]" OR
       NOT http2_connection_source MATCHES
           "const auto[*]? initialWindowChange = result[.]initialWindowChange[(][)]" OR
       NOT http2_connection_source MATCHES
           "initialWindowChange->delta[(][)]")
        boundary_error("HTTP/2 peer SETTINGS result escaped its connection owner"
            "the connection must branch on failure or the sole delta-owning alternative before mutating stream windows")
    endif()
    if(NOT http2_connection_source MATCHES
           "eventOffset_ < events_\.size\(\)" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kEventsPending" OR
       NOT http2_connection_source MATCHES
           "inputOffset_ < input_\.size\(\)" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kAccepted" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kNeedInput" OR
       NOT http2_connection_source MATCHES
           "Http2FeedResult::kProtocolFailure")
        boundary_error("HTTP/2 feed restored lossy event/input ownership"
            "retained spans, wholly accepted spans, partial protocol units, and terminal failure must remain distinguishable without a byte count")
    endif()
    if(NOT http2_connection_source MATCHES
           "Http2Connection::processRstStream" OR
       NOT http2_connection_source MATCHES
           "static_cast<Http2ErrorCode>[(]http2Read32" OR
       NOT http2_connection_source MATCHES
           "closeStream[(]header[.]streamId, Http2StreamCloseSource::kPeer, error[)]" OR
       NOT http2_connection_source MATCHES
           "Http2Event::streamClosed[(]streamId, source, error[)]")
        boundary_error("HTTP/2 stream-close events lost their RFC error reason"
            "RST_STREAM must be decoded once and the exact peer/local error must reach the typed close event")
    endif()
    if(NOT http2_connection_source MATCHES "Http2Connection::processGoaway" OR
       NOT http2_connection_source MATCHES
           "goaway[.]lastStreamId[(][)] > peerGoaway_->lastStreamId[(][)]" OR
       NOT http2_connection_source MATCHES
           "Http2StreamCloseSource::kPeerGoaway" OR
       NOT http2_connection_source MATCHES
           "Http2Event::goaway[(]goaway[)]" OR
       NOT http2_connection_source MATCHES
           "Http2Event::requestUnprocessed[(]streamId[)]" OR
       NOT http2_connection_source MATCHES "closeStreamImpl" OR
       NOT http2_connection_source MATCHES "beginDrain\\(\\)" OR
       NOT http2_connection_source MATCHES
           "localConnectionState_[.]fail[(]error[)]")
        boundary_error("HTTP/2 peer GOAWAY escaped the protocol core"
            "monotonic last-stream-id, bilateral drain, typed fatal error, cleanup, and safe-retry events must remain centralized")
    endif()
    if(NOT http2_connection_source MATCHES
           "http2DebitConnectionReceiveWindow" OR
       NOT http2_connection_source MATCHES
           "http2DebitStreamReceiveWindow" OR
       NOT http2_connection_source MATCHES
           "releaseDroppedDataConnectionWindow" OR
       NOT http2_connection_source MATCHES
           "http2CreditConnectionReceiveWindow")
        boundary_error("HTTP/2 DATA bypasses connection-first receive-window accounting"
            "every structurally valid DATA payload must debit connection credit before stream lookup and release discarded credit exactly once")
    endif()
    if(NOT http2_connection_source MATCHES
           "Http2BufferedResponseHeadSubmitResult::makeClosedFailure" OR
       NOT http2_connection_source MATCHES
           "Http2BufferedResponseHeadSubmitResult::makeSubmitted" OR
       NOT http2_connection_source MATCHES
           "Http2StreamingResponseHeadSubmitResult::makeClosedFailure" OR
       NOT http2_connection_source MATCHES
           "Http2StreamingResponseHeadSubmitResult::makeSubmitted" OR
       NOT http2_connection_source MATCHES
           "makeInvalidStateFailure" OR
       NOT http2_connection_source MATCHES
           "makeResponsePlanMismatchFailure" OR
       NOT http2_connection_source MATCHES
           "makeInvalidMessageFailure")
        boundary_error("HTTP/2 response-head transaction bypasses its typed result"
            "buffered and streaming heads must return error-only failures and plan-only committed submissions")
    endif()
endif()

set(HTTP2_LOCAL_CONNECTION_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2LocalConnectionState.h")
file(READ "${HTTP2_LOCAL_CONNECTION_STATE}" http2_local_connection_state)
file(READ
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    http2_connection_state_owner)
if(NOT http2_local_connection_state MATCHES
       "class Http2LocalConnectionState final" OR
   NOT http2_local_connection_state MATCHES
       "Http2LocalConnectionGracefulDrain" OR
   NOT http2_local_connection_state MATCHES
       "Http2LocalConnectionFatalFailure" OR
   NOT http2_connection_state_owner MATCHES
       "Http2LocalConnectionState localConnectionState_" OR
   http2_connection_state_owner MATCHES
       "bool draining_|goawayLastStreamId_|connectionError_")
    boundary_error("HTTP/2 local connection restored parallel terminal fields"
        "Open, graceful GOAWAY boundary, and fatal error must be one exclusive state")
endif()

set(HTTP2_FLOW_CONTROL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2FlowControl.h")
if(EXISTS "${HTTP2_FLOW_CONTROL}")
    file(READ "${HTTP2_FLOW_CONTROL}" http2_flow_control)
    if(NOT http2_flow_control MATCHES "Http2ReceiveWindowDebitStatus" OR
       NOT http2_flow_control MATCHES "http2DebitConnectionReceiveWindow" OR
       NOT http2_flow_control MATCHES "http2DebitStreamReceiveWindow" OR
       NOT http2_flow_control MATCHES "http2CreditConnectionReceiveWindow" OR
       NOT http2_flow_control MATCHES "http2CreditStreamReceiveWindow")
        boundary_error("HTTP/2 receive-window primitives lost scope ownership"
            "connection and stream debit/credit operations must remain separate and typed")
    endif()
endif()

set(HTTP2_EVENT_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Event.h")
if(NOT EXISTS "${HTTP2_EVENT_HEADER}")
    boundary_error("HTTP/2 typed event contract is missing"
        "ruvia-http must own Http2Event.h")
else()
    file(READ "${HTTP2_EVENT_HEADER}" http2_event_header)
    if(NOT http2_event_header MATCHES "enum class Http2EventKind" OR
       NOT http2_event_header MATCHES "using Value = std::variant" OR
       NOT http2_event_header MATCHES "class Http2StreamClosedEvent final" OR
       NOT http2_event_header MATCHES "Http2StreamCloseSource source" OR
       NOT http2_event_header MATCHES "Http2ErrorCode error" OR
       NOT http2_event_header MATCHES "class Http2RequestUnprocessedEvent final" OR
       NOT http2_event_header MATCHES "class Http2GoawayEvent final" OR
       NOT http2_event_header MATCHES "lastStreamId[(][)] const" OR
       NOT http2_event_header MATCHES "std::get_if<Http2StreamClosedEvent>" OR
       NOT http2_event_header MATCHES
           "messageHead[(][)] const [&] noexcept" OR
       NOT http2_event_header MATCHES
           "messageBodyChunk[(][)] const && = delete" OR
       NOT http2_event_header MATCHES
           "streamClosed[(][)] const [&] noexcept" OR
       NOT http2_event_header MATCHES
           "goaway[(][)] const && = delete" OR
       NOT http2_event_header MATCHES
           "peerGoaway[(][)] const && = delete")
        boundary_error("HTTP/2 event payloads lost their discriminated contract"
            "every materialized event must have one typed payload, lend alternatives only from live lvalues, and keep close/GOAWAY metadata on their actual owners")
    endif()
endif()

set(HTTP2_EVENT_TEST "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(HTTP_RESPONSE_CONTENT_SEMANTICS_TEST
    "${RUVIA_ROOT}/tests/unit_http_response_content_semantics.cpp")
set(HTTP2_PEER_SETTINGS_TEST "${RUVIA_ROOT}/tests/unit_http2_peer_settings.cpp")
set(HTTP2_LOCAL_CONTENT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_local_content_state.cpp")
set(HTTP2_LOCAL_SEND_TEST
    "${RUVIA_ROOT}/tests/unit_http2_stream_lifecycle.cpp")
set(HTTP2_REMOTE_CONTENT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_remote_content_state.cpp")
set(HTTP2_WEB_STREAM_RUNTIME_TEST
    "${RUVIA_ROOT}/tests/unit_http2_sansio_stream_runtime.cpp")
set(HTTP2_CONNECT_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connect.cpp")
set(HTTP_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(NOT EXISTS "${HTTP2_EVENT_TEST}" OR
   NOT EXISTS "${HTTP_PACKAGE_CONSUMER}")
    boundary_error("shared request-content semantics are untested"
        "HTTP/2 transactional rejection and installed value facts must remain pinned")
else()
    file(READ "${HTTP2_EVENT_TEST}" http_request_content_semantics_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}"
        http_request_content_semantics_package_test)
    if(NOT http_request_content_semantics_test MATCHES
           "http2_connection_enforces_request_method_content_semantics_transactionally" OR
       NOT http_request_content_semantics_test MATCHES
           "TRACE.*Http2RequestContent::knownLength" OR
       NOT http_request_content_semantics_test MATCHES
           "OPTIONS.*Http2RequestContent::streaming" OR
       NOT http_request_content_semantics_package_test MATCHES
           "is_enum_v<[\r\n \t]*ruvia::detail::HttpRequestContentSemantics>" OR
       NOT http_request_content_semantics_package_test MATCHES
           "httpRequestContentSemantics[(][\"]TRACE[\"][)][ \t\r\n]*==" OR
       NOT http_request_content_semantics_package_test MATCHES
           "httpRequestContentSemantics[(][\"]OPTIONS[\"][)][ \t\r\n]*==")
        boundary_error("shared request-content semantics ownership is under-tested"
            "TRACE, OPTIONS, transactional HTTP/2 rejection, and installed value facts must remain explicit")
    endif()
endif()
if(NOT EXISTS "${HTTP_RESPONSE_CONTENT_SEMANTICS_TEST}" OR
   NOT EXISTS "${HTTP_PACKAGE_CONSUMER}")
    boundary_error("shared response-content semantics are untested"
        "unit and installed consumers must pin every exclusive response classification")
else()
    file(READ "${HTTP_RESPONSE_CONTENT_SEMANTICS_TEST}"
        http_response_content_semantics_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}"
        http_response_content_semantics_package_test)
    if(NOT http_response_content_semantics_test MATCHES
           "response_content_semantics_owns_method_status_precedence" OR
       NOT http_response_content_semantics_test MATCHES
           "response_content_semantics_preserves_case_sensitive_method_tokens" OR
       NOT http_response_content_semantics_test MATCHES
           "HttpKnownMethod::kConnect, 204" OR
       NOT http_response_content_semantics_test MATCHES
           "HttpKnownMethod::kGet, 205" OR
       NOT http_response_content_semantics_package_test MATCHES
           "is_enum_v<[\r\n \t]*ruvia::detail::HttpResponseContentSemantics>" OR
       NOT http_response_content_semantics_package_test MATCHES
           "sizeof[(]ruvia::detail::HttpResponseContentSemantics[)] == 1" OR
       NOT http_response_content_semantics_package_test MATCHES
           "httpResponseContentSemantics")
        boundary_error("shared response-content semantics ownership is under-tested"
            "method/status precedence, case sensitivity, CONNECT, no-content, and installed value facts must remain explicit")
    endif()
endif()
if(NOT EXISTS "${HTTP2_PEER_SETTINGS_TEST}")
    boundary_error("HTTP/2 peer SETTINGS result contract is untested"
        "unit_http2_peer_settings.cpp must pin all exclusive alternatives")
elseif(EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_PEER_SETTINGS_TEST}" http2_peer_settings_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_package_consumer)
    if(NOT http2_peer_settings_test MATCHES
           "peer_setting_apply_result_is_discriminated" OR
       NOT http2_peer_settings_test MATCHES
           "!std::default_initializable<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "!HasPeerSettingStatusField<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "!HasPeerSettingChangedField<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "!HasPeerSettingDeltaField<Http2PeerSettingApplyResult>" OR
       NOT http2_peer_settings_test MATCHES
           "HasPeerSettingDeltaAccessor<Http2PeerInitialWindowChange>" OR
       NOT http2_peer_settings_test MATCHES
           "HasPeerSettingErrorAccessor<Http2PeerSettingFailure>" OR
       NOT http2_peer_settings_test MATCHES
           "Http2PeerSettingError::kInvalidInitialWindow" OR
       NOT http2_peer_settings_test MATCHES
           "Http2ErrorCode::kFlowControlError" OR
       NOT http_package_consumer MATCHES
           "Http2PeerSettingApplyResult" OR
       NOT http_package_consumer MATCHES
           "Http2PeerSettingApplied" OR
       NOT http_package_consumer MATCHES
           "Http2PeerInitialWindowChange" OR
       NOT http_package_consumer MATCHES
           "Http2PeerSettingFailure" OR
       NOT http_package_consumer MATCHES
           "!HasPeerSettingStatusField" OR
       NOT http_package_consumer MATCHES
           "!HasPeerSettingChangedField" OR
       NOT http_package_consumer MATCHES
           "!HasPeerSettingDeltaField" OR
       NOT http_package_consumer MATCHES
           "windowSetting[.]initialWindowChange[(][)]->delta[(][)]" OR
       NOT http_package_consumer MATCHES
           "invalidSetting[.]failure[(][)]->error[(][)]")
        boundary_error("HTTP/2 peer SETTINGS result contract is under-tested"
            "unit and installed-package consumers must pin payload-free application, delta-only change, and error-only failure")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SEND_TEST}")
    boundary_error("HTTP/2 local send alternatives are untested"
        "unit_http2_stream_lifecycle.cpp must pin every transition and exclusive payload owner")
elseif(EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_LOCAL_SEND_TEST}" http2_local_send_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_local_send_package_test)
    if(NOT http2_local_send_test MATCHES
           "http2_local_send_state_request_content_has_exclusive_transitions" OR
       NOT http2_local_send_test MATCHES
           "http2_local_send_state_response_content_and_trailers_are_distinct" OR
       NOT http2_local_send_test MATCHES
           "http2_local_send_state_connect_waits_for_acceptance" OR
       NOT http2_local_send_test MATCHES
           "http2_local_send_state_abort_owns_immutable_close_source" OR
       NOT http2_local_send_test MATCHES
           "!std::default_initializable<Http2LocalSendState>" OR
       NOT http2_local_send_test MATCHES
           "!std::default_initializable<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!std::default_initializable<Http2LocalHeadPending>" OR
       NOT http2_local_send_test MATCHES
           "HasCloseSource<Http2StreamAborted>" OR
       NOT http2_local_send_test MATCHES
           "std::constructible_from<[\r\n \t]*Http2StreamAborted" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleLocalSendProduct<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleResetAccessor<Http2LocalSendState>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleResetAccessor<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleMarkReset<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES
           "!HasStaleMarkClosed<Http2StreamLifecycle>" OR
       NOT http2_local_send_test MATCHES "!queuedThenAborted[.]queued[(][)]" OR
       NOT http2_local_send_test MATCHES
           "static_cast<Http2StreamCloseSource>[(]0xFF[)]" OR
       NOT http2_local_send_package_test MATCHES
           "HasHttp2LocalSendAlternatives" OR
       NOT http2_local_send_package_test MATCHES
           "!std::default_initializable<[\r\n \t]*ruvia::detail::Http2LocalSendState" OR
       NOT http2_local_send_package_test MATCHES
           "!HasStaleHttp2LocalSendProduct" OR
       NOT http2_local_send_package_test MATCHES
           "!HasStaleHttp2StreamLocalSendForwarders" OR
       NOT http2_local_send_package_test MATCHES "HasHttp2AbortLifecycle" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2IsReset" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2MarkReset" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2MarkClosed" OR
       NOT http2_local_send_package_test MATCHES "HasHttp2RemoveAborted" OR
       NOT http2_local_send_package_test MATCHES "!HasStaleHttp2RemoveReset" OR
       NOT http2_local_send_package_test MATCHES
           "HasHttp2LocalCloseSource" OR
       NOT http2_local_send_package_test MATCHES
           "std::constructible_from<[\r\n \t]*ruvia::detail::Http2StreamAborted" OR
       NOT http2_local_send_package_test MATCHES
           "const ruvia::detail::Http2LocalSendState&" OR
       NOT http2_local_send_package_test MATCHES
           "localSendStream[.]beginLocalResponseTrailersOnly" OR
       NOT http2_local_send_package_test MATCHES
           "localSend[.]responseTrailersOnly[(][)]" OR
       NOT http2_local_send_package_test MATCHES
           "localSend[.]endStreamQueued[(][)]" OR
       NOT http2_local_send_package_test MATCHES
           "localSend[.]aborted[(][)][-][>]source[(][)]")
        boundary_error("HTTP/2 local send alternative ownership is under-tested"
            "unit and installed consumers must reject phase/kind/boolean products, private alternatives, none/invalid abort sources, reset vocabulary, and stale forwarding accessors")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_SEND_TEST}" OR
   NOT EXISTS "${HTTP2_CONNECT_TEST}")
    boundary_error("HTTP/2 remote receive alternatives are untested"
        "stream lifecycle and CONNECT tests must pin every remote transition and terminal-flow regression")
elseif(EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_LOCAL_SEND_TEST}" http2_remote_receive_test)
    file(READ "${HTTP2_CONNECT_TEST}" http2_remote_receive_connect_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_remote_receive_package_test)
    if(NOT http2_remote_receive_test MATCHES
           "http2_remote_receive_state_owns_head_content_connect_and_terminal_transitions" OR
       NOT http2_remote_receive_test MATCHES
           "!std::default_initializable<Http2RemoteReceiveState>" OR
       NOT http2_remote_receive_test MATCHES
           "Http2RemoteConnectRejectedAwaitingEndStream" OR
       NOT http2_remote_receive_test MATCHES
           "!HasStaleBodyEnded<Http2StreamState>" OR
       NOT http2_remote_receive_test MATCHES
           "!HasStalePeerEndStream<Http2StreamState>" OR
       NOT http2_remote_receive_test MATCHES
           "!HasStaleHeadersDecoded<Http2StreamState>" OR
       NOT http2_remote_receive_connect_test MATCHES
           "http2_connect_server_rejection_accepts_empty_terminal_data" OR
       NOT http2_remote_receive_connect_test MATCHES
           "http2_connect_pending_accepts_empty_request_half_close" OR
       NOT http2_remote_receive_connect_test MATCHES
           "http2_connect_open_tunnel_batches_owner_released_window_credit" OR
       NOT http2_remote_receive_package_test MATCHES
           "HasHttp2RemoteReceiveAlternatives" OR
       NOT http2_remote_receive_package_test MATCHES
           "!std::default_initializable<[\r\n \t]*ruvia::detail::Http2RemoteReceiveState" OR
       NOT http2_remote_receive_package_test MATCHES
           "!HasStaleHttp2BodyEnded" OR
       NOT http2_remote_receive_package_test MATCHES
           "!HasStaleHttp2PeerEndStream" OR
       NOT http2_remote_receive_package_test MATCHES
           "!HasStaleHttp2HeadersDecoded" OR
       NOT http2_remote_receive_package_test MATCHES
           "const ruvia::detail::Http2RemoteReceiveState&" OR
       NOT http2_remote_receive_package_test MATCHES
           "remoteReceiveStream[.]finishRemoteRejectedConnect" OR
       NOT http2_remote_receive_package_test MATCHES
           "remotePendingEndStream[.]finishRemotePendingConnect")
        boundary_error("HTTP/2 remote receive alternative ownership is under-tested"
            "unit and installed consumers must reject head/body/peer booleans, pin private alternatives, and preserve rejected-CONNECT termination plus tunnel receive-credit batching")
    endif()
endif()
if(NOT EXISTS "${HTTP2_CONNECT_TEST}")
    boundary_error("HTTP/2 CONNECT tunnel alternatives are untested"
        "unit_http2_connect.cpp must pin phase and form ownership")
elseif(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_CONNECT_TEST}" http2_tunnel_test)
    file(READ "${HTTP2_EVENT_TEST}" http2_tunnel_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_tunnel_package_test)
    if(NOT http2_tunnel_test MATCHES
           "http2_tunnel_state_alternatives_own_valid_transitions" OR
       NOT http2_tunnel_test MATCHES
           "HasConnectForm<Http2ConnectPending>" OR
       NOT http2_tunnel_test MATCHES
           "!HasStaleTunnelKindPhase<Http2TunnelState>" OR
       NOT http2_tunnel_test MATCHES
           "static_cast<Http2ConnectForm>[(]0xFF[)]" OR
       NOT http2_tunnel_test MATCHES "state[.]notConnect[(][)]" OR
       NOT http2_tunnel_test MATCHES "state[.]pending[(][)]" OR
       NOT http2_tunnel_test MATCHES "state[.]open[(][)]" OR
       NOT http2_tunnel_test MATCHES "rejected[.]rejected[(][)]" OR
       NOT http2_tunnel_connection_test MATCHES
           "!HasStaleTunnelForwarders<Http2StreamState>" OR
       NOT http2_tunnel_package_test MATCHES
           "HasHttp2TunnelAlternatives" OR
       NOT http2_tunnel_package_test MATCHES
           "!HasStaleHttp2TunnelKindPhase" OR
       NOT http2_tunnel_package_test MATCHES
           "!HasStaleHttp2StreamTunnelForwarders" OR
       NOT http2_tunnel_package_test MATCHES
           "HasHttp2ConnectForm" OR
       NOT http2_tunnel_package_test MATCHES
           "tunnel[.]pending[(][)]->form[(][)]")
        boundary_error("HTTP/2 CONNECT tunnel alternative ownership is under-tested"
            "unit and installed consumers must reject kind/phase products and inspect form only on pending")
    endif()
endif()
if(NOT EXISTS "${HTTP2_REMOTE_CONTENT_TEST}")
    boundary_error("HTTP/2 remote content alternatives are untested"
        "unit tests must pin typed length ownership and transactional DATA acceptance")
elseif(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_REMOTE_CONTENT_TEST}" http2_remote_content_test)
    file(READ "${HTTP2_EVENT_TEST}" http2_remote_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_remote_package_test)
    if(NOT http2_remote_content_test MATCHES
           "http2_remote_content_allowance_and_length_alternatives_are_explicit" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_metadata_only_preserves_representation_length" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_accounting_is_atomic" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_counter_overflow_is_atomic" OR
       NOT http2_remote_content_test MATCHES
           "http2_remote_content_rejects_late_semantic_transitions" OR
       NOT http2_remote_content_test MATCHES
           "HasDeclaredLength<Http2RemoteContentAllowedKnownLength>" OR
       NOT http2_remote_content_test MATCHES
           "HasDeclaredLength<[\r\n \t]*Http2RemoteContentMetadataOnlyKnownLength>" OR
       NOT http2_remote_content_test MATCHES
           "!HasStaleLengthTuple<Http2RemoteContentState>" OR
       NOT http2_remote_content_test MATCHES
           "!HasReceivedBytes<Http2RemoteContentState>" OR
       NOT http2_remote_content_test MATCHES
           "!HasStaleCheckAcceptSplit<Http2RemoteContentState>" OR
       NOT http2_remote_content_test MATCHES
           "Http2RemoteContentAccountingResult::kCounterOverflow" OR
       NOT http2_remote_content_test MATCHES
           "Http2RemoteContentAccountingResult::kDeclaredLengthExceeded" OR
       NOT http2_remote_content_test MATCHES
           "Http2RemoteContentAccountingResult::kContentForbidden" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_feed_data_emits_body_chunk_and_end" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_same_feed_data_credit_queues_owner_batch" OR
       NOT http2_remote_connection_test MATCHES
           "releaseReceivedData[(]1[)]" OR
       NOT http2_remote_connection_test MATCHES "remoteKnownLength" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_client_head_representation_length_survives_trailer_terminal" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_client_rejects_data_for_responses_without_content" OR
       NOT http2_remote_connection_test MATCHES
           "http2_connection_client_allows_empty_terminal_data_without_content_event" OR
       NOT http2_remote_package_test MATCHES
           "HasHttp2RemoteContentAlternatives" OR
       NOT http2_remote_package_test MATCHES
           "!HasStaleHttp2RemoteContentTuple" OR
       NOT http2_remote_package_test MATCHES
           "!HasHttp2RemoteReceivedBytes<[\r\n \t]*ruvia::detail::Http2RemoteContentState>" OR
       NOT http2_remote_package_test MATCHES
           "!HasStaleHttp2RemoteCheckAcceptSplit" OR
       NOT http2_remote_package_test MATCHES
           "!HasStaleHttp2StreamRemoteContentForwarders")
        boundary_error("HTTP/2 remote content ownership is under-tested"
            "unit and installed consumers must pin metadata-only alternatives, atomic accounting, and malformed no-content DATA rejection")
    endif()
endif()
if(NOT EXISTS "${HTTP2_WEB_STREAM_RUNTIME_TEST}")
    boundary_error("Web-owned HTTP/2 stream runtime is untested"
        "unit_http2_sansio_stream_runtime.cpp must pin route/body storage, dispatch leases, concurrent wakeups, and stable per-stream ownership")
else()
    file(READ "${HTTP2_WEB_STREAM_RUNTIME_TEST}"
        http2_web_stream_runtime_test)
    if(NOT http2_web_stream_runtime_test MATCHES
           "http2_web_body_queue_preserves_fifo_and_tracks_backlog" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_route_selection_owns_exact_body_storage" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_request_body_runtime_enforces_total_and_backlog_limits" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_runtime_table_keeps_active_storage_stable" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_runtime_table_owns_dispatch_signal_and_lease" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_signal_wakes_concurrent_waiters_without_self_cancel" OR
       NOT http2_web_stream_runtime_test MATCHES
           "http2_web_stream_runtime_keeps_overflow_signal_reference_stable")
        boundary_error("Web-owned HTTP/2 stream runtime is under-tested"
            "FIFO/backlog accounting, stable storage, table-owned dispatch leases, and concurrent signal waiters must remain explicit")
    endif()
endif()
if(NOT EXISTS "${HTTP2_LOCAL_CONTENT_TEST}")
    boundary_error("HTTP/2 local content alternatives are untested"
        "unit_http2_local_content_state.cpp must pin state and payload ownership")
elseif(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_LOCAL_CONTENT_TEST}" http2_local_content_test)
    file(READ "${HTTP2_EVENT_TEST}" http2_local_content_connection_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_local_content_package_test)
    if(NOT http2_local_content_test MATCHES
           "http2_local_content_known_length_preflight_is_transactional" OR
       NOT http2_local_content_test MATCHES
           "http2_local_content_alternatives_are_explicit" OR
       NOT http2_local_content_test MATCHES
           "!HasLocalContentMode<Http2LocalContentState>" OR
       NOT http2_local_content_test MATCHES
           "HasDeclaredLength<Http2LocalContentKnownLength>" OR
       NOT http2_local_content_test MATCHES
           "Http2LocalContentCheck::kNotStarted" OR
       NOT http2_local_content_test MATCHES "!state[.]lengthComplete[(][)]" OR
       NOT http2_local_content_connection_test MATCHES
           "!HasStaleLocalContentForwarders<Http2StreamState>" OR
       NOT http2_local_content_connection_test MATCHES
           "requireLocalKnownLength" OR
       NOT http2_local_content_package_test MATCHES
           "HasHttp2LocalContentAlternatives" OR
       NOT http2_local_content_package_test MATCHES
           "!HasStaleHttp2LocalModeAccessor" OR
       NOT http2_local_content_package_test MATCHES
           "!HasStaleHttp2StreamLocalContentForwarders")
        boundary_error("HTTP/2 local content alternative ownership is under-tested"
            "unit and installed consumers must reject mode/fake-length access, pin unset rejection, and inspect counters through one const state")
    endif()
endif()
if(EXISTS "${HTTP2_EVENT_TEST}" AND EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP2_EVENT_TEST}" http2_event_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_package_consumer)
    if(NOT http2_event_test MATCHES
           "http2_connection_request_content_alternatives_own_wire_framing" OR
       NOT http2_event_test MATCHES
           "!HasRequestContentMode<Http2RequestContent>" OR
       NOT http2_event_test MATCHES
           "HasRequestContentLength<[\r\n \t]*ruvia::detail::Http2KnownLengthRequestContent>" OR
       NOT http2_event_test MATCHES "withoutContent[.]withoutContent" OR
       NOT http2_event_test MATCHES "zeroLength[.]knownLengthContent" OR
       NOT http2_event_test MATCHES "streaming[.]streamingContent" OR
       NOT http_package_consumer MATCHES
           "HasHttp2RequestContentAlternatives" OR
       NOT http_package_consumer MATCHES
           "!HasStaleHttp2ContentMode" OR
       NOT http_package_consumer MATCHES
           "HasHttp2RequestContentLength<[\r\n \t]*ruvia::detail::Http2KnownLengthRequestContent>")
        boundary_error("HTTP/2 request-content alternatives are under-tested"
            "unit and installed consumers must pin absent, explicit zero-length, and streaming payload ownership")
    endif()
    if(NOT http2_event_test MATCHES
           "http2_connection_event_queue_is_optional_and_discriminated" OR
       NOT http2_event_test MATCHES
           "closed->error[(][)] == Http2ErrorCode::kCancel" OR
       NOT http2_event_test MATCHES
           "closed->error[(][)] == Http2ErrorCode::kProtocolError" OR
       NOT http2_event_test MATCHES
           "event[.]goaway[(][)]->lastStreamId[(][)]" OR
       NOT http2_event_test MATCHES
           "event[.]requestUnprocessed[(][)]->streamId[(][)]" OR
       NOT http_package_consumer MATCHES
           "std::optional<ruvia::detail::Http2Event>" OR
       NOT http_package_consumer MATCHES
           "default_initializable<ruvia::detail::Http2Event>" OR
       NOT http_package_consumer MATCHES
           "Http2RequestUnprocessedEvent")
        boundary_error("HTTP/2 typed event contract is under-tested"
            "unit and installed-package consumers must pin optional draining, exclusive payloads, RST reasons, and GOAWAY ownership")
    endif()
    if(NOT http2_event_test MATCHES
           "http2_connection_feed_before_begin_retains_input_and_is_retryable" OR
       NOT http2_event_test MATCHES "std::is_enum_v<Http2FeedResult>" OR
       NOT http2_event_test MATCHES "!HasFeedStatusField<Http2FeedResult>" OR
       NOT http2_event_test MATCHES "!HasFeedConsumedField<Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "std::is_enum_v<ruvia::detail::Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "!HasFeedStatusField<ruvia::detail::Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "!HasFeedConsumedField<ruvia::detail::Http2FeedResult>" OR
       NOT http_package_consumer MATCHES
           "Http2FeedResult::kConnectionNotStarted")
        boundary_error("HTTP/2 feed ownership contract is under-tested"
            "unit and installed-package consumers must pin the direct enum shape, removed tuple fields, and retained pre-start input")
    endif()
    if(NOT http2_event_test MATCHES "Http2SubmittedRequestHead" OR
       NOT http2_event_test MATCHES "Http2RequestHeadSubmitFailure" OR
       NOT http2_event_test MATCHES
           "!HasRequestHeadStatusAccessor<Http2RequestHeadSubmitResult>" OR
       NOT http2_event_test MATCHES
           "!HasRequestHeadAcceptedAccessor<Http2RequestHeadSubmitResult>" OR
       NOT http2_event_test MATCHES
           "!HasRequestHeadStreamIdAccessor<Http2RequestHeadSubmitResult>" OR
       NOT http2_event_test MATCHES
           "!std::constructible_from<Http2SubmittedRequestHead, std::uint32_t>" OR
       NOT http2_event_test MATCHES
           "HasRequestHeadStreamIdAccessor<Http2SubmittedRequestHead>" OR
       NOT http2_event_test MATCHES
           "HasRequestHeadErrorAccessor<Http2RequestHeadSubmitFailure>" OR
       NOT http_package_consumer MATCHES
           "Http2SubmittedRequestHead" OR
       NOT http_package_consumer MATCHES
           "Http2RequestHeadSubmitFailure" OR
       NOT http_package_consumer MATCHES
           "!HasRequestHeadStatusAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasRequestHeadAcceptedAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasRequestHeadStreamIdAccessor" OR
       NOT http_package_consumer MATCHES
           "!std::constructible_from" OR
       NOT http_package_consumer MATCHES
           "submittedRequest->streamId[(][)]" OR
       NOT http_package_consumer MATCHES
           "unavailable[.]failure[(][)]->error[(][)]")
        boundary_error("HTTP/2 request-head result contract is under-tested"
            "unit and installed-package consumers must pin exclusive submitted/failure payloads and removed top-level accessors")
    endif()
    if(NOT http2_event_test MATCHES
           "http2_connection_response_head_submit_result_is_discriminated" OR
       NOT http2_event_test MATCHES
           "const ruvia::detail::HttpBufferedResponseWritePlan[*]" OR
       NOT http2_event_test MATCHES
           "const ruvia::detail::ResponseStreamCommitPlan[*]" OR
       NOT http2_event_test MATCHES
           "Http2ResponseHeadSubmitFailure" OR
       NOT http2_event_test MATCHES
           "!HasResponseHeadStatusAccessor" OR
       NOT http2_event_test MATCHES
           "!HasResponseHeadAcceptedAccessor" OR
       NOT http2_event_test MATCHES
           "!HasResponseHeadPlanAccessor" OR
       NOT http2_event_test MATCHES
           "HasResponseHeadErrorAccessor" OR
       NOT http_package_consumer MATCHES
           "Http2BufferedResponseHeadSubmitResult" OR
       NOT http_package_consumer MATCHES
           "Http2StreamingResponseHeadSubmitResult" OR
       NOT http_package_consumer MATCHES
           "const ruvia::detail::HttpBufferedResponseWritePlan[*]" OR
       NOT http_package_consumer MATCHES
           "const ruvia::detail::ResponseStreamCommitPlan[*]" OR
       NOT http_package_consumer MATCHES
           "Http2ResponseHeadSubmitFailure" OR
       NOT http_package_consumer MATCHES
           "!HasResponseHeadStatusAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasResponseHeadAcceptedAccessor" OR
       NOT http_package_consumer MATCHES
           "!HasResponseHeadPlanAccessor")
        boundary_error("HTTP/2 response-head result contract is under-tested"
            "unit and installed-package consumers must pin plan-only success, error-only failure, and removed top-level accessors")
    endif()
endif()

set(HTTP2_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
if(EXISTS "${HTTP2_CONNECTION_HEADER}")
    file(READ "${HTTP2_CONNECTION_HEADER}" http2_connection_header)
    if(NOT http2_connection_header MATCHES "submitRegularRequestHead" OR
       NOT http2_connection_header MATCHES "Http2RequestContent content" OR
       NOT http2_connection_header MATCHES "Http2RequestHeadSubmitResult" OR
       NOT http2_connection_header MATCHES "Http2RequestHeadSubmitError" OR
       NOT http2_connection_header MATCHES "class Http2SubmittedRequestHead final" OR
       NOT http2_connection_header MATCHES
           "class Http2RequestHeadSubmitFailure final" OR
       NOT http2_connection_header MATCHES "streamId_ == 0" OR
       NOT http2_connection_header MATCHES "[(]streamId_ [&] 1U[)] == 0" OR
       NOT http2_connection_header MATCHES
           "std::variant<[ \t\r\n]*Http2SubmittedRequestHead,[ \t\r\n]*Http2RequestHeadSubmitFailure" OR
       NOT http2_connection_header MATCHES
           "Http2SubmittedRequestHead[*][ \t\r\n]+submitted[(][)] const [&] noexcept" OR
       NOT http2_connection_header MATCHES
           "Http2RequestHeadSubmitFailure[*][ \t\r\n]+failure[(][)] const [&] noexcept" OR
       NOT http2_connection_header MATCHES "kPeerStreamLimitReached" OR
       http2_connection_header MATCHES "submitRequestHead")
        boundary_error("HTTP/2 client request API restored an ambiguous framing entry"
            "request submission must keep dedicated entries and exclusive submitted/failure payloads")
    endif()
    if(NOT http2_connection_header MATCHES
           "class Http2ResponseHeadSubmitError final : public std::exception" OR
       NOT http2_connection_header MATCHES
           "class Http2ResponseHeadSubmitFailure final" OR
       NOT http2_connection_header MATCHES
           "class Http2ResponseHeadSubmitResult final" OR
       NOT http2_connection_header MATCHES
           "std::variant<Plan, Http2ResponseHeadSubmitFailure>" OR
       NOT http2_connection_header MATCHES "std::get_if<Plan>" OR
       NOT http2_connection_header MATCHES
           "std::get_if<Http2ResponseHeadSubmitFailure>" OR
       NOT http2_connection_header MATCHES
           "Http2BufferedResponseHeadSubmitResult" OR
       NOT http2_connection_header MATCHES
           "Http2StreamingResponseHeadSubmitResult" OR
       NOT http2_connection_header MATCHES
           "peerClosed[(][)] const noexcept" OR
       NOT http2_connection_header MATCHES
           "exception[(][)] const noexcept" OR
       http2_connection_header MATCHES
           "Http2ResponseHeadSubmitFailure[^}]*error[(][)]" OR
       http2_connection_header MATCHES
           "Http2SubmittedResponseHead|Http2SubmittedBufferedResponseHead|Http2SubmittedStreamingResponseHead")
        boundary_error("HTTP/2 final response-head result lost exclusive ownership"
            "the successful variant must directly own its buffered/streaming plan and only failures may expose their typed error")
    endif()
    if(NOT http2_connection_header MATCHES "submitConnectRequestHead" OR
       NOT http2_connection_header MATCHES "submitExtendedConnectRequestHead" OR
       NOT http2_connection_header MATCHES "submitConnectResponseHead" OR
       NOT http2_connection_header MATCHES "kPeerCapabilityUnavailable")
        boundary_error("HTTP/2 CONNECT restored an implicit request path"
            "standard, extended, server acceptance, and SETTINGS-gate statuses need dedicated API")
    endif()
    if(NOT http2_connection_header MATCHES "Http2Role role" OR
       NOT http2_connection_header MATCHES "beginConnection" OR
       NOT http2_connection_header MATCHES "enum class Http2FeedResult" OR
       NOT http2_connection_header MATCHES "enum class PrefacePhase" OR
       NOT http2_connection_header MATCHES "kNotStarted" OR
       NOT http2_connection_header MATCHES "kAwaitingClientMagic" OR
       NOT http2_connection_header MATCHES "kAwaitingPeerSettings" OR
       NOT http2_connection_header MATCHES "kConnectionNotStarted" OR
       NOT http2_connection_header MATCHES "kEventsPending" OR
       NOT http2_connection_header MATCHES "kAccepted" OR
       NOT http2_connection_header MATCHES "kNeedInput" OR
       NOT http2_connection_header MATCHES "kProtocolFailure")
        boundary_error("HTTP/2 connection startup restored ambiguous configuration ordering"
            "role-aware startup and the direct all-or-nothing feed ownership enum must remain")
    endif()
    if(NOT http2_connection_header MATCHES "releaseReceivedData" OR
       http2_connection_header MATCHES "Http2ConnectionLimits" OR
       http2_connection_header MATCHES "deferStreamWindowRelease" OR
       http2_connection_header MATCHES "releaseStreamWindow")
        boundary_error("HTTP/2 DATA flow control regained implicit runtime policy"
            "non-empty DATA events must retain receive credit until the owner calls releaseReceivedData; route limits and defer-mode toggles do not belong in the protocol core")
    endif()
    if(NOT http2_connection_header MATCHES
           "ruvia/http/detail/http2/Http2Event.h" OR
       NOT http2_connection_header MATCHES
           "std::optional<Http2PeerGoaway> peerGoaway" OR
       NOT http2_connection_header MATCHES
           "std::optional<Http2ErrorCode> connectionError" OR
       NOT http2_connection_header MATCHES
           "std::optional<Http2Event> nextEvent")
        boundary_error("HTTP/2 peer GOAWAY lost typed lifecycle observability"
            "optional event draining, last-stream-id, error code, and per-request safe-retry events must remain public protocol facts")
    endif()
endif()

set(WEB_HTTP2_STREAM_RUNTIME
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoStreamRuntime.h")

check_files_no_match("stream request-body completion must commit once after the full decode pipeline"
    "${RULE_STALE_STREAM_BODY_COMPLETION_SPLIT}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderChunked.inl")
set(WEB_HTTP2_WS_TRANSPORT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
set(WEB_HTTP2_RESPONSE_STREAM_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
if(NOT EXISTS "${WEB_HTTP2_STREAM_RUNTIME}" OR
   NOT EXISTS "${WEB_HTTP2_WS_TRANSPORT}" OR
   NOT EXISTS "${WEB_HTTP2_RESPONSE_STREAM_SINK}")
    boundary_error("Web-owned HTTP/2 stream runtime is missing"
        "stable per-stream route/body/signal storage and asynchronous consumers must live under ruvia-web/include/ruvia/web/detail/http2")
else()
    file(READ "${WEB_HTTP2_STREAM_RUNTIME}" web_http2_stream_runtime)
    file(READ "${WEB_HTTP2_SESSION}" web_http2_stream_session)
    file(READ "${WEB_HTTP2_WS_TRANSPORT}" web_http2_ws_transport)
    file(READ "${WEB_HTTP2_RESPONSE_STREAM_SINK}"
        web_http2_response_stream_sink)
    if(NOT web_http2_stream_runtime MATCHES
           "class Http2SansIoStreamSignal final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2SansIoBodyQueue final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2RequestBodyRuntime final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2BufferedRequestBody final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2StreamingRequestBody final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2SansIoSelectedRoute final" OR
       NOT web_http2_stream_runtime MATCHES
           "class Http2SansIoStreamRuntimeTable final" OR
       NOT web_http2_stream_runtime MATCHES "RequestBodyMode" OR
       NOT web_http2_stream_runtime MATCHES
           "using Storage = std::variant" OR
       NOT web_http2_stream_runtime MATCHES
           "std::optional<Http2SansIoSelectedRoute>" OR
       NOT web_http2_stream_runtime MATCHES "selectedRoute[(]" OR
       NOT web_http2_stream_runtime MATCHES "selectRoute" OR
       NOT web_http2_stream_runtime MATCHES "streamingBacklogLimit" OR
       NOT web_http2_stream_runtime MATCHES
           "using DispatchState = std::variant" OR
       NOT web_http2_stream_runtime MATCHES
           "struct AwaitingDispatch final" OR
       NOT web_http2_stream_runtime MATCHES
           "friend class Http2SansIoStreamRuntimeTable" OR
       NOT web_http2_stream_runtime MATCHES "beginDispatch" OR
       NOT web_http2_stream_runtime MATCHES "dispatchedCount" OR
       NOT web_http2_stream_runtime MATCHES "void forEach" OR
       NOT web_http2_stream_runtime MATCHES "makePmrObject" OR
       NOT web_http2_stream_runtime MATCHES
           "Http2SansIoStreamRuntime& ensureAccepted" OR
       NOT web_http2_stream_runtime MATCHES
           "const Http2StreamState& acceptedStream" OR
       web_http2_stream_runtime MATCHES
           "kModeNotSelected|modeSelected|selectedMode|std::optional<RequestBodyMode>|std::optional<RouteResolution>|routeResolution[(]" OR
       NOT web_http2_stream_session MATCHES
           "streamRuntimes[.]ensureAccepted[(]streamState[)]" OR
       NOT web_http2_stream_session MATCHES
           "selectedRoute[-][>]body[(][)]" OR
       NOT web_http2_ws_transport MATCHES "releaseReceivedData" OR
       NOT web_http2_ws_transport MATCHES "Http2SansIoStreamSignal&" OR
       NOT web_http2_response_stream_sink MATCHES
           "Http2SansIoStreamSignal&" OR
       web_http2_ws_transport MATCHES "Http2SansIoStreamSignal[*]" OR
       web_http2_response_stream_sink MATCHES
           "Http2SansIoStreamSignal[*]" OR
       web_http2_stream_runtime MATCHES
           "std::optional<Http2SansIoStreamSignal>|dispatchSignal_|bool[ \t]+hasQueuedChunk_|${RULE_STALE_HTTP2_BODY_MODE_SPLIT}" OR
       web_http2_ws_transport MATCHES
           "Http2BodyQueue|Http2StreamBodyQueue|class Http2SansIoStreamSignal final" OR
       web_http2_stream_runtime MATCHES
           "Http2LocalSettings|kMaxConcurrentStreams|Http2SansIoStreamRuntime[*][ \t\r\n]+ensure[(]" OR
       web_http2_stream_session MATCHES "ensureStreamRuntime")
        boundary_error("HTTP/2 Web stream runtime lost its discriminated ownership boundary"
            "route resolution and exactly one buffered/streaming body storage must commit together before dispatch, while queued storage remains the sole backlog-presence authority")
    endif()
endif()
if(EXISTS "${HTTP2_REQUEST_BUILDER}")
    file(READ "${HTTP2_REQUEST_BUILDER}" http2_external_body_builder)
    if(NOT http2_external_body_builder MATCHES
           "std::string_view[ \t\r\n]+body[ \t\r\n]*[)]" OR
       http2_external_body_builder MATCHES
           "std::string_view[ \t\r\n]+body[ \t\r\n]*=" OR
       NOT http2_external_body_builder MATCHES
           "setBody[(]request, body[)]")
        boundary_error("HTTP/2 request builder regained protocol-owned body storage"
            "the external runtime must pass an explicit body view; an empty default would hide the ownership boundary")
    endif()
endif()

set(WEB_HTTP_PROTOCOL_ERROR_INFO
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/HttpProtocolErrorInfo.h")
set(WEB_HTTP2_REQUEST_BUILD_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(NOT EXISTS "${HTTP2_REQUEST_BUILDER}" OR
   NOT EXISTS "${WEB_HTTP_PROTOCOL_ERROR_INFO}" OR
   NOT EXISTS "${WEB_HTTP2_REQUEST_BUILD_SESSION}")
    boundary_error("HTTP/2 request build failure contract is incomplete"
        "HTTP must own the discriminated protocol failure and Web must copy it into request lifetime")
else()
    file(READ "${HTTP2_REQUEST_BUILDER}" http2_request_build_contract)
    file(READ "${WEB_HTTP_PROTOCOL_ERROR_INFO}" web_protocol_error_info)
    file(READ "${WEB_HTTP2_REQUEST_BUILD_SESSION}" web_http2_request_build_session)
    if(NOT http2_request_build_contract MATCHES
           "class Http2RequestBuildResult final" OR
       NOT http2_request_build_contract MATCHES
           "using Value = std::variant<Http2RequestBuilt, Http2RequestBuildFailure>" OR
       NOT http2_request_build_contract MATCHES
           "HttpProtocolError protocolError[(][)] const noexcept" OR
       NOT http2_request_build_contract MATCHES
           "Http2RequestBuildResult build" OR
       http2_request_build_contract MATCHES
           "static bool build" OR
       NOT web_protocol_error_info MATCHES
           "copyHttpProtocolErrorInfo" OR
       NOT web_protocol_error_info MATCHES
           "resource->allocate" OR
       NOT web_http2_request_build_session MATCHES
           "requestBuild[.]failure[(][)]" OR
       NOT web_http2_request_build_session MATCHES
           "failure->protocolError[(][)]" OR
       NOT web_http2_request_build_session MATCHES
           "copyHttpProtocolErrorInfo" OR
       web_http2_request_build_session MATCHES
           "invalid http2 request headers")
        boundary_error("HTTP/2 request build failure escaped its typed protocol owner"
            "Web must consume one HTTP-owned failure and retain its diagnostic through the request arena")
    endif()
endif()

set(WEB_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
set(WEB_HTTP2_SERVER_ENTRY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerCleartextHttp2.h")
set(WEB_HTTP2_SESSION_FIXTURE
    "${RUVIA_ROOT}/tests/http2_sansio_session_fixture.h")
set(WEB_HTTP2_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
if(NOT EXISTS "${WEB_HTTP2_SESSION}" OR
   NOT EXISTS "${WEB_HTTP2_SERVER_ENTRY}" OR
   NOT EXISTS "${WEB_HTTP2_SESSION_FIXTURE}" OR
   NOT EXISTS "${WEB_HTTP2_PACKAGE_CONSUMER}")
    boundary_error("HTTP/2 session wiring contract is incomplete"
        "production context, server entry, test fixture, and installed-package assertion are all required")
else()
    file(READ "${WEB_HTTP2_SESSION}" web_http2_session)
    file(READ "${WEB_HTTP2_SERVER_ENTRY}" web_http2_server_entry)
    file(READ "${WEB_HTTP2_SESSION_FIXTURE}" web_http2_session_fixture)
    file(READ "${WEB_HTTP2_PACKAGE_CONSUMER}" web_http2_package_consumer)
    file(READ "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp"
        web_http2_session_test)
    if(NOT web_http2_session MATCHES
           "class Http2SansIoSessionContext final" OR
       NOT web_http2_session MATCHES "ContextServices services" OR
       NOT web_http2_session MATCHES "const HttpServerOptions& options" OR
       NOT web_http2_session MATCHES
           "ConnectionScanner::Entry& scannerEntry" OR
       NOT web_http2_session MATCHES
           "const HttpServerWorkerState& workerState" OR
       NOT web_http2_session MATCHES
           "const ContextServices& services[(][)]" OR
       NOT web_http2_session MATCHES
           "Http2SansIoSessionContext session" OR
       web_http2_session MATCHES "${RULE_STALE_HTTP2_SESSION_ENV}" OR
       NOT web_http2_server_entry MATCHES
           "Http2SansIoSessionContext[(]" OR
       NOT web_http2_server_entry MATCHES
           "ContextServices services" OR
       web_http2_server_entry MATCHES
           "${RULE_STALE_HTTP2_SESSION_ENV}" OR
       NOT web_http2_session_fixture MATCHES
           "class Http2SansIoSessionFixture final" OR
       NOT web_http2_session_fixture MATCHES
           "runBareHttp2SansIoSession" OR
       NOT web_http2_session_fixture MATCHES
           "runBarePlainHttp2SansIoSession" OR
       NOT web_http2_package_consumer MATCHES
           "!std::is_default_constructible_v<[ \t\r\n]*ruvia::detail::Http2SansIoSessionContext")
        boundary_error("HTTP/2 session restored nullable or test-shaped wiring"
            "the coroutine must receive one non-default context with mandatory options/scanner/shutdown references; bare defaults belong only to tests")
    endif()
    if(NOT web_http2_session MATCHES "event->tunnelData[(][)]" OR
       NOT web_http2_session MATCHES "event->tunnelEnd[(][)]")
        boundary_error("ruvia-web collapses tunnel bytes back into HTTP message content"
            "the HTTP/2 driver must consume the core's dedicated tunnel DATA and FIN events")
    endif()
    if(NOT web_http2_session MATCHES "event->streamClosed[(][)]" OR
       NOT web_http2_session MATCHES "eraseStreamRuntime[(]streamId[)]" OR
       NOT web_http2_session MATCHES "already-removed core state")
        boundary_error("ruvia-web re-derived an already-closed HTTP/2 stream"
            "stream-close cleanup must use the typed event ID without querying erased protocol state")
    endif()
    if(NOT web_http2_session MATCHES
           "Http2SansIoStreamRuntimeTable" OR
       NOT web_http2_session MATCHES
           "requestBody[.]store" OR
       NOT web_http2_session MATCHES "requestBodyByteLimit" OR
       NOT web_http2_session MATCHES "releaseReceivedData" OR
       NOT web_http2_session MATCHES "markBufferedBodyCopied" OR
       NOT web_http2_session MATCHES "unmarkBufferedBodyCopied" OR
       NOT web_http2_session MATCHES "resetEventStream" OR
       NOT web_http2_session MATCHES "Owner-side reset" OR
       NOT web_http2_session MATCHES
           "streamRuntimes[.]beginDispatch" OR
       NOT web_http2_session MATCHES
           "streamRuntimes[.]ensureAccepted[(]streamState[)]" OR
       NOT web_http2_session MATCHES "runtime[.]selectRoute" OR
       NOT web_http2_session MATCHES "routes[.]resolve[(]method, path[)]" OR
       NOT web_http2_session MATCHES "activeHandlerTasks" OR
       NOT web_http2_session MATCHES
           "while [(]activeHandlerTasks != 0[)]" OR
       NOT web_http2_session MATCHES "streamRuntimes[.]size" OR
       NOT web_http2_session MATCHES "streamRuntimes[.]forEach" OR
       NOT web_http2_session MATCHES "http2SansIoInactivityPhase" OR
       NOT web_http2_session_test MATCHES
           "sansio_driver_h2_inactivity_phase_counts_predispatch_runtime" OR
       web_http2_session MATCHES
           "${RULE_HTTP2_PARALLEL_WEB_DISPATCH_STATE}" OR
       web_http2_session MATCHES "${RULE_STALE_ROUTE_MODE_SPLIT}" OR
       web_http2_session MATCHES "${RULE_STALE_HTTP2_BODY_MODE_SPLIT}" OR
       web_http2_session MATCHES
           "Http2ConnectionLimits|HttpRequestBodyMode|setBodyMode|usesStreamRequestBody")
        boundary_error("ruvia-web HTTP/2 session bypasses Web-owned body storage"
            "one stable runtime must attach to an accepted protocol stream before route/body/signal ownership and co_spawn, owner resets must reclaim undispatched runtimes, and protocol streams must remain policy-free")
    endif()
    if(NOT web_http2_session MATCHES "feedAndDrain" OR
       NOT web_http2_session MATCHES "Http2FeedResult::kEventsPending" OR
       NOT web_http2_session MATCHES "Http2FeedResult::kConnectionNotStarted" OR
       NOT web_http2_session MATCHES "Http2FeedResult::kProtocolFailure" OR
       web_http2_session MATCHES "(result|feedResult)[.]consumed")
        boundary_error("ruvia-web bypasses HTTP/2 feed ownership"
            "all inbound spans must share drain/retry ownership, trust the direct enum, and stop on its typed terminal failure")
    endif()
endif()

set(CORE_SANSIO_DRIVER
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/SansIoDriver.h")
if(EXISTS "${CORE_SANSIO_DRIVER}")
    file(READ "${CORE_SANSIO_DRIVER}" core_sansio_driver)
    if(NOT core_sansio_driver MATCHES "typename ShouldStop" OR
       NOT core_sansio_driver MATCHES "if.*shouldStop.*connection")
        boundary_error("generic sans-I/O driver regained protocol lifecycle coupling"
            "the protocol adapter must explicitly supply an inlinable transport-stop predicate")
    endif()
endif()

foreach(web_h2_content_consumer IN ITEMS
    "${RUVIA_ROOT}/ruvia-web/src/server/Http2BufferedResponseWrite.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
    if(EXISTS "${web_h2_content_consumer}")
        file(READ "${web_h2_content_consumer}" web_h2_content_consumer_source)
        if(NOT web_h2_content_consumer_source MATCHES "kContentLengthExceeded" OR
           NOT web_h2_content_consumer_source MATCHES "kContentLengthIncomplete")
            file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${web_h2_content_consumer}")
            boundary_error("ruvia-web can busy-retry an HTTP/2 length rejection"
                "${relative} must handle both zero-ownership Content-Length statuses")
        endif()
    endif()
endforeach()

set(WEB_HTTP1_STREAM_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")
set(HTTP1_SERVER_CONNECTION_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerConnectionPlan.h")
set(HTTP1_SERVER_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP1_PARSER_INTERNAL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h")
set(HTTP1_PARSER_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
set(PUBLIC_HTTP1_REQUEST_PARSER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1RequestParser.h")
set(HTTP_PARSE_ERROR
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpParseError.h")
set(HTTP_HEADER_BLOCK_PARSER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h")
if(NOT EXISTS "${HTTP1_SERVER_CONNECTION_PLAN}")
    boundary_error("HTTP/1 server connection plan is missing"
        "ruvia-http must own Http1ServerConnectionPlan")
else()
    file(READ "${HTTP1_SERVER_CONNECTION_PLAN}" http1_server_connection_plan)
    if(NOT http1_server_connection_plan MATCHES "class Http1ServerConnectionPlan" OR
       NOT http1_server_connection_plan MATCHES "HttpProtocolVersion protocolVersion_" OR
       NOT http1_server_connection_plan MATCHES "protocolVersion[(][)] const noexcept" OR
       NOT http1_server_connection_plan MATCHES "http1PlanHttp10RequestConnection" OR
       NOT http1_server_connection_plan MATCHES "http1PlanHttp11RequestConnection" OR
       NOT http1_server_connection_plan MATCHES "http11Close" OR
       NOT http1_server_connection_plan MATCHES "requireClose")
        boundary_error("HTTP/1 connection plan lost part of its typed contract"
            "exact protocol version, disposition, version-specific parser construction, and close-only tightening must stay bound")
    endif()
    if(http1_server_connection_plan MATCHES
           "${RULE_STALE_HTTP1_RESPONSE_VERSION_SIGNAL}")
        boundary_error("HTTP/1 connection plan compressed its protocol version"
            "the exact HTTP/1.0 or HTTP/1.1 value must survive; responseSignal and the generic version factory are forbidden")
    endif()
endif()
if(EXISTS "${HTTP1_SERVER_SEMANTICS}")
    file(READ "${HTTP1_SERVER_SEMANTICS}" http1_server_semantics)
    if(NOT http1_server_semantics MATCHES "Http1RequestBodyConsumption" OR
       NOT http1_server_semantics MATCHES "Http1ServerConnectionPlan requestConnectionPlan" OR
       NOT http1_server_semantics MATCHES "Http1ServerClosePolicy closePolicy" OR
       NOT http1_server_semantics MATCHES "class Http1FinalResponseCommitFailure final" OR
       NOT http1_server_semantics MATCHES
           "Http1FinalResponseCommitError final : public std::exception" OR
       NOT http1_server_semantics MATCHES
           "what[(][)] const noexcept override" OR
       NOT http1_server_semantics MATCHES
           "Http1FinalResponseCommitError exception[(][)] const noexcept" OR
       http1_server_semantics MATCHES
           "Http1FinalResponseControlPlanError error[(][)] const" OR
       NOT http1_server_semantics MATCHES "class Http1FinalResponseCommitResult final" OR
       NOT http1_server_semantics MATCHES
           "std::variant<[ \t\r\n]*Http1ServerConnectionPlan,[ \t\r\n]*Http1FinalResponseCommitFailure>" OR
       NOT http1_server_semantics MATCHES
           "std::get_if<Http1ServerConnectionPlan>" OR
       NOT http1_server_semantics MATCHES "class PreparedHttp1ResponseStreamResult final" OR
       http1_server_semantics MATCHES "takePrepared[(][)]" OR
       NOT http1_server_semantics MATCHES "http1CommitFinalResponse" OR
       NOT http1_server_semantics MATCHES "std::nullopt" OR
       NOT http1_server_semantics MATCHES "bodyPlan\\.bodySuppressed\\(\\)" OR
       NOT http1_server_semantics MATCHES "plan[.]protocolVersion[(][)]" OR
       http1_server_semantics MATCHES
           "class Http1FinalResponseCommit final|committed[(][)]->connectionPlan[(][)]")
        boundary_error("HTTP/1 connection lifecycle lost its commit-time typed plan"
            "request version, shared final-control result, runtime policy, response body semantics, status-line bytes, and socket disposition must share one typed path")
    endif()
    if(http1_server_semantics MATCHES "http1RequestNeedsKeepAliveSignal" OR
       http1_server_semantics MATCHES "needsKeepAliveSignal" OR
       http1_server_semantics MATCHES "http1RequestConnectionDisposition" OR
       http1_server_semantics MATCHES "http1FinalizeResponseConnection" OR
       http1_server_semantics MATCHES "throw std::invalid_argument")
        boundary_error("HTTP/1 connection plan was split back into scalar facts"
            "exact protocol version, disposition, and typed final-commit failure must remain bound")
    endif()
    if(http1_server_semantics MATCHES "httpFinalResponseControlPlan" OR
       NOT http1_server_semantics MATCHES "http1FinalResponseControlPlan")
        boundary_error("HTTP/1 final response control restored protocol redispatch"
            "the HTTP/1 commit path must call its protocol-specific control planner directly")
    endif()
endif()
set(WEB_HTTP1_FINAL_RESPONSE_COMMIT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseState.h")
set(WEB_HTTP1_STREAM_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
set(HTTP1_FINAL_COMMIT_TEST
    "${RUVIA_ROOT}/tests/unit_response_head_emit.cpp")
set(HTTP1_FINAL_COMMIT_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${WEB_HTTP1_FINAL_RESPONSE_COMMIT}" AND
   EXISTS "${WEB_HTTP1_STREAM_SINK}" AND
   EXISTS "${HTTP1_FINAL_COMMIT_TEST}" AND
   EXISTS "${HTTP1_FINAL_COMMIT_PACKAGE_CONSUMER}")
    file(READ "${WEB_HTTP1_FINAL_RESPONSE_COMMIT}"
        web_http1_final_response_commit)
    file(READ "${WEB_HTTP1_STREAM_SINK}" web_http1_stream_sink)
    file(READ "${HTTP1_FINAL_COMMIT_TEST}" http1_final_commit_test)
    file(READ "${HTTP1_FINAL_COMMIT_PACKAGE_CONSUMER}"
        http1_final_commit_package_consumer)
    if(web_http1_final_response_commit MATCHES
           "throwHttp1FinalResponseCommitFailure|Http1FinalResponseControlPlanError::|std::invalid_argument" OR
       NOT web_http1_final_response_commit MATCHES
           "requireHttp1FinalResponseCommit" OR
       NOT web_http1_final_response_commit MATCHES
           "http1CommitFinalResponse" OR
       NOT web_http1_final_response_commit MATCHES
           "throw failure->exception[(][)]" OR
       NOT web_http1_stream_sink MATCHES "prepareResult[.]failure[(][)]" OR
       NOT web_http1_stream_sink MATCHES "prepareResult[.]prepared[(][)]" OR
       NOT web_http1_stream_sink MATCHES "std::move[(][*]prepared[)]" OR
       NOT web_http1_stream_sink MATCHES
           "throw failure->exception[(][)]" OR
       web_http1_stream_sink MATCHES
           "throwHttp1FinalResponseCommitFailure|Http1FinalResponseControlPlanError::")
        boundary_error("Web bypassed the typed HTTP/1 final response commit"
            "buffered and streaming paths must propagate the same HTTP-owned failure without rebuilding diagnostics")
    endif()
    if(NOT http1_final_commit_test MATCHES
           "HasRawFinalCommitError" OR
       NOT http1_final_commit_test MATCHES
           "catch [(]const Http1FinalResponseCommitError& error[)]" OR
       NOT http1_final_commit_package_consumer MATCHES
           "HasRawHttp1FinalCommitError" OR
       NOT http1_final_commit_package_consumer MATCHES
           "Http1FinalResponseCommitError,[ \t\r\n]*std::exception" OR
       NOT http1_final_commit_package_consumer MATCHES
           "is_trivially_copyable_v<[ \t\r\n]*ruvia::detail::Http1FinalResponseCommitResult")
        boundary_error("HTTP/1 final commit failure classification is insufficiently tested"
            "unit and installed consumers must pin the HTTP-owned exception and removal of raw commit errors")
    endif()
endif()
if(EXISTS "${HTTP1_PARSER_INTERNAL}" AND EXISTS "${HTTP1_PARSER_SOURCE}")
    file(READ "${HTTP1_PARSER_INTERNAL}" http1_parser_internal)
    file(READ "${HTTP1_PARSER_SOURCE}" http1_parser_source)
    if(NOT http1_parser_internal MATCHES "Http1ServerConnectionPlan connectionPlan" OR
       NOT http1_parser_source MATCHES
           "http1PlanHttp10RequestConnection" OR
       NOT http1_parser_source MATCHES
           "http1PlanHttp11RequestConnection" OR
       NOT http1_parser_source MATCHES
           "state[.]connectionPlan[.]requireClose[(][)]")
        boundary_error("HTTP/1 parser stopped owning the connection plan"
            "the validated request version and flags must produce one plan stored in Http1ServerRequestParseState, and body-framing failure must preserve its version while forcing close")
    endif()
    if(NOT http1_parser_internal MATCHES "class Http1ServerNeedRequestHead final" OR
       NOT http1_parser_internal MATCHES "class Http1ServerRequestHeadReady final" OR
       NOT http1_parser_internal MATCHES "class Http1ServerNeedRequestBody final" OR
       NOT http1_parser_internal MATCHES "class Http1ServerRequestMessageReady final" OR
       NOT http1_parser_internal MATCHES "class Http1ServerRequestParseFailure final" OR
       NOT http1_parser_internal MATCHES "class Http1ServerRequestParseState final" OR
       NOT http1_parser_internal MATCHES
           "const Http1ServerNeedRequestHead[*][\r\n \t]*needRequestHead[(][)] const [&] noexcept" OR
       NOT http1_parser_internal MATCHES
           "const Http1ServerRequestHeadReady[*][\r\n \t]*headReady[(][)] const [&] noexcept" OR
       NOT http1_parser_internal MATCHES
           "const Http1ServerNeedRequestBody[*][\r\n \t]*needRequestBody[(][)] const [&] noexcept" OR
       NOT http1_parser_internal MATCHES
           "const Http1ServerRequestMessageReady[*][\r\n \t]*messageReady[(][)] const [&] noexcept" OR
       NOT http1_parser_internal MATCHES
           "const Http1ServerRequestParseFailure[*][\r\n \t]*failure[(][)] const [&] noexcept" OR
       NOT http1_parser_internal MATCHES
           "needRequestHead[(][)] const && = delete" OR
       NOT http1_parser_internal MATCHES
           "headReady[(][)] const && = delete" OR
       NOT http1_parser_internal MATCHES
           "needRequestBody[(][)] const && = delete" OR
       NOT http1_parser_internal MATCHES
           "messageReady[(][)] const && = delete" OR
       NOT http1_parser_internal MATCHES
           "failure[(][)] const && = delete" OR
       NOT http1_parser_internal MATCHES
           "HttpProtocolError protocolError[(][)] const noexcept" OR
       NOT http1_parser_internal MATCHES
           "Http1ServerRequestParseFailureSource[\r\n \t]+source[(][)] const noexcept" OR
       NOT http1_parser_internal MATCHES "using Progress = std::variant" OR
       NOT http1_parser_internal MATCHES "class Http1ServerRequestParser final" OR
       NOT http1_parser_internal MATCHES "void parseHead" OR
       NOT http1_parser_internal MATCHES "parseMessage" OR
       NOT http1_parser_source MATCHES
           "state\\.progress_[ \\t]*=[ \\t]*Http1ServerRequestHeadReady" OR
       NOT http1_parser_source MATCHES
           "state\\.progress_[ \\t]*=[ \\t]*Http1ServerRequestMessageReady")
        boundary_error("HTTP/1 parser lost its discriminated progress state"
            "head/message readiness, body requirements, and failures must be separate lightweight alternatives while the heavy request storage stays reusable")
    endif()
    if(http1_parser_internal MATCHES "Http1ServerRequestParsePhase" OR
       http1_parser_internal MATCHES "phase_" OR
       http1_parser_internal MATCHES
           "std::optional<HttpParseError>[ \\t]+error_" OR
       http1_parser_source MATCHES
           "parsed\\.(headerBytes|messageBytes|requiredTotalBytes)")
        boundary_error("HTTP/1 parser restored parallel progress scalars"
            "phase-specific metadata must live only in its active Progress alternative")
    endif()
endif()
if(NOT EXISTS "${PUBLIC_HTTP1_REQUEST_PARSER}")
    boundary_error("public typed HTTP/1 request parser is missing"
        "ruvia-http/include/ruvia/http/Http1RequestParser.h")
else()
    file(READ "${PUBLIC_HTTP1_REQUEST_PARSER}" public_http1_request_parser)
    file(READ "${HTTP_PARSE_ERROR}" public_http_parse_error)
    file(READ "${HTTP_HEADER_BLOCK_PARSER}" http_header_block_parser)
    if(NOT public_http1_request_parser MATCHES "class Http1RequestNeedMore final" OR
       NOT public_http1_request_parser MATCHES "class Http1ParsedRequest final" OR
       NOT public_http1_request_parser MATCHES "class Http1RequestParseFailure final" OR
       NOT public_http1_request_parser MATCHES "class Http1RequestParseResult final" OR
       NOT public_http1_request_parser MATCHES "std::variant" OR
       NOT public_http1_request_parser MATCHES "requiredTotalBytes" OR
       NOT public_http1_request_parser MATCHES "bodyPlan" OR
       NOT public_http1_request_parser MATCHES "wireBody" OR
       NOT public_http1_request_parser MATCHES
           "needMore[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT public_http1_request_parser MATCHES
           "parsed[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT public_http1_request_parser MATCHES
           "failure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT public_http1_request_parser MATCHES
           "HttpProtocolError protocolError[(][)] const noexcept" OR
       public_http1_request_parser MATCHES
           "HttpParseError error[(][)] const noexcept" OR
       NOT public_http_parse_error MATCHES
           "HttpProtocolError httpParseProtocolError" OR
       public_http_parse_error MATCHES
           "httpParseError(Status|Message)" OR
       public_http1_request_parser MATCHES "HttpParseError::kNone" OR
       public_http_parse_error MATCHES "kNone" OR
       NOT http_header_block_parser MATCHES
           "std::optional<HttpParseError> parseHttpHeaderBlock" OR
       NOT public_http1_request_parser MATCHES
           "requiredTotalBytes_[^\r\n]*==[ \t]*0")
        boundary_error("public HTTP/1 parse result lost its discriminated contract"
            "need-more, parsed request, and failure must own disjoint facts while success retains framing bytes")
    endif()
endif()
check_files_no_match("HTTP parse failures recovered a no-error sentinel"
    "HttpParseError::kNone|kNone,[ \t\r\n]*kHeaderTooLarge"
    "${HTTP_PARSE_ERROR}"
    "${PUBLIC_HTTP1_REQUEST_PARSER}"
    "${HTTP1_PARSER_INTERNAL}"
    "${HTTP1_PARSER_SOURCE}"
    "${HTTP_HEADER_BLOCK_PARSER}"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp")
if(EXISTS "${HTTP1_PARSER_INTERNAL}")
    file(READ "${HTTP1_PARSER_INTERNAL}" public_http1_request_parser_state)
    if(NOT public_http1_request_parser_state MATCHES
           "requiredTotalBytes[(][)] const noexcept" OR
       NOT public_http1_request_parser_state MATCHES
           "messageBytes[(][)] const noexcept" OR
       NOT public_http1_request_parser_state MATCHES
           "std::optional<std::size_t> requiredTotalBytes_" OR
       NOT public_http1_request_parser_state MATCHES
           "std::size_t messageBytes_")
        boundary_error("HTTP/1 internal parse progress conflates completed and required bytes"
            "message bytes and optional required totals must belong to different alternatives")
    endif()
endif()
if(EXISTS "${HTTP1_PARSER_SOURCE}")
    file(READ "${HTTP1_PARSER_SOURCE}" public_http1_request_parser_source)
    if(NOT public_http1_request_parser_source MATCHES
           "needBody->requiredTotalBytes[(][)]" OR
       NOT public_http1_request_parser_source MATCHES
           "buffer\\.substr[(][\r\n \t]*message->headerBytes[(][)],[\r\n \t]*message->messageBytes[(][)][ \t]*-[ \t]*message->headerBytes[(][)]")
        boundary_error("public HTTP/1 parser collapsed required size or discarded wire body"
            "incomplete fixed lengths and successful body bytes need separate typed outputs")
    endif()
endif()
set(HTTP1_WEB_CONNECTION_STATE_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpConnectionState.h")
set(HTTP1_WEB_CONNECTION_STATE_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpConnectionState.cpp")
set(HTTP1_WEB_STREAM_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
if(EXISTS "${HTTP1_WEB_CONNECTION_STATE_SOURCE}" AND
   EXISTS "${HTTP1_WEB_STREAM_SESSION}")
    file(READ "${HTTP1_WEB_CONNECTION_STATE_SOURCE}"
        http1_web_connection_state_source)
    file(READ "${HTTP1_WEB_STREAM_SESSION}" http1_web_stream_session)
    if(NOT http1_web_stream_session MATCHES
           "if[ \t]*[(]const auto[*] requestHead = parsed[.]headReady[(][)][)]" OR
       NOT http1_web_stream_session MATCHES
           "requestHead->headerBytes[(][)]" OR
       NOT http1_web_stream_session MATCHES
           "growReadBuffer[(]readBuffer, usedBytes[)]" OR
       NOT http1_web_stream_session MATCHES
           "failure->protocolError[(][)]" OR
       NOT http1_web_stream_session MATCHES
           "failure->source[(][)]" OR
       http1_web_stream_session MATCHES
           "failure->error[(][)]|httpParseError(Status|Message)" OR
       http1_web_connection_state_source MATCHES
           "parsed[.]requiredTotalBytes")
        boundary_error("HTTP/1 Web runtime lost typed request-head metadata"
            "header bytes must flow from the HeadReady alternative, while header-buffer growth must not depend on whole-message body requirements")
    endif()
endif()
check_files_no_match("HTTP/1 Web runtime restored phase-wide parse scalars"
    "parsed[.](headerBytes|messageBytes|requiredTotalBytes)|growReadBuffer[(][^)]*Http1ServerRequestParseState"
    "${HTTP1_WEB_CONNECTION_STATE_HEADER}"
    "${HTTP1_WEB_CONNECTION_STATE_SOURCE}"
    "${HTTP1_WEB_STREAM_SESSION}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBufferedRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerResponseStreamRoute.h")

set(HTTP1_REQUEST_BODY_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h")
if(NOT EXISTS "${HTTP1_REQUEST_BODY_PLAN}")
    boundary_error("HTTP/1 request-body plan is missing"
        "ruvia-http must own Http1RequestBodyPlan")
else()
    file(READ "${HTTP1_REQUEST_BODY_PLAN}" http1_request_body_plan)
    if(NOT http1_request_body_plan MATCHES "class Http1RequestBodyPlan" OR
       NOT http1_request_body_plan MATCHES "class Http1RequestWithoutBody final" OR
       NOT http1_request_body_plan MATCHES "class Http1KnownLengthRequestBody final" OR
       NOT http1_request_body_plan MATCHES "class Http1ChunkedRequestBody final" OR
       NOT http1_request_body_plan MATCHES "using Framing = std::variant" OR
       NOT http1_request_body_plan MATCHES "std::get_if<Http1RequestWithoutBody>" OR
       NOT http1_request_body_plan MATCHES "std::get_if<Http1KnownLengthRequestBody>" OR
       NOT http1_request_body_plan MATCHES "std::get_if<Http1ChunkedRequestBody>" OR
       NOT http1_request_body_plan MATCHES "requiresConsumption" OR
       NOT http1_request_body_plan MATCHES "Http1RequestBodyConsumption" OR
       NOT http1_request_body_plan MATCHES "transferCodings" OR
       NOT http1_request_body_plan MATCHES "HttpRequestExpectations" OR
       NOT http1_request_body_plan MATCHES "expectationPlan" OR
       NOT http1_request_body_plan MATCHES
           "HttpUnsupportedExpectationPolicy" OR
       NOT http1_request_body_plan MATCHES "friend class Http1ServerRequestParseState" OR
       NOT http1_request_body_plan MATCHES "friend class Http1ServerRequestParser" OR
       NOT http1_request_body_plan MATCHES
           "explicit Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*HttpRequestExpectations[ \t]+expectations" OR
       NOT http1_request_body_plan MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*std::size_t[ \t]+contentLength" OR
       NOT http1_request_body_plan MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*HttpTransferCodings[ \t]+transferCodings")
        boundary_error("HTTP/1 request-body plan lost part of its typed contract"
            "parser-only exclusive framing alternatives, transfer decode order, consumption, and 100-continue must stay bound")
    endif()
endif()

set(HTTP1_SERVER_PARSER "${RUVIA_ROOT}/ruvia-http/src/parser/Http1RequestParser.cpp")
set(WEB_HTTP1_BODY_READER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h")
set(WEB_HTTP1_BODY_READER_CORE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl")
if(EXISTS "${HTTP1_SERVER_PARSER}")
    file(READ "${HTTP1_SERVER_PARSER}" http1_server_parser)
    if(NOT http1_server_parser MATCHES
           "const auto transferEncoding = block[.]transferEncoding[.]value[(][)]" OR
       NOT http1_server_parser MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*finalChunked->transferCodings[(][)]" OR
       NOT http1_server_parser MATCHES
           "const auto contentLength = block[.]contentLength[.]value[(][)]" OR
       NOT http1_server_parser MATCHES
           "Http1RequestBodyPlan[ \t\r\n]*[(][ \t\r\n]*[*]contentLength" OR
       NOT http1_server_parser MATCHES
           "Http1RequestBodyPlan[(]expectations[)]" OR
       NOT http1_server_parser MATCHES "bodyPlan[.]chunked[(][)]" OR
       NOT http1_server_parser MATCHES "bodyPlan[.]knownLength[(][)]" OR
       NOT http1_server_parser MATCHES "expectations[.]ignore100Continue")
        boundary_error("HTTP/1 parser bypasses the typed request-body plan"
            "Http1RequestParser.cpp must produce Http1RequestBodyPlan once after header validation")
    endif()
endif()
if(EXISTS "${WEB_HTTP1_BODY_READER}" AND EXISTS "${WEB_HTTP1_BODY_READER_CORE}")
    file(READ "${WEB_HTTP1_BODY_READER}" web_http1_body_reader)
    file(READ "${WEB_HTTP1_BODY_READER_CORE}" web_http1_body_reader_core)
    if(NOT web_http1_body_reader MATCHES "Http1RequestBodyPlan[ \t]+bodyPlan" OR
       NOT web_http1_body_reader MATCHES "readKnownLengthAll" OR
       NOT web_http1_body_reader MATCHES "readKnownLength" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_[.]withoutBody[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_[.]knownLength[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_[.]chunked[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "chunked->transferCodings[(][)]" OR
       NOT web_http1_body_reader_core MATCHES "bodyPlan_\\.expectationPlan" OR
       NOT web_http1_body_reader_core MATCHES "send100Continue")
        boundary_error("ruvia-web request-body reader bypasses the HTTP-owned plan"
            "StreamBodyReader must consume Http1RequestBodyPlan directly")
    endif()
endif()

set(HTTP_REQUEST_BODY_FAILURE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpRequestBodyFailure.h")
set(WEB_HTTP1_REQUEST_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerRequestState.h")
set(WEB_HTTP2_REQUEST_BODY_RUNTIME
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoStreamRuntime.h")
set(WEB_HTTP2_REQUEST_BODY_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(NOT EXISTS "${HTTP_REQUEST_BODY_FAILURE}")
    boundary_error("HTTP request-body failure contract is missing"
        "size and completion failures must map to HTTP errors in ruvia-http")
elseif(EXISTS "${WEB_HTTP1_BODY_READER_CORE}" AND
       EXISTS "${WEB_HTTP1_REQUEST_STATE}" AND
       EXISTS "${HTTP1_WEB_STREAM_SESSION}" AND
       EXISTS "${WEB_HTTP2_REQUEST_BODY_RUNTIME}" AND
       EXISTS "${WEB_HTTP2_REQUEST_BODY_SESSION}")
    file(READ "${HTTP_REQUEST_BODY_FAILURE}" http_request_body_failure)
    file(READ "${WEB_HTTP1_BODY_READER_CORE}" web_http1_body_reader_core)
    file(READ "${WEB_HTTP1_REQUEST_STATE}" web_http1_request_state)
    file(READ "${HTTP1_WEB_STREAM_SESSION}" web_http1_stream_session)
    file(READ "${WEB_HTTP2_REQUEST_BODY_RUNTIME}" web_http2_request_body_runtime)
    file(READ "${WEB_HTTP2_REQUEST_BODY_SESSION}" web_http2_request_body_session)
    if(NOT http_request_body_failure MATCHES
           "class HttpRequestBodyFailure final" OR
       NOT http_request_body_failure MATCHES "tooLarge[(][)]" OR
       NOT http_request_body_failure MATCHES "incomplete[(][)]" OR
       NOT http_request_body_failure MATCHES "protocolError[(][)]" OR
       NOT http_request_body_failure MATCHES
           "HttpProtocolError[(]413, [\"]request body is too large[\"]" OR
       NOT http_request_body_failure MATCHES
           "HttpProtocolError[(]400, [\"]incomplete request body[\"]" OR
       NOT http_request_body_failure MATCHES
           "httpRequestBodySizeFailure" OR
       NOT http_request_body_failure MATCHES
           "httpRequestBodyAdditionFailure" OR
       NOT web_http1_body_reader_core MATCHES
           "HttpRequestBodyFailure::tooLarge" OR
       NOT web_http1_body_reader_core MATCHES
           "HttpRequestBodyFailure::incomplete" OR
       NOT web_http1_request_state MATCHES
           "contentLengthLimitFailure" OR
       NOT web_http1_stream_session MATCHES
           "bodyFailure->protocolError[(][)]" OR
       web_http1_stream_session MATCHES
           "HttpErrorInfo[(]413|request body is too large" OR
       NOT web_http2_request_body_runtime MATCHES
           "class Http2RequestBodyStoreResult final" OR
       NOT web_http2_request_body_runtime MATCHES
           "using Value = std::variant" OR
       NOT web_http2_request_body_runtime MATCHES
           "protocolFailure[(][)] const [&]" OR
       NOT web_http2_request_body_runtime MATCHES
           "backlogOverflow[(][)] const [&]" OR
       NOT web_http2_request_body_runtime MATCHES
           "httpRequestBodyAdditionFailure" OR
       web_http2_request_body_runtime MATCHES
           "kTotalLimitExceeded|kBacklogLimitExceeded" OR
       NOT web_http2_request_body_session MATCHES "stored[.]stored[(][)]" OR
       NOT web_http2_request_body_session MATCHES
           "stored[.]protocolFailure[(][)]" OR
       NOT web_http2_request_body_session MATCHES
           "stored[.]backlogOverflow[(][)]")
        boundary_error("request-body failure ownership or store outcomes regressed"
            "HTTP must own 400/413 mapping while Web keeps typed stored, protocol-failure, and backlog outcomes distinct")
    endif()
endif()
check_files_no_match("request-body 413 mapping duplicated outside its HTTP failure contract"
    "[\"]request body is too large[\"]"
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpParseError.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/RequestBodyDecoding.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/body/HttpTransferCodingDecoder.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
    "${WEB_HTTP1_BODY_READER_CORE}"
    "${HTTP1_WEB_STREAM_SESSION}")

set(HTTP_REQUEST_BODY_FAILURE_TEST
    "${RUVIA_ROOT}/tests/unit_request_body_decoding.cpp")
set(WEB_HTTP2_REQUEST_BODY_TEST
    "${RUVIA_ROOT}/tests/unit_http2_sansio_stream_runtime.cpp")
if(EXISTS "${HTTP_REQUEST_BODY_FAILURE_TEST}" AND
   EXISTS "${WEB_HTTP2_REQUEST_BODY_TEST}")
    file(READ "${HTTP_REQUEST_BODY_FAILURE_TEST}" http_request_body_failure_test)
    file(READ "${WEB_HTTP2_REQUEST_BODY_TEST}" web_http2_request_body_test)
    if(NOT http_request_body_failure_test MATCHES
           "request_body_failures_own_cross_runtime_http_errors" OR
       NOT web_http2_request_body_test MATCHES "protocolFailure[(][)]" OR
       NOT web_http2_request_body_test MATCHES "backlogOverflow[(][)]")
        boundary_error("request-body failure contracts are under-tested"
            "tests must pin HTTP error mapping and distinct H2 total/backlog outcomes")
    endif()
endif()

set(HTTP_REQUEST_EXPECTATIONS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpExpectations.h")
set(HTTP1_HEADER_BLOCK_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h")
set(HTTP1_HEADER_BLOCK_PARSER
    "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp")
set(HTTP2_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(HTTP2_REQUEST_HEADERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestHeaders.h")
set(WEB_HTTP1_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
set(WEB_HTTP2_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(NOT EXISTS "${HTTP_REQUEST_EXPECTATIONS}")
    boundary_error("shared request-expectation state is missing"
        "HTTP/1 and HTTP/2 recipients must share HttpRequestExpectations")
elseif(EXISTS "${HTTP1_HEADER_BLOCK_STATE}" AND
       EXISTS "${HTTP1_HEADER_BLOCK_PARSER}" AND
       EXISTS "${HTTP2_STREAM_STATE}" AND
       EXISTS "${HTTP2_REQUEST_HEADERS}" AND
       EXISTS "${WEB_HTTP1_SESSION}" AND
       EXISTS "${WEB_HTTP2_SESSION}")
    file(READ "${HTTP_REQUEST_EXPECTATIONS}" http_request_expectations)
    file(READ "${HTTP1_HEADER_BLOCK_STATE}" http1_header_block_state)
    file(READ "${HTTP1_HEADER_BLOCK_PARSER}" http1_header_block_parser)
    file(READ "${HTTP2_STREAM_STATE}" http2_stream_state)
    file(READ "${HTTP2_REQUEST_HEADERS}" http2_request_headers)
    file(READ "${WEB_HTTP1_SESSION}" web_http1_session)
    file(READ "${WEB_HTTP2_SESSION}" web_http2_session)
    if(NOT http_request_expectations MATCHES "class HttpRequestExpectations" OR
       NOT http_request_expectations MATCHES "httpVisitCommaSeparatedQuoted" OR
       NOT http_request_expectations MATCHES "HttpRequestContentIndication" OR
       NOT http_request_expectations MATCHES "HttpServerExpectationPlan" OR
       NOT http_request_expectations MATCHES "std::variant" OR
       NOT http_request_expectations MATCHES
           "HttpUnsupportedExpectationPolicy" OR
       NOT http_request_expectations MATCHES
           "HttpUnsupportedExpectationRejection" OR
       NOT http_request_expectations MATCHES "protocolError" OR
       NOT http_request_expectations MATCHES
           "HttpProtocolError[(]417, [\"]unsupported Expect header[\"]" OR
       NOT http_request_expectations MATCHES "kNoContent" OR
       NOT http_request_expectations MATCHES
           "HttpServerExpectationPlan serverPlan" OR
       NOT http_request_expectations MATCHES "kReject" OR
       NOT http1_header_block_state MATCHES "HttpRequestExpectations expectations" OR
       NOT http1_header_block_parser MATCHES "expectations[.]parseField" OR
       NOT http2_stream_state MATCHES "HttpRequestExpectations expectations_" OR
       NOT http2_stream_state MATCHES "HttpServerExpectationPlan" OR
       NOT http2_request_headers MATCHES "parseRequestExpectationField" OR
       NOT web_http1_session MATCHES "expectationPlan" OR
       NOT web_http1_session MATCHES "rejection[(][)]" OR
       NOT web_http1_session MATCHES "copyHttpProtocolErrorInfo" OR
       NOT web_http2_session MATCHES "submitInterimResponseHead" OR
       NOT web_http2_session MATCHES "HttpInterimResponseHead[(]100" OR
       NOT web_http2_session MATCHES "expectationPlan" OR
       NOT web_http2_session MATCHES "send100Continue" OR
       NOT web_http2_session MATCHES "rejection[(][)]" OR
       NOT web_http2_session MATCHES "copyHttpProtocolErrorInfo" OR
       web_http1_session MATCHES "HttpErrorInfo[(]417" OR
       web_http2_session MATCHES "HttpErrorInfo[(]417")
        boundary_error("server Expect ownership has split across protocol versions"
            "Web must choose the unsupported-extension policy while HTTP owns typed no-action, 100, and 417 outcomes")
    endif()
endif()
set(HTTP1_CLIENT_EXPECTATION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp")
set(HTTP2_CLIENT_EXPECTATION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2ConnectionSubmit.cpp")
if(NOT EXISTS "${HTTP1_CLIENT_EXPECTATION_SOURCE}" OR
   NOT EXISTS "${HTTP2_CLIENT_EXPECTATION_SOURCE}")
    boundary_error("shared client expectation semantics are missing"
        "HTTP/1 and HTTP/2 client writers must consume one content-indication check")
else()
    file(READ "${HTTP_REQUEST_EXPECTATIONS}"
        http_client_request_expectations)
    file(READ "${HTTP1_CLIENT_EXPECTATION_SOURCE}"
        http1_client_expectation_source)
    file(READ "${HTTP2_CLIENT_EXPECTATION_SOURCE}"
        http2_client_expectation_source)
    if(NOT http_client_request_expectations MATCHES
           "httpClientExpectationIsValid" OR
       NOT http1_client_expectation_source MATCHES
           "detail::httpClientExpectationIsValid" OR
       NOT http2_client_expectation_source MATCHES
           "httpClientExpectationIsValid" OR
       NOT http2_client_expectation_source MATCHES
           "expectations[.]parseField" OR
       NOT http2_client_expectation_source MATCHES
           "HttpRequestContentIndication::kNoContent")
        boundary_error("client 100-continue semantics split by HTTP version"
            "every client request shape must reject 100-continue unless request content follows the initial head")
    endif()
endif()
check_files_no_match("server Expect plans restored weak enum or optional actions"
    "HttpServerExpectationAction|expectationAction|serverAction|std::optional<HttpServerExpectation"
    "${HTTP_REQUEST_EXPECTATIONS}"
    "${HTTP1_REQUEST_BODY_PLAN}"
    "${HTTP2_STREAM_STATE}"
    "${WEB_HTTP1_SESSION}"
    "${WEB_HTTP2_SESSION}"
    "${RUVIA_ROOT}/tests/unit_header_params.cpp"
    "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp"
    "${RUVIA_ROOT}/tests/unit_http2_request_headers.cpp"
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")

set(HTTP_EXPECTATION_LIST_TEST "${RUVIA_ROOT}/tests/unit_header_params.cpp")
set(HTTP1_EXPECTATION_TEST "${RUVIA_ROOT}/tests/unit_http1_parser.cpp")
set(HTTP2_EXPECTATION_HEADER_TEST "${RUVIA_ROOT}/tests/unit_http2_request_headers.cpp")
set(HTTP2_EXPECTATION_RUNTIME_TEST "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
set(HTTP2_CLIENT_EXPECTATION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(HTTP_EXPECTATION_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${HTTP_EXPECTATION_LIST_TEST}" AND
   EXISTS "${HTTP1_EXPECTATION_TEST}" AND
   EXISTS "${HTTP2_EXPECTATION_HEADER_TEST}" AND
   EXISTS "${HTTP2_EXPECTATION_RUNTIME_TEST}" AND
   EXISTS "${HTTP2_CLIENT_EXPECTATION_TEST}" AND
   EXISTS "${HTTP_EXPECTATION_PACKAGE_CONSUMER}")
    file(READ "${HTTP_EXPECTATION_LIST_TEST}" http_expectation_list_test)
    file(READ "${HTTP1_EXPECTATION_TEST}" http1_expectation_test)
    file(READ "${HTTP2_EXPECTATION_HEADER_TEST}" http2_expectation_header_test)
    file(READ "${HTTP2_EXPECTATION_RUNTIME_TEST}" http2_expectation_runtime_test)
    file(READ "${HTTP2_CLIENT_EXPECTATION_TEST}"
        http2_client_expectation_test)
    file(READ "${HTTP_EXPECTATION_PACKAGE_CONSUMER}"
        http_expectation_package_consumer)
    if(NOT http_expectation_list_test MATCHES
           "client_expectation_requires_following_content" OR
       NOT http_expectation_list_test MATCHES
           "expectations_parse_one_logical_recipient_list" OR
       NOT http_expectation_list_test MATCHES
           "expectations_preserve_unsupported_extensions_as_semantics" OR
       NOT http1_expectation_test MATCHES
           "http1_public_parser_preserves_expect_extensions_as_semantics" OR
       NOT http2_expectation_header_test MATCHES
           "h2_headers_expect_is_an_extensible_repeated_list" OR
       NOT http2_expectation_runtime_test MATCHES
           "sansio_driver_h2_expectation_decision_precedes_request_content" OR
       NOT http2_client_expectation_test MATCHES
           "http2_connection_rejects_100_continue_without_following_content_transactionally" OR
       NOT http_expectation_package_consumer MATCHES
           "httpClientExpectationIsValid")
        boundary_error("cross-version Expect contract is under-tested"
            "tests must pin client content gating, list/repeat/empty parsing, semantic extensions, H2 100-before-DATA, and immediate Web 417")
    endif()
endif()

set(HTTP1_REQUEST_BODY_TEST
    "${RUVIA_ROOT}/tests/unit_request_body_decoding.cpp")
if(EXISTS "${HTTP1_REQUEST_BODY_TEST}")
    file(READ "${HTTP1_REQUEST_BODY_TEST}" http1_request_body_test)
    if(NOT http1_request_body_test MATCHES
           "http1_request_body_plan_has_one_framing_truth" OR
       NOT http1_request_body_test MATCHES
           "transfer_coded_chunked_request_plan_drives_decode_order" OR
       NOT http1_request_body_test MATCHES
           "!HasPublicRequestBodyPlanFactories<Http1RequestBodyPlan>" OR
       NOT http1_request_body_test MATCHES
           "!HasRequestBodyMode<Http1RequestBodyPlan>" OR
       NOT http1_request_body_test MATCHES
           "HasRequestContentLength<ruvia::detail::Http1KnownLengthRequestBody>" OR
       NOT http1_request_body_test MATCHES
           "HasRequestTransferCodings<ruvia::detail::Http1ChunkedRequestBody>" OR
       NOT http1_request_body_test MATCHES
           "!std::default_initializable<Http1RequestBodyPlan>")
        boundary_error("HTTP/1 request-body alternative ownership is under-tested"
            "tests must prove parser-only construction, exclusive alternative payloads, explicit Content-Length: 0, and dechunk-before-transfer-decoding order")
    endif()
endif()

set(HTTP1_CLIENT_REQUEST_WRITER_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h")
set(HTTP1_CLIENT_REQUEST_WRITER
    "${RUVIA_ROOT}/ruvia-http/src/client/Http1ClientRequestWriter.cpp")
set(HTTP1_CLIENT_REQUEST_TEST
    "${RUVIA_ROOT}/tests/unit_http_client_request.cpp")
if(NOT EXISTS "${HTTP1_CLIENT_REQUEST_WRITER_HEADER}" OR
   NOT EXISTS "${HTTP1_CLIENT_REQUEST_WRITER}")
    boundary_error("public HTTP/1 client request writer is missing"
        "ruvia-http must own the complete outbound HTTP/1 wire plan")
else()
    file(READ "${HTTP1_CLIENT_REQUEST_WRITER_HEADER}"
        http1_client_request_writer_header)
    file(READ "${HTTP1_CLIENT_REQUEST_WRITER}"
        http1_client_request_writer)
    if(NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestWirePolicy final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientNoRequestExpectation final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientContinueExpectation final" OR
       NOT http1_client_request_writer_header MATCHES
           "using Expectation = std::variant" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientNoRequestExpectation>" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientContinueExpectation>" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestContentPlan final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestWithoutContent final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientImmediateRequestContent final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientContinueGatedRequestContent final" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientRequestWithoutContent>" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientImmediateRequestContent>" OR
       NOT http1_client_request_writer_header MATCHES
           "std::get_if<Http1ClientContinueGatedRequestContent>" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestBufferTooSmall final" OR
       NOT http1_client_request_writer_header MATCHES
           "class PreparedHttp1ClientRequest final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestPrepareFailure final" OR
       NOT http1_client_request_writer_header MATCHES
           "class Http1ClientRequestPrepareResult final" OR
       NOT http1_client_request_writer_header MATCHES "std::variant" OR
       NOT http1_client_request_writer_header MATCHES
           "bufferTooSmall[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_client_request_writer_header MATCHES
           "prepared[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_client_request_writer_header MATCHES
           "failure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_client_request_writer_header MATCHES
           "std::span<char> headBuffer" OR
       NOT http1_client_request_writer_header MATCHES "prepareConnect" OR
       NOT http1_client_request_writer_header MATCHES
           "friend class Http1ClientResponseParser" OR
       http1_client_request_writer_header MATCHES "responseContext[ \t]*[(][ \t]*[)]" OR
       http1_client_request_writer_header MATCHES "expectsContinue")
        boundary_error("HTTP/1 client request writer lost its transactional contract"
            "buffer sizing, prepared content gate, typed failure, CONNECT, and Prepared-bound response state must remain one result")
    endif()
    if(NOT http1_client_request_writer MATCHES "isValidHttpMethodToken" OR
       NOT http1_client_request_writer MATCHES
           "isValidOriginOrAsteriskFormTarget" OR
       NOT http1_client_request_writer MATCHES "kHostPrefix" OR
       NOT http1_client_request_writer MATCHES "kContentLengthPrefix" OR
       NOT http1_client_request_writer MATCHES "kConnectionClose" OR
       NOT http1_client_request_writer MATCHES "kExpectContinue" OR
       NOT http1_client_request_writer MATCHES "kMaxHttpHeaderBytes" OR
       NOT http1_client_request_writer MATCHES "kMaxHttpHeaderFields" OR
       NOT http1_client_request_writer MATCHES "kExpectHeaderManagedByWriter" OR
       NOT http1_client_request_writer MATCHES "kExpectationWithoutContent" OR
       NOT http1_client_request_writer MATCHES
           "policy[.]continueExpectation[(][)] != nullptr" OR
       NOT http1_client_request_writer MATCHES
           "detail::httpClientExpectationIsValid" OR
       NOT http1_client_request_writer MATCHES "content[.]borrowedBytes" OR
       NOT http1_client_request_writer MATCHES "preparedWithoutContent" OR
       NOT http1_client_request_writer MATCHES "preparedImmediateContent" OR
       NOT http1_client_request_writer MATCHES
           "preparedContinueGatedContent" OR
       NOT http1_client_request_writer MATCHES
           "detail::httpRequestContentSemantics" OR
       NOT http1_client_request_writer MATCHES
           "HttpRequestContentSemantics::kForbidden" OR
       NOT http1_client_request_writer MATCHES
           "HttpRequestContentSemantics::kContentTypeRequired" OR
       NOT http1_client_request_writer MATCHES
           "headBuffer[.]size[(][)] < headBytes" OR
       http1_client_request_writer MATCHES "httpHasToken" OR
       http1_client_request_writer MATCHES "throw[ \t]" OR
       http1_client_request_writer MATCHES "std::pmr::string")
        boundary_error("HTTP/1 client request serialization split or became allocating"
            "one allocation-free writer must validate target/fields/method content, generate Host/framing/close, and size before writing")
    endif()
    if(http1_client_request_writer_header MATCHES
           "Http1ClientRequestExpectation" OR
       http1_client_request_writer_header MATCHES
           "enum class Http1ClientRequestExpectation[^{]*[{][^}]*kNone")
        boundary_error("HTTP/1 client wire policy restored an expectation sentinel"
            "without-expectation and 100-continue must remain exclusive typed alternatives")
    endif()
endif()
if(EXISTS "${HTTP1_CLIENT_REQUEST_TEST}")
    file(READ "${HTTP1_CLIENT_REQUEST_TEST}" http1_client_request_test)
    if(NOT http1_client_request_test MATCHES
           "http1_client_request_writer_emits_one_canonical_scatter_gather_plan" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_content_distinguishes_absent_from_explicit_empty" OR
       NOT http1_client_request_test MATCHES
           "http1_client_connect_entry_generates_authority_form_atomically" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_is_the_only_host_and_framing_owner" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_enforces_expect_content_semantics" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_enforces_method_content_semantics" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_writer_returns_exact_buffer_requirement_without_partial_output" OR
       NOT http1_client_request_test MATCHES
           "http1_client_request_context_binds_the_actual_close_signal")
        boundary_error("HTTP/1 client request writer invariants are under-tested"
            "tests must pin content presence/gating, target forms, Host/framing/Expect ownership, buffer atomicity, method semantics, and Prepared-bound response state")
    endif()
    if(NOT http1_client_request_test MATCHES
           "kWithoutExpectation[.]noExpectation[(][)] != nullptr" OR
       NOT http1_client_request_test MATCHES
           "kExpectContinue[.]continueExpectation[(][)] != nullptr")
        boundary_error("HTTP/1 client expectation alternatives are under-tested"
            "both wire-policy factories must expose only their active alternative")
    endif()
    if(NOT http1_client_request_test MATCHES
           "!AcceptsAnyTemporaryHttpClientRequestText<std::string>" OR
       NOT http1_client_request_test MATCHES
           "!AcceptsAnyTemporaryHttpClientRequestText<std::pmr::string>" OR
       NOT http1_client_request_test MATCHES
           "AcceptsLvalueHttpClientRequestText<std::string>")
        boundary_error("HTTP client request text lost its borrowed lifetime guard"
            "method/target must reject temporary owning strings while preserving borrowed lvalue input")
    endif()
    if(NOT http1_client_request_test MATCHES
           "!HasRequestContentMode<ruvia::HttpClientRequestContent>" OR
       NOT http1_client_request_test MATCHES
           "HasRequestContentValue<ruvia::HttpClientRequestBytes>" OR
       NOT http1_client_request_test MATCHES
           "!HasPreparedContentDisposition<[\r\n \t]*ruvia::Http1ClientRequestContentPlan>" OR
       NOT http1_client_request_test MATCHES
           "HasPreparedContentBytes<[\r\n \t]*ruvia::Http1ClientImmediateRequestContent>" OR
       NOT http1_client_request_test MATCHES
           "HasPreparedContentBytes<[\r\n \t]*ruvia::Http1ClientContinueGatedRequestContent>" OR
       NOT http1_client_request_test MATCHES
           "withoutContent[(][)][ 	]*==[ 	]*nullptr" OR
       NOT http1_client_request_test MATCHES
           "continueGated[(][)][ 	]*==[ 	]*nullptr")
        boundary_error("HTTP/1 outbound request-content alternatives are under-tested"
            "tests must reject plan-wide mode/payload access and prove absent, immediate-empty, and continue-gated exclusivity")
    endif()
endif()
set(HTTP1_CLIENT_API_SURFACE "${RUVIA_ROOT}/examples/api_surface.cpp")
set(HTTP1_CLIENT_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
if(EXISTS "${HTTP1_CLIENT_API_SURFACE}" AND
   EXISTS "${HTTP1_CLIENT_PACKAGE_CONSUMER}")
    file(READ "${HTTP1_CLIENT_API_SURFACE}" http1_client_api_surface)
    file(READ "${HTTP1_CLIENT_PACKAGE_CONSUMER}" http1_client_package_consumer)
    if(NOT http1_client_api_surface MATCHES
           "HasHttp1RequestBodyPlanAlternatives<[\r\n \t]*ruvia::detail::Http1RequestBodyPlan>" OR
       NOT http1_client_api_surface MATCHES
           "!HasPublicHttp1RequestBodyPlanFactories<[\r\n \t]*ruvia::detail::Http1RequestBodyPlan>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1RequestBodyContentLength<[\r\n \t]*ruvia::detail::Http1KnownLengthRequestBody>" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttp1RequestBodyPlanAlternatives" OR
       NOT http1_client_package_consumer MATCHES
           "!HasPublicHttp1RequestBodyPlanFactories" OR
       NOT http1_client_api_surface MATCHES
           "!HasRawHttpClientRequestBody<ruvia::HttpClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "HasDiscriminatedHttpClientRequestContent<ruvia::HttpClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttpClientRequestBorrowedText<ruvia::HttpClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "!AcceptsAnyTemporaryHttpClientRequestText<std::pmr::string>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttpClientRequestContentTuple<ruvia::HttpClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1ClientPreparedContentPlan<[\r\n \t]*ruvia::PreparedHttp1ClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1ClientExpectationAlternatives<[\r\n \t]*ruvia::Http1ClientRequestWirePolicy>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttp1ClientPreparedContentTuple<[\r\n \t]*ruvia::PreparedHttp1ClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttp1ClientResponseContext<[\r\n \t]*ruvia::PreparedHttp1ClientRequest>" OR
       NOT http1_client_api_surface MATCHES
           "std::is_constructible_v<[\r\n \t]*ruvia::Http1ClientResponseParser,[\r\n \t]*const ruvia::PreparedHttp1ClientRequest&>" OR
       NOT http1_client_api_surface MATCHES
           "!std::is_default_constructible_v<[\r\n \t]*ruvia::Http1ClientResponseParser>" OR
       NOT http1_client_api_surface MATCHES
           "!std::is_move_constructible_v<[\r\n \t]*ruvia::Http1ClientResponseParser>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1ClientResponsePlanAlternatives<[\r\n \t]*ruvia::Http1ClientResponsePlan>" OR
       NOT http1_client_api_surface MATCHES
           "std::same_as<std::optional<[\r\n \t]*ruvia::Http1ClientRequestContentSignal>>" OR
       NOT http1_client_api_surface MATCHES
           "!HasStaleHttp1ClientResponseMode<[\r\n \t]*ruvia::Http1ClientResponsePlan>" OR
       NOT http1_client_api_surface MATCHES
           "HasHttp1ClientResponsePersistence<[\r\n \t]*ruvia::Http1ClientInformationalResponse>" OR
       NOT http1_client_api_surface MATCHES
           "!HasHttp1ClientResponsePersistence<[\r\n \t]*ruvia::Http1ClientCloseDelimitedResponse>" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientRequestWriter" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttpClientRequestContentAlternatives" OR
       NOT http1_client_package_consumer MATCHES
           "!AcceptsAnyTemporaryHttpClientRequestText<std::pmr::string>" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttp1PreparedContentAlternatives" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttp1ClientExpectationAlternatives" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientRequestWirePolicy::expectContinue" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientRequestContentSignal::kContinue" OR
       NOT http1_client_package_consumer MATCHES "completeRequestContent" OR
       NOT http1_client_package_consumer MATCHES "HasHttp1ClientResponsePlanAlternatives" OR
       NOT http1_client_package_consumer MATCHES
           "std::same_as<std::optional<[\r\n \t]*ruvia::Http1ClientRequestContentSignal>>" OR
       NOT http1_client_package_consumer MATCHES "Http1ClientProtocolUpgrade" OR
       http1_client_package_consumer MATCHES "responseContext[(][)]")
        boundary_error("installed HTTP/1 API can bypass protocol preparation"
            "API surface must remove raw server/client body, context, and framing tuples; package consumers must use parser-only request alternatives and chain Prepared/content signals through exclusive client response alternatives")
    endif()
endif()

set(HTTP1_CLIENT_RESPONSE_PARSER_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h")
set(HTTP1_CLIENT_RESPONSE_PARSER
    "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientResponseParser.cpp")
check_files_no_match("HTTP/1 typed results must use one variant discriminator"
    "Http1(RequestParse|ClientResponseParse|ClientRequestPrepare|InterimResponsePrepare)Kind"
    "${PUBLIC_HTTP1_REQUEST_PARSER}"
    "${HTTP1_CLIENT_REQUEST_WRITER_HEADER}"
    "${HTTP1_CLIENT_RESPONSE_PARSER_HEADER}"
    "${HTTP1_INTERIM_RESPONSE_WRITER}")
set(HTTP_CONTENT_LENGTH_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentLength.h")
set(HTTP_TRANSFER_ENCODING_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpTransferEncoding.h")
set(HTTP_TRANSFER_CODING_VALUE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpTransferCoding.h")
foreach(obsolete_http1_client_response_header IN ITEMS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientResponseParser.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ClientResponsePlan.h")
    if(EXISTS "${obsolete_http1_client_response_header}")
        boundary_error("obsolete private HTTP/1 client response parser API was restored"
            "${obsolete_http1_client_response_header}")
    endif()
endforeach()
if(NOT EXISTS "${HTTP1_CLIENT_RESPONSE_PARSER_HEADER}")
    boundary_error("public HTTP/1 client response parser is missing"
        "ruvia-http must install Http1ClientResponseParser.h")
elseif(EXISTS "${HTTP1_CLIENT_RESPONSE_PARSER}")
    file(READ "${HTTP1_CLIENT_RESPONSE_PARSER_HEADER}" http1_client_response_parser_header)
    file(READ "${HTTP1_CLIENT_RESPONSE_PARSER}" http1_client_response_parser)
    if(NOT http1_client_response_parser_header MATCHES "Http1ClientRequestWriter[.]h" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponsePlan final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientInformationalResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseWithoutContent final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientKnownLengthResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientChunkedResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientCloseDelimitedResponse final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientConnectTunnel final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientProtocolUpgrade final" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientResponsePersistence" OR
       NOT http1_client_response_parser_header MATCHES "knownLength[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "closeDelimited[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "connectTunnel[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "protocolUpgrade[(][)]" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientRequestContentSignal" OR
       NOT http1_client_response_parser_header MATCHES "requestContentSignal" OR
       NOT http1_client_response_parser_header MATCHES
           "std::optional<Http1ClientRequestContentSignal>" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientRequestContentCompletionStatus" OR
       NOT http1_client_response_parser_header MATCHES "completeRequestContent" OR
       NOT http1_client_response_parser_header MATCHES "const PreparedHttp1ClientRequest& request" OR
       NOT http1_client_response_parser_header MATCHES
           "plan[.]continueGated" OR
       NOT http1_client_response_parser_header MATCHES
           "requestContentStartsComplete" OR
       NOT http1_client_response_parser_header MATCHES
           "enum class Http1ClientRequestContentPhase" OR
       NOT http1_client_response_parser_header MATCHES
           "kContentCompleteAwaitingContinue" OR
       NOT http1_client_response_parser_header MATCHES
           "kContinueReceivedContentComplete" OR
       NOT http1_client_response_parser_header MATCHES
           "initialRequestContentPhase" OR
       NOT http1_client_response_parser_header MATCHES
           "Http1ClientRequestContentPhase requestContentPhase_" OR
       http1_client_response_parser_header MATCHES
           "bool (continueGated_|sawContinue_|requestContentComplete_)" OR
       NOT http1_client_response_parser_header MATCHES "enum class Phase" OR
       NOT http1_client_response_parser_header MATCHES "kExchangeComplete" OR
       NOT http1_client_response_parser_header MATCHES "kExchangeFailed" OR
       NOT http1_client_response_parser_header MATCHES
           "kTooManyInformationalResponses" OR
       NOT http1_client_response_parser_header MATCHES
           "informationalResponseCount_" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseNeedMore final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ParsedClientResponseHead final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseParseFailure final" OR
       NOT http1_client_response_parser_header MATCHES "class Http1ClientResponseParseResult final" OR
       NOT http1_client_response_parser_header MATCHES "Http1ClientResponseParseError" OR
       NOT http1_client_response_parser_header MATCHES "std::variant" OR
       NOT http1_client_response_parser_header MATCHES "consumedBytes" OR
       NOT http1_client_response_parser_header MATCHES
           "const HttpClientResponseHead& head[(][)] const [&] noexcept" OR
       NOT http1_client_response_parser_header MATCHES
           "HttpClientResponseHead takeHead[(][)] && noexcept" OR
       http1_client_response_parser_header MATCHES
           "response[(][)] const [&]|takeResponse[(][)]" OR
       NOT http1_client_response_parser_header MATCHES
           "needMore[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_client_response_parser_header MATCHES
           "parsed[(][)] &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_client_response_parser_header MATCHES
           "parsed[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT http1_client_response_parser_header MATCHES
           "failure[(][)] const &&[ \\t]*=[ \\t]*delete")
        boundary_error("public HTTP/1 client response parser lost its discriminated contract"
            "NeedMore, owning Parsed, typed Failure, exact head consumption, and one immutable plan must stay bound")
    endif()
    if(http1_client_response_parser_header MATCHES
           "Http1ClientRequestContentSignal::kNone" OR
       http1_client_response_parser_header MATCHES
           "kNone,[\r\n \t]*kContinue")
        boundary_error("HTTP/1 client request-content signal restored a no-event sentinel"
            "absence must be represented by optional, while the enum contains only real protocol events")
    endif()
    if(NOT http1_client_response_parser MATCHES "findHttpHeaderEnd" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::withoutContent" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::[\r\n \t]*zeroContentKnownLength" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::[\r\n \t]*zeroContentChunked" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::[\r\n \t]*zeroContentCloseDelimited" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::knownLength" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::chunked" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::closeDelimited" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::connectTunnel" OR
       NOT http1_client_response_parser MATCHES "Http1ClientResponsePlanAccess::protocolUpgrade" OR
       NOT http1_client_response_parser MATCHES "using ResponsePlanningResult = std::variant" OR
       NOT http1_client_response_parser MATCHES "std::get_if<Http1ClientResponseParseError>" OR
       NOT http1_client_response_parser MATCHES "requestAllowsProtocolSwitch" OR
       NOT http1_client_response_parser MATCHES
           "kContinueReceivedContentComplete" OR
       NOT http1_client_response_parser MATCHES "requestContentSignal" OR
       NOT http1_client_response_parser MATCHES "receiveContinue" OR
       NOT http1_client_response_parser MATCHES
           "kMaxHttpClientInterimResponses" OR
       NOT http1_client_response_parser MATCHES
           "kTooManyInformationalResponses" OR
       NOT http1_client_response_parser MATCHES
           "kContentCompleteAwaitingContinue" OR
       http1_client_response_parser MATCHES
           "continueGated && !sawContinue|requestContentComplete_ = true|sawContinue_ = true" OR
       NOT http1_client_response_parser MATCHES "phase_ = Phase::kComplete" OR
       NOT http1_client_response_parser MATCHES "Http1ClientRequestContentSignal::kContinue" OR
       NOT http1_client_response_parser MATCHES "Http1ClientRequestContentSignal::kExchangeComplete" OR
       NOT http1_client_response_parser MATCHES
           "request[.]closePolicy[(][)] ==[\r\n \t]*Http1ClientRequestClosePolicy::kCloseAfterResponse" OR
       NOT http1_client_response_parser MATCHES "contentLengthFieldPresent" OR
       NOT http1_client_response_parser MATCHES
           "detail::httpResponseContentSemantics" OR
       NOT http1_client_response_parser MATCHES
           "detail::httpStatusCodeValid" OR
       http1_client_response_parser MATCHES "request[.]expectsContinue" OR
       http1_client_response_parser MATCHES
           "Http1ClientRequestContentSignal::kNone" OR
       http1_client_response_parser MATCHES "throw[ \t]+std::runtime_error")
        boundary_error("HTTP/1 client response parser bypasses its typed plan"
            "head scanning, Prepared-bound informational/final state, content signals, RFC body precedence, persistence, CONNECT, and Upgrade must have one output without wire exceptions")
    endif()
    if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/client/HttpClientRedirect.h")
        boundary_error("private redirect protocol header returned"
            "external sans-I/O redirect users must include the public HttpClientRedirect.h API")
    endif()
    file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
        http_client_redirect_header)
    file(READ "${RUVIA_ROOT}/ruvia-http/src/client/HttpClientRedirect.cpp"
        http_client_redirect)
    if(NOT http_client_redirect_header MATCHES "HttpClientRedirectRequestPlan" OR
       NOT http_client_redirect_header MATCHES "std::pmr::string method_" OR
       NOT http_client_redirect_header MATCHES "method[(][)] const [&] noexcept" OR
       NOT http_client_redirect_header MATCHES "method[(][)] const && = delete" OR
       NOT http_client_redirect MATCHES "httpPmrResourceOrDefault[(]resource[)]" OR
       NOT http_client_redirect MATCHES "request\\.method[ \t]*==[ \t]*\"POST\"" OR
       NOT http_client_redirect MATCHES "request\\.method[ \t]*==[ \t]*\"HEAD\"" OR
       http_client_redirect MATCHES "httpAsciiEqualsIgnoreCase\\([^,\r\n]*request\\.method")
        boundary_error("HTTP client redirect request plan lost ownership or RFC method semantics"
            "the plan must own its PMR method without temporary views; 303 selects GET/HEAD, only POST may become GET for 301/302, and tokens remain case-sensitive")
    endif()
    if(NOT http_client_redirect_header MATCHES "enum class HttpClientOriginAuthorityStatus" OR
       NOT http_client_redirect_header MATCHES "kSameOrigin" OR
       NOT http_client_redirect_header MATCHES "kDifferentOrigin" OR
       NOT http_client_redirect_header MATCHES "kInvalidAuthority" OR
       NOT http_client_redirect MATCHES "case HttpClientOriginAuthorityStatus::kInvalidAuthority" OR
       NOT http_client_redirect MATCHES "HttpClientRedirectTargetError::kInvalidLocation")
        boundary_error("HTTP client redirect collapsed authority syntax into origin equality"
            "malformed/userinfo authorities must be invalid Location values; only valid unequal origins are cross-origin")
    endif()
    if(NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderAbsent final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderFound final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderRepeated final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientResponseHeaderLookupResult final" OR
       NOT http_client_redirect_header MATCHES "std::get_if<HttpClientResponseHeaderFound>" OR
       NOT http_client_redirect_header MATCHES "enum class HttpClientRedirectTargetError" OR
       NOT http_client_redirect_header MATCHES "class HttpClientRedirectTarget final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientRedirectTargetFailure final" OR
       NOT http_client_redirect_header MATCHES "class HttpClientRedirectTargetResult final" OR
       NOT http_client_redirect_header MATCHES "std::get_if<HttpClientRedirectTarget>" OR
       NOT http_client_redirect_header MATCHES "std::get_if<HttpClientRedirectTargetFailure>" OR
       NOT http_client_redirect MATCHES "HttpClientRedirectTargetResult::makeTarget" OR
       NOT http_client_redirect MATCHES "HttpClientRedirectTargetResult::makeFailure")
        boundary_error("HTTP client redirect results lost their discriminated ownership"
            "only found may expose a borrowed field value, only target may own PMR bytes, and only failure may expose an error")
    endif()
    file(READ "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt" http_client_redirect_cmake)
    file(READ "${RUVIA_ROOT}/tests/unit_http_client_redirect.cpp" http_client_redirect_tests)
    file(READ "${RUVIA_ROOT}/tests/package-consumer/http.cpp" http_package_consumer)
    file(READ "${RUVIA_ROOT}/examples/api_surface.cpp" api_surface)
    if(NOT http_client_redirect_cmake MATCHES "src/client/HttpClientRedirect[.]cpp" OR
       NOT http_client_redirect_cmake MATCHES "include/ruvia/http/HttpClientRedirect[.]h" OR
       NOT http_client_redirect_tests MATCHES "HasHeaderValue" OR
       NOT http_client_redirect_tests MATCHES "HasRedirectTargetError" OR
       NOT http_package_consumer MATCHES "ruvia/http/HttpClientRedirect[.]h" OR
       NOT http_package_consumer MATCHES "HttpClientRedirectTargetResult" OR
       NOT api_surface MATCHES "ruvia/http/HttpClientRedirect[.]h" OR
       NOT api_surface MATCHES "HasHttpClientRedirectTargetAccessors")
        boundary_error("public redirect API is not pinned across consumers"
            "CMake install, unit ownership checks, package consumption, and API surface must compile the same result contract")
    endif()
endif()
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClient.h"
    public_http_client_value_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpClientRedirect.h"
    public_http_client_redirect_value_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h"
    public_http1_client_request_value_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h"
    public_http1_client_response_value_api)
file(READ "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/ProtocolByteLimit.h"
    public_protocol_byte_limit_api)
file(READ "${RUVIA_ROOT}/tests/package-consumer/http.cpp"
    public_http_value_package_contract)
file(READ "${RUVIA_ROOT}/examples/api_surface.cpp"
    public_http_value_api_surface)
file(READ "${RUVIA_ROOT}/tests/unit_http_client_response.cpp"
    public_http_client_response_owned_view_test)
file(READ "${RUVIA_ROOT}/tests/unit_http_client_redirect.cpp"
    public_http_client_redirect_owned_view_test)
if(NOT public_http_client_value_api MATCHES
       "withoutContent[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_value_api MATCHES
       "borrowedBytes[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_value_api MATCHES
       "name[(][)] const [&] noexcept" OR
   NOT public_http_client_value_api MATCHES
       "value[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_value_api MATCHES
       "headers[(][)] const [&] noexcept" OR
   public_http_client_value_api MATCHES
       "class HttpClientResponse final|body[(][)] const|body_[;]" OR
   NOT public_http_client_redirect_value_api MATCHES
       "absent[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_redirect_value_api MATCHES
       "found[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_redirect_value_api MATCHES
       "repeated[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_redirect_value_api MATCHES
       "target[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_redirect_value_api MATCHES
       "failure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_redirect_value_api MATCHES
       "value[(][)] const [&] noexcept" OR
   NOT public_http_client_redirect_value_api MATCHES
       "method[(][)] const [&] noexcept" OR
   NOT public_http_client_redirect_value_api MATCHES
       "method[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http_client_redirect_value_api MATCHES
       "std::pmr::string method_" OR
   NOT public_http_client_redirect_value_api MATCHES
       "lookupUniqueHttpClientResponseHeader[\r\n \t]*[(][\r\n \t]*const HttpClientResponseHead&&" OR
   NOT public_http1_client_request_value_api MATCHES
       "noExpectation[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_request_value_api MATCHES
       "continueExpectation[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_request_value_api MATCHES
       "withoutContent[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_request_value_api MATCHES
       "immediate[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_request_value_api MATCHES
       "continueGated[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "informational[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "withoutContent[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "zeroContent[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "knownLength[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "chunked[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "closeDelimited[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "connectTunnel[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "protocolUpgrade[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT public_http1_client_response_value_api MATCHES
       "detail::HttpTransferCodings[ \t\r\n]+transferCodings[(][)] const noexcept" OR
   NOT public_protocol_byte_limit_api MATCHES
       "std::optional<std::size_t> maximum[(][)] const noexcept" OR
   public_protocol_byte_limit_api MATCHES
       "const std::size_t[*] maximum[(][)]" OR
   NOT public_http_value_package_contract MATCHES
       "!HasAnyRvalueHttpClientRequestContentAccessor" OR
   NOT public_http_value_package_contract MATCHES
       "!HasAnyRvalueHttp1ClientRequestContentPlanAccessor" OR
   NOT public_http_value_package_contract MATCHES
       "!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor" OR
   NOT public_http_value_package_contract MATCHES
       "!HasAnyRvalueHttp1ClientResponsePlanAccessor" OR
   NOT public_http_value_package_contract MATCHES
       "!HasAnyRvalueHttpClientHeaderLookupAccessor" OR
   NOT public_http_value_package_contract MATCHES
       "!HasAnyRvalueHttpClientRedirectTargetAccessor" OR
   NOT public_http_value_package_contract MATCHES
       "ExposesAnyRvalueHttpClientOwnedView" OR
   NOT public_http_value_package_contract MATCHES
       "HasHttpClientResponseBody<ruvia::HttpClientResponseHead>" OR
   NOT public_http_value_package_contract MATCHES
       "AcceptsTemporaryHttpClientResponseHeaderLookup" OR
   NOT public_http_value_package_contract MATCHES
       "std::optional<std::size_t>" OR
   NOT public_http_value_api_surface MATCHES
       "!HasAnyRvalueHttpClientRequestContentAccessor" OR
   NOT public_http_value_api_surface MATCHES
       "!HasAnyRvalueHttp1ClientRequestContentPlanAccessor" OR
   NOT public_http_value_api_surface MATCHES
       "!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor" OR
   NOT public_http_value_api_surface MATCHES
       "!HasAnyRvalueHttp1ClientResponsePlanAccessor" OR
   NOT public_http_value_api_surface MATCHES
       "!HasAnyRvalueHttpClientHeaderLookupAccessor" OR
   NOT public_http_value_api_surface MATCHES
       "!HasAnyRvalueHttpClientRedirectTargetAccessor" OR
   NOT public_http_value_api_surface MATCHES
       "ExposesAnyRvalueHttpClientOwnedView" OR
   NOT public_http_value_api_surface MATCHES
       "AcceptsTemporaryHttpClientResponseHeaderLookup")
    boundary_error("public HTTP client values regained temporary borrow access"
        "owning responses, headers, redirect targets, and top-level alternatives must lend storage only from live lvalues")
endif()
if(NOT public_http_client_response_owned_view_test MATCHES
       "ExposesAnyRvalueHttpClientOwnedView" OR
   NOT public_http_client_response_owned_view_test MATCHES
       "HasHttpClientResponseBody<ruvia::HttpClientResponseHead>" OR
   NOT public_http_client_redirect_owned_view_test MATCHES
       "ExposesRvalueHttpClientRedirectTargetView" OR
   NOT public_http_client_redirect_owned_view_test MATCHES
       "AcceptsTemporaryHttpClientResponseHeaderLookup")
    boundary_error("HTTP client owned-view lifetime coverage is incomplete"
        "response, framing, redirect, and temporary lookup contracts require direct unit guards")
endif()
if(NOT EXISTS "${HTTP_CONTENT_LENGTH_STATE}")
    boundary_error("shared Content-Length state is missing"
        "HTTP/1 request and response parsers must share HttpContentLengthState")
else()
    file(READ "${HTTP_CONTENT_LENGTH_STATE}" http_content_length_state)
    if(NOT http_content_length_state MATCHES "class HttpContentLengthState" OR
       NOT http_content_length_state MATCHES "HttpContentLengthParseStatus::kConflicting" OR
       NOT http_content_length_state MATCHES "httpVisitCommaSeparatedQuotedItems" OR
       NOT http_content_length_state MATCHES "std::optional<std::size_t> value" OR
       NOT http_content_length_state MATCHES "auto parsedValue = value_" OR
       http_content_length_state MATCHES "bool present[(]" OR
       http_content_length_state MATCHES "bool present_" OR
       http_content_length_state MATCHES "std::size_t value_[{]0[}]")
        boundary_error("shared Content-Length parser lost full-list validation"
            "field updates must be transactional and absence must remain optional")
    endif()
endif()
if(NOT EXISTS "${HTTP_TRANSFER_ENCODING_STATE}")
    boundary_error("shared Transfer-Encoding state is missing"
        "HTTP/1 request and response parsers must share HttpTransferEncodingState")
else()
    file(READ "${HTTP_TRANSFER_ENCODING_STATE}" http_transfer_encoding_state)
    file(READ "${HTTP_TRANSFER_CODING_VALUE}" http_transfer_coding_value)
    file(READ "${HTTP1_REQUEST_BODY_PLAN}" http_transfer_coding_request_plan)
    file(READ "${HTTP1_CLIENT_RESPONSE_PARSER_HEADER}"
        http_transfer_coding_client_plan)
    if(NOT http_transfer_encoding_state MATCHES "class HttpTransferEncodingState" OR
       NOT http_transfer_encoding_state MATCHES "class HttpNonChunkedTransferEncoding" OR
       NOT http_transfer_encoding_state MATCHES "class HttpFinalChunkedTransferEncoding" OR
       NOT http_transfer_encoding_state MATCHES "class HttpTransferEncodingValue" OR
       NOT http_transfer_encoding_state MATCHES "std::variant" OR
       NOT http_transfer_encoding_state MATCHES "std::optional<HttpTransferEncodingValue>" OR
       NOT http_transfer_encoding_state MATCHES "auto next = value_" OR
       NOT http_transfer_encoding_state MATCHES "httpParseTransferCodingSyntax" OR
       NOT http_transfer_encoding_state MATCHES "httpValidTransferParameterValue" OR
       NOT http_transfer_encoding_state MATCHES "hasParameters"
       OR http_transfer_encoding_state MATCHES "bool present_"
       OR http_transfer_encoding_state MATCHES "bool finalChunked_"
       OR http_transfer_encoding_state MATCHES "bool present[(]")
        boundary_error("shared Transfer-Encoding parser lost ordered-list validation"
            "updates must be transactional and framing alternatives discriminated")
    endif()
    if(NOT http_transfer_coding_value MATCHES
           "is_trivially_copyable_v<HttpTransferCodings>" OR
       NOT http_transfer_coding_value MATCHES
           "sizeof[(]HttpTransferCodings[)] <= sizeof[(]std::size_t[)] [*] 2" OR
       http_transfer_encoding_state MATCHES
           "const[ \t]+HttpTransferCodings[ \t]*&[ \t\r\n]+transferCodings" OR
       http_transfer_coding_request_plan MATCHES
           "const[ \t]+HttpTransferCodings[ \t]*&[ \t\r\n]+transferCodings" OR
       http_transfer_coding_client_plan MATCHES
           "const[ \t]+detail::HttpTransferCodings[ \t]*&[ \t\r\n]+transferCodings" OR
       NOT http_transfer_encoding_state MATCHES
           "HttpTransferCodings[ \t\r\n]+transferCodings[(][)] const noexcept" OR
       NOT http_transfer_coding_request_plan MATCHES
           "HttpTransferCodings[ \t\r\n]+transferCodings[(][)] const noexcept" OR
       NOT http_transfer_coding_client_plan MATCHES
           "detail::HttpTransferCodings[ \t\r\n]+transferCodings[(][)] const noexcept" OR
       NOT http1_client_package_consumer MATCHES
           "HasHttp1RequestPlanTransferCodings" OR
       NOT public_http_client_response_owned_view_test MATCHES
           "same_as<ruvia::detail::HttpTransferCodings>")
        boundary_error("HTTP transfer-coding facts lost value semantics"
            "fixed-size coding lists must return by value across parser, request, and client-response plans")
    endif()
endif()
if(EXISTS "${HTTP1_SERVER_PARSER}" AND EXISTS "${HTTP1_CLIENT_RESPONSE_PARSER}")
    file(READ "${RUVIA_ROOT}/ruvia-http/src/parser/HttpHeaderBlockParser.cpp"
        http1_request_header_parser)
    if(NOT http1_request_header_parser MATCHES "contentLength\\.parseField" OR
       NOT http1_client_response_parser MATCHES "contentLength\\.parseField")
        boundary_error("HTTP/1 Content-Length parsing has split again"
            "request and client response heads must both drive HttpContentLengthState")
    endif()
    if(NOT http1_request_header_parser MATCHES "transferEncoding\\.parseField" OR
       NOT http1_client_response_parser MATCHES "transferEncoding\\.parseField")
        boundary_error("HTTP/1 Transfer-Encoding parsing has split again"
            "request and client response heads must both drive HttpTransferEncodingState")
    endif()
endif()
set(HTTP1_CLIENT_RESPONSE_TEST
    "${RUVIA_ROOT}/tests/unit_http_client_response.cpp")
if(EXISTS "${HTTP1_CLIENT_RESPONSE_TEST}")
    file(READ "${HTTP1_CLIENT_RESPONSE_TEST}" http1_client_response_test)
    if(NOT http1_client_response_test MATCHES
           "http_client_response_plan_alternatives_are_exclusive" OR
       NOT http1_client_response_test MATCHES
           "http_client_unframed_body_response_is_close_delimited" OR
       NOT http1_client_response_test MATCHES
           "http_client_successful_connect_transitions_to_tunnel" OR
       NOT http1_client_response_test MATCHES
           "http_client_transfer_coding_before_final_chunked_is_typed" OR
       NOT http1_client_response_test MATCHES
           "http_client_content_length_combined_and_repeated_equal_values" OR
       NOT http1_client_response_test MATCHES
           "http_client_no_body_precedence_ignores_framing_fields" OR
       NOT http1_client_response_test MATCHES
           "http_client_205_owns_zero_content_framing" OR
       NOT http1_client_response_test MATCHES
           "http_client_switching_protocols_is_an_exclusive_upgrade_transition" OR
       NOT http1_client_response_test MATCHES
           "http_client_switching_protocols_requires_wire_agreement" OR
       NOT http1_client_response_test MATCHES
           "http_client_expect_continue_is_one_stateful_exchange_contract" OR
       NOT http1_client_response_test MATCHES
           "http_client_upgrade_after_expect_requires_prior_continue" OR
       NOT http1_client_response_test MATCHES
           "http_client_upgrade_requires_complete_request_content" OR
       NOT http1_client_response_test MATCHES
           "http_client_response_parser_need_more_is_distinct" OR
       NOT http1_client_response_test MATCHES
           "http_client_response_parser_owns_exact_head_boundary" OR
       NOT http1_client_response_test MATCHES
           "http_client_response_parser_failure_is_typed_and_allocation_free" OR
       NOT http1_client_response_test MATCHES
           "!HasResponsePlanMode<ruvia::Http1ClientResponsePlan>" OR
       NOT http1_client_response_test MATCHES
           "!HasResponseConnectionDisposition" OR
       NOT http1_client_response_test MATCHES
           "HasResponseContentLength<ruvia::Http1ClientKnownLengthResponse>" OR
       NOT http1_client_response_test MATCHES
           "HasResponseTransferCodings<ruvia::Http1ClientChunkedResponse>" OR
       NOT http1_client_response_test MATCHES
           "Http1ClientResponseWithZeroContent" OR
       NOT http1_client_response_test MATCHES
           "Content-Length: 3" OR
       NOT http1_client_response_test MATCHES
           "!HasResponsePersistence<[\r\n \t]*ruvia::Http1ClientCloseDelimitedResponse>" OR
       NOT http1_client_response_test MATCHES
           "!std::is_default_constructible_v<ruvia::Http1ClientResponsePlan>")
        boundary_error("HTTP/1 client response plan invariants are under-tested"
            "tests must pin exclusive framing/lifecycle payload ownership, 205 zero-content framing, tri-state/stateful parsing, Expect progress, transactional ownership, close delimiting, CONNECT, Upgrade agreement/order, transfer order, and full Content-Length lists")
    endif()
endif()

if(EXISTS "${WEB_HTTP1_STREAM_ROUTE}")
    file(READ "${WEB_HTTP1_STREAM_ROUTE}" web_http1_stream_route)
    if(NOT web_http1_stream_route MATCHES "http1PlanResponseStream")
        boundary_error("ruvia-web HTTP/1 stream route bypasses the protocol plan"
            "HttpServerResponseStreamRoute.h must call http1PlanResponseStream")
    endif()
    if(NOT web_http1_stream_route MATCHES
       "requestSequence[.]nextResponseClosePolicy[(][)]")
        boundary_error("ruvia-web HTTP/1 stream limit is recomputed after commit"
            "the request limit must enter the pre-commit close policy before response bytes are emitted")
    endif()
    if(NOT web_http1_stream_route MATCHES
       "requestSequence[.]completeCommittedResponse[(]connectionPlan[)]")
        boundary_error("ruvia-web HTTP/1 stream completion bypasses its request sequence"
            "the same connection-private owner must record a successfully committed response")
    endif()
    if(web_http1_stream_route MATCHES
       "streamPlan\.(baseDisposition|connectionWillClose)\(\)")
        boundary_error("ruvia-web treats a pre-commit stream constraint as the final disposition"
            "HttpServerResponseStreamRoute.h must consume the committed sink disposition")
    endif()
    if(NOT web_http1_stream_route MATCHES
       "responseSink\.connectionPlan\(\)")
        boundary_error("ruvia-web ignores the committed HTTP/1 stream connection plan"
            "HttpServerResponseStreamRoute.h must drive responseSink.connectionPlan()")
    endif()
endif()

set(RESPONSE_TRAILER_H2_TEST "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
set(RESPONSE_TRAILER_H1_TEST "${RUVIA_ROOT}/tests/unit_http_server_request_state.cpp")
set(RESPONSE_TRAILER_H2_CONNECTION
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(RESPONSE_TRAILER_H2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(RESPONSE_TRAILER_H2_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(RESPONSE_TRAILER_H2_HEADER_BLOCKS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamHeaderBlocks.h")
set(RESPONSE_TRAILER_H2_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h")
set(RESPONSE_TRAILER_HTTP_CONTRACT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseTrailers.h")
set(RESPONSE_TRAILER_H1_SINK
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamSink.h")
set(RESPONSE_TRAILER_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(RESPONSE_TRAILER_PACKAGE_VERIFY
    "${RUVIA_ROOT}/tests/verify_package_consumers.cmake.in")
if(EXISTS "${RESPONSE_TRAILER_H2_TEST}" AND
   EXISTS "${RESPONSE_TRAILER_H1_TEST}" AND
   EXISTS "${RESPONSE_TRAILER_H2_CONNECTION}" AND
   EXISTS "${RESPONSE_TRAILER_H2_CONNECTION_SOURCE}" AND
   EXISTS "${RESPONSE_TRAILER_H2_STREAM_STATE}" AND
   EXISTS "${RESPONSE_TRAILER_H2_HEADER_BLOCKS}" AND
   EXISTS "${RESPONSE_TRAILER_H2_SINK}" AND
   EXISTS "${RESPONSE_TRAILER_HTTP_CONTRACT}" AND
   EXISTS "${RESPONSE_TRAILER_H1_SINK}" AND
   EXISTS "${RESPONSE_TRAILER_PACKAGE_CONSUMER}" AND
   EXISTS "${RESPONSE_TRAILER_PACKAGE_VERIFY}")
    file(READ "${RESPONSE_TRAILER_H2_TEST}" response_trailer_h2_test)
    file(READ "${RESPONSE_TRAILER_H1_TEST}" response_trailer_h1_test)
    file(READ "${RESPONSE_TRAILER_H2_CONNECTION}"
        response_trailer_h2_connection)
    read_http2_connection_implementation(
        response_trailer_h2_connection_source)
    file(READ "${RESPONSE_TRAILER_H2_STREAM_STATE}"
        response_trailer_h2_stream_state)
    file(READ "${RESPONSE_TRAILER_H2_HEADER_BLOCKS}"
        response_trailer_h2_header_blocks)
    file(READ "${RESPONSE_TRAILER_H2_SINK}"
        response_trailer_h2_sink)
    file(READ "${RESPONSE_TRAILER_HTTP_CONTRACT}"
        response_trailer_http_contract)
    file(READ "${RESPONSE_TRAILER_H1_SINK}"
        response_trailer_h1_sink)
    file(READ "${RESPONSE_TRAILER_PACKAGE_CONSUMER}"
        response_trailer_package_consumer)
    file(READ "${RESPONSE_TRAILER_PACKAGE_VERIFY}"
        response_trailer_package_verify)
    if(NOT response_trailer_http_contract MATCHES
           "class HttpResponseTrailerSection final" OR
       NOT response_trailer_http_contract MATCHES
           "class HttpResponseTrailerSectionFailure final" OR
       NOT response_trailer_http_contract MATCHES
           "class HttpResponseTrailerSectionResult final" OR
       NOT response_trailer_http_contract MATCHES
           "using Value = std::variant" OR
       NOT response_trailer_h2_connection MATCHES
           "const HttpResponseTrailerSection& trailers" OR
       response_trailer_h2_connection MATCHES "kInvalidTrailerSection" OR
       response_trailer_h2_connection_source MATCHES
           "responseTrailerSectionValid|responseTrailerFieldValid" OR
       NOT response_trailer_h2_connection_source MATCHES
           "std::pmr::string trailerBlock[(]resource_[)]" OR
       NOT response_trailer_h2_connection_source MATCHES
           "pending[.]trailerBlock[.]swap[(]trailerBlock[)]" OR
       response_trailer_h2_stream_state MATCHES "responseTrailerBlock" OR
       response_trailer_h2_header_blocks MATCHES "responseTrailers" OR
       NOT response_trailer_h2_sink MATCHES
           "finishResponse[^(]*[(][ \t\r\n]*streamId_,[ \t\r\n]*trailerSection[)]" OR
       NOT response_trailer_h2_sink MATCHES
           "httpResponseTrailerSection[(]trailers[)]" OR
       NOT response_trailer_h1_sink MATCHES
           "httpResponseTrailerSection[(]trailers[)]" OR
       NOT response_trailer_h1_sink MATCHES
           "appendHttp1TrailerSection[(]trailers_, trailerSection[)]" OR
       NOT response_trailer_package_consumer MATCHES
           "AcceptsStagedResponseTrailerSection" OR
       NOT response_trailer_package_consumer MATCHES
           "HasStagedResponseTrailerBlock" OR
       NOT response_trailer_package_verify MATCHES
           "installed HTTP/2 response finish restored staged trailer ownership" OR
       NOT response_trailer_package_verify MATCHES
           "installed Web HTTP/2 sink restored staged trailer submission")
        boundary_error("HTTP/2 response finish lost atomic trailer ownership"
            "one typed HTTP preflight must prove the section for H1/H2 encoding and H2 finish before any commit or mutation")
    endif()
    if(NOT response_trailer_h2_test MATCHES
           "http2_connection_head_response_can_end_with_trailers_only" OR
       NOT response_trailer_h2_test MATCHES
           "http2_response_finish_owns_trailer_section_atomically" OR
       NOT response_trailer_h2_test MATCHES
           "http2_connection_trailers_wait_for_blocked_body" OR
       NOT response_trailer_h1_test MATCHES
           "http1_stream_commit_plan_exposes_exact_trailer_capability")
        boundary_error("response trailer terminal contract is under-tested"
            "tests must pin H1 framing capability plus H2 trailers-only, phase refusal, atomic validation, and DATA-before-trailers ordering")
    endif()
endif()


set(WEB_REDIS_SET_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisTypes.h")
set(WEB_REDIS_SET_HANDLE
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisHandle.cpp")
set(WEB_REDIS_SET_ARGS
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisCommandArgs.cpp")
set(WEB_REDIS_SET_API_TEST
    "${RUVIA_ROOT}/tests/unit_redis_api_surface.cpp")
set(WEB_REDIS_SET_WIRE_TEST
    "${RUVIA_ROOT}/tests/unit_redis_protocol.cpp")
set(WEB_REDIS_SET_PACKAGE_TEST
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(redis_set_contract IN ITEMS
        "${WEB_REDIS_SET_MODEL}"
        "${WEB_REDIS_SET_HANDLE}"
        "${WEB_REDIS_SET_ARGS}"
        "${WEB_REDIS_SET_API_TEST}"
        "${WEB_REDIS_SET_WIRE_TEST}"
        "${WEB_REDIS_SET_PACKAGE_TEST}")
    if(NOT EXISTS "${redis_set_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${redis_set_contract}")
        boundary_error("Redis SET option contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_REDIS_SET_MODEL}" AND
   EXISTS "${WEB_REDIS_SET_HANDLE}" AND
   EXISTS "${WEB_REDIS_SET_ARGS}" AND
   EXISTS "${WEB_REDIS_SET_API_TEST}" AND
   EXISTS "${WEB_REDIS_SET_WIRE_TEST}" AND
   EXISTS "${WEB_REDIS_SET_PACKAGE_TEST}")
    file(READ "${WEB_REDIS_SET_MODEL}" web_redis_set_model)
    file(READ "${WEB_REDIS_SET_HANDLE}" web_redis_set_handle)
    file(READ "${WEB_REDIS_SET_ARGS}" web_redis_set_args)
    file(READ "${WEB_REDIS_SET_API_TEST}" web_redis_set_api_test)
    file(READ "${WEB_REDIS_SET_WIRE_TEST}" web_redis_set_wire_test)
    file(READ "${WEB_REDIS_SET_PACKAGE_TEST}" web_redis_set_package_test)
    if(web_redis_set_model MATCHES
           "milliseconds[ \t]+ttl|bool[ \t]+(nx|xx|get|keepTtl)" OR
       NOT web_redis_set_model MATCHES "enum class RedisSetCondition" OR
       NOT web_redis_set_model MATCHES "class RedisSetExpiration final" OR
       NOT web_redis_set_model MATCHES "using Value = std::variant" OR
       NOT web_redis_set_model MATCHES
           "std::optional<RedisSetCondition>[ \t]+condition" OR
       NOT web_redis_set_model MATCHES
           "std::optional<RedisSetExpiration>[ \t]+expiration" OR
       NOT web_redis_set_model MATCHES "bool[ \t]+returnPrevious" OR
       web_redis_set_handle MATCHES "options[.](ttl|nx|xx|get|keepTtl)" OR
       NOT web_redis_set_handle MATCHES "redisSetArgs[(]key, value, options" OR
       NOT web_redis_set_args MATCHES "switch [(][*]options[.]condition[)]" OR
       NOT web_redis_set_args MATCHES "options[.]expiration->duration[(][)]" OR
       NOT web_redis_set_args MATCHES "options[.]expiration->keepsExisting[(][)]" OR
       NOT web_redis_set_api_test MATCHES
           "redis_set_expiration_cannot_represent_conflicting_modes" OR
       NOT web_redis_set_api_test MATCHES
           "std::optional<ruvia::RedisSetCondition>" OR
       NOT web_redis_set_api_test MATCHES
           "std::optional<ruvia::RedisSetExpiration>" OR
       NOT web_redis_set_wire_test MATCHES
           "redis_set_options_build_one_valid_command_shape" OR
       NOT web_redis_set_package_test MATCHES
           "HasLegacyRedisSetOptionBooleans" OR
       NOT web_redis_set_package_test MATCHES
           "std::optional<ruvia::RedisSetCondition>" OR
       NOT web_redis_set_package_test MATCHES
           "std::optional<ruvia::RedisSetExpiration>")
        boundary_error("Redis SET options regained conflicting boolean state"
            "condition, expiration and returnPrevious must remain one typed command-building chain")
    endif()
    if(web_redis_set_model MATCHES "RedisSetCondition::kNone" OR
       web_redis_set_model MATCHES
           "enum class RedisSetCondition[^{]*[{][^}]*kNone" OR
       web_redis_set_model MATCHES "std::monostate" OR
       web_redis_set_model MATCHES
           "RedisSetExpiration[(][)] noexcept = default")
        boundary_error("Redis SET options restored nested absence sentinels"
            "condition and expiration absence must live only in RedisSetOptions optionals")
    endif()
endif()

set(WEB_DB_TIMEOUT_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbTypes.h")
set(WEB_REDIS_TIMEOUT_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisTypes.h")
set(WEB_DB_TIMEOUT_TEST "${RUVIA_ROOT}/tests/unit_db_sql.cpp")
set(WEB_REDIS_TIMEOUT_TEST "${RUVIA_ROOT}/tests/unit_redis_protocol.cpp")
set(WEB_TIMEOUT_PACKAGE_TEST
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(timeout_contract IN ITEMS
        "${WEB_DB_TIMEOUT_MODEL}"
        "${WEB_REDIS_TIMEOUT_MODEL}"
        "${WEB_DB_TIMEOUT_TEST}"
        "${WEB_REDIS_TIMEOUT_TEST}"
        "${WEB_TIMEOUT_PACKAGE_TEST}")
    if(NOT EXISTS "${timeout_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${timeout_contract}")
        boundary_error("integration timeout contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_DB_TIMEOUT_MODEL}" AND
   EXISTS "${WEB_REDIS_TIMEOUT_MODEL}" AND
   EXISTS "${WEB_DB_TIMEOUT_TEST}" AND
   EXISTS "${WEB_REDIS_TIMEOUT_TEST}" AND
   EXISTS "${WEB_TIMEOUT_PACKAGE_TEST}")
    file(READ "${WEB_DB_TIMEOUT_MODEL}" web_db_timeout_model)
    file(READ "${WEB_REDIS_TIMEOUT_MODEL}" web_redis_timeout_model)
    file(READ "${WEB_DB_TIMEOUT_TEST}" web_db_timeout_test)
    file(READ "${WEB_REDIS_TIMEOUT_TEST}" web_redis_timeout_test)
    file(READ "${WEB_TIMEOUT_PACKAGE_TEST}" web_timeout_package_test)
    foreach(db_timeout IN ITEMS
            connectTimeout readTimeout writeTimeout queryTimeout acquireTimeout)
        if(NOT web_db_timeout_model MATCHES
               "std::optional<std::chrono::milliseconds>[ \t]+${db_timeout}")
            boundary_error("database timeout regained a duration sentinel"
                "${db_timeout} must distinguish absence from a configured positive duration")
        endif()
    endforeach()
    foreach(redis_timeout IN ITEMS
            connectTimeout commandTimeout acquireTimeout)
        if(NOT web_redis_timeout_model MATCHES
               "std::optional<std::chrono::milliseconds>[ \t]+${redis_timeout}")
            boundary_error("Redis timeout regained a duration sentinel"
                "${redis_timeout} must distinguish absence from a configured positive duration")
        endif()
    endforeach()
    if(NOT web_db_timeout_test MATCHES
           "connectTimeout = milliseconds[(]0[)]" OR
       NOT web_redis_timeout_test MATCHES
           "commandTimeout = milliseconds[(]0[)]" OR
       NOT web_timeout_package_test MATCHES
           "decltype[(]ruvia::DbConfig[{][}][.]connectTimeout[)]" OR
       NOT web_timeout_package_test MATCHES
           "decltype[(]ruvia::RedisConfig[{][}][.]commandTimeout[)]")
        boundary_error("integration timeout policy is insufficiently pinned"
            "unit tests must reject configured zero and installed headers must expose optional durations")
    endif()
endif()

set(WEB_DB_POOL_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbTypes.h")
set(WEB_DB_POOL_API_SURFACE "${RUVIA_ROOT}/examples/api_surface.cpp")
set(WEB_DB_POOL_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
set(WEB_DB_POOL_README "${RUVIA_ROOT}/README.md")
if(EXISTS "${WEB_DB_POOL_MODEL}" AND
   EXISTS "${WEB_DB_POOL_API_SURFACE}" AND
   EXISTS "${WEB_DB_POOL_PACKAGE_CONSUMER}" AND
   EXISTS "${WEB_DB_POOL_README}")
    file(READ "${WEB_DB_POOL_MODEL}" web_db_pool_model)
    file(READ "${WEB_DB_POOL_API_SURFACE}" web_db_pool_api_surface)
    file(READ "${WEB_DB_POOL_PACKAGE_CONSUMER}" web_db_pool_package_consumer)
    file(READ "${WEB_DB_POOL_README}" web_db_pool_readme)
    if(NOT web_db_pool_model MATCHES
           "std::size_t[ \t]+poolSizePerWorker[{]4[}]" OR
       web_db_pool_model MATCHES "std::size_t[ \t]+poolSize[{]" OR
       NOT web_db_pool_api_surface MATCHES
           "decltype[(]ruvia::DbConfig[{][}][.]poolSizePerWorker[)]" OR
       NOT web_db_pool_api_surface MATCHES
           "!HasLegacyDbPoolSize<ruvia::DbConfig>" OR
       NOT web_db_pool_package_consumer MATCHES
           "decltype[(]ruvia::DbConfig[{][}][.]poolSizePerWorker[)]" OR
       NOT web_db_pool_package_consumer MATCHES
           "!HasLegacyDbPoolSize<ruvia::DbConfig>" OR
       NOT web_db_pool_readme MATCHES "config[.]poolSizePerWorker" OR
       NOT web_db_pool_readme MATCHES "total connection budget")
        boundary_error("database pool size lost its per-worker API meaning"
            "DbConfig, API/install consumers, and README must expose only poolSizePerWorker")
    endif()
endif()

set(WEB_DB_DEADLINE_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/db/DbInternal.h")
set(WEB_REDIS_DEADLINE_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/redis/RedisInternal.h")
set(WEB_DB_DEADLINE_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolLifecycle.cpp")
set(WEB_PG_DEADLINE_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/db/PgPoolLifecycle.cpp")
set(WEB_REDIS_DEADLINE_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolLifecycle.cpp")
foreach(deadline_contract IN ITEMS
        "${WEB_DB_DEADLINE_STATE}"
        "${WEB_REDIS_DEADLINE_STATE}"
        "${WEB_DB_DEADLINE_LIFECYCLE}"
        "${WEB_PG_DEADLINE_LIFECYCLE}"
        "${WEB_REDIS_DEADLINE_LIFECYCLE}")
    if(NOT EXISTS "${deadline_contract}")
        boundary_error("integration deadline contract is incomplete"
            "${deadline_contract} is required")
    endif()
endforeach()
file(READ "${WEB_DB_DEADLINE_STATE}" web_db_deadline_state)
file(READ "${WEB_REDIS_DEADLINE_STATE}" web_redis_deadline_state)
file(READ "${WEB_DB_DEADLINE_LIFECYCLE}" web_db_deadline_lifecycle)
file(READ "${WEB_PG_DEADLINE_LIFECYCLE}" web_pg_deadline_lifecycle)
file(READ "${WEB_REDIS_DEADLINE_LIFECYCLE}" web_redis_deadline_lifecycle)
if(NOT web_db_deadline_state MATCHES
       "OperationDeadline<DeadlineKind> deadline" OR
   NOT web_redis_deadline_state MATCHES
       "OperationDeadline<DeadlineKind> deadline" OR
   web_db_deadline_state MATCHES
       "deadlineActive|bool timedOut|deadlineKind|DeadlineKind[^;]*kNone" OR
   web_redis_deadline_state MATCHES
       "deadlineActive|bool timedOut|deadlineKind|DeadlineKind[^;]*kNone" OR
   NOT web_db_deadline_lifecycle MATCHES "deadline[.]expire[(]now[)]" OR
   NOT web_pg_deadline_lifecycle MATCHES "deadline[.]expire[(]now[)]" OR
   NOT web_redis_deadline_lifecycle MATCHES "deadline[.]expire[(]now[)]")
    boundary_error("integration runtimes restored split deadline state"
        "MariaDB, PostgreSQL, and Redis must share core's inactive/active/expired deadline lifecycle and retain cancellation kind only inside that state")
endif()

set(WEB_REDIS_OPTIONAL_LIMIT_MODEL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisTypes.h")
set(WEB_REDIS_OPTIONAL_LIMIT_ARGS
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisCommandArgs.cpp")
set(WEB_REDIS_OPTIONAL_LIMIT_IO
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolIo.cpp")
set(WEB_REDIS_OPTIONAL_LIMIT_TEST
    "${RUVIA_ROOT}/tests/unit_redis_protocol.cpp")
set(WEB_REDIS_OPTIONAL_LIMIT_PACKAGE
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
if(EXISTS "${WEB_REDIS_OPTIONAL_LIMIT_MODEL}" AND
   EXISTS "${WEB_REDIS_OPTIONAL_LIMIT_ARGS}" AND
   EXISTS "${WEB_REDIS_OPTIONAL_LIMIT_IO}" AND
   EXISTS "${WEB_REDIS_OPTIONAL_LIMIT_TEST}" AND
   EXISTS "${WEB_REDIS_OPTIONAL_LIMIT_PACKAGE}")
    file(READ "${WEB_REDIS_OPTIONAL_LIMIT_MODEL}" web_redis_optional_limit_model)
    file(READ "${WEB_REDIS_OPTIONAL_LIMIT_ARGS}" web_redis_optional_limit_args)
    file(READ "${WEB_REDIS_OPTIONAL_LIMIT_IO}" web_redis_optional_limit_io)
    file(READ "${WEB_REDIS_OPTIONAL_LIMIT_TEST}" web_redis_optional_limit_test)
    file(READ "${WEB_REDIS_OPTIONAL_LIMIT_PACKAGE}"
        web_redis_optional_limit_package)
    if(NOT web_redis_optional_limit_model MATCHES
           "std::optional<std::size_t>[ 	]+maxReplyBytes" OR
       NOT web_redis_optional_limit_model MATCHES
           "std::optional<std::uint64_t>[ 	]+count" OR
       web_redis_optional_limit_args MATCHES "options[.]count[ 	]*!=[ 	]*0" OR
       web_redis_optional_limit_io MATCHES
           "replyBytes[ 	]*[+][ 	]*bytesRead[ 	]*>" OR
       NOT web_redis_optional_limit_test MATCHES
           "redis_scan_count_distinguishes_absence_from_configured_zero" OR
       NOT web_redis_optional_limit_package MATCHES
           "decltype[(]ruvia::RedisScanOptions[{][}][.]count[)]")
        boundary_error("Redis optional limits regained zero sentinels"
            "reply byte limits and SCAN count hints must preserve absence, reject configured zero, and avoid overflowing cumulative checks")
    endif()
endif()

set(CONNECTION_TIMEOUT_CORE_MODEL
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/ConnectionScanner.h")
set(CONNECTION_TIMEOUT_CORE_RUNTIME
    "${RUVIA_ROOT}/ruvia-core/src/ConnectionScanner.cpp")
set(CONNECTION_TIMEOUT_WEB_API
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/App.h")
set(CONNECTION_TIMEOUT_WEB_OPTIONS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerOptions.h")
set(CONNECTION_TIMEOUT_WEB_BRIDGE
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpServerLifecycle.cpp")
set(CONNECTION_TIMEOUT_CORE_PACKAGE
    "${RUVIA_ROOT}/tests/package-consumer/core.cpp")
set(CONNECTION_TIMEOUT_WEB_PACKAGE
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
if(EXISTS "${CONNECTION_TIMEOUT_CORE_MODEL}" AND
   EXISTS "${CONNECTION_TIMEOUT_CORE_RUNTIME}" AND
   EXISTS "${CONNECTION_TIMEOUT_WEB_API}" AND
   EXISTS "${CONNECTION_TIMEOUT_WEB_OPTIONS}" AND
   EXISTS "${CONNECTION_TIMEOUT_WEB_BRIDGE}" AND
   EXISTS "${CONNECTION_TIMEOUT_CORE_PACKAGE}" AND
   EXISTS "${CONNECTION_TIMEOUT_WEB_PACKAGE}")
    file(READ "${CONNECTION_TIMEOUT_CORE_MODEL}" connection_timeout_core_model)
    file(READ "${CONNECTION_TIMEOUT_CORE_RUNTIME}" connection_timeout_core_runtime)
    file(READ "${CONNECTION_TIMEOUT_WEB_API}" connection_timeout_web_api)
    file(READ "${CONNECTION_TIMEOUT_WEB_OPTIONS}" connection_timeout_web_options)
    file(READ "${CONNECTION_TIMEOUT_WEB_BRIDGE}" connection_timeout_web_bridge)
    file(READ "${CONNECTION_TIMEOUT_CORE_PACKAGE}" connection_timeout_core_package)
    file(READ "${CONNECTION_TIMEOUT_WEB_PACKAGE}" connection_timeout_web_package)
    foreach(timeout_name IN ITEMS
            idleTimeout initialReadTimeout payloadReadTimeout writeTimeout)
        if(NOT connection_timeout_core_model MATCHES
               "std::optional<std::chrono::milliseconds>[ \t]+${timeout_name}")
            boundary_error("core connection timeout regained a numeric sentinel"
                "${timeout_name} must distinguish absence from a configured duration")
        endif()
    endforeach()
    foreach(timeout_name IN ITEMS
            keepaliveTimeout clientHeaderTimeout clientBodyTimeout sendTimeout)
        if(NOT connection_timeout_web_options MATCHES
               "std::optional<std::chrono::milliseconds>[ \t]+${timeout_name}")
            boundary_error("Web connection timeout regained a duration sentinel"
                "${timeout_name} must remain optional through worker normalization")
        endif()
    endforeach()
    if(connection_timeout_core_runtime MATCHES
           "(idle|initialRead|payloadRead|write)TimeoutMs[ \t]*>[ \t]*0" OR
       connection_timeout_web_bridge MATCHES
           "(keepalive|clientHeader|clientBody|send)Timeout[.]count[(][)]" OR
       NOT connection_timeout_web_api MATCHES
           "setKeepaliveTimeout[(]std::optional<std::chrono::milliseconds>" OR
       NOT connection_timeout_core_package MATCHES
           "HasConnectionTimeoutMillisecondSentinels" OR
       NOT connection_timeout_web_package MATCHES
           "AppSetConnectionTimeoutFunction")
        boundary_error("connection timeout presence was lost across layers"
            "App, Web worker options and core scanner must carry optional typed durations end to end")
    endif()
endif()

set(CONNECTION_PERIODIC_TEST
    "${RUVIA_ROOT}/tests/connection_scanner_lifetime.cpp")
set(CONNECTION_PERIODIC_WS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h")
set(CONNECTION_PERIODIC_WS_HEARTBEAT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionHeartbeat.inl")
set(CONNECTION_MAINTENANCE_SERVER_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServer.h")
set(CONNECTION_MAINTENANCE_SERVER_RUNTIME
    "${RUVIA_ROOT}/ruvia-web/src/server/HttpServerLifecycle.cpp")
if(EXISTS "${CONNECTION_TIMEOUT_CORE_MODEL}" AND
   EXISTS "${CONNECTION_TIMEOUT_CORE_RUNTIME}" AND
   EXISTS "${CONNECTION_PERIODIC_TEST}" AND
   EXISTS "${CONNECTION_PERIODIC_WS}" AND
   EXISTS "${CONNECTION_PERIODIC_WS_HEARTBEAT}" AND
   EXISTS "${CONNECTION_MAINTENANCE_SERVER_HEADER}" AND
   EXISTS "${CONNECTION_MAINTENANCE_SERVER_RUNTIME}" AND
   EXISTS "${CONNECTION_TIMEOUT_CORE_PACKAGE}")
    file(READ "${CONNECTION_PERIODIC_TEST}" connection_periodic_test)
    file(READ "${CONNECTION_PERIODIC_WS}" connection_periodic_ws)
    file(READ "${CONNECTION_PERIODIC_WS_HEARTBEAT}"
        connection_periodic_ws_heartbeat)
    file(READ "${CONNECTION_MAINTENANCE_SERVER_HEADER}"
        connection_maintenance_server_header)
    file(READ "${CONNECTION_MAINTENANCE_SERVER_RUNTIME}"
        connection_maintenance_server_runtime)
    if(NOT connection_timeout_core_model MATCHES
           "class PeriodicCheckRegistration final" OR
       NOT connection_timeout_core_model MATCHES
           "void registerPeriodicCheck" OR
       NOT connection_timeout_core_model MATCHES
           "using PeriodicCheck = void [(][*][)][(]void[*], std::int64_t[)] noexcept" OR
       connection_timeout_core_model MATCHES
           "using PeriodicCheck = bool" OR
       NOT connection_timeout_core_runtime MATCHES
           "registration[.]reset[(][)]" OR
       NOT connection_timeout_core_runtime MATCHES
           "void ConnectionScanner::Entry::runPeriodicChecks" OR
       connection_timeout_core_runtime MATCHES
           "periodicCheckFailed|shouldClose" OR
       NOT connection_timeout_core_runtime MATCHES
           "void ConnectionScanner::periodicCheckAdded" OR
       NOT connection_timeout_core_runtime MATCHES
           "periodicCheckCount_[ \t]*!=[ \t]*0" OR
       NOT connection_timeout_core_runtime MATCHES
           "if [(]hasScanningWork[(][)][)]" OR
       NOT connection_timeout_core_runtime MATCHES
           "running_[ 	]*=[ 	]*false;[\r\n 	]+throw;" OR
       NOT connection_periodic_ws MATCHES
           "ConnectionScanner::PeriodicCheckRegistration[ \t]+periodicCheck_" OR
       NOT connection_periodic_ws MATCHES
           "static void heartbeatTickThunk" OR
       connection_periodic_ws MATCHES
           "static bool heartbeatTickThunk|bool heartbeatTick" OR
       NOT connection_periodic_ws_heartbeat MATCHES
           "void WebSocketConnection<Transport>::heartbeatTick" OR
       NOT connection_periodic_test MATCHES
           "PeriodicCheckRegistration,[ \t\r\n]+12> registrations" OR
       NOT connection_periodic_test MATCHES
           "offWorkerRejected" OR
       NOT connection_timeout_core_package MATCHES
           "using ScannerRegistration" OR
       NOT connection_timeout_core_package MATCHES
           "void [(][*][)][(]void[*], std::int64_t[)] noexcept" OR
       NOT connection_timeout_core_model MATCHES
           "class WorkerMaintenanceRegistration final" OR
       NOT connection_timeout_core_model MATCHES
           "void registerWorkerMaintenance" OR
       NOT connection_timeout_core_runtime MATCHES
           "void ConnectionScanner::removeWorkerMaintenance" OR
       NOT connection_periodic_test MATCHES
           "WorkerMaintenanceRegistration,[ \t\r\n]+8> workerRegistrations" OR
       NOT connection_timeout_core_package MATCHES
           "using WorkerMaintenanceRegistration" OR
       NOT connection_maintenance_server_header MATCHES
           "WorkerMaintenanceRegistration[ \t]+databaseDeadlineCheck_" OR
       NOT connection_maintenance_server_header MATCHES
           "WorkerMaintenanceRegistration[ \t]+redisDeadlineCheck_" OR
       NOT connection_maintenance_server_runtime MATCHES
           "registerWorkerMaintenance" OR
       connection_timeout_core_model MATCHES
           "kMaxPeriodicChecks|PeriodicCheckSlot" OR
       connection_timeout_core_runtime MATCHES
           "No free slot|first free" OR
       connection_timeout_core_model MATCHES
           "workerScanners_|WorkerScanner[ \t]+final|setWorkerScanner")
        boundary_error("worker scanning ownership or registration contract regressed"
            "stream hooks must be void intrusive notifications that act on their own transport; only Core timeout policy may close the owning socket")
    endif()
endif()

set(POOL_WAITER_HEADER
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/PoolWaiterQueue.h")
set(POOL_LEASE_SCHEDULER
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/PoolLeaseScheduler.h")
set(POOL_WAITER_DB_SLOTS
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolSlots.cpp")
set(POOL_WAITER_PG_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/db/PgPoolLifecycle.cpp")
set(POOL_WAITER_REDIS_SLOTS
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolSlots.cpp")
set(POOL_WAITER_REDIS_LIFECYCLE
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolLifecycle.cpp")
set(POOL_WAITER_TEST "${RUVIA_ROOT}/tests/unit_pool_waiter_queue.cpp")
set(POOL_LEASE_TEST "${RUVIA_ROOT}/tests/pool_lease_scheduler.cpp")
set(POOL_WAITER_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/core.cpp")
foreach(required IN ITEMS
    "${POOL_WAITER_HEADER}"
    "${POOL_LEASE_SCHEDULER}"
    "${POOL_WAITER_DB_SLOTS}"
    "${POOL_WAITER_PG_LIFECYCLE}"
    "${POOL_WAITER_REDIS_SLOTS}"
    "${POOL_WAITER_REDIS_LIFECYCLE}"
    "${POOL_WAITER_TEST}"
    "${POOL_LEASE_TEST}"
    "${POOL_WAITER_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${required}")
        boundary_error("core pool lease ownership is missing" "${required}")
    endif()
endforeach()
if(EXISTS "${POOL_WAITER_HEADER}" AND
   EXISTS "${POOL_LEASE_SCHEDULER}" AND
   EXISTS "${POOL_WAITER_DB_SLOTS}" AND
   EXISTS "${POOL_WAITER_PG_LIFECYCLE}" AND
   EXISTS "${POOL_WAITER_REDIS_SLOTS}" AND
   EXISTS "${POOL_WAITER_REDIS_LIFECYCLE}" AND
   EXISTS "${POOL_WAITER_TEST}" AND
   EXISTS "${POOL_LEASE_TEST}" AND
   EXISTS "${POOL_WAITER_PACKAGE_CONSUMER}")
    file(READ "${POOL_WAITER_HEADER}" pool_waiter_header)
    file(READ "${POOL_LEASE_SCHEDULER}" pool_lease_scheduler)
    file(READ "${POOL_WAITER_DB_SLOTS}" pool_waiter_db_slots)
    file(READ "${POOL_WAITER_PG_LIFECYCLE}" pool_waiter_pg_lifecycle)
    file(READ "${POOL_WAITER_REDIS_SLOTS}" pool_waiter_redis_slots)
    file(READ "${POOL_WAITER_REDIS_LIFECYCLE}"
        pool_waiter_redis_lifecycle)
    file(READ "${POOL_WAITER_TEST}" pool_waiter_test)
    file(READ "${POOL_LEASE_TEST}" pool_lease_test)
    file(READ "${POOL_WAITER_PACKAGE_CONSUMER}"
        pool_waiter_package_consumer)
    if(NOT pool_waiter_header MATCHES "class PoolWaiterAcquired final" OR
       NOT pool_waiter_header MATCHES "class PoolWaiterTimedOut final" OR
       NOT pool_waiter_header MATCHES "class PoolWaiterClosed final" OR
       NOT pool_waiter_header MATCHES "class PoolWaiterResult final" OR
       NOT pool_waiter_header MATCHES "using Value = std::variant" OR
       NOT pool_waiter_header MATCHES "std::get_if<PoolWaiterAcquired>" OR
       NOT pool_waiter_header MATCHES "std::get_if<PoolWaiterTimedOut>" OR
       NOT pool_waiter_header MATCHES "std::get_if<PoolWaiterClosed>" OR
       NOT pool_waiter_header MATCHES
           "acquired[(][)] const [&] noexcept" OR
       NOT pool_waiter_header MATCHES
           "timedOut[(][)] const [&] noexcept" OR
       NOT pool_waiter_header MATCHES
           "closed[(][)] const [&] noexcept" OR
       NOT pool_waiter_header MATCHES
           "acquired[(][)] const && = delete" OR
       NOT pool_waiter_header MATCHES
           "struct PoolWaiterIdle final" OR
       NOT pool_waiter_header MATCHES
           "struct PoolWaiterQueued final" OR
       NOT pool_waiter_header MATCHES "using State = std::variant" OR
       NOT pool_waiter_header MATCHES
           "holds_alternative<PoolWaiterResult>" OR
       NOT pool_waiter_header MATCHES
           "holds_alternative<PoolWaiterQueued>" OR
       NOT pool_waiter_header MATCHES "bool await_ready[(][)] const noexcept" OR
       NOT pool_waiter_header MATCHES
           "void await_suspend[(]std::coroutine_handle<> handle[)] noexcept" OR
       NOT pool_waiter_header MATCHES
           "const PoolWaiterResult& await_resume[(][)] const noexcept" OR
       NOT pool_waiter_header MATCHES "void completeAcquired" OR
       NOT pool_waiter_header MATCHES "void completeTimedOut" OR
       NOT pool_waiter_header MATCHES "void completeClosed" OR
       NOT pool_waiter_header MATCHES "PoolWaiter[*] closedHead" OR
       NOT pool_waiter_header MATCHES "void closeAll[(][)] noexcept" OR
       pool_waiter_header MATCHES
           "std::optional<PoolWaiterResult>|bool queued_")
        boundary_error("pool waiter lost its discriminated await result"
            "idle, queued, and completed must remain exclusive states; acquired, timeout, and closure must remain exclusive results; closeAll must commit its entire queue before resuming")
    endif()
    if(NOT pool_lease_scheduler MATCHES "class PoolLeaseScheduler final" OR
       NOT pool_lease_scheduler MATCHES "Task<PoolWaiterResult> acquire" OR
       NOT pool_lease_scheduler MATCHES "enum class PoolLeaseReleaseStatus" OR
       NOT pool_lease_scheduler MATCHES "PoolLeaseReleaseStatus release" OR
       NOT pool_lease_scheduler MATCHES "kInvalidSlot" OR
       NOT pool_lease_scheduler MATCHES "kAlreadyReleased" OR
       NOT pool_lease_scheduler MATCHES "waiters_[.]resumeNext" OR
       NOT pool_lease_scheduler MATCHES "waiters_[.]closeAll" OR
       NOT pool_lease_scheduler MATCHES "vector<std::size_t> freeSlots_" OR
       NOT pool_lease_scheduler MATCHES "vector<std::uint8_t> busy_" OR
       NOT pool_waiter_db_slots MATCHES "scheduler_[.]acquire" OR
       NOT pool_waiter_db_slots MATCHES "scheduler_[.]release" OR
       NOT pool_waiter_pg_lifecycle MATCHES "scheduler_[.]acquire" OR
       NOT pool_waiter_pg_lifecycle MATCHES "scheduler_[.]release" OR
       NOT pool_waiter_redis_slots MATCHES
           "scheduler_[.]acquire" OR
       NOT pool_waiter_redis_slots MATCHES "scheduler_[.]release" OR
       NOT pool_waiter_redis_slots MATCHES "result[.]timedOut[(][)]" OR
       NOT pool_waiter_redis_slots MATCHES "result[.]closed[(][)]" OR
       NOT pool_waiter_redis_slots MATCHES "result[.]acquired[(][)]" OR
       NOT pool_waiter_redis_lifecycle MATCHES "scheduler_[.]close")
        boundary_error("DB/Redis pool leases escaped core ownership"
            "core must exclusively own free, busy, waiter, timeout, handoff, and closing state while integrations only map typed acquire outcomes")
    endif()
    if(NOT pool_waiter_test MATCHES
           "pool_waiter_is_its_own_typed_awaiter" OR
       NOT pool_waiter_test MATCHES
           "pool_waiter_queue_close_all_wakes_with_closed_result" OR
       NOT pool_waiter_test MATCHES
           "observeWaiterThenTryResumeNext" OR
       NOT pool_waiter_test MATCHES
           "completed waiter cannot re-enter" OR
       NOT pool_waiter_test MATCHES "PoolWaiterTimedOut" OR
       NOT pool_waiter_package_consumer MATCHES
           "AcceptsLoosePoolWaiterTuple" OR
       NOT pool_waiter_package_consumer MATCHES
           "AcceptsPoolCloseSentinel" OR
       NOT pool_waiter_package_consumer MATCHES
           "HasParallelPoolWaiterResultAccessor" OR
       NOT pool_waiter_package_consumer MATCHES
           "HasAnyRvaluePoolWaiterAccessor" OR
       NOT pool_waiter_package_consumer MATCHES
           "PoolLeaseScheduler" OR
       NOT pool_waiter_package_consumer MATCHES
           "PoolLeaseReleaseStatus" OR
       NOT pool_lease_test MATCHES "exerciseLeaseAndClose" OR
       NOT pool_lease_test MATCHES "exerciseAcquireTimeout")
        boundary_error("core pool lease scheduling is insufficiently pinned"
            "runtime tests and installed-core compile contracts must reject split waiter and lease state")
    endif()
endif()

foreach(stale_pool_scheduler IN ITEMS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/db/DbPoolScheduler.h"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolScheduler.cpp")
    if(EXISTS "${stale_pool_scheduler}")
        boundary_error("Web retained a duplicate DB pool scheduler"
            "${stale_pool_scheduler}")
    endif()
endforeach()
set(POOL_LEASE_DB_INTERNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/db/DbInternal.h")
set(POOL_LEASE_REDIS_INTERNAL
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/redis/RedisInternal.h")
if(EXISTS "${POOL_LEASE_DB_INTERNAL}" AND
   EXISTS "${POOL_LEASE_REDIS_INTERNAL}")
    file(READ "${POOL_LEASE_DB_INTERNAL}" pool_lease_db_internal)
    file(READ "${POOL_LEASE_REDIS_INTERNAL}" pool_lease_redis_internal)
    if(NOT pool_lease_db_internal MATCHES "PoolLeaseScheduler scheduler_" OR
       NOT pool_lease_redis_internal MATCHES "PoolLeaseScheduler scheduler_" OR
       NOT pool_lease_db_internal MATCHES
           "std::optional<std::size_t> defaultClientIndex_" OR
       pool_lease_db_internal MATCHES
           "DbPoolRef[ \t]+defaultClient_" OR
       NOT pool_lease_redis_internal MATCHES "RedisPool& pool_" OR
       NOT pool_lease_redis_internal MATCHES
           "std::optional<std::size_t> defaultPoolIndex_" OR
       pool_lease_redis_internal MATCHES
           "RedisPool[*][ \t]+(pool_|defaultPool_)" OR
       pool_lease_db_internal MATCHES "DbPoolScheduler" OR
       pool_lease_redis_internal MATCHES
           "free_|waiters_|closing_|bool[ \t]+busy")
        boundary_error("integration headers restored duplicate lease or alias ownership"
            "DB and Redis must retain one core scheduler; registry defaults must be derived by index and Redis guards must own references")
    endif()
endif()

check_files_no_match("normal responses must not reintroduce a dynamic streaming-body bypass"
    "${RULE_DYNAMIC_RESPONSE_BODY_STREAM}" ${EDGE_REFERENCE_SOURCE})
check_files_no_match("ruvia-http CMake contains stale mixed-responsibility names"
    "src/Streaming\.cpp|include/ruvia/http/(JsonUtils|HttpBodyStream)\.h"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("Router/error mapping must not decide HTTP/1 connection persistence"
    "${RULE_ROUTER_CONNECTION_POLICY}"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteTable.h"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterMiddlewareDispatch.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/Error.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/http/ContextErrorResponse.cpp")
check_files_no_match("ruvia-core must not contain HTTP/WebSocket semantics"
    "${RULE_CORE_PROTOCOL}" ${CORE_SOURCE})
check_files_no_match("pool wait completion must not restore readiness flags or a close sentinel"
    "${RULE_STALE_POOL_WAITER_TUPLE}"
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/PoolWaiterQueue.h"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolSlots.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/db/DbPoolLifecycle.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolSlots.cpp"
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisPoolLifecycle.cpp")
check_files_no_match("target src directories must not be PUBLIC/INTERFACE includes"
    "${RULE_PUBLIC_SRC_INCLUDE}"
    "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt")
check_files_no_match("targets must not include another target's private src tree"
    "${RULE_CROSS_TARGET_SRC}"
    "${RUVIA_ROOT}/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/CMakeLists.txt")
check_files_no_match("targets must not include another target by physical path"
    "${RULE_CROSS_TARGET_PHYSICAL_INCLUDE}"
    ${CORE_SOURCE} ${HTTP_SOURCE} ${WEB_SOURCE})
check_files_no_match("ruvia-web must not implement content/transfer coding"
    "${RULE_WEB_CODEC}" ${WEB_SOURCE})
check_files_no_match("ruvia-web must not provide an outbound HTTP client runtime"
    "${RULE_WEB_HTTP_CLIENT}" ${WEB_SOURCE})
check_files_no_lower_match("core connection scanner contains protocol/product semantics"
    "${RULE_SCANNER_SEMANTICS}"
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/ConnectionScanner.h"
    "${RUVIA_ROOT}/ruvia-core/src/ConnectionScanner.cpp")

# Cross-target contracts live in include/.../detail. Source trees contain
# implementations only (plus each target's own PCH), so another target cannot
# silently become a friend by reaching into private implementation headers.
file(GLOB_RECURSE HTTP_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-http/src/*.h"
    "${RUVIA_ROOT}/ruvia-http/src/*.inl")
if(HTTP_PRIVATE_HEADERS)
    list(JOIN HTTP_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-http/src contains private headers" "${details}")
endif()
file(GLOB_RECURSE CORE_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/src/*.h"
    "${RUVIA_ROOT}/ruvia-core/src/*.inl")
list(FILTER CORE_PRIVATE_HEADERS EXCLUDE REGEX "[/\\\\]pch\\.h$")
if(CORE_PRIVATE_HEADERS)
    list(JOIN CORE_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-core/src contains cross-target contract headers" "${details}")
endif()
file(GLOB_RECURSE WEB_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-web/src/*.h"
    "${RUVIA_ROOT}/ruvia-web/src/*.inl")
list(FILTER WEB_PRIVATE_HEADERS EXCLUDE REGEX "[/\\\\]pch\\.h$")
if(WEB_PRIVATE_HEADERS)
    list(JOIN WEB_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-web/src contains contract headers" "${details}")
endif()
foreach(stale_dir IN ITEMS
    "${RUVIA_ROOT}/ruvia-core/src/memory"
    "${RUVIA_ROOT}/ruvia-core/src/net"
    "${RUVIA_ROOT}/ruvia-core/src/runtime"
    "${RUVIA_ROOT}/ruvia-http/src/net"
    "${RUVIA_ROOT}/ruvia-web/src/net"
    "${RUVIA_ROOT}/ruvia-web/src/db/core"
    "${RUVIA_ROOT}/ruvia-web/src/redis/core"
    "${RUVIA_ROOT}/ruvia-web/src/router/core")
    if(IS_DIRECTORY "${stale_dir}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${stale_dir}")
        boundary_error("redundant source directory layer was reintroduced" "${relative}")
    endif()
endforeach()

file(GLOB_RECURSE EXAMPLE_SOURCE LIST_DIRECTORIES FALSE "${RUVIA_ROOT}/examples/*.cpp")
set(private_examples)
foreach(path IN LISTS EXAMPLE_SOURCE)
    file(READ "${path}" content)
    example_has_private_include("${content}" has_private)
    if(has_private)
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
        list(APPEND private_examples "${relative}")
    endif()
endforeach()
if(private_examples)
    list(JOIN private_examples "\n    " details)
    boundary_error("examples include target-private headers" "${details}")
endif()

set(WS_DEFLATE_NEGOTIATION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h")
set(WS_SERVER_NEGOTIATION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WebSocketServerNegotiation.h")
set(WS_HANDSHAKE_FIELDS_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketHandshakeFields.cpp")
set(WS_H1_HANDSHAKE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h")
set(WS_HANDSHAKE_VALIDATION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketHandshakeValidation.h")
set(WS_HANDSHAKE_VALIDATION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketValidation.cpp")
set(WS_H2_HANDSHAKE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2WebSocketHandshake.h")
set(WS_H2_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(WS_H1_HANDSHAKE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketHandshake.h")
set(WS_H1_ROUTE_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h")
set(WS_H2_ROUTE_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
foreach(required IN ITEMS
    "${WS_DEFLATE_NEGOTIATION_HEADER}"
    "${WS_SERVER_NEGOTIATION_HEADER}"
    "${WS_HANDSHAKE_FIELDS_SOURCE}"
    "${WS_H1_HANDSHAKE_HEADER}"
    "${WS_HANDSHAKE_VALIDATION_HEADER}"
    "${WS_HANDSHAKE_VALIDATION_SOURCE}"
    "${WS_H2_HANDSHAKE_HEADER}"
    "${WS_H2_CONNECTION_HEADER}"
    "${WS_H1_HANDSHAKE_WRITER}"
    "${WS_H1_ROUTE_DRIVER}"
    "${WS_H2_ROUTE_DRIVER}")
    if(NOT EXISTS "${required}")
        boundary_error("immutable WebSocket server negotiation is missing"
            "${required}")
    endif()
endforeach()
if(EXISTS "${WS_DEFLATE_NEGOTIATION_HEADER}" AND
   EXISTS "${WS_SERVER_NEGOTIATION_HEADER}" AND
   EXISTS "${WS_HANDSHAKE_FIELDS_SOURCE}" AND
   EXISTS "${WS_H1_HANDSHAKE_HEADER}" AND
   EXISTS "${WS_HANDSHAKE_VALIDATION_HEADER}" AND
   EXISTS "${WS_HANDSHAKE_VALIDATION_SOURCE}" AND
   EXISTS "${WS_H2_HANDSHAKE_HEADER}" AND
   EXISTS "${WS_H2_CONNECTION_HEADER}" AND
   EXISTS "${WS_H1_HANDSHAKE_WRITER}" AND
   EXISTS "${WS_H1_ROUTE_DRIVER}" AND
   EXISTS "${WS_H2_ROUTE_DRIVER}")
    file(READ "${WS_DEFLATE_NEGOTIATION_HEADER}" ws_deflate_negotiation)
    file(READ "${WS_SERVER_NEGOTIATION_HEADER}" ws_server_negotiation)
    file(READ "${WS_HANDSHAKE_FIELDS_SOURCE}" ws_handshake_fields_source)
    file(READ "${WS_H1_HANDSHAKE_HEADER}" ws_h1_handshake)
    file(READ "${WS_HANDSHAKE_VALIDATION_HEADER}" ws_handshake_validation)
    file(READ "${WS_HANDSHAKE_VALIDATION_SOURCE}" ws_h1_validation_source)
    file(READ "${WS_H2_HANDSHAKE_HEADER}" ws_h2_handshake)
    file(READ "${WS_H2_CONNECTION_HEADER}" ws_h2_connection)
    file(READ "${WS_H1_HANDSHAKE_WRITER}" ws_h1_writer)
    file(READ "${WS_H1_ROUTE_DRIVER}" ws_h1_route)
    file(READ "${WS_H2_ROUTE_DRIVER}" ws_h2_route)
    if(NOT ws_deflate_negotiation MATCHES
           "enum class WebSocketDeflateNegotiation" OR
       NOT ws_deflate_negotiation MATCHES "kDisabled" OR
       NOT ws_deflate_negotiation MATCHES "kAccepted" OR
       NOT ws_deflate_negotiation MATCHES
           "kAcceptedWithServerMaxWindowBits" OR
       NOT ws_deflate_negotiation MATCHES
           "failed to initialize WebSocket deflate encoder" OR
       NOT ws_deflate_negotiation MATCHES
           "failed to initialize WebSocket deflate decoder" OR
       ws_deflate_negotiation MATCHES
           "bool ok[(][)] const noexcept|deflateOk_|inflateOk_" OR
       NOT ws_deflate_negotiation MATCHES "webSocketDeflateNegotiated" OR
       NOT ws_deflate_negotiation MATCHES
           "webSocketDeflateResponseExtensions" OR
       NOT ws_server_negotiation MATCHES
           "class WebSocketServerNegotiation final" OR
       NOT ws_server_negotiation MATCHES
           "std::string_view subprotocol[(][)] const [&] noexcept" OR
       NOT ws_server_negotiation MATCHES
           "subprotocol[(][)] const && = delete" OR
       NOT ws_server_negotiation MATCHES "std::pmr::string subprotocol_" OR
       NOT ws_handshake_fields_source MATCHES
           "httpPmrResourceOrDefault[(]resource[)]" OR
       NOT ws_server_negotiation MATCHES
           "WebSocketDeflateNegotiation deflate[(][)]" OR
       NOT ws_server_negotiation MATCHES "std::string_view extensions[(][)]" OR
       NOT ws_server_negotiation MATCHES "makeWebSocketServerNegotiation" OR
       NOT ws_h1_handshake MATCHES
           "class HttpWebSocketServerHandshake final" OR
       NOT ws_h1_handshake MATCHES "const WebSocketServerNegotiation&" OR
       NOT ws_h1_handshake MATCHES "makeHttpWebSocketServerHandshake" OR
       NOT ws_handshake_validation MATCHES
           "class HttpWebSocketHandshakeValidationResult final" OR
       NOT ws_handshake_validation MATCHES
           "using Value = std::variant" OR
       NOT ws_handshake_validation MATCHES
           "HttpWebSocketHandshakeAccepted" OR
       NOT ws_handshake_validation MATCHES
           "HttpWebSocketHandshakeFailure" OR
       NOT ws_handshake_validation MATCHES
           "HttpProtocolError protocolError[(][)] const noexcept" OR
       NOT ws_handshake_validation MATCHES
           "applyRequiredResponseHeaders" OR
       NOT ws_handshake_validation MATCHES
           "Sec-WebSocket-Version" OR
       NOT ws_h1_validation_source MATCHES
           "validateHttp1WebSocketHandshake" OR
       NOT ws_h1_validation_source MATCHES
           "bodyPlan[.]requiresConsumption[(][)]" OR
       ws_h1_validation_source MATCHES
           "isValidWebSocketRequest" OR
       NOT ws_h2_handshake MATCHES
           "const WebSocketServerNegotiation& negotiation" OR
       NOT ws_h2_handshake MATCHES
           "validateHttp2WebSocketHandshake" OR
       ws_h2_handshake MATCHES
           "http2IsValidWebSocketRequest" OR
       NOT ws_h2_connection MATCHES
           "class Http2WebSocketHandshakeSubmitFailure final" OR
       NOT ws_h2_connection MATCHES
           "class Http2WebSocketHandshakeSubmitResult final" OR
       NOT ws_h2_connection MATCHES
           "std::variant<[ \t\r\n]*WebSocketServerNegotiation,[ \t\r\n]*Http2WebSocketHandshakeSubmitFailure>" OR
       NOT ws_h2_connection MATCHES
           "std::get_if<WebSocketServerNegotiation>" OR
       NOT ws_h2_connection MATCHES
           "WebSocketServerNegotiation&& negotiation" OR
       ws_h2_connection MATCHES
           "class Http2SubmittedWebSocketHandshake final|std::get_if<Http2SubmittedWebSocketHandshake>" OR
       NOT ws_h1_writer MATCHES
           "const HttpWebSocketServerHandshake& handshake" OR
       NOT ws_h1_route MATCHES "makeHttpWebSocketServerHandshake" OR
       NOT ws_h1_route MATCHES "memory[.]resource[(][)]" OR
       NOT ws_h1_route MATCHES
           "validateHttp1WebSocketHandshake" OR
       NOT ws_h1_route MATCHES
           "failure->protocolError[(][)]" OR
       NOT ws_h1_route MATCHES
           "failure->applyRequiredResponseHeaders[(]response[)]" OR
       ws_h1_route MATCHES
           "invalid websocket upgrade" OR
       NOT ws_h1_route MATCHES
           "handshake[.]negotiation[(][)][.]deflate[(][)]" OR
       NOT ws_h2_route MATCHES "makeWebSocketServerNegotiation" OR
       NOT ws_h2_route MATCHES "requestMemory[.]resource[(][)]" OR
       NOT ws_h2_route MATCHES "std::move[(]negotiation[)]" OR
       NOT ws_h2_route MATCHES
           "validateHttp2WebSocketHandshake" OR
       NOT ws_h2_route MATCHES
           "failure->applyRequiredResponseHeaders[(]response[)]" OR
       ws_h2_route MATCHES
           "invalid http2 websocket request" OR
       NOT ws_h2_route MATCHES "handshakeResult[.]submitted[(][)]" OR
       NOT ws_h2_route MATCHES
           "submittedHandshake->deflate[(][)]" OR
       ws_h2_route MATCHES
           "submittedHandshake->negotiation[(][)]")
        boundary_error("WebSocket server negotiation lost its single committed value"
            "HTTP/1 and RFC 8441 response metadata plus WsConnection compression must consume the same immutable negotiation")
    endif()
endif()

set(WS_PROTOCOL_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h")
set(WS_EVENT_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsEvent.h")
set(WS_INBOUND_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketUtils.h")
set(WS_PROTOCOL_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp")
set(WS_VALIDATION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/websocket/HttpWebSocketValidation.cpp")
set(WS_RUNTIME_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h")
set(WS_RUNTIME_READ
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl")
set(WS_RUNTIME_WRITE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl")
set(WS_H2_TRANSPORT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
set(WS_LIVENESS_POLICY
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketLiveness.h")
set(WS_PUBLIC_CONFIG "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/WebSocket.h")
set(WS_PROTOCOL_TEST "${RUVIA_ROOT}/tests/unit_ws_connection.cpp")
set(WS_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(required IN ITEMS
    "${WS_PROTOCOL_HEADER}"
    "${WS_EVENT_HEADER}"
    "${WS_INBOUND_HEADER}"
    "${WS_PROTOCOL_SOURCE}"
    "${WS_VALIDATION_SOURCE}"
    "${WS_RUNTIME_HEADER}"
    "${WS_RUNTIME_READ}"
    "${WS_RUNTIME_WRITE}"
    "${WS_H2_TRANSPORT}"
    "${WS_LIVENESS_POLICY}"
    "${WS_PUBLIC_CONFIG}"
    "${WS_PROTOCOL_TEST}"
    "${WS_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${required}")
        boundary_error("typed WebSocket close chain is missing" "${required}")
    endif()
endforeach()
if(EXISTS "${WS_PROTOCOL_HEADER}" AND EXISTS "${WS_EVENT_HEADER}" AND
   EXISTS "${WS_INBOUND_HEADER}" AND EXISTS "${WS_PROTOCOL_SOURCE}" AND
   EXISTS "${WS_VALIDATION_SOURCE}" AND
   EXISTS "${WS_RUNTIME_HEADER}" AND EXISTS "${WS_RUNTIME_READ}" AND
   EXISTS "${WS_RUNTIME_WRITE}" AND EXISTS "${WS_H2_TRANSPORT}" AND
   EXISTS "${WS_LIVENESS_POLICY}" AND EXISTS "${WS_PUBLIC_CONFIG}" AND
   EXISTS "${WS_PROTOCOL_TEST}" AND EXISTS "${WS_PACKAGE_CONSUMER}")
    file(READ "${WS_PROTOCOL_HEADER}" ws_protocol)
    file(READ "${WS_EVENT_HEADER}" ws_event)
    file(READ "${WS_INBOUND_HEADER}" ws_inbound)
    file(READ "${WS_PROTOCOL_SOURCE}" ws_protocol_source)
    file(READ "${WS_VALIDATION_SOURCE}" ws_validation_source)
    file(READ "${WS_RUNTIME_HEADER}" ws_runtime)
    file(READ "${WS_RUNTIME_READ}" ws_runtime_read)
    file(READ "${WS_RUNTIME_WRITE}" ws_runtime_write)
    file(READ "${WS_H2_TRANSPORT}" ws_h2_transport)
    file(READ "${WS_LIVENESS_POLICY}" ws_liveness)
    file(READ "${WS_PUBLIC_CONFIG}" ws_public_config)
    file(READ "${WS_PROTOCOL_TEST}" ws_protocol_test)
    file(READ "${WS_PACKAGE_CONSUMER}" ws_package_consumer)
    if(NOT ws_protocol MATCHES "enum class WsLivenessMode" OR
       NOT ws_protocol MATCHES "enum class WsAbortDisposition" OR
       NOT ws_protocol MATCHES "class WsOutputPlan" OR
       NOT ws_protocol MATCHES "WsTransportDisposition" OR
       NOT ws_protocol MATCHES
           "WsTransportDisposition disposition[(][)] const noexcept" OR
       ws_protocol MATCHES "endsTransport[(]" OR
       ws_protocol MATCHES "transportEndPending[(]" OR
       ws_protocol MATCHES "closePhase[(]" OR
       ws_protocol MATCHES "closed[(]" OR
       NOT ws_protocol MATCHES "std::optional<WsEvent> poll" OR
       NOT ws_protocol MATCHES "ruvia/http/detail/websocket/WsEvent.h" OR
       NOT ws_protocol MATCHES
           "poll[(][)][ \\t]*&[ \\t]*[;]" OR
       NOT ws_protocol MATCHES
           "poll[(][)][ \\t]*&&[ \\t]*=[ \\t]*delete" OR
       NOT ws_protocol MATCHES
           "outputPlan[(][)] const &[ \\t]+noexcept" OR
       NOT ws_protocol MATCHES
           "outputPlan[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT ws_protocol_test MATCHES
           "static_assert[(]!ExposesRvalueWsConnectionStorage<WsConnection>" OR
       NOT ws_package_consumer MATCHES
           "static_assert[(]!ExposesRvalueWsConnectionStorage<")
        boundary_error("WebSocket close lifecycle lost its protocol-owned plan"
            "liveness, abort, output, and event operations must expose typed plans without close-state side channels or temporary connection borrows")
    endif()
    if(NOT ws_protocol MATCHES "enum class WsFrameSubmitStatus" OR
       NOT ws_protocol MATCHES "enum class WsCloseSubmitStatus" OR
       NOT ws_protocol MATCHES "WsFrameSubmitStatus submitFrame" OR
       NOT ws_protocol MATCHES "WsCloseSubmitStatus submitClose" OR
       ws_protocol MATCHES "acceptsApplicationFrames[(]" OR
       ws_protocol_source MATCHES
           "throw[ \\t]+std::(invalid_argument|logic_error)" OR
       NOT ws_runtime_write MATCHES
           "switch [(]protocol_[.]submitFrame" OR
       NOT ws_runtime_write MATCHES
           "switch [(]protocol_[.]submitClose")
        boundary_error("WebSocket outbound submission lost its typed ownership"
            "the protocol core must own opcode, size, and close-state decisions while Web maps typed failures")
    endif()
    if(NOT ws_inbound MATCHES
           "enum class WebSocketClosePayloadEncodeError" OR
       NOT ws_inbound MATCHES
           "class WebSocketEncodedClosePayload final" OR
       NOT ws_inbound MATCHES
           "class WebSocketClosePayloadEncodeFailure final" OR
       NOT ws_inbound MATCHES
           "class WebSocketClosePayloadEncodeResult final" OR
       NOT ws_inbound MATCHES
           "std::get_if<WebSocketEncodedClosePayload>" OR
       NOT ws_inbound MATCHES
           "std::get_if<WebSocketClosePayloadEncodeFailure>" OR
       NOT ws_inbound MATCHES "encoded[(][)][ \t\r\n]+const [&] noexcept" OR
       NOT ws_inbound MATCHES "failure[(][)][ \t\r\n]+const [&] noexcept" OR
       NOT ws_inbound MATCHES "encoded[(][)][ \t\r\n]+const && = delete" OR
       ws_inbound MATCHES "using WebSocketClosePayload" OR
       ws_validation_source MATCHES
           "throw[ \\t]+std::invalid_argument")
        boundary_error("WebSocket close encoding lost its owned result"
            "outbound close validation must return an allocation-free discriminated value without an output buffer")
    endif()
    if(NOT ws_event MATCHES "enum class WsEventKind" OR
       NOT ws_event MATCHES "using Value = std::variant" OR
       NOT ws_event MATCHES "class WsMessageEvent final" OR
       NOT ws_event MATCHES "class WsPingEvent final" OR
       NOT ws_event MATCHES "class WsPongEvent final" OR
       NOT ws_event MATCHES "class WsCloseEvent final" OR
       NOT ws_event MATCHES "std::string_view reason" OR
       NOT ws_event MATCHES "class WsProtocolErrorEvent final" OR
       NOT ws_event MATCHES "class WsTransportEndEvent final" OR
       NOT ws_event MATCHES "std::get_if<WsCloseEvent>" OR
       NOT ws_event MATCHES "message[(][)] const [&] noexcept" OR
       NOT ws_event MATCHES "message[(][)] const && = delete" OR
       NOT ws_event MATCHES "protocolError[(][)] const && = delete" OR
       NOT ws_event MATCHES "transportEnd[(][)] const && = delete")
        boundary_error("WebSocket event payloads lost their discriminated contract"
            "need-input must be optional and every materialized message, control, close, failure, or terminal event must own only its valid fields and lend alternatives only from live lvalues")
    endif()
    if(NOT ws_inbound MATCHES "enum class WebSocketProtocolFailure" OR
       NOT ws_inbound MATCHES "enum class WebSocketFrameKind" OR
       NOT ws_inbound MATCHES "class WebSocketFrameStart final" OR
       NOT ws_inbound MATCHES "class WebSocketFrameView final" OR
       NOT ws_inbound MATCHES "std::optional<WebSocketFrameStart>" OR
       NOT ws_inbound MATCHES "WebSocketFrameKind kind_" OR
       NOT ws_inbound MATCHES "WebSocketFrameView continuation" OR
       ws_inbound MATCHES "struct WebSocketFrame(Start|View) final" OR
       ws_inbound MATCHES
           "unsigned char second,[ \t\r\n]+WebSocketFrameStart&" OR
       NOT ws_inbound MATCHES "class WebSocketFrameNeedInput final" OR
       NOT ws_inbound MATCHES "class WebSocketFrameReadFailure final" OR
       NOT ws_inbound MATCHES "class WebSocketFrameReadResult final" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketFrameNeedInput>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketFrameView>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketFrameReadFailure>" OR
       NOT ws_inbound MATCHES "class WebSocketInboundContinue final" OR
       NOT ws_inbound MATCHES "class WebSocketInboundControlFrame final" OR
       NOT ws_inbound MATCHES "enum class WebSocketInboundContentEncoding" OR
       NOT ws_inbound MATCHES "class WebSocketInboundMessage final" OR
       NOT ws_inbound MATCHES "class WebSocketInboundFailure final" OR
       NOT ws_inbound MATCHES "class WebSocketInboundResult final" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundContinue>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundControlFrame>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundMessage>" OR
       NOT ws_inbound MATCHES "std::get_if<WebSocketInboundFailure>" OR
       NOT ws_inbound MATCHES "needInput[(][)] const [&] noexcept" OR
       NOT ws_inbound MATCHES "frame[(][)] const [&] noexcept" OR
       NOT ws_inbound MATCHES "continueReading[(][)] const [&] noexcept" OR
       NOT ws_inbound MATCHES "controlFrame[(][)] const [&] noexcept" OR
       NOT ws_inbound MATCHES "message[(][)] const [&] noexcept" OR
       NOT ws_inbound MATCHES "needInput[(][)] const && = delete" OR
       NOT ws_inbound MATCHES "continueReading[(][)] const && = delete" OR
       NOT ws_inbound MATCHES "webSocketClosePayloadFailure" OR
       NOT ws_protocol_source MATCHES "read[.]failure[(][)]" OR
       NOT ws_protocol_source MATCHES "inbound[.]failure[(][)]" OR
       NOT ws_protocol_source MATCHES "webSocketProtocolFailureCloseCode" OR
       NOT ws_validation_source MATCHES
           "std::optional<WebSocketProtocolFailure>" OR
       NOT ws_validation_source MATCHES
           "webSocketClosePayloadFailure[(]std::string_view payload[)] noexcept")
        boundary_error("WebSocket inbound results lost their discriminated contract"
            "frame decode, reassembly, close validation, and WsConnection failure mapping must remain typed and nonthrowing for peer bytes")
    endif()
    if(NOT ws_runtime MATCHES "WsConnection protocol_" OR
       NOT ws_runtime_read MATCHES "event[.]has_value[(][)]" OR
       NOT ws_runtime_read MATCHES "event->message[(][)]" OR
       NOT ws_runtime_read MATCHES "event->ping[(][)]" OR
       NOT ws_runtime_read MATCHES "event->pong[(][)]" OR
       NOT ws_runtime_read MATCHES "event->close[(][)]" OR
       NOT ws_runtime_read MATCHES "event->protocolError[(][)]" OR
       NOT ws_runtime_read MATCHES "event->transportEnd[(][)]" OR
       NOT ws_runtime_write MATCHES "plan\\.disposition\\(\\)" OR
       NOT ws_runtime_write MATCHES "[(]void[)]co_await read\\(\\)")
        boundary_error("ruvia-web WebSocket runtime bypasses the protocol close plan"
            "the driver must flush WsOutputPlan and await peer Close after local Close")
    endif()
    if(NOT ws_h2_transport MATCHES
           "WsTransportDisposition::kEndTransport" OR
       NOT ws_h2_transport MATCHES
           "submitReset\\(streamId_, Http2ErrorCode::kCancel\\)")
        boundary_error("RFC 8441 transport mapping lost stream-local lifecycle"
            "typed end must map to END_STREAM and liveness abort to RST_STREAM(CANCEL)")
    endif()
    if(NOT ws_public_config MATCHES "class WebSocketHeartbeatPolicy final" OR
       NOT ws_public_config MATCHES
           "static WebSocketHeartbeatPolicy periodic" OR
       NOT ws_public_config MATCHES
           "std::optional<WebSocketHeartbeatPolicy> heartbeat" OR
       NOT ws_public_config MATCHES
           "std::optional<std::chrono::milliseconds> closeHandshakeTimeout" OR
       ws_public_config MATCHES "milliseconds pingInterval[{]" OR
       ws_public_config MATCHES "milliseconds pongTimeout[{]" OR
       NOT ws_public_config MATCHES "closeHandshakeTimeout" OR
       NOT ws_liveness MATCHES "options[.]heartbeat[.]has_value" OR
       NOT ws_liveness MATCHES "WsLivenessMode livenessMode" OR
       NOT ws_runtime MATCHES "WebSocketLifecycleOptions lifecycleOptions_")
        boundary_error("WebSocket liveness policy is not Web-owned"
            "timer configuration and enforcement must remain in ruvia-web")
    endif()
    if(NOT ws_protocol MATCHES "WebSocketDeflateNegotiation deflate" OR
       NOT ws_protocol_source MATCHES
           "webSocketDeflateNegotiated[(]deflate[)]" OR
       NOT ws_runtime MATCHES "WebSocketDeflateNegotiation deflate" OR
       NOT ws_runtime MATCHES "protocol_[(]buffer_, messageLimit, deflate[)]")
        boundary_error("WebSocket frame core lost the typed negotiation handoff"
            "the runtime and sans-I/O core must consume the committed deflate alternative, never a boolean")
    endif()
endif()

set(WS_RUNTIME_TEST "${RUVIA_ROOT}/tests/unit_websocket_connection.cpp")
set(WS_H2_DRIVER_TEST "${RUVIA_ROOT}/tests/unit_sansio_driver.cpp")
if(EXISTS "${WS_PROTOCOL_TEST}" AND EXISTS "${WS_RUNTIME_TEST}" AND
   EXISTS "${WS_H2_DRIVER_TEST}" AND EXISTS "${WS_PACKAGE_CONSUMER}")
    file(READ "${WS_PROTOCOL_TEST}" ws_protocol_test)
    file(READ "${WS_RUNTIME_TEST}" ws_runtime_test)
    file(READ "${WS_H2_DRIVER_TEST}" ws_h2_driver_test)
    file(READ "${WS_PACKAGE_CONSUMER}" ws_package_consumer)
    if(NOT ws_protocol_test MATCHES
           "ws_connection_event_is_optional_and_discriminated" OR
       NOT ws_protocol_test MATCHES "close_without_status_reports_1005" OR
       NOT ws_protocol_test MATCHES
           "protocolError[(][)]->closeCode[(][)]" OR
       NOT ws_protocol_test MATCHES "local_close_waits_for_peer_close" OR
       NOT ws_protocol_test MATCHES "transport_eof_discards_unsent_close" OR
       NOT ws_runtime_test MATCHES "liveness_aborts_transport_not_scanner_owner" OR
       NOT ws_h2_driver_test MATCHES "h2_server_close_waits_for_peer_close" OR
       NOT ws_package_consumer MATCHES
           "std::optional<ruvia::detail::WsEvent>" OR
       NOT ws_package_consumer MATCHES
           "default_initializable<ruvia::detail::WsEvent>" OR
       NOT ws_package_consumer MATCHES "WsProtocolErrorEvent")
        boundary_error("typed WebSocket close lifecycle is insufficiently tested"
            "optional typed events, close semantics, EOF, stream-local timeout, and RFC 8441 END_STREAM ordering are required")
    endif()
endif()

set(WS_H1_HANDSHAKE_TEST
    "${RUVIA_ROOT}/tests/unit_websocket_handshake.cpp")
set(WS_H2_HANDSHAKE_TEST
    "${RUVIA_ROOT}/tests/unit_http2_websocket_handshake.cpp")
set(WS_H2_CONNECTION_TEST
    "${RUVIA_ROOT}/tests/unit_http2_connection.cpp")
if(EXISTS "${WS_H1_HANDSHAKE_TEST}" AND
   EXISTS "${WS_H2_HANDSHAKE_TEST}" AND
   EXISTS "${WS_H2_CONNECTION_TEST}" AND
   EXISTS "${WS_RUNTIME_TEST}" AND
   EXISTS "${WS_PACKAGE_CONSUMER}")
    file(READ "${WS_H1_HANDSHAKE_TEST}" ws_h1_handshake_test)
    file(READ "${WS_H2_HANDSHAKE_TEST}" ws_h2_handshake_test)
    file(READ "${WS_H2_CONNECTION_TEST}" ws_h2_connection_test)
    file(READ "${WS_RUNTIME_TEST}" ws_negotiation_runtime_test)
    file(READ "${WS_PACKAGE_CONSUMER}" ws_negotiation_package_consumer)
    if(NOT ws_h1_handshake_test MATCHES
           "ws_server_handshake_response_serialization_is_http_owned" OR
       NOT ws_h1_handshake_test MATCHES
           "unsupported WebSocket version" OR
       NOT ws_h1_handshake_test MATCHES
           "applyRequiredResponseHeaders" OR
       NOT ws_h1_handshake_test MATCHES
           "kAcceptedWithServerMaxWindowBits" OR
       NOT ws_h2_handshake_test MATCHES
           "makeWebSocketServerNegotiation" OR
       NOT ws_h2_handshake_test MATCHES
           "websocket_server_negotiation_owns_selected_subprotocol" OR
       NOT ws_h2_handshake_test MATCHES
           "unsupported WebSocket version" OR
       NOT ws_h2_handshake_test MATCHES
           "applyRequiredResponseHeaders" OR
       NOT ws_h2_connection_test MATCHES
           "duplicateHandshakeResult[.]failure[(][)]->error[(][)]" OR
       NOT ws_negotiation_runtime_test MATCHES
           "WebSocketDeflateNegotiation::kAccepted" OR
       NOT ws_negotiation_package_consumer MATCHES
           "HasLooseWebSocketDeflateFields" OR
       NOT ws_negotiation_package_consumer MATCHES
           "HasLooseWebSocketNegotiationFields" OR
       NOT ws_negotiation_package_consumer MATCHES
           "ExposesRvalueWebSocketServerSubprotocol" OR
       NOT ws_negotiation_package_consumer MATCHES
           "AcceptsLooseWebSocketHandshakeSubmit" OR
       NOT ws_negotiation_package_consumer MATCHES
           "Http2WebSocketHandshakeSubmitResult" OR
       NOT ws_negotiation_package_consumer MATCHES
           "HttpWebSocketHandshakeValidationResult")
        boundary_error("immutable WebSocket server negotiation is insufficiently tested"
            "H1/H2 serialization, committed submission, typed frame handoff, and installed compile contracts must stay pinned")
    endif()
endif()

set(WS_FRAME_RESULT_TEST "${RUVIA_ROOT}/tests/unit_websocket_frame.cpp")
set(WS_ASSEMBLER_RESULT_TEST "${RUVIA_ROOT}/tests/unit_websocket_assembler.cpp")
set(WS_CLOSE_RESULT_TEST "${RUVIA_ROOT}/tests/unit_websocket_close.cpp")
if(EXISTS "${WS_FRAME_RESULT_TEST}" AND
   EXISTS "${WS_ASSEMBLER_RESULT_TEST}" AND
   EXISTS "${WS_CLOSE_RESULT_TEST}" AND
   EXISTS "${WS_PACKAGE_CONSUMER}")
    file(READ "${WS_FRAME_RESULT_TEST}" ws_frame_result_test)
    file(READ "${WS_ASSEMBLER_RESULT_TEST}" ws_assembler_result_test)
    file(READ "${WS_CLOSE_RESULT_TEST}" ws_close_result_test)
    file(READ "${WS_PACKAGE_CONSUMER}" ws_inbound_package_consumer)
    if(NOT ws_frame_result_test MATCHES
           "ws_frame_reader_needs_input_without_sentinel_metadata" OR
       NOT ws_frame_result_test MATCHES
           "ws_frame_reader_reports_typed_wire_failures" OR
       NOT ws_frame_result_test MATCHES
           "default_initializable<WebSocketFrameReadResult>" OR
       NOT ws_frame_result_test MATCHES
           "WebSocketProtocolFailure::kMessageTooLarge" OR
       NOT ws_assembler_result_test MATCHES
           "default_initializable<WebSocketInboundResult>" OR
       NOT ws_assembler_result_test MATCHES
           "WebSocketInboundContentEncoding::kPerMessageDeflate" OR
       NOT ws_assembler_result_test MATCHES
           "ws_assembler_protocol_errors" OR
       NOT ws_close_result_test MATCHES
           "webSocketClosePayloadFailure" OR
       NOT ws_close_result_test MATCHES
           "WebSocketProtocolFailure::kInvalidPayloadData" OR
       NOT ws_close_result_test MATCHES
           "HasAnyRvalueClosePayloadAccessor" OR
       NOT ws_frame_result_test MATCHES
           "HasAnyRvalueFrameReadAccessor" OR
       NOT ws_assembler_result_test MATCHES
           "HasAnyRvalueInboundAccessor" OR
       NOT ws_inbound_package_consumer MATCHES
           "WebSocketFrameReadResult" OR
       NOT ws_inbound_package_consumer MATCHES
           "WebSocketInboundResult" OR
       NOT ws_inbound_package_consumer MATCHES
           "HasWsRequiredBytesField" OR
       NOT ws_inbound_package_consumer MATCHES
           "HasWsInboundActionAccessor")
        boundary_error("typed WebSocket inbound results are insufficiently tested"
            "frame, reassembly, close validation, and installed-consumer tests must pin exclusive alternatives and RFC failure codes")
    endif()
endif()

set(HTTP1_SCAN ${HTTP_SOURCE} ${WEB_SOURCE} ${EXAMPLE_SOURCE})
file(GLOB_RECURSE TEST_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl")
list(APPEND HTTP1_SCAN ${TEST_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/CMakeLists.txt")
check_files_no_match("parallel Http1Connection state machine was reintroduced"
    "${RULE_HTTP1_CONNECTION}" ${HTTP1_SCAN})

set(HTTP_RUNTIME_STATE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpConnectionState.h")
set(HTTP_BODY_READER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h")
if(EXISTS "${HTTP_RUNTIME_STATE}" AND EXISTS "${HTTP_BODY_READER}")
    file(READ "${HTTP_RUNTIME_STATE}" runtime_state)
    file(READ "${HTTP_BODY_READER}" body_reader)
    if(NOT runtime_state MATCHES "Http1ServerRequestParser" OR
       NOT body_reader MATCHES "Http1ChunkedBodyDecoder")
        boundary_error("ruvia-web HTTP/1 runtime is not driving http-owned primitives"
            "expected Http1ServerRequestParser and Http1ChunkedBodyDecoder in web runtime state")
    endif()
endif()

set(HTTP1_CHUNK_WEB_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderChunked.inl")
set(HTTP1_CHUNK_DECODER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h")
set(HTTP1_CHUNK_DECODER_TEST "${RUVIA_ROOT}/tests/unit_chunk_decoder.cpp")
set(HTTP1_CHUNK_SCANNER_TEST "${RUVIA_ROOT}/tests/unit_http_parsing.cpp")
set(HTTP1_CHUNK_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
foreach(http1_chunk_driver_contract IN ITEMS
        "${HTTP1_CHUNK_DECODER}"
        "${HTTP1_CHUNK_WEB_DRIVER}"
        "${HTTP1_CHUNK_DECODER_TEST}"
        "${HTTP1_CHUNK_SCANNER_TEST}"
        "${HTTP1_CHUNK_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${http1_chunk_driver_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${http1_chunk_driver_contract}")
        boundary_error("HTTP/1 chunked driver contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${HTTP1_CHUNK_DECODER}" AND
   EXISTS "${HTTP1_CHUNK_WEB_DRIVER}" AND
   EXISTS "${HTTP1_CHUNK_DECODER_TEST}" AND
   EXISTS "${HTTP1_CHUNK_SCANNER_TEST}" AND
   EXISTS "${HTTP1_CHUNK_PACKAGE_CONSUMER}")
    file(READ "${HTTP1_CHUNK_DECODER}" http1_chunk_decoder)
    file(READ "${HTTP1_CHUNK_WEB_DRIVER}" http1_chunk_web_driver)
    file(READ "${HTTP1_CHUNK_DECODER_TEST}" http1_chunk_decoder_test)
    file(READ "${HTTP1_CHUNK_SCANNER_TEST}" http1_chunk_scanner_test)
    file(READ "${HTTP1_CHUNK_PACKAGE_CONSUMER}" http1_chunk_package_consumer)
    if(NOT http1_chunk_decoder MATCHES "HttpProtocolError protocolError[(][)] const noexcept" OR
       http1_chunk_decoder MATCHES "Http1ChunkDecodeError error[(][)] const" OR
       NOT http1_chunk_web_driver MATCHES "result[.]consumedBytes[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "result[.]bodyChunk[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "result[.]complete[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "result[.]needMore[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "result[.]failure[(][)]" OR
       NOT http1_chunk_web_driver MATCHES "failure->protocolError[(][)]" OR
       http1_chunk_web_driver MATCHES
           "Http1ChunkDecodeError|throwHttp1ChunkDecodeFailure")
        boundary_error("ruvia-web bypasses the typed HTTP/1 chunk decoder result"
            "HTTP must own failure status while Web only drives the typed result")
    endif()
    if(NOT http1_chunk_decoder_test MATCHES
           "chunked_body_decoder_emits_zero_copy_chunks_and_preserves_pipeline" OR
       NOT http1_chunk_decoder_test MATCHES
           "chunked_body_decoder_handles_single_byte_input_fragmentation" OR
       NOT http1_chunk_decoder_test MATCHES
           "chunked_body_decoder_reports_typed_size_and_limit_failures" OR
       NOT http1_chunk_decoder_test MATCHES "repeatedInvalid" OR
       NOT http1_chunk_decoder_test MATCHES
           "HasChunkBytes<Http1ChunkDecodeBodyChunk>" OR
       NOT http1_chunk_decoder_test MATCHES
           "!HasRawDecodeError<Http1ChunkDecodeFailure>" OR
       NOT http1_chunk_decoder_test MATCHES
           "protocolError[(][)][.]status[(][)]" OR
       NOT http1_chunk_scanner_test MATCHES
           "chunk_scan_result_is_discriminated" OR
       NOT http1_chunk_scanner_test MATCHES
           "HasChunkScanConsumedBytes<HttpChunkScanComplete>" OR
       NOT http1_chunk_package_consumer MATCHES
           "default_initializable<ruvia::detail::Http1ChunkDecodeResult>" OR
       NOT http1_chunk_package_consumer MATCHES
           "Http1ChunkDecodeFailure" OR
       NOT http1_chunk_package_consumer MATCHES
           "HasProtocolError<ruvia::detail::Http1ChunkDecodeFailure>" OR
       NOT http1_chunk_package_consumer MATCHES
           "!HasAnyRvalueHttp1ChunkDecodeAccessor" OR
       NOT http1_chunk_package_consumer MATCHES
           "!HasAnyRvalueHttpChunkScanAccessor" OR
       NOT http1_chunk_package_consumer MATCHES
           "HasConsumedBytes<ruvia::detail::HttpChunkScanComplete>")
        boundary_error("typed HTTP/1 chunk result ownership is insufficiently tested"
            "unit and installed-consumer contracts must pin protocol status ownership, fragmentation, and pipeline preservation")
    endif()
endif()

set(HTTP_TRANSFER_DECODER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/body/HttpTransferCodingDecoder.h")
set(HTTP_TRANSFER_DECODER_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/body/HttpTransferCodingDecoder.cpp")
set(WEB_BODY_READER_CORE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl")
set(WEB_BODY_READER_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReader.h")
set(WEB_BODY_READER_CHUNKED
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderChunked.inl")
set(HTTP_TRANSFER_DECODER_TEST
    "${RUVIA_ROOT}/tests/unit_request_body_decoding.cpp")
set(WEB_BODY_READER_TEST
    "${RUVIA_ROOT}/tests/unit_http_stream_body_reader.cpp")
if(EXISTS "${HTTP_TRANSFER_DECODER}" AND
   EXISTS "${HTTP_TRANSFER_DECODER_SOURCE}" AND
   EXISTS "${WEB_BODY_READER_CORE}" AND
   EXISTS "${WEB_BODY_READER_CHUNKED}" AND
   EXISTS "${HTTP_TRANSFER_DECODER_TEST}" AND
   EXISTS "${WEB_BODY_READER_TEST}" AND
   EXISTS "${HTTP1_CHUNK_PACKAGE_CONSUMER}")
    file(READ "${HTTP_TRANSFER_DECODER}" transfer_decoder)
    file(READ "${HTTP_TRANSFER_DECODER_SOURCE}" transfer_decoder_source)
    file(READ "${WEB_BODY_READER_CORE}" body_reader_core)
    file(READ "${WEB_BODY_READER_HEADER}" body_reader_header)
    file(READ "${WEB_BODY_READER_CHUNKED}" body_reader_chunked)
    file(READ "${HTTP_TRANSFER_DECODER_TEST}" transfer_decoder_test)
    file(READ "${WEB_BODY_READER_TEST}" body_reader_test)
    file(READ "${HTTP1_CHUNK_PACKAGE_CONSUMER}" transfer_package_consumer)
    if(NOT transfer_decoder MATCHES
           "enum class TransferCodingDecodeError" OR
       NOT transfer_decoder MATCHES
           "class TransferCodingDecodeNeedInput final" OR
       NOT transfer_decoder MATCHES
           "class TransferCodingDecodeOutput final" OR
       NOT transfer_decoder MATCHES
           "class TransferCodingDecodeComplete final" OR
       NOT transfer_decoder MATCHES
           "class TransferCodingDecodeProtocolFailure final" OR
       NOT transfer_decoder MATCHES
           "class TransferCodingDecoderFailure final" OR
       NOT transfer_decoder MATCHES
           "HttpProtocolError protocolError[(][)] const noexcept" OR
       transfer_decoder MATCHES
           "TransferCodingDecodeError error[(][)] const|std::optional<HttpProtocolError>" OR
       NOT transfer_decoder MATCHES
           "class TransferCodingDecodeResult final" OR
       NOT transfer_decoder MATCHES
           "TransferCodingDecodeResult decode" OR
       NOT transfer_decoder MATCHES "std::span<char> output" OR
       NOT transfer_decoder MATCHES
           "TransferCodingDecodeResult finishInput" OR
       NOT transfer_decoder MATCHES "struct Active final" OR
       NOT transfer_decoder MATCHES "struct GzipMemberBoundary final" OR
       NOT transfer_decoder MATCHES "struct Complete final" OR
       NOT transfer_decoder MATCHES "using State = std::variant" OR
       NOT transfer_decoder MATCHES
           "GzipMemberBoundary,[ \t\r\n]+Complete,[ \t\r\n]+TransferCodingDecodeError>" OR
       NOT transfer_decoder MATCHES
           "needInput[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT transfer_decoder MATCHES
           "output[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT transfer_decoder MATCHES
           "complete[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT transfer_decoder MATCHES
           "protocolFailure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       NOT transfer_decoder MATCHES
           "decoderFailure[(][)] const &&[ \\t]*=[ \\t]*delete" OR
       transfer_decoder MATCHES
           "${RULE_STALE_TRANSFER_CODING_TERMINAL_SPLIT}" OR
       transfer_decoder_source MATCHES
           "${RULE_STALE_TRANSFER_CODING_TERMINAL_SPLIT}" OR
       transfer_decoder MATCHES
           "bool[ \\t]+initialized_|void[ \\t]+cleanup[(]" OR
       transfer_decoder_source MATCHES
           "initialized_|TransferCodingDecoder::cleanup[(]" OR
       NOT transfer_decoder_source MATCHES
           "[(]void[)]inflateEnd[(]&stream_[)]" OR
       transfer_decoder MATCHES
           "decodeAppend[(]|produce[(]|setInput[(]|finished[(]|empty[(]" OR
       transfer_decoder_source MATCHES
           "throw[ \t]+(std::invalid_argument|HttpProtocolError)")
        boundary_error("transfer-coding recovered parallel buffered/streaming APIs"
            "one typed decode(input, output-span) primitive must own wire, limit, and terminal results")
    endif()
    if(NOT body_reader_core MATCHES
           "transferDecoder_->decode[(]" OR
       NOT body_reader_header MATCHES
           "PmrObjectDeleter<TransferCodingDecoder>>[ \t]+transferDecoder_" OR
       body_reader_header MATCHES
           "TransferCodingDecoder[*][ \t]+transferDecoder_|transferDecoderAllocator_" OR
       NOT body_reader_core MATCHES
           "makePmrObject<TransferCodingDecoder>" OR
       body_reader_core MATCHES
           "destroy_at[(]transferDecoder_|deallocate[(]transferDecoder_" OR
       NOT body_reader_chunked MATCHES
           "transferDecoder_->decode[(]" OR
       NOT body_reader_core MATCHES
           "throwTransferCodingProtocolFailure" OR
       NOT body_reader_core MATCHES
           "throwTransferCodingDecoderFailure" OR
       NOT body_reader_core MATCHES
           "finishResult[.]protocolFailure[(][)]" OR
       NOT body_reader_core MATCHES
           "finishResult[.]decoderFailure[(][)]" OR
       NOT body_reader_core MATCHES
           "requireCompleteTransferCoding" OR
       body_reader_core MATCHES "TransferCodingDecodeError::" OR
       body_reader_chunked MATCHES "TransferCodingDecodeError::" OR
       body_reader_core MATCHES "decodeAppend[(]" OR
       body_reader_chunked MATCHES "produce[(]|setInput[(]")
        boundary_error("Web request body reader bypasses the typed transfer decoder"
            "buffered and streaming reads must adapt storage around the same HTTP decode result")
    endif()
    if(NOT transfer_decoder_test MATCHES
           "transfer_coding_decoder_reports_typed_wire_failures" OR
       NOT transfer_decoder_test MATCHES
           "transfer_coding_decoder_gzip_decodes_every_rfc1952_member" OR
       NOT transfer_decoder_test MATCHES
           "TransferCodingDecodeResult" OR
       NOT transfer_decoder_test MATCHES
           "repeatedFinish[.]protocolFailure[(]" OR
       NOT transfer_decoder_test MATCHES
           "!HasRawTransferDecodeError<TransferCodingDecodeProtocolFailure>" OR
       NOT transfer_decoder_test MATCHES
           "protocolFailure[(][)]->protocolError[(][)][.]status[(][)]" OR
       NOT body_reader_test MATCHES
           "http1_transfer_coding_uses_one_decoder_for_streaming_and_buffered_reads" OR
       NOT body_reader_test MATCHES
           "http1_transfer_coding_failure_maps_once_for_both_read_surfaces" OR
       NOT body_reader_test MATCHES
           "http1_transfer_coding_eof_commits_only_the_complete_decode_pipeline" OR
       NOT body_reader_test MATCHES
           "http1_transfer_coding_preserves_gzip_members_across_chunks" OR
       NOT transfer_package_consumer MATCHES
           "TransferCodingDecodeResult" OR
       NOT transfer_package_consumer MATCHES
           "HasProtocolError.*TransferCodingDecodeProtocolFailure" OR
       NOT transfer_package_consumer MATCHES
           "!HasTransferDecodeError" OR
       NOT transfer_package_consumer MATCHES
           "!HasAnyRvalueTransferCodingDecodeAccessor")
        boundary_error("typed transfer-coding chain is insufficiently tested"
            "HTTP status ownership, both Web read surfaces, and installed consumers must stay pinned")
    endif()
endif()

set(HTTP1_WEB_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
if(EXISTS "${HTTP1_WEB_SESSION}")
    file(READ "${HTTP1_WEB_SESSION}" http1_web_session)
    if(NOT http1_web_session MATCHES "parsed\\.headReady[(][)]" OR
       NOT http1_web_session MATCHES "parsed\\.failure[(][)]")
        boundary_error("ruvia-web stopped respecting the HTTP/1 head/message boundary"
            "route dispatch consumes head-ready while body readers own completion")
    endif()
endif()

foreach(obsolete_http2_upgrade_path IN ITEMS
    "ruvia-http/include/ruvia/http/detail/http2/Http2Upgrade.h"
    "ruvia-web/include/ruvia/web/detail/http2/Http2UpgradeHandshake.h"
    "ruvia-web/include/ruvia/web/detail/server/HttpServerHttp2UpgradeRoute.h"
    "tests/unit_http2_upgrade.cpp")
    if(EXISTS "${RUVIA_ROOT}/${obsolete_http2_upgrade_path}")
        boundary_error("obsolete HTTP/2 HTTP/1.1 Upgrade path was restored"
            "${obsolete_http2_upgrade_path} exists")
    endif()
endforeach()
check_files_no_match("obsolete HTTP/2 HTTP/1.1 Upgrade path was restored"
    "${RULE_OBSOLETE_HTTP2_UPGRADE}"
    ${HTTP1_PARSE_PHASE_REFERENCE_SOURCE})
check_files_no_match("generic base64url helper escaped ruvia-core"
    "${RULE_HTTP_OWNED_BASE64URL}"
    ${HTTP1_PARSE_PHASE_REFERENCE_SOURCE})
if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpBase64Url.h")
    boundary_error("generic base64url helper escaped ruvia-core"
        "ruvia-http/include/ruvia/http/detail/HttpBase64Url.h exists")
endif()
set(CORE_BASE64URL
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/Base64Url.h")
set(WEB_JWT_ENCODING "${RUVIA_ROOT}/ruvia-web/src/auth/JwtEncoding.cpp")
set(WEB_JWT_JSON "${RUVIA_ROOT}/ruvia-web/src/auth/JwtJson.cpp")
set(WEB_JWT_TEST "${RUVIA_ROOT}/tests/unit_jwt.cpp")
check_files_no_match("JWT optional timestamps must not recover zero sentinels"
    "std::chrono::seconds[ \t]+(expiresIn|notBeforeDelay)"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/auth/Jwt.h")
check_files_no_match("JWT must not recover first-match JSON member lookup"
    "jwtFindJsonString"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/auth/JwtInternal.h"
    "${WEB_JWT_JSON}")
if(NOT EXISTS "${CORE_BASE64URL}")
    boundary_error("core base64url primitive is missing"
        "ruvia-core/include/ruvia/core/detail/Base64Url.h")
elseif(EXISTS "${WEB_JWT_ENCODING}")
    file(READ "${WEB_JWT_ENCODING}" web_jwt_encoding)
    if(NOT web_jwt_encoding MATCHES "ruvia/core/detail/Base64Url[.]h" OR
       NOT web_jwt_encoding MATCHES "decodeBase64UrlChar")
        boundary_error("JWT stopped reusing the core base64url primitive"
            "ruvia-web must not recreate or recover the removed HTTP-owned helper")
    endif()
endif()
if(EXISTS "${WEB_JWT_JSON}" AND EXISTS "${WEB_JWT_TEST}")
    file(READ "${WEB_JWT_JSON}" web_jwt_json)
    file(READ "${WEB_JWT_TEST}" web_jwt_test)
    if(NOT web_jwt_json MATCHES "visitUniqueJwtJsonObjectFields" OR
       NOT web_jwt_json MATCHES "jwtParseJoseAlgorithm" OR
       NOT web_jwt_test MATCHES
           "jwt_verify_requires_unique_complete_json_objects" OR
       NOT web_jwt_test MATCHES
           "jwt_sign_rejects_duplicate_custom_claim_names")
        boundary_error("JWT JSON objects lost their unique complete-member contract"
            "JOSE headers and claims must reject duplicate names and trailing significant bytes")
    endif()
endif()

set(WEB_JWT_PUBLIC "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/auth/Jwt.h")
set(WEB_JWT_SOURCE "${RUVIA_ROOT}/ruvia-web/src/auth/Jwt.cpp")
set(WEB_JWT_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(jwt_owned_view_contract IN ITEMS
        "${WEB_JWT_PUBLIC}"
        "${WEB_JWT_SOURCE}"
        "${WEB_JWT_TEST}"
        "${WEB_JWT_CONSUMER}")
    if(NOT EXISTS "${jwt_owned_view_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${jwt_owned_view_contract}")
        boundary_error("JWT owned-view lifetime contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_JWT_PUBLIC}" AND
   EXISTS "${WEB_JWT_SOURCE}" AND
   EXISTS "${WEB_JWT_TEST}" AND
   EXISTS "${WEB_JWT_CONSUMER}")
    file(READ "${WEB_JWT_PUBLIC}" web_jwt_public)
    file(READ "${WEB_JWT_SOURCE}" web_jwt_source)
    file(READ "${WEB_JWT_TEST}" web_jwt_owned_view_test)
    file(READ "${WEB_JWT_CONSUMER}" web_jwt_owned_view_consumer)
    if(NOT web_jwt_public MATCHES "name[(][)] const [&] noexcept" OR
       NOT web_jwt_public MATCHES "value[(][)] const && = delete" OR
       NOT web_jwt_public MATCHES "issuer[(][)] const [&] noexcept" OR
       NOT web_jwt_public MATCHES "subject[(][)] const && = delete" OR
       NOT web_jwt_public MATCHES "audience[(][)] const && = delete" OR
       NOT web_jwt_public MATCHES "id[(][)] const && = delete" OR
       NOT web_jwt_public MATCHES "claims[(][)] const [&] noexcept" OR
       NOT web_jwt_public MATCHES
           "claim[ \t\r\n]*[(][^)]*std::string_view[^)]*[)][ \t\r\n]*const && = delete" OR
       NOT web_jwt_source MATCHES "JwtPayload::issuer[(][)] const [&] noexcept" OR
       NOT web_jwt_source MATCHES "JwtPayload::claims[(][)] const [&] noexcept" OR
       NOT web_jwt_source MATCHES
           "JwtPayload::claim[(][^)]*std::string_view[^)]*[)] const [&] noexcept" OR
       NOT web_jwt_owned_view_test MATCHES
           "ExposesAnyRvalueJwtOwnedView" OR
       NOT web_jwt_owned_view_consumer MATCHES
           "ExposesAnyRvalueJwtOwnedView")
        boundary_error("JWT owning values expose views from temporary owners"
            "claims and decoded payloads must lend owner-backed strings and spans only from live lvalues")
    endif()
endif()

set(HTTP2_CLEARTEXT_DRIVER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerCleartextHttp2.h")
set(HTTP2_SANSIO_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
if(EXISTS "${HTTP2_CLEARTEXT_DRIVER}" AND EXISTS "${HTTP2_SANSIO_SESSION}")
    file(READ "${HTTP2_CLEARTEXT_DRIVER}" http2_cleartext_driver)
    file(READ "${HTTP2_SANSIO_SESSION}" http2_sansio_session)
    if(NOT http2_cleartext_driver MATCHES "probeCleartextHttp2Preface" OR
       NOT http2_cleartext_driver MATCHES "kHttp2ClientPreface" OR
       NOT http2_cleartext_driver MATCHES "kCompletePreface" OR
       NOT http2_cleartext_driver MATCHES "kNeedMorePreface" OR
       NOT http2_sansio_session MATCHES "connection\\.beginConnection[(][)]")
        boundary_error("HTTP/2 current startup path is incomplete"
            "TLS ALPN and cleartext prior knowledge must converge on beginConnection plus the client preface")
    endif()
endif()

set(PROTOCOL_BYTE_LIMIT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/ProtocolByteLimit.h")
set(HTTP_TRANSFER_CODING_DECODER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/body/HttpTransferCodingDecoder.h")
set(WEB_REQUEST_BODY_LIMIT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/RequestBodyLimit.h")
set(WEB_SERVER_OPTIONS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerOptions.h")
set(WEB_APP_HEADER "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/App.h")
set(HTTP_WEBSOCKET_UTILS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketUtils.h")
set(HTTP_WEBSOCKET_DEFLATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h")
set(HTTP_WEBSOCKET_CONNECTION
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/WsConnection.h")
set(HTTP_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
set(WEB_PACKAGE_CONSUMER "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(protocol_limit_contract IN ITEMS
        "${PROTOCOL_BYTE_LIMIT}"
        "${HTTP_TRANSFER_CODING_DECODER}"
        "${WEB_REQUEST_BODY_LIMIT}"
        "${WEB_SERVER_OPTIONS}"
        "${WEB_APP_HEADER}"
        "${HTTP_WEBSOCKET_UTILS}"
        "${HTTP_WEBSOCKET_DEFLATE}"
        "${HTTP_WEBSOCKET_CONNECTION}"
        "${HTTP_PACKAGE_CONSUMER}"
        "${WEB_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${protocol_limit_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${protocol_limit_contract}")
        boundary_error("explicit protocol byte-limit contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${PROTOCOL_BYTE_LIMIT}" AND
   EXISTS "${HTTP_TRANSFER_CODING_DECODER}" AND
   EXISTS "${WEB_REQUEST_BODY_LIMIT}" AND
   EXISTS "${WEB_SERVER_OPTIONS}" AND
   EXISTS "${WEB_APP_HEADER}" AND
   EXISTS "${HTTP_WEBSOCKET_UTILS}" AND
   EXISTS "${HTTP_WEBSOCKET_DEFLATE}" AND
   EXISTS "${HTTP_WEBSOCKET_CONNECTION}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}" AND
   EXISTS "${WEB_PACKAGE_CONSUMER}")
    file(READ "${PROTOCOL_BYTE_LIMIT}" protocol_byte_limit)
    file(READ "${HTTP_TRANSFER_CODING_DECODER}" http_transfer_coding_decoder)
    file(READ "${WEB_REQUEST_BODY_LIMIT}" web_request_body_limit)
    file(READ "${WEB_SERVER_OPTIONS}" web_server_options)
    file(READ "${WEB_APP_HEADER}" web_app_header)
    file(READ "${HTTP_WEBSOCKET_UTILS}" http_websocket_utils)
    file(READ "${HTTP_WEBSOCKET_DEFLATE}" http_websocket_deflate)
    file(READ "${HTTP_WEBSOCKET_CONNECTION}" http_websocket_connection)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_package_consumer)
    file(READ "${WEB_PACKAGE_CONSUMER}" web_package_consumer)
    if(NOT protocol_byte_limit MATCHES "class ProtocolByteLimit final" OR
       NOT protocol_byte_limit MATCHES "ProtocolByteLimit limited[(]std::size_t bytes[)]" OR
       NOT protocol_byte_limit MATCHES "ProtocolByteLimit unlimited[(][)]" OR
       NOT protocol_byte_limit MATCHES "bool additionExceeds[(]" OR
       NOT http_transfer_coding_decoder MATCHES "ProtocolByteLimit bodyLimit" OR
       NOT web_request_body_limit MATCHES "ProtocolByteLimit requestBodyByteLimit" OR
       NOT web_server_options MATCHES
           "std::optional<std::size_t> maxStreamBodyBytes" OR
       NOT web_app_header MATCHES
           "setMaxStreamBodyBytes[(]std::optional<std::size_t> bytes[)]" OR
       NOT http_websocket_utils MATCHES
           "ProtocolByteLimit messageLimit" OR
       NOT http_websocket_deflate MATCHES
           "ProtocolByteLimit messageLimit" OR
       NOT http_websocket_connection MATCHES
           "ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited" OR
       NOT http_package_consumer MATCHES
           "!std::constructible_from<ruvia::ProtocolByteLimit, std::size_t>" OR
       NOT web_package_consumer MATCHES
           "defaultOptions[.]maxStreamBodyBytes[.]has_value")
        boundary_error("protocol byte limits recovered a numeric sentinel"
            "HTTP body and WebSocket protocol paths must consume ProtocolByteLimit; Web configuration and package consumers pin the boundary contracts")
    endif()
endif()
if(EXISTS "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpBodyByteLimit.h")
    boundary_error("body-only byte-limit type was restored"
        "ProtocolByteLimit must remain shared by HTTP body and WebSocket protocol paths")
endif()
check_files_no_match("protocol byte limits recovered a numeric sentinel"
    "kDefaultMaxStreamBodyBytes|maxBodyBytes_[ 	]*(==|!=)[ 	]*0|totalLimit[ 	]*(==|!=)[ 	]*0|maxMessageBytes[ 	]*(==|!=)[ 	]*0|maxBytes[ 	]*(==|!=)[ 	]*0"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/HttpLimits.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/body/HttpTransferCodingDecoder.h"
    "${RUVIA_ROOT}/ruvia-http/src/body/HttpTransferCodingDecoder.cpp"
    "${HTTP_WEBSOCKET_UTILS}"
    "${HTTP_WEBSOCKET_DEFLATE}"
    "${HTTP_WEBSOCKET_CONNECTION}"
    "${RUVIA_ROOT}/ruvia-http/src/websocket/WsConnection.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoStreamRuntime.h")

set(HTTP2_STREAM_REQUEST_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamRequestState.h")
set(HTTP2_STREAM_STATE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2StreamState.h")
set(HTTP2_CONNECTION_SOURCE
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Connection.cpp")
set(HTTP2_RESPONSE_STATUS_TEST
    "${RUVIA_ROOT}/tests/unit_http2_request_headers.cpp")
set(HTTP_PACKAGE_VERIFY
    "${RUVIA_ROOT}/tests/verify_package_consumers.cmake.in")
if(EXISTS "${HTTP2_STREAM_REQUEST_STATE}" AND
   EXISTS "${HTTP2_STREAM_STATE}" AND
   EXISTS "${HTTP2_CONNECTION_SOURCE}" AND
   EXISTS "${HTTP2_RESPONSE_STATUS_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}" AND
   EXISTS "${HTTP_PACKAGE_VERIFY}")
    file(READ "${HTTP2_STREAM_REQUEST_STATE}" http2_stream_request_state)
    file(READ "${HTTP2_STREAM_STATE}" http2_response_status_stream)
    read_http2_connection_implementation(http2_response_status_connection)
    file(READ "${HTTP2_RESPONSE_STATUS_TEST}" http2_response_status_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_response_status_consumer)
    file(READ "${HTTP_PACKAGE_VERIFY}" http2_response_status_package_verify)
    if(NOT http2_stream_request_state MATCHES
           "std::optional<std::uint16_t> responseStatus_" OR
       NOT http2_stream_request_state MATCHES
           "const std::uint16_t[*] responseStatus[(][)] const & noexcept" OR
       NOT http2_stream_request_state MATCHES
           "bool setResponseStatus[(]std::uint16_t status[)] noexcept" OR
       NOT http2_stream_request_state MATCHES "if [(]responseStatus_[)]" OR
       NOT http2_response_status_stream MATCHES
           "return requestState_[.]setResponseStatus[(]status[)]" OR
       NOT http2_response_status_connection MATCHES
           "std::optional<std::uint16_t> status" OR
       NOT http2_response_status_connection MATCHES
           "if [(]!stream[.]setResponseStatus[(][*]context[.]status[)][)]" OR
       NOT http2_response_status_test MATCHES
           "h2_response_status_is_optional_and_single_assignment" OR
       NOT http2_response_status_consumer MATCHES
           "const std::uint16_t[*]>" OR
       NOT http2_response_status_package_verify MATCHES
           "client response-status state lost its explicit alternatives")
        boundary_error("HTTP/2 client response status lost explicit single assignment"
            "absence must be optional, final :status must commit once, and unit/install consumers must pin the contract")
    endif()
endif()
check_files_no_match("HTTP/2 client response status recovered a sentinel or parallel seen flag"
    "${RULE_STALE_H2_RESPONSE_STATUS_PRODUCT}"
    "${HTTP2_STREAM_REQUEST_STATE}"
    "${HTTP2_STREAM_STATE}"
    "${HTTP2_CONNECTION_SOURCE}")

set(HTTP2_HPACK
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Hpack.h")
set(HTTP2_HPACK_HEADER_DECODE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2HeaderDecode.h")
set(HTTP2_HPACK_SOURCES
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2Hpack.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2HpackDynamic.cpp"
    "${RUVIA_ROOT}/ruvia-http/src/http2/Http2HpackHuffman.cpp")
set(HTTP2_HPACK_TEST "${RUVIA_ROOT}/tests/unit_hpack.cpp")
if(EXISTS "${HTTP2_HPACK}" AND
   EXISTS "${HTTP2_HPACK_HEADER_DECODE}" AND
   EXISTS "${HTTP2_HPACK_TEST}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}" AND
   EXISTS "${HTTP_PACKAGE_VERIFY}")
    file(READ "${HTTP2_HPACK}" http2_hpack)
    file(READ "${HTTP2_HPACK_HEADER_DECODE}" http2_hpack_header_decode)
    file(READ "${HTTP2_HPACK_TEST}" http2_hpack_test)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http2_hpack_consumer)
    file(READ "${HTTP_PACKAGE_VERIFY}" http2_hpack_package_verify)
    if(NOT http2_hpack MATCHES "class HpackDecoded final" OR
       NOT http2_hpack MATCHES "class HpackDecodeFailure final" OR
       NOT http2_hpack MATCHES "class HpackDecodeResult final" OR
       NOT http2_hpack MATCHES "std::variant<HpackDecoded" OR
       NOT http2_hpack MATCHES "decoded[(][)] const [&] noexcept" OR
       NOT http2_hpack MATCHES "failure[(][)] const [&] noexcept" OR
       NOT http2_hpack MATCHES "decoded[(][)] const && = delete" OR
       NOT http2_hpack MATCHES "failure[(][)] const && = delete" OR
       NOT http2_hpack MATCHES "using StepResult = std::optional" OR
       NOT http2_hpack_header_decode MATCHES
           "result[.]failure[(][)][-][>]error[(][)]" OR
       NOT http2_hpack_test MATCHES
           "hpack_integer_overflow_is_rejected" OR
       NOT http2_hpack_test MATCHES "failure[-][>]error[(][)]" OR
       NOT http2_hpack_test MATCHES
           "HasAnyRvalueHpackDecodeAccessor" OR
       NOT http2_hpack_consumer MATCHES
           "!std::default_initializable<[^>]*HpackDecodeResult" OR
       NOT http2_hpack_package_verify MATCHES
           "installed HPACK decode result lost its discriminated success/failure contract")
        boundary_error("HPACK decode result lost its discriminated contract"
            "success, callback rejection, and compression faults must not share an error sentinel")
    endif()
endif()
check_files_no_match("HPACK decode result recovered an error sentinel"
    "${RULE_STALE_HPACK_DECODE_RESULT}"
    "${HTTP2_HPACK}"
    "${HTTP2_HPACK_HEADER_DECODE}"
    ${HTTP2_HPACK_SOURCES})

set(HTTP_OPERATION_RESULT_H1
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP_OPERATION_RESULT_H2_CONNECTION
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h")
set(HTTP_OPERATION_RESULT_H2_PEER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2PeerSettings.h")
set(HTTP_OPERATION_RESULT_H2_PLAN
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
set(HTTP_OPERATION_RESULT_CONTROL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpFinalResponseControlPlan.h")
set(HTTP_OPERATION_RESULT_TRAILERS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseTrailers.h")
if(EXISTS "${HTTP_OPERATION_RESULT_H1}" AND
   EXISTS "${HTTP_OPERATION_RESULT_H2_CONNECTION}" AND
   EXISTS "${HTTP_OPERATION_RESULT_H2_PEER}" AND
   EXISTS "${HTTP_OPERATION_RESULT_H2_PLAN}" AND
   EXISTS "${HTTP_OPERATION_RESULT_CONTROL}" AND
   EXISTS "${HTTP_OPERATION_RESULT_TRAILERS}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP_OPERATION_RESULT_H1}" http_operation_result_h1)
    file(READ "${HTTP_OPERATION_RESULT_H2_CONNECTION}"
        http_operation_result_h2_connection)
    file(READ "${HTTP_OPERATION_RESULT_H2_PEER}" http_operation_result_h2_peer)
    file(READ "${HTTP_OPERATION_RESULT_H2_PLAN}" http_operation_result_h2_plan)
    file(READ "${HTTP_OPERATION_RESULT_CONTROL}" http_operation_result_control)
    file(READ "${HTTP_OPERATION_RESULT_TRAILERS}" http_operation_result_trailers)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_operation_result_consumer)
    if(NOT http_operation_result_h1 MATCHES
           "committed[(][)] const [&] noexcept" OR
       NOT http_operation_result_h1 MATCHES
           "prepared[(][)] const [&] noexcept" OR
       NOT http_operation_result_h1 MATCHES
           "prepared[(][)] [&] noexcept" OR
       NOT http_operation_result_h1 MATCHES
           "committed[(][)] const && = delete" OR
       NOT http_operation_result_h2_connection MATCHES
           "WebSocketServerNegotiation[*][ \t\r\n]+submitted[(][)] const [&] noexcept" OR
       NOT http_operation_result_h2_connection MATCHES
           "Http2SubmittedRequestHead[*][ \t\r\n]+submitted[(][)] const [&] noexcept" OR
       NOT http_operation_result_h2_connection MATCHES
           "const Plan[*] submitted[(][)] const [&] noexcept" OR
       NOT http_operation_result_h2_connection MATCHES
           "submitted[(][)] const && = delete" OR
       NOT http_operation_result_h2_peer MATCHES
           "applied[(][)] const [&] noexcept" OR
       NOT http_operation_result_h2_peer MATCHES
           "initialWindowChange[(][)] const && = delete" OR
       NOT http_operation_result_h2_plan MATCHES
           "plan[(][)] const [&] noexcept" OR
       NOT http_operation_result_control MATCHES
           "control[(][)] const [&] noexcept" OR
       NOT http_operation_result_control MATCHES
           "control[(][)] const && = delete" OR
       NOT http_operation_result_trailers MATCHES
           "section[(][)] const [&] noexcept" OR
       NOT http_operation_result_trailers MATCHES
           "section[(][)] const && = delete" OR
       NOT http_operation_result_consumer MATCHES
           "ExposesAnyRvalueHttpOperationResultAccessor")
        boundary_error("HTTP operation results expose alternatives from temporary owners"
            "HTTP/1 commit, HTTP/2 submission/settings/planning, final control, and trailer results must lend alternative pointers only from live lvalue result owners")
    endif()
endif()

set(HTTP_PROTOCOL_PLAN_RANGE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpByteRange.h")
set(HTTP_PROTOCOL_PLAN_SEMANTICS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpResponseContentSemantics.h")
set(HTTP_PROTOCOL_PLAN_WRITE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseWritePlan.h")
set(HTTP_PROTOCOL_PLAN_POLICY
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseHeadPolicy.h")
set(HTTP_PROTOCOL_PLAN_H1_REQUEST
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1RequestBodyPlan.h")
set(HTTP_PROTOCOL_PLAN_H1_RESPONSE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ResponseHeadPlan.h")
set(HTTP_PROTOCOL_PLAN_H2_REQUEST
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2RequestContent.h")
set(HTTP_PROTOCOL_PLAN_H2_RESPONSE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
set(HTTP_PROTOCOL_PLAN_CONTROL
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpFinalResponseControlPlan.h")
set(HTTP_PROTOCOL_PLAN_STREAM
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h")
set(HTTP_PROTOCOL_REQUEST_FACTS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpExpectations.h")
set(HTTP_PROTOCOL_CONNECTION_FACTS
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpConnectionFields.h")
set(HTTP_PROTOCOL_CLIENT_REQUEST_CONTEXT
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h")
check_files_no_match("response write policy must remain a small value fact"
    "const[ \t]+auto&[ \t]+policy[ \t]*=[ \t]*bodyPlan[.]policy[(][)]"
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
check_files_no_match("response body facts must remain value semantic"
    "const[ \t]+auto&[ \t]+bodyPlan[ \t]*=[ \t]*(commitPlan|plan|writePlan)[.]bodyPlan[(][)]"
    "${RUVIA_ROOT}/ruvia-http/src/server/HttpResponseHead.cpp"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2ResponseHeadPlan.h")
check_files_no_match("request and connection facts must remain value semantic"
    "const[ \t]+auto&[ \t]+(responseOptions|upgradeProtocols)[ \t]*=[ \t]*http1Control[.](connectionOptions|upgradeProtocols)[(][)]"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
if(EXISTS "${HTTP_PROTOCOL_PLAN_RANGE}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_SEMANTICS}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_WRITE}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_POLICY}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_H1_REQUEST}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_H1_RESPONSE}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_H2_REQUEST}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_H2_RESPONSE}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_CONTROL}" AND
   EXISTS "${HTTP_PROTOCOL_PLAN_STREAM}" AND
   EXISTS "${HTTP_PROTOCOL_REQUEST_FACTS}" AND
   EXISTS "${HTTP_PROTOCOL_CONNECTION_FACTS}" AND
   EXISTS "${HTTP_PROTOCOL_CLIENT_REQUEST_CONTEXT}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP_PROTOCOL_PLAN_RANGE}" http_protocol_plan_range)
    file(READ "${HTTP_PROTOCOL_PLAN_SEMANTICS}" http_protocol_plan_semantics)
    file(READ "${HTTP_PROTOCOL_PLAN_WRITE}" http_protocol_plan_write)
    file(READ "${HTTP_PROTOCOL_PLAN_POLICY}" http_protocol_plan_policy)
    file(READ "${HTTP_PROTOCOL_PLAN_H1_REQUEST}" http_protocol_plan_h1_request)
    file(READ "${HTTP_PROTOCOL_PLAN_H1_RESPONSE}" http_protocol_plan_h1_response)
    file(READ "${HTTP_PROTOCOL_PLAN_H2_REQUEST}" http_protocol_plan_h2_request)
    file(READ "${HTTP_PROTOCOL_PLAN_H2_RESPONSE}" http_protocol_plan_h2_response)
    file(READ "${HTTP_PROTOCOL_PLAN_CONTROL}" http_protocol_plan_control)
    file(READ "${HTTP_PROTOCOL_PLAN_STREAM}" http_protocol_plan_stream)
    file(READ "${HTTP_PROTOCOL_REQUEST_FACTS}" http_protocol_request_facts)
    file(READ "${HTTP_PROTOCOL_CONNECTION_FACTS}" http_protocol_connection_facts)
    file(READ "${HTTP_PROTOCOL_CLIENT_REQUEST_CONTEXT}"
        http_protocol_client_request_context)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_protocol_plan_consumer)
    if(NOT http_protocol_plan_range MATCHES
           "ignored[(][)] const [&] noexcept" OR
       NOT http_protocol_plan_range MATCHES
           "resolved[(][)] const && = delete" OR
       NOT http_protocol_plan_semantics MATCHES
           "enum class HttpResponseContentSemantics : std::uint8_t" OR
       http_protocol_plan_semantics MATCHES
           "std::variant|std::holds_alternative|bool withContent[(]" OR
       NOT http_protocol_plan_write MATCHES
           "ResponseWritePolicy[ \t]+policy[(][)] const noexcept" OR
       http_protocol_plan_write MATCHES
           "const[ \t]+ResponseWritePolicy[ \t]*&[ \t]+policy" OR
       NOT http_protocol_plan_policy MATCHES
           "is_trivially_copyable_v<ResponseWritePolicy>" OR
       NOT http_protocol_plan_policy MATCHES
           "sizeof[(]ResponseWritePolicy[)] <= 2" OR
       NOT http_protocol_plan_policy MATCHES
           "using State = std::variant" OR
       NOT http_protocol_plan_policy MATCHES
           "ResponseNormalWrite" OR
       NOT http_protocol_plan_policy MATCHES
           "ResponseBodyForbiddenWrite" OR
       NOT http_protocol_plan_policy MATCHES
           "ResponseZeroLengthWrite" OR
       NOT http_protocol_plan_policy MATCHES
           "ResponseNotModifiedWrite" OR
       NOT http_protocol_plan_policy MATCHES
           "normal[(][)] const [&] noexcept" OR
       NOT http_protocol_plan_policy MATCHES
           "notModified[(][)] const && = delete" OR
       http_protocol_plan_policy MATCHES
           "bool[ 	]+bodyAllowed_|bool[ 	]+autoContentLengthAllowed_|bool[ 	]+explicitContentLengthAllowed_|bool[ 	]+transferEncodingAllowed_|zeroLengthContent" OR
       NOT http_protocol_plan_write MATCHES
           "HttpResponseContentSemantics[ \t\r\n]+contentSemantics[(][)] const noexcept" OR
       NOT http_protocol_plan_write MATCHES
           "HttpResponseBodyPlan bodyPlan[(][)] const noexcept" OR
       NOT http_protocol_plan_write MATCHES
           "is_trivially_copyable_v<HttpResponseBodyPlan>" OR
       NOT http_protocol_plan_write MATCHES
           "sizeof[(]HttpResponseBodyPlan[)] <= 12" OR
       NOT http_protocol_plan_h1_request MATCHES
           "withoutBody[(][)] const [&] noexcept" OR
       NOT http_protocol_plan_h1_request MATCHES
           "HttpRequestExpectations[ \t]+expectations[(][)] const noexcept" OR
       http_protocol_plan_h1_request MATCHES
           "const[ \t]+HttpRequestExpectations[ \t]*&[ \t]+expectations" OR
       NOT http_protocol_request_facts MATCHES
           "is_trivially_copyable_v<HttpRequestExpectations>" OR
       NOT http_protocol_request_facts MATCHES
           "sizeof[(]HttpRequestExpectations[)] <= 1" OR
       NOT http_protocol_plan_h1_response MATCHES
           "buffered[(][)] const [&] noexcept" OR
       NOT http_protocol_plan_h1_response MATCHES
           "closeDelimitedStream[(][)] const && = delete" OR
       NOT http_protocol_plan_h1_response MATCHES
           "HttpResponseBodyPlan bodyPlan[(][)] const noexcept" OR
       NOT http_protocol_plan_h1_response MATCHES
           "std::uint16_t responseStatus[(][)] const noexcept" OR
       NOT http_protocol_plan_h1_response MATCHES
           "std::uint64_t contentLength[(][)] const noexcept" OR
       NOT http_protocol_plan_h1_response MATCHES
           "bool sendBody[(][)] const noexcept" OR
       NOT http_protocol_plan_h1_response MATCHES
           "is_trivially_copyable_v<Http1BufferedResponsePlan>" OR
       NOT http_protocol_plan_h1_response MATCHES
           "sizeof[(]Http1BufferedResponsePlan[)] == sizeof[(]Http1ResponseHeadPlan[)]" OR
       http_protocol_plan_h1_response MATCHES
           "HttpBufferedResponseWritePlan writePlan_" OR
       http_protocol_plan_h1_response MATCHES
           "writePlan[(][)] const [&] noexcept" OR
       NOT http_protocol_plan_h1_response MATCHES
           "headPlan[(][)] const && = delete" OR
       NOT http_protocol_plan_h2_request MATCHES
           "withoutContent[(][)] const [&] noexcept" OR
       NOT http_protocol_plan_h2_request MATCHES
           "streamingContent[(][)] const && = delete" OR
       NOT http_protocol_plan_h2_response MATCHES
           "contentLength[(][)] const noexcept" OR
       NOT http_protocol_plan_h2_response MATCHES
           "streamingContentLength[(][)] const noexcept" OR
       NOT http_protocol_plan_h2_response MATCHES
           "is_trivially_copyable_v<Http2ResponseHeadPlan>" OR
       NOT http_protocol_plan_h2_response MATCHES
           "sizeof[(]Http2ResponseHeadPlan[)] <= 24" OR
       NOT http_protocol_plan_h2_response MATCHES
           "HttpResponseBodyPlan bodyPlan[(][)] const noexcept" OR
       NOT http_protocol_plan_control MATCHES
           "control[(][)] const [&] noexcept" OR
       NOT http_protocol_plan_control MATCHES
           "control[(][)] const && = delete" OR
       NOT http_protocol_plan_control MATCHES
           "HttpConnectionOptions[ \t\r\n]+connectionOptions[(][)] const noexcept" OR
       NOT http_protocol_plan_control MATCHES
           "HttpUpgradeProtocols[ \t\r\n]+upgradeProtocols[(][)] const noexcept" OR
       NOT http_protocol_client_request_context MATCHES
           "HttpConnectionOptions[ \t]+connectionOptions[(][)] const noexcept" OR
       NOT http_protocol_connection_facts MATCHES
           "is_trivially_copyable_v<HttpConnectionOptions>" OR
       NOT http_protocol_connection_facts MATCHES
           "sizeof[(]HttpConnectionOptions[)] == 1" OR
       NOT http_protocol_connection_facts MATCHES
           "kFieldPresentBit" OR
       http_protocol_connection_facts MATCHES
           "bool[ \t]+fieldPresent_" OR
       NOT http_protocol_connection_facts MATCHES
           "is_trivially_copyable_v<HttpUpgradeProtocols>" OR
       NOT http_protocol_connection_facts MATCHES
           "enum class HttpUpgradeFieldState" OR
       NOT http_protocol_connection_facts MATCHES
           "HttpUpgradeFieldState[ \t]+state_" OR
       http_protocol_connection_facts MATCHES
           "bool[ \t]+hasProtocol_" OR
       NOT http_protocol_connection_facts MATCHES
           "sizeof[(]HttpUpgradeProtocols[)] == 1" OR
       NOT http_protocol_plan_stream MATCHES
           "HttpResponseBodyPlan bodyPlan[(][)] const noexcept" OR
       NOT http_protocol_plan_consumer MATCHES
           "ExposesAnyRvalueHttpProtocolPlanBorrow" OR
       NOT http_protocol_plan_consumer MATCHES
           "HasValueSemanticResponseWritePolicy" OR
       NOT http_protocol_plan_consumer MATCHES
           "ExposesAnyRvalueResponseWritePolicyAlternative" OR
       NOT http_protocol_plan_consumer MATCHES
           "HasLegacyResponseWritePolicyFactory" OR
       NOT http_protocol_plan_consumer MATCHES
           "is_enum_v<[\r\n \t]*ruvia::detail::HttpResponseContentSemantics>" OR
       NOT http_protocol_plan_consumer MATCHES
           "HasValueSemanticResponseBodyPlan")
        boundary_error("HTTP protocol plan fact ownership is inconsistent"
            "payload alternatives may borrow only from live lvalues, while cheap response semantics, body plans, request facts, and connection facts must propagate by value")
    endif()
endif()

set(HTTP_OPERATION_PAYLOAD_REQUEST
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1RequestParser.h")
set(HTTP_OPERATION_PAYLOAD_CLIENT_RESPONSE
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientResponseParser.h")
set(HTTP_OPERATION_PAYLOAD_CLIENT_REQUEST
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1ClientRequestWriter.h")
set(HTTP_OPERATION_PAYLOAD_H1
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerSemantics.h")
set(HTTP_OPERATION_PAYLOAD_STREAM
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/server/HttpResponseStreamHead.h")
set(HTTP_OPERATION_PAYLOAD_WEBSOCKET
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketServerHandshake.h")
if(EXISTS "${HTTP_OPERATION_PAYLOAD_REQUEST}" AND
   EXISTS "${HTTP_OPERATION_PAYLOAD_CLIENT_RESPONSE}" AND
   EXISTS "${HTTP_OPERATION_PAYLOAD_CLIENT_REQUEST}" AND
   EXISTS "${HTTP_OPERATION_PAYLOAD_H1}" AND
   EXISTS "${HTTP_OPERATION_PAYLOAD_STREAM}" AND
   EXISTS "${HTTP_OPERATION_PAYLOAD_WEBSOCKET}" AND
   EXISTS "${HTTP_PACKAGE_CONSUMER}")
    file(READ "${HTTP_OPERATION_PAYLOAD_REQUEST}" http_operation_payload_request)
    file(READ "${HTTP_OPERATION_PAYLOAD_CLIENT_RESPONSE}"
        http_operation_payload_client_response)
    file(READ "${HTTP_OPERATION_PAYLOAD_CLIENT_REQUEST}"
        http_operation_payload_client_request)
    file(READ "${HTTP_OPERATION_PAYLOAD_H1}" http_operation_payload_h1)
    file(READ "${HTTP_OPERATION_PAYLOAD_STREAM}" http_operation_payload_stream)
    file(READ "${HTTP_OPERATION_PAYLOAD_WEBSOCKET}"
        http_operation_payload_websocket)
    file(READ "${HTTP_PACKAGE_CONSUMER}" http_operation_payload_consumer)
    if(NOT http_operation_payload_request MATCHES
           "request[(][)] const [&] noexcept" OR
       NOT http_operation_payload_request MATCHES
           "request[(][)] const && = delete" OR
       NOT http_operation_payload_request MATCHES
           "bodyPlan[(][)] const && = delete" OR
       NOT http_operation_payload_client_response MATCHES
           "head[(][)] const [&] noexcept" OR
       NOT http_operation_payload_client_response MATCHES
           "head[(][)] const && = delete" OR
       NOT http_operation_payload_client_response MATCHES
           "plan[(][)] const && = delete" OR
       NOT http_operation_payload_client_request MATCHES
           "contentPlan[(][)] const [&] noexcept" OR
       NOT http_operation_payload_client_request MATCHES
           "contentPlan[(][)] const && = delete" OR
       NOT http_operation_payload_stream MATCHES
           "response[(][)] [&] noexcept" OR
       NOT http_operation_payload_stream MATCHES
           "response[(][)] && = delete" OR
       NOT http_operation_payload_stream MATCHES
           "response[(][)] const && = delete" OR
       NOT http_operation_payload_stream MATCHES
           "commitPlan[(][)] const && = delete" OR
       NOT http_operation_payload_h1 MATCHES
           "response[(][)] [&] noexcept" OR
       NOT http_operation_payload_h1 MATCHES
           "responseHeadPlan[(][)] const && = delete" OR
       NOT http_operation_payload_h1 MATCHES
           "commitPlan[(][)] const && = delete" OR
       NOT http_operation_payload_websocket MATCHES
           "negotiation[(][)] const [&] noexcept" OR
       NOT http_operation_payload_websocket MATCHES
           "negotiation[(][)] const && = delete" OR
       NOT http_operation_payload_consumer MATCHES
           "ExposesAnyRvalueHttpOperationPayloadBorrow")
        boundary_error("HTTP operation payloads lend owned state from temporary values"
            "parsed and prepared payloads must expose owned request, response head, and negotiation objects only from live lvalues")
    endif()
endif()

set(WEB_EXECUTION_ROUTE_RESOLUTION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouteResolution.h")
set(WEB_EXECUTION_STREAM_DISPATCH
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpResponseStreamDispatch.h")
set(WEB_EXECUTION_WEBSOCKET_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerWebSocketRoute.h")
set(WEB_EXECUTION_HTTP1_WRITE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1BufferedResponseWrite.h")
set(WEB_EXECUTION_HTTP2_WRITE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2BufferedResponseWrite.h")
set(WEB_EXECUTION_REQUEST_COMPLETION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1SessionRequestCompletion.h")
set(WEB_EXECUTION_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(web_execution_contract IN ITEMS
        "${WEB_EXECUTION_ROUTE_RESOLUTION}"
        "${WEB_EXECUTION_STREAM_DISPATCH}"
        "${WEB_EXECUTION_WEBSOCKET_ROUTE}"
        "${WEB_EXECUTION_HTTP1_WRITE}"
        "${WEB_EXECUTION_HTTP2_WRITE}"
        "${WEB_EXECUTION_REQUEST_COMPLETION}"
        "${WEB_EXECUTION_PACKAGE_CONSUMER}")
    if(NOT EXISTS "${web_execution_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${web_execution_contract}")
        boundary_error("Web execution result lifetime contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_EXECUTION_ROUTE_RESOLUTION}" AND
   EXISTS "${WEB_EXECUTION_STREAM_DISPATCH}" AND
   EXISTS "${WEB_EXECUTION_WEBSOCKET_ROUTE}" AND
   EXISTS "${WEB_EXECUTION_HTTP1_WRITE}" AND
   EXISTS "${WEB_EXECUTION_HTTP2_WRITE}" AND
   EXISTS "${WEB_EXECUTION_REQUEST_COMPLETION}" AND
   EXISTS "${WEB_EXECUTION_PACKAGE_CONSUMER}")
    file(READ "${WEB_EXECUTION_ROUTE_RESOLUTION}"
        web_execution_route_resolution)
    file(READ "${WEB_EXECUTION_STREAM_DISPATCH}"
        web_execution_stream_dispatch)
    file(READ "${WEB_EXECUTION_WEBSOCKET_ROUTE}"
        web_execution_websocket_route)
    file(READ "${WEB_EXECUTION_HTTP1_WRITE}"
        web_execution_http1_write)
    file(READ "${WEB_EXECUTION_HTTP2_WRITE}"
        web_execution_http2_write)
    file(READ "${WEB_EXECUTION_REQUEST_COMPLETION}"
        web_execution_request_completion)
    file(READ "${WEB_EXECUTION_PACKAGE_CONSUMER}"
        web_execution_package_consumer)
    if(NOT web_execution_route_resolution MATCHES
           "values[(][)] const [&] noexcept" OR
       NOT web_execution_route_resolution MATCHES
           "match[(][)] const && = delete" OR
       NOT web_execution_route_resolution MATCHES
           "resolved[(][)] const [&] noexcept" OR
       NOT web_execution_route_resolution MATCHES
           "notFound[(][)] const && = delete" OR
       NOT web_execution_stream_dispatch MATCHES
           "completed[(][)] const [&] noexcept" OR
       NOT web_execution_stream_dispatch MATCHES
           "routeResponse[(][)] && = delete" OR
       NOT web_execution_stream_dispatch MATCHES
           "recoveredFailure[(][)] && = delete" OR
       NOT web_execution_websocket_route MATCHES
           "Task<std::optional<Http1SessionRequestCompletion>> dispatchHttpWebSocketRoute" OR
       web_execution_websocket_route MATCHES
           "HttpWebSocketRouteResult|HttpWebSocketSessionFinished|std::variant|requestCompletion[(]|sessionFinished[(]|class HttpWebSocketBufferedResponse final|bufferedResponse[(][)] const [&] noexcept" OR
       NOT web_execution_http1_write MATCHES
           "completed[(][)] const [&] noexcept" OR
       NOT web_execution_http1_write MATCHES
           "failedAfterCommit[(][)] const && = delete" OR
       NOT web_execution_http1_write MATCHES
           "committedStatus[(][)] const noexcept" OR
       NOT web_execution_http2_write MATCHES
           "committedStatus[(][)] const noexcept" OR
       NOT web_execution_request_completion MATCHES
           "compaction[(][)] const [&] noexcept" OR
       NOT web_execution_request_completion MATCHES
           "bufferCompletion[(][)] const && = delete" OR
       NOT web_execution_package_consumer MATCHES
           "ExposesAnyRvalueWebExecutionBorrow")
        boundary_error("Web execution results expose payloads from temporary owners"
            "routing, streaming, WebSocket, buffered-write, file, and request-completion results must lend alternatives only from live lvalues")
    endif()
endif()

set(WEB_DB_OWNED_VIEW_TYPES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbTypes.h")
set(WEB_DB_OWNED_VIEW_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbQueryResult.h")
set(WEB_DB_OWNED_VIEW_MIGRATION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/db/DbMigration.h")
set(WEB_DB_OWNED_VIEW_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/db/DbTypes.cpp")
set(WEB_DB_OWNED_VIEW_TEST
    "${RUVIA_ROOT}/tests/unit_db_api_surface.cpp")
set(WEB_REDIS_OWNED_VIEW_TYPES
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/redis/RedisTypes.h")
set(WEB_REDIS_OWNED_VIEW_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/redis/RedisTypes.cpp")
set(WEB_REDIS_OWNED_VIEW_TEST
    "${RUVIA_ROOT}/tests/unit_redis_api_surface.cpp")
set(WEB_INTEGRATION_OWNED_VIEW_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/web.cpp")
foreach(integration_owned_view_contract IN ITEMS
        "${WEB_DB_OWNED_VIEW_TYPES}"
        "${WEB_DB_OWNED_VIEW_RESULT}"
        "${WEB_DB_OWNED_VIEW_MIGRATION}"
        "${WEB_DB_OWNED_VIEW_SOURCE}"
        "${WEB_DB_OWNED_VIEW_TEST}"
        "${WEB_REDIS_OWNED_VIEW_TYPES}"
        "${WEB_REDIS_OWNED_VIEW_SOURCE}"
        "${WEB_REDIS_OWNED_VIEW_TEST}"
        "${WEB_INTEGRATION_OWNED_VIEW_CONSUMER}")
    if(NOT EXISTS "${integration_owned_view_contract}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}"
            "${integration_owned_view_contract}")
        boundary_error("integration owned-view lifetime contract is incomplete"
            "${relative} is required")
    endif()
endforeach()
if(EXISTS "${WEB_DB_OWNED_VIEW_TYPES}" AND
   EXISTS "${WEB_DB_OWNED_VIEW_RESULT}" AND
   EXISTS "${WEB_DB_OWNED_VIEW_MIGRATION}" AND
   EXISTS "${WEB_DB_OWNED_VIEW_SOURCE}" AND
   EXISTS "${WEB_DB_OWNED_VIEW_TEST}" AND
   EXISTS "${WEB_REDIS_OWNED_VIEW_TYPES}" AND
   EXISTS "${WEB_REDIS_OWNED_VIEW_SOURCE}" AND
   EXISTS "${WEB_REDIS_OWNED_VIEW_TEST}" AND
   EXISTS "${WEB_INTEGRATION_OWNED_VIEW_CONSUMER}")
    file(READ "${WEB_DB_OWNED_VIEW_TYPES}" web_db_owned_view_types)
    file(READ "${WEB_DB_OWNED_VIEW_RESULT}" web_db_owned_view_result)
    file(READ "${WEB_DB_OWNED_VIEW_MIGRATION}" web_db_owned_view_migration)
    file(READ "${WEB_DB_OWNED_VIEW_SOURCE}" web_db_owned_view_source)
    file(READ "${WEB_DB_OWNED_VIEW_TEST}" web_db_owned_view_test)
    file(READ "${WEB_REDIS_OWNED_VIEW_TYPES}" web_redis_owned_view_types)
    file(READ "${WEB_REDIS_OWNED_VIEW_SOURCE}" web_redis_owned_view_source)
    file(READ "${WEB_REDIS_OWNED_VIEW_TEST}" web_redis_owned_view_test)
    file(READ "${WEB_INTEGRATION_OWNED_VIEW_CONSUMER}"
        web_integration_owned_view_consumer)
    if(NOT web_db_owned_view_types MATCHES
           "text[(][)] const [&] noexcept" OR
       NOT web_db_owned_view_types MATCHES
           "text[(][)] const && = delete" OR
       NOT web_db_owned_view_types MATCHES
           "operator[[]][(][^)]*[)] const && = delete" OR
       NOT web_db_owned_view_types MATCHES
           "begin[(][)] const && = delete" OR
       NOT web_db_owned_view_result MATCHES
           "rows[(][)] const [&] noexcept" OR
       NOT web_db_owned_view_result MATCHES
           "rows[(][)] const && = delete" OR
       NOT web_db_owned_view_migration MATCHES
           "applied[(][)] const [&] noexcept" OR
       NOT web_db_owned_view_migration MATCHES
           "skipped[(][)] const && = delete" OR
       NOT web_db_owned_view_source MATCHES
           "DbValue::text[(][)] const [&] noexcept" OR
       NOT web_db_owned_view_test MATCHES
           "ExposesAnyRvalueDbOwnedView" OR
       NOT web_redis_owned_view_types MATCHES
           "duration[(][)] const [&] noexcept" OR
       NOT web_redis_owned_view_types MATCHES
           "key[(][)] const && = delete" OR
       NOT web_redis_owned_view_types MATCHES
           "entries[(][)] const && = delete" OR
       NOT web_redis_owned_view_types MATCHES
           "message[(][)] const [&] noexcept" OR
       NOT web_redis_owned_view_types MATCHES
           "string[(][)] const && = delete" OR
       NOT web_redis_owned_view_types MATCHES
           "array[(][)] const && = delete" OR
       NOT web_redis_owned_view_source MATCHES
           "RedisValue::array[(][)] const [&]" OR
       NOT web_redis_owned_view_test MATCHES
           "ExposesAnyRvalueRedisOwnedView" OR
       NOT web_integration_owned_view_consumer MATCHES
           "ExposesAnyRvalueDbOwnedView" OR
       NOT web_integration_owned_view_consumer MATCHES
           "ExposesAnyRvalueRedisOwnedView")
        boundary_error("DB or Redis owning values expose views from temporary owners"
            "optional integration strings, rows, arrays, scan results, and migration reports must lend owner-backed storage only from live lvalues")
    endif()
endif()

set(BOUNDARY_DOCS "${RUVIA_ROOT}/README.md" "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("docs reference the deleted coroutine h2 server session"
    "${RULE_DELETED_H2_SESSION}" ${BOUNDARY_DOCS})
check_files_no_match("docs contain stale dependency/runtime ownership"
    "${RULE_STALE_DEPENDENCY}" ${BOUNDARY_DOCS})
check_files_no_match("docs must read the vcpkg toolchain from VCPKG_ROOT"
    "${RULE_HARDCODED_VCPKG_TOOLCHAIN}" ${BOUNDARY_DOCS})
check_files_no_match("README must describe the current product, not migration history"
    "former implementation|former API|no longer|was removed|migration history|旧实现|迁移历史"
    "${RUVIA_ROOT}/README.md")

# Keep the two root documents intentionally small and role-specific. Protocol
# implementation invariants belong to source, unit tests, package consumers, and
# the checks above; requiring every internal type name in both documents caused
# them to become duplicate change logs.
file(READ "${RUVIA_ROOT}/README.md" readme_content)
file(READ "${RUVIA_ROOT}/AGENTS.md" agents_content)
string(REGEX REPLACE "[^\n]" "" readme_newlines "${readme_content}")
string(REGEX REPLACE "[^\n]" "" agents_newlines "${agents_content}")
string(LENGTH "${readme_newlines}" readme_line_count)
string(LENGTH "${agents_newlines}" agents_line_count)
if(NOT readme_content STREQUAL "" AND NOT readme_content MATCHES "\n$")
    math(EXPR readme_line_count "${readme_line_count} + 1")
endif()
if(NOT agents_content STREQUAL "" AND NOT agents_content MATCHES "\n$")
    math(EXPR agents_line_count "${agents_line_count} + 1")
endif()
if(readme_line_count GREATER 400)
    boundary_error("README exceeded its user-document scope"
        "README.md has ${readme_line_count} lines; keep it under 400 and move executable invariants to tests/guards")
endif()
if(agents_line_count GREATER 400)
    boundary_error("AGENTS exceeded its contributor-guide scope"
        "AGENTS.md has ${agents_line_count} lines; keep it under 400 and do not append per-refactor type catalogs")
endif()

if(NOT readme_content MATCHES "## Targets" OR
   NOT readme_content MATCHES "ruvia::core" OR
   NOT readme_content MATCHES "ruvia::http" OR
   NOT readme_content MATCHES "ruvia::web" OR
   NOT readme_content MATCHES "## Build" OR
   NOT readme_content MATCHES "## Install and Consume" OR
   NOT readme_content MATCHES "## (Minimal Web App|Quick Start)" OR
   NOT readme_content MATCHES
       "VCPKG_ROOT/scripts/buildsystems/vcpkg[.]cmake")
    boundary_error("README lost its user-facing contract"
        "README must retain targets, build/install guidance, a minimal app, and VCPKG_ROOT-based configuration")
endif()
if(NOT agents_content MATCHES "README 面向使用者" OR
   NOT agents_content MATCHES "AGENTS 面向贡献者" OR
   NOT agents_content MATCHES "## 目录规则" OR
   NOT agents_content MATCHES "## Target 边界" OR
   NOT agents_content MATCHES "ruvia-web  -> ruvia-core [+] ruvia-http" OR
   NOT agents_content MATCHES "## 性能原则" OR
   NOT agents_content MATCHES "## 验证要求" OR
   NOT agents_content MATCHES
       "VCPKG_ROOT/scripts/buildsystems/vcpkg[.]cmake")
    boundary_error("AGENTS lost its contributor-guide contract"
        "AGENTS must retain document roles, directory/target/dependency rules, performance constraints, verification, and VCPKG_ROOT guidance")
endif()
set(HTTP2_SEND_WINDOW_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoSendWindow.h")
set(HTTP2_SEND_WINDOW_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/server/Http2SansIoSendWindow.cpp")
if(NOT EXISTS "${HTTP2_SEND_WINDOW_HEADER}" OR
   NOT EXISTS "${HTTP2_SEND_WINDOW_SOURCE}")
    boundary_error("HTTP/2 send-window wait contract is missing"
        "Web buffered, streaming, and WebSocket writers must share one typed wait path")
else()
    file(READ "${HTTP2_SEND_WINDOW_HEADER}" http2_send_window_header_content)
    file(READ "${HTTP2_SEND_WINDOW_SOURCE}" http2_send_window_source_content)
    if(NOT http2_send_window_header_content MATCHES
           "class Http2SendWindowWaitResult final" OR
       NOT http2_send_window_header_content MATCHES
           "Task<Http2SendWindowWaitResult> awaitHttp2SendWindow" OR
       NOT http2_send_window_source_content MATCHES
           "signal == nullptr [|][|] signal->terminated[(][)]" OR
       NOT http2_send_window_source_content MATCHES
           "connection[.]hasQueuedData[(]streamId[)]")
        boundary_error("HTTP/2 send-window wait lost its typed state recheck"
            "The shared Web helper must jointly recheck stream, signal, abort, and queued DATA state")
    endif()
endif()

set(http2_send_window_consumers
    "${RUVIA_ROOT}/ruvia-web/src/server/Http2BufferedResponseWrite.cpp"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
foreach(send_window_consumer IN LISTS http2_send_window_consumers)
    file(READ "${send_window_consumer}" send_window_consumer_content)
    if(NOT send_window_consumer_content MATCHES "awaitHttp2SendWindow")
        boundary_error("HTTP/2 writer bypasses the shared send-window wait"
            "${send_window_consumer} must consume awaitHttp2SendWindow")
    endif()
    if(send_window_consumer_content MATCHES "Task<bool>.*awaitSendWindow" OR
       send_window_consumer_content MATCHES "while [(].*hasQueuedData")
        boundary_error("HTTP/2 writer duplicates send-window state decisions"
            "${send_window_consumer} must not restore a bool or local polling path")
    endif()
endforeach()

file(READ "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt" web_cmake_send_window_content)
if(NOT web_cmake_send_window_content MATCHES "Http2SansIoSendWindow[.]cpp")
    boundary_error("HTTP/2 send-window implementation is not built"
        "ruvia-web must compile Http2SansIoSendWindow.cpp")
endif()

set(WS_CONNECTION_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnection.h")
set(WS_CONNECTION_WRITE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl")
set(WS_CONNECTION_HEARTBEAT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketConnectionHeartbeat.inl")
set(WS_CONNECTION_LIVENESS
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketLiveness.h")
file(READ "${WS_CONNECTION_HEADER}" ws_connection_header_content)
file(READ "${WS_CONNECTION_WRITE}" ws_connection_write_content)
file(READ "${WS_CONNECTION_HEARTBEAT}" ws_connection_heartbeat_content)
file(READ "${WS_CONNECTION_LIVENESS}" ws_connection_liveness_content)
set(ws_connection_write_state
    "${ws_connection_header_content}${ws_connection_write_content}${ws_connection_heartbeat_content}")
if(NOT ws_connection_header_content MATCHES "enum class WritePhase" OR
   NOT ws_connection_header_content MATCHES
       "WritePhase writePhase_[{]WritePhase::kIdle[}]" OR
   NOT ws_connection_header_content MATCHES "class WriteGuard final" OR
   NOT ws_connection_header_content MATCHES
       "~WriteGuard[(][)][\r\n \t]*[{][\r\n \t]*connection_[.]finishWrite[(]phase_[)]" OR
   NOT ws_connection_heartbeat_content MATCHES
       "WriteClaim::kAdopt" OR
   NOT ws_connection_write_content MATCHES
       "abortTransport[(][)][;][\r\n \t]+while [(]writePhase_ != WritePhase::kIdle[)]" OR
   ws_connection_write_state MATCHES "detachAndDrainBackgroundWrites" OR
   ws_connection_write_content MATCHES
       "writePhase_ = WritePhase::k(Application|Idle)" OR
   ws_connection_write_state MATCHES
       "writeActive_|heartbeatWriteActive_|backgroundWriteCount_")
    boundary_error("WebSocket write ownership is not a single typed phase"
        "Application and heartbeat writes must transition one Idle/Application/Heartbeat state through the cancellation-safe WriteGuard")
endif()
if(NOT ws_connection_header_content MATCHES
       "WebSocketLivenessState livenessState_" OR
   NOT ws_connection_header_content MATCHES
       "WebSocketLivenessIdle" OR
   NOT ws_connection_liveness_content MATCHES
       "WebSocketSendingPing" OR
   NOT ws_connection_heartbeat_content MATCHES
       "WebSocketAwaitingPong" OR
   NOT ws_connection_write_content MATCHES
       "WebSocketAwaitingPeerClose" OR
   ws_connection_write_state MATCHES
       "awaitingPong_|heartbeatPingSentMs_|localCloseStartedMs_" OR
   NOT ws_connection_write_content MATCHES
       "flushProtocolOutputNow[(][)][;][\r\n \t]+[}][\r\n \t]+if [(]awaitPeerClose [&&]")
    boundary_error("WebSocket liveness ownership is not one committed state"
        "idle, awaiting Pong, and awaiting peer Close must be exclusive; the Close timeout starts only after its transport write commits")
endif()

set(WS_HANDSHAKE_WRITER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketHandshake.h")
file(READ "${WS_HANDSHAKE_WRITER}" ws_handshake_writer_content)
if(NOT ws_handshake_writer_content MATCHES
       "Task<std::error_code> writeWebSocketHandshake" OR
   ws_handshake_writer_content MATCHES
       "Task<bool> writeWebSocketHandshake" OR
   NOT ws_handshake_writer_content MATCHES
       "co_return writeCompletion[.]errorCode[(][)]")
    boundary_error("WebSocket handshake writer discards transport failure identity"
        "The HTTP/1 upgrade path must preserve the concrete async write error_code")
endif()

set(WS_TRANSPORT_READ_RESULT
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/WsTransportReadResult.h")
file(READ "${WS_TRANSPORT_READ_RESULT}" ws_transport_read_result_content)
if(NOT ws_transport_read_result_content MATCHES
       "class WsTransportReadResult final" OR
   NOT ws_transport_read_result_content MATCHES
       "class WsTransportReadFailure final")
    boundary_error("WebSocket transport reads lack exclusive outcomes"
        "Data, orderly end, and concrete transport failure must be distinct alternatives")
endif()
set(ws_transport_read_consumers
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http2/Http2SansIoWsTransport.h")
foreach(ws_read_consumer IN LISTS ws_transport_read_consumers)
    file(READ "${ws_read_consumer}" ws_read_consumer_content)
    if(NOT ws_read_consumer_content MATCHES
           "Task<WsTransportReadResult> readMore" OR
       ws_read_consumer_content MATCHES "Task<bool> readMore")
        boundary_error("WebSocket transport restored a bool read result"
            "${ws_read_consumer} must preserve data/end/failure identity")
    endif()
endforeach()

set(APP_LIFECYCLE_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/app/AppLifecycle.h")
set(APP_INTERNAL_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/app/AppInternal.h")
file(READ "${APP_LIFECYCLE_HEADER}" app_lifecycle_content)
file(READ "${APP_INTERNAL_HEADER}" app_internal_lifecycle_content)
if(NOT app_lifecycle_content MATCHES "enum class AppLifecycleState" OR
   NOT app_lifecycle_content MATCHES "enum class AppStopRequest" OR
   NOT app_internal_lifecycle_content MATCHES "AppLifecycle lifecycle" OR
   app_internal_lifecycle_content MATCHES
       "bool (running|startHooksRunning|stopHooksClaimed|stopRequested)")
    boundary_error("App lifecycle is not represented by one typed state"
        "Run, hooks, deferred stop, and stopping must transition through AppLifecycle")
endif()

set(APP_STARTUP_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/app/App.cpp")
file(READ "${APP_STARTUP_SOURCE}" app_startup_content)
if(NOT app_internal_lifecycle_content MATCHES
       "std::unique_ptr<Router, PmrObjectDeleter<Router>> router" OR
   app_internal_lifecycle_content MATCHES "autoControllersLoaded" OR
   NOT app_startup_content MATCHES "preparedRouter" OR
   NOT app_startup_content MATCHES "preparedControllerLifetimes" OR
   NOT app_startup_content MATCHES "preparedOptions[.]workerFailure" OR
   app_startup_content MATCHES "state[.]options[.]workerFailure[ \\t]*=" OR
   app_startup_content MATCHES
       "registerControllers[(][ \\n\\r\\t]*state[.]router")
    boundary_error("App startup preparation lost its strong exception boundary"
        "Controllers, router, worker options, and runtime must remain prepared until commit")
endif()
string(FIND "${app_startup_content}"
    "processMemory.freeze();" app_startup_freeze_position)
string(FIND "${app_startup_content}"
    "state.runtime = std::move(runtime);" app_runtime_commit_position)
string(FIND "${app_startup_content}"
    "std::visit(" app_listener_preflight_position)
if(app_startup_freeze_position LESS 0 OR
   app_runtime_commit_position LESS app_startup_freeze_position OR
   app_startup_freeze_position LESS app_listener_preflight_position)
    boundary_error("App startup commit occurs before fallible preparation completes"
        "Listener preparation must precede ProcessMemory freeze and runtime publication")
endif()

set(HTTP2_SESSION_LIFECYCLE_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSessionLifecycle.h")
set(HTTP2_SESSION_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
file(READ "${HTTP2_SESSION_LIFECYCLE_HEADER}"
    http2_session_lifecycle_content)
file(READ "${HTTP2_SESSION_HEADER}" http2_session_lifecycle_consumer)
if(NOT http2_session_lifecycle_content MATCHES
       "enum class Http2SansIoSessionPhase" OR
   NOT http2_session_lifecycle_content MATCHES
       "class Http2SansIoSessionLifecycle final" OR
   NOT http2_session_lifecycle_consumer MATCHES
       "Http2SansIoSessionLifecycle lifecycle" OR
   http2_session_lifecycle_consumer MATCHES
       "bool (stopping|writeFailed|writerDone)")
    boundary_error("HTTP/2 Web session restored parallel lifecycle flags"
        "Reader stop, write failure, and writer join must transition one typed lifecycle")
endif()

# Zero-copy protocol results may borrow caller storage, but accepting an owning
# string rvalue would make that storage disappear at the end of the call.
set(HTTP_BORROWED_VIEW_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/BorrowedView.h")
if(NOT EXISTS "${HTTP_BORROWED_VIEW_HEADER}")
    boundary_error("HTTP borrowed-view lifetime guard is missing"
        "All escaping zero-copy APIs must share one owning-string predicate")
else()
    file(READ "${HTTP_BORROWED_VIEW_HEADER}" http_borrowed_view_content)
    if(NOT http_borrowed_view_content MATCHES
           "kIsHttpOwningCharString" OR
       NOT http_borrowed_view_content MATCHES
           "HttpTemporaryOwningCharString")
        boundary_error("HTTP borrowed-view lifetime trait is incomplete"
            "std::string and std::pmr::string rvalues need one shared deleted-overload contract")
    endif()
endif()

set(http_escaping_borrowed_input_headers
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/Http1RequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/MultipartParser.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http1/Http1ServerRequestParser.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/http2/Http2Connection.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpHeaderBlockParser.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/parser/HttpRequestTarget.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/UrlEncoding.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HeaderTokenUtils.h"
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/MultipartParsing.h")
foreach(http_borrowed_input_header IN LISTS http_escaping_borrowed_input_headers)
    file(READ "${http_borrowed_input_header}" http_borrowed_input_content)
    if(NOT http_borrowed_input_content MATCHES
           "HttpTemporaryOwningCharString" OR
       NOT http_borrowed_input_content MATCHES "= delete")
        boundary_error("Escaping HTTP borrowed view accepts a temporary owning string"
            "${http_borrowed_input_header} must reject owning character-string rvalues")
    endif()
endforeach()

set(CONTEXT_SESSION_STATE_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/ContextSessionState.h")
set(CONTEXT_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Context.h")
set(CSRF_INTERNAL_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/CsrfInternal.h")
file(READ "${CONTEXT_SESSION_STATE_HEADER}" context_session_state_content)
file(READ "${CONTEXT_HEADER}" context_session_consumer_content)
file(READ "${CSRF_INTERNAL_HEADER}" secure_token_result_content)
if(NOT context_session_state_content MATCHES "class ContextSessionState final" OR
   NOT context_session_state_content MATCHES "SessionPersistNew" OR
   NOT context_session_state_content MATCHES "SessionRotate" OR
   NOT context_session_state_content MATCHES "SessionClear" OR
   NOT context_session_consumer_content MATCHES
       "ContextSessionState sessionState_" OR
   context_session_consumer_content MATCHES
       "session(Id|Data)_|session(Dirty|Regenerate)_")
    boundary_error("Context session mutation restored parallel nullable/boolean state"
        "Load, persist, rotate, and clear must be exclusive ContextSessionState alternatives")
endif()
if(NOT secure_token_result_content MATCHES "class SecureTokenResult final" OR
   NOT secure_token_result_content MATCHES "class SecureTokenReady final" OR
   NOT secure_token_result_content MATCHES "struct SecureTokenFailure final" OR
   secure_token_result_content MATCHES "generateCsrfToken")
    boundary_error("secure token generation restored an empty-view failure sentinel"
        "CSRF and Session must exhaustively handle SecureTokenReady/SecureTokenFailure")
endif()

set(MULTIPART_INPUT_LIFECYCLE_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/MultipartParser.h")
set(MULTIPART_INPUT_LIFECYCLE_TEST
    "${RUVIA_ROOT}/tests/unit_multipart.cpp")
set(MULTIPART_INPUT_LIFECYCLE_PACKAGE_CONSUMER
    "${RUVIA_ROOT}/tests/package-consumer/http.cpp")
file(READ "${MULTIPART_INPUT_LIFECYCLE_HEADER}"
    multipart_input_lifecycle_content)
file(READ "${MULTIPART_INPUT_LIFECYCLE_TEST}"
    multipart_input_lifecycle_test)
file(READ "${MULTIPART_INPUT_LIFECYCLE_PACKAGE_CONSUMER}"
    multipart_input_lifecycle_package_consumer)
if(NOT multipart_input_lifecycle_content MATCHES
       "class MultipartInputLifecycle final" OR
   NOT multipart_input_lifecycle_content MATCHES
       "MultipartBorrowedInput" OR
   NOT multipart_input_lifecycle_content MATCHES
       "MultipartStreamingInputOpen" OR
   NOT multipart_input_lifecycle_content MATCHES
       "MultipartStreamingInputEof" OR
   NOT multipart_input_lifecycle_content MATCHES
       "borrowed[(][)] const &[ \\t]+noexcept" OR
   NOT multipart_input_lifecycle_content MATCHES
       "borrowed[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_input_lifecycle_content MATCHES
       "streamingOpen[(][)] const &[ \\t]+noexcept" OR
   NOT multipart_input_lifecycle_content MATCHES
       "streamingOpen[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_input_lifecycle_content MATCHES
       "streamingEof[(][)] const &[ \\t]+noexcept" OR
   NOT multipart_input_lifecycle_content MATCHES
       "streamingEof[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_input_lifecycle_content MATCHES
       "view[(][)] const &[ \\t]+noexcept" OR
   NOT multipart_input_lifecycle_content MATCHES
       "view[(][)] const &&[ \\t]*=[ \\t]*delete" OR
   NOT multipart_input_lifecycle_test MATCHES
       "static_assert[(]!ExposesRvalueMultipartInputStorage<" OR
   NOT multipart_input_lifecycle_package_consumer MATCHES
       "static_assert[(]!ExposesRvalueMultipartInputStorage<" OR
   multipart_input_lifecycle_content MATCHES
       "borrowedInputMode_|inputFinished_|borrowedInput_|std::pmr::string buffer_")
    boundary_error("MultipartParser restored parallel input source/EOF state"
        "Borrowed complete input, streaming open, and streaming EOF must be exclusive alternatives whose storage can only be borrowed from stable lvalues")
endif()

set(ROUTER_INTERNAL_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/router/RouterInternal.h")
set(ROUTER_BUILD_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/router/Router.cpp")
set(ROUTER_REGISTRATION_SOURCE
    "${RUVIA_ROOT}/ruvia-web/src/router/RouterRegistration.cpp")
file(READ "${ROUTER_INTERNAL_HEADER}" router_internal_state_content)
file(READ "${ROUTER_BUILD_SOURCE}" router_build_state_content)
file(READ "${ROUTER_REGISTRATION_SOURCE}" router_registration_state_content)
if(router_internal_state_content MATCHES "finalized_" OR
   NOT router_internal_state_content MATCHES "routeTable_" OR
   NOT router_build_state_content MATCHES "if [(]routeTable_[)]" OR
   NOT router_registration_state_content MATCHES "if [(]routeTable_[)]")
    boundary_error("Router restored a parallel finalized flag"
        "Published RouteTable ownership must be the sole finalized-state source")
endif()

set(HTTP1_CLOSING_REJECTION_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http1ClosingRejection.h")
set(HTTP1_STREAM_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamSession.inl")
file(READ "${HTTP1_CLOSING_REJECTION_HEADER}" http1_closing_rejection_content)
file(READ "${HTTP1_STREAM_SESSION}" http1_closing_rejection_consumer)
if(NOT http1_closing_rejection_content MATCHES
       "class Http1ClosingRejection final" OR
   NOT http1_closing_rejection_content MATCHES
       "Http1ClosingRateLimitRejection" OR
   NOT http1_closing_rejection_content MATCHES "std::variant" OR
   NOT http1_closing_rejection_consumer MATCHES
       "Http1ClosingRejection closingRejection" OR
   http1_closing_rejection_consumer MATCHES
       "std::optional<HttpErrorInfo>[ \\t]+closingError" OR
   http1_closing_rejection_consumer MATCHES
       "std::optional<RateLimitRejection>[ \\t]+closingRateLimitRejection")
    boundary_error("HTTP/1 close rejection restored parallel optional state"
        "Ordinary and rate-limit closing errors must be exclusive typed alternatives")
endif()

set(REQUEST_BODY_FACADE_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpRequestBodyFacade.h")
set(REQUEST_BODY_LOADER_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/http/RequestBodyLoader.h")
set(LAZY_BUFFERED_BODY_HEADER
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/body/HttpLazyBufferedBody.h")
set(HTTP1_STREAM_BODY_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerStreamBodyRoute.h")
set(HTTP1_BUFFERED_BODY_ROUTE
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/HttpServerBodyRouteCompletion.h")
set(HTTP2_WEB_SESSION
    "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/detail/server/Http2SansIoSession.h")
file(READ "${REQUEST_BODY_FACADE_HEADER}" request_body_facade_binding_content)
file(READ "${REQUEST_BODY_LOADER_HEADER}" request_body_loader_state_content)
file(READ "${LAZY_BUFFERED_BODY_HEADER}" lazy_buffered_body_state_content)
file(READ "${HTTP1_STREAM_BODY_ROUTE}" http1_stream_body_binding_consumer)
file(READ "${HTTP1_BUFFERED_BODY_ROUTE}" http1_buffered_body_binding_consumer)
file(READ "${HTTP2_WEB_SESSION}" http2_body_binding_consumer)
file(READ "${RUVIA_ROOT}/ruvia-web/include/ruvia/web/Streaming.h"
    request_body_public_facade_content)
file(READ "${RUVIA_ROOT}/ruvia-web/src/http/HttpRuntimeFacades.cpp"
    request_body_public_facade_runtime)
if(NOT request_body_facade_binding_content MATCHES
       "class BodyReaderBinding final" OR
   NOT request_body_facade_binding_content MATCHES
       "class RequestBodyLoaderBinding final" OR
   NOT http1_stream_body_binding_consumer MATCHES
       "optional<BodyReaderBinding<StreamBodyReader<Stream>>>" OR
   http1_stream_body_binding_consumer MATCHES
       "optional<StreamBodyReader<Stream>>" OR
   NOT http1_buffered_body_binding_consumer MATCHES
       "optional<RequestBodyLoaderBinding<LazyBufferedBody<Stream>>>" OR
   http1_buffered_body_binding_consumer MATCHES
       "optional<RequestBodyLoader>" OR
   NOT http2_body_binding_consumer MATCHES
       "optional<BodyReaderBinding<Http2SansIoRequestBodyReader>>" OR
   http2_body_binding_consumer MATCHES
       "optional<Http2SansIoRequestBodyReader>" OR
   NOT request_body_public_facade_content MATCHES
       "bool[ \t]+readActive_" OR
   NOT request_body_public_facade_runtime MATCHES
       "request body read is already in progress")
    boundary_error("Request-body target and facade regained split optional ownership"
        "H1/H2 routes must publish one atomic binding and one consumer may borrow the read buffer at a time")
endif()
if(NOT request_body_loader_state_content MATCHES "kReading" OR
   NOT request_body_loader_state_content MATCHES "kDiscarding" OR
   NOT request_body_loader_state_content MATCHES "kDiscarded" OR
   NOT request_body_loader_state_content MATCHES "kFailed" OR
   NOT request_body_loader_state_content MATCHES "class OperationGuard final" OR
   NOT request_body_loader_state_content MATCHES "state_ = State::kFailed" OR
   lazy_buffered_body_state_content MATCHES "bool[ 	]+read_|bodyView_" OR
   multipart_web_api MATCHES "bodyEnded_")
    boundary_error("request-body consumers restored parallel or retryable consumption state"
        "lazy buffering, discard, and multipart reads must have one fail-fast lifecycle authority")
endif()

set(HTTP_CONTENT_CODING_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/HttpContentCoding.h")
set(WEBSOCKET_INBOUND_HEADER
    "${RUVIA_ROOT}/ruvia-http/include/ruvia/http/detail/websocket/HttpWebSocketUtils.h")
file(READ "${HTTP_CONTENT_CODING_HEADER}" http_content_coding_state_content)
file(READ "${WEBSOCKET_INBOUND_HEADER}" websocket_inbound_state_content)
if(NOT http_content_coding_state_content MATCHES
       "HttpInvalidContentCodingField> state_" OR
   http_content_coding_state_content MATCHES "codingCount_|unsupported_")
    boundary_error("Content-Encoding field parsing restored parallel terminal state"
        "supported accumulation, unsupported capability, and malformed syntax must remain exclusive")
endif()
if(NOT websocket_inbound_state_content MATCHES
       "class WebSocketInboundFragmented final" OR
   NOT websocket_inbound_state_content MATCHES
       "std::variant<WebSocketInboundIdle, WebSocketInboundFragmented> state_" OR
   websocket_inbound_state_content MATCHES "fragmented_")
    boundary_error("WebSocket inbound assembly restored parallel fragment flags"
        "Idle and fragmented opcode/content-encoding must be one exclusive state")
endif()

get_property(boundary_failed GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED)
if(boundary_failed)
    message(FATAL_ERROR "Ruvia layer-boundary checks failed")
endif()
message(STATUS "layer boundaries OK")
