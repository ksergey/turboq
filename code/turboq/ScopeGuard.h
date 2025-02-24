// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <type_traits>
#include <utility>

namespace turboq {

template <typename Fn>
class [[nodiscard]] ScopeGuard final {
private:
  static_assert(std::is_nothrow_move_constructible_v<Fn>);

  [[no_unique_address]] Fn fn_;
  bool released_ = false;

public:
  ScopeGuard(Fn&& fn) noexcept : fn_((Fn&&)fn) {}

  ~ScopeGuard() noexcept {
    reset();
  }

  void release() noexcept {
    released_ = true;
  }

  void reset() noexcept {
    static_assert(noexcept(((Fn&&)fn_)()));
    if (!std::exchange(released_, true)) {
      ((Fn&&)fn_)();
    }
  }
};

template <typename Fn>
ScopeGuard(Fn&&) -> ScopeGuard<Fn>;

} // namespace turboq
