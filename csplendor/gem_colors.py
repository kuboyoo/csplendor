"""Canonical gem names and symbols shared by Python-facing adapters.

Numeric indices are defined by the native ``GemType`` contract:
Diamond/White, Sapphire/Blue, Emerald/Green, Ruby/Red, Onyx/Black, Gold.
"""

GEM_NAMES = ["Diamond", "Sapphire", "Emerald", "Ruby", "Onyx", "Gold"]

# Jewel initials for display-only callers.  USI deliberately uses a different
# alphabet so White/Blue/Black remain unambiguous.
GEM_SYMBOLS = ["D", "S", "E", "R", "O", "G"]
GEM_USI_SYMBOLS = ["W", "U", "G", "R", "K", "D"]
