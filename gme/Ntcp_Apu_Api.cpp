// nt-chiptune-player fork addition (Issue #1946 / ADR 0152): standalone APU C API.
// Added 2026-11-05 by nt-chiptune-player project -- see NTCP-MODIFICATIONS.md
//
// Drives the fork's existing sound-chip classes (Nes_Apu [+ Nes_Fds_Apu],
// Gb_Apu, Hes_Apu) directly from register writes, without a Music_Emu or CPU.
// Only the classes' existing public members are used (constructor, volume,
// treble_eq, osc_output, reset, write_register / write / write_data,
// end_frame, dmc_reader, get_osc_state); no existing code is changed. The
// contract of every entry point is documented in gme.h (the normative text).

/* Copyright (C) 2026 nt-chiptune-player project. This module is free
software; you can redistribute it and/or modify it under the terms of the GNU
Lesser General Public License as published by the Free Software Foundation;
either version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "gme.h"
#include "gme_types.h"
#include "Blip_Buffer.h"

#ifdef USE_GME_NSF
	#include "Nes_Apu.h"
	#include "Nes_Fds_Apu.h"
#endif
#ifdef USE_GME_GBS
	#include "Gb_Apu.h"
#endif
#ifdef USE_GME_HES
	#include "Hes_Apu.h"
#endif

#include <new>
#include <string.h>

namespace {

// Same Blip_Buffer length Classic_Emu::set_sample_rate_ uses (1000 / 20 ms).
// Its length bounds how many frames one end_frame() may cover.
int const buffer_msec = 50;

// Frames emulated per internal step. Must stay well below the buffer length
// (2448 samples at 48000 Hz / 50 ms, 408 at the 8000 Hz minimum that
// gme_apu_new accepts) minus Blip_Buffer's impulse width, because
// Blip_Synth checks its write position with assert() only (no check under
// NDEBUG). gme_apu_render() splits longer requests into steps of this size.
int const max_step = 256;

long const min_sample_rate = 8000;
long const max_sample_rate = 96000;
long const max_clock_rate  = 20000000; // chip time units per second (HuC6280: after doubling)

// Default volume / treble / bass of the Music_Emu subclass that plays the same
// chip (see NTCP-MODIFICATIONS.md): Nsf_Emu set_gain( 1.4 ) and nes_eq
// (-1.0 dB, 80 Hz), x0.75 when the FDS is present (Nsf_Emu::init_sound's
// adjusted_gain for one expansion chip); Gbs_Emu set_gain( 1.2 ) and
// make_equalizer( -1.0, 120 ); Hes_Emu set_gain( 1.11 ) and the Music_Emu
// default equalizer (-1.0 dB, 60 Hz).
double const treble_db = -1.0;

#ifdef USE_GME_GBS
// Value-for-value copy of Gbs_Emu.cpp's sound_data: the 48 register bytes
// Gbs_Emu::start_track_ writes after Gb_Apu::reset() (ADR 0152 U-8).
unsigned char const gb_initial_regs [Gb_Apu::register_count] = {
	0x80, 0xBF, 0x00, 0x00, 0xBF, // square 1
	0x00, 0x3F, 0x00, 0x00, 0xBF, // square 2
	0x7F, 0xFF, 0x9F, 0x00, 0xBF, // wave
	0x00, 0xFF, 0x00, 0x00, 0xBF, // noise
	0x77, 0xF3, 0xF1, // vin/volume, status, power mode
	0, 0, 0, 0, 0, 0, 0, 0, 0, // unused
	0xAC, 0xDD, 0xDA, 0x48, 0x36, 0x02, 0xCF, 0x16, // waveform data
	0x2C, 0x04, 0xE5, 0x2C, 0xAC, 0xDD, 0xDA, 0x48
};
#endif

}

struct gme_apu_t
{
	int  type;
	int  fds;
	int  mute_mask;    // bit i = voice i is muted (routed to `discard`)
	Blip_Buffer bufs [3]; // center, left, right (NES uses center only)
	Blip_Buffer discard;  // muted voices' output; cleared after every step
#ifdef USE_GME_NSF
	Nes_Apu nes;
	Nes_Fds_Apu fds_apu;
	unsigned char* mem; // DMC memory image (64 KiB, NES only)
#endif
#ifdef USE_GME_GBS
	Gb_Apu gb;
#endif
#ifdef USE_GME_HES
	Hes_Apu hes;
#endif
};

namespace {

int voice_count( gme_apu_t const* a )
{
	switch ( a->type )
	{
	case gme_apu_nes:     return 5 + (a->fds ? 1 : 0);
	case gme_apu_gb:      return 4;
	case gme_apu_huc6280: return 6;
	}
	return 0;
}

#ifdef USE_GME_NSF
int read_dmc( void* user, nes_addr_t addr )
{
	return static_cast<gme_apu_t*>( user )->mem [addr & 0xFFFF];
}
#endif

// Point voice `i` at the real buffers, or at `discard` when muted. A muted
// voice keeps a non-NULL output so the chip keeps evaluating it (NULL makes
// Gb_Apu skip the oscillator and Nes_Apu "reduces emulation accuracy"), which
// keeps get_osc_state() and the sequence identical to the unmuted run.
void route_voice( gme_apu_t* a, int i )
{
	bool const muted = (a->mute_mask >> i) & 1;
	Blip_Buffer* c = muted ? &a->discard : &a->bufs [0];
	Blip_Buffer* l = muted ? &a->discard : &a->bufs [1];
	Blip_Buffer* r = muted ? &a->discard : &a->bufs [2];
	switch ( a->type )
	{
#ifdef USE_GME_NSF
	case gme_apu_nes:
		if ( i < Nes_Apu::osc_count )
			a->nes.osc_output( i, c );
		else
			a->fds_apu.osc_output( 0, c );
		break;
#endif
#ifdef USE_GME_GBS
	case gme_apu_gb:
		a->gb.osc_output( i, c, l, r );
		break;
#endif
#ifdef USE_GME_HES
	case gme_apu_huc6280:
		a->hes.osc_output( i, c, l, r );
		break;
#endif
	}
	(void) c; (void) l; (void) r;
}

// Rebuild the chip object(s) in place (no allocation) and bring them to the
// state the matching *_Emu::start_track_ leaves them in (ADR 0152 U-8).
// Constructing anew makes reset bit-exact with creation by construction.
void init_chip( gme_apu_t* a )
{
	blip_eq_t const eq( treble_db );
	switch ( a->type )
	{
#ifdef USE_GME_NSF
	case gme_apu_nes: {
		double const gain = a->fds ? 1.4 * 0.75 : 1.4;
		a->nes.~Nes_Apu();
		::new (&a->nes) Nes_Apu;
		a->nes.dmc_reader( read_dmc, a );
		a->nes.volume( gain );
		a->nes.treble_eq( eq );
		if ( a->fds )
		{
			a->fds_apu.~Nes_Fds_Apu();
			::new (&a->fds_apu) Nes_Fds_Apu;
			a->fds_apu.volume( gain );
			a->fds_apu.treble_eq( eq );
		}
		for ( int i = 0; i < voice_count( a ); i++ )
			route_voice( a, i );
		// Nsf_Emu::start_track_ (NTSC; initial DMC DAC 0; $4017 = 0, i.e. no
		// PAL-only / DMC-init header flags), then Nes_Fds_Apu::reset().
		a->nes.reset( false, 0 );
		a->nes.write_register( 0, 0x4015, 0x0F );
		a->nes.write_register( 0, 0x4017, 0x00 );
		if ( a->fds )
			a->fds_apu.reset();
		break;
	}
#endif
#ifdef USE_GME_GBS
	case gme_apu_gb:
		a->gb.~Gb_Apu();
		::new (&a->gb) Gb_Apu;
		a->gb.volume( 1.2 );
		a->gb.treble_eq( eq );
		for ( int i = 0; i < Gb_Apu::osc_count; i++ )
			route_voice( a, i );
		// Gbs_Emu::start_track_
		a->gb.reset();
		for ( int i = 0; i < (int) sizeof gb_initial_regs; i++ )
			a->gb.write_register( 0, i + Gb_Apu::start_addr, gb_initial_regs [i] );
		break;
#endif
#ifdef USE_GME_HES
	case gme_apu_huc6280:
		a->hes.~Hes_Apu();
		::new (&a->hes) Hes_Apu;
		a->hes.volume( 1.11 );
		a->hes.treble_eq( eq );
		for ( int i = 0; i < Hes_Apu::osc_count; i++ )
			route_voice( a, i );
		// Hes_Emu::start_track_ does Hes_Apu::reset() only (the constructor
		// already did it).
		a->hes.reset();
		break;
#endif
	}
	(void) eq;
}

int bass_freq( int type )
{
	switch ( type )
	{
	case gme_apu_nes: return 80;
	case gme_apu_gb:  return 120;
	}
	return 60;
}

inline void clamp16( blip_long& s )
{
	if ( (blip_sample_t) s != s )
		s = 0x7FFF - (s >> 24); // same saturation as Stereo_Buffer
}

// Emulate exactly up to the time that makes `n` more samples available, then
// mix those `n` samples. Blip_Buffer::count_clocks() returns the smallest
// such time, so the end time after any sequence of steps depends only on the
// total number of samples produced -- output is independent of how a render
// request is split (ADR 0152 ruling 3, condition 3).
void run_step( gme_apu_t* a, short* out, int n )
{
	blip_time_t const t = a->bufs [0].count_clocks( n );
	int const nbufs = (a->type == gme_apu_nes) ? 1 : 3;
	switch ( a->type )
	{
#ifdef USE_GME_NSF
	case gme_apu_nes:
		a->nes.end_frame( t );
		if ( a->fds )
			a->fds_apu.end_frame( t );
		break;
#endif
#ifdef USE_GME_GBS
	case gme_apu_gb:
		a->gb.end_frame( t );
		break;
#endif
#ifdef USE_GME_HES
	case gme_apu_huc6280:
		a->hes.end_frame( t );
		break;
#endif
	}
	for ( int i = 0; i < nbufs; i++ )
		a->bufs [i].end_frame( t );

	// Drop what the muted voices wrote (nothing reads it). Same as Nsf_Emu's
	// mute_sink_ (Issue #812): never end_frame()d, so its time offset stays 0
	// and every step writes within the first step-length of the buffer;
	// clear() after every step keeps unread samples from piling up there.
	if ( a->discard.clear_modified() )
		a->discard.clear();

	if ( nbufs == 1 )
	{
		int const bass = BLIP_READER_BASS( a->bufs [0] );
		BLIP_READER_BEGIN( center, a->bufs [0] );
		for ( int k = 0; k < n; k++ )
		{
			blip_long s = BLIP_READER_READ( center );
			clamp16( s );
			BLIP_READER_NEXT( center, bass );
			out [0] = (short) s;
			out [1] = (short) s;
			out += 2;
		}
		BLIP_READER_END( center, a->bufs [0] );
	}
	else
	{
		// Always the three-buffer mix (Stereo_Buffer::mix_stereo). Stereo_Buffer
		// switches to a center-only mix depending on which buffers were written
		// in a frame, which would make the output depend on the step lengths.
		int const bass = BLIP_READER_BASS( a->bufs [1] );
		BLIP_READER_BEGIN( center, a->bufs [0] );
		BLIP_READER_BEGIN( left, a->bufs [1] );
		BLIP_READER_BEGIN( right, a->bufs [2] );
		for ( int k = 0; k < n; k++ )
		{
			blip_long c = BLIP_READER_READ( center );
			blip_long l = c + BLIP_READER_READ( left );
			blip_long r = c + BLIP_READER_READ( right );
			clamp16( l );
			clamp16( r );
			BLIP_READER_NEXT( center, bass );
			BLIP_READER_NEXT( left, bass );
			BLIP_READER_NEXT( right, bass );
			out [0] = (short) l;
			out [1] = (short) r;
			out += 2;
		}
		BLIP_READER_END( center, a->bufs [0] );
		BLIP_READER_END( left, a->bufs [1] );
		BLIP_READER_END( right, a->bufs [2] );
	}
	for ( int i = 0; i < nbufs; i++ )
		a->bufs [i].remove_samples( n );
}

}

extern "C" {

BLARGG_EXPORT gme_err_t gme_apu_new( int type, int flags, long clock_rate, long sample_rate, gme_apu_t** out )
{
	if ( !out )
		return "NULL parameter";
	*out = 0;

	long chip_clock = clock_rate;
	switch ( type )
	{
	case gme_apu_nes:
#ifndef USE_GME_NSF
		return "APU type not built";
#endif
		break;
	case gme_apu_gb:
#ifndef USE_GME_GBS
		return "APU type not built";
#endif
		break;
	case gme_apu_huc6280:
#ifndef USE_GME_HES
		return "APU type not built";
#endif
		// Hes_Apu counts the waveform period as period * 2 (Hes_Apu.cpp), so its
		// time unit is twice the PSG input clock (Hes_Emu uses 7159091 Hz).
		if ( clock_rate > max_clock_rate / 2 )
			return "Clock rate out of range";
		chip_clock = clock_rate * 2;
		break;
	default:
		return "Invalid APU type";
	}
	if ( flags & ~gme_apu_flag_fds )
		return "Invalid APU flags";
	if ( (flags & gme_apu_flag_fds) && type != gme_apu_nes )
		return "Invalid APU flags";
	if ( sample_rate < min_sample_rate || sample_rate > max_sample_rate )
		return "Sample rate out of range";
	// At least 4 chip clocks per output sample, so Blip_Buffer's 16-bit
	// clock/sample ratio keeps its precision; at most max_clock_rate so one
	// step's clock count stays far inside blip_time_t.
	if ( chip_clock < sample_rate * 4 || chip_clock > max_clock_rate )
		return "Clock rate out of range";

	gme_apu_t* a = new (std::nothrow) gme_apu_t;
	if ( !a )
		return "Out of memory";
	a->type = type;
	a->fds = (flags & gme_apu_flag_fds) ? 1 : 0;
	a->mute_mask = 0;
#ifdef USE_GME_NSF
	a->mem = 0;
	if ( type == gme_apu_nes )
	{
		a->mem = new (std::nothrow) unsigned char [0x10000];
		if ( !a->mem )
		{
			delete a;
			return "Out of memory";
		}
		memset( a->mem, 0, 0x10000 );
	}
#endif
	Blip_Buffer* const all [4] = { &a->bufs [0], &a->bufs [1], &a->bufs [2], &a->discard };
	for ( int i = 0; i < 4; i++ )
	{
		gme_err_t err = all [i]->set_sample_rate( sample_rate, buffer_msec );
		if ( err )
		{
			gme_apu_delete( a );
			return err;
		}
		all [i]->clock_rate( (uint32_t) chip_clock );
		all [i]->bass_freq( bass_freq( type ) );
	}
	init_chip( a );
	*out = a;
	return 0;
}

BLARGG_EXPORT void gme_apu_delete( gme_apu_t* a )
{
	if ( !a )
		return;
#ifdef USE_GME_NSF
	delete [] a->mem;
#endif
	delete a;
}

BLARGG_EXPORT void gme_apu_reset( gme_apu_t* a )
{
	if ( !a )
		return;
	// Buffers first: init_chip()'s register writes at time 0 put deltas into
	// them, exactly as in gme_apu_new().
	for ( int i = 0; i < 3; i++ )
		a->bufs [i].clear();
	a->discard.clear();
#ifdef USE_GME_NSF
	if ( a->mem )
		memset( a->mem, 0, 0x10000 );
#endif
	init_chip( a );
}

BLARGG_EXPORT void gme_apu_write( gme_apu_t* a, unsigned addr, int data )
{
	if ( !a )
		return;
	data &= 0xFF;
	switch ( a->type )
	{
#ifdef USE_GME_NSF
	case gme_apu_nes:
		if ( addr >= Nes_Apu::start_addr && addr <= Nes_Apu::end_addr && addr != 0x4014 && addr != 0x4016 )
			a->nes.write_register( 0, addr, data );
		else if ( a->fds && addr >= Nes_Fds_Apu::io_addr && addr < Nes_Fds_Apu::io_addr + Nes_Fds_Apu::io_size )
			a->fds_apu.write( 0, addr, data );
		break;
#endif
#ifdef USE_GME_GBS
	case gme_apu_gb:
		if ( addr >= Gb_Apu::start_addr && addr <= Gb_Apu::end_addr )
			a->gb.write_register( 0, addr, data );
		break;
#endif
#ifdef USE_GME_HES
	case gme_apu_huc6280:
		if ( addr >= Hes_Apu::start_addr && addr <= Hes_Apu::end_addr )
			a->hes.write_data( 0, (int) addr, data );
		break;
#endif
	}
}

BLARGG_EXPORT void gme_apu_render( gme_apu_t* a, short* out, int frames )
{
	if ( !a || !out )
		return;
	while ( frames > 0 )
	{
		int const n = frames < max_step ? frames : max_step;
		run_step( a, out, n );
		out += n * 2;
		frames -= n;
	}
}

BLARGG_EXPORT gme_err_t gme_apu_mute_voice( gme_apu_t* a, int index, int mute )
{
	if ( !a )
		return "NULL parameter";
	if ( (unsigned) index >= (unsigned) voice_count( a ) )
		return "Voice index out of range";
	if ( mute )
		a->mute_mask |= 1 << index;
	else
		a->mute_mask &= ~(1 << index);
	route_voice( a, index );
	return 0;
}

BLARGG_EXPORT void gme_apu_write_memory( gme_apu_t* a, unsigned addr, void const* data, long size )
{
#ifdef USE_GME_NSF
	if ( !a || !a->mem || !data || size <= 0 || addr > 0xFFFF )
		return;
	long const room = 0x10000 - (long) addr;
	memcpy( a->mem + addr, data, (size_t) (size < room ? size : room) );
#else
	(void) a; (void) addr; (void) data; (void) size;
#endif
}

BLARGG_EXPORT gme_err_t gme_apu_nes_channel_state( gme_apu_t const* a, int index, gme_nsf_channel_state_t* out )
{
	if ( !a || !out )
		return "NULL parameter";
	if ( a->type != gme_apu_nes )
		return "Not a NES APU";
	if ( (unsigned) index >= (unsigned) voice_count( a ) )
		return "Voice index out of range";
#ifdef USE_GME_NSF
	if ( index < Nes_Apu::osc_count )
		a->nes.get_osc_state( index, out );
	else
		a->fds_apu.get_osc_state( 0, out );
#endif
	return 0;
}

BLARGG_EXPORT gme_err_t gme_apu_gb_channel_state( gme_apu_t const* a, int index, gme_gbs_channel_state_t* out )
{
	if ( !a || !out )
		return "NULL parameter";
	if ( a->type != gme_apu_gb )
		return "Not a Game Boy APU";
	if ( (unsigned) index >= (unsigned) voice_count( a ) )
		return "Voice index out of range";
#ifdef USE_GME_GBS
	a->gb.get_osc_state( index, out );
#endif
	return 0;
}

BLARGG_EXPORT gme_err_t gme_apu_huc6280_channel_state( gme_apu_t const* a, int index, gme_hes_channel_state_t* out )
{
	if ( !a || !out )
		return "NULL parameter";
	if ( a->type != gme_apu_huc6280 )
		return "Not a HuC6280 APU";
	if ( (unsigned) index >= (unsigned) voice_count( a ) )
		return "Voice index out of range";
#ifdef USE_GME_HES
	a->hes.get_osc_state( index, out );
#endif
	return 0;
}

}
