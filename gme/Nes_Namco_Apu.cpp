// Nes_Snd_Emu 0.1.8. http://www.slack.net/~ant/
// Modified 2026-09-12 by nt-chiptune-player project -- see NTCP-MODIFICATIONS.md

#include "Nes_Namco_Apu.h"

#include <string.h>

/* Copyright (C) 2003-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "blargg_source.h"

Nes_Namco_Apu::Nes_Namco_Apu()
{
	output( NULL );
	volume( 1.0 );
	reset();
}

void Nes_Namco_Apu::reset()
{
	last_time = 0;
	addr_reg = 0;

	int i;
	for ( i = 0; i < reg_count; i++ )
		reg [i] = 0;

	for ( i = 0; i < osc_count; i++ )
	{
		Namco_Osc& osc = oscs [i];
		osc.delay = 0;
		osc.last_amp = 0;
		osc.wave_pos = 0;
	}
}

void Nes_Namco_Apu::output( Blip_Buffer* buf )
{
	for ( int i = 0; i < osc_count; i++ )
		osc_output( i, buf );
}

/*
void Nes_Namco_Apu::reflect_state( Tagged_Data& data )
{
	reflect_int16( data, BLARGG_4CHAR('A','D','D','R'), &addr_reg );

	static const char hex [17] = "0123456789ABCDEF";
	int i;
	for ( i = 0; i < reg_count; i++ )
		reflect_int16( data, 'RG\0\0' + hex [i >> 4] * 0x100 + hex [i & 15], &reg [i] );

	for ( i = 0; i < osc_count; i++ )
	{
		reflect_int32( data, BLARGG_4CHAR('D','L','Y','0') + i, &oscs [i].delay );
		reflect_int16( data, BLARGG_4CHAR('P','O','S','0') + i, &oscs [i].wave_pos );
	}
}
*/

void Nes_Namco_Apu::end_frame( blip_time_t time )
{
	if ( time > last_time )
		run_until( time );

	assert( last_time >= time );
	last_time -= time;
}

void Nes_Namco_Apu::run_until( blip_time_t nes_end_time )
{
	int active_oscs = (reg [0x7F] >> 4 & 7) + 1;
	for ( int i = osc_count - active_oscs; i < osc_count; i++ )
	{
		Namco_Osc& osc = oscs [i];
		Blip_Buffer* output = osc.output;
		if ( !output )
			continue;
		output->set_modified();

		blip_resampled_time_t time =
				output->resampled_time( last_time ) + osc.delay;
		blip_resampled_time_t end_time = output->resampled_time( nes_end_time );
		osc.delay = 0;
		if ( time < end_time )
		{
			const uint8_t* osc_reg = &reg [i * 8 + 0x40];
			if ( !(osc_reg [4] & 0xE0) )
				continue;

			int volume = osc_reg [7] & 15;
			if ( !volume )
				continue;

			int32_t freq = (osc_reg [4] & 3) * 0x10000 + osc_reg [2] * 0x100L + osc_reg [0];
			if ( freq < 64 * active_oscs )
				continue; // prevent low frequencies from excessively delaying freq changes
			blip_resampled_time_t period =
					output->resampled_duration( 983040 ) / freq * active_oscs;

			int wave_size = 32 - (osc_reg [4] >> 2 & 7) * 4;
			if ( !wave_size )
				continue;

			int last_amp = osc.last_amp;
			int wave_pos = osc.wave_pos;

			do
			{
				// read wave sample
				int addr = wave_pos + osc_reg [6];
				int sample = reg [addr >> 1] >> (addr << 2 & 4);
				wave_pos++;
				sample = (sample & 15) * volume;

				// output impulse if amplitude changed
				int delta = sample - last_amp;
				if ( delta )
				{
					last_amp = sample;
					synth.offset_resampled( time, delta, output );
				}

				// next sample
				time += period;
				if ( wave_pos >= wave_size )
					wave_pos = 0;
			}
			while ( time < end_time );

			osc.wave_pos = wave_pos;
			osc.last_amp = last_amp;
		}
		osc.delay = time - end_time;
	}

	last_time = nes_end_time;
}

// nt-chiptune-player fork addition: see the ponytail note in Nes_Namco_Apu.h
// (last_amp remains the enabled/volume signal -- exact per-channel volume
// register decode is still out of scope). Issue #569 adds period: the same
// chip-independent "normalized period N" convention as Nes_Vrc7_Apu (hz =
// nes_cpu_clock/(N+1)). Frequency/wave-size decode mirrors run_until() above
// exactly (same osc_reg layout, active_oscs count, wave_size formula) --
// nes_cpu_clock cancels out of the ratio the same way it does for FDS, so no
// clock constant is needed here either.
void Nes_Namco_Apu::get_osc_state( int index, gme_nsf_channel_state_t* out ) const
{
	require( (unsigned) index < osc_count );
	memset( out, 0, sizeof *out );
	out->chip_id = 4; // N163 (Namco 106)
	Namco_Osc const& osc = oscs[index];
	out->enabled = (unsigned char) (osc.last_amp != 0);
	out->channel_vol = out->enabled ? 15 : 0;
	out->period = 0;
	if ( out->enabled )
	{
		int const active_oscs = (reg [0x7F] >> 4 & 7) + 1;
		const uint8_t* osc_reg = &reg [index * 8 + 0x40];
		int32_t const freq = (osc_reg [4] & 3) * 0x10000 + osc_reg [2] * 0x100L + osc_reg [0];
		int const wave_size = 32 - (osc_reg [4] >> 2 & 7) * 4;
		// wave_size = 32 - (bits)*4 has a structural minimum of 4 (bits max 7),
		// so `wave_size != 0` can never actually fail -- kept only to mirror the
		// same defensive shape run_until() above already uses for the identical
		// expression (PR #570 review round 1 SEC-L-1: confirmed unreachable, not
		// a real guard).
		if ( freq != 0 && wave_size != 0 )
		{
			double const ratio = 983040.0 * active_oscs * wave_size / (double) freq;
			long norm = (long) (ratio + 0.5) - 1;
			if ( norm < 0 )     norm = 0;
			if ( norm > 65535 ) norm = 65535;
			out->period = (unsigned short) norm;
		}
	}
	out->gain_l = out->gain_r = osc.last_amp;
}

