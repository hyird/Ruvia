#include "server/acceptor.h"

#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <sys/socket.h>
#endif

#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>
#include <asio/socket_base.hpp>

#if defined(_WIN32)
#include <asio/windows/overlapped_ptr.hpp>
#endif

#include "ruvia/core/memory/process_resource.h"

namespace ruvia::detail {
namespace {

// Errors that concern only the connection being accepted or a transient
// resource shortage. accept(2) reports network errors already pending on the
// new connection and requires callers to treat them like EAGAIN; none of these
// invalidate the listener, so accepting resumes after a short backoff.
bool recoverable_accept_error(const asio::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset ||
        error == asio::error::eof || error == asio::error::try_again || error == asio::error::would_block ||
        error == asio::error::interrupted || error == asio::error::no_descriptors ||
        error == asio::error::no_buffer_space || error == asio::error::no_memory ||
        error == std::errc::too_many_files_open_in_system || error == std::errc::protocol_error ||
        error == asio::error::network_down || error == asio::error::network_unreachable ||
        error == asio::error::host_unreachable || error == asio::error::no_protocol_option ||
        error == asio::error::operation_not_supported || error == asio::error::no_permission ||
        error == asio::error::timed_out) {
        return true;
    }
#if defined(_WIN32)
    // AcceptEx can report a peer abort as an IOCP system error rather than
    // WSAECONNABORTED. The listening socket is still usable.
    if (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED) {
        return true;
    }
#endif
#if defined(EHOSTDOWN) && defined(ENONET)
    return error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET);
#else
    return false;
#endif
}

// Bind the listener exclusively. Windows SO_REUSEADDR (asio's reuse_address)
// lets another socket bind a port that is already listening, so Windows uses
// SO_EXCLUSIVEADDRUSE; elsewhere SO_REUSEADDR only permits rebinding over
// TIME_WAIT connections.
void set_listener_address_option(asio::ip::tcp::acceptor& socket, asio::error_code& error) noexcept {
#if defined(_WIN32)
    const BOOL exclusive = TRUE;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
        error = asio::error_code(::WSAGetLastError(), asio::system_category());
    }
#else
    socket.set_option(asio::socket_base::reuse_address(true), error);
#endif
}

}  // namespace

acceptor::acceptor(std::span<const http_server_listener_definition> listeners,
    std::span<const worker_target> targets, void* failure_target, failure_callback failure)
    : runtime_({.queue_capacity_ = 128, .io_policy_ = worker_io_policy::single_owner}),
      io_context_(runtime_.context().io_context()),
      listeners_(process_resource()),
      targets_(targets.begin(), targets.end(), process_resource()),
      failure_target_(failure_target),
      failure_callback_(failure) {
    for (const auto& target : targets_) {
        if (!target.submission_.valid()) {
            throw std::invalid_argument("acceptor worker target is incomplete");
        }
    }
    listeners_.reserve(listeners.size());
    for (const auto& configured : listeners) {
        listeners_.push_back(make_pmr_object<listener>(process_resource(), io_context_,
            configured.endpoint_));
    }
    runtime_.configure({
        .startup_ = [this] { runtime_.mark_ready(); },
        .serve_ = [this] {
            for (std::size_t i = 0; i < listeners_.size(); ++i) {
                begin_accept(i);
            } },
        .stop_admission_ = [this] { stop_on_owner(); },
        .failure_ = [this](std::exception_ptr) noexcept { failure_callback_(failure_target_); },
        .shutdown_ = [this]() noexcept { close_listeners(); },
    });
}

acceptor::~acceptor() {
    // Ingress has no external producers and finalizes after closing listeners.
    runtime_.request_stop();
    runtime_.join();
}

void acceptor::prepare() {
    if (runtime_.state() != runtime_lifecycle::state_type::ready) {
        throw std::logic_error("acceptor listeners must be prepared before launch");
    }
    for (auto& configured : listeners_) {
        auto& socket = configured->socket_;
        asio::error_code error;
        socket.open(configured->endpoint_.protocol(), error);
        if (!error) {
            set_listener_address_option(socket, error);
        }
        if (!error) {
            socket.bind(configured->endpoint_, error);
        }
        if (!error) {
            socket.listen(asio::socket_base::max_listen_connections, error);
        }
#if defined(__linux__)
        if (!error) {
            socket.native_non_blocking(true, error);
        }
#elif defined(_WIN32)
        if (!error) {
            GUID extension = WSAID_ACCEPTEX;
            DWORD received = 0;
            if (::WSAIoctl(socket.native_handle(), SIO_GET_EXTENSION_FUNCTION_POINTER,
                    &extension, sizeof(extension), &configured->accept_function_,
                    sizeof(configured->accept_function_), &received, nullptr, nullptr) != 0) {
                error = asio::error_code(::WSAGetLastError(), asio::system_category());
            }
        }
#endif
        if (!error) {
            configured->endpoint_ = socket.local_endpoint(error);
        }
        if (error) {
            close_listeners();
            throw std::system_error(error, "failed to prepare TCP acceptor listener");
        }
    }
}

void acceptor::close_admission() noexcept {
    // A refused control means stopping has already closed the listeners.
    (void)runtime_.post_control([this] {
        admitting_ = false;
        close_listeners();
    });
}

bool acceptor::admitting() const noexcept {
    return admitting_ && runtime_.state() == runtime_lifecycle::state_type::running;
}

void acceptor::stop_on_owner() noexcept {
    admitting_ = false;
    close_listeners();
    runtime_.finalize();
}

void acceptor::begin_accept(std::size_t index) noexcept {
    if (!admitting() || targets_.empty()) {
        return;
    }
    auto& socket = listeners_[index]->socket_;
    if (!socket.is_open()) {
        return;
    }
    try {
#if defined(__linux__)
        // Accept native descriptors before their first reactor registration.
        // One bounded turn offers a connection per worker, then yields so stop
        // and other listeners cannot starve under a continuously full backlog.
        for (std::size_t count = 0; count < targets_.size(); ++count) {
            const auto native = ::accept4(socket.native_handle(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (native >= 0) {
                if (!hand_off(index, native_accepted_socket_ticket(listeners_[index]->endpoint_.protocol(), index, native))) {
                    return;
                }
                continue;
            }
            const asio::error_code error(errno, asio::system_category());
            if (error == asio::error::interrupted) {
                continue;
            }
            if (error == asio::error::would_block || error == asio::error::try_again) {
                socket.async_wait(asio::ip::tcp::acceptor::wait_read, [this, index](const asio::error_code& ready_error) {
                    if (ready_error) {
                        accept_failed(index, ready_error);
                    } else {
                        begin_accept(index);
                    }
                });
                return;
            }
            accept_failed(index, error);
            return;
        }
        asio::post(io_context_, [this, index] { begin_accept(index); });
#elif defined(_WIN32)
        const auto protocol = listeners_[index]->endpoint_.protocol();
        const SOCKET native = ::WSASocketW(protocol.family(), protocol.type(), protocol.protocol(),
            nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (native == INVALID_SOCKET) {
            accept_failed(index, asio::error_code(::WSAGetLastError(), asio::system_category()));
            return;
        }
        native_accepted_socket_ticket ticket(protocol, index, native);
        // Only the listener belongs to ingress's IOCP. The new socket remains
        // unregistered until its worker adopts it, avoiding Asio release()'s
        // cancellation and completion-port detachment after every accept.
        asio::windows::overlapped_ptr operation(io_context_,
            asio::bind_allocator(std::pmr::polymorphic_allocator<std::byte>(process_resource()),
                [this, index, ticket = std::move(ticket)](const asio::error_code& error, std::size_t) mutable noexcept {
                    if (error) {
                        accept_failed(index, error);
                        return;
                    }
                    if (!admitting()) {
                        return;
                    }
                    const SOCKET listener_socket = listeners_[index]->socket_.native_handle();
                    if (::setsockopt(ticket.native_handle(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                            reinterpret_cast<const char*>(&listener_socket), sizeof(listener_socket)) != 0) {
                        accept_failed(index, asio::error_code(::WSAGetLastError(), asio::system_category()));
                        return;
                    }
                    if (hand_off(index, std::move(ticket))) {
                        begin_accept(index);
                    }
                }));
        constexpr DWORD address_size = sizeof(sockaddr_storage) + 16;
        DWORD received = 0;
        const auto accepted = listeners_[index]->accept_function_(socket.native_handle(), native,
            listeners_[index]->accept_addresses_.data(), 0, address_size, address_size, &received, operation.get());
        const auto error = accepted ? 0 : ::WSAGetLastError();
        if (accepted || error == WSA_IO_PENDING) {
            static_cast<void>(operation.release());
        } else {
            operation.complete(asio::error_code(error, asio::system_category()), 0);
        }
#else
        socket.async_accept([this, index](const asio::error_code& error, asio::ip::tcp::socket accepted_socket) {
            if (error) {
                accept_failed(index, error);
                return;
            }
            asio::error_code release_error;
            const auto native = accepted_socket.release(release_error);
            if (release_error) {
                try {
                    throw std::system_error(release_error, "failed to detach accepted TCP socket");
                } catch (...) {
                    (void)runtime_.fail(std::current_exception());
                }
                return;
            }
            if (hand_off(index, native_accepted_socket_ticket(listeners_[index]->endpoint_.protocol(), index, native))) {
                begin_accept(index);
            }
        });
#endif
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

void acceptor::accept_failed(std::size_t index, const asio::error_code& error) noexcept {
    if (error == asio::error::operation_aborted && !admitting()) {
        return;
    }
    if (!recoverable_accept_error(error)) {
        try {
            throw std::system_error(error, "fatal TCP accept error");
        } catch (...) {
            (void)runtime_.fail(std::current_exception());
        }
        return;
    }
    schedule_retry(index);
}

bool acceptor::hand_off(std::size_t index, native_accepted_socket_ticket ticket) noexcept {
    if (!admitting() || targets_.empty()) {
        return false;
    }
    auto selected = targets_.size();
    for (std::size_t offset = 0; offset < targets_.size(); ++offset) {
        const auto worker_value = (next_target_ + offset) % targets_.size();
        if (targets_[worker_value].available_(targets_[worker_value].object_) && targets_[worker_value].submission_.accepting()) {
            selected = worker_value;
            next_target_ = (worker_value + 1) % targets_.size();
            break;
        }
    }
    if (selected == targets_.size()) {
        // Dropping the ticket closes the connection at ingress.
        refused_connections_.fetch_add(1, std::memory_order_relaxed);
        schedule_retry(index);
        return false;
    }
    const auto target = targets_[selected];
    try {
        // Selection above is advisory. The worker's accept path holds the one
        // authoritative capacity check and counts its own refusals.
        auto posted = target.submission_.post([target, ticket = std::move(ticket)]() mutable noexcept {
            target.accept_(target.object_, std::move(ticket));
        });
        if (!posted.accepted()) {
            refused_connections_.fetch_add(1, std::memory_order_relaxed);
            auto rejected = std::move(posted).take_rejected();
            rejected = {};
        }
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
        return false;
    }
    return admitting();
}

void acceptor::schedule_retry(std::size_t index) noexcept {
    if (!admitting()) {
        return;
    }
    try {
        auto& timer = listeners_[index]->retry_;
        timer.expires_after(std::chrono::milliseconds(25));
        timer.async_wait([this, index](const asio::error_code& error) {
            if (!error) {
                begin_accept(index);
            }
        });
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

void acceptor::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        asio::error_code ignored;
        configured->retry_.cancel();
        ignored.clear();
        configured->socket_.cancel(ignored);
        ignored.clear();
        configured->socket_.close(ignored);
    }
}

}  // namespace ruvia::detail
