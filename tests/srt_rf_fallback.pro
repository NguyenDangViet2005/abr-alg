QT += core testlib
QT -= gui
CONFIG += console c++17 testcase
TARGET = tst_srt_rf_fallback
SOURCES += $$PWD/tst_srt_rf_fallback.cpp $$PWD/../SRTAdaptiveBitrateStreaming.cpp $$PWD/../IAdaptiveBitrateStreaming.cpp $$PWD/../SRTPeerStat.cpp
HEADERS += $$PWD/../SRTAdaptiveBitrateStreaming.h $$PWD/../IAdaptiveBitrateStreaming.h
