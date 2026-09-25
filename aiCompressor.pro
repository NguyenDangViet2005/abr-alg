# QT += dbus
QT += core
QT += network
# QT += serialport
# QT += websockets
QT += gui
QT += xml
# QT += positioning
QT += multimedia
QT += concurrent
QT += widgets

DEPENDENCY_PROJECT += log
DEPENDENCY_PROJECT += mainCrypto
DEPENDENCY_PROJECT += QXlsx nmeaParse quazip ubxParse settings
DEPENDENCY_PROJECT += ntp

CONFIG += c++17
CONFIG += console

# XBFIRM: true = lib (như hiện tại), false = app (có main.cpp)
XBFIRM = false

equals(XBFIRM, true) {
    TEMPLATE = lib
    CONFIG += staticlib
    DEFINES += XBFIRM
} else {
    TEMPLATE = app
}

TARGET = aiCompressor

DEPENDPATH += $$PWD/include

INCLUDEPATH += $$PWD/include

unix {
    INCLUDEPATH += /usr/local/include/
    LIBS += -latomic
}

equals(XBFIRM, true) {
    ! include( ../common.pri ) {
        error( "projectA Couldn't find the common.pri file!" )
    }
}

# USB
# LIBS += -lusb-1.0 -ludev -lz

HEADERS += \
    ABRConfigs.h \
    ABRFactory.h \
    CellularPredictive.h \
    IAdaptiveBitrateStreaming.h \
    SRTAdaptiveBitrateStreaming.h \
    SRTPeerStat.h \
    XBAdaptiveBitrateStreaming.h \
    XBDualCellAdaptiveBitrateStreaming.h \
    XBQoSService.h \
    VideoResolutionAdapter.h \
    dev/CameraControl.h \
    dev/NetworkHandler.h \
    lib/fuzzyData.h

SOURCES += \
    ABRFactory.cpp \
    CellularPredictive.cpp \
    IAdaptiveBitrateStreaming.cpp \
    SRTAdaptiveBitrateStreaming.cpp \
    SRTPeerStat.cpp \
    XBAdaptiveBitrateStreaming.cpp \
    XBDualCellAdaptiveBitrateStreaming.cpp \
    XBQoSService.cpp \
    VideoResolutionAdapter.cpp \
    dev/CameraControl.cpp \
    dev/NetworkHandler.cpp



!equals(XBFIRM, true) {
    SOURCES += main.cpp
}
