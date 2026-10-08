QT += core network

CONFIG += c++17
CONFIG += console

TEMPLATE = app
TARGET = aiCompressor

HEADERS += \
    ABRConfigs.h \
    ABRFactory.h \
    IAdaptiveBitrateStreaming.h \
    NetworkReference.h \
    RouterInterfaceStat.h \
    SRTAdaptiveBitrateStreaming.h \
    SRTPeerStat.h \
    VideoResolutionAdapter.h \
    XBAdaptiveBitrateStreaming.h \
    XBQoSService.h \
    dev/CameraControl.h \
    dev/NetworkHandler.h \
    dev/QoSDebugProtocol.h \
    dev/QoSDebugSender.h \
    dev/RouterNetworkHandler.h

SOURCES += \
    dev/RouterNetworkHandler.cpp \
    main.cpp \
    ABRFactory.cpp \
    IAdaptiveBitrateStreaming.cpp \
    NetworkReference.cpp \
    SRTAdaptiveBitrateStreaming.cpp \
    SRTPeerStat.cpp \
    VideoResolutionAdapter.cpp \
    XBAdaptiveBitrateStreaming.cpp \
    XBQoSService.cpp \
    dev/CameraControl.cpp \
    dev/NetworkHandler.cpp \
    dev/QoSDebugProtocol.cpp \
    dev/QoSDebugSender.cpp
