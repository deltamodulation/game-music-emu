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

### 2026-09-12 (6): 2A03 square/noise `get_osc_state()` also mirrors run()'s mute conditions (PR #573 review, user device report)

After the N163 fix above, the user found the same class of bug on the 2A03
side: `Nes_Apu::get_osc_state()`'s square (PUL1/PUL2) case gated `enabled` on
`length_counter > 0` alone, never checking the mute conditions
`Nes_Square::run()` itself applies (`volume() == 0`, `period < 8`, or a sweep
push moving `period >= 0x800`). The length counter is note-duration
bookkeeping; it does not go to zero just because the envelope decayed to
silence, so a channel could sit at `enabled=true`/`channel_vol=0` indefinitely
-- confirmed on real data (Megami Tensei II and user-provided King of Kings,
both showing PUL1/PUL2 stuck at `enabled=true, volume=0` for the entire
observed 300-frame window, ~19s). The noise case had the analogous gap
(`Nes_Noise::run()` mutes on `volume()==0`, `get_osc_state()` didn't check it).

Fixed by mirroring `Nes_Square::run()`'s mute check verbatim (same sweep
`offset`/`negate_flag` computation) and adding `volume()!=0` to the noise
case.

- `gme/Nes_Apu.cpp` `get_osc_state()`: square case now computes `period`,
  `volume`, and the sweep-overflow `offset` the same way `run()` does, and
  ANDs `!muted` into `enabled`; noise case ANDs `volume!=0` into `enabled`.

No ABI change. No change to `run()`/`run_until()` (audio output path
untouched). Verified locally: ctest (core host-debug preset) 378/378 green
(bit-exact golden unaffected -- this getter is not on the audio path).

### 2026-09-12 (7): N163 `get_osc_state()` comment-only follow-up (PR #573 review Round 2 L-5)

`freq != 0` in the `period` calculation's guard is unreachable once `enabled`
(which now implies `freq >= 64*active_oscs >= 64`) gates the whole block --
documented this alongside the pre-existing `wave_size != 0` unreachability
note. No code change, no behavior change.

### 2026-09-12 (8): 2A03 triangle get_osc_state() also mirrors run()'s timer_period floor (PR #573 review Round 3 M-11)

The square/noise fix above (entry 6) still left the same class of gap on the
triangle case: `Nes_Triangle::run()` mutes when
`!(length_counter && linear_counter && timer_period >= 3)` (`timer_period =
period() + 1`), but `get_osc_state()`'s triangle case only checked
`length_counter > 0 && linear_counter > 0`, missing `timer_period < 3`.

- `gme/Nes_Apu.cpp` `get_osc_state()`: triangle case now also requires
  `osc.period() + 1 >= 3` in `enabled` (computed once, matching `run()`'s
  `timer_period` naming/formula exactly).

No ABI change. No change to `run()`/`run_until()`. Verified locally: ctest
(core host-debug preset) 378/378 green (bit-exact golden unaffected).

**Known limitation (documented, not a defect in this fix)**: unlike the
square/noise fix, this one has no dedicated regression test. A host-side
dump of both real NSF files already in this repo's test corpus (King of
Kings, Megami Tensei II) over 300 render frames each found the triangle
channel's `period` never drops below ~257 (far above the `timer_period < 3`
threshold), so there is no real data available to exercise this branch, and
authoring a synthetic NSF ROM that drives the triangle at that extreme a
frequency is out of scope for this fix. The change is verified by direct
comparison against `Nes_Triangle::run()`'s condition (verbatim match) and by
the full existing test suite remaining green.

### 2026-09-13: NSF FDS + bank-switched titles -- `$5FF6`/`$5FF7` (`$6000`-`$7FFF` window) were never handled (Issue #578)

nt-chiptune-player Issue #578: a machine sweep of real NSF + M3U data found
21 tracks across 3 FDS-equipped, bank-switched titles (Almana no Kiseki,
Castlevania 2 - Simon's Quest, Famicom Grand Prix II) that never produced
audible output, plus one title (The Legend of Zelda 2) whose every track
failed to load at all.

Root cause, confirmed by instrumentation (temporary, not committed) that
dumped writes into `Nsf_Emu::cpu_write` and `Nes_Fds_Apu::write_` while
rendering Almana no Kiseki track 0/1: `Nsf_Emu` only ever mapped the 8
standard bank-select registers `$5FF8`-`$5FFF` (selecting 4KB pages into
`$8000`-`$FFFF`). Per the NSF spec, an FDS-equipped, bank-switched title also
uses `$5FF6`/`$5FF7` to bank-switch the `$6000`-`$7FFF` FDS RAM window
(initialized from header bank bytes 6/7, the same bytes already used for
`$5FFE`/`$5FFF`), but this fork's `cpu_write` had no case for those two
addresses -- writes to them silently fell through to the unmapped-write
debug path and were dropped. Since NSF drivers commonly stage a song's FDS
wavetable and/or code through that window before playing a note, the window
staying permanently zero-filled left either dead code (crash/no-op) or an
all-zero FDS wavetable (silent output) at that address range, depending on
the title. Separately, `Nsf_Emu::load_()` rejected `load_addr`/`init_addr`
below `rom_begin` ($8000) unconditionally; Zelda 2's NSF places `load_addr`
at `$6000` (inside the FDS window), which this floor rejected outright as
"Corrupt file", independent of the bank-switch bug.

Fixed, scoped to FDS-equipped, **bank-switched** titles only (`fds &&
fds_bankswitched`; a non-bank-switched FDS title -- header `banks[]` all
zero -- keeps the exact pre-fix behavior on both counts below, per PR #580
review M-1/M-2, see the 2026-09-13 follow-up entry further down for why the
initial version of this patch gated on `fds` alone and what changed):

- `gme/nes_cpu_io.h` `cpu_write()`: added a case for `$5FF6`/`$5FF7` (checked
  before the existing `$5FF8`-`$5FFF` bank_select_addr case, gated on
  `fds && fds_bankswitched`) that copies the selected 4KB ROM bank directly
  into the `sram` array at offset 0/`bank_size`. This mirrors the existing
  `$5FF8`-`$5FFF` case's bank lookup (`rom.mask_addr`/`rom.at_addr`) but
  targets `sram` instead of `cpu::map_code`, because reads of `$6000`-`$7FFF`
  already go through the flat `sram` array in `cpu_read()` (not through
  `cpu::get_code`), and that same `sram` buffer is what `map_code(sram_addr,
  ...)` in `start_track_` points CPU code fetches at -- so writing into
  `sram` keeps both instruction fetch and data reads of that window
  consistent with the newly-selected bank. A non-bank-switched FDS title's
  writes to `$5FF6`/`$5FF7` fall through to the unmapped-write debug path,
  same as before this fix (such a title has no declared bank data for this
  window and may be using `$6000`-`$7FFF` as ordinary work/battery RAM,
  already read/written unconditionally as flat `sram` elsewhere).
- `gme/Nsf_Emu.h`/`gme/Nsf_Emu.cpp`: added a `fds_bankswitched` member,
  detected in `load_()` from the header alone (nonzero `header_.banks[i]`,
  same condition the existing bank-assignment loop already used to decide
  whether to copy the raw header bank array into `initial_banks`) before the
  address-floor check below, since both need it. `start_track_()` also
  applies `initial_banks[6]`/`initial_banks[7]` to `$5FF6`/`$5FF7` via the
  new `cpu_write` case (in addition to their existing, unchanged application
  to `$5FFE`/`$5FFF` via the 8-register loop) when `fds && fds_bankswitched`,
  matching the NSF spec's initial-value rule for FDS titles.
- `gme/Nsf_Emu.cpp` `load_()`: the `load_addr < rom_begin || init_addr <
  rom_begin` rejection now uses `sram_addr` ($6000) instead of `rom_begin`
  ($8000) as the floor when `fds && fds_bankswitched`, since such titles may
  legitimately place load/init inside the FDS RAM window. A non-bank-switched
  FDS title keeps the original `rom_begin` floor and the original "Corrupt
  file" rejection for `load_addr` < $8000 (no declared bank data exists to
  seed the window in that case, so `first_bank` would otherwise go negative
  and only alias into $8000+ while leaving the window zero-filled).

Verified against real data (`test-data/nsf`, not committed; see
`docs/research/2026-09-13-issue578-nsf-silent-m3u-tracks.md` for the
before/after measurement): 9 of Almana no Kiseki's 10 previously
never-audible M3U tracks, and all of Castlevania 2's and Famicom Grand Prix
II's previously never-audible tracks, are now audible; Zelda 2 now loads
successfully. Track 0 ("Almana Stolen") remains silent after this fix -- its
FDS driver explicitly sets the wave-halt bit (`$4083` bit 7) during its init
routine and no further FDS register writes are observed for the remainder of
the song's declared length in this fork's emulation, which may be a genuine
compositional silence (a short stinger/cutscene cue) or a separate,
unconfirmed emulation gap; tracked as a known limitation, not re-opened by
this fix (see ADR 0070 addendum for Issue #578).

No ABI change. Bit-exact golden output unaffected: verified locally with
`ctest` (core host-debug preset), full suite green (see the 2026-09-13
follow-up entry below for the post-review test count). This fix's gate is
`fds && fds_bankswitched`, and `core/tests/golden/manifest.tsv` (2 NSF rows:
`Super Mario Bros. ...nsf` = 2A03-only, `vrc6-init.nsf` = VRC6-only, verified
by reading the manifest directly -- it has no `chip_flags` column to `git
grep` for) contains no FDS-equipped NSF, so the new code paths are not
exercised by the golden fixtures at all. Golden green therefore does not
demonstrate correctness of the new code paths; see nt-chiptune-player's ADR
0023 addendum for Issue #578 for what evidence does (real-data before/after
plus a corpus-invariance check over the tracks the fix does not touch).

### 2026-09-13 (follow-up): tighten the Issue #578 gate to `fds && fds_bankswitched` (PR #580 review M-1/M-2)

Code review on nt-chiptune-player PR #580 (M-1/M-2) found that the initial
version of the Issue #578 patch above gated `cpu_write()`'s new `$5FF6`/
`$5FF7` case on `fds` alone, while `start_track_()`'s initial-value
application already gated on `fds && fds_bankswitched`. This asymmetry meant
an FDS-equipped title that does **not** declare bank switching (header
`banks[]` all zero) would, after the initial patch, have any runtime write to
`$5FF6`/`$5FF7` treated as a bank-select and overwrite 4KB of its
`$6000`-`$7FFF` window with unrelated ROM bytes -- a window such a title may
be using as ordinary work/battery RAM. Separately, the `load_()` address
floor relaxation had the same gap: a non-bank-switched FDS title with
`load_addr` < `$8000` would newly pass the address check (gate was `fds`
alone) but get no bank-6/7 seed data for the window (only bank-switched
titles copy `header_.banks` into `initial_banks`), turning a previous
explicit "Corrupt file" rejection into a silent, hard-to-diagnose failure.

Fixed by moving the bank-switch declaration check (nonzero `header_.banks[i]`)
earlier in `load_()` -- it no longer depends on `load_addr`/`rom.set_addr()`
having run -- and using it consistently in all three places: the `cpu_write()`
`$5FF6`/`$5FF7` case, `start_track_()`'s initial-value application (already
correct), and the `load_()` address floor. A non-bank-switched FDS title now
gets byte-for-byte the same behavior as before the Issue #578 patch on both
counts: `$5FF6`/`$5FF7` writes fall through to the unmapped-write debug path,
and `load_addr`/`init_addr` < `$8000` is still rejected as "Corrupt file".

No real-data title in this repo's `test-data/nsf` corpus exercises the
non-bank-switched-FDS-with-low-load-addr combination this closes a gap for
(all 4 titles affected by the original Issue #578 fix are bank-switched), so
this change is unverified against real data; it is verified by (a) the
existing Issue #578 regression tests (2 real-data + 2 synthetic, unaffected
-- all 4 titles are bank-switched) remaining green, and (b) code reading
confirming the three gate sites now use the identical condition.

Also added, same review round:
- `gme/Nsf_Emu.cpp` constructor: initialize `fds_bankswitched = false`
  (M-1's sibling nit L-1 -- matches the existing `fds = 0` etc. discipline;
  `load_()` was already the only place it was previously set, before any
  code path could read it, so this is defense-in-depth, not a fix for an
  observed bug).

Files touched: `gme/nes_cpu_io.h`, `gme/Nsf_Emu.cpp`. No ABI change. Verified
locally: `ctest` (core host-debug preset) full suite green (382 tests: adds
`NsfEngine.FdsInitialBankApplicationEnablesAudioSynthetic`, a second CI-run
synthetic regression test for the `start_track_()` initial-application path
specifically, per PR #580 review M-6).

### Issue #587: pack the 2A03 noise period-table index instead of discarding it

`Nes_Apu::get_osc_state()`'s noise case (`index == 3`) previously always set
`out->period = 0` with the comment "not pitched in the musical sense; keycode
stays invalid". nt-chiptune-player (ADR 0072) now maps this channel's period
register to a keyboard position for visualization purposes (not a claim that
it is a musical pitch), which needs the raw 4-bit noise period-table index
(`regs[2] & 15`, values 0-15) to do the mapping. Changed `period` to carry
that index instead of the constant 0 -- ADR 0023 classification 1
(observation-only addition; `run_until()` and all other emulation behavior
are unchanged, the field is read-only and post-render). Updated the `period`
field doc comment in `gme.h` to note this per-voice meaning change (0 is now
a valid index for this voice, not "silent").

Files touched: `gme/Nes_Apu.cpp`, `gme/gme.h`. No ABI change (field width/
offset unchanged). `grep -rn "\.period" core/src core/tests` in the
superproject confirms no other consumer treats this voice's `period == 0` as
"invalid" after the nsf_engine.cpp change made in the same PR.

### Issue #651: pack the 2A03 DMC $4010 rate-index instead of discarding it

`Nes_Apu::get_osc_state()`'s dmc case (`index == 4`) previously always set
`out->period = 0` with the comment "playback rate, not a musical pitch".
nt-chiptune-player (ADR 0076) now maps this channel's `$4010` rate register to
a keyboard position for visualization purposes (not a claim that it is a
musical pitch), which needs the raw 4-bit rate index (`regs[0] & 0x0F`, values
0-15) to do the mapping -- same convention as the Issue #587 noise-voice
change above. Changed `period` to carry that index instead of the constant 0
-- ADR 0023 classification 1 (observation-only addition; `run_until()` and
all other emulation behavior are unchanged, the field is read-only and
post-render). Updated the `period` field doc comment in `gme.h` to note this
per-voice meaning.

Files touched: `gme/Nes_Apu.cpp`, `gme/gme.h`. No ABI change (field width/
offset unchanged).

### 2026-09-17 -- Add read-only `gme_spc_channel_state` C API for SPC (Issue #591)

Adds a minimal, additions-only C API (`gme_spc_channel_state` /
`gme_spc_set_observe_interval_ms` in `gme/gme.h`) for the SPC (SNES S-DSP)
emulator, following the same design rationale as the HES/NSF additions above
(ADR 0023 classification 1: additions only, no existing behavior changed).

`gme_spc_channel_state` exposes exactly four S-DSP register fields per voice
(`pitch`, `non`, `noise_rate`, `envx` -- see ADR 0075 裁定 7 for why only
these four): `Spc_Dsp::read()` was already `public`, but `Snes_Spc::dsp` and
`Spc_Emu::apu` are both `private`, so a new accessor was needed at each layer
to reach it from outside:
- `gme/Snes_Spc.h`: added `int dsp_read( int addr ) const { return dsp.read( addr ); }`.
- `gme/Spc_Emu.h` / `gme/Spc_Emu.cpp`: added `Spc_Emu::channel_state()`,
  which decodes the pitch/non/noise_rate/envx fields from the raw register
  values and is the only caller of the new `dsp_read()`.

`gme_spc_set_observe_interval_ms` mirrors `gme_hes_set_observe_interval_ms` /
`gme_nsf_set_observe_interval_ms`'s contract (opt-in, valid range 1..1000 ms,
call after load and before start_track) but not their mechanism: `Spc_Emu` is
not a `Classic_Emu` subclass, so `Classic_Emu::set_buffer_length_ms`
(Blip_Buffer resize) does not apply. Instead, `Spc_Emu::set_observe_interval_ms()`
resizes the `Fir_Resampler<24> resampler` member that `Spc_Emu::set_sample_rate_()`
already allocates at a fixed 50ms (`native_sample_rate / 20 * 2`) -- the same
buffer whose size bounds how often `play_()` re-enters the emulation core when
resampling is active. When the output rate equals `native_sample_rate`
(32000 Hz) the resampler is bypassed entirely (`play_()` takes the
`sample_rate() == native_sample_rate` branch straight into `play_and_filter`),
so the call is then a harmless no-op and the caller's own `gme_play()` chunk
size determines the effective granularity.

Both new C API entry points do their own `me->type() != Spc_Emu::static_type()`
check (RTTI is disabled in this build) and `gme_spc_channel_state` range-checks
`index` against `Snes_Spc::voice_count` (8) before it reaches
`Spc_Emu::channel_state()` -- same discipline as the HES/NSF entry points
(SEC-L-1: a caller bug must not translate into an out-of-bounds `Spc_Dsp`
register read across the C ABI boundary).

Files touched: `gme/gme.h`, `gme/gme.exports`, `gme/Snes_Spc.h`,
`gme/Spc_Emu.h`, `gme/Spc_Emu.cpp`.

### 2026-09-18 -- Rename `Snes_Spc::dsp_read` and add defense-in-depth range checks (PR #721 review, nt-chiptune-player#591)

Follow-up to the 2026-09-17 addition above, same Issue/PR. Two changes, both
additions-only / no behavior change to any existing code path (ADR 0023
classification 1):

- Renamed the new accessor from `dsp_read( int addr )` to `read_dsp_reg( int addr )`.
  `Snes_Spc` already declares a private `int dsp_read( rel_time_t )` (an
  unrelated CPU-time-scheduled $F2/$F3 port read, part of the pre-existing
  implementation). The two are overload-distinct by parameter type, so this
  was not a compile error, but the name collision invited confusion at call
  sites -- caught in nt-chiptune-player PR #721 code review.
- `read_dsp_reg` now range-checks `addr` itself (`(unsigned) addr <
  (unsigned) Spc_Dsp::register_count`) instead of trusting the caller, and
  `Spc_Emu::channel_state()` now also range-checks its own `i` parameter and
  zero-fills `*out` on an out-of-range index. `gme_spc_channel_state`
  (the C API) already range-checked `index` before this change, so no
  previously-reachable behavior changes; this is defense-in-depth for any
  future second caller added inside this fork (PR #721 review SEC-L-1/SEC-L-2).

Files touched: `gme/Snes_Spc.h`, `gme/Spc_Emu.cpp`.

### 2026-09-18 -- Add read-only `gme_gbs_channel_state` C API for GBS (Issue #599)

Adds a minimal, additions-only C API (`gme_gbs_channel_state` /
`gme_gbs_set_observe_interval_ms` in `gme/gme.h`) for the GBS (Game Boy
`Gb_Apu`) emulator, following the same design rationale as the SPC addition
above (ADR 0023 classification 1: additions only, no existing behavior
changed; ADR 0081 裁定 7).

`gme_gbs_channel_state` exposes three fields per voice (`keyon`, `volume`,
`period` -- see ADR 0081 裁定 5/6/7 for why only these three; duty, sweep,
wave RAM and the NR50/NR51 pan bits are deliberately left out). `keyon` is
not a single raw register bit: `Gb_Apu::get_osc_state()` (new, `gme/Gb_Apu.h`
/ `gme/Gb_Apu.cpp`) recomputes the full "would this voice currently be
audible" condition directly from the oscillator's registers -- the same gate
`Gb_Apu::run_until()` uses, plus the Square 1 sweep-overflow silence and the
Square/Wave out-of-range-frequency DC conditions that `Gb_Square::run()` /
`Gb_Wave::run()` apply -- because `run_until()` only evaluates any of this
when `osc.output` is non-NULL, so a muted voice (`gme_mute_voice()`) would
otherwise read as permanently silent regardless of its actual register state
(ADR 0081 裁定 5, ADR 0071 mute-row keyboard display). `Gbs_Emu::channel_state()`
(new, `gme/Gbs_Emu.h`) dispatches straight through to `apu.get_osc_state()`
since GBS has exactly one chip.

`gme_gbs_set_observe_interval_ms` mirrors `gme_hes_set_observe_interval_ms` /
`gme_nsf_set_observe_interval_ms`'s contract and mechanism exactly: `Gbs_Emu`
is a `Classic_Emu` subclass, so `Gbs_Emu::set_observe_interval_ms()` (new,
`gme/Gbs_Emu.h`) is a one-line re-export of the existing protected
`Classic_Emu::set_buffer_length_ms()` -- no code is added to `Classic_Emu`
itself (ADR 0060 裁定 2 制約 3, ADR 0081 裁定 7).

Both new C API entry points do their own `me->type() != Gbs_Emu::static_type()`
check (RTTI is disabled in this build) and `gme_gbs_channel_state`
range-checks `index` against `gme_voice_count()` (4) before it reaches
`Gbs_Emu::channel_state()` / `Gb_Apu::get_osc_state()` (which also asserts
the range via `require()`) -- same discipline as the HES/NSF/SPC entry
points (a caller bug must not translate into an out-of-bounds `Gb_Apu::oscs`
read across the C ABI boundary).

Files touched: `gme/gme.h`, `gme/gme.exports`, `gme/Gb_Apu.h`,
`gme/Gb_Apu.cpp`, `gme/Gbs_Emu.h`, `gme/Gbs_Emu.cpp`.

### 2026-09-18 -- Add missing per-file LGPL-2.1 2(a) notices to `Gb_Apu.cpp` / `Gbs_Emu.cpp` (nt-chiptune-player PR #738 review)

The GBS addition above (2026-09-18 entry) added the dated notice comment
(`Modified 2026-09-18 by nt-chiptune-player project -- see
NTCP-MODIFICATIONS.md`) to `gme/Gb_Apu.h` and `gme/Gbs_Emu.h`, but missed the
two `.cpp` files that actually carry the new code (`gme/Gb_Apu.cpp`,
`gme/Gbs_Emu.cpp`). This commit adds the same notice line to both, matching
the existing convention used by the HES/NSF/SPC additions.

Also clarifies a comment in `Gb_Apu::get_osc_state()`: the function
unconditionally zero-fills `*out` before any error check, so `gme.h`'s "*out
is left unmodified" error contract for `gme_gbs_channel_state` holds only
because the C wrapper range-checks `index` and returns before reaching this
function -- a note added to make that dependency explicit for future callers.

No behavior change; comment/notice-only.

Files touched: `gme/Gb_Apu.cpp`, `gme/Gbs_Emu.cpp`.

### 2026-09-22 -- NSF: keep muted voices running into a discarded buffer (Issue #812)

Behavior change in `Nsf_Emu` (not an addition-only patch; audible output is
unchanged). `Classic_Emu::mute_voices_()` mutes a voice by calling
`set_voice( i, 0, 0, 0 )`, i.e. by giving the oscillator a NULL output.
The 2A03 DMC, the VRC6 saw and the FDS oscillator return early when their
output is NULL, *before* they advance their own state (DMC `dac` / `silence`,
the VRC6 saw accumulator `amp`, FDS `env_gain` / `last_amp`). The values
`gme_nsf_channel_state()` derives `enabled` / `channel_vol` from therefore
froze while a voice was muted, so a visualizer or a silence detector reading
them saw a different song than the audible one (nt-chiptune-player Issue #812,
contract C-18 (1)).

`Nsf_Emu` now owns a `Blip_Buffer mute_sink_`. `Nsf_Emu::set_voice()` replaces
a NULL buffer with `&mute_sink_`, so a muted voice runs exactly as an audible
one and its output lands in a buffer nothing reads. `run_clocks()` clears the
sink after every chip's `end_frame()` (the expansion chips run their
oscillators in `end_frame()`). `load_()` sizes the sink after `setup_buffer()`
with the same sample rate and clock rate as the main buffer -- the N163
oscillator derives its wave-step timing from its output buffer's rate
(`resampled_time` / `resampled_duration`), so a sink at another rate would let
a muted N163 voice drift against an audible one -- and 1000 ms, the longest
length `set_buffer_length_ms()` accepts. The allocation error is propagated.

Nothing else changes: with no voice muted the sink is never written and
`clear_modified()` skips the `clear()`, so the audible path is unchanged
(nt-chiptune-player's `Golden.BitExactAgainstManifest`, plus a PCM hash
comparison of 10 s of every track 0-2 of every NSF in the local corpus, are
identical before and after). `Classic_Emu` and the other emulators
(HES / GBS / SPC / KSS) are untouched.

Files touched: `gme/Nsf_Emu.h`, `gme/Nsf_Emu.cpp`.

## Known upstream bugs (not modified)

Bugs found in upstream code during nt-chiptune-player development that this fork
does **not** patch (out of scope for the fix that found them, or not yet
prioritized). Listed here so they aren't rediscovered from scratch; see the
linked issue for detail and status.

(None at this time -- the `Hes_Emu::cpu_write` / `CPU_WRITE_FAST_` entry
previously listed here was fixed on 2026-08-31; see above.)
