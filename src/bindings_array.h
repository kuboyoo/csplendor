#ifndef CSPLENDOR_BINDINGS_ARRAY_H
#define CSPLENDOR_BINDINGS_ARRAY_H

#include <pybind11/numpy.h>
#include <array>
#include <cstddef>
#include <cstring>

namespace csplendor::python::detail {

template <typename T, std::size_t N>
pybind11::array_t<T> owning_array_copy(const std::array<T, N> &source) {
  pybind11::array_t<T> result(N);
  std::memcpy(result.mutable_data(), source.data(), N * sizeof(T));
  return result;
}

} // namespace csplendor::python::detail

#endif // CSPLENDOR_BINDINGS_ARRAY_H
