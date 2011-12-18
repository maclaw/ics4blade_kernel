/*
 *  linux/drivers/mmc/card/queue.c
 *
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/scatterlist.h>
#include <linux/delay.h>

#include <linux/mmc/mmc.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include "queue.h"

#define MMC_QUEUE_BOUNCESZ	65536

#define MMC_QUEUE_SUSPENDED	(1 << 0)

/*
 * Prepare a MMC request. This just filters out odd stuff.
 */
static int mmc_prep_request(struct request_queue *q, struct request *req)
{
	/*
	 * We only like normal block requests.
	 */
	if (!blk_fs_request(req)) {
		blk_dump_rq_flags(req, "MMC bad request");
		return BLKPREP_KILL;
	}

	req->cmd_flags |= REQ_DONTPREP;

	return BLKPREP_OK;
}
//ruanmeisi_20100603
int mmc_send_status(struct mmc_card *card, u32 *status);
	
int remove_all_req(struct mmc_queue *mq)
{
	int i = 0;
	struct request_queue *q = mq->queue;
	struct request *req = NULL;
	if (NULL == mq) {
		return 0;
	}
	spin_lock_irq(q->queue_lock);
	while ((req = blk_fetch_request(q)) != NULL) {
		int ret = 0;
		do {
			req->cmd_flags |= REQ_QUIET;
			ret = __blk_end_request(req, -EIO,
						blk_rq_cur_bytes(req));
		} while (ret);
		i ++;
	}
	spin_unlock_irq(q->queue_lock);

	printk(KERN_ERR"rms:%s %d req %d\n", __FUNCTION__, __LINE__, i);
	return i;
}
static int mmc_queue_thread(void *d)
{
	struct mmc_queue *mq = d;
	struct request_queue *q = mq->queue;
	struct request *req;
	//ruanmeisi_20100603
	int issue_ret = 0;

#ifdef CONFIG_MMC_PERF_PROFILING
	ktime_t start, diff;
	struct mmc_host *host = mq->card->host;
	unsigned long bytes_xfer;
#endif


	current->flags |= PF_MEMALLOC;

	down(&mq->thread_sem);
	do {
        req = NULL;

		//ruanmeisi_20100603
		if (kthread_should_stop()) {
			remove_all_req(mq);
			break;
		}
		//end
		spin_lock_irq(q->queue_lock);
		set_current_state(TASK_INTERRUPTIBLE);
		if (!blk_queue_plugged(q))
			req = blk_fetch_request(q);
		mq->req = req;
		spin_unlock_irq(q->queue_lock);

		if (!req) {
			if (kthread_should_stop()) {
				set_current_state(TASK_RUNNING);
				break;
			}
			up(&mq->thread_sem);
			schedule();
			down(&mq->thread_sem);
			continue;
		}
		set_current_state(TASK_RUNNING);
#ifdef CONFIG_MMC_AUTO_SUSPEND
		mmc_auto_suspend(mq->card->host, 0);
#endif
#ifdef CONFIG_MMC_BLOCK_PARANOID_RESUME
		if (mq->check_status) {
			struct mmc_command cmd;
			int retries = 3;

			do {
				int err;

				cmd.opcode = MMC_SEND_STATUS;
				cmd.arg = mq->card->rca << 16;
				cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

				mmc_claim_host(mq->card->host);
				err = mmc_wait_for_cmd(mq->card->host, &cmd, 5);
				mmc_release_host(mq->card->host);

				if (err) {
					printk(KERN_ERR "%s: failed to get status (%d)\n",
					       __func__, err);
					msleep(5);
					retries--;
					continue;
				}
				printk(KERN_DEBUG "%s: status 0x%.8x\n", __func__, cmd.resp[0]);
			} while (retries &&
				(!(cmd.resp[0] & R1_READY_FOR_DATA) ||
				(R1_CURRENT_STATE(cmd.resp[0]) == 7)));
			mq->check_status = 0;
                }
#endif
//ruanmeisi_20100529
	
		
		#ifdef CONFIG_MMC_PERF_PROFILING
		bytes_xfer = blk_rq_bytes(req);
		if (rq_data_dir(req) == READ) {
			start = ktime_get();
			issue_ret = mq->issue_fn(mq, req);
			diff = ktime_sub(ktime_get(), start);
			host->perf.rbytes_mmcq += bytes_xfer;
			host->perf.rtime_mmcq =
				ktime_add(host->perf.rtime_mmcq, diff);
		} else {
			start = ktime_get();
			issue_ret = mq->issue_fn(mq, req);
			diff = ktime_sub(ktime_get(), start);
			host->perf.wbytes_mmcq += bytes_xfer;
			host->perf.wtime_mmcq =
				ktime_add(host->perf.wtime_mmcq, diff);
		}
#else
				issue_ret = mq->issue_fn(mq, req);
#endif

		//ruanmeisi
		if (0 == issue_ret) {
			int err;
			mmc_claim_host(mq->card->host);
			err = mmc_send_status(mq->card, NULL);
			mmc_release_host(mq->card->host);
			if (err) {
				printk(KERN_ERR "rms:%s: failed to get status (%d) maybe the card is removed\n",
				       __func__, err);
				//sdcard is removed?
				mmc_detect_change(mq->card->host, 0);
				msleep(500);
				//set_current_state(TASK_INTERRUPTIBLE);
				//schedule_timeout(HZ / 2);
				continue;
			}
		}
	} while (1);
	up(&mq->thread_sem);

	return 0;
}

/*
 * Generic MMC request handler.  This is called for any queue on a
 * particular host.  When the host is not busy, we look for a request
 * on any queue on this host, and attempt to issue it.  This may
 * not be the queue we were asked to process.
 */
static void mmc_request(struct request_queue *q)
{
	struct mmc_queue *mq = q->queuedata;
	struct request *req;

	if (!mq) {
		while ((req = blk_fetch_request(q)) != NULL) {
			req->cmd_flags |= REQ_QUIET;
			__blk_end_request_all(req, -EIO);
		}
		return;
	}

	if (!mq->req)
		wake_up_process(mq->thread);
}

/**
 * mmc_init_queue - initialise a queue structure.
 * @mq: mmc queue
 * @card: mmc card to attach this queue
 * @lock: queue lock
 *
 * Initialise a MMC card request queue.
 */
int mmc_init_queue(struct mmc_queue *mq, struct mmc_card *card, spinlock_t *lock)
{
	struct mmc_host *host = card->host;
	u64 limit = BLK_BOUNCE_HIGH;
	int ret;

	if (mmc_dev(host)->dma_mask && *mmc_dev(host)->dma_mask)
		limit = *mmc_dev(host)->dma_mask;

	mq->card = card;
	mq->queue = blk_init_queue(mmc_request, lock);
	if (!mq->queue)
		return -ENOMEM;

	mq->queue->queuedata = mq;
	mq->req = NULL;

	blk_queue_prep_rq(mq->queue, mmc_prep_request);
	blk_queue_ordered(mq->queue, QUEUE_ORDERED_DRAIN, NULL);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, mq->queue);

#ifdef CONFIG_MMC_BLOCK_BOUNCE
	if (host->max_hw_segs == 1) {
		unsigned int bouncesz;

		bouncesz = MMC_QUEUE_BOUNCESZ;

		if (bouncesz > host->max_req_size)
			bouncesz = host->max_req_size;
		if (bouncesz > host->max_seg_size)
			bouncesz = host->max_seg_size;
		if (bouncesz > (host->max_blk_count * 512))
			bouncesz = host->max_blk_count * 512;

		if (bouncesz > 512) {
			mq->bounce_buf = kmalloc(bouncesz, GFP_KERNEL);
			if (!mq->bounce_buf) {
				printk(KERN_WARNING "%s: unable to "
					"allocate bounce buffer\n",
					mmc_card_name(card));
			}
		}

		if (mq->bounce_buf) {
			blk_queue_bounce_limit(mq->queue, BLK_BOUNCE_ANY);
			blk_queue_max_hw_sectors(mq->queue, bouncesz / 512);
			blk_queue_max_segments(mq->queue, bouncesz / 512);
			blk_queue_max_segment_size(mq->queue, bouncesz);

			mq->sg = kmalloc(sizeof(struct scatterlist),
				GFP_KERNEL);
			if (!mq->sg) {
				ret = -ENOMEM;
				goto cleanup_queue;
			}
			sg_init_table(mq->sg, 1);

			mq->bounce_sg = kmalloc(sizeof(struct scatterlist) *
				bouncesz / 512, GFP_KERNEL);
			if (!mq->bounce_sg) {
				ret = -ENOMEM;
				goto cleanup_queue;
			}
			sg_init_table(mq->bounce_sg, bouncesz / 512);
		}
	}
#endif

	if (!mq->bounce_buf) {
		blk_queue_bounce_limit(mq->queue, limit);
		blk_queue_max_hw_sectors(mq->queue,
			min(host->max_blk_count, host->max_req_size / 512));
		blk_queue_max_segments(mq->queue, host->max_hw_segs);
		blk_queue_max_segment_size(mq->queue, host->max_seg_size);

		mq->sg = kmalloc(sizeof(struct scatterlist) *
			host->max_phys_segs, GFP_KERNEL);
		if (!mq->sg) {
			ret = -ENOMEM;
			goto cleanup_queue;
		}
		sg_init_table(mq->sg, host->max_phys_segs);
	}

	init_MUTEX(&mq->thread_sem);

	mq->thread = kthread_run(mmc_queue_thread, mq, "mmcqd");
	if (IS_ERR(mq->thread)) {
		ret = PTR_ERR(mq->thread);
		goto free_bounce_sg;
	}

	return 0;
 free_bounce_sg:
 	if (mq->bounce_sg)
 		kfree(mq->bounce_sg);
 	mq->bounce_sg = NULL;
 cleanup_queue:
 	if (mq->sg)
		kfree(mq->sg);
	mq->sg = NULL;
	if (mq->bounce_buf)
		kfree(mq->bounce_buf);
	mq->bounce_buf = NULL;
	blk_cleanup_queue(mq->queue);
	return ret;
}

void mmc_cleanup_queue(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	/* Make sure the queue isn't suspended, as that will deadlock */
	mmc_queue_resume(mq);

	/* Then terminate our worker thread */
	kthread_stop(mq->thread);

	/* Empty the queue */
	spin_lock_irqsave(q->queue_lock, flags);
	q->queuedata = NULL;
	blk_start_queue(q);
	spin_unlock_irqrestore(q->queue_lock, flags);

 	if (mq->bounce_sg)
 		kfree(mq->bounce_sg);
 	mq->bounce_sg = NULL;

	kfree(mq->sg);
	mq->sg = NULL;

	if (mq->bounce_buf)
		kfree(mq->bounce_buf);
	mq->bounce_buf = NULL;

	mq->card = NULL;
}
EXPORT_SYMBOL(mmc_cleanup_queue);

/**
 * mmc_queue_suspend - suspend a MMC request queue
 * @mq: MMC queue to suspend
 *
 * Stop the block request queue, and wait for our thread to
 * complete any outstanding requests.  This ensures that we
 * won't suspend while a request is being processed.
 */
void mmc_queue_suspend(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	if (!(mq->flags & MMC_QUEUE_SUSPENDED)) {
		mq->flags |= MMC_QUEUE_SUSPENDED;

		spin_lock_irqsave(q->queue_lock, flags);
		blk_stop_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);

		down(&mq->thread_sem);
	}
}

/**
 * mmc_queue_resume - resume a previously suspended MMC request queue
 * @mq: MMC queue to resume
 */
void mmc_queue_resume(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	if (mq->flags & MMC_QUEUE_SUSPENDED) {
		mq->flags &= ~MMC_QUEUE_SUSPENDED;

		up(&mq->thread_sem);

		spin_lock_irqsave(q->queue_lock, flags);
		blk_start_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
}

/*
 * Prepare the sg list(s) to be handed of to the host driver
 */
unsigned int mmc_queue_map_sg(struct mmc_queue *mq)
{
	unsigned int sg_len;
	size_t buflen;
	struct scatterlist *sg;
	int i;

	if (!mq->bounce_buf)
		return blk_rq_map_sg(mq->queue, mq->req, mq->sg);

	BUG_ON(!mq->bounce_sg);

	sg_len = blk_rq_map_sg(mq->queue, mq->req, mq->bounce_sg);

	mq->bounce_sg_len = sg_len;

	buflen = 0;
	for_each_sg(mq->bounce_sg, sg, sg_len, i)
		buflen += sg->length;

	sg_init_one(mq->sg, mq->bounce_buf, buflen);

	return 1;
}

/*
 * If writing, bounce the data to the buffer before the request
 * is sent to the host driver
 */
void mmc_queue_bounce_pre(struct mmc_queue *mq)
{
	unsigned long flags;

	if (!mq->bounce_buf)
		return;

	if (rq_data_dir(mq->req) != WRITE)
		return;

	local_irq_save(flags);
	sg_copy_to_buffer(mq->bounce_sg, mq->bounce_sg_len,
		mq->bounce_buf, mq->sg[0].length);
	local_irq_restore(flags);
}

/*
 * If reading, bounce the data from the buffer after the request
 * has been handled by the host driver
 */
void mmc_queue_bounce_post(struct mmc_queue *mq)
{
	unsigned long flags;

	if (!mq->bounce_buf)
		return;

	if (rq_data_dir(mq->req) != READ)
		return;

	local_irq_save(flags);
	sg_copy_from_buffer(mq->bounce_sg, mq->bounce_sg_len,
		mq->bounce_buf, mq->sg[0].length);
	local_irq_restore(flags);
}
