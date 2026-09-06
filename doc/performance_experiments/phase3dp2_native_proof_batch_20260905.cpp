// Supplemental aggregation of the UNCHANGED native benchmark's solve timers.
// Fresh solver and existing warmup/order/digest checks on each repetition.
// This adapter is not a production engine patch or a new statistics framework.
#define main original_engine_benchmark_main
#include CSPLENDOR_EXISTING_BENCHMARK
#undef main

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto fixture = make_fixture(arguments);
    auto combined = run_exact_reveal(arguments, fixture);
    const auto counters = combined.counters.render();
    const auto semantics = combined.semantics.render();
    for (int repeat = 1; repeat < 2000; ++repeat) {
      auto sample = run_exact_reveal(arguments, fixture);
      if (sample.digest != combined.digest || sample.counters.render() != counters ||
          sample.semantics.render() != semantics)
        throw std::runtime_error("native proof batch changed deterministic result");
      combined.operations += sample.operations;
      combined.elapsed_ns += sample.elapsed_ns;
    }
    combined.semantics.integer("batch_repetitions", 2000);
    combined.semantics.string("batch_scope", "sum of existing native solve timers; counters per call");
    emit_result("native_proof_batch", arguments, fixture, std::move(combined));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
