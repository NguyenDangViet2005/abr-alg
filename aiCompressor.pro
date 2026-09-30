QT += core network

CONFIG += c++17
CONFIG += console

TEMPLATE = app
TARGET = aiCompressor

HEADERS += \
    ABRConfigs.h \
    ABRFactory.h \
    IAdaptiveBitrateStreaming.h \
    SRTAdaptiveBitrateStreaming.h \
    SRTPeerStat.h \
    VideoResolutionAdapter.h \
    XBQoSService.h \
    dev/CameraControl.h \
    dev/NetworkHandler.h

SOURCES += \
    main.cpp \
    ABRFactory.cpp \
    IAdaptiveBitrateStreaming.cpp \
    SRTAdaptiveBitrateStreaming.cpp \
    SRTPeerStat.cpp \
    VideoResolutionAdapter.cpp \
    XBQoSService.cpp \
    dev/CameraControl.cpp \
    dev/NetworkHandler.cpp
