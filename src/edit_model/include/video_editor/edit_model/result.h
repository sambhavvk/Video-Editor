// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <stdexcept>
#include <utility>
#include <variant>

namespace video_editor::edit {

template <typename T, typename E>
class Result {
 public:
  [[nodiscard]] static Result success(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  [[nodiscard]] static Result failure(E error) {
    return Result(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool hasValue() const noexcept { return value_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

  [[nodiscard]] const T& value() const& {
    if (!hasValue()) {
      throw std::logic_error("Result does not contain a value");
    }
    return std::get<0>(value_);
  }

  [[nodiscard]] T& value() & {
    if (!hasValue()) {
      throw std::logic_error("Result does not contain a value");
    }
    return std::get<0>(value_);
  }

  [[nodiscard]] T&& value() && {
    if (!hasValue()) {
      throw std::logic_error("Result does not contain a value");
    }
    return std::get<0>(std::move(value_));
  }

  [[nodiscard]] const E& error() const& {
    if (hasValue()) {
      throw std::logic_error("Result does not contain an error");
    }
    return std::get<1>(value_);
  }

  [[nodiscard]] E& error() & {
    if (hasValue()) {
      throw std::logic_error("Result does not contain an error");
    }
    return std::get<1>(value_);
  }

 private:
  template <std::size_t Index, typename U>
  explicit Result(std::in_place_index_t<Index> index, U&& value)
      : value_(index, std::forward<U>(value)) {}

  std::variant<T, E> value_;
};

}  // namespace video_editor::edit
