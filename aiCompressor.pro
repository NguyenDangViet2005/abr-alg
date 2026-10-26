QT += core network

CONFIG += c++17
CONFIG += console

TEMPLATE = app
TARGET = aiCompressor

HEADERS += \
    C2TelemetryStat.h \
    C2TelemetryPolicy.h \
    dev/XbTelemetryHandler.h \
    dev/telemetry/C2TelemetryDecoder.h \
    dev/telemetry/SrtTelemetrySession.h \
    dev/telemetry/MavlinkStream.h \
    dev/telemetry/MavlinkCrcTable.h \
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
    dev/telemetry/C2TelemetryDecoder.cpp \
    dev/telemetry/SrtTelemetrySession.cpp \
    dev/telemetry/MavlinkStream.cpp \
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
