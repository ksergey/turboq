// Copyright (c) Sergey Kovalevich <inndie@gmail.com>
// SPDX-License-Identifier: AGPL-3.0

#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <variant>

namespace turboq::detail {

struct BadExpectedAccess : public std::runtime_error {
  template <typename... Args>
  BadExpectedAccess(Args&&... args) : std::runtime_error(std::forward<Args>(args)...) {}
};

/// @brief Required tagging type for cases where expected and unexpected type are identical.
template <typename E>
class Unexpected {
public:
  constexpr Unexpected(Unexpected const&) = default;
  constexpr Unexpected& operator=(Unexpected const&) = default;
  constexpr Unexpected(Unexpected&&) = default;
  constexpr Unexpected& operator=(Unexpected&&) = default;

  constexpr explicit Unexpected(E const& rhs) : storage_(rhs) {}
  constexpr explicit Unexpected(E&& rhs) : storage_(std::move(rhs)) {}

  constexpr E const& error() const& noexcept {
    return storage_;
  }
  constexpr E&& error() && noexcept {
    return std::move(storage_);
  }

  friend constexpr bool operator==(Unexpected const&, Unexpected const&) = default;
  friend constexpr auto operator<=>(Unexpected const&, Unexpected const&) = default;

private:
  E storage_;
};

template <typename E>
Unexpected(E) -> Unexpected<E>;

/// @brief A stripped-down version of std::expected from C++23.
template <typename T, typename E>
class Expected {
private:
  std::variant<T, Unexpected<E>> storage_;

public:
  constexpr Expected() : storage_(std::in_place_index<0>) {}

  template <typename... Args>
  constexpr Expected(Args&&... args) : storage_(std::forward<Args>(args)...) {}

  constexpr Expected(Expected const&) = default;
  constexpr Expected& operator=(Expected const&) = default;
  constexpr Expected(Expected&&) = default;
  constexpr Expected& operator=(Expected&&) = default;

  constexpr Expected& operator=(T&& value) {
    storage_.template emplace<0>(std::move(value));
    return *this;
  }

  constexpr Expected& operator=(T const& value) {
    storage_.template emplace<0>(value);
    return *this;
  }

  constexpr Expected& operator=(Unexpected<E>&& value) {
    storage_.template emplace<1>(std::move(value));
    return *this;
  }

  constexpr Expected& operator=(Unexpected<E> const& value) {
    storage_.template emplace<1>(value);
    return *this;
  }

  template <typename U = T, typename G = E>
    requires(!std::is_same_v<Expected<U, G>, Expected> && std::is_convertible_v<U, T> && std::is_convertible_v<G, E>)
  constexpr Expected& operator=(Expected<U, G> value) {
    if (value.has_value()) {
      storage_.template emplace<0>(std::move(static_cast<Expected<U, G>&&>(value).value()));
    } else {
      storage_.template emplace<1>(std::move(static_cast<Expected<U, G>&&>(value).error()));
    }
    return *this;
  }

  constexpr bool has_value() const noexcept {
    return storage_.index() == 0;
  }

  constexpr explicit operator bool() const noexcept {
    return has_value();
  }

  template <class X>
  constexpr T value_or(X&& v) const& noexcept {
    return has_value() ? std::get<T>(storage_) : static_cast<T>(std::forward<X>(v));
  }

  constexpr T const& value() const& {
    if (!has_value())
      throw BadExpectedAccess("Attempt to access value() when unexpected");
    return std::get<T>(storage_);
  }

  constexpr T value() && {
    if (!has_value())
      throw BadExpectedAccess("Attempt to access value() when unexpected");
    return std::move(std::get<T>(storage_));
  }

  constexpr E const& error() const& noexcept {
    return std::get_if<Unexpected<E>>(&storage_)->error();
  }

  constexpr E error() && noexcept {
    return std::move(*std::get_if<Unexpected<E>>(&storage_)).error();
  }

  constexpr T const& operator*() const noexcept {
    return *std::get_if<T>(&storage_);
  }

  constexpr T const* operator->() const noexcept {
    return std::get_if<T>(&storage_);
  }
};

/// \overload
template <typename E>
class Expected<void, E> {
private:
  std::variant<std::monostate, Unexpected<E>> storage_;

public:
  constexpr Expected() noexcept : storage_(std::in_place_index<0>) {}

  template <typename... Args>
  constexpr Expected(Args&&... args) : storage_(std::forward<Args>(args)...) {}

  constexpr Expected(Expected const&) = default;
  constexpr Expected& operator=(Expected const&) = default;
  constexpr Expected(Expected&&) = default;
  constexpr Expected& operator=(Expected&&) = default;

  template <typename G = E>
    requires std::is_convertible_v<G, E>
  constexpr Expected& operator=(Expected<void, G> value) {
    if (!value.has_value()) {
      storage_.template emplace<1>(std::move(static_cast<Expected<void, G>&&>(value).error()));
    } else {
      storage_.template emplace<0>();
    }
    return *this;
  }

  constexpr bool has_value() const noexcept {
    return storage_.index() == 0;
  }

  constexpr explicit operator bool() const noexcept {
    return has_value();
  }

  constexpr E const& error() const& noexcept {
    return std::get_if<Unexpected<E>>(&storage_)->error();
  }

  constexpr E error() && noexcept {
    return std::move(*std::get_if<Unexpected<E>>(&storage_)).error();
  }

  constexpr Expected& operator=(Unexpected<E>&& value) {
    storage_.template emplace<1>(std::move(value));
    return *this;
  }

  constexpr Expected& operator=(Unexpected<E> const& value) {
    storage_.template emplace<1>(value);
    return *this;
  }
};

} // namespace turboq::detail
