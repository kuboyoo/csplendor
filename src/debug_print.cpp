#include "cli_utils.h"
#include "game.h"
#include <iostream>

int main() {
  // Initialize game with a seed for reproducibility
  Game game(42);

  // Print initial board
  cli::print_board(game.board);

  // Print legal actions
  auto actions = game.legal_actions();
  cli::print_legal_actions(actions);

  if (actions.empty()) {
    std::cout << "No legal actions available!" << std::endl;
    return 0;
  }

  // Apply a random legal action (e.g., the first one)
  std::cout << "\nApplying action: " << actions[0].to_string() << std::endl;
  game.apply(actions[0]);

  // Print board again
  cli::print_board(game.board);

  // Print legal actions again
  actions = game.legal_actions();
  cli::print_legal_actions(actions);

  return 0;
}
