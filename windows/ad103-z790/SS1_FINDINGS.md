# SS1 pin instability — findings and the fix

`profile.c`'s `Registers()` does not pin `*Ss1` to any literal "cold" value.
This document records why, since a naive first attempt did pin it, twice,
and both pins were independently falsified by real hardware before this
fix was applied.

## What happened, in order

| Run | ss0 | ss1 | Shutdown before it |
|---|---|---|---|
| Original `probe/` capture, session 1 | `0x3634054` | `5` | warm restart (never verified cold) |
| Original `probe/` capture, session 2 | `0x3630050` | `5` | warm restart (never verified cold) |
| `RUN0005.TXT` | `0x3634054` | `1` | unknown, likely warm (Fast Startup still on) |
| `RUN0006.TXT` | `0x3634054` | `1` | unknown, likely warm (Fast Startup still on) |
| `RUN0007.TXT` | not logged (passed `Registers()`) | `5` (inferred: it passed the then-pin) | *assumed* cold (followed a hard power-off from a hang) — never explicitly confirmed the way the two below were |
| `RUN0008.TXT` | `0x3630044` | `1` | unknown (Fast Startup still on at this point) |
| `RUN0009.TXT` | `0x3630044` | `1` | **confirmed**: complete shutdown, everything off, >1 minute, Fast Startup just disabled |
| `RUN0010.TXT` | `0x3630044` | `1` | **confirmed**: same as RUN0009 |
| `RUN0011.TXT` | `0x3630050` | `5` | **confirmed**: same kind of cold boot as RUN0009/0010 |

Pin history: originally `5` (from the warm-restart-contaminated capture) →
corrected to `1` after RUN0009/0010 (two confirmed-cold reproductions) →
contradicted again by RUN0011, also confirmed-cold.

## Why this isn't "which single value is right"

1. **RUN0011 falsifies the premise, not just the guess.** RUN0009, RUN0010,
   and RUN0011 all followed the same confirmed procedure (full power-off,
   >1 minute, Fast Startup already disabled), yet SS1 read `1` twice and `5`
   once. Two values are now independently reproduced under identical
   confirmed-cold conditions on the same board. There is no remaining theory
   ("warm vs cold," "Fast Startup," "needs longer off-time") left to explain
   the split — the shutdown procedure itself is no longer a variable across
   these three runs.
2. **A vendor comment in `vendor/unlock_610.c` names a *third* value:**
   "SS0 holds eight 4-bit fields; SS1 stores only its low 4 bits (0x2 when
   cold)." Neither `1` nor `5` matches this either. Three sources, three
   different numbers, for what was being treated as one fixed per-board
   constant.
3. **The rest of the codebase never treats SS1's entry value as a literal.**
   `port.c`'s `GuSelectorPhase()` and `GuEntrySelectorsMatch()` both compare
   against the *latched* entry value (`entry1` / `EntrySs1`), never a
   hardcoded number, for every mid-transaction consistency check. The only
   two places that ever hardcoded a specific entry value were `profile.c`'s
   `Registers()` (the pin that flipped twice) and a bare `ss1!=1` in
   `unlock_610.c`, itself a leftover `ad104-msi` constant since removed --
   for the same underlying reason this document is about.
4. **SS0 — the 8-field register right next to it, same BAR0 region — was
   never pinned to a literal in the first place.** It's only checked
   structurally: not `0xffffffff` (inaccessible), not the `0xbadf0000`-masked
   error marker, not `0x88888888` (the post-unlock target value). SS1 being a
   single 4-bit field apparently got treated differently with no principled
   reason, and that's exactly the assumption that failed.

## The fix

`profile.c`'s `Registers()` does not pin `*Ss1` to any literal. It uses the
same structural philosophy already used for SS0:

- Reject `Ss1==0xffffffff`-class inaccessible reads (already partially
  covered: `port.c`'s `GuLatchEntrySelectors` independently enforces
  `Ss1<=0xf`, which already excludes this and anything out of the known
  4-bit range).
- Reject `Ss1==GU_SS1_UNLOCK` (`0x8`) -- the already-unlocked target value,
  the direct SS1 analogue of SS0's `!=0x88888888` exclusion (meaning: this
  card already has the selectors in the post-unlock state, i.e. not cold).
- No equality check against any specific "cold" literal. Whatever value SS1
  actually reads at entry gets latched via the existing
  `GuLatchEntrySelectors(ss0,ss1)` call, and `GuEntrySelectorsMatch` already
  correctly re-verifies equality to *that* latched value everywhere else in
  the transaction (`check_entry_selectors`, `GuSelectorPhase`, `port.c`'s
  final pre-mutation re-check) -- exactly the same way it already handles
  SS0's dynamic value.

Net effect: `Registers()` stops refusing based on which of several
apparently-legitimate entry states the hardware happened to land in on a
given cold boot, while still refusing on anything that looks like "not
actually cold" (already-unlocked) or "not actually readable." Nothing about
the mid-transaction consistency guarantees changes -- they were already
value-agnostic.

## Open question for other boards

Is the `Ss1==0x8` exclusion the right (and only) "known-bad" sentinel to
carry over from the SS0 pattern on a different board, or is there a more
complete set worth excluding given SS1 is a single 4-bit field rather than
SS0's 8-field layout? Does treating SS1 as purely "latch whatever it is,
verify consistency later" rather than "pin an expected cold value" close a
safety property a literal pin was quietly providing, or was that pin never
doing real work in the first place (per point 3 above, nothing downstream
depended on a *specific* value, only on consistency with the latched one)?
