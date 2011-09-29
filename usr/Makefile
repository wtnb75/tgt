OS=$(shell uname)
ifeq ($(OS),FreeBSD)
CFLAGS += -I/usr/local/include -include freebsd/epoll2event.h -Ifreebsd
LIBS += -levent -L/usr/local/lib
endif
ifeq ($(OS),DragonFly)
CFLAGS += -I/usr/pkg/include -include dragonfly/epoll2event.h -Idragonfly
LIBS += -levent -L/usr/pkg/lib
endif
ifeq ($(OS),OpenBSD)
CFLAGS += -I/usr/local/include -include openbsd/epoll2event.h -Iopenbsd
LIBS += -levent -L/usr/local/lib
endif
ifeq ($(OS),NetBSD)
CFLAGS += -I/usr/pkg/include -include netbsd/epoll2event.h -Inetbsd
LIBS += -levent -L/usr/pkg/lib
endif
ifeq ($(OS),SunOS)
CFLAGS += -include solaris/epoll2event.h -Isolaris
LIBS += -levent -lsocket -lnsl -L/usr/local/lib
TGTADM_LIBS += -lsocket -lnsl
endif

sbindir ?= $(PREFIX)/sbin

ifneq ($(shell test -e /usr/include/linux/signalfd.h && echo 1),)
CFLAGS += -DUSE_SIGNALFD
endif

ifneq ($(shell test -e /usr/include/sys/timerfd.h && echo 1),)
CFLAGS += -DUSE_TIMERFD
endif

TGTD_OBJS += $(addprefix iscsi/, conn.o param.o session.o \
		iscsid.o target.o chap.o sha1.o md5.o transport.o iscsi_tcp.o \
		isns.o)

TGTD_OBJS += bs_rdwr.o
ifeq ($(OS),Linux)
TGTD_OBJS += bs_sg.o bs_aio.o
CFLAGS += -Wno-unused-but-set-variable
endif
ifeq ($(OS),FreeBSD)
TGTD_OBJS += bs_sg.o
endif
ifeq ($(OS),DragonFly)
TGTD_OBJS += bs_sg.o
endif

ifneq ($(ISCSI_RDMA),)
TGTD_OBJS += iscsi/iser.o iscsi/iser_text.o
LIBS += -libverbs -lrdmacm
endif

INCLUDES += -I.

CFLAGS += -D_GNU_SOURCE
CFLAGS += $(INCLUDES)
ifneq ($(DEBUG),)
CFLAGS += -g -O0 -ggdb -rdynamic
else
CFLAGS += -g -O2 -fno-strict-aliasing
endif
CFLAGS += -Wall -Wstrict-prototypes -fPIC
CFLAGS += -DTGT_VERSION=\"$(VERSION)$(EXTRAVERSION)\"

LIBS += -lpthread

PROGRAMS += tgtd tgtadm tgtimg
TGTD_OBJS += tgtd.o mgmt.o target.o scsi.o log.o driver.o util.o work.o \
		parser.o spc.o sbc.o mmc.o osd.o scc.o smc.o \
		ssc.o bs_ssc.o libssc.o \
		bs_null.o bs.o libcrc32c.o

TGTD_DEP = $(TGTD_OBJS:.o=.d)

.PHONY:all
all: $(PROGRAMS)

tgtd: $(TGTD_OBJS)
	$(CC) $^ -o $@ $(LIBS)

-include $(TGTD_DEP)

tgtadm: tgtadm.o
	$(CC) $^ -o $@ $(TGTADM_LIBS)

-include tgtadm.d

tgtimg: tgtimg.o libssc.o libcrc32c.o
	$(CC) $^ -o $@

-include tgtimg.d libssc.d

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c -o $*.o
	@$(CC) -MM $(CFLAGS) -MF $*.d -MT $*.o $*.c

.PHONY: install
install: $(PROGRAMS)
	install -d -m 755 $(DESTDIR)$(sbindir)
	install -m 755 $(PROGRAMS) $(DESTDIR)$(sbindir)

.PHONY: clean
clean:
	rm -f *.[od] $(PROGRAMS) iscsi/*.[od] ibmvio/*.[od] fc/*.[od]
