// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
// Modified 2026-09-12 by nt-chiptune-player project -- see NTCP-MODIFICATIONS.md

#include "Nes_Fds_Apu.h"

/* Copyright (C) 2006 Shay Green. This module is free software; you
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

#include <string.h>
#include <algorithm>

static int const fract_range = 65536;

void Nes_Fds_Apu::reset()
{
	memset( regs_, 0, sizeof regs_ );
	memset( mod_wave, 0, sizeof mod_wave );

	last_time     = 0;
	env_delay     = 0;
	sweep_delay   = 0;
	wave_pos      = 0;
	last_amp      = 0;
	wave_fract    = fract_range;
	mod_fract     = fract_range;
	mod_pos       = 0;
	mod_write_pos = 0;

	static byte const initial_regs [0x0B] = {
		0x80,       // disable envelope
		0, 0, 0xC0, // disable wave and lfo
		0x80,       // disable sweep
		0, 0, 0x80, // disable modulation
		0, 0, 0xFF  // LFO period // TODO: use 0xE8 as FDS ROM does?
	};
	for ( int i = 0; i < (int) sizeof initial_regs; i++ )
	{
		// two writes to set both gain and period for envelope registers
		write_( io_addr + wave_size + i, 0 );
		write_( io_addr + wave_size + i, initial_regs [i] );
	}
}

void Nes_Fds_Apu::write_( unsigned addr, int data )
{
	unsigned reg = addr - io_addr;
	if ( reg < io_size )
	{
		if ( reg < wave_size )
		{
			if ( regs (0x4089) & 0x80 )
				regs_ [reg] = data & wave_sample_max;
		}
		else
		{
			regs_ [reg] = data;
			switch ( addr )
			{
			case 0x4080:
				if ( data & 0x80 )
					env_gain = data & 0x3F;
				else
					env_speed = (data & 0x3F) + 1;
				break;

			case 0x4084:
				if ( data & 0x80 )
					sweep_gain = data & 0x3F;
				else
					sweep_speed = (data & 0x3F) + 1;
				break;

			case 0x4085:
				mod_pos = mod_write_pos;
				regs (0x4085) = data & 0x7F;
				break;

			case 0x4088:
				if ( regs (0x4087) & 0x80 )
				{
					int pos = mod_write_pos;
					data &= 0x07;
					mod_wave [pos    ] = data;
					mod_wave [pos + 1] = data;
					mod_write_pos = (pos     + 2) & (wave_size - 1);
					mod_pos       = (mod_pos + 2) & (wave_size - 1);
				}
				break;
			}
		}
	}
}

void Nes_Fds_Apu::set_tempo( double t )
{
	lfo_tempo = lfo_base_tempo;
	if ( t != 1.0 )
	{
		lfo_tempo = int ((double) lfo_base_tempo / t + 0.5);
		if ( lfo_tempo <= 0 )
			lfo_tempo = 1;
	}
}

void Nes_Fds_Apu::run_until( blip_time_t final_end_time )
{
	int const wave_freq = (regs (0x4083) & 0x0F) * 0x100 + regs (0x4082);
	Blip_Buffer* const output_ = this->output_;
	if ( wave_freq && output_ && !((regs (0x4089) | regs (0x4083)) & 0x80) )
	{
		output_->set_modified();

		// master_volume
		#define MVOL_ENTRY( percent ) (master_vol_max * percent + 50) / 100
		static unsigned char const master_volumes [4] = {
			MVOL_ENTRY( 100 ), MVOL_ENTRY( 67 ), MVOL_ENTRY( 50 ), MVOL_ENTRY( 40 )
		};
		int const master_volume = master_volumes [regs (0x4089) & 0x03];

		// lfo_period
		blip_time_t lfo_period = regs (0x408A) * lfo_tempo;
		if ( regs (0x4083) & 0x40 )
			lfo_period = 0;

		// sweep setup
		blip_time_t sweep_time = last_time + sweep_delay;
		blip_time_t const sweep_period = lfo_period * sweep_speed;
		if ( !sweep_period || regs (0x4084) & 0x80 )
			sweep_time = final_end_time;

		// envelope setup
		blip_time_t env_time = last_time + env_delay;
		blip_time_t const env_period = lfo_period * env_speed;
		if ( !env_period || regs (0x4080) & 0x80 )
			env_time = final_end_time;

		// modulation
		int mod_freq = 0;
		if ( !(regs (0x4087) & 0x80) )
			mod_freq = (regs (0x4087) & 0x0F) * 0x100 + regs (0x4086);

		blip_time_t end_time = last_time;
		do
		{
			// sweep
			if ( sweep_time <= end_time )
			{
				sweep_time += sweep_period;
				int mode = regs (0x4084) >> 5 & 2;
				int new_sweep_gain = sweep_gain + mode - 1;
				if ( (unsigned) new_sweep_gain <= (unsigned) 0x80 >> mode )
					sweep_gain = new_sweep_gain;
				else
					regs (0x4084) |= 0x80; // optimization only
			}

			// envelope
			if ( env_time <= end_time )
			{
				env_time += env_period;
				int mode = regs (0x4080) >> 5 & 2;
				int new_env_gain = env_gain + mode - 1;
				if ( (unsigned) new_env_gain <= (unsigned) 0x80 >> mode )
					env_gain = new_env_gain;
				else
					regs (0x4080) |= 0x80; // optimization only
			}

			// new end_time
			blip_time_t const start_time = end_time;
			end_time = final_end_time;
			if ( end_time > env_time   ) end_time = env_time;
			if ( end_time > sweep_time ) end_time = sweep_time;

			// frequency modulation
			int freq = wave_freq;
			if ( mod_freq )
			{
				// time of next modulation clock
				blip_time_t mod_time = start_time + (mod_fract + mod_freq - 1) / mod_freq;
				if ( end_time > mod_time )
					end_time = mod_time;

				// run modulator up to next clock and save old sweep_bias
				int sweep_bias = regs (0x4085);
				mod_fract -= (end_time - start_time) * mod_freq;
				if ( mod_fract <= 0 )
				{
					mod_fract += fract_range;
					check( (unsigned) mod_fract <= fract_range );

					static short const mod_table [8] = { 0, +1, +2, +4, 0, -4, -2, -1 };
					int mod = mod_wave [mod_pos];
					mod_pos = (mod_pos + 1) & (wave_size - 1);
					int new_sweep_bias = (sweep_bias + mod_table [mod]) & 0x7F;
					if ( mod == 4 )
						new_sweep_bias = 0;
					regs (0x4085) = new_sweep_bias;
				}

				// apply frequency modulation
				sweep_bias = (sweep_bias ^ 0x40) - 0x40;
				int factor = sweep_bias * sweep_gain;
				int extra = factor & 0x0F;
				factor >>= 4;
				if ( extra )
				{
					factor--;
					if ( sweep_bias >= 0 )
						factor += 3;
				}
				if ( factor > 193 ) factor -= 258;
				if ( factor < -64 ) factor += 256;
				freq += (freq * factor) >> 6;
				if ( freq <= 0 )
					continue;
			}

			// wave
			int wave_fract = this->wave_fract;
			blip_time_t delay = (wave_fract + freq - 1) / freq;
			blip_time_t time = start_time + delay;

			if ( time <= end_time )
			{
				// at least one wave clock within start_time...end_time

				blip_time_t const min_delay = fract_range / freq;
				int wave_pos = this->wave_pos;

				int volume = env_gain;
				if ( volume > vol_max )
					volume = vol_max;
				volume *= master_volume;

				int const min_fract = min_delay * freq;

				do
				{
					// clock wave
					int amp = regs_ [wave_pos] * volume;
					wave_pos = (wave_pos + 1) & (wave_size - 1);
					int delta = amp - last_amp;
					if ( delta )
					{
						last_amp = amp;
						synth.offset_inline( time, delta, output_ );
					}

					wave_fract += fract_range - delay * freq;
					check( unsigned (fract_range - wave_fract) < freq );

					// delay until next clock
					delay = min_delay;
					if ( wave_fract > min_fract )
						delay++;
					check( delay && delay == (wave_fract + freq - 1) / freq );

					time += delay;
				}
				while ( time <= end_time ); // TODO: using < breaks things, but <= is wrong

				this->wave_pos = wave_pos;
			}
			this->wave_fract = wave_fract - (end_time - (time - delay)) * freq;
			check( this->wave_fract > 0 );
		}
		while ( end_time < final_end_time );

		env_delay   = env_time   - final_end_time; check( env_delay >= 0 );
		sweep_delay = sweep_time - final_end_time; check( sweep_delay >= 0 );
	}
	last_time = final_end_time;
}

// nt-chiptune-player fork addition: read-only snapshot of the FDS oscillator's
// raw state, for visualization (see gme_nsf_channel_state in gme.h). env_gain
// is the wave (master) envelope generator's current gain (0-0x20); wave_pos/
// regs_ hold the 64-sample custom waveform but no single "pitch register" the
// way pulse/square channels have, so period is left at 0 (keycode invalid) --
// ponytail: FDS pitch is a 12-bit frequency word split across two registers
// plus an LFO sweep; decode it if FDS pitch display accuracy is needed
// (Issue #558 follow-up).
// Issue #569 (PR #570 review round 1 H-1 fix): period is the same
// chip-independent "normalized period N" convention added to
// Nes_Vrc7_Apu::get_osc_state (hz = nes_cpu_clock/(N+1)). FDS wavetable
// frequency is NOT simply wave_freq*clock/2^16 -- run_until() above clocks
// one wave *sample* every `fract_range/freq` (fract_range=65536) CPU cycles
// and a full cycle is `wave_size` (0x40=64) samples, so
// hz = clock*wave_freq / (65536*wave_size) and therefore
// nes_cpu_clock/hz = 65536*wave_size/wave_freq (clock cancels out, matching
// the N163 getter's wave_size factor below -- the first version of this
// getter omitted the *wave_size term, which put every FDS keycode 6 octaves
// (72 semitones) too high; caught in PR #570 review round 1 H-1). This
// getter approximates with the *raw* $4082/$4083 register value, ignoring
// run_until()'s sweep/frequency-modulation adjustment to `freq` -- an
// approximation adequate for keycode display (PR #570 review round 1 L-2).
void Nes_Fds_Apu::get_osc_state( int index, gme_nsf_channel_state_t* out ) const
{
	require( (unsigned) index < osc_count );
	memset( out, 0, sizeof *out );
	out->chip_id = 3; // FDS
	out->enabled = (unsigned char) (env_gain != 0 && last_amp != 0);
	out->channel_vol = (unsigned char) (std::min)( env_gain, 15 );
	int const wave_freq = (regs_ [0x4083 - io_addr] & 0x0F) * 0x100 + regs_ [0x4082 - io_addr];
	out->period = 0;
	if ( out->enabled && wave_freq != 0 )
	{
		long norm = (long) (65536.0 * wave_size / (double) wave_freq + 0.5) - 1;
		if ( norm < 0 )     norm = 0;
		// PR #570 review round 1 H-1 follow-up (round 2 L-2): the *wave_size
		// factor above means this clamp is now reachable for very low wave_freq
		// (wave_freq < wave_size=64 saturates norm to 65535, i.e. a keycode near
		// MIDI ~21/A0). That range corresponds to well under 30 Hz (clock/65536),
		// far below any musically intended FDS pitch, so no further handling is
		// added here -- confirmed by inspection of the formula, not measured
		// against a real ROM that reaches it.
		if ( norm > 65535 ) norm = 65535;
		out->period = (unsigned short) norm;
	}
	out->gain_l = out->gain_r = (short) last_amp;
}

