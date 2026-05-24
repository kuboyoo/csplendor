import csplendor as cs
import time
import random

def benchmark_legal_actions(iterations=10000):
    game = cs.Game(seed=42)
    
    # Generate some random states by playing a few moves
    for _ in range(20):
        actions = game.legal_actions
        if not actions: break
        game.apply(random.choice(actions))
    
    print(f"Benchmarking legal_actions() over {iterations} iterations on a mid-game state...")
    
    start_time = time.time()
    for _ in range(iterations):
        _ = game.legal_actions
    end_time = time.time()
    
    duration = end_time - start_time
    ips = iterations / duration
    print(f"Total time: {duration:.4f}s")
    print(f"Iterations per second: {ips:.2f}")
    return ips

if __name__ == "__main__":
    # Note: To see the true effect, we would need to compare against the old version.
    # Since we've already overwritten the code, we'll just measure the current performance.
    # Typical unoptimized Python-bound C++ Splendor engines do ~5k-10k legal_actions/s.
    # Highly optimized ones can do 50k-100k+.
    benchmark_legal_actions()
