// Atari XL/XE SAP music file emulator

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
// Modified 2026-09-28 by nt-chiptune-player project -- see NTCP-MODIFICATIONS.md
#ifndef SAP_EMU_H
#define SAP_EMU_H

#include "Classic_Emu.h"
#include "Sap_Apu.h"
#include "Sap_Cpu.h"

class Sap_Emu : private Sap_Cpu, public Classic_Emu {
	typedef Sap_Cpu cpu;
public:
	static gme_type_t static_type() { return gme_sap_type; }
public:
	Sap_Emu();
	~Sap_Emu();
	struct info_t {
		byte const* rom_data;
		const char* warning;
		long init_addr;
		long play_addr;
		long music_addr;
		int  type;
		int  track_count;
		int  fastplay;
		bool stereo;
		char author    [256];
		char name      [256];
		char copyright [ 32];
	};

	// nt-chiptune-player fork addition (Issue #1017): read-only per-channel
	// state snapshot, dispatched to whichever POKEY chip owns voice `i` (see
	// gme_sap_channel_state in gme.h). Index ordering matches set_voice()/
	// gme_voice_count(): 0-3 = first chip (`apu`), 4-7 (stereo only) =
	// second chip (`apu2`).
	void channel_state( int i, gme_sap_channel_state_t* out ) const {
		if ( i < Sap_Apu::osc_count )
			apu.get_osc_state( i, out );
		else
			apu2.get_osc_state( i - Sap_Apu::osc_count, out );
	}

	// nt-chiptune-player fork addition (Issue #1017): opt-in observation
	// granularity, delegated to the Classic_Emu base (see
	// gme_sap_set_observe_interval_ms in gme.h). Sap_Emu is a Classic_Emu
	// subclass, so this re-exports the existing protected member -- no new
	// code is added to Classic_Emu itself (ADR 0060 裁定 2 制約 3).
	blargg_err_t set_observe_interval_ms( int msec ) { return set_buffer_length_ms( msec ); }

protected:
	blargg_err_t track_info_( track_info_t*, int track ) const;
	blargg_err_t load_mem_( byte const*, long );
	blargg_err_t start_track_( int );
	blargg_err_t run_clocks( blip_time_t&, int );
	void set_tempo_( double );
	void set_voice( int, Blip_Buffer*, Blip_Buffer*, Blip_Buffer* );
	void update_eq( blip_eq_t const& );
public: private: friend class Sap_Cpu;
	int cpu_read( sap_addr_t );
	void cpu_write( sap_addr_t, int );
	void cpu_write_( sap_addr_t, int );
private:
	info_t info;

	byte const* file_end;
	sap_time_t scanline_period;
	sap_time_t next_play;
	sap_time_t time_mask;
	Sap_Apu apu;
	Sap_Apu apu2;

	// large items
	struct {
		byte padding1 [0x100];
		byte ram [0x10000 + 0x100];
	} mem;
	Sap_Apu_Impl apu_impl;

	sap_time_t play_period() const;
	void call_play();
	void cpu_jsr( sap_addr_t );
	void call_init( int track );
	void run_routine( sap_addr_t );
};

#endif
