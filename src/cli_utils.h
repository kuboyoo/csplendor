#ifndef CSPLENDOR_CLI_UTILS_H
#define CSPLENDOR_CLI_UTILS_H

#include "action.h"
#include "board.h"
#include "types.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace cli {

// ANSI Color Codes
const std::string RESET = "\033[0m";
const std::string BOLD = "\033[1m";
const std::string DIM = "\033[2m";

const std::string BG_WHITE =
    "\033[107m\033[30m"; // Light White Background, Black Text
const std::string BG_BLUE =
    "\033[104m\033[37m"; // Light Blue Background, White Text
const std::string BG_GREEN =
    "\033[102m\033[30m"; // Light Green Background, Black Text
const std::string BG_RED =
    "\033[101m\033[30m"; // Light Red Background, Black Text
const std::string BG_BLACK = "\033[40m\033[37m"; // Black Background, White Text
const std::string BG_GOLD =
    "\033[103m\033[30m"; // Light Yellow(Gold) Background, Black Text

inline std::string gem_bg(int gem_type) {
  switch (gem_type) {
  case DIAMOND:
    return BG_WHITE;
  case SAPPHIRE:
    return BG_BLUE;
  case EMERALD:
    return BG_GREEN;
  case RUBY:
    return BG_RED;
  case ONYX:
    return BG_BLACK;
  case GOLD:
    return BG_GOLD;
  default:
    return RESET;
  }
}

inline void print_card_line(int8_t card_id, int line) {
  if (card_id == -1) {
    std::cout << "         ";
    return;
  }
  const auto &card = get_card(card_id);
  std::string bg = gem_bg(card.bonus);

  std::cout << bg;
  if (line == 0) {
    printf("      %d  ", card.points);
  } else {
    std::vector<int> cost_indices;
    for (int i = 0; i < 5; ++i)
      if (card.cost[i] > 0)
        cost_indices.push_back(i);

    if (line - 1 < cost_indices.size()) {
      int c_idx = cost_indices[line - 1];
      std::cout << " " << gem_bg(c_idx) << " " << (int)card.cost[c_idx] << " "
                << bg << "     ";
    } else {
      std::cout << "         ";
    }
  }
  std::cout << RESET;
}

inline void print_board(const Board &board) {
  std::cout << "\n========== round " << board.turn << " PLAYER "
            << (int)board.current_player << "'s turn ==========" << std::endl;
  for (int p = 0; p < 2; ++p) {
    std::cout << BOLD << " P" << p << RESET << ": "
              << (int)board.players[p].points << " points  ";
  }
  std::cout << std::endl;

  std::cout << BOLD << " Nobles:  " << RESET;
  for (uint8_t n_id : board.nobles) {
    const auto &noble = get_noble(n_id);
    std::cout << "< 3 pts ";
    for (int i = 0; i < 5; ++i) {
      if (noble.requirement[i] > 0) {
        std::cout << gem_bg(i) << " " << (int)noble.requirement[i] << " "
                  << RESET << " ";
      }
    }
    std::cout << "> ";
  }
  std::cout << std::endl << std::endl;

  for (int l = 2; l >= 0; --l) {
    for (int line = 0; line < 5; ++line) {
      if (line == 2)
        printf("  Tier %d:   ", l + 1); // Exact 12 chars
      else if (line == 3)
        printf("    (%2zu)    ", board.decks[l].size()); // Exact 12 chars
      else
        printf("            "); // 12 spaces

      for (int s = 0; s < 4; ++s) {
        print_card_line(board.visible[l][s], line);
        std::cout << "    ";
      }
      std::cout << std::endl;
    }
    std::cout << std::endl;
  }

  std::cout << BOLD << " Bank: " << RESET << "   ";
  for (int i = 0; i < 6; ++i) {
    std::cout << gem_bg(i) << " " << (int)board.bank[i] << " " << RESET << " ";
  }
  std::cout << std::endl << std::endl;

  std::cout
      << "                      Player 0                          Player 1"
      << std::endl;

  std::cout << "         ";
  for (int p = 0; p < 2; ++p) {
    for (uint8_t n_id : board.players[p].acquired_nobles) {
      std::cout << "  < " << BOLD << "3" << RESET << " >  ";
    }
    for (size_t i = board.players[p].acquired_nobles.size(); i < 3; ++i)
      std::cout << "        ";
    std::cout << "          ";
  }
  std::cout << std::endl;

  std::cout << BOLD << " Gems: " << RESET << "   ";
  for (int p = 0; p < 2; ++p) {
    int total = 0;
    for (int i = 0; i < 6; ++i) {
      std::cout << gem_bg(i) << " " << (int)board.players[p].gems[i] << " "
                << RESET << " ";
      total += board.players[p].gems[i];
    }
    printf(" Σ%2d      ", total);
  }
  std::cout << std::endl;

  std::cout << BOLD << " Cards:" << RESET << "   ";
  for (int p = 0; p < 2; ++p) {
    for (int i = 0; i < 5; ++i) {
      std::cout << gem_bg(i) << " " << (int)board.players[p].bonuses[i] << " "
                << RESET << " ";
    }
    std::cout << "              ";
  }
  std::cout << std::endl;

  bool has_reserved = false;
  for (int p = 0; p < 2; ++p)
    if (board.players[p].reserved_count > 0)
      has_reserved = true;

  if (has_reserved) {
    std::cout << std::endl;
    for (int line = 0; line < 5; ++line) {
      if (line == 2)
        std::cout << BOLD << " Reserve:  " << RESET;
      else
        std::cout << "           ";

      for (int p = 0; p < 2; ++p) {
        for (int r = 0; r < 3; ++r) {
          print_card_line(board.players[p].reserved[r], line);
          std::cout << "  ";
        }
        std::cout << "    ";
      }
      std::cout << std::endl;
    }
  }
  std::cout
      << "================================================================"
         "=========="
      << std::endl;
}

inline std::string format_gem_info(int type, int count) {
  return gem_bg(type) + "  " + RESET + "x" + std::to_string(count);
}

inline std::string find_card_location(const Board &board, int8_t card_id) {
  if (card_id == -1)
    return "";
  for (int l = 0; l < 3; ++l) {
    for (int s = 0; s < 4; ++s) {
      if (board.visible[l][s] == card_id) {
        return "T" + std::to_string(l + 1) + "-C" + std::to_string(s + 1);
      }
    }
  }
  for (int p = 0; p < 2; ++p) {
    for (int r = 0; r < 3; ++r) {
      if (board.players[p].reserved[r] == card_id) {
        std::string loc =
            "P" + std::to_string(p) + "-R" + std::to_string(r + 1);
        if (board.players[p].reserved_is_hidden[r])
          loc += "(H)";
        return loc;
      }
    }
  }
  return "Deck";
}

inline std::string format_card_brief(const Board &board, int8_t card_id) {
  if (card_id == -1)
    return "EMPTY";
  const auto &card = get_card(card_id);
  std::stringstream ss;
  ss << "[" << find_card_location(board, card_id) << "] " << gem_bg(card.bonus)
     << " " << (int)card.points << "pts " << RESET;
  return ss.str();
}

inline std::string format_action(const Board &board, const Action &a) {
  std::stringstream ss;
  switch (a.type) {
  case TAKE_DIFFERENT:
    ss << BOLD << "TAKE_DIFF: " << RESET;
    for (int i = 0; i < 5; ++i)
      if (a.take[i])
        ss << format_gem_info(i, a.take[i]) << " ";
    break;
  case TAKE_SAME:
    ss << BOLD << "TAKE_SAME: " << RESET;
    for (int i = 0; i < 5; ++i)
      if (a.take[i])
        ss << format_gem_info(i, a.take[i]) << " ";
    break;
  case RESERVE_VISIBLE:
    ss << BOLD << "RESERVE: " << RESET << format_card_brief(board, a.card_id);
    break;
  case RESERVE_DECK:
    ss << BOLD << "RESERVE_DECK: " << RESET << "Tier " << (int)a.deck_level + 1;
    break;
  case PURCHASE:
    ss << BOLD << "PURCHASE: " << RESET << format_card_brief(board, a.card_id)
       << (a.from_reserved ? BOLD + " (Res)" + RESET : "");
    break;
  default:
    ss << "UNKNOWN";
    break;
  }

  bool has_return = false;
  for (int i = 0; i < 6; ++i)
    if (a.return_gems[i])
      has_return = true;
  if (has_return) {
    ss << BOLD << " RETURN: " << RESET;
    for (int i = 0; i < 6; ++i) {
      if (a.return_gems[i]) {
        ss << format_gem_info(i, a.return_gems[i]) << " ";
      }
    }
  }

  if (a.noble_choice != -1) {
    ss << BOLD << " NOBLE: " << RESET << "N" << (int)a.noble_choice;
  }

  return ss.str();
}

inline void print_legal_actions(const Board &board,
                                const std::vector<Action> &actions) {
  std::cout << BOLD << "Legal Actions (" << actions.size() << "):" << RESET
            << std::endl;
  for (size_t i = 0; i < actions.size(); ++i) {
    std::cout << i << ": " << format_action(board, actions[i]) << std::endl;
  }
}

} // namespace cli

#endif // CSPLENDOR_CLI_UTILS_H
