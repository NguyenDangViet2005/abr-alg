QT += core network

CONFIG += c++17
CONFIG += console

TEMPLATE = app
TARGET = aiCompressor

HEADERS += \
    C2TelemetryStat.h \
    C2TelemetryPolicy.h \
    dev/XbTelemetryHandler.h \
    dev/C2TelemetryDecoder.h \
    dev/SrtTelemetrySession.h \
    dev/MavlinkStream.h \
    dev/MavlinkCrcTable.h \
    ABRConfigs.h \
    ABRFactory.h \
    IAdaptiveBitrateStreaming.h \
    SRTAdaptiveBitrateStreaming.h \
    SRTPeerStat.h \
    SiyiLinkStat.h \
    VideoResolutionAdapter.h \
    XBQoSService.h \
    dev/CameraControl.h \
    dev/NetworkHandler.h \
    dev/SiyiLinkHandler.h

SOURCES += \
    C2TelemetryPolicy.cpp \
    dev/XbTelemetryHandler.cpp \
    dev/C2TelemetryDecoder.cpp \
    dev/SrtTelemetrySession.cpp \
    dev/MavlinkStream.cpp \
    main.cpp \
    ABRFactory.cpp \
    IAdaptiveBitrateStreaming.cpp \
    SRTAdaptiveBitrateStreaming.cpp \
    SRTPeerStat.cpp \
    VideoResolutionAdapter.cpp \
    XBQoSService.cpp \
    dev/CameraControl.cpp \
    dev/NetworkHandler.cpp \
    dev/SiyiLinkHandler.cpp
