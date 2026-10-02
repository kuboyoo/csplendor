#include "bindings.h"
#include "game.h"
#include "mcts_v3.h"
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace py = pybind11;

namespace {

using csplendor::v3search::Batch;
using csplendor::v3search::Config;
using csplendor::v3search::Session;

template <typename T>
py::array_t<T> vector_array(const std::vector<T> &source) {
  py::array_t<T> result(static_cast<py::ssize_t>(source.size()));
  if (!source.empty())
    std::memcpy(result.mutable_data(), source.data(), source.size() * sizeof(T));
  return result;
}

py::array_t<float> feature_matrix(const Batch &batch) {
  const py::ssize_t rows = static_cast<py::ssize_t>(batch.rows());
  const py::ssize_t dim = batch.state_dim;
  py::array_t<float> result({rows, dim});
  if (rows)
    std::memcpy(result.mutable_data(), batch.features.data(),
                batch.features.size() * sizeof(float));
  return result;
}

} // namespace

namespace csplendor::python {

void bind_mcts_v3(py::module_ &m) {
  py::class_<Config>(m, "V3SearchConfig",
                     "Settings of the native multi-game V3 PUCT search")
      .def(py::init<>())
      .def_readwrite("num_simulations", &Config::num_simulations)
      .def_readwrite("leaf_batch_size", &Config::leaf_batch_size)
      .def_readwrite("c_puct", &Config::c_puct)
      .def_readwrite("fpu_reduction", &Config::fpu_reduction)
      .def_readwrite("virtual_loss", &Config::virtual_loss)
      .def_readwrite("determinization", &Config::determinization)
      .def_readwrite("semantic_groups", &Config::semantic_groups)
      .def_readwrite("intra_group_cpuct", &Config::intra_group_cpuct)
      .def_readwrite("max_depth", &Config::max_depth)
      .def_readwrite("draw_value", &Config::draw_value)
      .def_readwrite("public_card_features", &Config::public_card_features)
      .def_readwrite("physical_seat_feature", &Config::physical_seat_feature)
      .def_readwrite("dirichlet_alpha", &Config::dirichlet_alpha)
      .def_readwrite("dirichlet_epsilon", &Config::dirichlet_epsilon)
      .def_readwrite("unseen_action_prior", &Config::unseen_action_prior)
      .def_readwrite("rollout_samples", &Config::rollout_samples)
      .def_readwrite("rollout_candidates", &Config::rollout_candidates)
      .def_readwrite("rollout_horizon", &Config::rollout_horizon)
      .def_readwrite("rollout_min_visits", &Config::rollout_min_visits)
      .def_readwrite("opponent_redeterminization", &Config::opponent_redeterminization)
      .def_readwrite("dynamic_cpuct", &Config::dynamic_cpuct)
      .def_readwrite("dynamic_cpuct_variance_floor", &Config::dynamic_cpuct_variance_floor)
      .def_readwrite("dynamic_cpuct_min_scale", &Config::dynamic_cpuct_min_scale)
      .def_readwrite("dynamic_cpuct_max_scale", &Config::dynamic_cpuct_max_scale)
      .def_readwrite("chance_enumeration_depth", &Config::chance_enumeration_depth)
      .def_readwrite("chance_risk_weight", &Config::chance_risk_weight)
      .def_readwrite("chance_control_variate", &Config::chance_control_variate)
      .def_property_readonly("state_dim", &Config::state_dim);

  py::class_<Session>(
      m, "V3SearchSession",
      "Many independent V3 searches advanced in lockstep with batched leaves")
      .def(py::init<const Config &>(), py::arg("config"))
      .def_property_readonly("state_dim", &Session::state_dim)
      .def("__len__", &Session::size)
      .def(
          "add_game",
          [](Session &session, const Game &root, int observer, uint64_t seed,
             bool root_noise, int num_simulations) {
            return session.add_game(root, observer, seed, root_noise,
                                    num_simulations);
          },
          py::arg("game"), py::arg("observer"), py::arg("seed"),
          py::arg("root_noise") = false, py::arg("num_simulations") = -1,
          "Register a root position; returns its slot index")
      .def(
          "reset_game",
          [](Session &session, int slot, const Game &root, int observer,
             uint64_t seed, bool root_noise, int num_simulations) {
            session.reset_game(slot, root, observer, seed, root_noise,
                               num_simulations);
          },
          py::arg("slot"), py::arg("game"), py::arg("observer"), py::arg("seed"),
          py::arg("root_noise") = false, py::arg("num_simulations") = -1,
          "Replace the tree of a slot with a new root position")
      .def(
          "advance_game",
          [](Session &session, int slot, const Game &root, std::vector<int32_t> actions,
             int observer, uint64_t seed, bool root_noise, int num_simulations) {
            return session.advance_game(slot, root, actions, observer, seed, root_noise,
                                        num_simulations);
          },
          py::arg("slot"), py::arg("game"), py::arg("actions"), py::arg("observer"),
          py::arg("seed"), py::arg("root_noise") = false, py::arg("num_simulations") = -1,
          "Re-root the slot's tree along the played actions when possible "
          "(returns True), else reset it")
      .def(
          "reused_visits",
          [](const Session &session, int slot) {
            return session.game(slot).reused_visits();
          },
          py::arg("slot"))
      .def(
          "set_root_visit_floor",
          [](Session &session, int slot, std::vector<int32_t> actions,
             std::vector<int> visits) {
            if (actions.size() != visits.size())
              throw std::invalid_argument("actions and visits must align");
            std::vector<std::pair<int32_t, int>> floors;
            for (size_t i = 0; i < actions.size(); ++i)
              floors.emplace_back(actions[i], visits[i]);
            session.set_root_visit_floor(slot, floors);
          },
          py::arg("slot"), py::arg("actions"), py::arg("visits"),
          "Force at least `visits` additional root visits on each action")
      .def(
          "root_visit_floor_allocated",
          [](const Session &session, int slot) {
            py::dict result;
            for (const auto &[action, forced] :
                 session.game(slot).root_visit_floor_allocated())
              result[py::int_(action)] = forced;
            return result;
          },
          py::arg("slot"))
      .def(
          "root_clean_priors",
          [](const Session &session, int slot) {
            py::dict result;
            for (const auto &[action, prior] : session.game(slot).root_clean_priors())
              result[py::int_(action)] = prior;
            return result;
          },
          py::arg("slot"))
      .def("done", &Session::done, py::arg("slot"))
      .def("all_done", &Session::all_done)
      .def("pending", &Session::pending)
      .def(
          "collect",
          [](Session &session) {
            Batch batch;
            {
              py::gil_scoped_release release;
              batch = session.collect();
            }
            return py::make_tuple(feature_matrix(batch),
                                  vector_array(batch.legal_ids),
                                  vector_array(batch.offsets),
                                  vector_array(batch.slots));
          },
          "Descend every active tree; returns (features[N,D], legal_ids[M], "
          "offsets[N+1], slots[N]) for the pending leaves")
      .def(
          "apply",
          [](Session &session,
             py::array_t<int32_t, py::array::c_style | py::array::forcecast>
                 legal_ids,
             py::array_t<int32_t, py::array::c_style | py::array::forcecast>
                 offsets,
             py::array_t<float, py::array::c_style | py::array::forcecast> priors,
             py::array_t<float, py::array::c_style | py::array::forcecast>
                 values) {
            if (offsets.ndim() != 1 || legal_ids.ndim() != 1 ||
                priors.ndim() != 1 || values.ndim() != 1)
              throw std::invalid_argument("apply expects one-dimensional arrays");
            const size_t rows = static_cast<size_t>(values.shape(0));
            if (static_cast<size_t>(offsets.shape(0)) != rows + 1)
              throw std::invalid_argument("offsets must have rows + 1 entries");
            if (priors.shape(0) != legal_ids.shape(0))
              throw std::invalid_argument("priors must align with legal_ids");
            const int32_t *offset_data = offsets.data();
            if (rows && offset_data[rows] != legal_ids.shape(0))
              throw std::invalid_argument("offsets do not cover legal_ids");
            const int32_t *legal_data = legal_ids.data();
            const float *prior_data = priors.data();
            const float *value_data = values.data();
            py::gil_scoped_release release;
            session.apply(legal_data, offset_data, rows, prior_data, value_data);
          },
          py::arg("legal_ids"), py::arg("offsets"), py::arg("priors"),
          py::arg("values"),
          "Install priors (aligned with the collected legal_ids) and values")
      .def(
          "root_visits",
          [](const Session &session, int slot) {
            py::dict result;
            for (const auto &[action, visits] : session.game(slot).root_visits())
              result[py::int_(action)] = visits;
            return result;
          },
          py::arg("slot"))
      .def(
          "root_action_values",
          [](const Session &session, int slot) {
            py::dict result;
            for (const auto &[action, value] :
                 session.game(slot).root_action_values())
              result[py::int_(action)] = value;
            return result;
          },
          py::arg("slot"))
      .def(
          "root_priors",
          [](const Session &session, int slot) {
            py::dict result;
            for (const auto &[action, prior] : session.game(slot).root_priors())
              result[py::int_(action)] = prior;
            return result;
          },
          py::arg("slot"))
      .def(
          "root_value",
          [](const Session &session, int slot) {
            return session.game(slot).root_value();
          },
          py::arg("slot"))
      .def(
          "simulations",
          [](const Session &session, int slot) {
            return session.game(slot).completed();
          },
          py::arg("slot"))
      .def(
          "node_count",
          [](const Session &session, int slot) {
            return session.game(slot).node_count();
          },
          py::arg("slot"))
      .def(
          "rollout_result",
          [](const Session &session, int slot) -> py::object {
            const auto &game = session.game(slot);
            if (!game.rollout_available())
              return py::none();
            const auto &candidates = game.rollout_candidates();
            const py::ssize_t rows = static_cast<py::ssize_t>(candidates.size());
            const py::ssize_t samples = game.rollout_sample_count();
            py::array_t<float> returns({rows, samples});
            py::array_t<uint8_t> terminal({rows, samples});
            // Stored sample-major (index = sample * C + candidate); expose
            // candidate-major like paired_policy_rollouts.
            for (py::ssize_t c = 0; c < rows; ++c)
              for (py::ssize_t s = 0; s < samples; ++s) {
                returns.mutable_at(c, s) = game.rollout_returns()[s * rows + c];
                terminal.mutable_at(c, s) = game.rollout_terminal()[s * rows + c];
              }
            py::dict result;
            result["candidates"] = candidates;
            result["returns"] = returns;
            result["terminal"] = terminal;
            return result;
          },
          py::arg("slot"),
          "Candidate-major rollout returns from the root player's view, or None")
      .def("stats", [](const Session &session) {
        const auto &stats = session.stats();
        py::dict result;
        result["simulations"] = stats.simulations;
        result["leaves"] = stats.leaves;
        result["terminals"] = stats.terminals;
        result["depth_limits"] = stats.depth_limits;
        result["collisions"] = stats.collisions;
        result["nodes"] = stats.nodes;
        result["rollout_rows"] = stats.rollout_rows;
        result["chance_enumerations"] = stats.chance_enumerations;
        result["chance_rows"] = stats.chance_rows;
        result["rollout_games"] = stats.rollout_games;
        return result;
      });

  m.def("v3_semantic_group_id", &csplendor::v3search::semantic_group_id,
        py::arg("action_id"),
        "Decision group of a V3 action id before payment/return details");
}

} // namespace csplendor::python
