/* Host-only QEMU fixture: bound SLIRP's proxy buffering for one loopback peer.
 * No kernel or system-wide socket settings are modified. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
int connect(int fd,const struct sockaddr *address,socklen_t size) {
    int (*next)(int,const struct sockaddr *,socklen_t)=dlsym(RTLD_NEXT,"connect");
    if (!next) { errno=ENOSYS; return -1; }
    if (address && size>=sizeof(struct sockaddr_in) && address->sa_family==AF_INET) {
        const struct sockaddr_in *in=(const struct sockaddr_in *)address;
        if (ntohs(in->sin_port)==17779 && ntohl(in->sin_addr.s_addr)==INADDR_LOOPBACK) {
            int bound=1024;
            if (setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&bound,sizeof(bound))) return -1;
            fprintf(stderr,"[TCP PEER FIXTURE] bounded proxy fd=%d\n",fd);
        }
    }
    return next(fd,address,size);
}
