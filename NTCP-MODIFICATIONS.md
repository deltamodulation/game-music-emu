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

### 2026-09-04 -- Add opt-in `gme_hes_set_observe_interval_ms` C API (Issue #321)

Adds a minimal, **additions-only** C API that lets the caller shorten the internal
Blip_Buffer emulation-batch length of a HES emulator. Upstream fixes that batch at
`1000 / 20` ms (50 ms) in `Classic_Emu::set_sample_rate_`, and because the
emulator only advances the CPU/APU one whole batch at a time, 50 ms is also the
finest resolution at which the `gme_hes_channel_state` accessor added on
2026-08-17 can observe register state. nt-chiptune-player publishes its visualizer
snapshot at 60 Hz, so any register change faster than 50 ms (a fast arpeggio, for
instance) was structurally invisible to it.

Files touched (all additions, no existing line modified):

- `gme/Classic_Emu.h` / `gme/Classic_Emu.cpp`: new `protected` member
  `blargg_err_t Classic_Emu::set_buffer_length_ms( int msec )`. It calls
  `buf->set_sample_rate( sample_rate(), msec )`, which is safe to do *after* load
  because upstream's `Blip_Buffer::set_sample_rate()` preserves `clock_rate_` and
  clears the buffer. The member is `protected` on purpose: the public surface of
  this shared base class is unchanged for every `Classic_Emu`-derived format.
- `gme/Hes_Emu.h`: `Hes_Emu::set_observe_interval_ms( int )`, a one-line
  re-export of the base member -- the only public exposure of the new capability.
- `gme/Hes_Emu.cpp`: `extern "C" gme_hes_set_observe_interval_ms( Music_Emu*, int )`.
  The type check uses `gme_type_t` (`me->type() != Hes_Emu::static_type()`), not
  `dynamic_cast`, because libgme is built with RTTI disabled.
- `gme/gme.h`: declaration and contract comment.
- `gme/gme.exports`: the new symbol, required by the linker version script when
  building the shared library (`GME_BUILD_SHARED=ON`).

Range check: `msec` outside `1..1000` is rejected with an error string before it
reaches upstream code. Upstream's `Blip_Buffer::set_sample_rate()` computes
`(new_rate * (msec + 1) + 999) / 1000` in `long`, which overflows where `long` is
32-bit (MSVC / LLP64), and guards the result only with `assert( 0 )` -- a no-op
under `NDEBUG`, where it would silently over-allocate instead.

**The default is untouched.** A caller that never invokes this API gets exactly
upstream's 50 ms batch; nothing in the existing code paths reads any new state,
because the patch stores none.

The per-file LGPL-2.1 §2(a) modification notice is added here as a **new** line
rather than by extending the existing `Modified <dates>` line, so that this patch
keeps its additions-only property (see the verification below).

Verified locally (nt-chiptune-player, MSVC x64):

- Additions-only: `git diff --numstat 71199af..HEAD` deletion column is `0` for
  all six files.
- **Default behavior unchanged**: with this patch applied to the submodule and the
  consuming project *not* calling the new API, nt-chiptune-player's
  `ctest --preset host-debug` was green at **298/298**, including
  `Golden.BitExactAgainstManifest` (bit-exact PCM against the pre-existing golden
  hashes for both HES and MDX reference renderings) and `Golden.ChunkSizeInvariance`.
  This is a one-shot experiment: once the consuming project starts calling the API
  the HES golden hashes change by design, so the evidence is recorded here.
- Effect when opted in: on `PL91001` (Magical Chase) track 12, counting distinct
  `period` values observed on channel index 1 over the 18-24 s window, the default
  50 ms batch yields **3** transitions while a 16 ms batch yields **359** -- the
  latter matching the ground truth measured by instrumenting register writes
  directly.
- Range rejection: `msec` of `0`, `-1`, `1001` and `100000` all return
  `"Invalid buffer length"` and never reach `Blip_Buffer::set_sample_rate()`.

### 2026-09-12: `gme_nsf_channel_state` + `gme_nsf_set_observe_interval_ms` (Issue #558)

Same rationale as the HES additions above (ADR 0023 region 1, additions-only):
nt-chiptune-player's NSF engine needs read-only per-channel state (2A03 APU +
optional VRC6/VRC7/FDS/MMC5/N163/S5B expansion chips) for the same 30-60Hz
visualization snapshot HES already gets. Files touched (all additions-only --
new methods/structs/exports, no existing behavior changed):

- `gme/gme.h`: `gme_nsf_channel_state_t` struct (field composition mirrors
  `gme_hes_channel_state_t`) and the two new C API declarations.
- `gme/Nes_Apu.h`/`.cpp`: `Nes_Apu::get_osc_state()` -- covers the base 2A03
  channels (square1/2, triangle, noise, dmc) via the existing `length_counter`/
  `Nes_Envelope::volume()`/`period()` accessors, reused unmodified by MMC5 (see
  below).
- `gme/Nes_Vrc6_Apu.h`/`.cpp`, `gme/Nes_Vrc7_Apu.h`/`.cpp`: `get_osc_state()`
  decoding the documented VRC6/VRC7 register layout (see the getter's own
  comment for the bit layout, taken from each chip's existing `write_osc`/
  `write_data`/`run_square`/`run_saw` implementations).
- `gme/Nes_Mmc5_Apu.h`: `get_osc_state()` -- thin index-remapping wrapper over
  `Nes_Apu::get_osc_state()` (same remap `osc_output()` already does), header-
  only like the rest of this class.
- `gme/Nes_Namco_Apu.h`/`.cpp`, `gme/Nes_Fme7_Apu.h`/`.cpp`,
  `gme/Nes_Fds_Apu.h`/`.cpp`: `get_osc_state()`. **ponytail-scoped**: Namco
  (N163) and FDS use a reduced heuristic (last synthesized amplitude /
  envelope gain as the sole enabled/volume signal) rather than a full register
  decode, because their channel-to-register mapping is either
  runtime-configured (N163: channel count read from a control register) or
  spread across an LFO+sweep pair with no single pitch register (FDS). FME-7/
  S5B gets a full register decode (its register file has one fixed layout).
  See the ponytail comment on each getter for the precise ceiling and the
  upgrade path if per-chip pitch display accuracy for N163/FDS is needed.
- `gme/Nsf_Emu.h`/`.cpp`: `Nsf_Emu::channel_state()` dispatches to whichever
  chip owns voice `i`, replicating `set_voice()`'s existing index-subtraction
  chain (including the VRC6 "put saw first" reindexing) so that
  `gme_nsf_channel_state`'s index always names the same voice
  `gme_voice_count()`/`gme_voice_name()` do. `Nsf_Emu::set_observe_interval_ms`
  re-exports the already-shared `Classic_Emu::set_buffer_length_ms` (see the
  2026-09-04 entry above) the same way `Hes_Emu::set_observe_interval_ms` does
  -- no new logic, same range check and call-order contract (after load,
  before `gme_start_track`).
- `gme/gme.exports`: the two new symbols.

**The default is untouched.** A caller that never invokes
`gme_nsf_set_observe_interval_ms` gets exactly upstream's 50 ms batch, same as
NSF always has; `gme_nsf_channel_state` only reads existing oscillator state,
it writes nothing.

Verified locally (nt-chiptune-player, MSVC x64): with this patch applied and
the consuming project's NSF engine wired to call these APIs,
`ctest --preset host-release` golden hashes for the pre-existing HES and MDX
reference renderings are unchanged (see the PR that references this commit for
the exact run). Additions-only is `git diff --stat <old-pin>..HEAD` in
`third_party/game-music-emu` showing only insertions across the files listed
above, no deletions to pre-existing lines.

### 2026-09-12: VRC7/FDS/N163 exact `period` decode for keycode display (Issue #569)

Follow-up to the 2026-09-12 `gme_nsf_channel_state` entry above: that entry's
ponytail scope cut left `period` unset (FDS/N163) or missing the octave/block
bits (VRC7) for these three chips, so the consuming project's `keycode_for()`
could not reconstruct pitch for them at all (always `0xFF`). This entry
upgrades exactly those three `get_osc_state()` getters -- no other file
changes, no ABI change (the frozen `gme_nsf_channel_state_t` struct layout is
unchanged; only what `period` computes for these three chip_ids changes):

- `gme/Nes_Vrc7_Apu.cpp`: `get_osc_state()` now decodes the F-number's
  missing octave/block bits (`regs[1]` bits 1-3, alongside the bit-0 high
  F-number bit already read) and converts to the chip-independent
  "normalized period" convention documented on the getter (`hz =
  nes_cpu_clock/(period+1)`), matching the convention the consuming project's
  `keycode_for()` already uses for 2A03/VRC6/S5B (divisor=1 case).
- `gme/Nes_Fds_Apu.cpp`: `get_osc_state()` now reads the existing
  `$4082`/`$4083` wavetable frequency registers (same registers `run_until()`
  already reads) and converts them the same way.
- `gme/Nes_Namco_Apu.cpp`: `get_osc_state()` now reads the existing
  per-channel frequency/wave-size registers and the active-channel-count
  register (same registers `run_until()` already reads) and converts them
  the same way. The ponytail note on the header (last_amp-only *volume*
  signal) still applies -- only pitch/`period` was upgraded, not volume.

No behavior change to audio synthesis or any pre-existing getter output other
than these three `period` fields (previously 0/incomplete, now populated).
Verified locally (nt-chiptune-player, MSVC x64): `ctest --preset host-debug`
golden hashes for the pre-existing HES/MDX/NSF reference renderings are
unchanged after this patch (see the PR referencing this commit for the exact
run); `git diff --stat <old-pin>..HEAD` in `third_party/game-music-emu` shows
insertions only in the three files above (no deletions to pre-existing
lines, aside from the two `period` assignment lines each getter replaces
with the new decode).

### 2026-09-12 (2): Fix FDS keycode octave error, dead N163 guard note, PR #570 review Round 1 (Issue #569)

Follow-up to the same-day `get_osc_state()` entry above (2f27151), addressing
PR #570 review round 1 findings on the fork side:

- `gme/Nes_Fds_Apu.cpp` `get_osc_state()` (**H-1, bug fix**): the normalized
  `period` conversion was missing a `wave_size` (0x40=64) factor. `run_until()`
  clocks one wave *sample* every `fract_range/freq` (fract_range=65536) CPU
  cycles, and a full cycle is `wave_size` samples, so `hz = clock*wave_freq /
  (65536*wave_size)`, not `clock*wave_freq/65536` as the first version
  computed. This put every FDS keycode 6 octaves (72 semitones) too high.
  Fixed by multiplying by `wave_size`. Also reverted an unnecessary edit to
  the pre-existing `run_until()` line (it had been changed from `regs(0x4083)`
  to `regs_[0x4083-io_addr]` even though `run_until()` is non-const and the
  original `regs()` accessor already worked there -- only the new *const*
  getter needed the direct array access; **M-1**), and corrected a stale
  comment above the getter that still said "period is left at 0" (**L-1**),
  and documented that the getter approximates with the raw frequency register
  rather than `run_until()`'s post-sweep/modulation `freq` (**L-2**).
- `gme/Nes_Namco_Apu.cpp` `get_osc_state()` (**SEC-L-1, comment only**): added
  a comment noting the `wave_size != 0` check mirrors `run_until()`'s
  identically-shaped (and equally unreachable, `wave_size`'s structural
  minimum is 4) defensive check -- not an actual guard, kept only for
  parity/readability.

No ABI change, no change to any other getter's fields. `git diff --stat
2f27151..<this commit>` shows only `gme/Nes_Fds_Apu.cpp` and
`gme/Nes_Namco_Apu.cpp`. Verified locally: `ctest --preset host-debug`
376/376 green (mutation-confirmed: reverting the `wave_size` factor
reproduces the old wrong value, see PR #570 for the raw failing output).

### 2026-09-12 (3): Document the FDS keycode clamp's saturation region, PR #570 review Round 2 L-2 (Issue #569)

Comment-only follow-up to the Round 1 H-1 fix above: documents that the
`wave_size` factor added in that fix makes the `norm > 65535` clamp
reachable for very low `wave_freq` (below `wave_size`=64), saturating to a
keycode near MIDI ~21 (A0) for pitches under ~27 Hz -- far below any
musically intended FDS pitch. No code change, no ABI change, no behavior
change (the clamp itself is unmodified; only a comment was added explaining
when it engages).

### 2026-09-12 (4): N163 `get_osc_state()` full register decode, replacing the last_amp heuristic (Issue #572)

The ponytail-scoped heuristic documented above (last synthesized amplitude as
the sole enabled/volume signal) turned out to have a real-world bug, not just
reduced pitch accuracy: `run_until()` only updates `oscs[i]` for `i` in
`[osc_count-active_oscs, osc_count)` (the RAM-configured active-channel
range), so any index outside that range keeps whatever `last_amp` it last had
while active -- `osc.last_amp != 0` then reports it enabled forever (a level
meter stuck at its last value, a keyboard key lit for a channel that is
silent because N163's active-channel count went down). Deactivating a channel
by lowering the active count in `$7F` never zeroes its stale `last_amp`.

Fixed by decoding the RAM registers `run_until()` itself gates on, the same
approach `Nes_Fme7_Apu::get_osc_state()` already uses: `reg[0x7F]` bits 4-6
for `active_oscs` (only indices `>= osc_count-active_oscs` can be enabled),
`osc_reg[4] & 0xE0` for the frequency-enable gate, `osc_reg[7] & 15` for the
4-bit volume (now surfaced as `channel_vol`, previously hardcoded to a fixed
15 whenever `enabled`). `period` decode (Issue #569) and its `wave_size`
formula are unchanged. `gain_l`/`gain_r` are zeroed when disabled rather than
leaking the last stale `last_amp`.

- `gme/Nes_Namco_Apu.cpp` `get_osc_state()`: replaced the enabled/channel_vol
  logic as above; comment on the getter and the ponytail note in
  `Nes_Namco_Apu.h` updated to match (the h file's ponytail note about
  `channel_vol` being fixed-15 no longer applies -- period decode note is
  unchanged).

No ABI change (same `gme_nsf_channel_state_t` fields, same meaning). No
change to `run_until()` (audio output path untouched, ADR 0023 region 1).
`git diff --stat 083fc5b..<this commit>` shows only `gme/Nes_Namco_Apu.h`/
`.cpp` and this file.

Note: `enabled` is not visualization-only -- nt-chiptune-player's
`is_channel_keyon()` (core/src/nsf/nsf_engine.cpp) reads the same field for
both the visualization snapshot and the S4 silence-detection accumulator
(PR #573 review Round 1 M-5). This change makes `enabled` stricter (a
previously-stuck-true channel now correctly reports false), which only
tightens S4's silence detection -- it cannot make an audible channel report
silent. Direction is safe; existing real-data golden hashes are unchanged.

### 2026-09-12 (5): N163 `get_osc_state()` also mirrors run_until()'s low-frequency skip (PR #573 review Round 1 M-4)

The (4) fix above still missed one of `run_until()`'s three skip conditions:
`if ( freq < 64 * active_oscs ) continue;`. A channel whose frequency falls
below that floor produces no output from `run_until()` at all, but the (4)
version of `get_osc_state()` did not check it, so such a channel could still
report `enabled=true` -- the same class of bug (4) fixed, on a smaller
surface (a channel that never produces output, rather than one that used to
and got deactivated).

- `gme/Nes_Namco_Apu.cpp` `get_osc_state()`: folded `freq >= 64 * active_oscs`
  into the `enabled` expression (computed `freq` once, reused for both
  `enabled` and the existing `period` calculation -- no behavior change to
  `period`).

No ABI change. No change to `run_until()`. Verified locally: ctest (core
host-debug preset) 377/377 green (bit-exact golden unaffected -- this getter
is not on the audio path).

## Known upstream bugs (not modified)

Bugs found in upstream code during nt-chiptune-player development that this fork
does **not** patch (out of scope for the fix that found them, or not yet
prioritized). Listed here so they aren't rediscovered from scratch; see the
linked issue for detail and status.

(None at this time -- the `Hes_Emu::cpu_write` / `CPU_WRITE_FAST_` entry
previously listed here was fixed on 2026-08-31; see above.)
