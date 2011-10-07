// nbd client backing store
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <string.h>
#include <sys/epoll.h>
#include <pthread.h>

#include "list.h"
#include "util.h"
#include "tgtd.h"
#include "scsi.h"

#define INIT_PASS 0x00420281861253LL

struct nbdreq{
#define NBD_REQUEST_MAGIC 0x25609513
	uint32_t magic;  // 0x25609513
	uint32_t cmd;    // 0=READ, 1=WRITE, 2=DISCONNECT
#define NBD_READ       0
#define NBD_WRITE      1
#define NBD_DISCONNECT 2
	uint64_t hdl;
	uint64_t off;
	uint32_t len;
} __attribute__((packed));

struct nbdres{
#define NBD_RESPONSE_MAGIC 0x67446698
	uint32_t magic;  // 0x67446698
	uint32_t err;    // 0=OK, other=err
	uint64_t hdl;
} __attribute__((packed));

struct nbdcmd{
	struct list_head list;
	struct nbdreq req;
	struct nbdres res;
	void *data;
	struct scsi_cmd *cmd;
};

struct nbdconn{
	int fd;
	char *path;
	pthread_mutex_t lock;
	loff_t capacity;
	struct list_head cmds;
};

static void nbdconn_init(struct nbdconn *c, char *path)
{
	c->path=strdup(path);
	pthread_mutex_init(&c->lock, NULL);
	INIT_LIST_HEAD(&c->cmds);
}

static int connect_in(char *path)
{
	int fd=-1;
	char *addr=strdup(path);
	char *port=strrchr(addr, '@');
	if(port==NULL){
		port=strrchr(addr, ':');
	}
	if(port==NULL){
		eprintf("no port number\n");
		free(addr);
		return -1;
	}
	*port=0;
	port++;
	struct addrinfo *res, *rp;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family=AF_UNSPEC;
	hints.ai_socktype=SOCK_STREAM;
	int err=getaddrinfo(addr, port, &hints, &res);
	free(addr);
	if(err){
		eprintf("getaddrinfo: %s\n", gai_strerror(err));
		return -1;
	}
	for(rp=res; rp!=NULL; rp=rp->ai_next){
		fd=socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(fd==-1){
			eprintf("socket: %m\n");
			continue;
		}
		if(connect(fd, rp->ai_addr, rp->ai_addrlen)!=-1){
			break;
		}
		close(fd);
	}
	if(rp==NULL){
		eprintf("can't connect to %s\n", path);
		freeaddrinfo(res);
		return -1;
	}
	freeaddrinfo(res);
	return fd;
}

static int connect_un(char *path)
{
	// unix domain socket
	int fd=socket(AF_UNIX, SOCK_STREAM, 0);
	if(fd==-1){
		eprintf("socket: %m\n");
		return -1;
	}
	struct sockaddr_un unaddr;
	unaddr.sun_family=AF_UNIX;
	strncpy(unaddr.sun_path, path, sizeof(unaddr.sun_path));
	if(connect(fd, (struct sockaddr *)&unaddr, sizeof(unaddr))==-1){
		close(fd);
		return -1;
	}
	return fd;
}

static int nbdconn_connect(struct nbdconn *c)
{
	if(c->path[0]=='/'){
		c->fd=connect_un(c->path);
	}else{
		c->fd=connect_in(c->path);
	}
	// recv nego
	char buf[8+8+8+4+124];
	int err=recv(c->fd, buf, sizeof(buf), MSG_WAITALL);
	if(err!=sizeof(buf)){
		eprintf("can't recv nego(%d): %m\n", err);
		goto err_exit;
	}
	if(strncmp("NBDMAGIC", buf, 8)!=0){
		eprintf("magic mismatch\n");
		goto err_exit;
	}
	if(INIT_PASS!=__be64_to_cpu(*(uint64_t *)&buf[8])){
		eprintf("passwd mismatch\n");
		goto err_exit;
	}
	c->capacity=__be64_to_cpu(*(uint64_t *)&buf[16]);
	eprintf("connect success\n");
	return 0;
err_exit:
	if(c->fd){ close(c->fd); }
	return -1;
}

static void nbdconn_close(struct nbdconn *c)
{
	// send disconnect request, close
	struct nbdreq req;
	memset(&req, 0, sizeof(req));
	req.magic=__cpu_to_be32(NBD_REQUEST_MAGIC);
	req.cmd=__cpu_to_be32(NBD_DISCONNECT);
	pthread_mutex_lock(&c->lock);
	send(c->fd, &req, sizeof(req), 0);
	close(c->fd);
	pthread_mutex_unlock(&c->lock);
	pthread_mutex_destroy(&c->lock);
	free(c->path);
}

static void nbdconn_reconnect(struct nbdconn *c)
{
	struct nbdreq req;
	memset(&req, 0, sizeof(req));
	req.magic=__cpu_to_be32(NBD_REQUEST_MAGIC);
	req.cmd=__cpu_to_be32(NBD_DISCONNECT);
	pthread_mutex_lock(&c->lock);
	send(c->fd, &req, sizeof(req), 0);
	close(c->fd);
	pthread_mutex_unlock(&c->lock);
	nbdconn_connect(c);
}

static void bs_nbd_endio(int df, int events, void *data)
{
	struct nbdconn *c=data;
	dprintf("endio: conn=%p\n", c);
	// recv header
	struct nbdres res;
	int r;
	r=recv(c->fd, &res, sizeof(res), MSG_WAITALL);
	if(r!=sizeof(res)){
		eprintf("recv hdr error: %d, %m\n", r);
		goto err_out;
	}
	if(res.magic!=__cpu_to_be32(NBD_RESPONSE_MAGIC)){
		eprintf("invalid magic\n");
		goto err_out;
	}
	struct nbdcmd *nc;
	nc=(struct nbdcmd *)(res.hdl);
	if(nc->req.magic!=__cpu_to_be32(NBD_REQUEST_MAGIC) ||
	   nc->req.hdl!=(uint64_t)nc ||
	   (nc->req.cmd!=__cpu_to_be32(NBD_READ) && nc->req.cmd!=__cpu_to_be32(NBD_WRITE))){
		eprintf("invalid handle\n");
		goto err_out;
	}
	memcpy(&nc->res, &res, sizeof(res));
	if(nc->req.cmd==__cpu_to_be32(NBD_READ)){
		int len=__cpu_to_be32(nc->req.len);
		int cur=0;
		dprintf("read response: %d\n", len);
		while(len-cur!=0){
			r=recv(c->fd, nc->data+cur, len-cur, MSG_WAITALL);
			if(r>0){
				cur+=r;
			}else{
				eprintf("recv body error: %d, %m", r);
				goto err_out;
			}
		}
	}
	if(nc->res.err==0){
		target_cmd_io_done(nc->cmd, SAM_STAT_GOOD);
	}else{
		sense_data_build(nc->cmd, MEDIUM_ERROR, ASC_WRITE_ERROR);
		target_cmd_io_done(nc->cmd, SAM_STAT_CHECK_CONDITION);
	}
	free(nc);
	return;
err_out:
	tgt_event_del(c->fd);
	nbdconn_reconnect(c);
	tgt_event_add(c->fd, EPOLLIN|EPOLLERR, bs_nbd_endio, c);
}

static int bs_nbd_cmd_submit(struct scsi_cmd *cmd)
{
	struct scsi_lu *lu=cmd->dev;
	struct nbdconn *c=(struct nbdconn *)(lu+1);
	dprintf("submit: %p, %#x\n", c, cmd->scb[0]);
	switch(cmd->scb[0]){
	case WRITE_6: case WRITE_10: case WRITE_12: case WRITE_16:
	case READ_6: case READ_10: case READ_12: case READ_16:
		break;
	case SYNCHRONIZE_CACHE: case SYNCHRONIZE_CACHE_16:
		return SAM_STAT_GOOD;
	default:
		sense_data_build(cmd, ILLEGAL_REQUEST, ASC_INVALID_OP_CODE);
		return SAM_STAT_CHECK_CONDITION;
	}
	struct nbdcmd *nc=calloc(1, sizeof(*nc));
	nc->cmd=cmd;
	nc->req.magic=__cpu_to_be32(NBD_REQUEST_MAGIC);
	nc->req.hdl=(uint64_t)nc;
	nc->req.off=__cpu_to_be64(cmd->offset);
	switch(cmd->scb[0]){
	case WRITE_6: case WRITE_10: case WRITE_12: case WRITE_16:
		nc->req.cmd=__cpu_to_be32(NBD_WRITE);
		nc->req.len=__cpu_to_be32(scsi_get_out_length(cmd));
		nc->data=scsi_get_out_buffer(cmd);
		break;
	case READ_6: case READ_10: case READ_12: case READ_16:
		nc->req.cmd=__cpu_to_be32(NBD_READ);
		nc->req.len=__cpu_to_be32(scsi_get_in_length(cmd));
		nc->data=scsi_get_in_buffer(cmd);
		break;
	}
	set_cmd_async(cmd);
	pthread_mutex_lock(&c->lock);
	int r=send(c->fd, &nc->req, sizeof(nc->req),
		   (nc->req.cmd==__cpu_to_be32(NBD_WRITE)?MSG_MORE:0));
	if(r!=sizeof(nc->req)){
		eprintf("send1 err?: %d, %m\n", r);
		pthread_mutex_unlock(&c->lock);
		clear_cmd_async(cmd);
		sense_data_build(cmd, MEDIUM_ERROR, ASC_WRITE_ERROR);
		return SAM_STAT_CHECK_CONDITION;
	}
	if(nc->req.cmd==__cpu_to_be32(NBD_WRITE)){
		r=send(c->fd, nc->data, scsi_get_out_length(cmd), 0);
		if(r!=scsi_get_out_length(cmd)){
			pthread_mutex_unlock(&c->lock);
			clear_cmd_async(cmd);
			eprintf("send2 err?: %d, %m\n", r);
			sense_data_build(cmd, MEDIUM_ERROR, ASC_WRITE_ERROR);
			return SAM_STAT_CHECK_CONDITION;
		}
	}
	pthread_mutex_unlock(&c->lock);
	return 0;
}

static void bs_nbd_close(struct scsi_lu *lu)
{
	struct nbdconn *c=(struct nbdconn *)(lu+1);
	dprintf("closing: %p\n", c);
	tgt_event_del(c->fd);
	nbdconn_close(c);
}

static int bs_nbd_open(struct scsi_lu *lu, char *path, int *fd, uint64_t *size)
{
	struct nbdconn *c=(struct nbdconn *)(lu+1);
	dprintf("opening: %p\n", c);
	nbdconn_init(c, path);
	if(nbdconn_connect(c)!=0){
		nbdconn_close(c);
		return -1;
	}
	*fd=c->fd;
	*size=c->capacity;
	if(tgt_event_add(c->fd, EPOLLIN|EPOLLERR, bs_nbd_endio, c)){
		nbdconn_close(c);
		return -1;
	}
	return 0;
}

static int bs_nbd_init(struct scsi_lu *lu)
{
	dprintf("init: %p\n", lu);
	return 0;
}
static void bs_nbd_exit(struct scsi_lu *lu)
{
	dprintf("exit: %p\n", lu);
}

static struct backingstore_template nbd_bst = {
	.bs_name	= "nbd",
	.bs_datasize	= sizeof(struct nbdconn),
	.bs_init	= bs_nbd_init,
	.bs_exit	= bs_nbd_exit,
	.bs_open	= bs_nbd_open,
	.bs_close	= bs_nbd_close,
	.bs_cmd_submit	= bs_nbd_cmd_submit,
};

__attribute__((constructor)) static void bs_nbd_constructor(void)
{
	register_backingstore_template(&nbd_bst);
}

/*
 * Local Variables:
 * c-file-style: "linux"
 * End:
 */
