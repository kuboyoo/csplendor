import csplendor as cs

# Initialize game
game = cs.Game()

# Print board
print("Initial Board State:")
game.print_board()

# Print legal actions
print("\nLegal Actions:")
game.print_legal_actions()

# Apply an action (TAKE_DIFFERENT if available)
legals = game.legal_actions
if legals:
    action = legals[0]
    print(f"\nApplying action: {action}")
    game.apply(action)
    
    print("\nUpdated Board State:")
    game.print_board()
else:
    print("No legal actions available.")