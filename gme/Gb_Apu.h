// Nintendo Game Boy PAPU sound chip emulator

// Gb_Snd_Emu 0.1.5
// Modified 2026-09-18 by nt-chiptune-player project -- see NTCP-MODIFICATIONS.md
#ifndef GB_APU_H
#define GB_APU_H

#include "Gb_Oscs.h"
#include "gme.h" // nt-chiptune-player fork addition: gme_gbs_channel_state_t

class Gb_Apu {
public:

	// Set overall volume of all oscillators, where 1.0 is full volume
	void volume( double );

	// Set treble equalization
	void treble_eq( const blip_eq_t& );

	// Outputs can be assigned to a single buffer for mono output, or to three
	// buffers for stereo output (using Stereo_Buffer to do the mixing).

	// Assign all oscillator outputs to specified buffer(s). If buffer
	// is NULL, silences all oscillators.
	void output( Blip_Buffer* mono );
	void output( Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right );

	// Assign single oscillator output to buffer(s). Valid indicies are 0 to 3,
	// which refer to Square 1, Square 2, Wave, and Noise. If buffer is NULL,
	// silences oscillator.
	static const int osc_count = 4;
	void osc_output( int index, Blip_Buffer* mono );
	void osc_output( int index, Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right );

	// nt-chiptune-player fork addition (Issue #599): read-only snapshot of
	// oscillator `index`'s current raw state, for visualization (see
	// gme_gbs_channel_state in gme.h). `keyon` is recomputed directly from
	// registers (same gate Gb_Apu::run_until uses, plus the Square 1 sweep
	// silence and the Square/Wave out-of-range DC conditions Gb_Square::run /
	// Gb_Wave::run apply), so it stays correct even while gme_mute_voice()
	// has set this oscillator's output to NULL and run_until() has stopped
	// evaluating it.
	void get_osc_state( int index, gme_gbs_channel_state_t* out ) const;

	// Reset oscillators and internal state
	void reset();

	// Reads and writes at addr must satisfy start_addr <= addr <= end_addr
	static const unsigned int start_addr = 0xFF10;
	static const unsigned int end_addr   = 0xFF3F;
	static const unsigned int register_count = end_addr - start_addr + 1;

	// Write 'data' to address at specified time
	void write_register( blip_time_t, unsigned addr, int data );

	// Read from address at specified time
	int read_register( blip_time_t, unsigned addr );

	// Run all oscillators up to specified time, end current time frame, then
	// start a new frame at time 0.
	void end_frame( blip_time_t );

	void set_tempo( double );

public:
	Gb_Apu();
private:
	// noncopyable
	Gb_Apu( const Gb_Apu& );
	Gb_Apu& operator = ( const Gb_Apu& );

	Gb_Osc*     oscs [osc_count];
	blip_time_t   next_frame_time;
	blip_time_t   last_time;
	blip_time_t frame_period;
	double      volume_unit;
	int         frame_count;

	Gb_Square   square1;
	Gb_Square   square2;
	Gb_Wave     wave;
	Gb_Noise    noise;
	uint8_t regs [register_count];
	Gb_Square::Synth square_synth; // used by squares
	Gb_Wave::Synth   other_synth;  // used by wave and noise

	void update_volume();
	void run_until( blip_time_t );
	void write_osc( int index, int reg, int data );
};

inline void Gb_Apu::output( Blip_Buffer* b ) { output( b, b, b ); }

inline void Gb_Apu::osc_output( int i, Blip_Buffer* b ) { osc_output( i, b, b, b ); }

inline void Gb_Apu::volume( double vol )
{
	volume_unit = 0.60 / osc_count / 15 /*steps*/ / 2 /*?*/ / 8 /*master vol range*/ * vol;
	update_volume();
}

#endif
