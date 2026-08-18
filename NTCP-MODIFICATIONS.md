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
