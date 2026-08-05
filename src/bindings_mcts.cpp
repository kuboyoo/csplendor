#include "bindings.h"
#include "bindings_array.h"
#include "action.h"
#include "action_encoder.h"
#include "game.h"
#include "mcts.h"
#include "mcts_parallel_searcher.h"
#include "mcts_root_parallel.h"
#include "mcts_searcher.h"
#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using csplendor::python::detail::owning_array_copy;

namespace py = pybind11;

namespace {

mcts_parallel::ParallelInferenceFunction
make_python_parallel_inference(py::function &inference_fn) {
  return [&inference_fn](
             const std::vector<mcts_parallel::ParallelInferenceRequest>
                 &requests) {
    // Root-parallel serialization is owned by its native runner. It locks
    // before acquiring the GIL and can therefore re-check timeout/cancel after
    // waiting instead of draining stale Python callback work.
    py::gil_scoped_acquire acquire;
    py::list python_requests(requests.size());
    for (size_t index = 0; index < requests.size(); ++index) {
      const auto &request = requests[index];
      py::dict key;
      key["position_hash"] = request.key.position_hash;
      key["key_version"] = request.key.key_version;
      key["observer"] = request.key.observer;
      key["domain"] = static_cast<uint8_t>(request.key.domain);
      key["mode_bits"] = request.key.mode_bits;
      py::dict item;
      item["pending_id"] = request.pending_id;
      item["simulation_id"] = request.owner_simulation_id;
      item["tree_key"] = std::move(key);
      // Callbacks may retain these arrays after returning. Every request owns
      // independent contiguous storage.
      item["features"] = owning_array_copy(request.features);
      item["valid_actions"] = owning_array_copy(request.owner_world_mask);
      python_requests[index] = std::move(item);
    }

    py::object callback_result = inference_fn(python_requests);
    py::sequence sequence = callback_result.cast<py::sequence>();
    if (py::len(sequence) != requests.size())
      throw py::value_error(
          "parallel inference result count does not match requests");
    std::vector<mcts_parallel::ParallelInferenceResult> results;
    results.reserve(requests.size());
    for (size_t index = 0; index < requests.size(); ++index) {
      py::dict item = sequence[index].cast<py::dict>();
      if (!item.contains("policy") || !item.contains("value"))
        throw py::value_error(
            "parallel inference result requires policy and value");
      py::array_t<float, py::array::c_style | py::array::forcecast> policy =
          item["policy"].cast<py::array_t<
              float, py::array::c_style | py::array::forcecast>>();
      py::array_t<float, py::array::c_style | py::array::forcecast> value =
          item["value"].cast<py::array_t<
              float, py::array::c_style | py::array::forcecast>>();
      if (policy.ndim() != 1 || value.ndim() != 1 ||
          policy.shape(0) < static_cast<py::ssize_t>(MAX_ACTIONS) ||
          value.shape(0) < static_cast<py::ssize_t>(NUM_PLAYERS))
        throw py::value_error(
            "parallel policy/value arrays have invalid shape");
      mcts_parallel::ParallelInferenceResult result;
      std::copy_n(policy.data(), MAX_ACTIONS, result.policy.begin());
      std::copy_n(value.data(), NUM_PLAYERS, result.value.begin());
      results.push_back(result);
    }
    return results;
  };
}

// Python callback featurizer
class PyFeaturizer : public IFeaturizer {
public:
  PyFeaturizer(py::object featurizer) : featurizer_(featurizer) {}

  std::array<float, 196> featurize(const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result = featurizer_.attr("featurize")(py::cast(game));
    py::array_t<float> arr = result.cast<py::array_t<float>>();
    std::array<float, 196> features = {0};
    auto r = arr.unchecked<1>();
    for (py::ssize_t i = 0;
         i < std::min(static_cast<py::ssize_t>(196), r.shape(0)); ++i) {
      features[i] = r(i);
    }
    return features;
  }

private:
  py::object featurizer_;
};

// Python callback action encoder
class PyActionEncoder : public IActionEncoder {
public:
  PyActionEncoder(py::object encoder) : encoder_(encoder) {}

  int encode(const Action &action, const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result =
        encoder_.attr("encode")(py::cast(action), py::cast(game));
    return result.cast<int>();
  }

  Action decode(int action_idx, const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result = encoder_.attr("decode")(action_idx, py::cast(game));
    if (result.is_none()) {
      return Action(); // Return default action
    }
    return result.cast<Action>();
  }

  std::array<uint8_t, MAX_ACTIONS> get_action_mask(const Game &game) override {
    py::gil_scoped_acquire acquire;
    py::object result = encoder_.attr("get_action_mask")(py::cast(game));
    return copy_mask(result);
  }

  std::array<uint8_t, MAX_ACTIONS> get_action_mask_owned(Game &&game) override {
    py::gil_scoped_acquire acquire;
    py::object result = encoder_.attr("get_action_mask")(
        py::cast(std::move(game), py::return_value_policy::move));
    return copy_mask(result);
  }

private:
  static std::array<uint8_t, MAX_ACTIONS> copy_mask(const py::object &result) {
    py::array_t<uint8_t> arr = result.cast<py::array_t<uint8_t>>();
    std::array<uint8_t, MAX_ACTIONS> mask = {0};
    auto r = arr.unchecked<1>();
    for (py::ssize_t i = 0;
         i < std::min(static_cast<py::ssize_t>(MAX_ACTIONS), r.shape(0));
         ++i) {
      mask[i] = r(i);
    }
    return mask;
  }

  py::object encoder_;
};

} // namespace

namespace csplendor::python {

void bind_mcts(py::module_ &m) {
  // MCTS bindings
  py::enum_<mcts_parallel::TreeBackend>(m, "ParallelTreeBackend")
      .value("COARSE", mcts_parallel::TreeBackend::Coarse)
      .value("SHARDED", mcts_parallel::TreeBackend::Sharded);

  py::enum_<mcts_parallel::ParallelSearchMode>(m, "ParallelSearchMode")
      .value("THROUGHPUT", mcts_parallel::ParallelSearchMode::Throughput)
      .value("DETERMINISTIC_EPOCH",
             mcts_parallel::ParallelSearchMode::DeterministicEpoch)
      .value("ROOT_PARALLEL",
             mcts_parallel::ParallelSearchMode::RootParallel);

  py::enum_<mcts_parallel::SearchStopReason>(m, "ParallelSearchStopReason")
      .value("COMPLETED", mcts_parallel::SearchStopReason::Completed)
      .value("CANCELLED", mcts_parallel::SearchStopReason::Cancelled)
      .value("TIMED_OUT", mcts_parallel::SearchStopReason::TimedOut)
      .value("TREE_CAPACITY_REACHED",
             mcts_parallel::SearchStopReason::TreeCapacityReached)
      .value("CALLBACK_ERROR",
             mcts_parallel::SearchStopReason::CallbackError)
      .value("WORKER_ERROR", mcts_parallel::SearchStopReason::WorkerError);

  py::class_<mcts_parallel::ParallelCancellationToken>(
      m, "ParallelCancellationToken")
      .def(py::init<>())
      .def("request_cancel",
           &mcts_parallel::ParallelCancellationToken::request_cancel)
      .def_property_readonly(
          "is_cancelled",
          &mcts_parallel::ParallelCancellationToken::is_cancelled);

  py::class_<mcts_parallel::ParallelSearchOptions>(m,
                                                    "ParallelSearchOptions")
      .def(py::init<>())
      .def_readwrite("num_threads",
                     &mcts_parallel::ParallelSearchOptions::num_threads)
      .def_readwrite("batch_size",
                     &mcts_parallel::ParallelSearchOptions::batch_size)
      .def_readwrite("batch_wait_us",
                     &mcts_parallel::ParallelSearchOptions::batch_wait_us)
      .def_readwrite("max_inflight",
                     &mcts_parallel::ParallelSearchOptions::max_inflight)
      .def_readwrite(
          "deterministic_epoch_size",
          &mcts_parallel::ParallelSearchOptions::deterministic_epoch_size)
      .def_readwrite("num_simulations",
                     &mcts_parallel::ParallelSearchOptions::num_simulations)
      .def_readwrite("master_seed",
                     &mcts_parallel::ParallelSearchOptions::master_seed)
      .def_readwrite("search_nonce",
                     &mcts_parallel::ParallelSearchOptions::search_nonce)
      .def_readwrite("simulation_id_base",
                     &mcts_parallel::ParallelSearchOptions::simulation_id_base)
      .def_readwrite("evaluator_version",
                     &mcts_parallel::ParallelSearchOptions::evaluator_version)
      .def_readwrite("timeout_ms",
                     &mcts_parallel::ParallelSearchOptions::timeout_ms)
      .def_readwrite("max_tree_nodes",
                     &mcts_parallel::ParallelSearchOptions::max_tree_nodes)
      .def_readwrite("shard_count",
                     &mcts_parallel::ParallelSearchOptions::shard_count)
      .def_readwrite("tree_backend",
                     &mcts_parallel::ParallelSearchOptions::tree_backend)
      .def_readwrite("mode", &mcts_parallel::ParallelSearchOptions::mode)
      .def_readwrite(
          "cancellation_token",
          &mcts_parallel::ParallelSearchOptions::cancellation_token);

  py::class_<mcts_parallel::SearchLedgerSnapshot>(
      m, "ParallelSearchLedger")
      .def_readonly("issued",
                    &mcts_parallel::SearchLedgerSnapshot::issued)
      .def_readonly("selected",
                    &mcts_parallel::SearchLedgerSnapshot::selected)
      .def_readonly("evaluation_owner",
                    &mcts_parallel::SearchLedgerSnapshot::evaluation_owner)
      .def_readonly("evaluation_waiter",
                    &mcts_parallel::SearchLedgerSnapshot::evaluation_waiter)
      .def_readonly("evaluation_requested",
                    &mcts_parallel::SearchLedgerSnapshot::evaluation_requested)
      .def_readonly("evaluated_boards",
                    &mcts_parallel::SearchLedgerSnapshot::evaluated_boards)
      .def_readonly(
          "completed_evaluated",
          &mcts_parallel::SearchLedgerSnapshot::completed_evaluated)
      .def_readonly("completed_terminal",
                    &mcts_parallel::SearchLedgerSnapshot::completed_terminal)
      .def_readonly("completed_max_depth",
                    &mcts_parallel::SearchLedgerSnapshot::completed_max_depth)
      .def_readonly("cancelled",
                    &mcts_parallel::SearchLedgerSnapshot::cancelled)
      .def_readonly("failed", &mcts_parallel::SearchLedgerSnapshot::failed)
      .def_readonly(
          "virtual_loss_added",
          &mcts_parallel::SearchLedgerSnapshot::virtual_loss_added)
      .def_readonly(
          "virtual_loss_released",
          &mcts_parallel::SearchLedgerSnapshot::virtual_loss_released)
      .def_readonly(
          "reservations_committed",
          &mcts_parallel::SearchLedgerSnapshot::reservations_committed)
      .def_readonly("reservations_aborted",
                    &mcts_parallel::SearchLedgerSnapshot::reservations_aborted)
      .def_readonly("expansion_claimed",
                    &mcts_parallel::SearchLedgerSnapshot::expansion_claimed)
      .def_readonly("expansion_published",
                    &mcts_parallel::SearchLedgerSnapshot::expansion_published)
      .def_readonly("expansion_waited",
                    &mcts_parallel::SearchLedgerSnapshot::expansion_waited)
      .def_readonly("stale_result",
                    &mcts_parallel::SearchLedgerSnapshot::stale_result)
      .def_readonly("duplicate_result",
                    &mcts_parallel::SearchLedgerSnapshot::duplicate_result)
      .def_readonly("invalid_replay",
                    &mcts_parallel::SearchLedgerSnapshot::invalid_replay)
      .def_readonly("integrity_errors",
                    &mcts_parallel::SearchLedgerSnapshot::integrity_errors)
      .def_readonly(
          "max_inflight_observed",
          &mcts_parallel::SearchLedgerSnapshot::max_inflight_observed)
      .def_property_readonly("completed",
                             &mcts_parallel::SearchLedgerSnapshot::completed)
      .def_property_readonly(
          "virtual_loss_balanced",
          &mcts_parallel::SearchLedgerSnapshot::virtual_loss_balanced);

  py::class_<mcts_parallel::ParallelSearchResult>(m, "ParallelSearchResult")
      .def_property_readonly(
          "visits", [](const mcts_parallel::ParallelSearchResult &result) {
            return std::vector<uint64_t>(result.visits.begin(),
                                         result.visits.end());
          })
      .def_property_readonly(
          "q_values", [](const mcts_parallel::ParallelSearchResult &result) {
            return std::vector<double>(result.q_values.begin(),
                                       result.q_values.end());
          })
      .def_property_readonly(
          "probabilities",
          [](const mcts_parallel::ParallelSearchResult &result) {
            return std::vector<float>(result.probabilities.begin(),
                                      result.probabilities.end());
          })
      .def_readonly("ledger", &mcts_parallel::ParallelSearchResult::ledger)
      .def_readonly("stop_reason",
                    &mcts_parallel::ParallelSearchResult::stop_reason)
      .def_readonly("resolved_seed",
                    &mcts_parallel::ParallelSearchResult::resolved_seed)
      .def_readonly("rng_version",
                    &mcts_parallel::ParallelSearchResult::rng_version)
      .def_readonly("search_nonce",
                    &mcts_parallel::ParallelSearchResult::search_nonce)
      .def_readonly("tree_generation",
                    &mcts_parallel::ParallelSearchResult::tree_generation)
      .def_readonly("tree_size",
                    &mcts_parallel::ParallelSearchResult::tree_size)
      .def_readonly("elapsed_microseconds",
                    &mcts_parallel::ParallelSearchResult::elapsed_microseconds)
      .def_readonly("partial", &mcts_parallel::ParallelSearchResult::partial);

  py::class_<mcts_parallel::RootParallelResult>(m, "RootParallelSearchResult")
      .def_readonly("merged", &mcts_parallel::RootParallelResult::merged)
      .def_readonly("workers", &mcts_parallel::RootParallelResult::workers)
      .def_readonly(
          "duplicate_root_evaluations_avoided",
          &mcts_parallel::RootParallelResult::duplicate_root_evaluations_avoided);

  py::class_<MCTSConfig>(m, "MCTSConfig")
      .def(py::init<>())
      .def_readwrite("cpuct", &MCTSConfig::cpuct)
      .def_readwrite("dirichlet_alpha", &MCTSConfig::dirichlet_alpha)
      .def_readwrite("dirichlet_epsilon", &MCTSConfig::dirichlet_epsilon)
      .def_readwrite("use_dirichlet_noise", &MCTSConfig::use_dirichlet_noise)
      .def_readwrite("use_determinization", &MCTSConfig::use_determinization)
      .def_readwrite("num_simulations", &MCTSConfig::num_simulations)
      .def_readwrite("num_determinizations", &MCTSConfig::num_determinizations)
      .def_readwrite("fpu", &MCTSConfig::fpu)
      .def_readwrite("forced_playouts", &MCTSConfig::forced_playouts)
      .def_readwrite("forced_playouts_k", &MCTSConfig::forced_playouts_k);

  py::class_<MCTSNode>(m, "MCTSNode")
      .def(py::init<>())
      .def_readonly("total_visits", &MCTSNode::total_visits)
      .def_readonly("is_terminal", &MCTSNode::is_terminal)
      .def_readonly("is_expanded", &MCTSNode::is_expanded)
      .def_property_readonly("valid_actions",
                             [](const MCTSNode &n) {
                               std::vector<uint8_t> v(n.valid_actions.begin(),
                                                      n.valid_actions.end());
                               return v;
                             })
      .def_property_readonly("prior",
                             [](const MCTSNode &n) {
                               std::vector<float> v(n.prior.begin(),
                                                    n.prior.end());
                               return v;
                             })
      .def_property_readonly("Q",
                             [](const MCTSNode &n) {
                               std::vector<float> v(n.Q.begin(), n.Q.end());
                               return v;
                             })
      .def_property_readonly("N",
                             [](const MCTSNode &n) {
                               std::vector<uint32_t> v(n.N.begin(), n.N.end());
                               return v;
                             })
      .def_property_readonly("virtual_loss",
                             [](const MCTSNode &n) {
                               std::vector<int32_t> v(n.virtual_loss.begin(),
                                                      n.virtual_loss.end());
                               return v;
                             })
      .def_property_readonly("value", [](const MCTSNode &n) {
        std::vector<float> v(n.value.begin(), n.value.end());
        return v;
      });

  py::class_<MCTS>(m, "MCTS")
      .def(py::init<const MCTSConfig &>())
      .def("clear", &MCTS::clear)
      .def("reset_replay_sequence", &MCTS::reset_replay_sequence,
           py::arg("seed"), py::arg("nonce"),
           "Reset the parallel-search seed and next nonce while idle")
      .def("tree_size", &MCTS::tree_size)
      .def("prune_if_needed", &MCTS::prune_if_needed)
      .def("get_node",
           [](MCTS &mcts, uint64_t hash) -> py::object {
             auto node = mcts.get_node_snapshot(hash);
             if (node)
               return py::cast(*node);
             return py::none();
           })
      .def("expand_node",
           [](MCTS &mcts, uint64_t hash, const std::vector<float> &policy,
              const std::vector<float> &value,
              const std::vector<uint8_t> &valid_actions) {
             std::array<float, MAX_ACTIONS> policy_arr = {0};
             std::array<float, NUM_PLAYERS> value_arr = {0};
             std::array<uint8_t, MAX_ACTIONS> valid_arr = {0};

             for (size_t i = 0; i < policy.size() && i < MAX_ACTIONS; ++i)
               policy_arr[i] = policy[i];
             for (size_t i = 0; i < value.size() && i < NUM_PLAYERS; ++i)
               value_arr[i] = value[i];
             for (size_t i = 0; i < valid_actions.size() && i < MAX_ACTIONS;
                  ++i)
               valid_arr[i] = valid_actions[i];

             mcts.expand_node(hash, policy_arr, value_arr, valid_arr);
           })
      .def("get_action_probs",
           [](const MCTS &mcts, uint64_t hash, float temperature) {
             auto probs = mcts.get_action_probs(hash, temperature);
             return std::vector<float>(probs.begin(), probs.end());
           })
      .def("update_stats", &MCTS::update_stats, py::arg("hash"),
           py::arg("action"), py::arg("value"),
           "Update node statistics after backpropagation")
      .def(
          "select_action_with_virtual_loss",
          [](MCTS &mcts, uint64_t hash, bool is_root,
             py::object dirichlet_noise_obj, int current_sim) {
            if (dirichlet_noise_obj.is_none()) {
              return mcts.select_action_with_virtual_loss(hash, is_root,
                                                          nullptr, current_sim);
            }
            std::vector<float> noise_vec =
                dirichlet_noise_obj.cast<std::vector<float>>();
            std::array<float, MAX_ACTIONS> noise = {0};
            for (size_t i = 0; i < noise_vec.size() && i < MAX_ACTIONS; ++i) {
              noise[i] = noise_vec[i];
            }
            return mcts.select_action_with_virtual_loss(hash, is_root, &noise,
                                                        current_sim);
          },
          py::arg("hash"), py::arg("is_root"),
          py::arg("dirichlet_noise") = py::none(), py::arg("current_sim") = 0,
          "Select action with virtual loss for parallel MCTS (supports FPU and "
          "forced playouts)")
      .def("add_virtual_loss", &MCTS::add_virtual_loss, py::arg("hash"),
           py::arg("action"), "Add virtual loss to an action")
      .def("remove_virtual_loss", &MCTS::remove_virtual_loss, py::arg("hash"),
           py::arg("action"), "Remove virtual loss from an action")
      .def("clear_virtual_losses", &MCTS::clear_virtual_losses,
           "Clear all virtual losses")
      .def(
          "generate_dirichlet_noise",
          [](MCTS &mcts, uint64_t hash) {
            auto noise = mcts.generate_dirichlet_noise_for_node(hash);
            return std::vector<float>(noise.begin(), noise.end());
          },
          py::arg("hash"), "Generate Dirichlet noise for a node")
      .def(
          "prepare_batch_simulations",
          [](MCTS &mcts, const Game &root_game, uint8_t observer,
             int batch_size, int num_determinizations,
             py::object dirichlet_noise_obj) {
            // Convert dirichlet noise
            const std::array<float, MAX_ACTIONS> *noise_ptr = nullptr;
            std::array<float, MAX_ACTIONS> noise = {0};
            if (!dirichlet_noise_obj.is_none()) {
              std::vector<float> noise_vec =
                  dirichlet_noise_obj.cast<std::vector<float>>();
              for (size_t i = 0; i < noise_vec.size() && i < MAX_ACTIONS; ++i) {
                noise[i] = noise_vec[i];
              }
              noise_ptr = &noise;
            }

            // Use native C++ ActionEncoder (no GIL contention!)
            auto result =
                mcts.prepare_batch_simulations(root_game, observer, batch_size,
                                               num_determinizations, noise_ptr);

            // Convert to Python-friendly format. Every array remains an
            // independent owning copy; only list growth is eliminated.
            py::dict py_result;

            // Flatten encoded boards and valid actions for batch NN inference
            const size_t leaf_count = result.leaves.size();
            py::list flat_boards(result.total_boards);
            py::list flat_valids(result.total_boards);
            py::list leaf_world_counts(leaf_count);
            py::list leaf_hashes(leaf_count);
            py::list leaf_paths(leaf_count);
            size_t flat_index = 0;

            for (size_t leaf_index = 0; leaf_index < leaf_count;
                 ++leaf_index) {
              const auto &leaf = result.leaves[leaf_index];
              leaf_hashes[leaf_index] = leaf.hash;
              leaf_world_counts[leaf_index] = leaf.num_worlds;

              // Convert path to Python list
              py::list py_path(leaf.path.size());
              for (size_t path_index = 0; path_index < leaf.path.size();
                   ++path_index) {
                const auto &entry = leaf.path[path_index];
                py_path[path_index] =
                    py::make_tuple(entry.hash, entry.action, entry.player);
              }
              leaf_paths[leaf_index] = py_path;

              // Add boards and valid actions
              const size_t leaf_flat_start = flat_index;
              for (const auto &board : leaf.encoded_boards) {
                flat_boards[flat_index] = owning_array_copy(board);
                ++flat_index;
              }
              for (size_t valid_index = 0;
                   valid_index < leaf.valid_actions.size(); ++valid_index) {
                const auto &valid = leaf.valid_actions[valid_index];
                flat_valids[leaf_flat_start + valid_index] =
                    owning_array_copy(valid);
              }
            }

            // Handle terminals
            py::list py_terminals(result.terminals.size());
            for (size_t terminal_index = 0;
                 terminal_index < result.terminals.size(); ++terminal_index) {
              const auto &[path, value] = result.terminals[terminal_index];
              py::list py_path(path.size());
              for (size_t path_index = 0; path_index < path.size();
                   ++path_index) {
                const auto &entry = path[path_index];
                py_path[path_index] =
                    py::make_tuple(entry.hash, entry.action, entry.player);
              }
              py::list py_value(NUM_PLAYERS);
              for (size_t value_index = 0; value_index < NUM_PLAYERS;
                   ++value_index)
                py_value[value_index] = value[value_index];
              py_terminals[terminal_index] = py::make_tuple(py_path, py_value);
            }

            py_result["flat_boards"] = flat_boards;
            py_result["flat_valids"] = flat_valids;
            py_result["leaf_world_counts"] = leaf_world_counts;
            py_result["leaf_hashes"] = leaf_hashes;
            py_result["leaf_paths"] = leaf_paths;
            py_result["terminals"] = py_terminals;
            py_result["total_boards"] = result.total_boards;
            py_result["num_leaves"] = static_cast<int>(result.leaves.size());
            py_result["tree_generation"] = result.tree_generation;

            return py_result;
          },
          py::arg("root_game"), py::arg("observer"), py::arg("batch_size"),
          py::arg("num_determinizations"), py::arg("dirichlet_noise"),
          "Prepare batch simulations for NN evaluation (uses native C++ "
          "ActionEncoder)")
      .def(
          "apply_batch_results",
          [](MCTS &mcts, py::dict request, py::list policies, py::list values) {
            py::list leaf_hashes = request["leaf_hashes"].cast<py::list>();
            py::list leaf_world_counts =
                request["leaf_world_counts"].cast<py::list>();
            py::list leaf_paths = request["leaf_paths"].cast<py::list>();
            py::list flat_valids = request["flat_valids"].cast<py::list>();
            py::list terminals = request["terminals"].cast<py::list>();

            if (py::len(leaf_hashes) != py::len(leaf_world_counts) ||
                py::len(leaf_hashes) != py::len(leaf_paths))
              throw py::value_error("batch leaf metadata lengths differ");
            size_t expected_worlds = 0;
            for (auto item : leaf_world_counts) {
              const int count = item.cast<int>();
              if (count <= 0)
                throw py::value_error("leaf world count must be positive");
              expected_worlds += static_cast<size_t>(count);
            }
            if (py::len(policies) < expected_worlds ||
                py::len(values) < expected_worlds ||
                py::len(flat_valids) < expected_worlds)
              throw py::value_error(
                  "batch results do not match requested worlds");

            // Convert the complete Python payload first. The canonical native
            // implementation validates it once more before mutating any node,
            // so malformed/stale batches cannot be partially applied.
            BatchSimulationRequest native_request;
            native_request.tree_generation =
                request.contains("tree_generation")
                    ? request["tree_generation"].cast<uint64_t>()
                    : mcts.tree_generation_snapshot();
            native_request.total_boards = static_cast<int>(expected_worlds);
            size_t result_idx = 0;
            for (size_t i = 0; i < py::len(leaf_hashes); ++i) {
              BatchLeafData leaf;
              leaf.hash = leaf_hashes[i].cast<uint64_t>();
              leaf.num_worlds = leaf_world_counts[i].cast<int>();
              py::list py_path = leaf_paths[i].cast<py::list>();
              leaf.path.reserve(py::len(py_path));
              for (auto item : py_path) {
                py::tuple t = item.cast<py::tuple>();
                if (py::len(t) != 3)
                  throw py::value_error("batch path entry must have 3 fields");
                PathEntry entry;
                entry.hash = t[0].cast<uint64_t>();
                entry.action = t[1].cast<int>();
                entry.player = t[2].cast<int>();
                leaf.path.push_back(entry);
              }
              for (int world = 0; world < leaf.num_worlds; ++world) {
                py::array_t<uint8_t> valid =
                    flat_valids[result_idx].cast<py::array_t<uint8_t>>();
                auto valid_mask = valid.unchecked<1>();
                if (valid_mask.shape(0) <
                    static_cast<py::ssize_t>(MAX_ACTIONS))
                  throw py::value_error("valid-action mask is too short");
                std::array<uint8_t, MAX_ACTIONS> mask{};
                for (size_t action = 0; action < MAX_ACTIONS; ++action)
                  mask[action] =
                      valid_mask(static_cast<py::ssize_t>(action));
                leaf.valid_actions.push_back(mask);
                result_idx++;
              }
              native_request.leaves.push_back(std::move(leaf));
            }

            for (auto item : terminals) {
              py::tuple t = item.cast<py::tuple>();
              if (py::len(t) != 2)
                throw py::value_error("terminal batch entry must have 2 fields");
              py::list py_path = t[0].cast<py::list>();
              py::list py_value = t[1].cast<py::list>();
              std::vector<PathEntry> path;
              path.reserve(py::len(py_path));
              for (auto p_item : py_path) {
                py::tuple pt = p_item.cast<py::tuple>();
                if (py::len(pt) != 3)
                  throw py::value_error("batch path entry must have 3 fields");
                PathEntry entry;
                entry.hash = pt[0].cast<uint64_t>();
                entry.action = pt[1].cast<int>();
                entry.player = pt[2].cast<int>();
                path.push_back(entry);
              }

              std::array<float, NUM_PLAYERS> value = {0};
              for (size_t j = 0; j < py::len(py_value) && j < NUM_PLAYERS;
                   ++j) {
                value[j] = py_value[j].cast<float>();
              }
              native_request.terminals.push_back({std::move(path), value});
            }

            std::vector<std::array<float, MAX_ACTIONS>> native_policies;
            std::vector<std::array<float, NUM_PLAYERS>> native_values;
            native_policies.reserve(expected_worlds);
            native_values.reserve(expected_worlds);
            for (size_t index = 0; index < expected_worlds; ++index) {
              py::array_t<float> policy =
                  policies[index].cast<py::array_t<float>>();
              py::array_t<float> value =
                  values[index].cast<py::array_t<float>>();
              auto policy_view = policy.unchecked<1>();
              auto value_view = value.unchecked<1>();
              if (policy_view.shape(0) <
                      static_cast<py::ssize_t>(MAX_ACTIONS) ||
                  value_view.shape(0) <
                      static_cast<py::ssize_t>(NUM_PLAYERS))
                throw py::value_error("batch policy/value array is too short");
              std::array<float, MAX_ACTIONS> policy_array{};
              std::array<float, NUM_PLAYERS> value_array{};
              for (size_t action = 0; action < MAX_ACTIONS; ++action)
                policy_array[action] =
                    policy_view(static_cast<py::ssize_t>(action));
              for (size_t player = 0; player < NUM_PLAYERS; ++player)
                value_array[player] =
                    value_view(static_cast<py::ssize_t>(player));
              native_policies.push_back(policy_array);
              native_values.push_back(value_array);
            }
            mcts.apply_batch_results(native_request, native_policies,
                                     native_values);
          },
          py::arg("request"), py::arg("policies"), py::arg("values"),
          "Apply batch NN results to the tree")
      .def("get_config_snapshot", &MCTS::get_config_snapshot)
      .def("set_config", &MCTS::set_config, py::arg("config"))
      .def("is_parallel_search_active", &MCTS::is_parallel_search_active)
      .def("tree_generation", &MCTS::tree_generation_snapshot)
      .def_property(
          "config", [](const MCTS &mcts) { return mcts.get_config_snapshot(); },
          [](MCTS &mcts, const MCTSConfig &config) { mcts.set_config(config); });

  m.def(
      "mcts_search_parallel_native",
      [](MCTS &mcts, const Game &root_game,
         const mcts_parallel::ParallelSearchOptions &options,
         py::function inference_fn, float temperature) {
        auto inference = make_python_parallel_inference(inference_fn);

        mcts_parallel::ParallelMCTSSearcher searcher;
        mcts_parallel::ParallelSearchResult result;
        // Snapshot every Python-owned input while the GIL is held. Acquiring
        // the search guard here also freezes config/generation at API entry,
        // before another Python thread can mutate the same MCTS/options.
        mcts_parallel::ParallelSearchOptions options_snapshot = options;
        mcts_parallel::ParallelMCTSSearcher::validate_entry_options(
            options_snapshot, inference, temperature);
        mcts_parallel::ParallelMCTSSearcher::validate_config_snapshot(
            mcts.get_config_snapshot());
        Game root_snapshot = root_game.clone_light();
        if (options_snapshot.num_simulations > 0 &&
            !options_snapshot.cancellation_requested() &&
            mcts_internal::GameAdapter::requires_forced_pass(root_snapshot))
          throw std::invalid_argument(
              "MCTS root requires a forced pass; apply it before searching");
        auto guard = mcts.begin_parallel_search();
        {
          py::gil_scoped_release release;
          result = searcher.run_with_guard(
              mcts, std::move(guard), root_snapshot, options_snapshot,
              inference, temperature);
        }
        return result;
      },
      py::arg("mcts"), py::arg("root_game"), py::arg("options"),
      py::arg("inference_fn"), py::arg("temperature") = 1.0f,
      "Experimental native parallel MCTS search. Traversal releases the GIL; "
      "one coordinator serializes Python inference callbacks.");

  m.def(
      "mcts_search_root_parallel_native",
      [](const MCTSConfig &config, const Game &root_game,
         uint64_t simulation_budget, uint32_t num_workers,
         const mcts_parallel::ParallelSearchOptions &options,
         py::function inference_fn, float temperature) {
        const MCTSConfig config_snapshot = config;
        mcts_parallel::ParallelSearchOptions options_snapshot = options;
        options_snapshot.serialize_root_callbacks = true;
        Game root_snapshot = root_game.clone_light();
        auto evaluator_factory =
            [&inference_fn](uint32_t /*worker_id*/) {
              return make_python_parallel_inference(inference_fn);
            };
        mcts_parallel::RootParallelResult result;
        {
          py::gil_scoped_release release;
          result = mcts_parallel::run_root_parallel(
              config_snapshot, root_snapshot, simulation_budget, num_workers,
              options_snapshot, evaluator_factory, temperature);
        }
        return result;
      },
      py::arg("config"), py::arg("root_game"),
      py::arg("simulation_budget"), py::arg("num_workers"),
      py::arg("options"), py::arg("inference_fn"),
      py::arg("temperature") = 1.0f,
      "Experimental independent-tree root-parallel fallback. Worker "
      "traversal releases the GIL; Python callbacks remain GIL-serialized.");

  // LeafRequest binding
  py::class_<LeafRequest>(m, "LeafRequest")
      .def(py::init<>())
      .def_readonly("hash", &LeafRequest::hash)
      .def_property_readonly("features",
                             [](const LeafRequest &req) {
                               return std::vector<float>(req.features.begin(),
                                                         req.features.end());
                             })
      .def_property_readonly("valid_actions",
                             [](const LeafRequest &req) {
                               return std::vector<uint8_t>(
                                   req.valid_actions.begin(),
                                   req.valid_actions.end());
                             })
      .def_readonly("path_index", &LeafRequest::path_index);

  // InferenceResult binding
  py::class_<InferenceResult>(m, "InferenceResult")
      .def(py::init<>())
      .def(py::init([](const std::vector<float> &policy,
                       const std::vector<float> &value) {
             InferenceResult res;
             for (size_t i = 0; i < policy.size() && i < MAX_ACTIONS; ++i)
               res.policy[i] = policy[i];
             for (size_t i = 0; i < value.size() && i < NUM_PLAYERS; ++i)
               res.value[i] = value[i];
             return res;
           }),
           py::arg("policy"), py::arg("value"))
      .def_property(
          "policy",
          [](const InferenceResult &res) {
            return std::vector<float>(res.policy.begin(), res.policy.end());
          },
          [](InferenceResult &res, const std::vector<float> &policy) {
            res.policy.fill(0.0f);
            for (size_t i = 0; i < policy.size() && i < MAX_ACTIONS; ++i)
              res.policy[i] = policy[i];
          })
      .def_property(
          "value",
          [](const InferenceResult &res) {
            return std::vector<float>(res.value.begin(), res.value.end());
          },
          [](InferenceResult &res, const std::vector<float> &value) {
            res.value.fill(0.0f);
            for (size_t i = 0; i < value.size() && i < NUM_PLAYERS; ++i)
              res.value[i] = value[i];
          });

  // Full search function that runs entirely in C++ with Python inference
  // callback
  m.def(
      "mcts_search",
      [](MCTS &mcts, py::object featurizer, py::object encoder,
         const Game &root_game, int num_simulations, py::function inference_fn,
         float temperature) {
        PyFeaturizer py_feat(featurizer);
        PyActionEncoder py_enc(encoder);
        MCTSSearcher searcher(mcts, py_feat, py_enc);
        // Python inference callbacks can retain and mutate the caller's root
        // through a closure. Snapshot it while the GIL is still held so every
        // simulation and the final root lookup use one stable position.
        Game root_snapshot = mcts.config().use_determinization
                                 ? root_game.clone_light()
                                 : root_game.clone();

        // Inference callback wrapper
        auto cpp_inference =
            [&inference_fn](const std::vector<LeafRequest> &requests)
            -> std::vector<InferenceResult> {
          py::gil_scoped_acquire acquire;

          // Convert LeafRequests to Python-friendly format
          py::list py_requests;
          for (const auto &req : requests) {
            py::dict d;
            d["hash"] = req.hash;
            // The callback may retain a request after it returns. Keep the
            // ndarray lifetime independent of the temporary C++ request
            // vector instead of exposing a dangling view.
            d["features"] = owning_array_copy(req.features);
            d["valid_actions"] = owning_array_copy(req.valid_actions);
            d["path_index"] = req.path_index;
            py_requests.append(d);
          }

          // Call Python inference function
          py::object result = inference_fn(py_requests);
          py::list results = result.cast<py::list>();

          std::vector<InferenceResult> cpp_results;
          for (auto item : results) {
            InferenceResult ir;
            py::dict d = item.cast<py::dict>();
            py::array_t<float> policy = d["policy"].cast<py::array_t<float>>();
            py::array_t<float> value = d["value"].cast<py::array_t<float>>();

            auto p = policy.unchecked<1>();
            for (py::ssize_t i = 0;
                 i < std::min(static_cast<py::ssize_t>(MAX_ACTIONS),
                              p.shape(0));
                 ++i)
              ir.policy[i] = p(i);

            auto v = value.unchecked<1>();
            for (py::ssize_t i = 0;
                 i < std::min(static_cast<py::ssize_t>(NUM_PLAYERS),
                              v.shape(0));
                 ++i)
              ir.value[i] = v(i);

            cpp_results.push_back(ir);
          }
          return cpp_results;
        };

        // Run search
        {
          py::gil_scoped_release release;
          searcher.search(root_snapshot, num_simulations, cpp_inference);
        }

        // Get action probabilities
        auto probs = searcher.get_action_probs(root_snapshot, temperature);
        return std::vector<float>(probs.begin(), probs.end());
      },
      py::arg("mcts"), py::arg("featurizer"), py::arg("encoder"),
      py::arg("root_game"), py::arg("num_simulations"), py::arg("inference_fn"),
      py::arg("temperature"),
      "Run full MCTS search with C++ searcher and Python inference callback");
}

} // namespace csplendor::python
