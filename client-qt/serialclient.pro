QT += core serialport
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
SOURCES += serialclient_main.cpp protocol.cpp
HEADERS += protocol.h
