/* Makes SIGTERM quit Engine, instead of killing it.
 *
 * Engine has no SIGTERM handling of its own. The one in effect is Qt's: the
 * eglfs platform plugin installs a handler (QFbVtHandler) for SIGINT and
 * SIGTERM that puts the console keyboard back and calls _exit(1). Nothing of
 * Engine's runs after that -- no window is closed, no destructor runs, and what
 * it had open on a USB drive is left as it was at that instant. On hardware
 * that does not matter, because nothing there ever sends the signal: Engine
 * quits itself and then powers the unit off. Run as a service on an ordinary
 * machine, SIGTERM is how every stop, reboot and shutdown reaches it.
 *
 * So this takes the signal over. Its handler wakes a thread, and the thread
 * calls QCoreApplication::quit() -- the call Engine's own quit ends in, and one
 * Qt allows from any thread. Engine then leaves its event loop and exits 0.
 * Qt's later attempt to install its own SIGTERM handler is turned away;
 * SIGINT and the rest are left to it.
 *
 * A caller should still give up after a while and use SIGKILL: quit() is a
 * request, and it does nothing at all before Engine has entered its event loop.
 * Each further SIGTERM asks again.
 *
 * Quitting has one more need. On its way out Engine hands the display to the
 * firmware's splash service, to hold the last frame while the unit powers off:
 * az0x_splash_client_new() from libaz0x-splashctl, which connects to
 * /run/az0x-splash-control, then az0x_splash_freeze_display(). Nothing listens
 * on that socket outside the real boot chain, and Engine treats a missing
 * client as a failed postcondition ("Could not create splash client.") and
 * aborts, a few steps into its shutdown. So the client functions are answered
 * here: a client that is always there, and that does nothing.
 *
 * Preloaded into everything that starts with Engine, like the other shims. The
 * signal handling only does anything in the Engine process itself.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

static int wake_fd = -1;
static int installed;

static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);

static void on_term(int sig) {
    (void)sig;
    int saved = errno;
    uint64_t one = 1;
    if (write(wake_fd, &one, sizeof(one)) < 0) { /* nothing to be done about it here */ }
    errno = saved;
}

static void *quit_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t count;
        if (read(wake_fd, &count, sizeof(count)) < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        /* QCoreApplication::quit(), a static member. Looked up only now: the
         * constructor below runs before Qt is necessarily loaded. */
        void (*quit)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "_ZN16QCoreApplication4quitEv");
        if (!quit) {
            fprintf(stderr, "=== QUITSHIM: SIGTERM, and no QCoreApplication::quit to call; exiting\n");
            _exit(1);
        }
        fprintf(stderr, "=== QUITSHIM: SIGTERM, asking Engine to quit\n");
        quit();
    }
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (!real_sigaction)
        real_sigaction = (int (*)(int, const struct sigaction *, struct sigaction *))
            dlsym(RTLD_NEXT, "sigaction");
    /* Qt's handler would replace ours. Report what is installed and keep it. */
    if (installed && sig == SIGTERM && act)
        return old ? real_sigaction(sig, NULL, old) : 0;
    return real_sigaction(sig, act, old);
}

/* libaz0x-splashctl's client interface. Only the two calls Engine was seen to
 * make have known use; the rest are here so that no call reaches the real
 * library with the handle below. They return 0 or a negative errno there, and
 * their other arguments are not looked at. */
static int splash_client;

int az0x_splash_client_new(void **client) { *client = &splash_client; return 0; }
void az0x_splash_client_free(void *client) { (void)client; }
int az0x_splash_freeze_display(void) { return 0; }
int az0x_splash_drop_display(void) { return 0; }
int az0x_splash_show_image(void) { return 0; }
int az0x_splash_shutdown(void) { return 0; }
int az0x_splash_enable_debug(void) { return 0; }

__attribute__((constructor))
static void quitshim_init(void) {
    /* LD_PRELOAD reaches every process started with Engine -- shells, cat,
     * fscryptctl. Only Engine is a Qt application to ask. */
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    const char *base = strrchr(exe, '/');
    if (!base || strcmp(base + 1, "Engine") != 0) return;

    wake_fd = eventfd(0, EFD_CLOEXEC);
    if (wake_fd < 0) return;
    pthread_t t;
    if (pthread_create(&t, NULL, quit_thread, NULL) != 0) return;
    pthread_detach(t);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) == 0) installed = 1;
}
