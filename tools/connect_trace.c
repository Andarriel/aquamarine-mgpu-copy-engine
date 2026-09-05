// LD_PRELOAD shim: prints a backtrace whenever something calls connect() on a unix socket.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int connect(int fd, const struct sockaddr* addr, socklen_t len) {
    static int (*real)(int, const struct sockaddr*, socklen_t) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "connect");
    if (addr && addr->sa_family == AF_UNIX) {
        const struct sockaddr_un* u = (const struct sockaddr_un*)addr;
        char name[120]; memset(name, 0, sizeof name);
        if (len > sizeof(sa_family_t)) { if (u->sun_path[0] == 0) { name[0]='@'; memcpy(name+1, u->sun_path+1, len - sizeof(sa_family_t) - 1); } else strncpy(name, u->sun_path, sizeof(name)-1); }
        fprintf(stderr, "\n[connect-trace] connect() to '%s' from:\n", name);
        void* bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
    }
    return real(fd, addr, len);
}
