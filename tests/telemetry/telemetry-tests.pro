QT += core network
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = test-c2-telemetry
INCLUDEPATH += ../..
HEADERS += ../../IAdaptiveBitrateStreaming.h ../../SRTAdaptiveBitrateStreaming.h ../../ABRFactory.h ../../dev/XbTelemetryHandler.h
SOURCES += test-c2-telemetry.cpp \
    ../../C2TelemetryPolicy.cpp \
    ../../dev/XbTelemetryHandler.cpp \
    ../../dev/telemetry/MavlinkStream.cpp \
    ../../dev/telemetry/SrtTelemetrySession.cpp \
    ../../dev/telemetry/C2TelemetryDecoder.cpp \
    ../../IAdaptiveBitrateStreaming.cpp \
    ../../SRTAdaptiveBitrateStreaming.cpp \
    ../../ABRFactory.cpp
