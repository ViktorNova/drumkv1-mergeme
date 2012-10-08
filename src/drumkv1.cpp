// drumkv1.cpp
//
/****************************************************************************
   Copyright (C) 2012, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*****************************************************************************/

#include "drumkv1.h"

#include "drumkv1_sample.h"

#include "drumkv1_wave.h"
#include "drumkv1_ramp.h"

#include "drumkv1_fx.h"


#ifdef CONFIG_DEBUG_0
#include <stdio.h>
#endif

#include <string.h>


//-------------------------------------------------------------------------
// drumkv1_impl
//
// -- borrowed and revamped from synth.h of synth4
//    Copyright (C) 2007 jorgen, linux-vst.com
//

const uint16_t MAX_VOICES = 24;			// polyphony
const uint8_t  MAX_NOTES  = 128;

const float MIN_ENV_MSECS = 0.5f;		// min 0.5ms per stage
const float MAX_ENV_MSECS = 5000.0f;	// max 5 sec per stage

const float DETUNE_SCALE  = 0.5f;
const float PHASE_SCALE   = 0.5f;
const float COARSE_SCALE  = 12.0f;
const float FINE_SCALE    = 1.0f;
const float SWEEP_SCALE   = 0.5f;

const float LFO_FREQ_MIN  = 0.4f;
const float LFO_FREQ_MAX  = 40.0f;


// sigmoids

inline float drumkv1_sigmoid ( const float x )
{
//	return 2.0f / (1.0f + ::expf(-5.0f * x)) - 1.0f;
	return drumkv1_fx_tanhf(2.0f * x);
}

inline float drumkv1_sigmoid0 ( const float x, const float t )
{
	const float t0 = t - 1.0f;
	const float t1 = 1.0f - t;

	return (x < t0 ? t0 : (x > t1 ? t1 : x * (1.5f - 0.5f * x * x)));
}

inline float drumkv1_sigmoid1 ( const float x, const float t = 0.01f )
{
	return 0.5f * (1.0f + drumkv1_sigmoid0(2.0f * x - 1.0f, t));
}


// envelope

struct drumkv1_env
{
	// envelope stages

	enum Stage { Attack = 0, Decay1, Decay2, Done };

	// per voice

	struct State
	{
		// process
		float value(uint32_t n) const
			{ return level + float(n) * delta; }

		float value2(uint32_t n) const
			{ const float y = value(n); return y * y; }

		// state
		bool running;
		Stage stage;
		float level;
		float delta;
		uint32_t frames;
	};

	void start(State *p)
	{
		p->running = true;
		p->stage = Attack;
		p->level = 0.0f;
		p->frames = int(*attack * *attack * max_frames);
	#if 0
		if (p->frames < min_frames) // prevent click on too fast attack
			p->frames = min_frames;
	#endif
		if (p->frames > 0)
			p->delta = 1.0f / float(p->frames);
		else
			p->delta = 0.0f;
	}

	void next(State *p)
	{
		if (p->stage == Attack) {
			p->stage = Decay1;
			p->level = 1.0f;
			p->frames = int(*decay1 * *decay1 * max_frames);
			if (p->frames < min_frames) // prevent click on too fast decay
				p->frames = min_frames;
			p->delta = -0.5f / float(p->frames);
		}
		else if (p->stage == Decay1) {
			p->stage = Decay2;
			p->level = 0.5f;
			p->frames = int(*decay2 * *decay2 * max_frames);
			if (p->frames < min_frames) // prevent click on too fast decay
				p->frames = min_frames;
			p->delta = -0.5f / float(p->frames);
		}
		else if (p->stage == Decay2) {
			p->running = false;
			p->stage = Done;
			p->level = 0.0f;
			p->frames = 0;
			p->delta = 0.0f;
		}
	}

	void note_on(State *p)
	{
		p->running = true;
		p->stage = Attack;
		p->frames = int(*attack * *attack * max_frames);
		if (p->frames < min_frames) // prevent click on too fast attack
			p->frames = min_frames;
		p->delta = (1.0f - p->level) / float(p->frames);
	}

	void note_off(State *p)
	{
		p->running = true;
		p->stage = Decay2;
		p->frames = int(*decay2 * *decay2 * max_frames);
		if (p->frames < min_frames) // prevent click on too fast release
			p->frames = min_frames;
		p->delta = -(p->level) / float(p->frames);
	}

	void note_off_fast(State *p)
	{
		p->running = true;
		p->stage = Decay2;
		if (p->frames > min_frames) {
			p->frames = min_frames;
			p->delta = -(p->level) / float(p->frames);
		}
	}

	// parameters

	float *attack;
	float *decay1;
	float *decay2;

	uint32_t min_frames;
	uint32_t max_frames;
};


// midi control

struct drumkv1_ctl
{
	drumkv1_ctl() { reset(); }

	void reset()
	{
		pressure = 0.0f;
		pitchbend = 0.0f;
		modwheel = 0.0f;
		panning = 0.0f;
		volume = 1.0f;
	}

	float pressure;
	float pitchbend;
	float modwheel;
	float panning;
	float volume;
};


// internal control

struct drumkv1_aux
{
	drumkv1_aux() { reset(); }

	void reset()
	{
		panning = 0.0f;
		volume = 1.0f;
	}

	float panning;
	float volume;
};


// dco

struct drumkv1_gen
{
	float *sample, sample0;
	float *coarse;
	float *fine;
};


// dcf

struct drumkv1_dcf
{
	float *cutoff;
	float *reso;
	float *type;
	float *slope;
	float *envelope;

	drumkv1_env env;
};


// lfo

struct drumkv1_lfo
{
	float *shape;
	float *width;
	float *rate;
	float *sweep;
	float *pitch;
	float *cutoff;
	float *reso;
	float *panning;
	float *volume;

	drumkv1_env env;
};


// dca

struct drumkv1_dca
{
	float *volume;

	drumkv1_env env;
};



// def (ranges)

struct drumkv1_def
{
	float *pitchbend;
	float *modwheel;
	float *pressure;
};


// out (mix)

struct drumkv1_out
{
	float *width;
	float *panning;
	float *volume;
};


// chorus (fx)

struct drumkv1_cho
{
	float *wet;
	float *delay;
	float *feedb;
	float *rate;
	float *mod;
};


// flanger (fx)

struct drumkv1_fla
{
	float *wet;
	float *delay;
	float *feedb;
	float *daft;
};


// phaser (fx)

struct drumkv1_pha
{
	float *wet;
	float *rate;
	float *feedb;
	float *depth;
	float *daft;
};


// delay (fx)

struct drumkv1_del
{
	float *wet;
	float *delay;
	float *feedb;
	float *bpm;
};


// dynamic(compressor/limiter)

struct drumkv1_dyn
{
	float *compress;
	float *limiter;
};


// (Hal Chamberlin's state variable) filter

class drumkv1_filter1
{
public:

	enum Type { Low = 0, Band, High, Notch };

	drumkv1_filter1(Type type = Low, uint16_t nover = 2)
		{ reset(type, nover); }

	Type type() const
		{ return m_type; }

	void reset(Type type = Low, uint16_t nover = 2)
	{
		m_type  = type;
		m_nover = nover;

		m_low   = 0.0f;
		m_band  = 0.0f;
		m_high  = 0.0f;
		m_notch = 0.0f;

		switch (m_type) {
		case Notch:
			m_out = &m_notch;
			break;
		case High:
			m_out = &m_high;
			break;
		case Band:
			m_out = &m_band;
			break;
		case Low:
		default:
			m_out = &m_low;
			break;
		}
	}

	float output(float input, float cutoff, float reso)
	{
		const float q = (1.0f - reso);

		for (uint16_t i = 0; i < m_nover; ++i) {
			m_low  += cutoff * m_band;
			m_high  = input - m_low - q * m_band;
			m_band += cutoff * m_high;
			m_notch = m_high + m_low;
		}

		return *m_out;
	}

private:

	Type     m_type;

	uint16_t m_nover;

	float    m_low;
	float    m_band;
	float    m_high;
	float    m_notch;

	float   *m_out;
};


// (second kind of) filter

class drumkv1_filter2
{
public:

	enum Type { Low = 0, Band, High, Notch };

	drumkv1_filter2(Type type = Low) { reset(type); }

	Type type() const
		{ return m_type; }

	void reset(Type type = Low)
	{
		m_type = type;

		m_b0 = m_b1 = m_b2 = m_b3 = m_b4 = 0.0f;
		m_t1 = m_t2 = m_t3 = 0.0f;
		m_f  = m_p  = m_q  = 0.0f;
	}

	float output(float input, float cutoff, float reso)
	{
		m_q = 1.0f - cutoff;
		m_p = cutoff + 0.8f * cutoff * m_q;
		m_f = m_p + m_p - 1.0f;
		m_q = 0.89f * reso * (1.0f + 0.5f * m_q * (1.0f - m_q + 5.6f * m_q * m_q));

		input -= m_q * m_b4;

		m_t1 = m_b1;
		m_b1 = (input + m_b0) * m_p - m_b1 * m_f;
		m_t2 = m_b2;
		m_b2 = (m_b1 + m_t1) * m_p - m_b2 * m_f;
		m_t1 = m_b3;
		m_b3 = (m_b2 + m_t2) * m_p - m_b3 * m_f;
		m_b4 = (m_b3 + m_t1) * m_p - m_b4 * m_f;
		m_b0 = input;

		switch (m_type) {
		case Notch:
			return 3.0f * (m_b3 - m_b4) - input;
		case High:
			return input - m_b4;
		case Band:
			return 3.0f * (m_b3 - m_b4);
		case Low:
		default:
			return m_b4;
		}
	}

private:

	Type   m_type;

	float  m_b0, m_b1, m_b2, m_b3, m_b4;
	float  m_t1, m_t2, m_t3;
	float  m_f,  m_p,  m_q;
};


// panning smoother (3 parameters)

class drumkv1_pan : public drumkv1_ramp3
{
public:

	drumkv1_pan() : drumkv1_ramp3(2) {}

protected:

	virtual float evaluate(uint16_t i)
	{
		drumkv1_ramp3::evaluate(i);

		const float wpan = 0.25f * M_PI
			* (1.0f + m_param1_v)
			* (1.0f + m_param2_v)
			* (1.0f + m_param3_v);

		return M_SQRT2 * (i == 0 ? ::cosf(wpan) : ::sinf(wpan));
	}
};



// convert note to frequency (hertz)

inline float note_freq ( float note )
{
	return (440.0f / 32.0f) * ::powf(2.0f, (note - 9.0f) / 12.0f);
}


// forward decl.
class drumkv1_impl;

// voice

struct drumkv1_voice
{
	drumkv1_voice(drumkv1_impl *pImpl);

	drumkv1_generator  gen1;
	drumkv1_oscillator lfo1;

	int note;									// voice note
	float vel;									// velocity to vol

	float gen1_freq;							// frequency and phase

	float lfo1_sample;

	drumkv1_filter1 dcf11, dcf12, dcf13, dcf14;	// filters

	drumkv1_env::State dca1_env;				// envelope states
	drumkv1_env::State dcf1_env;
	drumkv1_env::State lfo1_env;

	struct list_node
	{
		list_node() : prev(0), next(0) {}

		void append(drumkv1_voice *pv)
		{
			pv->node.prev = prev;
			pv->node.next = 0;

			if (prev)
				prev->node.next = pv;
			else
				next = pv;

			prev = pv;
		}

		void remove(drumkv1_voice *pv)
		{
			if (pv->node.prev)
				pv->node.prev->node.next = pv->node.next;
			else
				next = pv->node.next;

			if (pv->node.next)
				pv->node.next->node.prev = pv->node.prev;
			else
				prev = pv->node.prev;
		}

		drumkv1_voice *prev;
		drumkv1_voice *next;

	} node;
};


// polyphonic synth implementation

class drumkv1_impl
{
public:

	drumkv1_impl(uint16_t iChannels, uint32_t iSampleRate);

	~drumkv1_impl();

	void setChannels(uint16_t iChannels);
	uint16_t channels() const;

	void setSampleRate(uint32_t iSampleRate);
	uint32_t sampleRate() const;

	void setSampleFile(const char *pszSampleFile);
	const char *sampleFile() const;

	void setParamPort(drumkv1::ParamIndex index, float *pfParam = 0);
	float *paramPort(drumkv1::ParamIndex index);

	void process_midi(uint8_t *data, uint32_t size);
	void process(float **ins, float **outs, uint32_t nframes);

	void reset();

	drumkv1_sample gen1_sample;
	drumkv1_wave   lfo1_wave;

	float gen1_last;

protected:

	void allSoundOff();
	void allControllersOff();
	void allNotesOff();

	drumkv1_voice *alloc_voice ()
	{
		drumkv1_voice *pv = m_free_list.next;
		if (pv) {
			m_free_list.remove(pv);
			m_play_list.append(pv);
		}
		return pv;
	}

	void free_voice ( drumkv1_voice *pv )
	{
		m_play_list.remove(pv);
		m_free_list.append(pv);
	}

private:

	uint16_t m_iChannels;
	uint32_t m_iSampleRate;

	drumkv1_ctl m_ctl;

	drumkv1_gen m_gen1;
	drumkv1_dcf m_dcf1;
	drumkv1_lfo m_lfo1;
	drumkv1_dca m_dca1;
	drumkv1_def m_def1;
	drumkv1_out m_out1;

	drumkv1_cho m_cho;
	drumkv1_fla m_fla;
	drumkv1_pha m_pha;
	drumkv1_del m_del;
	drumkv1_dyn m_dyn;

	drumkv1_voice **m_voices;
	drumkv1_voice  *m_notes[MAX_NOTES];

	drumkv1_voice::list_node m_free_list;
	drumkv1_voice::list_node m_play_list;

	drumkv1_aux   m_aux1;

	drumkv1_ramp1 m_wid1;
	drumkv1_pan   m_pan1;
	drumkv1_ramp4 m_vol1;

	drumkv1_ramp2 m_pre1;

	drumkv1_fx_chorus   m_chorus;
	drumkv1_fx_flanger *m_flanger;
	drumkv1_fx_phaser  *m_phaser;
	drumkv1_fx_delay   *m_delay;
	drumkv1_fx_comp    *m_comp;
};


// voice constructor

drumkv1_voice::drumkv1_voice ( drumkv1_impl *pImpl ) :
	gen1(pImpl->gen1_sample),
	lfo1(pImpl->lfo1_wave)
{
}


// constructor

drumkv1_impl::drumkv1_impl ( uint16_t iChannels, uint32_t iSampleRate )
{
	// null sample.
	m_gen1.sample0 = 0.0f;

	// glide note.
	gen1_last = 0.0f;

	// allocate voice pool.
	m_voices = new drumkv1_voice * [MAX_VOICES];

	for (int i = 0; i < MAX_VOICES; ++i) {
		m_voices[i] = new drumkv1_voice(this);
		m_free_list.append(m_voices[i]);
	}

	for (int note = 0; note < MAX_NOTES; ++note)
		m_notes[note] = 0;

	// flangers none yet
	m_flanger = 0;

	// phasers none yet
	m_phaser = 0;

	// delays none yet
	m_delay = 0;

	// compressors none yet
	m_comp = 0;

	// number of channels
	setChannels(iChannels);

	// parameters
	for (int i = 0; i < int(drumkv1::NUM_PARAMS); ++i)
		setParamPort(drumkv1::ParamIndex(i));

	// set default sample rate
	setSampleRate(iSampleRate);

	// reset all voices
	allControllersOff();
	allNotesOff();
}


// destructor

drumkv1_impl::~drumkv1_impl (void)
{
	// deallocate sample filenames
	setSampleFile(0);

	// deallocate voice pool.
	for (int i = 0; i < MAX_VOICES; ++i)
		delete m_voices[i];

	delete [] m_voices;

	// deallocate channels
	setChannels(0);
}


void drumkv1_impl::setChannels ( uint16_t iChannels )
{
	m_iChannels = iChannels;

	// deallocate flangers
	if (m_flanger) {
		delete [] m_flanger;
		m_flanger = 0;
	}

	// deallocate phasers
	if (m_phaser) {
		delete [] m_phaser;
		m_phaser = 0;
	}

	// deallocate delays
	if (m_delay) {
		delete [] m_delay;
		m_delay = 0;
	}

	// deallocate compressors
	if (m_comp) {
		delete [] m_comp;
		m_comp = 0;
	}
}


uint16_t drumkv1_impl::channels (void) const
{
	return m_iChannels;
}


void drumkv1_impl::setSampleRate ( uint32_t iSampleRate )
{
	// set internal sample rate
	m_iSampleRate = iSampleRate;

	// update waves sample rate
	gen1_sample.setSampleRate(m_iSampleRate);
	lfo1_wave.setSampleRate(m_iSampleRate);

	// update envelope range times in frames
	const float srate_ms = 0.001f * float(m_iSampleRate);

	const uint32_t min_frames = uint32_t(srate_ms * MIN_ENV_MSECS);
	const uint32_t max_frames = uint32_t(srate_ms * MAX_ENV_MSECS);

	m_dcf1.env.min_frames = min_frames;
	m_dcf1.env.max_frames = max_frames;

	m_lfo1.env.min_frames = min_frames;
	m_lfo1.env.max_frames = max_frames;

	m_dca1.env.min_frames = min_frames;
	m_dca1.env.max_frames = max_frames;
}


uint32_t drumkv1_impl::sampleRate (void) const
{
	return m_iSampleRate;
}


void drumkv1_impl::setSampleFile ( const char *pszSampleFile )
{
	reset();

	gen1_sample.close();

	if (pszSampleFile) {
		m_gen1.sample0 = *m_gen1.sample;
		gen1_sample.open(pszSampleFile, note_freq(m_gen1.sample0));
	}
}


const char *drumkv1_impl::sampleFile (void) const
{
	return gen1_sample.filename();
}


void drumkv1_impl::setParamPort ( drumkv1::ParamIndex index, float *pfParam )
{
	static float s_fDummy = 0.0f;

	if (pfParam == 0)
		pfParam = &s_fDummy;

	switch (index) {
	case drumkv1::GEN1_SAMPLE:    m_gen1.sample      = pfParam; break;
	case drumkv1::GEN1_COARSE:    m_gen1.coarse      = pfParam; break;
	case drumkv1::GEN1_FINE:      m_gen1.fine        = pfParam; break;
	case drumkv1::DCF1_CUTOFF:    m_dcf1.cutoff      = pfParam; break;
	case drumkv1::DCF1_RESO:      m_dcf1.reso        = pfParam; break;
	case drumkv1::DCF1_TYPE:      m_dcf1.type        = pfParam; break;
	case drumkv1::DCF1_SLOPE:     m_dcf1.slope       = pfParam; break;
	case drumkv1::DCF1_ENVELOPE:  m_dcf1.envelope    = pfParam; break;
	case drumkv1::DCF1_ATTACK:    m_dcf1.env.attack  = pfParam; break;
	case drumkv1::DCF1_DECAY1:    m_dcf1.env.decay1  = pfParam; break;
	case drumkv1::DCF1_DECAY2:    m_dcf1.env.decay2  = pfParam; break;
	case drumkv1::LFO1_SHAPE:     m_lfo1.shape       = pfParam; break;
	case drumkv1::LFO1_WIDTH:     m_lfo1.width       = pfParam; break;
	case drumkv1::LFO1_RATE:      m_lfo1.rate        = pfParam; break;
	case drumkv1::LFO1_SWEEP:     m_lfo1.sweep       = pfParam; break;
	case drumkv1::LFO1_PITCH:     m_lfo1.pitch       = pfParam; break;
	case drumkv1::LFO1_CUTOFF:    m_lfo1.cutoff      = pfParam; break;
	case drumkv1::LFO1_RESO:      m_lfo1.reso        = pfParam; break;
	case drumkv1::LFO1_PANNING:   m_lfo1.panning     = pfParam; break;
	case drumkv1::LFO1_VOLUME:    m_lfo1.volume      = pfParam; break;
	case drumkv1::LFO1_ATTACK:    m_lfo1.env.attack  = pfParam; break;
	case drumkv1::LFO1_DECAY1:    m_lfo1.env.decay1  = pfParam; break;
	case drumkv1::LFO1_DECAY2:    m_lfo1.env.decay2  = pfParam; break;
	case drumkv1::DCA1_VOLUME:    m_dca1.volume      = pfParam; break;
	case drumkv1::DCA1_ATTACK:    m_dca1.env.attack  = pfParam; break;
	case drumkv1::DCA1_DECAY1:    m_dca1.env.decay1  = pfParam; break;
	case drumkv1::DCA1_DECAY2:    m_dca1.env.decay2  = pfParam; break;
	case drumkv1::DEF1_PITCHBEND: m_def1.pitchbend   = pfParam; break;
	case drumkv1::DEF1_MODWHEEL:  m_def1.modwheel    = pfParam; break;
	case drumkv1::DEF1_PRESSURE:  m_def1.pressure    = pfParam; break;
	case drumkv1::OUT1_WIDTH:     m_out1.width       = pfParam; break;
	case drumkv1::OUT1_PANNING:   m_out1.panning     = pfParam; break;
	case drumkv1::OUT1_VOLUME:    m_out1.volume      = pfParam; break;
	case drumkv1::CHO1_WET:       m_cho.wet          = pfParam; break;
	case drumkv1::CHO1_DELAY:     m_cho.delay        = pfParam; break;
	case drumkv1::CHO1_FEEDB:     m_cho.feedb        = pfParam; break;
	case drumkv1::CHO1_RATE:      m_cho.rate         = pfParam; break;
	case drumkv1::CHO1_MOD:       m_cho.mod          = pfParam; break;
	case drumkv1::FLA1_WET:       m_fla.wet          = pfParam; break;
	case drumkv1::FLA1_DELAY:     m_fla.delay        = pfParam; break;
	case drumkv1::FLA1_FEEDB:     m_fla.feedb        = pfParam; break;
	case drumkv1::FLA1_DAFT:      m_fla.daft         = pfParam; break;
	case drumkv1::PHA1_WET:       m_pha.wet          = pfParam; break;
	case drumkv1::PHA1_RATE:      m_pha.rate         = pfParam; break;
	case drumkv1::PHA1_FEEDB:     m_pha.feedb        = pfParam; break;
	case drumkv1::PHA1_DEPTH:     m_pha.depth        = pfParam; break;
	case drumkv1::PHA1_DAFT:      m_pha.daft         = pfParam; break;
	case drumkv1::DEL1_WET:       m_del.wet          = pfParam; break;
	case drumkv1::DEL1_DELAY:     m_del.delay        = pfParam; break;
	case drumkv1::DEL1_FEEDB:     m_del.feedb        = pfParam; break;
	case drumkv1::DEL1_BPM:       m_del.bpm          = pfParam; break;
	case drumkv1::DYN1_COMPRESS:  m_dyn.compress     = pfParam; break;
	case drumkv1::DYN1_LIMITER:   m_dyn.limiter      = pfParam; break;
	default: break;
	}
}


float *drumkv1_impl::paramPort ( drumkv1::ParamIndex index )
{
	float *pfParam= 0;

	switch (index) {
	case drumkv1::GEN1_SAMPLE:    pfParam = m_gen1.sample;      break;
	case drumkv1::GEN1_COARSE:    pfParam = m_gen1.coarse;      break;
	case drumkv1::GEN1_FINE:      pfParam = m_gen1.fine;        break;
	case drumkv1::DCF1_CUTOFF:    pfParam = m_dcf1.cutoff;      break;
	case drumkv1::DCF1_RESO:      pfParam = m_dcf1.reso;        break;
	case drumkv1::DCF1_TYPE:      pfParam = m_dcf1.type;        break;
	case drumkv1::DCF1_SLOPE:     pfParam = m_dcf1.slope;       break;
	case drumkv1::DCF1_ENVELOPE:  pfParam = m_dcf1.envelope;    break;
	case drumkv1::DCF1_ATTACK:    pfParam = m_dcf1.env.attack;  break;
	case drumkv1::DCF1_DECAY1:    pfParam = m_dcf1.env.decay1;  break;
	case drumkv1::DCF1_DECAY2:    pfParam = m_dcf1.env.decay2;  break;
	case drumkv1::LFO1_SHAPE:     pfParam = m_lfo1.shape;       break;
	case drumkv1::LFO1_WIDTH:     pfParam = m_lfo1.width;       break;
	case drumkv1::LFO1_RATE:      pfParam = m_lfo1.rate;        break;
	case drumkv1::LFO1_SWEEP:     pfParam = m_lfo1.sweep;       break;
	case drumkv1::LFO1_PITCH:     pfParam = m_lfo1.pitch;       break;
	case drumkv1::LFO1_CUTOFF:    pfParam = m_lfo1.cutoff;      break;
	case drumkv1::LFO1_RESO:      pfParam = m_lfo1.reso;        break;
	case drumkv1::LFO1_PANNING:   pfParam = m_lfo1.panning;     break;
	case drumkv1::LFO1_VOLUME:    pfParam = m_lfo1.volume;      break;
	case drumkv1::LFO1_ATTACK:    pfParam = m_lfo1.env.attack;  break;
	case drumkv1::LFO1_DECAY1:    pfParam = m_lfo1.env.decay1;  break;
	case drumkv1::LFO1_DECAY2:    pfParam = m_lfo1.env.decay2;  break;
	case drumkv1::DCA1_VOLUME:    pfParam = m_dca1.volume;      break;
	case drumkv1::DCA1_ATTACK:    pfParam = m_dca1.env.attack;  break;
	case drumkv1::DCA1_DECAY1:    pfParam = m_dca1.env.decay1;  break;
	case drumkv1::DCA1_DECAY2:    pfParam = m_dca1.env.decay2;  break;
	case drumkv1::DEF1_PITCHBEND: pfParam = m_def1.pitchbend;   break;
	case drumkv1::DEF1_MODWHEEL:  pfParam = m_def1.modwheel;    break;
	case drumkv1::DEF1_PRESSURE:  pfParam = m_def1.pressure;    break;
	case drumkv1::OUT1_WIDTH:     pfParam = m_out1.width;       break;
	case drumkv1::OUT1_PANNING:   pfParam = m_out1.panning;     break;
	case drumkv1::OUT1_VOLUME:    pfParam = m_out1.volume;      break;
	case drumkv1::CHO1_WET:       pfParam = m_cho.wet;          break;
	case drumkv1::CHO1_DELAY:     pfParam = m_cho.delay;        break;
	case drumkv1::CHO1_FEEDB:     pfParam = m_cho.feedb;        break;
	case drumkv1::CHO1_RATE:      pfParam = m_cho.rate;         break;
	case drumkv1::CHO1_MOD:       pfParam = m_cho.mod;          break;
	case drumkv1::FLA1_WET:       pfParam = m_fla.wet;          break;
	case drumkv1::FLA1_DELAY:     pfParam = m_fla.delay;        break;
	case drumkv1::FLA1_FEEDB:     pfParam = m_fla.feedb;        break;
	case drumkv1::FLA1_DAFT:      pfParam = m_fla.daft;         break;
	case drumkv1::PHA1_WET:       pfParam = m_pha.wet;          break;
	case drumkv1::PHA1_RATE:      pfParam = m_pha.rate;         break;
	case drumkv1::PHA1_FEEDB:     pfParam = m_pha.feedb;        break;
	case drumkv1::PHA1_DEPTH:     pfParam = m_pha.depth;        break;
	case drumkv1::PHA1_DAFT:      pfParam = m_pha.daft;         break;
	case drumkv1::DEL1_WET:       pfParam = m_del.wet;          break;
	case drumkv1::DEL1_DELAY:     pfParam = m_del.delay;        break;
	case drumkv1::DEL1_FEEDB:     pfParam = m_del.feedb;        break;
	case drumkv1::DEL1_BPM:       pfParam = m_del.bpm;          break;
	case drumkv1::DYN1_COMPRESS:  pfParam = m_dyn.compress;     break;
	case drumkv1::DYN1_LIMITER:   pfParam = m_dyn.limiter;      break;
	default: break;
	}

	return pfParam;
}


// handle midi input

void drumkv1_impl::process_midi ( uint8_t *data, uint32_t size )
{
	// check data size (#1)
	if (size < 2)
		return;

	// note on
	const int status = (data[0] & 0xf0);
	const int key    = (data[1] & 0x7f);

	if (status == 0xd0) {
		// channel aftertouch
		m_ctl.pressure = float(key) / 127.0f;
	}

	// check data size (#2)
	if (size < 3)
		return;

	const int value  = (data[2] & 0x7f);

	// note on
	if (status == 0x90 && value > 0) {
		drumkv1_voice *pv = m_notes[key];
		if (pv) {
			// retrigger fast release
			m_dcf1.env.note_off_fast(&pv->dcf1_env);
			m_lfo1.env.note_off_fast(&pv->lfo1_env);
			m_dca1.env.note_off_fast(&pv->dca1_env);
			pv->note = -1;
			m_notes[key] = 0;
		}
		// find free voice
		pv = alloc_voice();
		if (pv) {
			// waveform
			pv->note = key;
			// velocity
			pv->vel = float(value) / 127.0f;
			pv->vel *= pv->vel; // quadratic velocity law
			// generate
			pv->gen1.start();
			// frequencies
			const float freq1 = float(key)
				+ *m_gen1.coarse * COARSE_SCALE
				+ *m_gen1.fine * FINE_SCALE;
			pv->gen1_freq = note_freq(freq1);
			// filters
			const drumkv1_filter1::Type type1
				= drumkv1_filter1::Type(int(*m_dcf1.type));
			pv->dcf11.reset(type1);
			pv->dcf12.reset(type1);
			pv->dcf13.reset(type1);
			pv->dcf14.reset(type1);
			// envelopes
			m_dcf1.env.start(&pv->dcf1_env);
			m_lfo1.env.start(&pv->lfo1_env);
			m_dca1.env.start(&pv->dca1_env);
			// lfos
			pv->lfo1_sample = pv->lfo1.start();
			// allocated
			m_notes[key] = pv;
		}
	}
	// note off
	else if (status == 0x80 || (status == 0x90 && value == 0)) {
		drumkv1_voice *pv = m_notes[key];
		if (pv && pv->note >= 0) {
			if (pv->dca1_env.stage != drumkv1_env::Decay2) {
				m_dca1.env.note_off(&pv->dca1_env);
				m_dcf1.env.note_off(&pv->dcf1_env);
				m_lfo1.env.note_off(&pv->lfo1_env);
			}
		}
	}
	// control change
	else if (status == 0xb0) {
		switch (key) {
		case 0x01:
			// modulation wheel (cc#1)
			m_ctl.modwheel = float(value) / 127.0f;
			break;
		case 0x07:
			// channel volume (cc#7)
			m_ctl.volume = float(value) / 127.0f;
			break;
		case 0x0a:
			// channel panning (cc#10)
			m_ctl.panning = float(value - 64) / 64.0f;
			break;
		case 0x78:
			// all sound off (cc#120)
			allSoundOff();
			break;
		case 0x79:
			// all controllers off (cc#121)
			allControllersOff();
			break;
		case 0x7b:
			// all notes off (cc#123)
			allNotesOff();
			break;
		}
	}
	// pitch bend
	else if (status == 0xe0) {
		m_ctl.pitchbend = float(key + (value << 7) - 0x2000) / 8192.0f;
	}
}


// all controllers off

void drumkv1_impl::allControllersOff (void)
{
	m_ctl.reset();
}


// all sound off

void drumkv1_impl::allSoundOff (void)
{
	m_chorus.setSampleRate(m_iSampleRate);
	m_chorus.reset();

	for (uint16_t k = 0; k < m_iChannels; ++k) {
		m_phaser[k].setSampleRate(m_iSampleRate);
		m_delay[k].setSampleRate(m_iSampleRate);
		m_comp[k].setSampleRate(m_iSampleRate);
		m_flanger[k].reset();
		m_phaser[k].reset();
		m_delay[k].reset();
		m_comp[k].reset();
	}
}


// all notes off

void drumkv1_impl::allNotesOff (void)
{
	drumkv1_voice *pv = m_play_list.next;
	while (pv) {
		if (pv->note >= 0)
			m_notes[pv->note] = 0;
		free_voice(pv);
		pv = m_play_list.next;
	}

	gen1_last = 0.0f;

	m_aux1.reset();
}


// all reset clear

void drumkv1_impl::reset (void)
{
	m_vol1.reset(m_out1.volume, m_dca1.volume, &m_ctl.volume, &m_aux1.volume);
	m_pan1.reset(m_out1.panning, &m_ctl.panning, &m_aux1.panning);
	m_wid1.reset(m_out1.width);

	m_pre1.reset(m_def1.pressure, &m_ctl.pressure);

	// flangers
	if (m_flanger == 0)
		m_flanger = new drumkv1_fx_flanger [m_iChannels];

	// phasers
	if (m_phaser == 0)
		m_phaser = new drumkv1_fx_phaser [m_iChannels];

	// delays
	if (m_delay == 0)
		m_delay = new drumkv1_fx_delay [m_iChannels];

	// compressors
	if (m_comp == 0)
		m_comp = new drumkv1_fx_comp [m_iChannels];

	allSoundOff();
//	allControllersOff();
	allNotesOff();
}


// synthesize

void drumkv1_impl::process ( float **ins, float **outs, uint32_t nframes )
{
	float *v_outs[m_iChannels];

	// buffer i/o transfer

	uint16_t k;

	for (k = 0; k < m_iChannels; ++k)
		::memcpy(outs[k], ins[k], nframes * sizeof(float));

	// channel indexes

	const uint16_t k11 = 0;
	const uint16_t k12 = (gen1_sample.channels() > 1 ? 1 : 0);

	// controls

	const float lfo1_rate  = *m_lfo1.rate  * *m_lfo1.rate;
	const float lfo1_freq  = LFO_FREQ_MIN + lfo1_rate * (LFO_FREQ_MAX - LFO_FREQ_MIN);
	const float lfo1_pitch = *m_lfo1.pitch * *m_lfo1.pitch;
	const float modwheel1  = *m_def1.modwheel * (lfo1_pitch + m_ctl.modwheel);
	const float pitchbend1 = (1.0f + *m_def1.pitchbend * m_ctl.pitchbend);

	if (m_gen1.sample0 != *m_gen1.sample) {
		m_gen1.sample0  = *m_gen1.sample;
		gen1_sample.reset(note_freq(m_gen1.sample0));
	}

	if (int(*m_lfo1.shape) != int(lfo1_wave.shape()) || *m_lfo1.width != lfo1_wave.width())
		lfo1_wave.reset(drumkv1_wave::Shape(*m_lfo1.shape), *m_lfo1.width);

	m_wid1.process(nframes);
	m_pan1.process(nframes);
	m_vol1.process(nframes);

	m_pre1.process(nframes);

	// per voice

	drumkv1_voice *pv = m_play_list.next;

	while (pv) {

		drumkv1_voice *pv_next = pv->node.next;

		// output buffers

		for (k = 0; k < m_iChannels; ++k)
			v_outs[k] = outs[k];

		uint32_t nblock = nframes;

		while (nblock > 0) {

			uint32_t ngen = nblock;

			// process envelope stages

			if (pv->dca1_env.running && pv->dca1_env.frames < ngen)
				ngen = pv->dca1_env.frames;
			if (pv->dcf1_env.running && pv->dcf1_env.frames < ngen)
				ngen = pv->dcf1_env.frames;
			if (pv->lfo1_env.running && pv->lfo1_env.frames < ngen)
				ngen = pv->lfo1_env.frames;

			for (uint32_t j = 0; j < ngen; ++j) {

				// velocities

				const float vel1
					= (pv->vel + (1.0f - pv->vel) * m_pre1.value(j));

				// generators

				const float lfo1_env = pv->lfo1_env.value2(j);
				const float lfo1 = pv->lfo1_sample * lfo1_env;

				pv->gen1.next(pv->gen1_freq * (pitchbend1 + modwheel1 * lfo1));

				const float gen1 = pv->gen1.value(k11);
				const float gen2 = pv->gen1.value(k12);

				pv->lfo1_sample = pv->lfo1.sample(lfo1_freq
					* (1.0f + SWEEP_SCALE * *m_lfo1.sweep * lfo1_env));

				// filters

				const float env1 = 0.5f * (1.0f + vel1
					* *m_dcf1.envelope * pv->dcf1_env.value2(j));
				const float cutoff1 = drumkv1_sigmoid1(*m_dcf1.cutoff
					* env1 * (1.0f + *m_lfo1.cutoff * lfo1));
				const float reso1 = drumkv1_sigmoid1(*m_dcf1.reso
					* env1 * (1.0f + *m_lfo1.reso * lfo1));

				float dcf11 = pv->dcf11.output(gen1, cutoff1, reso1);
				float dcf12 = pv->dcf12.output(gen2, cutoff1, reso1);
				if (int(*m_dcf1.slope) > 0) { // 24db/octave
					dcf11 = pv->dcf13.output(dcf11, cutoff1, reso1);
					dcf12 = pv->dcf14.output(dcf12, cutoff1, reso1);
				}

				// volumes

				const float wid1 = m_wid1.value(j);
				const float mid1 = 0.5f * (dcf11 + dcf12);
				const float sid1 = 0.5f * (dcf11 - dcf12);
				const float vol1 = vel1 * m_vol1.value(j)
					* pv->dca1_env.value2(j);

				// outputs

				const float out1
					= vol1 * (mid1 + sid1 * wid1) * m_pan1.value(j, 0);
				const float out2
					= vol1 * (mid1 - sid1 * wid1) * m_pan1.value(j, 1);

				for (k = 0; k < m_iChannels; ++k)
					*v_outs[k]++ += (k & 1 ? out2 : out1);

				if (j == 0) {
					m_aux1.panning = lfo1 * *m_lfo1.panning;
					m_aux1.volume  = lfo1 * *m_lfo1.volume + 1.0f;
				}
			}

			nblock -= ngen;

			// envelope countdowns

			if (pv->dca1_env.running) {
				if (pv->dca1_env.frames >= ngen) {
					pv->dca1_env.frames -= ngen;
					pv->dca1_env.level  += ngen * pv->dca1_env.delta;
				}
				else pv->dca1_env.frames = 0;
				if (pv->dca1_env.frames == 0)
					m_dca1.env.next(&pv->dca1_env);
			}

			if (pv->dca1_env.stage == drumkv1_env::Done || pv->gen1.isOver()) {
				if (pv->note >= 0)
					m_notes[pv->note] = 0;
				free_voice(pv);
				nblock = 0;
			} else {
				if (pv->dcf1_env.running) {
					if (pv->dcf1_env.frames >= ngen) {
						pv->dcf1_env.frames -= ngen;
						pv->dcf1_env.level  += ngen * pv->dcf1_env.delta;
					}
					else pv->dcf1_env.frames = 0;
					if (pv->dcf1_env.frames == 0)
						m_dcf1.env.next(&pv->dcf1_env);
				}
				if (pv->lfo1_env.running) {
					if (pv->lfo1_env.frames >= ngen) {
						pv->lfo1_env.frames -= ngen;
						pv->lfo1_env.level  += ngen * pv->lfo1_env.delta;
					}
					else pv->lfo1_env.frames = 0;
					if (pv->lfo1_env.frames == 0)
						m_lfo1.env.next(&pv->lfo1_env);
				}
			}
		}

		// next playing voice

		pv = pv_next;
	}

	// effects

	for (k = 0; k < m_iChannels; ++k) {
		float *in = outs[k];
		float *out = in;
		// chorus
		if (k > 0) {
			m_chorus.process(outs[k - 1], outs[k], nframes, *m_cho.wet,
				*m_cho.delay, *m_cho.feedb, *m_cho.rate, *m_cho.mod);
		}
		// flanger
		m_flanger[k].process(in, nframes, *m_fla.wet,
			*m_fla.delay, *m_fla.feedb, *m_fla.daft * float(k));
		// phaser
		m_phaser[k].process(in, nframes, *m_pha.wet,
			*m_pha.rate, *m_pha.feedb, *m_pha.depth, *m_pha.daft * float(k));
		// delay
		m_delay[k].process(in, nframes, *m_del.wet,
			*m_del.delay, *m_del.feedb, *m_del.bpm * 100.0f);
		// compressor
		if (int(*m_dyn.compress) > 0)
			m_comp[k].process(in, nframes);
		// limiter
		if (int(*m_dyn.limiter) > 0) {
			for (uint32_t n = 0; n < nframes; ++n)
				*out++ = drumkv1_sigmoid(*in++);
		}
	}
}


//-------------------------------------------------------------------------
// drumkv1 - decl.
//

drumkv1::drumkv1 ( uint16_t iChannels, uint32_t iSampleRate )
{
	m_pImpl = new drumkv1_impl(iChannels, iSampleRate);
}


drumkv1::~drumkv1 (void)
{
	delete m_pImpl;
}


void drumkv1::setChannels ( uint16_t iChannels )
{
	m_pImpl->setChannels(iChannels);
}


uint16_t drumkv1::channels (void) const
{
	return m_pImpl->channels();
}


void drumkv1::setSampleRate ( uint32_t iSampleRate )
{
	m_pImpl->setSampleRate(iSampleRate);
}


uint32_t drumkv1::sampleRate (void) const
{
	return m_pImpl->sampleRate();
}


void drumkv1::setSampleFile ( const char *pszSampleFile )
{
	m_pImpl->setSampleFile(pszSampleFile);
}


const char *drumkv1::sampleFile (void) const
{
	return m_pImpl->sampleFile();
}


drumkv1_sample *drumkv1::sample (void) const
{
	return &(m_pImpl->gen1_sample);
}


void drumkv1::setParamPort ( ParamIndex index, float *pfParam )
{
	m_pImpl->setParamPort(index, pfParam);
}

float *drumkv1::paramPort ( ParamIndex index ) const
{
	return m_pImpl->paramPort(index);
}


void drumkv1::process_midi ( uint8_t *data, uint32_t size )
{
#ifdef CONFIG_DEBUG_0
	fprintf(stderr, "drumkv1[%p]::process_midi(%u)", this, size);
	for (uint32_t i = 0; i < size; ++i)
		fprintf(stderr, " %02x", data[i]);
	fprintf(stderr, "\n");
#endif

	m_pImpl->process_midi(data, size);
}


void drumkv1::process ( float **ins, float **outs, uint32_t nframes )
{
	m_pImpl->process(ins, outs, nframes);
}


// all reset clear

void drumkv1::reset (void)
{
	m_pImpl->reset();
}


// end of drumkv1.cpp