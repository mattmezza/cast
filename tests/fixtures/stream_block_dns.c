/* Test-only libc interposer. It proves that actual getaddrinfo execution can
 * remain blocked indefinitely while the daemon retires its isolated worker. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    if (node && !strcmp(node, "cast-blocked.invalid")) {
        const char *marker = getenv("CAST_TEST_DNS_MARKER");
        if (marker) {
            int fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (fd >= 0) {
                (void)write(fd, "1", 1);
                close(fd);
            }
        }
        for (;;) {
            pause();
        }
    }
    int (*original)(const char *, const char *, const struct addrinfo *, struct addrinfo **) =
        dlsym(RTLD_NEXT, "getaddrinfo");
    return original(node, service, hints, result);
}
