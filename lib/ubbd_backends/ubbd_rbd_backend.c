#include <rados/librados.h>
#include <pthread.h>

#include "ubbd_backend.h"
#include "ubbd_uio.h"

// rbd ops
#define RBD_BACKEND(ubbd_b) ((struct ubbd_rbd_backend *)container_of(ubbd_b, struct ubbd_rbd_backend, ubbd_b))
static int rbd_backend_open(struct ubbd_backend *ubbd_b)
{
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);
        int err;

        err = rados_create2(&rbd_b->cluster, "ceph", "client.admin", rbd_b->flags);
        if (err < 0) {
                ubbd_err("Couldn't create the cluster handle! %s\n", strerror(-err));
                return err;
        } else {
                ubbd_info("\nCreated a cluster handle.\n");
        }

        /* Read a Ceph configuration file to configure the cluster handle. */
        err = rados_conf_read_file(rbd_b->cluster, ubbd_b->dev_info.rbd.ceph_conf);
        if (err < 0) {
                ubbd_err("cannot read config file: %s\n", strerror(-err));
		goto shutdown_cluster;
        } else {
                ubbd_info("\nRead the config file.\n");
        }

	rados_conf_set(rbd_b->cluster, "rbd_cache", "false");
        /* Connect to the cluster */
        err = rados_connect(rbd_b->cluster);
        if (err < 0) {
                ubbd_err("cannot connect to cluster: %s\n",  strerror(-err));
		goto shutdown_cluster;
        } else {
                ubbd_info("\nConnected to the cluster.\n");
        }

	err = rados_ioctx_create(rbd_b->cluster, rbd_b->pool, &rbd_b->io_ctx);
        if (err < 0) {
                ubbd_err("cannot create ioctx to %s pool: %s\n", rbd_b->pool, strerror(-err));
		goto shutdown_cluster;
        } else {
                ubbd_info("\nioctx created.\n");
        }

	err = rbd_open(rbd_b->io_ctx, rbd_b->imagename, &rbd_b->image, NULL);
        if (err < 0) {
                ubbd_err("cannot open image(%s): %s\n", rbd_b->imagename, strerror(-err));
		goto destroy_ioctx;
        } else {
                ubbd_info("\nimage opened.\n");
        }

	return 0;

destroy_ioctx:
	rados_ioctx_destroy(rbd_b->io_ctx);
shutdown_cluster:
	rados_shutdown(rbd_b->cluster);
	return err;
}

static void rbd_backend_close(struct ubbd_backend *ubbd_b)
{
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);

	rbd_close(rbd_b->image);
	rados_ioctx_destroy(rbd_b->io_ctx);
	rados_shutdown(rbd_b->cluster);
}

static void rbd_backend_release(struct ubbd_backend *ubbd_b)
{
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);

	free(rbd_b);
}

enum rbd_aio_type {
	RBD_AIO_TYPE_WRITE = 0,
	RBD_AIO_TYPE_READ,
	RBD_AIO_TYPE_FLUSH,
	RBD_AIO_TYPE_DISCARD,
	RBD_AIO_TYPE_WRITE_ZEROS,
};

struct rbd_aio_cb {
	enum rbd_aio_type type;
	struct ubbd_queue *ubbd_q;
	uint64_t priv_data;
	uint32_t len;
	struct iovec iovec[0];
};

static void rbd_finish_aio_generic(rbd_completion_t completion,
				   struct rbd_aio_cb *aio_cb)
{
	struct ubbd_queue *ubbd_q = aio_cb->ubbd_q;
	int64_t ret;

	ubbd_dbg("into finish op \n");
	ret = rbd_aio_get_return_value(completion);
	ubbd_dbg("ret: %ld\n", ret);

	if (aio_cb->type == RBD_AIO_TYPE_READ ||
			aio_cb->type == RBD_AIO_TYPE_WRITE)
		ret = (ret == aio_cb->len? 0 : ret);

	free(aio_cb);
	rbd_aio_release(completion);

	ubbd_queue_add_ce(ubbd_q, aio_cb->priv_data, ret);
}

static int rbd_backend_writev(struct ubbd_queue *ubbd_q, struct ubbd_se *se)
{
	struct ubbd_backend *ubbd_b = ubbd_q->ubbd_b;
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);
	struct rbd_aio_cb *aio_cb;
	struct iovec *iov;
	rbd_completion_t completion;
	ssize_t ret;
	int i;

	aio_cb = calloc(1, sizeof(*aio_cb) + sizeof(struct iovec) * se->iov_cnt);
	if (!aio_cb) {
		ubbd_err("Could not allocate aio_cb.\n");
		return -1;
	}

	iov = aio_cb->iovec;
	for (i = 0; i < se->iov_cnt; i++) {
		ubbd_dbg("iov_base: %lu", (size_t)se->iov[i].iov_base);
		iov[i].iov_base = (void*)ubbd_q->uio_info.map + (size_t)se->iov[i].iov_base;
		iov[i].iov_len = se->iov[i].iov_len;
	}

	aio_cb->type = RBD_AIO_TYPE_WRITE;
	aio_cb->priv_data = se->priv_data;
	aio_cb->len = se->len;
	aio_cb->ubbd_q = ubbd_q;

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		ubbd_err("create completion failed\n");
		return -1;
	}
	ubbd_dbg("writev");
	ret = rbd_aio_writev(rbd_b->image, iov, se->iov_cnt, se->offset, completion);
	return ret;
}

static int rbd_backend_readv(struct ubbd_queue *ubbd_q, struct ubbd_se *se)
{
	struct ubbd_backend *ubbd_b = ubbd_q->ubbd_b;
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);
	struct rbd_aio_cb *aio_cb;
	struct iovec *iov;
	rbd_completion_t completion;
	ssize_t ret;
	int i;

	aio_cb = calloc(1, sizeof(*aio_cb) + sizeof(struct iovec) * se->iov_cnt);
	if (!aio_cb) {
		ubbd_err("Could not allocate aio_cb.\n");
		return -1;
	}

	aio_cb->type = RBD_AIO_TYPE_READ;
	aio_cb->priv_data = se->priv_data;
	aio_cb->len = se->len;
	aio_cb->ubbd_q = ubbd_q;

	iov = aio_cb->iovec;
	for (i = 0; i < se->iov_cnt; i++) {
		ubbd_dbg("iov_base: %lu", (size_t)se->iov[i].iov_base);
		iov[i].iov_base = (void*)ubbd_q->uio_info.map + (size_t)se->iov[i].iov_base;
		iov[i].iov_len = se->iov[i].iov_len;
	}

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		return -1;
	}
	ubbd_dbg("readv");

	ret = rbd_aio_readv(rbd_b->image, iov, se->iov_cnt, se->offset, completion);
	ubbd_dbg("after wait\n");
	return ret;
}

static int rbd_backend_flush(struct ubbd_queue *ubbd_q, struct ubbd_se *se)
{
	struct ubbd_backend *ubbd_b = ubbd_q->ubbd_b;
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb) + sizeof(struct iovec) * se->iov_cnt);
	if (!aio_cb) {
		ubbd_err("Could not allocate aio_cb.\n");
		return -1;
	}

	aio_cb->type = RBD_AIO_TYPE_FLUSH;
	aio_cb->priv_data = se->priv_data;
	aio_cb->len = se->len;
	aio_cb->ubbd_q = ubbd_q;

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		return -1;
	}
	ubbd_dbg("flush");

	ret = rbd_aio_flush(rbd_b->image, completion);
	ubbd_dbg("after wait\n");
	return ret;
}

static int rbd_backend_discard(struct ubbd_queue *ubbd_q, struct ubbd_se *se)
{
	struct ubbd_backend *ubbd_b = ubbd_q->ubbd_b;
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb) + sizeof(struct iovec) * se->iov_cnt);
	if (!aio_cb) {
		ubbd_err("Could not allocate aio_cb.\n");
		return -1;
	}

	aio_cb->type = RBD_AIO_TYPE_DISCARD;
	aio_cb->priv_data = se->priv_data;
	aio_cb->len = se->len;
	aio_cb->ubbd_q = ubbd_q;

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		return -1;
	}
	ubbd_dbg("discard");

	ret = rbd_aio_discard(rbd_b->image, se->offset, se->len, completion);
	ubbd_dbg("after wait\n");
	return ret;
}

#ifdef LIBRBD_SUPPORTS_WRITE_ZEROES
static int rbd_backend_write_zeros(struct ubbd_queue *ubbd_q, struct ubbd_se *se)
{
	struct ubbd_backend *ubbd_b = ubbd_q->ubbd_b;
	struct ubbd_rbd_backend *rbd_b = RBD_BACKEND(ubbd_b);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb) + sizeof(struct iovec) * se->iov_cnt);
	if (!aio_cb) {
		ubbd_err("Could not allocate aio_cb.\n");
		return -1;
	}

	aio_cb->type = RBD_AIO_TYPE_WRITE_ZEROS;
	aio_cb->priv_data = se->priv_data;
	aio_cb->len = se->len;
	aio_cb->ubbd_q = ubbd_q;

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		return -1;
	}
	ubbd_dbg("write_zeros");

	ret = rbd_aio_write_zeroes(rbd_b->image, se->offset, se->len, completion, 0, 0);
	ubbd_dbg("after wait\n");
	return ret;
}
#else
static int rbd_backend_write_zeros(struct ubbd_queue *ubbd_q, struct ubbd_se *se)
{
	ubbd_err("write_zeros is not supported");

	return -1;
}
#endif

struct ubbd_backend_ops rbd_backend_ops = {
	.open = rbd_backend_open,
	.close = rbd_backend_close,
	.release = rbd_backend_release,
	.writev = rbd_backend_writev,
	.readv = rbd_backend_readv,
	.flush = rbd_backend_flush,
	.discard = rbd_backend_discard,
	.write_zeros = rbd_backend_write_zeros,
};


