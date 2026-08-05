#pragma once

#include <exception>
#include <iostream>

namespace csplendor::test {

template <typename Suite> int run_suite(const char *name, Suite &&suite) {
  try {
    suite();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << name << ": " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << name << ": unknown exception\n";
    return 1;
  }
}

} // namespace csplendor::test
