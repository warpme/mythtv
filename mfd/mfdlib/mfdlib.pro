include (../settings.pro)

!exists(../config.pro ) {
    error(Missing config.pro: please run the configure script)
}

TEMPLATE = lib
CONFIG += thread dll
TARGET = mfdlib

target.path = $${PREFIX}/lib
INSTALLS += target

HEADERS += mfd_events.h   mfd_plugin.h   requestthread.h   clientsocket.h  
SOURCES += mfd_events.cpp mfd_plugin.cpp requestthread.cpp clientsocket.cpp

HEADERS += httprequest.h   httpresponse.h   httpheader.h   httpgetvar.h
SOURCES += httprequest.cpp httpresponse.cpp httpheader.cpp httpgetvar.cpp

HEADERS += decoder.h   decoder_event.h   visual.h constants.h 
SOURCES += decoder.cpp decoder_event.cpp 

HEADERS += output.h   output_event.h   recycler.h   buffer.h
SOURCES += output.cpp output_event.cpp recycler.cpp buffer.cpp

HEADERS += vorbisdecoder.h   maddecoder.h
SOURCES += vorbisdecoder.cpp maddecoder.cpp

HEADERS += flacdecoder.h   cddecoder.h
SOURCES += flacdecoder.cpp cddecoder.cpp


LIBS += -logg -lvorbisfile -lvorbis  -lmad -lid3tag -lcdaudio \
        -lFLAC -lcdda_interface -lcdda_paranoia
