import time
from csplendor import Game, Action
import random

def run_performance_test(n_games=1000):
    start_time = time.time()
    total_moves = 0
    
    for i in range(n_games):
        game = Game(seed=i)
        while not game.is_game_over():
            legals = game.legal_actions
            if not legals:
                break
            action = random.choice(legals)
            game.apply(action)
            total_moves += 1
            
    end_time = time.time()
    duration = end_time - start_time
    gps = n_games / duration
    mps = total_moves / duration
    
    print(f"Performance Test Results:")
    print(f"  Games played: {n_games}")
    print(f"  Total moves: {total_moves}")
    print(f"  Duration: {duration:.2f}s")
    print(f"  Games per second: {gps:.2f}")
    print(f"  Moves per second: {mps:.2f}")

if __name__ == "__main__":
    run_performance_test(10000)
