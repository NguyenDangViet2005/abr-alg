QT += core testlib
QT -= gui
CONFIG += console c++17 testcase
TARGET = tst_srt_recovery
SOURCES += $$PWD/tst_srt_recovery.cpp $$PWD/../SRTAdaptiveBitrateStreaming.cpp $$PWD/../IAdaptiveBitrateStreaming.cpp $$PWD/../SRTPeerStat.cpp
HEADERS += $$PWD/../SRTAdaptiveBitrateStreaming.h $$PWD/../SRTRecoveryState.h $$PWD/../IAdaptiveBitrateStreaming.h
SOURCES += $$PWD/../ABRFactory.cpp $$PWD/../XBAdaptiveBitrateStreaming.cpp $$PWD/../NetworkReference.cpp
HEADERS += $$PWD/../ABRFactory.h $$PWD/../XBAdaptiveBitrateStreaming.h $$PWD/../SRTTuning.h
