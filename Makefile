CXX ?= g++
PKG_CONFIG ?= pkg-config
CLANG_FORMAT ?= clang-format

TARGET := dsfemon
BUILD_DIR := build

SOURCES := \
	command_line.cpp \
	dsfemon.cpp \
	ncurses_present.cpp \
	color.cpp \
	demux_monitor.cpp \
	demux_reader.cpp \
	demux_snapshot.cpp \
	si_parser.cpp \
	device_discovery.cpp \
	frontend_monitor.cpp \
	frontend_status.cpp \
	ui_helpers.cpp \
	frontend_view.cpp \
	demux_view.cpp

OBJECTS := $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)
FORMAT_SOURCES := $(SOURCES) $(wildcard *.h)

NCURSES_CFLAGS := $(shell $(PKG_CONFIG) --cflags ncursesw)
NCURSES_LIBS := $(shell $(PKG_CONFIG) --libs ncursesw)

CXXFLAGS ?= -Wall -Wextra -Wpedantic -g
LDLIBS = $(NCURSES_LIBS) -lpthread

.PHONY: clean format format-check install

$(TARGET): $(OBJECTS)
	$(CXX) $(NCURSES_CFLAGS) $(CXXFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(NCURSES_CFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

format:
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

install: $(TARGET)
	cp ./$(TARGET) /usr/local/bin/
