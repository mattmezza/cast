/* IPC-only test: never creates NSApplication, a window, or capture devices. */
#include "panel_macos.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (argc == 2) {
        char error[512];
        return mac_panel_claim(argv[1], error, sizeof error) == 1 ? 0 : 1;
    }
    char directory[] = "/tmp/cast-panel-test-XXXXXX";
    assert(mkdtemp(directory));
    char socket_path[256], lock_path[256], error[512] = {0};
    snprintf(socket_path, sizeof socket_path, "%s/control", directory);
    snprintf(lock_path, sizeof lock_path, "%s/control.panel.lock", directory);
    assert(mac_panel_claim("/tmp/cast-panel-unsafe", error, sizeof error) == -1);
    assert(mac_panel_claim(socket_path, error, sizeof error) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        /* exec gives this launch independent static state/descriptors. */
        execl(argv[0], argv[0], socket_path, NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    mac_panel_release();
    assert(mac_panel_claim(socket_path, error, sizeof error) == 0);
    mac_panel_release();
    unlink(lock_path);
    assert(!rmdir(directory));
    puts("macOS panel single-instance IPC and release/reacquire passed (no UI)");
    return 0;
}
