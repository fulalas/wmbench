# The benchmarks and artifact checks behind the measured numbers.
# A separate, deliberately plain Makefile.
#
#   make            build everything
#   make check      run the nine artifact checks against whatever compositor is
#                   currently running

CC      ?= cc
CFLAGS  ?= -O2 -Wall
X11      = -lX11
LIBM     = -lm
GL       = -lX11 -lepoxy
# the shared helpers live in lib/, the programs include them by plain name
INC      = -Ilib

# Shared by more than one program: photographing the screen, waiting at the
# starting gate, keeping out of the compositor's way, drawing the scene
CAPTURE = lib/capture.c lib/capture.h
GATE    = lib/gate.c lib/gate.h
POLITE  = lib/polite.c lib/polite.h
STAGE   = lib/stage.c lib/stage.h

TOOLS  = tools/cmcheck tools/restack tools/fsbench2 tools/multiscene \
         tools/movebench tools/transbench tools/manywin tools/popbench \
         tools/argbbench tools/videobench tools/usagebench
CHECKS = checks/motion_check checks/stale_check checks/pop_check \
         checks/suspend_check checks/zoom_check checks/shape_check \
         checks/resize_check checks/offscreen_check checks/iconify_check

all: $(TOOLS) $(CHECKS)

# Put the stress mix's windows in a known order
tools/restack: tools/restack.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(X11)
# Does anything own the compositing selection on this display?
tools/cmcheck: tools/cmcheck.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(X11)
tools/fsbench2: tools/fsbench2.c $(POLITE) $(GATE) $(STAGE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/polite.c lib/gate.c lib/stage.c $(GL)
tools/multiscene: tools/multiscene.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(GL) $(LIBM)
tools/movebench: tools/movebench.c $(GATE) $(STAGE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/gate.c lib/stage.c $(X11) $(LIBM)
tools/transbench: tools/transbench.c $(POLITE) $(GATE) $(STAGE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/polite.c lib/gate.c lib/stage.c $(X11)
tools/manywin: tools/manywin.c $(STAGE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/stage.c $(X11)
tools/popbench: tools/popbench.c $(GATE) $(STAGE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/gate.c lib/stage.c $(X11)
tools/argbbench: tools/argbbench.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(X11)
tools/videobench: tools/videobench.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(X11) -lXext
tools/usagebench: tools/usagebench.c $(CAPTURE) $(POLITE) $(GATE) $(STAGE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c lib/gate.c lib/stage.c $(X11)

# Every check photographs the screen through lib/capture.c, which knows how to
# do it on plain X11 and through an external screenshot command under Wayland
checks/motion_check checks/stale_check checks/pop_check checks/suspend_check \
checks/resize_check checks/offscreen_check checks/iconify_check: %: %.c $(CAPTURE) $(POLITE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c $(X11)

# The magnifier has to be driven through the pointer
checks/zoom_check: checks/zoom_check.c $(CAPTURE) $(POLITE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c $(X11) -lXtst

checks/shape_check: checks/shape_check.c $(CAPTURE) $(POLITE)
	$(CC) $(CFLAGS) $(INC) -o $@ $< lib/capture.c lib/polite.c $(X11) -lXext

check: $(CHECKS)
	@fail=0; \
	for c in $(CHECKS); do \
	    printf '%-16s ' "$$(basename $$c)"; \
	    out=$$(./$$c 2) || fail=1; \
	    echo "$$out" | tail -1; \
	done; \
	exit $$fail

clean:
	rm -f $(TOOLS) $(CHECKS)

.PHONY: all check clean
