#pragma once
// std::jthread / std::stop_token are missing from the Android NDK's libc++
// (they sit behind its experimental flag). Use the standard ones when the
// library has them, otherwise a minimal stand-in with the same shape:
// cooperative stop flag, request_stop(), join-on-destruction. It has no
// stop_callback, so callers must wake their own threads (both users do).

#include <version>
#include <thread>

#if defined(__cpp_lib_jthread) && !defined(REX_FORCE_JTHREAD_COMPAT)

namespace rex::compat {
using std::jthread;
using std::stop_token;
}  // namespace rex::compat

#else

#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace rex::compat {

class stop_token {
 public:
  stop_token() = default;
  explicit stop_token(std::shared_ptr<std::atomic<bool>> state) : state_(std::move(state)) {}
  bool stop_requested() const noexcept {
    return state_ && state_->load(std::memory_order_acquire);
  }
  bool stop_possible() const noexcept { return static_cast<bool>(state_); }

 private:
  std::shared_ptr<std::atomic<bool>> state_;
};

class jthread {
 public:
  using id = std::thread::id;

  jthread() noexcept = default;

  template <typename F, typename... Args,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, jthread>>>
  explicit jthread(F&& f, Args&&... args)
      : state_(std::make_shared<std::atomic<bool>>(false)) {
    if constexpr (std::is_invocable_v<std::decay_t<F>, stop_token, std::decay_t<Args>...>) {
      thread_ = std::thread(std::forward<F>(f), stop_token(state_), std::forward<Args>(args)...);
    } else {
      thread_ = std::thread(std::forward<F>(f), std::forward<Args>(args)...);
    }
  }

  jthread(jthread&&) noexcept = default;
  jthread& operator=(jthread&& other) noexcept {
    if (this != &other) {
      stop_and_join();
      thread_ = std::move(other.thread_);
      state_ = std::move(other.state_);
    }
    return *this;
  }
  jthread(const jthread&) = delete;
  jthread& operator=(const jthread&) = delete;

  ~jthread() { stop_and_join(); }

  bool request_stop() noexcept {
    if (!state_) return false;
    return !state_->exchange(true, std::memory_order_acq_rel);
  }
  stop_token get_stop_token() const noexcept { return stop_token(state_); }

  bool joinable() const noexcept { return thread_.joinable(); }
  void join() { thread_.join(); }
  void detach() { thread_.detach(); }
  id get_id() const noexcept { return thread_.get_id(); }

 private:
  void stop_and_join() {
    if (thread_.joinable()) {
      request_stop();
      thread_.join();
    }
  }

  std::thread thread_;
  std::shared_ptr<std::atomic<bool>> state_;
};

}  // namespace rex::compat

#endif
