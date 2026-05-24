import csplendor as cs
import random

def test_determinization():
    # Fix seed for reproducibility of the initial state
    game = cs.Game(seed=42)
    board = game.board
    
    # Player 0 reserves from Tier 1 deck
    # Find a legal RESERVE_DECK action for Tier 1
    actions = game.legal_actions
    reserve_deck_t1 = next(a for a in actions if a.type == cs.ActionType.RESERVE_DECK and a.deck_level == 0)
    
    print(f"Player 0 reserving from Tier 1 deck...")
    game.apply(reserve_deck_t1)
    
    p0 = board.get_player(0)
    print(f"P0 reserved count: {p0.reserved_count}")
    print(f"P0 reserved IDs: {p0.reserved}")
    print(f"P0 reserved is_hidden: {p0.reserved_is_hidden}")
    
    # Verify it is hidden
    assert p0.reserved_count == 1
    assert p0.reserved_is_hidden[0] == True
    
    original_id = p0.reserved[0]
    print(f"Original hidden card ID: {original_id}")
    
    # Now randomize from Player 1's perspective
    print("\nRandomizing hidden info from P1's perspective...")
    different_ids = set()
    for i in range(10):
        # We need a copy of the board to randomize without affecting the main game
        # or just randomize and check
        board.randomize_hidden_information(1, seed=i)
        new_id = board.get_player(0).reserved[0]
        different_ids.add(new_id)
        # Verify it's still Tier 1
        assert cs.get_card(new_id).level == 1
        
    print(f"Unique IDs encountered after 10 randomizations: {different_ids}")
    assert len(different_ids) > 1, "ID should have changed during randomization"
    
    # Player 1 reserves from board (visible)
    # Find a legal RESERVE_VISIBLE action
    actions = game.legal_actions
    reserve_visible = next(a for a in actions if a.type == cs.ActionType.RESERVE_VISIBLE)
    card_id = reserve_visible.card_id
    
    print(f"\nPlayer 1 reserving visible card {card_id}...")
    game.apply(reserve_visible)
    
    p1 = board.get_player(1)
    assert p1.reserved_is_hidden[0] == False
    
    # Randomize from Player 0's perspective
    # P1's card is NOT hidden, so it should NOT change
    print("Randomizing from P0's perspective (P1's card is visible)...")
    board.randomize_hidden_information(0, seed=99)
    assert board.get_player(1).reserved[0] == card_id, "Visible reserved card should not change"
    print("Success: Visible reserved card remained unchanged.")

if __name__ == "__main__":
    try:
        test_determinization()
        print("\nALL DETERMINIZATION TESTS PASSED!")
    except Exception as e:
        print(f"\nTEST FAILED: {e}")
        import traceback
        traceback.print_exc()
