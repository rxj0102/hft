# ADR 40: A keyless public mode for the smoke test

Status: Accepted

## Context
The smoke test (ADR 25 onward) needs a sandbox key, secret and passphrase. They were not available when Phase 31 tried to run it: the key dialog gave only a key ID. Part of what the bridge and the capture tools assume about the sandbox is public and needs no key: the clock, the product's size and price steps, the ticker, and the level-2 snapshot on the public websocket feed.

## Decision
- `smoke_sandbox.py --public-only` runs only those checks: 1 target, 2 clock, 3 product, 4 ticker, 4 public feed. It reads no environment variable, makes no signed request and places no order. It exits 0 when they pass and says what it did not test (the signature, the account, orders, the user channel).
- The public feed check subscribes to `--channel` (default `level2_batch`, the capture's channel) with no credentials and reads the first snapshot. It fails if the exchange refuses the subscription (the message is printed), if no snapshot arrives within `--timeout` seconds, if the levels are not `[price, size]` pairs, if the book is empty on a side, locked, crossed or has a zero size, or if the best bid or the best ask is more than 1 percent from the REST ticker. Each side is compared, not the mid, so a book too wide around the right mid fails.
- The `Api` object has a public flag that refuses a signed call before it is built. The test calls it directly.
- The URL rules are unchanged: only the sandbox, and the local fake with `--test-exchange`. `--cancel-existing` and `--skip-bridge` with `--public-only` are refused (exit 2). `--skip-ws` leaves the feed out.
- Steps 2 to 4 moved into functions shared by both modes. The existing tests pass unchanged.

## Consequences
A first real contact with the sandbox is possible without a key. It can show whether the product's sizes fit the bridge's scaling, how the public feed answers (including whether it wants authentication for level 2), and the shape of the snapshot. It cannot show anything about order entry, which remains unproven.

## Rejected
- Reading the key from a file or taking it as an argument: keys come only from environment variables.
- A key-less "order" test against the sandbox: an order needs a signed request.
- Comparing only the mid to the ticker: a book of the right centre and the wrong width passed it (found by the tests while writing this).

## Evidence
`scripts/test_smoke.py` (public section): a healthy run with no environment variables, no signed request reached the fake (`signed_seen` stays 0), one public subscription and no user-channel subscription, the report holds no credential, the refused option combinations, `--skip-ws`, nine bad-feed modes (refused, silent, unparseable, too far, wide around the right mid, crossed, locked, zero size, one side empty), the channel subscribed to, the clock-off stop and the production URL refusal. Mutation checks are listed in the CHANGELOG. Caveat: the fake's public feed is written from the documented message shapes, not recorded from the real sandbox.
