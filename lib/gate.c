/*
 * A starting gun for a mix of programs.
 *
 * The stress test runs six programs together, and each one has to do its own
 * fixed amount of work with all the others already on screen. Left to
 * themselves they start whenever their windows happen to be ready, so the
 * first tasks of the quick ones are done while the slow ones are still
 * appearing - work that belongs to the measurement but was not made under the
 * load the measurement is about.
 *
 * With BENCH_GO set to a file name, a program finishes everything it can do on
 * its own and then waits here. The script arranges the screen, creates the
 * file, and every program starts measuring at once.
 *
 * There is no timeout on purpose: nothing here is ever cut short on the clock.
 * The script that opened the gate is also the one that kills the programs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "gate.h"

void bench_wait_go (void)
{
    const char *path = getenv ("BENCH_GO");

    if (path == NULL || *path == '\0')
    {
        return;
    }
    /*
     * Say so before waiting. Programs reach here at very different times - a
     * GL window is up in a moment, a big pixmap of content takes seconds to
     * draw - and a gate opened before the slow ones arrive does not hold them
     * at all: they start late, finish late, and the mix is staggered by
     * however long their setup took. The script waits for every one of these
     * lines before it opens the gate.
     */
    printf ("MEASURE-READY\n");
    fflush (stdout);

    while (access (path, F_OK) != 0)
    {
        usleep (5000);
    }
}
