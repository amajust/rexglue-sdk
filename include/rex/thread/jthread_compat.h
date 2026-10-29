/**
 ******************************************************************************
 * ReXGlue runtime (Android port): std::jthread / std::stop_token shim.
 ******************************************************************************
 * Android NDK ships libc++ from LLVM < 20, which does not provide
 * std::jthread / std::stop_token (P0660/P2140, landed in libc++ 20). The
 * only consumer in the runtime is TimerQueue, which polls
 * stop_requested() from its dispatch thread and relies on the auto-join
 * destructor; no stop_callback registrations exist. This shim implements
 * exactly that subset on top of a shared atomic flag:
 *   - stop_token::stop_requested()
 *   - jthread: token-taking or argument-less callables, request_stop(),
 *     join/joinable/get_id, auto-join on destruction
 * On every other toolchain (libstdc++, libc++ >= 20, Apple, MSVC STL)
 * these are plain aliases of the standard types, so behavior is unchanged.
 */

#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000 && !defined(__APPLE__)

namespace rex::thread {

class stop_token {
 public:
  stop_token() noexcept = default;

  bool stop_requested() const noexcept {
    return state_ && state_->load(std::memory_order_acquire);
  }

 private:
  friend class jthread;
  explicit stop_token(std::shared_ptr<std::atomic_bool> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<std::atomic_bool> state_;
};

class jthread {
 public:
  using id = std::thread::id;
  using native_handle_type = std::thread::native_handle_type;

  jthread() noexcept = default;

  template <typename Callable,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<Callable>, jthread>>>
  explicit jthread(Callable&& callable) {
    token_ = stop_token(std::make_shared<std::atomic_bool>(false));
    thread_ = std::thread(
        [callable = std::forward<Callable>(callable), token = token_]() mutable {
          if constexpr (std::is_invocable_v<Callable&, stop_token>) {
            callable(std::move(token));
          } else {
            callable();
          }
        });
  }

  ~jthread() {
    if (joinable()) {
      request_stop();
      join();
    }
  }

  jthread(const jthread&) = delete;
  jthread& operator=(const jthread&) = delete;

  jthread(jthread&& other) noexcept
      : thread_(std::move(other.thread_)), token_(std::move(other.token_)) {
    other.token_ = stop_token();
  }

  jthread& operator=(jthread&& other) noexcept {
    if (this != &other) {
      if (joinable()) {
        request_stop();
        join();
      }
      thread_ = std::move(other.thread_);
      token_ = std::move(other.token_);
      other.token_ = stop_token();
    }
    return *this;
  }

  bool joinable() const noexcept { return thread_.joinable(); }
  void join() { thread_.join(); }
  void detach() { thread_.detach(); }
  id get_id() const noexcept { return thread_.get_id(); }
  native_handle_type native_handle() { return thread_.native_handle(); }

  bool request_stop() noexcept {
    if (!token_.state_) {
      return false;
    }
    return token_.state_->exchange(true, std::memory_order_acq_rel) == false;
  }

  const stop_token& get_stop_token() const noexcept { return token_; }

 private:
  std::thread thread_;
  stop_token token_;
};

}  // namespace rex::thread

#else  // Standard library provides jthread natively.

#include <stop_token>

namespace rex::thread {

using jthread = std::jthread;
using stop_token = std::stop_token;

}  // namespace rex::thread

#endif
