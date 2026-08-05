#include "bindings.h"

PYBIND11_MODULE(_csplendor, module) {
  csplendor::python::bind_domain(module);
  csplendor::python::bind_rules(module);
  csplendor::python::bind_encoding(module);
  csplendor::python::bind_mcts(module);
  csplendor::python::bind_solvers(module);
}
