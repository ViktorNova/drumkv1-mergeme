// drumkv1_jack.cpp
//
/****************************************************************************
   Copyright (C) 2012-2016, rncbc aka Rui Nuno Capela. All rights reserved.

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

#include "drumkv1_jack.h"
#include "drumkv1_config.h"
#include "drumkv1_param.h"

#include "drumkv1_programs.h"
#include "drumkv1_controls.h"

#include <jack/midiport.h>

#include <stdio.h>
#include <string.h>

#include <math.h>

#include <QCoreApplication>
#include <QDir>


#ifdef CONFIG_ALSA_MIDI

//-------------------------------------------------------------------------
// alsa input thread.

#include <QThread>


class drumkv1_alsa_thread : public QThread
{
public:

	drumkv1_alsa_thread(drumkv1_jack *drumk)
		: QThread(), m_drumk(drumk), m_running(false) {}

	~drumkv1_alsa_thread()
	{	// fake sync and wait
		if (m_running && isRunning()) do {
			m_running = false;
		}
		while (!wait(100));
	}

protected:

	void run()
	{
		snd_seq_t *seq = m_drumk->alsa_seq();
		if (seq == NULL)
			return;

		m_running = true;

		int nfds;
		struct pollfd *pfds;

		nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
		pfds = (struct pollfd *) alloca(nfds * sizeof(struct pollfd));
		snd_seq_poll_descriptors(seq, pfds, nfds, POLLIN);

		int poll_rc = 0;

		while (m_running && poll_rc >= 0) {
			poll_rc = ::poll(pfds, nfds, 200);
			while (poll_rc > 0) {
				snd_seq_event_t *ev = NULL;
				snd_seq_event_input(seq, &ev);
				m_drumk->alsa_capture(ev);
			//	snd_seq_free_event(ev);
				poll_rc = snd_seq_event_input_pending(seq, 0);
			}
		}

		m_running = false;
	}

private:

	drumkv1_jack *m_drumk;

	bool m_running;
};

#endif	// CONFIG_ALSA_MIDI


//-------------------------------------------------------------------------
// JACK process callback.

static
int drumkv1_jack_process ( jack_nframes_t nframes, void *arg )
{
	return static_cast<drumkv1_jack *> (arg)->process(nframes);
}

//----------------------------------------------------------------------
// JACK buffer-size change callback.

static int drumkv1_jack_buffer_size ( jack_nframes_t nframes, void *arg )
{
	static_cast<drumkv1_jack *> (arg)->setBufferSize(nframes);

	return 0;
}


#ifdef CONFIG_JACK_SESSION

#include <jack/session.h>

#include <QDir>

//----------------------------------------------------------------------
// drumkv1_jack_session_event -- JACK session event callabck
//

static void drumkv1_jack_session_event (
	jack_session_event_t *pSessionEvent, void *pvArg )
{
	drumkv1_jack *pDrumk = static_cast<drumkv1_jack *> (pvArg);

	if (pDrumk)
		pDrumk->sessionEvent(pSessionEvent);
}

#endif	// CONFIG_JACK_SESSION


//-------------------------------------------------------------------------
// drumkv1_jack - impl.
//

drumkv1_jack::drumkv1_jack (void) : drumkv1(2)
{
	m_client = NULL;

	m_activated = false;

	m_audio_ins = NULL;
	m_audio_outs = NULL;

	m_ins = m_outs = NULL;

	::memset(m_params, 0, drumkv1::NUM_PARAMS * sizeof(float));

#ifdef CONFIG_JACK_MIDI
	m_midi_in = NULL;
#endif
#ifdef CONFIG_ALSA_MIDI
	m_alsa_seq     = NULL;
//	m_alsa_client  = -1;
	m_alsa_port    = -1;
	m_alsa_decoder = NULL;
	m_alsa_buffer  = NULL;
	m_alsa_thread  = NULL;
#endif

	drumkv1::programs()->enabled(true);
	drumkv1::controls()->enabled(true);

	open(DRUMKV1_TITLE);
	activate();
}


drumkv1_jack::~drumkv1_jack (void)
{
	deactivate();
	close();
}


jack_client_t *drumkv1_jack::client (void) const
{
	return m_client;
}


int drumkv1_jack::process ( jack_nframes_t nframes )
{
	if (!m_activated)
		return 0;

	const uint16_t nchannels = drumkv1::channels();
	float *ins[nchannels], *outs[nchannels];
	for (uint16_t k = 0; k < nchannels; ++k) {
		ins[k]  = static_cast<float *> (
			::jack_port_get_buffer(m_audio_ins[k], nframes));
		outs[k] = static_cast<float *> (
			::jack_port_get_buffer(m_audio_outs[k], nframes));
	}

	jack_position_t pos;
	jack_transport_query(m_client, &pos);
	if (pos.valid & JackPositionBBT) {
		const float host_bpm = float(pos.beats_per_minute);
		if (paramValue(drumkv1::LFO1_BPMSYNC) > 0.0f) {
		#ifdef CONFIG_LFO_BPMRATEX_0
			const float rate_bpm = lfo_rate_bpm(host_bpm);
			const float rate = paramValue(drumkv1::LFO1_RATE);
			if (::fabsf(rate_bpm - rate) > 0.01f)
				setParamValue(drumkv1::LFO1_RATE, rate_bpm);
		#else
			const float bpm = paramValue(drumkv1::LFO1_BPM);
			if (::fabsf(host_bpm - bpm) > 0.01f)
				setParamValue(drumkv1::LFO1_BPM, host_bpm);
		#endif
		}
		if (paramValue(drumkv1::DEL1_BPMSYNC) > 0.0f) {
			const float bpm = paramValue(drumkv1::DEL1_BPM);
			if (bpm > 0.0f && ::fabsf(host_bpm - bpm) > 0.01f)
				setParamValue(drumkv1::DEL1_BPM, host_bpm);
		}
	}

	uint32_t ndelta = 0;

#ifdef CONFIG_JACK_MIDI
	void *midi_in = ::jack_port_get_buffer(m_midi_in, nframes);
	if (midi_in) {
		const uint32_t nevents = ::jack_midi_get_event_count(midi_in);
		for (uint32_t n = 0; n < nevents; ++n) {
			jack_midi_event_t event;
			::jack_midi_event_get(&event, midi_in, n);
			if (event.time > ndelta) {
				const uint32_t nread = event.time - ndelta;
				if (nread > 0) {
					drumkv1::process(ins, outs, nread);
					for (uint16_t k = 0; k < nchannels; ++k) {
						ins[k]  += nread;
						outs[k] += nread;
					}
				}
			}
			ndelta = event.time;
			drumkv1::process_midi(event.buffer, event.size);
		}
	}
#endif
#ifdef CONFIG_ALSA_MIDI
	const jack_nframes_t buffer_size = ::jack_get_buffer_size(m_client);
	const jack_nframes_t frame_time  = ::jack_last_frame_time(m_client);
	uint8_t event_buffer[1024];
	jack_midi_event_t event;
	while (::jack_ringbuffer_peek(m_alsa_buffer,
			(char *) &event, sizeof(event)) == sizeof(event)) {
		if (event.time > frame_time)
			break;
		jack_nframes_t event_time = frame_time - event.time;
		if (event_time > buffer_size)
			event_time = 0;
		else
			event_time = buffer_size - event_time;
		if (event_time > ndelta) {
			const uint32_t nread = event_time - ndelta;
			if (nread > 0) {
				drumkv1::process(ins, outs, nread);
				for (uint16_t k = 0; k < nchannels; ++k) {
					ins[k]  += nread;
					outs[k] += nread;
				}
			}
			ndelta = event_time;
		}
		::jack_ringbuffer_read_advance(m_alsa_buffer, sizeof(event));
		::jack_ringbuffer_read(m_alsa_buffer, (char *) event_buffer, event.size);
		drumkv1::process_midi(event_buffer, event.size);
	}
#endif // CONFIG_ALSA_MIDI

	if (nframes > ndelta)
		drumkv1::process(ins, outs, nframes - ndelta);

	return 0;
}


void drumkv1_jack::open ( const char *client_id )
{
	// init param ports
	for (uint32_t i = 0; i < drumkv1::NUM_PARAMS; ++i) {
		const drumkv1::ParamIndex index = drumkv1::ParamIndex(i);
		m_params[i] = drumkv1_param::paramDefaultValue(index);
		drumkv1::setParamPort(index, &m_params[i]);
	}

	// open client
	m_client = ::jack_client_open(client_id, JackNullOption, NULL);
	if (m_client == NULL)
		return;

	// set sample rate
	drumkv1::setSampleRate(float(jack_get_sample_rate(m_client)));
//	drumkv1::reset();

	// register audio ports & buffers
	const uint16_t nchannels = drumkv1::channels();

	m_audio_ins  = new jack_port_t * [nchannels];
	m_audio_outs = new jack_port_t * [nchannels];

	m_ins  = new float * [nchannels];
	m_outs = new float * [nchannels];

	char port_name[32];
	for (uint16_t k = 0; k < nchannels; ++k) {
		::snprintf(port_name, sizeof(port_name), "in_%d", k + 1);
		m_audio_ins[k] = ::jack_port_register(m_client,
			port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
		m_ins[k] = NULL;
		::snprintf(port_name, sizeof(port_name), "out_%d", k + 1);
		m_audio_outs[k] = ::jack_port_register(m_client,
			port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		m_outs[k] = NULL;
	}

	// register midi port
#ifdef CONFIG_JACK_MIDI
	m_midi_in = ::jack_port_register(m_client,
		"in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
#endif
#ifdef CONFIG_ALSA_MIDI
	m_alsa_seq     = NULL;
//	m_alsa_client  = -1;
	m_alsa_port    = -1;
	m_alsa_decoder = NULL;
	m_alsa_buffer  = NULL;
	m_alsa_thread  = NULL;
	// open alsa sequencer client...
	if (snd_seq_open(&m_alsa_seq, "hw", SND_SEQ_OPEN_INPUT, 0) >= 0) {
		snd_seq_set_client_name(m_alsa_seq, client_id);
	//	m_alsa_client = snd_seq_client_id(m_alsa_seq);
		m_alsa_port = snd_seq_create_simple_port(m_alsa_seq, "in",
			SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
		snd_midi_event_new(1024, &m_alsa_decoder);
		m_alsa_buffer = ::jack_ringbuffer_create(
			1024 * (sizeof(jack_midi_event_t) + 4));
		m_alsa_thread = new drumkv1_alsa_thread(this);
		m_alsa_thread->start(QThread::TimeCriticalPriority);
	}
#endif	// CONFIG_ALSA_MIDI

	// setup any local, initial buffers...
	drumkv1::setBufferSize(::jack_get_buffer_size(m_client));

	jack_set_buffer_size_callback(m_client,
		drumkv1_jack_buffer_size, this);

	// set process callbacks...
	::jack_set_process_callback(m_client,
		drumkv1_jack_process, this);

#ifdef CONFIG_JACK_SESSION
	// JACK session event callback...
	if (::jack_set_session_callback) {
		::jack_set_session_callback(m_client,
			drumkv1_jack_session_event, this);
	}
#endif
}


void drumkv1_jack::activate (void)
{
	if (!m_activated) {
		drumkv1::reset();
		if (m_client) {
			::jack_activate(m_client);
			m_activated = true;
		}
	}
}

void drumkv1_jack::deactivate (void)
{
	if (m_activated) {
		if (m_client) {
			m_activated = false;
			::jack_deactivate(m_client);
		}
	}
}


void drumkv1_jack::close (void)
{
#ifdef CONFIG_ALSA_MIDI
	// close alsa sequencer client...
	if (m_alsa_seq) {
		if (m_alsa_thread) {
			delete m_alsa_thread;
			m_alsa_thread = NULL;
		}
		if (m_alsa_buffer) {
			::jack_ringbuffer_free(m_alsa_buffer);
			m_alsa_buffer = NULL;
		}
		if (m_alsa_decoder) {
			snd_midi_event_free(m_alsa_decoder);
			m_alsa_decoder = NULL;
		}
		if (m_alsa_port >= 0) {
			snd_seq_delete_simple_port(m_alsa_seq, m_alsa_port);
			m_alsa_port = -1;
		}
		snd_seq_close(m_alsa_seq);
	//	m_alsa_client = -1;
		m_alsa_seq = NULL;
	}
#endif

	if (m_client == NULL)
		return;

#ifdef CONFIG_JACK_MIDI
	// unregister midi port
	if (m_midi_in) {
		::jack_port_unregister(m_client, m_midi_in);
		m_midi_in = NULL;
	}
#endif

	// unregister audio ports
	const uint16_t nchannels = drumkv1::channels();

	for (uint16_t k = 0; k < nchannels; ++k) {
		if (m_audio_outs && m_audio_outs[k]) {
			::jack_port_unregister(m_client, m_audio_outs[k]);
			m_audio_outs[k] = NULL;
		}
		if (m_outs && m_outs[k])
			m_outs[k] = NULL;
		if (m_audio_ins && m_audio_ins[k]) {
			::jack_port_unregister(m_client, m_audio_ins[k]);
			m_audio_ins[k] = NULL;
		}
		if (m_ins && m_ins[k])
			m_ins[k] = NULL;
	}

	if (m_outs) {
		delete [] m_outs;
		m_outs = NULL;
	}
	if (m_ins) {
		delete [] m_ins;
		m_ins = NULL;
	}

	if (m_audio_outs) {
		delete [] m_audio_outs;
		m_audio_outs = NULL;
	}
	if (m_audio_ins) {
		delete [] m_audio_ins;
		m_audio_ins = NULL;
	}

	// close client
	::jack_client_close(m_client);
	m_client = NULL;
}


void drumkv1_jack::setParamValue ( drumkv1::ParamIndex index, float fValue )
{
	m_params[index] = fValue;
}

float drumkv1_jack::paramValue ( drumkv1::ParamIndex index ) const
{
	return m_params[index];
}


#ifdef CONFIG_ALSA_MIDI

// alsa sequencer client.
snd_seq_t *drumkv1_jack::alsa_seq (void) const
{
	return m_alsa_seq;
}

// alsa event capture.
void drumkv1_jack::alsa_capture ( snd_seq_event_t *ev )
{
	if (m_alsa_decoder == NULL)
		return;

	if (ev == NULL)
		return;

	// ignored events...
	switch(ev->type) {
	case SND_SEQ_EVENT_OSS:
	case SND_SEQ_EVENT_CLIENT_START:
	case SND_SEQ_EVENT_CLIENT_EXIT:
	case SND_SEQ_EVENT_CLIENT_CHANGE:
	case SND_SEQ_EVENT_PORT_START:
	case SND_SEQ_EVENT_PORT_EXIT:
	case SND_SEQ_EVENT_PORT_CHANGE:
	case SND_SEQ_EVENT_PORT_SUBSCRIBED:
	case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
	case SND_SEQ_EVENT_USR0:
	case SND_SEQ_EVENT_USR1:
	case SND_SEQ_EVENT_USR2:
	case SND_SEQ_EVENT_USR3:
	case SND_SEQ_EVENT_USR4:
	case SND_SEQ_EVENT_USR5:
	case SND_SEQ_EVENT_USR6:
	case SND_SEQ_EVENT_USR7:
	case SND_SEQ_EVENT_USR8:
	case SND_SEQ_EVENT_USR9:
	case SND_SEQ_EVENT_BOUNCE:
	case SND_SEQ_EVENT_USR_VAR0:
	case SND_SEQ_EVENT_USR_VAR1:
	case SND_SEQ_EVENT_USR_VAR2:
	case SND_SEQ_EVENT_USR_VAR3:
	case SND_SEQ_EVENT_USR_VAR4:
	case SND_SEQ_EVENT_NONE:
		return;
	}

#ifdef CONFIG_DEBUG_0
	// - show (input) event for debug purposes...
	fprintf(stderr, "ALSA MIDI In: 0x%02x", ev->type);
	if (ev->type == SND_SEQ_EVENT_SYSEX) {
		fprintf(stderr, " SysEx {");
		unsigned char *data = (unsigned char *) ev->data.ext.ptr;
		for (unsigned int i = 0; i < ev->data.ext.len; ++i)
			fprintf(stderr, " %02x", data[i]);
		fprintf(stderr, " }\n");
	} else {
		for (unsigned int i = 0; i < sizeof(ev->data.raw8.d); ++i)
			fprintf(stderr, " %3d", ev->data.raw8.d[i]);
		fprintf(stderr, "\n");
	}
#endif

	const unsigned int nlimit = ::jack_ringbuffer_write_space(m_alsa_buffer);
	if (nlimit > sizeof(jack_midi_event_t) + 4) {
		unsigned char  ev_buff[nlimit];
		unsigned char *ev_data = &ev_buff[0] + sizeof(jack_midi_event_t);
		const int ev_size = snd_midi_event_decode(m_alsa_decoder,
			ev_data, nlimit - sizeof(jack_midi_event_t), ev);
		if (ev_size > 0) {
			jack_midi_event_t *ev_head = (jack_midi_event_t *) &ev_buff[0];
			ev_head->time = ::jack_frame_time(m_client);
			ev_head->size = ev_size;
			ev_head->buffer = (jack_midi_data_t *) ev_data;
			::jack_ringbuffer_write(m_alsa_buffer,
				(const char *) ev_buff, sizeof(jack_midi_event_t) + ev_size);
		}
		snd_midi_event_reset_decode(m_alsa_decoder);
	}
}

#endif	// CONFIG_ALSA_MIDI


#ifdef CONFIG_JACK_SESSION

// JACK session event handler.
void drumkv1_jack::sessionEvent ( void *pvSessionArg )
{
	jack_session_event_t *pJackSessionEvent
		= (jack_session_event_t *) pvSessionArg;

#ifdef CONFIG_DEBUG
	qDebug("drumkv1_jack::sessionEvent()"
		" type=%d client_uuid=\"%s\" session_dir=\"%s\"",
		int(pJackSessionEvent->type),
		pJackSessionEvent->client_uuid,
		pJackSessionEvent->session_dir);
#endif

	const bool bQuit = (pJackSessionEvent->type == JackSessionSaveAndQuit);

	const QString sSessionDir
		= QString::fromUtf8(pJackSessionEvent->session_dir);
	const QString sSessionName
		= QFileInfo(QFileInfo(sSessionDir).canonicalPath()).completeBaseName();
	const QString sSessionFile = sSessionName + '.' + DRUMKV1_TITLE;

	QStringList args;
	args << QCoreApplication::applicationFilePath();
	args << QString("\"${SESSION_DIR}%1\"").arg(sSessionFile);

	drumkv1_param::savePreset(this,
		QFileInfo(sSessionDir, sSessionFile).absoluteFilePath());

	const QByteArray aCmdLine = args.join(" ").toUtf8();
	pJackSessionEvent->command_line = ::strdup(aCmdLine.constData());

	jack_session_reply(m_client, pJackSessionEvent);
	jack_session_event_free(pJackSessionEvent);

	if (bQuit)
		QCoreApplication::quit();
}

#endif	// CONFIG_JACK_SESSION


//-------------------------------------------------------------------------
// drumkv1_jack_application -- Singleton application instance.
//

#include "drumkv1widget_jack.h"

#include <QApplication>
#include <QTextStream>

#ifdef CONFIG_NSM
#include "drumkv1_nsm.h"
#endif


// Constructor.
drumkv1_jack_application::drumkv1_jack_application ( int& argc, char **argv )
	: QObject(NULL), m_pApp(NULL), m_bGui(true),
		m_pDrumk(NULL), m_pWidget(NULL)
	  #ifdef CONFIG_NSM
		, m_pNsmClient(NULL)
	  #endif
{
#ifdef Q_WS_X11
	m_bGui = (::getenv("DISPLAY") != 0);
#endif
	for (int i = 1; i < argc; ++i) {
		const QString& sArg = QString::fromLocal8Bit(argv[i]);
		if (sArg[0] != '-')
			m_presets.append(sArg);
		else
		if (sArg == "-g" || sArg == "--no-gui")
			m_bGui = false;
	}

	if (m_bGui)
		m_pApp = new QApplication(argc, argv);
	else
		m_pApp = new QCoreApplication(argc, argv);
}


// Destructor.
drumkv1_jack_application::~drumkv1_jack_application (void)
{
#ifdef CONFIG_NSM
	if (m_pNsmClient) delete m_pNsmClient;
#endif
	if (m_pWidget) delete m_pWidget;
	if (m_pDrumk) delete m_pDrumk;
	if (m_pApp) delete m_pApp;
}


// Argument parser method.
bool drumkv1_jack_application::parse_args (void)
{
	QTextStream out(stderr);

	const QStringList& args = m_pApp->arguments();
	QStringListIterator iter(args);
	while (iter.hasNext()) {
		const QString& sArg = iter.next();
		if (sArg == "-h" || sArg == "--help") {
			out << QObject::tr(
				"Usage: %1 [options] [preset-file]\n\n"
				DRUMKV1_TITLE " - " DRUMKV1_SUBTITLE "\n\n"
				"Options:\n\n"
				"  -g, --no-gui\n\tDisable the graphical user interface (GUI)\n\n"
				"  -h, --help\n\tShow help about command line options\n\n"
				"  -v, --version\n\tShow version information\n\n")
				.arg(args.at(0));
			return false;
		}
		else
		if (sArg == "-v" || sArg == "-V" || sArg == "--version") {
			out << QObject::tr("Qt: %1\n").arg(qVersion());
			out << QObject::tr(DRUMKV1_TITLE ": %1\n").arg(DRUMKV1_VERSION);
			return false;
		}
	}

	return true;
}


// Startup methods.
bool drumkv1_jack_application::setup (void)
{
	if (m_pApp == NULL)
		return false;

	if (!parse_args()) {
		m_pApp->quit();
		return false;
	}

	m_pDrumk = new drumkv1_jack();

	if (m_bGui) {
		m_pWidget = new drumkv1widget_jack(m_pDrumk);
		if (m_presets.isEmpty())
			m_pWidget->initPreset();
		else
			m_pWidget->loadPreset(m_presets.first());
		m_pWidget->show();
	}
	else
	if (!m_presets.isEmpty())
		drumkv1_param::loadPreset(m_pDrumk, m_presets.first());

#ifdef CONFIG_NSM
	// Check whether to participate into a NSM session...
	const QString& nsm_url
		= QString::fromLatin1(::getenv("NSM_URL"));
	if (!nsm_url.isEmpty()) {
		m_pNsmClient = new drumkv1_nsm(nsm_url);
		QObject::connect(m_pNsmClient,
			SIGNAL(open()),
			SLOT(openSession()));
		QObject::connect(m_pNsmClient,
			SIGNAL(save()),
			SLOT(saveSession()));
		QObject::connect(m_pNsmClient,
			SIGNAL(show()),
			SLOT(showSession()));
		QObject::connect(m_pNsmClient,
			SIGNAL(hide()),
			SLOT(hideSession()));
		QString caps(":switch:dirty:");
		if (m_bGui)
			caps += "optional-gui:";
		m_pNsmClient->announce(DRUMKV1_TITLE, caps.toLatin1().constData());
		if (m_pWidget)
			m_pWidget->setNsmClient(m_pNsmClient);
	}
#endif	// CONFIG_NSM

	return true;
}


// Facade method.
int drumkv1_jack_application::exec (void)
{
	return (setup() ? m_pApp->exec() : 1);
}


#ifdef CONFIG_NSM

void drumkv1_jack_application::openSession (void)
{
	if (m_pDrumk == NULL)
		return;

	if (m_pNsmClient == NULL)
		return;

	if (!m_pNsmClient->is_active())
		return;

#ifdef CONFIG_DEBUG
	qDebug("drumkv1_jack::openSession()");
#endif

	m_pDrumk->deactivate();
	m_pDrumk->close();

	const QString& client_id = m_pNsmClient->client_id();
	const QString& path_name = m_pNsmClient->path_name();
	const QString& display_name = m_pNsmClient->display_name();

	m_pDrumk->open(client_id.toUtf8().constData());
	m_pDrumk->activate();

	const QDir dir(path_name);
	if (!dir.exists())
		dir.mkpath(path_name);

	const QFileInfo fi(path_name, display_name + '.' + DRUMKV1_TITLE);
	if (fi.exists()) {
		const QString& sFilename = fi.absoluteFilePath();
		if (m_pWidget) {
			m_pWidget->loadPreset(sFilename);
		} else {
			drumkv1_param::loadPreset(m_pDrumk, sFilename);
		}
	}

	m_pNsmClient->open_reply();
	m_pNsmClient->dirty(false);

	if (m_pWidget)
		m_pNsmClient->visible(m_pWidget->isVisible());
}

void drumkv1_jack_application::saveSession (void)
{
	if (m_pDrumk == NULL)
		return;

	if (m_pNsmClient == NULL)
		return;

	if (!m_pNsmClient->is_active())
		return;

#ifdef CONFIG_DEBUG
	qDebug("drumkv1_jack::saveSession()");
#endif

	const QString& path_name = m_pNsmClient->path_name();
	const QString& display_name = m_pNsmClient->display_name();
//	const QString& client_id = m_pNsmClient->client_id();
	const QFileInfo fi(path_name, display_name + '.' + DRUMKV1_TITLE);

	drumkv1_param::savePreset(m_pDrumk, fi.absoluteFilePath());

	m_pNsmClient->save_reply();
	m_pNsmClient->dirty(false);
}


void drumkv1_jack_application::showSession (void)
{
	if (m_pNsmClient == NULL)
		return;

	if (!m_pNsmClient->is_active())
		return;

#ifdef CONFIG_DEBUG
	qDebug("drumkv1_jack::showSession()");
#endif

	if (m_pWidget) {
		m_pWidget->show();
		m_pWidget->raise();
		m_pWidget->activateWindow();
	}
}

void drumkv1_jack_application::hideSession (void)
{
	if (m_pNsmClient == NULL)
		return;

	if (!m_pNsmClient->is_active())
		return;

#ifdef CONFIG_DEBUG
	qDebug("drumkv1_jack::hideSession()");
#endif

	if (m_pWidget)
		m_pWidget->hide();
}


#endif	// CONFIG_NSM


//-------------------------------------------------------------------------
// main

int main ( int argc, char *argv[] )
{
	Q_INIT_RESOURCE(drumkv1);

	drumkv1_jack_application app(argc, argv);

	return app.exec();
}


// end of drumkv1_jack.cpp
