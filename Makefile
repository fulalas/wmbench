# The benchmarks and artifact checks behind the measured numbers.
# A separate, deliberately plain Makefile.
#
#   make            build everything
#
# ./validate.sh runs the tests in checks/ and ./benchmark.sh the loads in
# tools/; both build what they need first, so this is only for building alone.
#
# Every program carries both backends: lib/win.c picks X11 or Wayland at
# startup from the session (BENCH_BACKEND overrides). The Wayland protocol
# XMLs are vendored under lib/protocols/ and wayland-scanner turns them into
# code here, so the build does not depend on which protocol packages a
# distribution ships.

CC      ?= cc
CFLAGS  ?= -O2 -Wall
X11      = -lX11 -lXext
LIBM     = -lm
# Simply expanded, or pkg-config would run again on every link line. An empty
# answer means the .pc file is missing, which is said here rather than left to
# surface much later as undefined references to wl_display_connect.
WL_LIBS   := $(shell pkg-config --libs wayland-client 2>/dev/null)
WL_CFLAGS := $(shell pkg-config --cflags wayland-client 2>/dev/null)
GL_LIBS   := $(shell pkg-config --libs wayland-egl egl 2>/dev/null)
GL_CFLAGS := $(shell pkg-config --cflags wayland-egl egl 2>/dev/null)
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(strip $(WL_LIBS)),)
$(error pkg-config cannot find wayland-client; install the Wayland client development files)
endif
ifeq ($(strip $(GL_LIBS)),)
$(error pkg-config cannot find wayland-egl or egl; install the EGL development files)
endif
endif
WL       = $(WL_LIBS)
GL       = -lepoxy $(GL_LIBS)
# the shared helpers live in lib/, the programs include them by plain name
INC      = -Ilib $(WL_CFLAGS) $(GL_CFLAGS)

# The Wayland protocols spoken, each probed at runtime; a compositor that
# lacks one gets the suite's refusal channels, not a crash
PROTOS = xdg-shell wlr-layer-shell-unstable-v1 xdg-activation-v1 \
         alpha-modifier-v1 xdg-output-unstable-v1 \
         xdg-decoration-unstable-v1 xx-zones-v1 idle-inhibit-unstable-v1 \
         wlr-foreign-toplevel-management-unstable-v1
PROTO_SRC = $(PROTOS:%=lib/protocols/%-protocol.c)
PROTO_HDR = $(PROTOS:%=lib/protocols/%-client-protocol.h)

lib/protocols/%-protocol.c: lib/protocols/%.xml
	wayland-scanner private-code $< $@
lib/protocols/%-client-protocol.h: lib/protocols/%.xml
	wayland-scanner client-header $< $@

# One window layer for everything: the backend split, the two backends, the
# software renderer the Wayland one draws with, screen capture and the
# placement policy.
#
# Compiled once into objects rather than handed to every link line: there are
# seventeen programs and fifteen shared sources, and rebuilding the second from
# source for each of the first is most of a build.
WIN_SRC = lib/win.c lib/win_x11.c lib/win_wl.c lib/draw.c lib/capture.c \
          lib/place.c lib/now.c $(PROTO_SRC)
WIN  = $(WIN_SRC:.c=.o)
WINH = lib/win.h lib/win_priv.h lib/draw.h lib/capture.h lib/place.h \
       lib/now.h $(PROTO_HDR)
GATE = lib/gate.o
LIBS = $(X11) $(WL)

TOOLS  = tools/cmcheck tools/restack tools/fsbench2 tools/movebench \
         tools/transbench tools/manywin tools/popbench tools/argbbench \
         tools/videobench tools/usagebench
CHECKS = checks/motion_check checks/stale_check checks/pop_check \
         checks/suspend_check checks/shape_check checks/resize_check \
         checks/offscreen_check checks/iconify_check checks/leftover_check

all: $(TOOLS) $(CHECKS)

# Below "all", or this static pattern rule would become the default goal.
# Every object waits on the generated headers, which do not exist on a fresh
# checkout until wayland-scanner has run.
$(WIN) $(GATE): %.o: %.c $(WINH) lib/gate.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# Does anything own the compositing selection on this display? X11 only.
tools/cmcheck: tools/cmcheck.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< -lX11
# Put windows in a known order: its own X11 tree walk, the foreign-toplevel
# list on Wayland
tools/restack: tools/restack.c $(WIN) $(WINH)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(LIBS)
tools/fsbench2: tools/fsbench2.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS) $(GL)
tools/movebench: tools/movebench.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS) $(LIBM)
tools/transbench: tools/transbench.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS)
tools/manywin: tools/manywin.c $(WIN) $(WINH)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(LIBS)
tools/popbench: tools/popbench.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS)
tools/argbbench: tools/argbbench.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS)
tools/videobench: tools/videobench.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS)
tools/usagebench: tools/usagebench.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS)

checks/stale_check checks/pop_check checks/suspend_check \
checks/resize_check checks/offscreen_check checks/iconify_check: %: %.c $(WIN) $(WINH)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(LIBS)

# This one also runs inside the stress mix, so it waits at the gate
checks/motion_check: checks/motion_check.c $(WIN) $(WINH) $(GATE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(GATE) $(LIBS)

# Whole-screen photographs and the window tree, which only X11 hands out
checks/leftover_check: checks/leftover_check.c $(WIN) $(WINH)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(LIBS)

checks/shape_check: checks/shape_check.c $(WIN) $(WINH)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(WIN) $(LIBS)

clean:
	rm -f $(TOOLS) $(CHECKS) $(WIN) $(GATE) $(PROTO_SRC) $(PROTO_HDR)

.PHONY: all clean
