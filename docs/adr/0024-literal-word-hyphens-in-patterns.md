# Treat word hyphens as literals in patterns

## Context

Luau treats `-` as a minimum-repetition suffix everywhere outside a character class. Common literal names such as `a-b` and `Sec-WebSocket-Accept` therefore match partial text or fail unless every hyphen is escaped as `%-`.

## Decision

Treat an unescaped hyphen between two literal ASCII word characters as a literal hyphen. Preserve the minimum-repetition suffix in every other position. Continue accepting `%-` as an explicit literal hyphen.

## Consequences

Match common identifiers and protocol header names directly. Preserve patterns such as `a-`, `.-`, and `%w-`. Change the ambiguous `a-b` form from minimum repetition to literal matching.
