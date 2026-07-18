function(csplendor_add_native_test target source labels timeout)
  add_executable(${target} ${source})
  target_link_libraries(
      ${target} PRIVATE csplendor_core csplendor_sanitizer Threads::Threads)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES LABELS "${labels}"
                                           TIMEOUT "${timeout}")
  if(CSPLENDOR_SANITIZER STREQUAL "thread")
    set_property(TEST ${target} APPEND PROPERTY LABELS tsan)
  endif()
endfunction()

csplendor_add_native_test(mcts_parallel_unit mcts_parallel_unit.cpp
                          "unit;replay" 30)
target_compile_definitions(mcts_parallel_unit PRIVATE CSPLENDOR_MCTS_TESTING=1)
csplendor_add_native_test(mcts_parallel_stress mcts_parallel_stress.cpp
                          "stress" 60)
csplendor_add_native_test(mcts_parallel_scheduler mcts_parallel_scheduler.cpp
                          "unit;stress;scheduler" 60)
csplendor_add_native_test(mcts_parallel_replay mcts_parallel_replay.cpp
                          "unit;replay;deterministic;root-parallel" 90)
