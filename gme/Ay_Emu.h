// Sinclair Spectrum AY music file emulator

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
#ifndef AY_EMU_H
#define AY_EMU_H

#include "Classic_Emu.h"
#include "Ay_Apu.h"
#include "Ay_Cpu.h"

class Ay_Emu : private Ay_Cpu, public Classic_Emu {
	typedef Ay_Cpu cpu;
public:
	// AY file header
	enum { header_size = 0x14 };
	struct header_t
	{
		byte tag [8];
		byte vers;
		byte player;
		byte unused [2];
		byte author [2];
		byte comment [2];
		byte max_track;
		byte first_track;
		byte track_info [2];
	};

	static gme_type_t static_type() { return gme_ay_type; }

	// nt-chiptune-player fork addition (Issue #1009 / ADR 0106 裁定 7): read-only
	// per-channel state snapshot (see gme_ay_channel_state in gme.h). Index
	// ordering matches set_voice()/gme_voice_count() exactly (Wave 1, Wave 2,
	// Wave 3, Beeper). Indices 0-2 dispatch to the (sole) Ay_Apu; index 3
	// (Beeper) is handled here directly since the Beeper is not one of
	// Ay_Apu's oscillators (ADR 0106 Context). `clock_rate` is filled from
	// Classic_Emu::clock_rate() for every index.
	void channel_state( int i, gme_ay_channel_state_t* out ) const;

	// nt-chiptune-player fork addition (Issue #1009 / ADR 0106 裁定 7): opt-in
	// observation granularity, delegated to the Classic_Emu base (see
	// gme_ay_set_observe_interval_ms in gme.h). Ay_Emu is a Classic_Emu
	// subclass, so this re-exports the existing protected member -- no new
	// code is added to Classic_Emu itself (ADR 0060 裁定 2 制約 3).
	blargg_err_t set_observe_interval_ms( int msec ) { return set_buffer_length_ms( msec ); }

public:
	Ay_Emu();
	~Ay_Emu();
	struct file_t {
		header_t const* header;
		byte const* end;
		byte const* tracks;
	};
protected:
	blargg_err_t track_info_( track_info_t*, int track ) const;
	blargg_err_t load_mem_( byte const*, long );
	blargg_err_t start_track_( int );
	blargg_err_t run_clocks( blip_time_t&, int );
	void set_tempo_( double );
	void set_voice( int, Blip_Buffer*, Blip_Buffer*, Blip_Buffer* );
	void update_eq( blip_eq_t const& );
private:
	file_t file;

	cpu_time_t play_period;
	cpu_time_t next_play;
	Blip_Buffer* beeper_output;
	int beeper_delta;
	int last_beeper;
	// nt-chiptune-player fork addition (Issue #1009 / ADR 0106 裁定 7):
	// monotonic count of ay_cpu_out() observing `last_beeper` toggle to a
	// new value since the current track started (reset in
	// start_track_()). See gme_ay_channel_state_t's beeper_toggle_count
	// doc comment in gme.h for the full rationale.
	unsigned beeper_toggle_count;
	int apu_addr;
	int cpc_latch;
	bool spectrum_mode;
	bool cpc_mode;

	// large items
	struct {
		byte padding1 [0x100];
		byte ram [0x10000 + 0x100];
	} mem;
	Ay_Apu apu;
	friend void ay_cpu_out( Ay_Cpu*, cpu_time_t, unsigned addr, int data );
	void cpu_out_misc( cpu_time_t, unsigned addr, int data );
};

#endif
