TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

LIBS += -lavcodec -lavformat -lavutil -lavfilter -lpthread -lswresample -lm -lz

SOURCES += main.c
