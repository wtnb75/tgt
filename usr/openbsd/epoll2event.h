#ifndef __EPOLL2EVENT_H__
#define  __EPOLL2EVENT_H__

#include <pthread.h>
#include <event.h>
#include <netinet/in_systm.h>
#define EPOLLIN   EV_READ
#define EPOLLOUT  EV_WRITE
#define EPOLLERR  EV_SIGNAL|EV_TIMEOUT

#define s6_addr8  __u6_addr.__u6_addr8
#define s6_addr16 __u6_addr.__u6_addr16
#define s6_addr32 __u6_addr.__u6_addr32

#define O_LARGEFILE 0
#define O_DIRECT 0
typedef off_t loff_t;

#define lseek64(a,b,c)      lseek(a,b,c)
#define semtimedop(a,b,c,d) semop(a,b,c)
#define pwrite64(a,b,c,d) pwrite(a,b,c,d)
#define pread64(a,b,c,d)  pread(a,b,c,d)
#define fdatasync(a)      fsync(a)
#define stat64            stat
#define fstat64           fstat

#define MSG_MORE	0

#endif // __EPOLL2EVENT_H__
