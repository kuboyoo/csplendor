import pytest
from csplendor import Game, Action, ActionType, GemType

def test_max_gems_rule():
    game = Game(seed=123)
    # Give player 0 exactly 10 gems
    p0 = game.board.players[0]
    p0.gems = [2, 2, 2, 2, 2, 0]
    game.board.set_player(0, p0)
    
    # Check that any action taking more gems forces a return
    legals = game.legal_actions
    for a in legals:
        if a.type in [ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME]:
            # All legal take actions in this state MUST have return_gems
            assert sum(a.return_gems) > 0
            # Total gems after action minus returns must be <= 10
            total_after = sum(p0.gems) + sum(a.take) - sum(a.return_gems)
            assert total_after <= 10

def test_reserve_limit():
    game = Game()
    p0 = game.board.players[0]
    p0.reserved = [1, 2, 3] # Max 3
    game.board.set_player(0, p0)
    
    # If 3 reserved, RESERVE actions should be illegal
    legals = game.legal_actions
    for a in legals:
        assert a.type not in [ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK]

def test_noble_visit():
    game = Game()
    # Setup player with enough bonuses for any noble
    p0 = game.board.players[0]
    from csplendor import get_noble
    noble_id = game.board.nobles[0]
    noble = get_noble(noble_id)
    
    bonuses = [0]*5
    for i in range(5):
        bonuses[i] = noble.requirement[i]
    p0.bonuses = bonuses
    game.board.set_player(0, p0)
        
    legals = game.legal_actions
    action = [a for a in legals if a.type == ActionType.TAKE_DIFFERENT][0]
    game.apply(action)
    
    # Player 0 should have the noble points
    # (Since current_player is now 1, we check players[0])
    assert game.board.players[0].points >= 3
    assert noble_id not in game.board.nobles

if __name__ == "__main__":
    test_max_gems_rule()
    test_reserve_limit()
    # test_purchase_with_gold() # Requires careful setup of visible
    # test_noble_visit()
    print("Rule tests passed!")
