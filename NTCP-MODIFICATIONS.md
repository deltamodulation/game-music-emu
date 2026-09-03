# nt-chiptune-player modifications

This is a fork of game-music-emu, forked from
https://github.com/libgme/game-music-emu at `fe8da4b` (upstream also lives at
https://bitbucket.org/mpyne/game-music-emu/). Maintained at
https://github.com/deltamodulation/game-music-emu on branch
`ntcp-hes-channel-state`, for use by
[nt-chiptune-player](https://github.com/deltamodulation/nt-chiptune-player).

game-music-emu is licensed under the **GNU Lesser General Public License,
version 2.1 or later (LGPL-2.1+)**. See `license.txt` in this repository for
the full license text. This file exists to satisfy LGPL-2.1 §2(a), which
requires prominent notice of, and a description of, any modified files.

Corresponding source for this fork (including this modification history) is
publicly available at the repository and branch named above. No upstream
rebase is planned for the v1 release; see nt-chiptune-player's
`docs/licenses-libgme-lgpl.md` for the consumer-side LGPL compliance
bookkeeping, the (future) upstream-tracking rebase procedure, and
rebuild/replacement instructions.

## Design rationale (why a minimal C API addition)

nt-chiptune-player's UI renders a fixed-layout binary "snapshot" of chip
state (the layout is defined by the consuming file format, not by the
engine) at 30-60Hz for visualization. For HES it needs, per channel: the
enable/DDA/noise flags, volume, pan (balance), and pitch (period/noise
frequency) -- but upstream `gme.h`/`gme.cpp` expose no per-channel state
accessor at all. Getting at it without modifying libgme was not possible: the
raw oscillator registers (`Hes_Apu::oscs[]`) are C++ `private`, the shared
library's default symbol visibility is `hidden`, and the linker version
script (`gme.exports`) additionally blocks any symbol not explicitly listed
-- a threefold barrier that persists even if any one layer is bypassed.
Two alternatives were considered and rejected: (a) estimating channel state
from the existing multi-channel PCM output -- rejected because pitch would
have to be guessed from waveform analysis, which is unreliable, and would
also require reworking the emulator's initialization path; (b) vendoring
libgme (dropping the submodule and committing a full copy) instead of
forking it -- rejected because it would forfeit tracking upstream security
and bug fixes. Forking with a minimal, additions-only C API (see the
modification history below) was chosen instead: it adds exactly one
read-only snapshot accessor, changes no existing behavior, and keeps the
modification surface small and auditable.

For the full design record and rejected-alternatives detail, see
nt-chiptune-player's ADR 0023 (secondary reference, if accessible --
https://github.com/deltamodulation/nt-chiptune-player/blob/main/docs/adr/0023-libgme-fork-minimal-c-api.md
-- that repository's visibility is not guaranteed, so this section is
written to stand on its own without it).

## Modification history

### 2026-08-17 -- Add read-only `gme_hes_channel_state` C API

Commit `ecfcf9e`. Adds a minimal, additions-only C API
(`gme_hes_channel_state` in `gme/gme.h`) that exposes a read-only snapshot of
a HES (PC Engine/HuC6280 PSG) oscillator's raw state: enable/DDA/noise flags,
channel volume, balance, period, noise frequency, and the already-synthesized
linear gain per stereo side. This is needed by nt-chiptune-player to
reconstruct per-channel pitch/pan/volume for a visualizer without duplicating
the chip's log-volume table or register decoding. Purely additive: no
existing behavior is changed.

Files touched: `gme/gme.h`, `gme/gme.exports`, `gme/Hes_Apu.h`,
`gme/Hes_Apu.cpp`, `gme/Hes_Emu.h`, `gme/Hes_Emu.cpp`.

### 2026-08-19 -- LGPL modification notices, cleanup, and documentation

- Added an in-file "Modified <dates> by nt-chiptune-player project" notice
  (LGPL-2.1 §2(a)) to the header of each file touched by the 2026-08-17
  change, and added this file.
- `gme/Hes_Emu.cpp`: replaced a C-style cast with `static_cast` in
  `gme_hes_channel_state()` for readability/consistency with the rest of the
  codebase's style; no behavior change.
- `gme/Hes_Emu.cpp`: added `BLARGG_EXPORT` to the *definition* of
  `gme_hes_channel_state()` to match its declaration in `gme/gme.h`. This is
  a **no-op**: the actual export/visibility control for this build is
  `gme/gme.exports` (a linker version script), and the symbol was already
  exported via that mechanism plus the declaration-side `BLARGG_EXPORT` in
  `gme.h` (which `Hes_Emu.cpp` includes). The change is purely for
  declaration/definition consistency and readability.
- `gme/gme.h`: repointed the `gme_hes_channel_state` doc comment's rationale
  reference from an internal `AGENTS.md`/README mention to this file's
  "Design rationale" section above (primary reference), with
  nt-chiptune-player's ADR 0023 kept as a secondary, may-not-be-accessible
  pointer for readers who want the full design record.
- `gme/gme.h`: added a one-line comment on `gme_hes_channel_state_t` noting
  its struct layout is frozen for ABI compatibility with the
  nt-chiptune-player JNI bridge (append-only; no reordering/removal).

No functional/behavioral change in this update; golden-test bit-exactness of
the HES decode path is preserved.

### 2026-08-30 -- Fix out-of-bounds `code_map` read on corrupted/malformed HES data (SEGV)

`gme/Hes_Cpu.cpp`, in `Hes_Cpu::run`: added `pc &= 0xFFFF;` immediately after the
`loop:` label. This is the **first behavior-changing patch in this fork** -- the
2026-08-17 and 2026-08-19 changes above were additions-only / no-op by design (see
their entries and the "Design rationale" section, both of which describe only
those two commits, not a blanket policy for all future changes to this fork).

Root cause: `pc` is declared `uint_fast16_t`, which is a 32-bit type on both MSVC
and Android NDK toolchains. It is masked back to 16 bits only on taken branches
and on `JMP`/`JSR`/`RTS`/`RTI`. Straight-line execution through corrupted or
out-of-spec instruction bytes -- including the pre-existing `default:` (illegal
opcode) case, which upstream already treats as a 1-byte/2-cycle NOP and continues
executing -- never touches those masking sites, so on malformed input `pc` can
grow past `0xFFFF` without bound. `state_t::code_map` has exactly 9 entries,
covering only address range `$00000`-`$11FFF`; once `pc >> page_shift` reaches 9,
the read indexes past the end of `code_map` into adjacent struct fields. On MSVC
x64 this reads `state_t::base`/`state_t::time` (an `int32_t` pair) as a pointer,
and the next instruction fetch dereferences it, producing a SEGV. This is
unrelated to opcode legality -- it reproduces even when the corrupted region is
filled entirely with `$EA` (real-hardware NOP); what matters is that execution
never re-enters a PC-masking site.

The fix mirrors real HuC6280 hardware, which wraps the program counter at 16
bits unconditionally, every cycle -- not only on branches/jumps.

Verified locally (nt-chiptune-player coordinator, pre-push):
- 18 corrupted-file repros + an 80-run fuzz sweep (98 runs total) against the
  pre-fix build reproduced the SEGV; against the post-fix build, 0/98 crashed.
- Well-formed files are bit-exact before and after: full-PCM-sample hashes for
  Final Soldier and Gradius across 12 tracks x 30.7s (24 runs) match exactly.
  This is expected -- well-formed HES data never drives `pc` past `$FFFF`, so the
  added mask is a no-op on any file this engine already played correctly.

Files touched: `gme/Hes_Cpu.cpp` (one-line fix + explanatory comment + in-file
LGPL-2.1 §2(a) modification notice).

### 2026-08-31 -- Fix `cpu_write` always dispatching on `mmr[0]` instead of the target page (Issue #192)

`gme/hes_cpu_io.h`, in `Hes_Emu::cpu_write` and the sibling `CPU_WRITE_FAST_`
macro: evaluate the target page (`addr >> page_shift`) *before* masking `addr`
down to a page-local offset, instead of after. This is a **behavior-changing
patch** (second one in this fork, after the 2026-08-30 SEGV fix above).

Root cause: both `cpu_write` and `CPU_WRITE_FAST_` computed `write_pages [addr
>> page_shift]` correctly (before masking), but then masked `addr &= page_size
- 1` and only afterwards evaluated `mmr [addr >> page_shift]` to decide whether
to dispatch to `cpu_write_()` (the special-I/O handler). Since the masked
`addr` is always `< page_size`, `addr >> page_shift` is always `0` after
masking, so the dispatch check always looked at `mmr[0]` regardless of which
page was actually being written. The sibling read path, `Hes_Emu::cpu_read`,
already gets this right -- it evaluates `mmr [addr >> page_shift]` before any
masking (in fact `cpu_read` never masks `addr` at all, since it does not need
a page-local offset). This was previously logged as a known-but-unfixed
upstream bug (see `Hes_Emu::cpu_write` entry removed from "Known upstream
bugs" below); memory safety was never at risk (`addr` stayed within the masked
page's bounds, and `mmr[0]` is a valid index), so this is a pure
correctness/dispatch fix, not a security fix.

The fix: compute `page = addr >> page_shift` once, before masking `addr`, and
use `page` (instead of the freshly-masked `addr >> page_shift`) in the `mmr[]`
lookup. This mirrors `cpu_read`'s order of operations.

Verified locally (nt-chiptune-player coordinator, pre-push):
- Real-data regression: rendered tracks 0-40 (41 tracks each, 8s @ 48kHz
  stereo) of Final Soldier and Gradius (82 renders total) with a pre-fix and a
  post-fix build of `spike_hes2wav`; all 82 output WAV files are byte-for-byte
  identical between the two builds (0/82 differ). This is consistent with the
  bug never having been observed to affect real HES data (see Issue #192):
  `write_pages[page]` being null (the precondition for the `mmr[]` check to
  even run) combined with `mmr[page] != 0xFF` while `mmr[0] == 0xFF` (or vice
  versa) apparently does not occur in these titles' actual mmr configurations.
- `ctest` (core host-debug preset): 243/243 passed, including the HES golden
  bit-exactness tests (`Golden.BitExactAgainstManifest` et al.), confirming no
  regression in the existing golden-test corpus either.

Files touched: `gme/hes_cpu_io.h` (four-line fix across `cpu_write` and
`CPU_WRITE_FAST_` + in-file LGPL-2.1 §2(a) modification notice -- this file had
no prior in-file notice, unlike its siblings, since it was untouched by the
2026-08-17/2026-08-19 additions-only changes).

### 2026-09-03 -- Rename `CPU_WRITE_FAST_`'s local `page_` to `hes_write_page_` (Issue #221)

`gme/hes_cpu_io.h`, in the `CPU_WRITE_FAST_` macro added by the 2026-08-31 patch
above: renamed the macro-local variable `page_` to `hes_write_page_`. This is a
**pure rename, no behavior change** -- naming cleanup only, deferred from PR #218
(Issue #192's review, Round 1 L-2) to be bundled with the next fork change rather
than spending a standalone fork commit on it.

Rationale: nt-chiptune-player's codebase convention reserves a trailing `_` for
member variables (e.g. `emu_track_ended_` / `warning_` / `ignore_silence_`), so a
macro-local variable shaped like `page_` misleads a reader into thinking it is a
member. `page_` is confined to the macro body -- confirmed via a fork-wide grep
that no other file (`Hes_Cpu.cpp` / `Hes_Cpu.h` etc.) references an identifier
named `page_`, so the rename cannot collide with or shadow anything.

Verified locally (nt-chiptune-player coordinator, pre-push): `ctest` (core
host-debug preset) full suite green, golden HES bit-exactness tests included --
expected, since a macro-local rename cannot change generated code.

Files touched: `gme/hes_cpu_io.h` (rename only, three occurrences within
`CPU_WRITE_FAST_`; updated the file's in-file modification-notice date list to
add 2026-09-03).

## Known upstream bugs (not modified)

Bugs found in upstream code during nt-chiptune-player development that this fork
does **not** patch (out of scope for the fix that found them, or not yet
prioritized). Listed here so they aren't rediscovered from scratch; see the
linked issue for detail and status.

(None at this time -- the `Hes_Emu::cpu_write` / `CPU_WRITE_FAST_` entry
previously listed here was fixed on 2026-08-31; see above.)
