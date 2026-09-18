// Nintendo Game Boy GBS music file emulator

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
// Modified 2026-09-18 by nt-chiptune-player project -- see NTCP-MODIFICATIONS.md
#ifndef GBS_EMU_H
#define GBS_EMU_H

#include "Classic_Emu.h"
#include "Gb_Apu.h"
#include "Gb_Cpu.h"

class Gbs_Emu : private Gb_Cpu, public Classic_Emu {
	typedef Gb_Cpu cpu;
public:
	// Equalizer profiles for Game Boy Color speaker and headphones
	static equalizer_t const handheld_eq;
	static equalizer_t const headphones_eq;

	// GBS file header
	enum { header_size = 112 };
	struct header_t
	{
		char tag [3];
		byte vers;
		byte track_count;
		byte first_track;
		byte load_addr [2];
		byte init_addr [2];
		byte play_addr [2];
		byte stack_ptr [2];
		byte timer_modulo;
		byte timer_mode;
		char game [32];
		char author [32];
		char copyright [32];
	};

	// Header for currently loaded file
	header_t const& header() const { return header_; }

	static gme_type_t static_type() { return gme_gbs_type; }

	// nt-chiptune-player fork addition (Issue #599): read-only per-channel
	// state snapshot, dispatched straight to the (sole) Gb_Apu (see
	// gme_gbs_channel_state in gme.h). Index ordering matches set_voice()/
	// gme_voice_count() exactly (Square 1, Square 2, Wave, Noise).
	void channel_state( int i, gme_gbs_channel_state_t* out ) const { apu.get_osc_state( i, out ); }

	// nt-chiptune-player fork addition (Issue #599): opt-in observation
	// granularity, delegated to the Classic_Emu base (see
	// gme_gbs_set_observe_interval_ms in gme.h). Gbs_Emu is a Classic_Emu
	// subclass, so this re-exports the existing protected member -- no new
	// code is added to Classic_Emu itself (ADR 0060 裁定 2 制約 3).
	blargg_err_t set_observe_interval_ms( int msec ) { return set_buffer_length_ms( msec ); }

public:
	// deprecated
	using Music_Emu::load;
	blargg_err_t load( header_t const& h, Data_Reader& in ) // use Remaining_Reader
			{ return load_remaining_( &h, sizeof h, in ); }

public:
	Gbs_Emu();
	~Gbs_Emu();
protected:
	blargg_err_t track_info_( track_info_t*, int track ) const;
	blargg_err_t load_( Data_Reader& );
	blargg_err_t start_track_( int );
	blargg_err_t run_clocks( blip_time_t&, int );
	void set_tempo_( double );
	void set_voice( int, Blip_Buffer*, Blip_Buffer*, Blip_Buffer* );
	void update_eq( blip_eq_t const& );
	void unload();
private:
	// rom
	enum { bank_size = 0x4000 };
	Rom_Data<bank_size> rom;
	void set_bank( int );

	// timer
	blip_time_t cpu_time;
	blip_time_t play_period;
	blip_time_t next_play;
	void update_timer();

	header_t header_;
	void cpu_jsr( gb_addr_t );

public: private: friend class Gb_Cpu;
	blip_time_t clock() const { return cpu_time - cpu::remain(); }

	enum { joypad_addr = 0xFF00 };
	enum { ram_addr = 0xA000 };
	enum { hi_page = 0xFF00 - ram_addr };
	byte ram [0x4000 + 0x2000 + Gb_Cpu::cpu_padding];
	Gb_Apu apu;

	int cpu_read( gb_addr_t );
	void cpu_write( gb_addr_t, int );
};

#endif
