# Standalone build for module-maemo-volume.
#
# The real build happens through debian/rules; this is for hacking on the
# module directly.
#
#   make            build
#   make install    install into the system pipewire module directory
#   make clean

MODULE   := maemo-volume
TARGET   := libpipewire-module-$(MODULE).so
SRC      := module-maemo-volume.c

PKG_CFLAGS  := $(shell pkg-config --cflags libpipewire-0.3 libspa-0.2)
PKG_LIBS    := $(shell pkg-config --libs libpipewire-0.3)

MULTIARCH := $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)
DESTDIR     ?= /
MODDIR      := $(DESTDIR)/usr/lib/$(MULTIARCH)/pipewire-0.3

# -fPIC goes in its own variable, appended after CFLAGS: dpkg injects CFLAGS
# and a plain "CFLAGS ?= -fPIC" gets replaced outright, which breaks the
# shared link with "relocation ... can not be used when making a shared
# object".
CFLAGS ?= -O2 -Wall
PICFLAGS := -fPIC

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(PICFLAGS) -shared $(PKG_CFLAGS) -o $@ $< $(PKG_LIBS)

install: $(TARGET)
	install -d $(MODDIR)
	install -m 0644 $(TARGET) $(MODDIR)/

clean:
	rm -f $(TARGET)

.PHONY: all install clean
