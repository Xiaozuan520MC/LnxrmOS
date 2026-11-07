/* /init -- first user process. Like Linux: userland drives everything;
 * the kernel just provides fork/exec/wait and files. */
#include "ulib.h"

static char buf[256];

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* wire stdin/stdout/stderr to the console */
    long c = kopen("/dev/console", 2);
    if (c >= 0) {
        if (c != 0) {
            kdup2(c, 0);
            kclose(c);
        }
        kdup2(0, 1);
        kdup2(0, 2);
    }

    /* disk smoke test through IDE+FAT32 */
    int dfd = kopen("/README.md", 0);
    if (dfd >= 0) {
        long n = kread(dfd, buf, sizeof(buf));
        if (n > 0) kwrite(1, buf, n);
        kclose(dfd);
    } else {
        xputs("init: no /README.md\n");
    }

    /* Boot straight into the desktop: it captures the console (the boot
     * log stays readable in its Terminal window) and forks the shell the
     * Terminal types into.  When it leaves the screen -- killed from its
     * own Task Manager -- the console gets a shell.  Respawning the
     * desktop here would make an exited desktop unkillable in effect:
     * one run, then the console is yours. */
    long pid = kfork();
    if (pid == 0) {
        char *d_argv[] = {"/bin/desktop", 0};
        char *envp[] = {0};
        if (kexecve("/bin/desktop", d_argv, envp) < 0) {
            xputs("init: exec /bin/desktop failed\n");
            kexit(1);
        }
    }
    int st = 0;
    kwaitpid(pid, &st, 0);

    /* The desktop is gone: hand the console to a shell.  If the shell
     * dies too it stays dead -- respawning it would make it unkillable. */
    pid = kfork();
    if (pid == 0) {
        char *sh_argv[] = {"/bin/sh", 0};
        char *envp[] = {0};
        if (kexecve("/bin/sh", sh_argv, envp) < 0) {
            xputs("init: exec /bin/sh failed\n");
            kexit(1);
        }
    }
    kwaitpid(pid, &st, 0);
    xputs("\ninit: terminal exited\n");

    /* P0-2: init is every orphan's reaper.  After this loop no corpse has
     * to outlive its reaper; with no children at all ECHILD would spin the
     * loop, so breathe once per second instead. */
    for (;;)
        if (kwaitpid(-1, &st, 0) == LNXRM_ECHILD) ksleep_ms(1000, 0);
    return 0;
}
