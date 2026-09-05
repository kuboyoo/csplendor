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

set(CSPLENDOR_STANDALONE_HEADERS
    action.h
    action_encoder.h
    action_encoder_v2.h
    action_encoder_v3.h
    board.h
    card_data.h
    game.h
    game_snapshot.h
    mcts.h
    move_generator.h
    noble_data.h
    player.h
    solver_types.h
    state_encoder.h
    types.h
    visible_only_solver.h)

foreach(header IN LISTS CSPLENDOR_STANDALONE_HEADERS)
  string(REPLACE ".h" "" header_stem "${header}")
  set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${header_stem}.cpp")
  file(GENERATE OUTPUT "${source}"
       CONTENT "#include \"${header}\"\nint main() { return 0; }\n")
  csplendor_add_native_test(
    "header_${header_stem}" "${source}" "header;standalone;api" 10)
endforeach()

# Internal helpers get standalone checks without expanding the public API
# header matrix (which is versioned in doc/refactoring_contracts.json).
set(reveal_order_header_source
    "${CMAKE_CURRENT_BINARY_DIR}/header_solver_reveal_order.cpp")
file(GENERATE OUTPUT "${reveal_order_header_source}"
     CONTENT "#include \"solver_reveal_order.h\"\nint main() { return 0; }\n")
csplendor_add_native_test(header_solver_reveal_order "${reveal_order_header_source}"
                          "header;standalone;internal" 10)
set(scratch_header_source "${CMAKE_CURRENT_BINARY_DIR}/header_solver_search_scratch.cpp")
file(GENERATE OUTPUT "${scratch_header_source}"
     CONTENT "#include \"solver_search_scratch.h\"\nint main() { return 0; }\n")
csplendor_add_native_test(header_solver_search_scratch "${scratch_header_source}"
                          "header;standalone;internal" 10)

csplendor_add_native_test(mcts_parallel_unit mcts_parallel_unit.cpp
                          "unit;replay" 30)
target_compile_definitions(mcts_parallel_unit PRIVATE CSPLENDOR_MCTS_TESTING=1)
csplendor_add_native_test(mcts_parallel_stress mcts_parallel_stress.cpp
                          "stress" 60)
add_library(mcts_parallel_scheduler_suites OBJECT mcts_parallel_scheduler.cpp)
target_link_libraries(
  mcts_parallel_scheduler_suites
  PRIVATE csplendor_core_config csplendor_warnings Threads::Threads)

function(csplendor_add_scheduler_suite target source labels timeout)
  add_executable(
    ${target} ${source} $<TARGET_OBJECTS:mcts_parallel_scheduler_suites>)
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

csplendor_add_scheduler_suite(
  mcts_scheduler_lifecycle mcts_scheduler_lifecycle_main.cpp
  "unit;scheduler;lifecycle" 60)
csplendor_add_scheduler_suite(
  mcts_scheduler_limits mcts_scheduler_limits_main.cpp
  "unit;scheduler;limits;failure-injection;stress" 90)
csplendor_add_scheduler_suite(
  mcts_scheduler_hidden mcts_scheduler_hidden_main.cpp
  "unit;scheduler;determinization;hidden-information" 120)
csplendor_add_native_test(mcts_parallel_replay mcts_parallel_replay.cpp
                          "unit;replay;deterministic;root-parallel" 90)
csplendor_add_native_test(mcts_optimization_unit mcts_optimization_unit.cpp
                          "unit;optimization;equivalence" 90)
target_compile_definitions(
  mcts_optimization_unit
  PRIVATE CSPLENDOR_MCTS_TESTING=1 CSPLENDOR_ACTION_ENCODER_REFERENCE=1)
csplendor_add_native_test(mcts_ownership_unit mcts_ownership_unit.cpp
                          "unit;mcts;ownership;lifecycle;configuration" 30)
csplendor_add_native_test(game_snapshot_unit game_snapshot_unit.cpp
                          "unit;snapshot;serialization;standalone-header" 30)
csplendor_add_native_test(state_invariants_unit state_invariants_unit.cpp
                          "unit;state;invariants;hash" 30)
csplendor_add_native_test(domain_state_unit domain_state_unit.cpp
                          "unit;state;editor;mutation;fixed-capacity" 30)
csplendor_add_native_test(incremental_hash_unit incremental_hash_unit.cpp
                          "unit;state;hash;incremental;differential" 120)
csplendor_add_native_test(state_copy_unit state_copy_unit.cpp
                          "unit;state;copy;snapshot;undo" 30)
csplendor_add_native_test(perf_counters_unit perf_counters_unit.cpp
                          "unit;performance;instrumentation" 30)
csplendor_add_native_test(rule_query_unit rule_query_unit.cpp
                          "unit;rules;query;validation;transition;equivalence" 90)
csplendor_add_native_test(encoding_schema_unit encoding_schema_unit.cpp
                          "unit;encoding;schema;equivalence;golden" 90)
csplendor_add_native_test(solver_components_unit solver_components_unit.cpp
                          "unit;solver;ownership;limits;proof;standalone-header" 30)
