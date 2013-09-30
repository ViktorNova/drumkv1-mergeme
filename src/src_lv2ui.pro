# drumkv1_lv2ui.pro
#
NAME = drumkv1

TARGET = $${NAME}_ui
TEMPLATE = lib
CONFIG += shared plugin

include(src_lv2.pri)

HEADERS = \
	config.h \
	drumkv1_config.h \
	drumkv1_param.h \
	drumkv1widget.h \
	drumkv1widget_env.h \
	drumkv1widget_filt.h \
	drumkv1widget_sample.h \
	drumkv1widget_wave.h \
	drumkv1widget_knob.h \
	drumkv1widget_preset.h \
	drumkv1widget_status.h \
	drumkv1widget_config.h \
	drumkv1widget_elements.h \
	drumkv1widget_lv2.h

SOURCES = \
	drumkv1_param.cpp \
	drumkv1widget.cpp \
	drumkv1widget_env.cpp \
	drumkv1widget_filt.cpp \
	drumkv1widget_sample.cpp \
	drumkv1widget_wave.cpp \
	drumkv1widget_knob.cpp \
	drumkv1widget_preset.cpp \
	drumkv1widget_status.cpp \
	drumkv1widget_config.cpp \
	drumkv1widget_elements.cpp \
	drumkv1widget_lv2.cpp

FORMS = \
	drumkv1widget.ui

RESOURCES += drumkv1.qrc


unix {

	OBJECTS_DIR = .obj_lv2ui
	MOC_DIR     = .moc_lv2ui
	UI_DIR      = .ui_lv2ui

	isEmpty(PREFIX) {
		PREFIX = /usr/local
	}

	contains(PREFIX, $$system(echo $HOME)) {
		LV2DIR = $${PREFIX}/.lv2
	} else {
		ARCH = $$system(uname -m)
		contains(ARCH, x86_64) {
			LV2DIR = $${PREFIX}/lib64/lv2
		} else {
			LV2DIR = $${PREFIX}/lib/lv2
		}
	}

	TARGET_LV2UI = $${NAME}.lv2/$${TARGET}.so

	!exists($${TARGET_LV2UI}) {
		system(touch $${TARGET_LV2UI})
	}

	QMAKE_POST_LINK += $${QMAKE_COPY} -vp $(TARGET) $${TARGET_LV2UI}

	INSTALLS += target

	target.path  = $${LV2DIR}/$${NAME}.lv2
	target.files = $${TARGET_LV2UI} \
		$${NAME}.lv2/$${TARGET}.ttl

	QMAKE_CLEAN += $${TARGET_LV2UI}

	LIBS += -L$${NAME}.lv2 -l$${NAME} -Wl,-rpath,$${LV2DIR}/$${NAME}.lv2
}

QT += xml

# QT5 support
greaterThan(QT_MAJOR_VERSION, 4) {
	QT += widgets
}