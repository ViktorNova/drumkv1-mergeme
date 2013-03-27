// drumkv1_lv2.cpp
//
/****************************************************************************
   Copyright (C) 2012-2013, rncbc aka Rui Nuno Capela. All rights reserved.

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

#include "drumkv1_lv2.h"

#include "drumkv1widget_lv2.h"

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/midi/midi.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"

#include "lv2/lv2plug.in/ns/ext/instance-access/instance-access.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"

#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <QDomDocument>

//-------------------------------------------------------------------------
// drumkv1_lv2_map_path - abstract/absolute path functors.
//

class drumkv1_lv2_map_path : public drumkv1_map_path
{
public:

	drumkv1_lv2_map_path(const LV2_Feature *const *features)
		: m_map_path(NULL)
	{
		for (int i = 0; features && features[i]; ++i) {
			if (::strcmp(features[i]->URI, LV2_STATE__mapPath) == 0) {
				m_map_path = (LV2_State_Map_Path *) features[i]->data;
				break;
			}
		}
	}

	QString absolutePath(const QString& sAbstractPath) const
	{
		QString sAbsolutePath(sAbstractPath);
		if (m_map_path) {
			const char *pszAbsolutePath
				= m_map_path->absolute_path(
					m_map_path->handle, sAbstractPath.toUtf8().constData());
			if (pszAbsolutePath) {
				sAbsolutePath = pszAbsolutePath;
				::free((void *) pszAbsolutePath);
			}
		}
		return sAbsolutePath;
	}
	
	QString abstractPath(const QString& sAbsolutePath) const
	{
		QString sAbstractPath(sAbsolutePath);
		if (m_map_path) {
			const char *pszAbstractPath
				= m_map_path->abstract_path(
					m_map_path->handle, sAbsolutePath.toUtf8().constData());
			if (pszAbstractPath) {
				sAbstractPath = pszAbstractPath;
				::free((void *) pszAbstractPath);
			}
		}
		return sAbstractPath;
	}

private:

	LV2_State_Map_Path *m_map_path;		
};

#ifdef CONFIG_LV2_EXTERNAL_UI
#include <QApplication>
#endif


//-------------------------------------------------------------------------
// drumkv1_lv2 - impl.
//

drumkv1_lv2::drumkv1_lv2 (
	double sample_rate, const LV2_Feature *const *host_features )
	: drumkv1(2, uint32_t(sample_rate))
{
	m_urid_map = NULL;
	m_midi_event_type = 0;
	m_atom_sequence = NULL;

	for (int i = 0; host_features && host_features[i]; ++i) {
		if (::strcmp(host_features[i]->URI, LV2_URID_MAP_URI) == 0) {
			m_urid_map = (LV2_URID_Map *) host_features[i]->data;
			if (m_urid_map) {
				m_midi_event_type = m_urid_map->map(
					m_urid_map->handle, LV2_MIDI__MidiEvent);
				break;
			}
		}
	}

	const uint16_t nchannels = channels();
	m_ins  = new float * [nchannels];
	m_outs = new float * [nchannels];
	for (uint16_t k = 0; k < nchannels; ++k)
		m_ins[k] = m_outs[k] = NULL;

	::socketpair(AF_UNIX, SOCK_STREAM, 0, m_update_fds);
	m_update_count = 0;
}


drumkv1_lv2::~drumkv1_lv2 (void)
{
	delete [] m_outs;
	delete [] m_ins;
}


void drumkv1_lv2::connect_port ( uint32_t port, void *data )
{
	switch(PortIndex(port)) {
	case MidiIn:
		m_atom_sequence = (LV2_Atom_Sequence *) data;
		break;
	case AudioInL:
		m_ins[0] = (float *) data;
		break;
	case AudioInR:
		m_ins[1] = (float *) data;
		break;
	case AudioOutL:
		m_outs[0] = (float *) data;
		break;
	case AudioOutR:
		m_outs[1] = (float *) data;
		break;
	default:
		setParamPort(ParamIndex(port - ParamBase), (float *) data);
		break;
	}
}


void drumkv1_lv2::run ( uint32_t nframes )
{
	const uint16_t nchannels = channels();
	float *ins[nchannels], *outs[nchannels];
	for (uint16_t k = 0; k < nchannels; ++k) {
		ins[k]  = m_ins[k];
		outs[k] = m_outs[k];
	}

	uint32_t ndelta = 0;

	if (m_atom_sequence) {
		LV2_ATOM_SEQUENCE_FOREACH(m_atom_sequence, event) {
			if (event && event->body.type == m_midi_event_type) {
				uint8_t *data = (uint8_t *) LV2_ATOM_BODY(&event->body);
				uint32_t nread = event->time.frames - ndelta;
				if (nread > 0) {
					process(ins, outs, nread);
					for (uint16_t k = 0; k < nchannels; ++k) {
						ins[k]  += nread;
						outs[k] += nread;
					}
				}
				ndelta = event->time.frames;
				process_midi(data, event->body.size);
			}
		}
	//	m_atom_sequence = NULL;
	}

	process(ins, outs, nframes - ndelta);
}


void drumkv1_lv2::activate (void)
{
	reset();
}


void drumkv1_lv2::deactivate (void)
{
	reset();
}


uint32_t drumkv1_lv2::urid_map ( const char *uri ) const
{
	return (m_urid_map ? m_urid_map->map(m_urid_map->handle, uri) : 0);
}


int drumkv1_lv2::update_fds ( int mode )
{
	return m_update_fds[mode];
}


void drumkv1_lv2::update_notify (void)
{
	if (m_update_count < 1) {
		char c = 1;
		if (::write(m_update_fds[0], &c, sizeof(c)) > 0)
			++m_update_count;
	}
}


void drumkv1_lv2::update_reset (void)
{
	if (m_update_count > 0) {
		char c;
		if (::read(m_update_fds[1], &c, sizeof(c)) > 0)
			m_update_count = 0;
	}
}


static LV2_State_Status drumkv1_lv2_state_save ( LV2_Handle instance,
	LV2_State_Store_Function store, LV2_State_Handle handle,
	uint32_t flags, const LV2_Feature *const *features )
{
	drumkv1_lv2 *pPlugin = static_cast<drumkv1_lv2 *> (instance);
	if (pPlugin == NULL)
		return LV2_STATE_ERR_UNKNOWN;

	uint32_t key = pPlugin->urid_map(DRUMKV1_LV2_PREFIX "state");
	if (key == 0)
		return LV2_STATE_ERR_NO_PROPERTY;

	uint32_t type = pPlugin->urid_map(LV2_ATOM__Chunk);
	if (type == 0)
		return LV2_STATE_ERR_BAD_TYPE;
#if 0
	if ((flags & (LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE)) == 0)
		return LV2_STATE_ERR_BAD_FLAGS;
#else
	flags |= (LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
#endif
	drumkv1_lv2_map_path mapPath(features);

	QDomDocument doc(DRUMKV1_TITLE);
	QDomElement eElements = doc.createElement("elements");
	drumkv1widget::saveElements(pPlugin, doc, eElements, mapPath);
	doc.appendChild(eElements);

	const QByteArray data(doc.toByteArray());
	const char *value = data.constData();
	size_t size = data.size();

	return (*store)(handle, key, value, size, type, flags);
}


static LV2_State_Status drumkv1_lv2_state_restore ( LV2_Handle instance,
	LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
	uint32_t flags, const LV2_Feature *const *features )
{
	drumkv1_lv2 *pPlugin = static_cast<drumkv1_lv2 *> (instance);
	if (pPlugin == NULL)
		return LV2_STATE_ERR_UNKNOWN;

	uint32_t key = pPlugin->urid_map(DRUMKV1_LV2_PREFIX "state");
	if (key == 0)
		return LV2_STATE_ERR_NO_PROPERTY;

	uint32_t chunk_type = pPlugin->urid_map(LV2_ATOM__Chunk);
	if (chunk_type == 0)
		return LV2_STATE_ERR_BAD_TYPE;

	size_t size = 0;
	uint32_t type = 0;
//	flags = 0;

	const char *value
		= (const char *) (*retrieve)(handle, key, &size, &type, &flags);

	if (size < 2)
		return LV2_STATE_ERR_UNKNOWN;

	if (type != chunk_type)
		return LV2_STATE_ERR_BAD_TYPE;

	if ((flags & (LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE)) == 0)
		return LV2_STATE_ERR_BAD_FLAGS;

	if (value == NULL)
		return LV2_STATE_ERR_UNKNOWN;

	drumkv1_lv2_map_path mapPath(features);

	QDomDocument doc(DRUMKV1_TITLE);
	if (doc.setContent(QByteArray(value, size))) {
		QDomElement eElements = doc.documentElement();
		if (eElements.tagName() == "elements")
			drumkv1widget::loadElements(pPlugin, eElements, mapPath);
	}

	return LV2_STATE_SUCCESS;
}


static const LV2_State_Interface drumkv1_lv2_state_interface =
{
	drumkv1_lv2_state_save,
	drumkv1_lv2_state_restore
};


static LV2_Handle drumkv1_lv2_instantiate (
	const LV2_Descriptor *, double sample_rate, const char *,
	const LV2_Feature *const *host_features )
{
	return new drumkv1_lv2(sample_rate, host_features);
}


static void drumkv1_lv2_connect_port (
	LV2_Handle instance, uint32_t port, void *data )
{
	drumkv1_lv2 *pPlugin = static_cast<drumkv1_lv2 *> (instance);
	if (pPlugin)
		pPlugin->connect_port(port, data);
}


static void drumkv1_lv2_run ( LV2_Handle instance, uint32_t nframes )
{
	drumkv1_lv2 *pPlugin = static_cast<drumkv1_lv2 *> (instance);
	if (pPlugin)
		pPlugin->run(nframes);
}


static void drumkv1_lv2_activate ( LV2_Handle instance )
{
	drumkv1_lv2 *pPlugin = static_cast<drumkv1_lv2 *> (instance);
	if (pPlugin)
		pPlugin->activate();
}


static void drumkv1_lv2_deactivate ( LV2_Handle instance )
{
	drumkv1_lv2 *pPlugin = static_cast<drumkv1_lv2 *> (instance);
	if (pPlugin)
		pPlugin->deactivate();
}


static void drumkv1_lv2_cleanup ( LV2_Handle instance )
{
	drumkv1_lv2 *pPlugin = static_cast<drumkv1_lv2 *> (instance);
	if (pPlugin)
		delete pPlugin;
}


static const void *drumkv1_lv2_extension_data ( const char *uri )
{
	if (::strcmp(uri, LV2_STATE__interface))
		return NULL;

	return &drumkv1_lv2_state_interface;
}


static LV2UI_Handle drumkv1_lv2ui_instantiate (
	const LV2UI_Descriptor *, const char *, const char *,
	LV2UI_Write_Function write_function,
	LV2UI_Controller controller, LV2UI_Widget *widget,
	const LV2_Feature *const *features )
{
	drumkv1_lv2 *pSampl = NULL;

	for (int i = 0; features && features[i]; ++i) {
		if (::strcmp(features[i]->URI, LV2_INSTANCE_ACCESS_URI) == 0) {
			pSampl = static_cast<drumkv1_lv2 *> (features[i]->data);
			break;
		}
	}

	if (pSampl == NULL)
		return NULL;

	drumkv1widget_lv2 *pWidget
		= new drumkv1widget_lv2(pSampl, controller, write_function);
	*widget = pWidget;

	return pWidget;
}

static void drumkv1_lv2ui_cleanup ( LV2UI_Handle ui )
{
	drumkv1widget_lv2 *pWidget = static_cast<drumkv1widget_lv2 *> (ui);
	if (pWidget)
		delete pWidget;
}

static void drumkv1_lv2ui_port_event (
	LV2UI_Handle ui, uint32_t port_index,
	uint32_t buffer_size, uint32_t format, const void *buffer )
{
	drumkv1widget_lv2 *pWidget = static_cast<drumkv1widget_lv2 *> (ui);
	if (pWidget)
		pWidget->port_event(port_index, buffer_size, format, buffer);
}

static const void *drumkv1_lv2ui_extension_data ( const char * )
{
	return NULL;
}


#ifdef CONFIG_LV2_EXTERNAL_UI

struct drumkv1_lv2ui_external_widget
{
	LV2_External_UI_Widget external;
	static QApplication   *app_instance;
	static unsigned int    app_refcount;
	drumkv1widget_lv2     *widget;
};

QApplication *drumkv1_lv2ui_external_widget::app_instance = NULL;
unsigned int  drumkv1_lv2ui_external_widget::app_refcount = 0;


static void drumkv1_lv2ui_external_run ( LV2_External_UI_Widget *ui_external )
{
	drumkv1_lv2ui_external_widget *pExtWidget
		= (drumkv1_lv2ui_external_widget *) (ui_external);
	if (pExtWidget && pExtWidget->app_instance)
		pExtWidget->app_instance->processEvents();
}

static void drumkv1_lv2ui_external_show ( LV2_External_UI_Widget *ui_external )
{
	drumkv1_lv2ui_external_widget *pExtWidget
		= (drumkv1_lv2ui_external_widget *) (ui_external);
	if (pExtWidget && pExtWidget->widget)
		pExtWidget->widget->show();
}

static void drumkv1_lv2ui_external_hide ( LV2_External_UI_Widget *ui_external )
{
	drumkv1_lv2ui_external_widget *pExtWidget
		= (drumkv1_lv2ui_external_widget *) (ui_external);
	if (pExtWidget && pExtWidget->widget)
		pExtWidget->widget->hide();
}

static LV2UI_Handle drumkv1_lv2ui_external_instantiate (
	const LV2UI_Descriptor *, const char *, const char *,
	LV2UI_Write_Function write_function,
	LV2UI_Controller controller, LV2UI_Widget *widget,
	const LV2_Feature *const *ui_features )
{
	drumkv1_lv2 *pDrumk = NULL;
	LV2_External_UI_Host *external_host = NULL;

	for (int i = 0; ui_features && ui_features[i]; ++i) {
		if (::strcmp(ui_features[i]->URI, LV2_INSTANCE_ACCESS_URI) == 0)
			pDrumk = static_cast<drumkv1_lv2 *> (ui_features[i]->data);
		else
		if (::strcmp(ui_features[i]->URI, LV2_EXTERNAL_UI__Host) == 0 ||
			::strcmp(ui_features[i]->URI, LV2_EXTERNAL_UI_DEPRECATED_URI) == 0)
			external_host = (LV2_External_UI_Host *) ui_features[i]->data;
	}

	if (pDrumk == NULL)
		return NULL;

	drumkv1_lv2ui_external_widget *pExtWidget = new drumkv1_lv2ui_external_widget;
	if (qApp == NULL && pExtWidget->app_instance == NULL) {
		static int s_argc = 1;
		static const char *s_argv[] = { __func__, NULL };
		pExtWidget->app_instance = new QApplication(s_argc, (char **) s_argv);
	}
	pExtWidget->app_refcount++;

	pExtWidget->external.run  = drumkv1_lv2ui_external_run;
	pExtWidget->external.show = drumkv1_lv2ui_external_show;
	pExtWidget->external.hide = drumkv1_lv2ui_external_hide;
	pExtWidget->widget = new drumkv1widget_lv2(pDrumk, controller, write_function);
	if (external_host)
		pExtWidget->widget->setExternalHost(external_host);
	*widget = pExtWidget;
	return pExtWidget;
}

static void drumkv1_lv2ui_external_cleanup ( LV2UI_Handle ui )
{
	drumkv1_lv2ui_external_widget *pExtWidget
		= static_cast<drumkv1_lv2ui_external_widget *> (ui);
	if (pExtWidget) {
		if (pExtWidget->widget)
			delete pExtWidget->widget;
		if (--pExtWidget->app_refcount == 0 && pExtWidget->app_instance) {
			delete pExtWidget->app_instance;
			pExtWidget->app_instance = NULL;
		}
		delete pExtWidget;
	}
}

static void drumkv1_lv2ui_external_port_event (
	LV2UI_Handle ui, uint32_t port_index,
	uint32_t buffer_size, uint32_t format, const void *buffer )
{
	drumkv1_lv2ui_external_widget *pExtWidget
		= static_cast<drumkv1_lv2ui_external_widget *> (ui);
	if (pExtWidget && pExtWidget->widget)
		pExtWidget->widget->port_event(port_index, buffer_size, format, buffer);
}

static const void *drumkv1_lv2ui_external_extension_data ( const char * )
{
	return NULL;
}

#endif	// CONFIG_LV2_EXTERNAL_UI


static const LV2_Descriptor drumkv1_lv2_descriptor =
{
	DRUMKV1_LV2_URI,
	drumkv1_lv2_instantiate,
	drumkv1_lv2_connect_port,
	drumkv1_lv2_activate,
	drumkv1_lv2_run,
	drumkv1_lv2_deactivate,
	drumkv1_lv2_cleanup,
	drumkv1_lv2_extension_data
};

static const LV2UI_Descriptor drumkv1_lv2ui_descriptor =
{
	DRUMKV1_LV2UI_URI,
	drumkv1_lv2ui_instantiate,
	drumkv1_lv2ui_cleanup,
	drumkv1_lv2ui_port_event,
	drumkv1_lv2ui_extension_data
};

#ifdef CONFIG_LV2_EXTERNAL_UI
static const LV2UI_Descriptor drumkv1_lv2ui_external_descriptor =
{
	DRUMKV1_LV2UI_EXTERNAL_URI,
	drumkv1_lv2ui_external_instantiate,
	drumkv1_lv2ui_external_cleanup,
	drumkv1_lv2ui_external_port_event,
	drumkv1_lv2ui_external_extension_data
};
#endif	// CONFIG_LV2_EXTERNAL_UI


LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor ( uint32_t index )
{
	return (index == 0 ? &drumkv1_lv2_descriptor : NULL);
}


LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor ( uint32_t index )
{
	if (index == 0)
		return &drumkv1_lv2ui_descriptor;
	else
#ifdef CONFIG_LV2_EXTERNAL_UI
	if (index == 1)
		return &drumkv1_lv2ui_external_descriptor;
	else
#endif
	return NULL;
}


// end of drumkv1_lv2.cpp
