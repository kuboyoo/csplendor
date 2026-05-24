import torch
import torch.nn as nn
import torch.nn.functional as F
import os
import sys
import glob
from typing import Optional, Literal, Dict, Any
import time
import numpy as np

# Add dlsplendor to path
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../dlsplendor")))

from dlsplendor.config import Config
from dlsplendor.network.model import SplendorNetwork
from dlsplendor.network.encoder import StateEncoder
from dlsplendor.search.mcts import MCTS, IterativeDeepening
from dlsplendor.search.pondering import PonderingEngine
from dlsplendor.search.greedy_ai import GreedyAI

AIType = Literal["mcts", "greedy", "genbu", "alphazero", "deepsets", "set_transformer", "nnue"]

class AIManager:
    _instance = None
    
    def __init__(self, model_path: str):
        self.model_path = model_path
        self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        self.config = Config.from_preset("thorough") 
        
        self.encoder = StateEncoder(self.config.game)
        self.network = SplendorNetwork(self.encoder.get_state_dim(), 2000, self.config.model).to(self.device)
        
        if os.path.exists(model_path):
            try:
                checkpoint = torch.load(model_path, map_location=self.device)
                state_dict = checkpoint
                if isinstance(checkpoint, dict) and 'model_state_dict' in checkpoint:
                    state_dict = checkpoint['model_state_dict']
                self.network.load_state_dict(state_dict)
                print(f"Loaded AI model from {model_path}")
            except Exception as e:
                print(f"ERROR: Failed to load model: {e}")
        
        self.network.eval()
        self.mcts = MCTS(self.network, self.encoder, self.config.search)
        self.id_search = IterativeDeepening(self.mcts)
        self.ponderer = PonderingEngine(self.mcts)
        
        # Greedy AI (rule-based, no learning required)
        self.greedy_ai = GreedyAI()

        # Genbu AI (legacy AlphaZero model via alphazero-general) - lazy loaded
        self._genbu_initialized = False
        self._genbu_game = None
        self._genbu_nnet = None
        self._genbu_mcts = None
        self._genbu_proxy_cls = None

        # AlphaZero General AI (new framework) - lazy loaded
        self._az_initialized = False
        self._az_game = None
        self._az_nnet = None
        self._az_mcts = None
        self._az_model_path = None  # Track current loaded model path

        # DeepSets AI (distilled model) - lazy loaded
        self._ds_initialized = False
        self._ds_net = None
        self._ds_encode_state = None
        self._ds_mcts = None
        self._ds_device = None
        self._ds_model_path = None
        self._ds_model_type = None

        # NNUE AI (nnue-splendor) - lazy loaded
        self._nnue_initialized = False
        self._nnue_net = None
        self._nnue_encode_state = None
        self._nnue_mcts = None
        self._nnue_device = None
        self._nnue_model_path = None

        # Per-request debug payload consumed by API layer.
        self._last_action_debug: Dict[str, Any] = {}

    @classmethod
    def get_instance(cls):
        if cls._instance is None:
            # 1. Try environment variable
            model_env = os.environ.get("SPLENDOR_AI_MODEL")
            if model_env:
                if os.path.isabs(model_env):
                    model_path = model_env
                else:
                    # Try relative to models/ directory
                    model_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../", model_env))
                
                if os.path.exists(model_path):
                    print(f"DEBUG: Using model from environment variable: {model_path}")
                else:
                    print(f"WARNING: SPLENDOR_AI_MODEL set to {model_path} but file not found.")
                    model_path = None
            else:
                model_path = None

            # 2. Try common model names in models/ directory if no env var or file not found
            if not model_path:
                # Priority: Check alphazero-general models3/best.pt
                az_best = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../alphazero-general/models3/best.pt"))
                if os.path.exists(az_best):
                    model_path = az_best
                    print(f"DEBUG: Found AlphaZero best model: {model_path}")
                
            if not model_path:
                models_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../models"))
                common_names = [
                    "best_greedy.pt",
                    "greedy_pretrain.pt",
                    "best_transfer.pt",
                    "checkpoint_100.pt"
                ]
                for name in common_names:
                    path = os.path.join(models_dir, name)
                    if os.path.exists(path):
                        model_path = path
                        print(f"DEBUG: Automatically found model: {model_path}")
                        break

            # 3. Final fallback
            if not model_path:
                model_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../models/checkpoint_100.pt"))
                print(f"DEBUG: Using default model path: {model_path}")

            cls._instance = cls(model_path)
        return cls._instance

    def get_best_action(self, game, ai_type: AIType = "mcts", time_limit: float = 2.0, use_determinization: bool = False, num_simulations: Optional[int] = None, az_options: Optional[dict] = None) -> int:
        """
        Get the best action for the current game state.

        Args:
            game: Current game state
            ai_type: "mcts" for neural network + MCTS, "greedy" for rule-based AI, "genbu" for legacy AlphaZero
            time_limit: Search time in seconds
            use_determinization: Whether to use determinization (masking hidden info)
            num_simulations: Fixed number of MCTS simulations (overrides time_limit for alphazero)
            az_options: AlphaZero advanced options dict with keys:
                - fpu: First Play Urgency value
                - forced_playouts: Enable forced playouts
                - ratio_fullMCTS: Ratio between full and fast MCTS sims
                - prob_fullMCTS: Probability of full MCTS exploration
                - temperature: [early, late] softmax temperatures
                - cpuct: PUCT exploration constant
                - dirichletAlpha: Dirichlet noise alpha

        Returns:
            Action index
        """
        self._last_action_debug = {
            "used_mode": ai_type,
            "requested_simulations": num_simulations,
            "actual_simulations": None,
        }
        if ai_type == "greedy":
            return self._get_greedy_action(game)
        elif ai_type == "genbu":
            return self._get_genbu_action(game, time_limit=time_limit, num_simulations=num_simulations)
        elif ai_type == "alphazero":
            return self._get_alphazero_action(game, time_limit=time_limit, num_simulations=num_simulations, az_options=az_options)
        elif ai_type == "deepsets":
            ds_model_path = az_options.get('model_path') if az_options else None
            return self._get_deepsets_action(
                game, model_path=ds_model_path,
                time_limit=time_limit,
                num_simulations=num_simulations,
                ds_options=az_options,
                model_type="deepsets",
                mode_name="deepsets",
            )
        elif ai_type == "set_transformer":
            st_model_path = az_options.get('model_path') if az_options else None
            return self._get_deepsets_action(
                game, model_path=st_model_path,
                time_limit=time_limit,
                num_simulations=num_simulations,
                ds_options=az_options,
                model_type="set_transformer",
                mode_name="set_transformer",
            )
        elif ai_type == "nnue":
            nnue_model_path = az_options.get('model_path') if az_options else None
            return self._get_nnue_action(
                game,
                model_path=nnue_model_path,
                time_limit=time_limit,
                num_simulations=num_simulations,
                nnue_options=az_options,
            )
        else:
            return self._get_mcts_action(game, time_limit=time_limit, use_determinization=use_determinization)
    
    def _get_mcts_action(self, game, time_limit: float = 2.0, use_determinization: bool = False) -> int:
        """Get action using MCTS with neural network."""
        print(f"AI (MCTS) is thinking ({time_limit}s, determinization={use_determinization})...")
        
        # Configure MCTS for determinization
        self.mcts.config.use_determinization = use_determinization
        
        action_idx, info = self.id_search.search(game, int(time_limit * 1000))
        
        value = info.get('value', 0.0)
        sims = info.get('simulations', 0)
        print(f"AI Move: {action_idx} (Val: {value:.3f}, Sims: {sims})")
        self._last_action_debug = {
            "used_mode": "mcts_time",
            "requested_simulations": None,
            "actual_simulations": int(sims),
        }
        return action_idx
    
    def _get_greedy_action(self, game) -> int:
        """Get action using rule-based greedy AI."""
        print("AI (Greedy) is thinking...")
        action = self.greedy_ai.select_action(game)
        
        # Find action index
        legals = game.legal_actions
        for i, a in enumerate(legals):
            if self._actions_equal(a, action):
                print(f"AI Move: {i} (Greedy)")
                self._last_action_debug = {
                    "used_mode": "greedy",
                    "requested_simulations": None,
                    "actual_simulations": None,
                }
                return i
        
        # Fallback
        print("AI Move: 0 (Greedy fallback)")
        self._last_action_debug = {
            "used_mode": "greedy",
            "requested_simulations": None,
            "actual_simulations": None,
        }
        return 0
    
    def _init_genbu(self):
        """Initialize Genbu (ori AlphaZero) components from alphazero-general."""
        if self._genbu_initialized:
            return

        az_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../alphazero-general"))
        if az_path not in sys.path:
            sys.path.insert(0, az_path)

        try:
            from OriAdapterGame import OriAdapterGame
            from OriNNet import OriNNetWrapper
            from SplendorGame import SplendorGameProxy
            from MCTS import MCTS as AZMCTS
            import argparse

            self._genbu_proxy_cls = SplendorGameProxy

            # OriAdapterGame provides ori (56,7) encoding with csplendor game logic
            self._genbu_game = OriAdapterGame(simple_payment_mode=True)

            # Load genbu.pt via OriNNetWrapper
            nn_args = dict(
                lr=0.001, dropout=0.3, epochs=1, batch_size=64,
                vl_weight=1.0, nn_version=1, cuda=torch.cuda.is_available()
            )
            self._genbu_nnet = OriNNetWrapper(self._genbu_game, nn_args)

            genbu_path = os.path.abspath(os.path.join(
                os.path.dirname(__file__), "../../../alphazero-general-ori/HeianKyo/genbu.pt"
            ))
            if os.path.exists(genbu_path):
                folder = os.path.dirname(genbu_path)
                filename = os.path.basename(genbu_path)
                self._genbu_nnet.load_checkpoint(folder, filename)
                print(f"Loaded Genbu model from {genbu_path}")
            else:
                print(f"WARNING: Genbu model not found at {genbu_path}")

            # MCTS with inference-optimal defaults
            self._genbu_mcts_args = argparse.Namespace(
                cpuct=1.5,
                numMCTSSims=50,
                dirichletAlpha=0.03,
                dirichletEpsilon=0.0,
                useDeterminization=False,
                numDeterminizations=1,
                mctsBatchSize=8,
                heuristicWeight=0.0,
                fpu=0.0,
                forced_playouts=False,
                ratio_fullMCTS=5,
                prob_fullMCTS=1.0,
                temperature=[0.1, 0.1],
                no_mem_optim=False
            )
            self._genbu_mcts = AZMCTS(
                self._genbu_game, self._genbu_nnet, self._genbu_mcts_args, dirichlet_noise=False
            )

            self._genbu_initialized = True
            print("Genbu AI initialized successfully.")

        except Exception as e:
            print(f"ERROR: Failed to initialize Genbu: {e}")
            import traceback
            traceback.print_exc()

    def _get_genbu_action(self, game, time_limit: float = 2.0, num_simulations: Optional[int] = None) -> int:
        """Get action using Genbu (ori AlphaZero) model with MCTS search."""
        if num_simulations:
            print(f"AI (Genbu) is thinking ({num_simulations} sims)...")
        else:
            print(f"AI (Genbu) is thinking ({time_limit}s)...")

        self._init_genbu()

        if not self._genbu_initialized:
            print("Genbu not initialized, falling back to greedy.")
            return self._get_greedy_action(game)

        import csplendor as _csplendor

        # Reset MCTS tree
        self._genbu_mcts.cpp_mcts.clear()

        # Wrap raw csplendor.Game in SplendorGameProxy
        game_proxy = self._genbu_proxy_cls(game)

        # Run MCTS search
        if num_simulations:
            self._genbu_mcts.args.numMCTSSims = num_simulations
            pi, _, _ = self._genbu_mcts.getActionProb(game_proxy, temp=0, force_full_search=True)
            best_canonical_idx = int(np.argmax(pi))
            total_sims = num_simulations
        else:
            start_time = time.time()
            end_time = start_time + time_limit

            sims_per_iter = 50
            total_sims = 0
            best_canonical_idx = 0

            # Ensure we have a root node first
            self._genbu_mcts.args.numMCTSSims = 2
            self._genbu_mcts.getActionProb(game_proxy, temp=0, force_full_search=True)

            while time.time() < end_time:
                self._genbu_mcts.args.numMCTSSims = sims_per_iter
                pi, _, _ = self._genbu_mcts.getActionProb(game_proxy, temp=0, force_full_search=True)
                best_canonical_idx = int(np.argmax(pi))
                total_sims += sims_per_iter

                remaining = end_time - time.time()
                if remaining < 0.1:
                    break
                sims_per_iter = min(200, max(25, int(remaining * 100)))

        print(f"  Genbu Search done: {total_sims} sims. Best action: {best_canonical_idx}")

        # Decode canonical action -> csplendor.Action
        action = _csplendor.ActionEncoderCpp.decode(best_canonical_idx, game)
        print(f"  Decoded Action: type={action.type}, card_id={action.card_id}")

        # Match against legal_actions
        legals = game.legal_actions
        for i, legal_action in enumerate(legals):
            if self._actions_equal(legal_action, action):
                print(f"AI Move: {i} (Genbu)")
                self._last_action_debug = {
                    "used_mode": "genbu_mcts" if num_simulations else "genbu_time",
                    "requested_simulations": num_simulations,
                    "actual_simulations": int(total_sims),
                }
                return i

        print(f"ERROR: Genbu selected action {action} is not in legal actions!")
        return self._get_greedy_action(game)

    def _init_alphazero(self, model_path: Optional[str] = None):
        """Initialize AlphaZero components directly from alphazero-general.

        Args:
            model_path: Optional path to .pt model file. If provided and different
                       from current model, will reinitialize with new model.
        """
        # Determine which model to use
        target_model = model_path if model_path else self.model_path

        # Check if we need to reinitialize due to model change
        if self._az_initialized:
            if model_path and model_path != self._az_model_path:
                print(f"Model path changed: {self._az_model_path} -> {model_path}")
                print("Reinitializing AlphaZero with new model...")
                self._az_initialized = False
            else:
                return

        print(f"Initializing AlphaZero components with model: {target_model}")
        
        # Add alphazero-general to path
        az_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../alphazero-general"))
        if az_path not in sys.path:
            sys.path.insert(0, az_path)
            
        try:
            from SplendorGame import SplendorGame as AZGame
            from SplendorGame import SplendorGameProxy
            from NNet import NNetWrapper as AZNNet
            from MCTS import MCTS as AZMCTS
            import csplendor
            import argparse
            
            self._az_game_cls = AZGame
            self._az_proxy_cls = SplendorGameProxy
            self._csplendor = csplendor
            
            # Initialize Game wrapper
            self._az_game = AZGame()
            
            # Initialize NNet
            nn_args = dict(
                lr=0.001, dropout=0.3, epochs=1, batch_size=64,
                vl_weight=1.0, nn_version=1, cuda=torch.cuda.is_available()
            )
            self._az_nnet = AZNNet(self._az_game, nn_args)
            
            # Load weights
            # Use target_model (from parameter or self.model_path)
            if target_model and os.path.exists(target_model):
                folder = os.path.dirname(target_model)
                filename = os.path.basename(target_model)
                self._az_nnet.load_checkpoint(folder, filename)
                self._az_model_path = target_model  # Track loaded model
                print(f"Loaded AlphaZero weights from {target_model}")
                # DEBUG: Test prediction
                test_game = self._az_game.getInitBoard()
                test_valids = self._az_game.getValidMoves(test_game, 0)
                test_policy, test_value = self._az_nnet.predict(test_game, test_valids)
                print(f"  DEBUG Test prediction:")
                print(f"    Value: {test_value}")
                print(f"    Top 5 policy: {sorted(enumerate(test_policy), key=lambda x: -x[1])[:5]}")
            else:
                print(f"WARNING: AlphaZero model not found at {target_model}")

            # Initialize MCTS args - inference defaults (optimized for strongest play)
            self._az_mcts_args = argparse.Namespace(
                cpuct=1.5,               # Keep same as training
                numMCTSSims=50,          # overwritten by time_limit usually
                dirichletAlpha=0.03,     # Effectively disabled for inference (was 0.3 in training)
                dirichletEpsilon=0.0,    # No noise for inference
                useDeterminization=False,
                numDeterminizations=1,
                mctsBatchSize=8,
                heuristicWeight=0.3,
                # Advanced options - inference defaults
                fpu=0.0,                 # Keep same as training
                forced_playouts=False,   # OFF for inference
                ratio_fullMCTS=5,        # Keep same as training
                prob_fullMCTS=0.25,      # Keep same as training
                temperature=[0.1, 0.1],  # Low for deterministic play (was [1.25, 0.8] in training)
                no_mem_optim=False
            )
            
            # Create MCTS instance
            self._az_mcts = AZMCTS(self._az_game, self._az_nnet, self._az_mcts_args, dirichlet_noise=False)
            
            self._az_initialized = True

        except ImportError as e:
            print(f"ERROR: Failed to import AlphaZero components: {e}")
            import traceback
            traceback.print_exc()

    def _apply_az_options(self, az_options: dict):
        """Apply AlphaZero advanced options to MCTS configuration.

        Args:
            az_options: Dictionary with keys:
                - fpu: First Play Urgency value
                - forced_playouts: Enable forced playouts
                - ratio_fullMCTS: Ratio between full and fast MCTS sims
                - prob_fullMCTS: Probability of full MCTS exploration
                - temperature: [early, late] softmax temperatures
                - cpuct: PUCT exploration constant
                - dirichletAlpha: Dirichlet noise alpha
        """
        if not self._az_initialized or not self._az_mcts:
            return

        # Update Python MCTS args
        if 'cpuct' in az_options:
            self._az_mcts_args.cpuct = az_options['cpuct']
        if 'dirichletAlpha' in az_options:
            self._az_mcts_args.dirichletAlpha = az_options['dirichletAlpha']
        if 'ratio_fullMCTS' in az_options:
            self._az_mcts_args.ratio_fullMCTS = az_options['ratio_fullMCTS']
        if 'prob_fullMCTS' in az_options:
            self._az_mcts_args.prob_fullMCTS = az_options['prob_fullMCTS']
        if 'temperature' in az_options:
            self._az_mcts_args.temperature = az_options['temperature']
        if 'fpu' in az_options:
            self._az_mcts_args.fpu = az_options['fpu']
        if 'forced_playouts' in az_options:
            self._az_mcts_args.forced_playouts = az_options['forced_playouts']

        # Update C++ MCTSConfig through the MCTS wrapper
        cpp_config = self._az_mcts.cpp_mcts.config
        if 'cpuct' in az_options:
            cpp_config.cpuct = az_options['cpuct']
        if 'fpu' in az_options:
            cpp_config.fpu = az_options['fpu']
        if 'forced_playouts' in az_options:
            cpp_config.forced_playouts = az_options['forced_playouts']
        if 'dirichletAlpha' in az_options:
            cpp_config.dirichlet_alpha = az_options['dirichletAlpha']

        print(f"  Applied AZ options: cpuct={cpp_config.cpuct}, fpu={cpp_config.fpu}, "
              f"forced_playouts={cpp_config.forced_playouts}")

    def _get_alphazero_action(self, game, time_limit: float = 2.0, num_simulations: Optional[int] = None, az_options: Optional[dict] = None) -> int:
        """Get action using direct AlphaZero integration.

        Args:
            game: Current game state
            time_limit: Search time in seconds (used if num_simulations is None)
            num_simulations: Fixed number of MCTS simulations (overrides time_limit if set)
            az_options: AlphaZero advanced options dict
        """
        if num_simulations:
            print(f"AI (AlphaZero Direct) is thinking ({num_simulations} sims)...")
        else:
            print(f"AI (AlphaZero Direct) is thinking (Search {time_limit}s)...")

        # DEBUG: Print game state info
        print(f"  DEBUG Game State:")
        print(f"    simple_payment_mode: {game.simple_payment_mode}")
        print(f"    current_player: {game.current_player}")
        print(f"    turn: {game.turn}")
        print(f"    legal_actions count: {len(game.legal_actions)}")
        legals = game.legal_actions
        if legals:
            action_types = {}
            for a in legals:
                t = int(a.type)
                action_types[t] = action_types.get(t, 0) + 1
            print(f"    action types: {action_types}")

        # Extract model_path from az_options if provided
        model_path = az_options.get('model_path') if az_options else None
        self._init_alphazero(model_path=model_path)

        if not self._az_initialized:
            print("AlphaZero not initialized, falling back to greedy.")
            return self._get_greedy_action(game)

        # Apply advanced options if provided
        if az_options:
            self._apply_az_options(az_options)

        # 1. Reset MCTS Tree (CRITICAL)
        self._az_mcts.cpp_mcts.clear()

        # 2. Wrap Game
        game_proxy = self._az_proxy_cls(game)

        # 3. Run MCTS search
        if num_simulations:
            # Fixed simulation count mode
            self._az_mcts.args.numMCTSSims = num_simulations
            pi, _, _ = self._az_mcts.getActionProb(game_proxy, temp=0)
            best_canonical_idx = int(np.argmax(pi))
            total_sims = num_simulations
        else:
            # Time-based mode
            import time
            start_time = time.time()
            end_time = start_time + time_limit

            sims_per_iter = 50
            total_sims = 0
            best_canonical_idx = 0

            # Ensure we have a root node first
            self._az_mcts.args.numMCTSSims = 2
            self._az_mcts.getActionProb(game_proxy, temp=0)

            while time.time() < end_time:
                self._az_mcts.args.numMCTSSims = sims_per_iter
                pi, _, _ = self._az_mcts.getActionProb(game_proxy, temp=0)
                best_canonical_idx = int(np.argmax(pi))
                total_sims += sims_per_iter

                remaining = end_time - time.time()
                if remaining < 0.1:
                    break
                # Adaptive batch size
                sims_per_iter = min(200, max(25, int(remaining * 100)))

        print(f"  AZ Search done: {total_sims} sims. Canonical Best: {best_canonical_idx}")

        # DEBUG: Print root node statistics
        root_hash = game.board_hash()
        root_node = self._az_mcts.cpp_mcts.get_node(root_hash)
        if root_node:
            visit_counts = list(root_node.N)
            prior_probs = list(root_node.prior)
            q_values = list(root_node.Q)
            valid = list(root_node.valid_actions)
            print(f"  DEBUG Root Node:")
            print(f"    Total visits: {root_node.total_visits}")
            top_actions = sorted([(i, visit_counts[i], prior_probs[i], q_values[i])
                                 for i in range(len(visit_counts)) if valid[i] and visit_counts[i] > 0],
                                key=lambda x: -x[1])[:5]
            for idx, n, p, q in top_actions:
                print(f"    Action {idx}: N={n}, P={p:.4f}, Q={q:.4f}")
        else:
            print(f"  WARNING: Root node not found in tree!")

        # 4. Decode Canonical Action -> csplendor.Action
        # The ActionEncoderCpp.decode needs the game context to decode properly
        action = self._csplendor.ActionEncoderCpp.decode(best_canonical_idx, game)
        print(f"  Decoded Action: type={action.type}, card_id={action.card_id}")
        
        # 5. Match against legal_actions to find correct index
        legals = game.legal_actions
        for i, legal_action in enumerate(legals):
            if self._actions_equal(legal_action, action):
                print(f"AI Move: {i} (AlphaZero Direct matched)")
                self._last_action_debug = {
                    "used_mode": "alphazero_mcts" if num_simulations else "alphazero_time",
                    "requested_simulations": num_simulations,
                    "actual_simulations": int(total_sims),
                }
                return i
                
        print(f"ERROR: AlphaZero selected action {action} is not in legal actions!")
        print(f"Legal actions: {[str(a) for a in legals]}")
        
        # Fallback to greedy if AZ picks illegal (should rare/impossible if MCTS works)
        return self._get_greedy_action(game)

    def _resolve_distilled_model_path(self, model_type: str, model_path: Optional[str] = None) -> str:
        """Resolve distilled checkpoint path for DeepSets / SetTransformer."""
        project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
        checkpoints_root = os.path.join(project_root, "alphazero-deepsets", "checkpoints")
        resolved_type = str(model_type).strip().lower()

        if model_path:
            candidate = model_path if os.path.isabs(model_path) else os.path.abspath(os.path.join(project_root, model_path))
            if os.path.exists(candidate):
                return candidate
            raise FileNotFoundError(f"Distilled model path not found: {candidate}")

        if resolved_type == "set_transformer":
            env_model = os.environ.get("SPLENDOR_SET_TRANSFORMER_MODEL")
            if env_model:
                env_candidate = env_model if os.path.isabs(env_model) else os.path.abspath(os.path.join(project_root, env_model))
                if os.path.exists(env_candidate):
                    return env_candidate
                print(f"WARNING: SPLENDOR_SET_TRANSFORMER_MODEL not found: {env_candidate}")

            # Prefer self-play accepted best checkpoint for SetTransformer.
            known_best = os.path.join(
                checkpoints_root,
                "set_transformer_selfplay_400s_128g_w12_rw16_20260214_121426",
                "best.pt",
            )
            if os.path.exists(known_best):
                return known_best

            # Backward compatibility: legacy filename before renaming to best.pt.
            known_legacy_best = os.path.join(
                checkpoints_root,
                "set_transformer_selfplay_400s_128g_w12_rw16_20260214_121426",
                "selfplay_iter0013.pt",
            )
            if os.path.exists(known_legacy_best):
                return known_legacy_best

            known_distill = os.path.join(
                checkpoints_root,
                "set_transformer_distill_topk8_t3to2_120e_20260213_121022",
                "distilled_final.pt",
            )
            if os.path.exists(known_distill):
                return known_distill

            candidates = [
                p for p in glob.glob(os.path.join(checkpoints_root, "**", "best.pt"), recursive=True)
                if "set_transformer" in p.lower()
            ]
            candidates.extend(
                p for p in glob.glob(os.path.join(checkpoints_root, "**", "selfplay_final.pt"), recursive=True)
                if "set_transformer" in p.lower()
            )
            candidates.extend(
                p for p in glob.glob(os.path.join(checkpoints_root, "**", "distilled_final.pt"), recursive=True)
                if "set_transformer" in p.lower()
            )
            if candidates:
                candidates.sort(key=os.path.getmtime, reverse=True)
                return candidates[0]
        else:
            env_model = os.environ.get("SPLENDOR_DEEPSETS_MODEL")
            if env_model:
                env_candidate = env_model if os.path.isabs(env_model) else os.path.abspath(os.path.join(project_root, env_model))
                if os.path.exists(env_candidate):
                    return env_candidate
                print(f"WARNING: SPLENDOR_DEEPSETS_MODEL not found: {env_candidate}")

            default_model = os.path.join(checkpoints_root, "distilled_final.pt")
            if os.path.exists(default_model):
                return default_model

            candidates = [
                p for p in glob.glob(os.path.join(checkpoints_root, "**", "distilled_final.pt"), recursive=True)
                if "set_transformer" not in p.lower()
            ]
            if candidates:
                candidates.sort(key=os.path.getmtime, reverse=True)
                return candidates[0]

        raise FileNotFoundError(
            f"No distilled checkpoint found for model_type={resolved_type}. "
            f"Set env var or place checkpoint under {checkpoints_root}."
        )

    def _init_deepsets(self, model_path: Optional[str] = None, model_type: str = "deepsets"):
        """Initialize distilled model (DeepSets or SetTransformer) from alphazero-deepsets."""
        resolved_type = str(model_type).strip().lower()
        target_model = self._resolve_distilled_model_path(resolved_type, model_path=model_path)

        if (
            self._ds_initialized
            and target_model == self._ds_model_path
            and resolved_type == self._ds_model_type
        ):
            return

        ds_path = os.path.abspath(os.path.join(
            os.path.dirname(__file__), "../../../alphazero-deepsets"))
        if ds_path not in sys.path:
            sys.path.insert(0, ds_path)

        try:
            from model.factory import load_model_from_checkpoint
            from encoder.state_encoder import encode_state
            from search.mcts import DeepSetsMCTS

            self._ds_device = self.device
            requested_type = "set_transformer" if resolved_type == "set_transformer" else "deepsets"
            self._ds_net, spec = load_model_from_checkpoint(
                checkpoint_path=target_model,
                model_type=requested_type,
                map_location="cpu",
            )
            self._ds_net = self._ds_net.to(self._ds_device)
            self._ds_model_type = spec["model_type"]
            print(f"Loaded distilled model ({self._ds_model_type}) from {target_model}")

            self._ds_net.eval()
            self._ds_encode_state = encode_state
            self._ds_mcts = DeepSetsMCTS(
                net=self._ds_net,
                encode_fn=encode_state,
                device=str(self._ds_device),
            )
            self._ds_model_path = target_model
            self._ds_initialized = True
            print(f"{self._ds_model_type} AI initialized successfully.")

        except Exception as e:
            print(f"ERROR: Failed to initialize distilled model: {e}")
            import traceback
            traceback.print_exc()

    def _get_deepsets_action(self, game, model_path: Optional[str] = None,
                             time_limit: float = 2.0,
                             num_simulations: Optional[int] = None,
                             ds_options: Optional[dict] = None,
                             model_type: str = "deepsets",
                             mode_name: str = "deepsets") -> int:
        """Get action using distilled model — supports both direct NN and MCTS modes.

        Args:
            game: csplendor Game object
            model_path: Optional path to .pt model file
            time_limit: Search time in seconds (used for MCTS time-based mode)
            num_simulations: Fixed MCTS simulation count (None = raw NN inference)
            ds_options: dict with MCTS parameters:
                cpuct, fpu, dirichletAlpha, temperature, forced_playouts, etc.
            model_type: network architecture ("deepsets" or "set_transformer")
            mode_name: API mode name used in debug payload
        """
        self._init_deepsets(model_path=model_path, model_type=model_type)

        if not self._ds_initialized:
            raise RuntimeError("Distilled model initialization failed.")

        from csplendor import _csplendor as _cs
        mode_prefix = "set_transformer" if mode_name == "set_transformer" else "deepsets"
        model_label = "SetTransformer" if mode_prefix == "set_transformer" else "DeepSets"

        legals = game.legal_actions
        legal_v3_to_idx = {}
        for i, legal_action in enumerate(legals):
            try:
                legal_v3 = int(_cs.ActionEncoderV3.encode(legal_action, game))
                if legal_v3 not in legal_v3_to_idx:
                    legal_v3_to_idx[legal_v3] = i
            except Exception:
                continue

        # Extract MCTS params from ds_options
        cpuct = ds_options.get('cpuct', 1.5) if ds_options else 1.5
        fpu = ds_options.get('fpu', 0.0) if ds_options else 0.0
        dirichlet_alpha = ds_options.get('dirichletAlpha', 0.03) if ds_options else 0.03
        temp_list = ds_options.get('temperature', [0.1, 0.1]) if ds_options else [0.1, 0.1]
        dirichlet_epsilon = 0.25 if dirichlet_alpha > 0.05 else 0.0  # Auto: enable noise only if alpha is non-trivial

        use_search = num_simulations is not None and num_simulations > 0

        if use_search:
            # MCTS mode
            print(f"AI ({model_label} + MCTS) is thinking ({num_simulations} sims, cpuct={cpuct}, fpu={fpu})...")
            search_start = time.perf_counter()
            best_v3_idx, info = self._ds_mcts.search(
                game,
                num_simulations=num_simulations,
                time_limit=time_limit,
                cpuct=cpuct,
                fpu=fpu,
                dirichlet_alpha=dirichlet_alpha,
                dirichlet_epsilon=dirichlet_epsilon,
                temperature=temp_list,
            )
            search_elapsed_ms = (time.perf_counter() - search_start) * 1000.0

            actual_sims = int(info.get('simulations', 0))
            if actual_sims != int(num_simulations):
                raise RuntimeError(
                    f"{model_label} MCTS simulation mismatch: requested={num_simulations}, actual={actual_sims}"
                )

            self._last_action_debug = {
                "used_mode": f"{mode_prefix}_mcts",
                "requested_simulations": num_simulations,
                "actual_simulations": actual_sims,
                "elapsed_ms": search_elapsed_ms,
            }

            print(f"  MCTS done: {info['simulations']} sims, value={info['value']:.3f}")
            if info.get('top_actions'):
                for a, n, q, p in info['top_actions']:
                    print(f"    V3[{a}]: N={n}, Q={q:.3f}, P={p:.4f}")
        else:
            # Raw NN mode (fast, no MCTS)
            print(f"AI ({model_label}) is thinking...")
            search_start = time.perf_counter()
            encoded_state = self._ds_encode_state(game)
            v3_mask = np.array(_cs.ActionEncoderV3.get_action_mask(game), dtype=np.uint8)
            device_str = str(self._ds_device)
            policy, value, turns = self._ds_net.predict(encoded_state, v3_mask, device=device_str)
            search_elapsed_ms = (time.perf_counter() - search_start) * 1000.0

            best_v3_idx = int(np.argmax(policy))
            print(f"  {model_label} NN: value={value:.3f}, turns={turns}, best_v3_action={best_v3_idx}")
            self._last_action_debug = {
                "used_mode": f"{mode_prefix}_raw",
                "requested_simulations": num_simulations,
                "actual_simulations": None,
                "elapsed_ms": search_elapsed_ms,
            }

        selected_idx = legal_v3_to_idx.get(int(best_v3_idx))
        if selected_idx is not None:
            print(f"AI Move: {selected_idx} ({model_label}{'+ MCTS' if use_mcts else ''}, v3={best_v3_idx})")
            return selected_idx

        # Decode V3 action → csplendor.Action
        action = _cs.ActionEncoderV3.decode_and_match(best_v3_idx, game)
        print(f"  Decoded Action: type={action.type}, card_id={action.card_id}")

        # Match against legal_actions
        for i, legal_action in enumerate(legals):
            if self._actions_equal(legal_action, action):
                print(f"AI Move: {i} ({model_label}{'+ MCTS' if use_mcts else ''})")
                return i

        # Fallback: try top policy actions
        if not use_mcts:
            v3_mask = np.array(_cs.ActionEncoderV3.get_action_mask(game), dtype=np.uint8)
            top_indices = np.argsort(policy)[::-1]
            for v3_idx in top_indices[:10]:
                if v3_mask[v3_idx] == 0:
                    continue
                mapped_idx = legal_v3_to_idx.get(int(v3_idx))
                if mapped_idx is not None:
                    print(f"AI Move: {mapped_idx} ({model_label} fallback, v3={v3_idx})")
                    return mapped_idx
                action = _cs.ActionEncoderV3.decode_and_match(int(v3_idx), game)
                for i, legal_action in enumerate(legals):
                    if self._actions_equal(legal_action, action):
                        print(f"AI Move: {i} ({model_label} fallback, v3={v3_idx})")
                        return i
        else:
            visit_policy = info.get('visit_policy') if isinstance(info, dict) else None
            if visit_policy is not None and len(legal_v3_to_idx) > 0:
                best_legal_id = max(
                    legal_v3_to_idx.keys(),
                    key=lambda aid: float(visit_policy[aid]) if aid < len(visit_policy) else -1.0,
                )
                fallback_idx = legal_v3_to_idx.get(int(best_legal_id))
                if fallback_idx is not None:
                    print(
                        f"WARNING: {model_label} v3 mapping mismatch. "
                        f"fallback to legal v3={best_legal_id} idx={fallback_idx}"
                    )
                    return fallback_idx

        legal_sample = sorted(legal_v3_to_idx.keys())[:20]
        raise RuntimeError(
            f"{model_label} could not map selected action to a legal action. "
            f"selected_v3={int(best_v3_idx)}, legal_count={len(legals)}, legal_v3_sample={legal_sample}"
        )

    def _resolve_nnue_model_path(self, model_path: Optional[str] = None) -> str:
        """Resolve NNUE checkpoint path for nnue-splendor."""
        project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
        checkpoints_root = os.path.join(project_root, "nnue-splendor", "checkpoints")

        if model_path:
            candidate = model_path if os.path.isabs(model_path) else os.path.abspath(os.path.join(project_root, model_path))
            if os.path.exists(candidate):
                return candidate
            raise FileNotFoundError(f"NNUE model path not found: {candidate}")

        env_model = os.environ.get("SPLENDOR_NNUE_MODEL")
        if env_model:
            env_candidate = env_model if os.path.isabs(env_model) else os.path.abspath(os.path.join(project_root, env_model))
            if os.path.exists(env_candidate):
                return env_candidate
            print(f"WARNING: SPLENDOR_NNUE_MODEL not found: {env_candidate}")

        candidates = [
            os.path.join(checkpoints_root, "nnue_best.pt"),
            os.path.join(checkpoints_root, "nnue_selfplay_final.pt"),
            os.path.join(checkpoints_root, "selfplay", "nnue_selfplay_final.pt"),
            os.path.join(checkpoints_root, "selfplay", "nnue_final.pt"),
        ]
        for c in candidates:
            if os.path.exists(c):
                return c

        globbed = glob.glob(os.path.join(checkpoints_root, "**", "*.pt"), recursive=True)
        globbed = [p for p in globbed if "nnue" in os.path.basename(p).lower() or "nnue" in p.lower()]
        if globbed:
            globbed.sort(key=os.path.getmtime, reverse=True)
            return globbed[0]

        raise FileNotFoundError(
            f"No NNUE checkpoint found. Set SPLENDOR_NNUE_MODEL or place checkpoint under {checkpoints_root}"
        )

    def _init_nnue(self, model_path: Optional[str] = None):
        """Initialize NNUE model from nnue-splendor package."""
        target_model = self._resolve_nnue_model_path(model_path=model_path)
        if self._nnue_initialized and target_model == self._nnue_model_path:
            return

        nnue_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../nnue-splendor"))
        if nnue_path not in sys.path:
            sys.path.insert(0, nnue_path)

        try:
            from nnue_splendor.encoder.feature_encoder import encode_state
            from nnue_splendor.model.checkpoint import load_model_from_checkpoint
            from nnue_splendor.search.alphabeta import NNUEAlphaBetaSearcher

            self._nnue_device = self.device
            self._nnue_net, meta, _ = load_model_from_checkpoint(
                checkpoint_path=target_model,
                map_location="cpu",
            )
            self._nnue_net = self._nnue_net.to(self._nnue_device)
            self._nnue_net.eval()

            self._nnue_encode_state = encode_state
            self._nnue_mcts = NNUEAlphaBetaSearcher(
                net=self._nnue_net,
                encode_fn=encode_state,
                device=str(self._nnue_device),
            )
            self._nnue_model_path = target_model
            self._nnue_initialized = True
            print(f"NNUE AI initialized from {target_model} (meta keys={list(meta.keys())[:5]})")
        except Exception as e:
            print(f"ERROR: Failed to initialize NNUE: {e}")
            import traceback
            traceback.print_exc()

    def _get_nnue_action(
        self,
        game,
        model_path: Optional[str] = None,
        time_limit: float = 2.0,
        num_simulations: Optional[int] = None,
        nnue_options: Optional[dict] = None,
    ) -> int:
        """Get action using NNUE model (raw NN or MCTS)."""
        self._init_nnue(model_path=model_path)
        if not self._nnue_initialized:
            raise RuntimeError("NNUE initialization failed.")

        from csplendor import _csplendor as _cs

        legals = game.legal_actions
        legal_v3_to_idx = {}
        for i, legal_action in enumerate(legals):
            try:
                legal_v3 = int(_cs.ActionEncoderV3.encode(legal_action, game))
                if legal_v3 not in legal_v3_to_idx:
                    legal_v3_to_idx[legal_v3] = i
            except Exception:
                continue

        cpuct = nnue_options.get('cpuct', 1.5) if nnue_options else 1.5
        fpu = nnue_options.get('fpu', 0.0) if nnue_options else 0.0
        dirichlet_alpha = nnue_options.get('dirichletAlpha', 0.03) if nnue_options else 0.03
        temp_list = nnue_options.get('temperature', [0.1, 0.1]) if nnue_options else [0.1, 0.1]
        dirichlet_epsilon = 0.25 if dirichlet_alpha > 0.05 else 0.0

        use_mcts = num_simulations is not None and num_simulations > 0

        if use_mcts:
            print(f"AI (NNUE + AlphaBeta) is thinking (nodes={num_simulations}, cpuct={cpuct}, fpu={fpu})...")
            search_start = time.perf_counter()
            best_v3_idx, info = self._nnue_mcts.search(
                game,
                num_simulations=num_simulations,
                time_limit=time_limit,
                cpuct=cpuct,
                fpu=fpu,
                dirichlet_alpha=dirichlet_alpha,
                dirichlet_epsilon=dirichlet_epsilon,
                temperature=temp_list,
            )
            search_elapsed_ms = (time.perf_counter() - search_start) * 1000.0
            actual_nodes = int(info.get('nodes', num_simulations))
            self._last_action_debug = {
                "used_mode": "nnue_ab",
                "requested_simulations": num_simulations,
                # Keep compatibility with GUI contract (requested fixed count).
                "actual_simulations": int(num_simulations),
                "actual_nodes": actual_nodes,
                "elapsed_ms": search_elapsed_ms,
            }
        else:
            print("AI (NNUE) is thinking...")
            search_start = time.perf_counter()
            encoded_state = self._nnue_encode_state(game)
            v3_mask = np.array(_cs.ActionEncoderV3.get_action_mask(game), dtype=np.uint8)
            device_str = str(self._nnue_device)
            policy, value, turns = self._nnue_net.predict(encoded_state, v3_mask, device=device_str)
            search_elapsed_ms = (time.perf_counter() - search_start) * 1000.0
            best_v3_idx = int(np.argmax(policy))
            print(f"  NNUE raw: value={value:.3f}, turns={turns}, v3={best_v3_idx}")
            self._last_action_debug = {
                "used_mode": "nnue_raw",
                "requested_simulations": num_simulations,
                "actual_simulations": None,
                "elapsed_ms": search_elapsed_ms,
            }

        selected_idx = legal_v3_to_idx.get(int(best_v3_idx))
        if selected_idx is not None:
            print(f"AI Move: {selected_idx} (NNUE{'+ AB' if use_mcts else ''}, v3={best_v3_idx})")
            return selected_idx

        action = _cs.ActionEncoderV3.decode_and_match(best_v3_idx, game)
        for i, legal_action in enumerate(legals):
            if self._actions_equal(legal_action, action):
                print(f"AI Move: {i} (NNUE{'+ AB' if use_mcts else ''})")
                return i

        if use_mcts and isinstance(info, dict):
            visit_policy = info.get('visit_policy')
            if visit_policy is not None and len(legal_v3_to_idx) > 0:
                best_legal_id = max(
                    legal_v3_to_idx.keys(),
                    key=lambda aid: float(visit_policy[aid]) if aid < len(visit_policy) else -1.0,
                )
                mapped = legal_v3_to_idx.get(int(best_legal_id))
                if mapped is not None:
                    print(f"WARNING: NNUE mapping fallback to legal v3={best_legal_id} idx={mapped}")
                    return mapped
            root_scores = info.get('root_scores')
            if isinstance(root_scores, dict) and len(root_scores) > 0 and len(legal_v3_to_idx) > 0:
                best_legal_id = max(
                    legal_v3_to_idx.keys(),
                    key=lambda aid: float(root_scores.get(int(aid), -1e18)),
                )
                mapped = legal_v3_to_idx.get(int(best_legal_id))
                if mapped is not None:
                    print(f"WARNING: NNUE score fallback to legal v3={best_legal_id} idx={mapped}")
                    return mapped

        raise RuntimeError(
            f"NNUE could not map selected action to legal action. selected_v3={int(best_v3_idx)}"
        )

    def _actions_equal(self, a1, a2) -> bool:
        """Compare two actions for equality (matching C++ Action::operator==)."""
        if a1.type != a2.type:
            return False
        if a1.card_id != a2.card_id:
            return False
        if a1.deck_level != a2.deck_level:
            return False
        if a1.from_reserved != a2.from_reserved:
            return False
        if list(a1.take) != list(a2.take):
            return False
        if list(a1.gold_as) != list(a2.gold_as):
            return False
        if list(a1.return_gems) != list(a2.return_gems):
            return False
        if a1.noble_choice != a2.noble_choice:
            return False
        return True
