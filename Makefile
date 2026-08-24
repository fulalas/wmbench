# The benchmarks and artifact checks behind the measured numbers.
# A separate, deliberately plain Makefile.
#
#   make            build everything
#
# ./validate.sh runs the tests in checks/ and ./benchmark.sh the loads in
# tools/; both build what they need first, so this is only for building alone.

CC      ?= cc
CFLAGS  ?= -O2 -Wall
X11      = -lX11
LIBM     = -lm
GL       = -lX11 -lepoxy
# the shared helpers live in lib/, the programs include them by plain name
INC      = -Ilib

# Shared by more than one program: photographing the screen, waiting at the
# starting gate, telling the time, keeping out of the compositor's way,
# drawing the scene
CAPTURE = lib/capture.c lib/capture.h
GATE    = lib/gate.c lib/gate.h
NOW     = lib/now.c lib/now.h
POLITE  = lib/polite.c lib/polite.h
STAGE   = lib/stage.c lib/stage.h
PLACE   = lib/place.c lib/place.h

TOOLS  = tools/cmcheck tools/restack tools/fsbench2 tools/movebench \
         tools/transbench tools/manywin tools/popbench tools/argbbench \
         tools/videobench tools/usagebench
CHECKS = checks/motion_check checks/stale_check checks/pop_check \
         checks/suspend_check checks/shape_check checks/resize_check \
         checks/offscreen_check checks/iconify_check checks/leftover_check

all: $(TOOLS) $(CHECKS)

# Put the stress mix's windows in a known order
tools/restack: tools/restack.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(X11)
# Does anything own the compositing selection on this display?
tools/cmcheck: tools/cmcheck.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(X11)
tools/fsbench2: tools/fsbench2.c $(POLITE) $(GATE) $(NOW) $(STAGE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/polite.c lib/gate.c lib/now.c lib/stage.c $(GL)
tools/movebench: tools/movebench.c $(GATE) $(NOW) $(STAGE) $(PLACE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/gate.c lib/now.c lib/stage.c lib/place.c $(X11) $(LIBM)
tools/transbench: tools/transbench.c $(POLITE) $(GATE) $(NOW) $(STAGE) $(PLACE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/polite.c lib/gate.c lib/now.c lib/stage.c lib/place.c $(X11)
tools/manywin: tools/manywin.c $(STAGE) $(PLACE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/stage.c lib/place.c $(X11)
tools/popbench: tools/popbench.c $(GATE) $(NOW) $(STAGE) $(PLACE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/gate.c lib/now.c lib/stage.c lib/place.c $(X11)
tools/argbbench: tools/argbbench.c $(GATE) $(NOW) $(PLACE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/gate.c lib/now.c lib/place.c $(X11)
tools/videobench: tools/videobench.c $(GATE) $(NOW) $(PLACE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/gate.c lib/now.c lib/place.c $(X11) -lXext
tools/usagebench: tools/usagebench.c $(CAPTURE) $(POLITE) $(GATE) $(NOW) $(STAGE) $(PLACE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c lib/gate.c lib/now.c lib/stage.c lib/place.c $(X11)

# Every check photographs the screen through lib/capture.c, which knows how to
# do it on plain X11 and through an external screenshot command under Wayland
checks/stale_check checks/pop_check checks/suspend_check \
checks/resize_check checks/offscreen_check checks/iconify_check: %: %.c $(CAPTURE) $(POLITE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c $(X11)

# This one also runs inside the stress mix, so it waits at the gate
checks/motion_check: checks/motion_check.c $(CAPTURE) $(POLITE) $(GATE) $(NOW)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c lib/gate.c lib/now.c $(X11)

# Whole-screen photographs, no window of its own
checks/leftover_check: checks/leftover_check.c $(CAPTURE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c $(X11)

checks/shape_check: checks/shape_check.c $(CAPTURE) $(POLITE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c $(X11) -lXext

clean:
	rm -f $(TOOLS) $(CHECKS)

.PHONY: all clean
