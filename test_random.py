import csplendor
import random

def test_random_playout():
    game = csplendor.Game(seed=42)
    print("Initial Board:")
    print(game.board)
    
    turn_limit = 200
    while not game.is_game_over() and game.turn < turn_limit:
        actions = game.legal_actions
        if not actions:
            print("No legal actions!")
            break
        action = random.choice(actions)
        # print(f"Applying Action: {action}")
        game.apply(action)
    
    print("\nFinal Board:")
    print(game.board)
    print(f"Game Over: {game.is_game_over()}")
    print(f"Winner: {game.winner}")
    print(f"Turns: {game.turn}")

if __name__ == "__main__":
    test_random_playout()
