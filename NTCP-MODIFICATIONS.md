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

## Known upstream bugs (not modified)

Bugs found in upstream code during nt-chiptune-player development that this fork
does **not** patch (out of scope for the fix that found them, or not yet
prioritized). Listed here so they aren't rediscovered from scratch; see the
linked issue for detail and status.

- **`Hes_Emu::cpu_write` / `CPU_WRITE_FAST_` in `gme/hes_cpu_io.h` always test
  `mmr[0]`, never the actual target page's mmr entry**, because `addr` is masked
  to a page-local offset *before* `addr >> page_shift` is computed (the sibling
  read path, `Hes_Emu::cpu_read`, gets this right by checking `mmr[]` *before*
  masking). This does not cause any out-of-bounds access -- `addr` stays within
  the masked page's bounds, and `mmr[0]` is a valid index -- so it is a
  correctness/dispatch bug, not a memory-safety bug, and is unrelated to the
  2026-08-30 SEGV fix above. Not fixed here (scope discipline for that fix); see
  https://github.com/deltamodulation/nt-chiptune-player/issues/192 for the full
  writeup and future fix plan.
