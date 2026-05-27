CXX ?= g++
PKG_CONFIG ?= pkg-config

TARGET := dsfemon
BUILD_DIR := build

SOURCES := \
	femon.cpp \
	ncurses_present.cpp \
	color.cpp \
	demux_monitor.cpp \
	device_discovery.cpp \
	frontend_monitor.cpp \
	frontend_status.cpp \
	ui_helpers.cpp \
	frontend_view.cpp \
	demux_view.cpp

OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)

NCURSES_CFLAGS := $(shell $(PKG_CONFIG) --cflags ncursesw)
NCURSES_LIBS := $(shell $(PKG_CONFIG) --libs ncursesw)

CXXFLAGS ?= -Wall -Wextra -Wpedantic -g
LDLIBS = $(NCURSES_LIBS) -lpthread

.PHONY: femon clean install

femon: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(NCURSES_CFLAGS) $(CXXFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(NCURSES_CFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

install: $(TARGET)
	cp ./$(TARGET) /usr/local/bin/
