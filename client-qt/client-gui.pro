QT += core gui widgets printsupport serialport network
CONFIG += c++17
TEMPLATE = app
TARGET = client
SOURCES += main_gui.cpp mainwindow.cpp protocol.cpp qcustomplot.cpp tcpclient.cpp
HEADERS += mainwindow.h protocol.h qcustomplot.h tcpclient.h
