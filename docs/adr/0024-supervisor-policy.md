# ADR 24: The supervisor restarts on loss, never on a verdict

Status: Accepted

## Context
A process pair that dies should come back; a process that says "my book disagrees with the model" should not be silently restarted into the same state.

## Decision
`hft_live` exits 4 for an integrity verdict (model disagreement, failed flatten, desync, book error). The supervisor treats that as final: no restart, exit 4. Every other exit, including a crash by signal, restarts the pair with exponential backoff, bounded by a restart count within a time window. Liveness is judged by a file the main loop refreshes whether or not input arrives, so a quiet feed is not mistaken for a hang and a stopped process is not mistaken for a quiet feed. SIGTERM to the supervisor reaches `hft_live` as SIGTERM, which is the kill path.

## Consequences
A bug that makes the model check fail stops the service instead of looping; a flapping feed does not (it is handled inside `hft_live` without a restart). The window and limit are chosen, not tuned. The supervisor cannot tell a hung `hft_live` from one with a broken metrics write.

## Evidence
`scripts/drill.py` (integrity, crash_loop, slow_flapping, hang, sigterm, term_ignored); 11 injected supervisor bugs, all caught.
