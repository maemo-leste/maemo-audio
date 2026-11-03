# Out-of-tree equivalence harness: Nokia's delayed-HW-volume queue vs
# ours. Not part of the package; links the mixer sources directly.
#
#   make && ./equiv

MIXER ?= /home/user/git/maemo-audio/mixer

CC      ?= cc
PKG_CFLAGS := $(shell pkg-config --cflags spa-0.2 2>/dev/null)
ifeq ($(strip $(PKG_CFLAGS)),)
PKG_CFLAGS := -I/usr/include/spa-0.2 -I/usr/include/pipewire-0.3
endif
CFLAGS  ?= -O1 -g -Wall -Wno-unused-variable
INCLUDES = -I. -I$(MIXER) $(PKG_CFLAGS)

# The scheduling law and its dependencies. Nothing from PipeWire at
# runtime: the registry is linked only because snap_to_grid reads the
# measured period grid out of it.
MIXER_SRC = $(MIXER)/maemo-vol-engine.c $(MIXER)/maemo-hw-queue.c \
            $(MIXER)/maemo-vol-map.c    $(MIXER)/mix-registry.c

OBJS = equiv.o nokia_queue.o \
       maemo-vol-engine.o maemo-hw-queue.o maemo-vol-map.o mix-registry.o

equiv: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

equiv.o: equiv.c nokia_queue.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

nokia_queue.o: nokia_queue.c nokia_queue.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

maemo-vol-engine.o: $(MIXER)/maemo-vol-engine.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

maemo-hw-queue.o: $(MIXER)/maemo-hw-queue.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

maemo-vol-map.o: $(MIXER)/maemo-vol-map.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

mix-registry.o: $(MIXER)/mix-registry.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f equiv *.o

.PHONY: clean run
run: equiv
	./equiv
