// Nes_Snd_Emu 0.1.8. http://www.slack.net/~ant/
// Modified 2026-09-12 by nt-chiptune-player project -- see NTCP-MODIFICATIONS.md

#include "Nes_Vrc6_Apu.h"

#include <algorithm>
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

Nes_Vrc6_Apu::Nes_Vrc6_Apu()
{
	output( NULL );
	volume( 1.0 );
	reset();
}

void Nes_Vrc6_Apu::reset()
{
	last_time = 0;
	for ( int i = 0; i < osc_count; i++ )
	{
		Vrc6_Osc& osc = oscs [i];
		for ( int j = 0; j < reg_count; j++ )
			osc.regs [j] = 0;
		osc.delay = 0;
		osc.last_amp = 0;
		osc.phase = 1;
		osc.amp = 0;
	}
}

void Nes_Vrc6_Apu::output( Blip_Buffer* buf )
{
	for ( int i = 0; i < osc_count; i++ )
		osc_output( i, buf );
}

void Nes_Vrc6_Apu::run_until( blip_time_t time )
{
	require( time >= last_time );
	run_square( oscs [0], time );
	run_square( oscs [1], time );
	run_saw( time );
	last_time = time;
}

void Nes_Vrc6_Apu::write_osc( blip_time_t time, int osc_index, int reg, int data )
{
	require( (unsigned) osc_index < osc_count );
	require( (unsigned) reg < reg_count );

	run_until( time );
	oscs [osc_index].regs [reg] = data;
}

// nt-chiptune-player fork addition: read-only snapshot of oscillator `index`'s
// raw state, for visualization (see gme_nsf_channel_state in gme.h). Register
// layout matches write_osc()/run_square()/run_saw() above: for the two square
// oscillators (0,1) regs[2] bit 0x80 enables the oscillator (or the "gate" bit
// in regs[0] bit 0x80 forces it on at max volume) and regs[0] bits 0-3 are the
// volume; for the saw oscillator (2) regs[2] bit 0x80 enables it and the
// running accumulator amp approximates loudness (the saw has no discrete
// volume register). period() is shared by all three oscillators.
void Nes_Vrc6_Apu::get_osc_state( int index, gme_nsf_channel_state_t* out ) const
{
	require( (unsigned) index < osc_count );
	memset( out, 0, sizeof *out );
	out->chip_id = 1; // VRC6
	Vrc6_Osc const& osc = oscs[index];
	bool const gate_or_enabled = (osc.regs[2] & 0x80) != 0;
	if ( index < 2 )
	{
		bool const gate = (osc.regs[0] & 0x80) != 0;
		out->enabled = (unsigned char) (gate_or_enabled && (gate || (osc.regs[0] & 0x0F) != 0));
		out->channel_vol = (unsigned char) (osc.regs[0] & 0x0F);
	}
	else
	{
		out->enabled = (unsigned char) (gate_or_enabled && osc.amp != 0);
		out->channel_vol = (unsigned char)( std::min( osc.amp, 63 ) >> 2 ); // 6-bit accumulator -> 0-15
	}
	out->period = (unsigned short) osc.period();
	out->gain_l = out->gain_r = (short) osc.last_amp;
}

void Nes_Vrc6_Apu::end_frame( blip_time_t time )
{
	if ( time > last_time )
		run_until( time );

	assert( last_time >= time );
	last_time -= time;
}

void Nes_Vrc6_Apu::save_state( vrc6_apu_state_t* out ) const
{
	blaarg_static_assert( sizeof (vrc6_apu_state_t) == 20, "VRC APU State layout incorrect!" );
	out->saw_amp = oscs [2].amp;
	for ( int i = 0; i < osc_count; i++ )
	{
		Vrc6_Osc const& osc = oscs [i];
		for ( int r = 0; r < reg_count; r++ )
			out->regs [i] [r] = osc.regs [r];

		out->delays [i] = osc.delay;
		out->phases [i] = osc.phase;
	}
}

void Nes_Vrc6_Apu::load_state( vrc6_apu_state_t const& in )
{
	reset();
	oscs [2].amp = in.saw_amp;
	for ( int i = 0; i < osc_count; i++ )
	{
		Vrc6_Osc& osc = oscs [i];
		for ( int r = 0; r < reg_count; r++ )
			osc.regs [r] = in.regs [i] [r];

		osc.delay = in.delays [i];
		osc.phase = in.phases [i];
	}
	if ( !oscs [2].phase )
		oscs [2].phase = 1;
}

void Nes_Vrc6_Apu::run_square( Vrc6_Osc& osc, blip_time_t end_time )
{
	Blip_Buffer* output = osc.output;
	if ( !output )
		return;
	output->set_modified();

	int volume = osc.regs [0] & 15;
	if ( !(osc.regs [2] & 0x80) )
		volume = 0;

	int gate = osc.regs [0] & 0x80;
	int duty = ((osc.regs [0] >> 4) & 7) + 1;
	int delta = ((gate || osc.phase < duty) ? volume : 0) - osc.last_amp;
	blip_time_t time = last_time;
	if ( delta )
	{
		osc.last_amp += delta;
		square_synth.offset( time, delta, output );
	}

	time += osc.delay;
	osc.delay = 0;
	int period = osc.period();
	if ( volume && !gate && period > 4 )
	{
		if ( time < end_time )
		{
			int phase = osc.phase;

			do
			{
				phase++;
				if ( phase == 16 )
				{
					phase = 0;
					osc.last_amp = volume;
					square_synth.offset( time, volume, output );
				}
				if ( phase == duty )
				{
					osc.last_amp = 0;
					square_synth.offset( time, -volume, output );
				}
				time += period;
			}
			while ( time < end_time );

			osc.phase = phase;
		}
		osc.delay = time - end_time;
	}
}

void Nes_Vrc6_Apu::run_saw( blip_time_t end_time )
{
	Vrc6_Osc& osc = oscs [2];
	Blip_Buffer* output = osc.output;
	if ( !output )
		return;
	output->set_modified();

	int amp = osc.amp;
	int amp_step = osc.regs [0] & 0x3F;
	blip_time_t time = last_time;
	int last_amp = osc.last_amp;
	if ( !(osc.regs [2] & 0x80) || !(amp_step | amp) )
	{
		osc.delay = 0;
		int delta = (amp >> 3) - last_amp;
		last_amp = amp >> 3;
		saw_synth.offset( time, delta, output );
	}
	else
	{
		time += osc.delay;
		if ( time < end_time )
		{
			int period = osc.period() * 2;
			int phase = osc.phase;

			do
			{
				if ( --phase == 0 )
				{
					phase = 7;
					amp = 0;
				}

				int delta = (amp >> 3) - last_amp;
				if ( delta )
				{
					last_amp = amp >> 3;
					saw_synth.offset( time, delta, output );
				}

				time += period;
				amp = (amp + amp_step) & 0xFF;
			}
			while ( time < end_time );

			osc.phase = phase;
			osc.amp = amp;
		}

		osc.delay = time - end_time;
	}

	osc.last_amp = last_amp;
}

