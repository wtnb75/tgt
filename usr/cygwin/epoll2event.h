#ifndef __EPOLL2EVENT_H__
#define  __EPOLL2EVENT_H__

#include <event.h>
#define EPOLLIN   EV_READ
#define EPOLLOUT  EV_WRITE
#define EPOLLERR  EV_SIGNAL|EV_TIMEOUT

#define O_LARGEFILE 0
typedef off_t loff_t;

#define lseek64(a,b,c)      lseek(a,b,c)
#define semtimedop(a,b,c,d) semop(a,b,c)
#define pwrite64(a,b,c,d) pwrite(a,b,c,d)
#define pread64(a,b,c,d)  pread(a,b,c,d)
#define fdatasync(a)      fsync(a)
#define stat64            stat
#define fstat64           fstat

// #define __cpu_to_le32(x) htole32(x)
#define __le32_to_cpu(x) le32toh(x)

#define MSG_MORE	0

#endif // __EPOLL2EVENT_H__
