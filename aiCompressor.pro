# QT += dbus
QT += core
QT += network
# QT += serialport
QT += websockets
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
    AICompressor.h \
    CellularPredictive.h \
    IAdaptiveBitrateStreaming.h \
    SRTAdaptiveBitrateStreaming.h \
    XBAdaptiveBitrateStreaming.h \
    XBDualCellAdaptiveBitrateStreaming.h \
    XBQoSService.h \
    dev/NetworkHandler.h \
    lib/fuzzyData.h

SOURCES += \
    ABRFactory.cpp \
    AICompressor.cpp \
    CellularPredictive.cpp \
    IAdaptiveBitrateStreaming.cpp \
    SRTAdaptiveBitrateStreaming.cpp \
    XBAdaptiveBitrateStreaming.cpp \
    XBDualCellAdaptiveBitrateStreaming.cpp \
    XBQoSService.cpp \
    dev/NetworkHandler.cpp
    XBDualCellAdaptiveBitrateStreaming.cpp

# CameraControl / NetworkHandler chỉ build khi XBFIRM (lib mode).
equals(XBFIRM, true) {
    HEADERS += \
        dev/CameraControl.h \
        dev/NetworkHandler.h
    SOURCES += \
        dev/CameraControl.cpp \
        dev/NetworkHandler.cpp
}

!equals(XBFIRM, true) {
    SOURCES += main.cpp
}
