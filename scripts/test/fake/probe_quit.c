/* quit: quitshim's two jobs.
 *
 * Qt's eglfs plugin installs a SIGTERM handler that calls _exit(1). quitshim
 * installs its own first, turns Qt's attempt away, and has a thread call
 * QCoreApplication::quit() when the signal comes. This program plays Qt's
 * part: it exports that symbol, tries to install a handler the way Qt does,
 * and sends itself the signal.
 *
 * On its way out Engine asks libaz0x-splashctl for a splash client and aborts
 * without one. Nothing listens for it outside the firmware's boot chain, so
 * quitshim answers that too.
 *
 * quitshim arms itself only in a process named Engine, which is why this
 * program is installed under that name. */
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "fake.h"

static volatile sig_atomic_t quit_called, qt_handler_ran;

/* QCoreApplication::quit(), which quitshim looks up by this name. */
void _ZN16QCoreApplication4quitEv(void);
void _ZN16QCoreApplication4quitEv(void) { quit_called = 1; }

static void qt_handler(int sig) { (void)sig; qt_handler_ran = 1; }

void probe_quit(void) {
    if (!shim_exporting("sigaction")) {
        t_skip("quit", "no quitshim preloaded");
        return;
    }
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = qt_handler;
    sigaction(SIGTERM, &sa, &old);
    kill(getpid(), SIGTERM);
    for (int i = 0; i < 200 && !quit_called && !qt_handler_ran; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (qt_handler_ran) t_fail("quit.sigterm", "Qt's handler ran, which exits on the spot");
    else if (!quit_called) t_fail("quit.sigterm", "nothing called QCoreApplication::quit");
    else t_ok("quit.sigterm", "QCoreApplication::quit called, Qt's handler kept out");
    if (qt_handler_ran) sigaction(SIGTERM, &old, NULL);

    int (*client_new)(void **) = entry("az0x_splash_client_new");
    int (*freeze)(void) = entry("az0x_splash_freeze_display");
    void *client = NULL;
    if (!client_new || !freeze) t_fail("quit.splash", "no splash client functions");
    else if (client_new(&client) != 0 || !client) t_fail("quit.splash", "no splash client");
    else if (freeze() != 0) t_fail("quit.splash", "freeze_display failed");
    else t_ok("quit.splash", "client created and display frozen");
}
