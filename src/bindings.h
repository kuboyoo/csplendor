#ifndef CSPLENDOR_BINDINGS_H
#define CSPLENDOR_BINDINGS_H

#include <pybind11/pybind11.h>

namespace csplendor::python {

void bind_domain(pybind11::module_ &module);
void bind_rules(pybind11::module_ &module);
void bind_encoding(pybind11::module_ &module);
void bind_mcts(pybind11::module_ &module);
void bind_solvers(pybind11::module_ &module);

} // namespace csplendor::python

#endif // CSPLENDOR_BINDINGS_H
