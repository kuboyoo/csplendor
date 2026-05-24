from csplendor import Game, ActionType, get_card

def test_payment_options():
    game = Game(seed=42)
    # Give player 0 some gems and gold
    p0 = game.board.players[0]
    p0.gems = [3, 3, 3, 3, 3, 3]  # 3 of each color + 3 gold
    game.board.set_player(0, p0)
    
    legals = game.legal_actions
    purchase_actions = [a for a in legals if a.type == ActionType.PURCHASE]
    
    print(f"Total PURCHASE actions: {len(purchase_actions)}")
    
    # Group by card_id
    from collections import defaultdict
    by_card = defaultdict(list)
    for a in purchase_actions:
        by_card[a.card_id].append(a)
    
    print(f"Unique cards purchasable: {len(by_card)}")
    
    # Show details for one card
    if by_card:
        card_id = list(by_card.keys())[0]
        options = by_card[card_id]
        card = get_card(card_id)
        print(f"\nCard {card_id}: cost={list(card.cost)}, bonus={int(card.bonus)}")
        print(f"Number of payment options: {len(options)}")
        
        for i, opt in enumerate(options[:5]):  # Show first 5
            print(f"  Option {i+1}: gold_as={list(opt.gold_as)}")
        if len(options) > 5:
            print(f"  ... and {len(options) - 5} more options")

if __name__ == "__main__":
    test_payment_options()
