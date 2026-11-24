/* Gate-only in-process cooperative stop polling. No interposed symbols, signals,
 * timeouts, or successful product publication on cancellation. Load ONLY into
 * the finish child. Preserve the frozen binary and partial files for inspection. */
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static void *monitor(void *arg) {
    const char *path = arg;
    for (;;) {
        if (!access(path, F_OK)) {
            static const char msg[]="STOP_OBSERVED phase=finish-gate-monitor exit=75\n";
            ssize_t logged = write(2, msg, sizeof msg-1); (void)logged; _exit(75);
        }
        struct timespec delay={0,100000000}; nanosleep(&delay,0);
    }
    return 0;
}
__attribute__((constructor)) static void start(void) {
    char *path=getenv("XSA_GATE_PRIVATE_STOP"); pthread_t tid;
    if (!path || path[0]!='/') _exit(70);
    int err=pthread_create(&tid,0,monitor,path);
    if(err){dprintf(2,"finish monitor startup: %s\n",strerror(err));_exit(70);}
    pthread_detach(tid);
}
