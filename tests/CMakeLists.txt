function(csplendor_add_native_test target source labels timeout)
  add_executable(${target} ${source})
  target_link_libraries(
      ${target} PRIVATE csplendor_core csplendor_sanitizer csplendor_warnings
                        Threads::Threads)
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
csplendor_add_native_test(mcts_optimization_unit mcts_optimization_unit.cpp
                          "unit;optimization;equivalence" 90)
target_compile_definitions(
  mcts_optimization_unit
  PRIVATE CSPLENDOR_MCTS_TESTING=1 CSPLENDOR_ACTION_ENCODER_REFERENCE=1)
csplendor_add_native_test(game_snapshot_unit game_snapshot_unit.cpp
                          "unit;snapshot;serialization;standalone-header" 30)
csplendor_add_native_test(state_invariants_unit state_invariants_unit.cpp
                          "unit;state;invariants;hash" 30)
csplendor_add_native_test(domain_state_unit domain_state_unit.cpp
                          "unit;state;editor;mutation;fixed-capacity" 30)
