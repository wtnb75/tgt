#ifndef __EPOLL2EVENT_H__
#define  __EPOLL2EVENT_H__

#include <event.h>
#define EPOLLIN   EV_READ
#define EPOLLOUT  EV_WRITE
#define EPOLLERR  EV_SIGNAL|EV_TIMEOUT
#define SOL_TCP IPPROTO_TCP

#define s6_addr8  _S6_un._S6_u8
#define s6_addr16 _S6_un._S6_u8    // XXX! will not work
#define s6_addr32 _S6_un._S6_u32

#include <sys/int_fmtio.h>

#define O_DIRECT 0   // XXX!
typedef off_t loff_t;

#define MSG_MORE	0

#endif // __EPOLL2EVENT_H__
