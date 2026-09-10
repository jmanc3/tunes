TARGET := tunes

CC := cc
CXX := c++

SRC := $(shell find src -type f \( -name '*.c' -o -name '*.cpp' \))

C_SRC := $(filter %.c,$(SRC))
CPP_SRC := $(filter %.cpp,$(SRC))

OBJ := $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o)

PKGS := \
	cairo \
	egl \
	glesv2 \
	wayland-egl \
	pango \
	pangocairo \
	librsvg-2.0 \
	wayland-server \
	wayland-client \
	wayland-cursor \
	xkbcommon \
	libdrm \
	pixman-1 \
	hyprland \
	libinput \
	libudev \
	gio-2.0 \
	dbus-1 \
	alsa \
	libpipewire-0.3 \
	libpulse \
	gtk+-3.0 \
	libmagic \
	taglib

CPPFLAGS := -Iinclude $(shell pkg-config --cflags $(PKGS))
CFLAGS := -std=c11
CXXFLAGS := -std=c++26
LDLIBS := $(shell pkg-config --libs $(PKGS))

.PHONY: all release clean

all: CFLAGS += -O0 -g
all: CXXFLAGS += -O0 -g
all: $(TARGET)

release: CFLAGS += -O3 -DNDEBUG
release: CXXFLAGS += -O3 -DNDEBUG
release: clean $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
