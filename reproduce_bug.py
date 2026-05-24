from csplendor import Game, ActionType
import numpy as np

def test_gem_return_bug():
    game = Game(seed=42)
    # Setup player 0 with 9 gems (getting close to limit)
    # Let's say: 2 Emerald, 2 Sapphire, 2 Ruby, 2 Diamond, 1 Onyx = 9 gems
    p0 = game.board.players[0]
    p0.gems = [2, 2, 2, 2, 1, 0]
    game.board.set_player(0, p0)
    
    # Force the bank to have enough gems for a TAKE_3 action
    # We want to take 3 gems (different colors) to reach 12 gems.
    # Player needs to return 2 gems.
    # User report says: "10枚制限オーバー時に3枚返却が必要な状況" -> so let's start with 10 gems and take 3 -> 13 gems -> return 3.
    
    # Reset to 10 gems logic as per user report
    # 2 of each 5 colors = 10 gems
    p0.gems = [2, 2, 2, 2, 2, 0]
    game.board.set_player(0, p0)
    
    print(f"Initial gems: {p0.gems}")
    
    # Current legal actions should include TAKE_DIFFERENT (3 gems)
    # Taking 3 gems -> 13 gems total. limit is 10. Must return 3.
    
    legals = game.legal_actions
    take_actions = [a for a in legals if a.type == ActionType.TAKE_DIFFERENT]
    
    print(f"Found {len(take_actions)} TAKE_DIFFERENT actions")
    
    faulty_found = False
    expected_found = False
    
    for a in take_actions:
        taken_sum = sum(a.take)
        return_sum = sum(a.return_gems)
        
        # We expect return_sum to be exactly 3
        # The bug report says engine produces things like [3, 1, 1, 1, 1, 0] -> sum = 7
        
        if return_sum == 3:
            # Check if it's a valid return (e.g. returning what we have)
            # This is just a basic check
            expected_found = True
        elif return_sum > 3:
            print(f"BUG DETECTED: Action taking {a.take} requires returning {a.return_gems} (sum={return_sum})")
            faulty_found = True
            
    if faulty_found:
        print("FAIL: Found actions returning too many gems.")
    elif not expected_found:
        print("FAIL: Did not find any valid return actions (returning 3 gems).")
    else:
        print("PASS: Seems okay (or bug not reproduced exactly).")

if __name__ == "__main__":
    test_gem_return_bug()
