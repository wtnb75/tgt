// POSIX AIO backing store

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/epoll.h>

#include <aio.h>

#include "list.h"
#include "util.h"
#include "tgtd.h"
#include "scsi.h"

struct bs_paio_info {
	int efd[2];
};

static void bs_paio_endio(int fd, int events, void *data)
{
	struct bs_paio_info *info = data;
	int ret;
	while(1) {
		struct scsi_cmd *cmd;
		ret=read(info->efd[0], &cmd, sizeof(cmd));
		if(ret!=sizeof(cmd)){ break; }
		int result=SAM_STAT_GOOD;

		target_cmd_io_done(cmd, result);
	}
}

static int bs_paio_open(struct scsi_lu *lu, char *path, int *fd, uint64_t *size)
{
	*fd = backed_file_open(path, O_RDWR|O_LARGEFILE|O_DIRECT, size);
	/* If we get access denied, try opening the file in readonly mode */
	if (*fd == -1 && (errno == EACCES || errno == EROFS)) {
		*fd = backed_file_open(path, O_RDONLY|O_LARGEFILE|O_DIRECT,
				       size);
		lu->attrs.readonly = 1;
	}
	if (*fd < 0)
		return *fd;
	return 0;
}

static int bs_paio_init(struct scsi_lu *lu)
{
	int ret;
	struct bs_paio_info *info =
		(struct bs_paio_info *) ((char *)lu + sizeof(*lu));

	ret=pipe(info->efd);
	if(ret){
		eprintf("fail to create pipe, %m\n");
		goto err_exit;
	}
	ret=fcntl(info->efd[0], F_SETFL,
		  fcntl(info->efd[0], F_GETFL, 0)|O_NONBLOCK);
	if(ret){
		eprintf("fail to confiture pipe, %m\n");
		goto close_pipe;
	}
	ret=fcntl(info->efd[1], F_SETFL,
		  fcntl(info->efd[1], F_GETFL, 0)|O_NONBLOCK);
	if(ret){
		eprintf("fail to confiture pipe, %m\n");
		goto close_pipe;
	}

	ret = tgt_event_add(info->efd[0], EPOLLIN, bs_paio_endio, info);
	if (ret)
		goto close_pipe;

	return 0;

close_pipe:
	close(info->efd[0]);
	close(info->efd[1]);
err_exit:
	return -1;
}

static void bs_paio_close(struct scsi_lu *lu)
{
	close(lu->fd);
}

static void bs_paio_exit(struct scsi_lu *lu)
{
	struct bs_paio_info *info =
		(struct bs_paio_info *) ((char *)lu + sizeof(*lu));

	close(info->efd[0]);
	close(info->efd[1]);
}

void paio_done(union sigval sv)
{
	struct aiocb *io=sv.sival_ptr;
	struct scsi_cmd *cmd=*(struct scsi_cmd **)(io+1);
	struct scsi_lu *lu=cmd->dev;
	struct bs_paio_info *info=(void *)(lu+1);
	write(info->efd[1], &cmd, sizeof(cmd));
	free(io);
}

static int bs_paio_cmd_submit(struct scsi_cmd *cmd)
{
	struct scsi_lu *lu = cmd->dev;
	struct aiocb *io=NULL;
	int ret = 0;

	switch (cmd->scb[0]) {
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		break;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		io=calloc(1, sizeof(*io)+sizeof(cmd));
		io->aio_fildes=lu->fd;
		io->aio_buf=scsi_get_out_buffer(cmd);
		io->aio_offset=cmd->offset;
		io->aio_nbytes=scsi_get_out_length(cmd);
		io->aio_sigevent.sigev_notify=SIGEV_THREAD;
		io->aio_sigevent.sigev_notify_function=paio_done;
		io->aio_sigevent.sigev_notify_attributes=NULL;
		io->aio_sigevent.sigev_value.sival_ptr=io;
		*(struct scsi_cmd **)(io+1)=cmd;
		ret=aio_write(io);
		if(ret==0){
			set_cmd_async(cmd);
			return 0;
		}
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		io=calloc(1, sizeof(*io)+sizeof(cmd));
		io->aio_fildes=lu->fd;
		io->aio_buf=scsi_get_in_buffer(cmd);
		io->aio_offset=cmd->offset;
		io->aio_nbytes=scsi_get_in_length(cmd);
		io->aio_sigevent.sigev_notify=SIGEV_THREAD;
		io->aio_sigevent.sigev_notify_function=paio_done;
		io->aio_sigevent.sigev_notify_attributes=NULL;
		io->aio_sigevent.sigev_value.sival_ptr=io;
		*(struct scsi_cmd **)(io+1)=cmd;
		ret=aio_read(io);
		if(ret==0){
			set_cmd_async(cmd);
			return 0;
		}
		break;
	default:
		break;
	}
	if(ret!=0){
		sense_data_build(cmd, MEDIUM_ERROR, 0);
		ret = SAM_STAT_CHECK_CONDITION;
	}

	return ret;
}

static struct backingstore_template paio_bst = {
	.bs_name		= "paio",
	.bs_datasize		= sizeof(struct bs_paio_info),
	.bs_init		= bs_paio_init,
	.bs_exit		= bs_paio_exit,
	.bs_open		= bs_paio_open,
	.bs_close		= bs_paio_close,
	.bs_cmd_submit		= bs_paio_cmd_submit,
};

__attribute__((constructor)) static void bs_paio_constructor(void)
{
	register_backingstore_template(&paio_bst);
}

/*
 * Local Variables:
 * c-file-style: "linux"
 * End:
 */
