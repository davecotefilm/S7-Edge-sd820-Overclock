/*
 * snd-dbmdx-pcm.c -- DVF99 DBMDX ASoC platform driver
 *
 *  Copyright (C) 2014 DSPG Technologies GmbH
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define DEBUG

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#endif
#include <linux/dma-mapping.h>
#include "dbmdx-interface.h"

#define DRV_NAME "dbmdx-snd-soc-platform"

/* defaults */
/* must be a multiple of 4 */
#define MAX_BUFFER_SIZE		(131072*4) /* 3 seconds for each channel */
#define MIN_PERIOD_SIZE		4096
#define MAX_PERIOD_SIZE		(MAX_BUFFER_SIZE / 64)
#define USE_FORMATS		(SNDRV_PCM_FMTBIT_S16_LE)
#define USE_RATE		(SNDRV_PCM_RATE_16000 |	\
				SNDRV_PCM_RATE_32000 |	\
				SNDRV_PCM_RATE_48000)
#define USE_RATE_MIN		16000
#define USE_RATE_MAX		48000
#define USE_CHANNELS_MIN	1
#define USE_CHANNELS_MAX	2
#define USE_PERIODS_MIN		1
#define USE_PERIODS_MAX		1024
/* 3 seconds + 4 bytes for position */
#define REAL_BUFFER_SIZE	(MAX_BUFFER_SIZE + 4)

struct snd_dbmdx {
	struct snd_soc_card *card;
	struct snd_pcm_hardware pcm_hw;
};

struct snd_dbmdx_runtime_data {
	struct snd_pcm_substream *substream;
	struct timer_list *timer;
	bool timer_is_active;
	struct work_struct pcm_start_capture_work;
	struct work_struct pcm_stop_capture_work;
	unsigned int capture_in_progress;
};

static struct snd_pcm_hardware dbmdx_pcm_hardware = {
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 SNDRV_PCM_INFO_BATCH),
	.formats =		USE_FORMATS,
	.rates =		USE_RATE,
	.rate_min =		USE_RATE_MIN,
	.rate_max =		USE_RATE_MAX,
	.channels_min =		USE_CHANNELS_MIN,
	.channels_max =		USE_CHANNELS_MAX,
	.buffer_bytes_max =	MAX_BUFFER_SIZE,
	.period_bytes_min =	MIN_PERIOD_SIZE,
	.period_bytes_max =	MAX_PERIOD_SIZE,
	.periods_min =		USE_PERIODS_MIN,
	.periods_max =		USE_PERIODS_MAX,
	.fifo_size =		0,
};

u32 stream_get_position(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	/* pr_debug("%s\n", __func__); */

	if (runtime == NULL) {
		pr_err("%s: NULL ptr runtime\n", __func__);
		return 0;
	}

	return *(u32 *)&(runtime->dma_area[MAX_BUFFER_SIZE]);
}

void stream_set_position(struct snd_pcm_substream *substream,
				u32 position)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	/* pr_debug("%s\n", __func__); */

	if (runtime == NULL) {
		pr_err("%s: NULL ptr runtime\n", __func__);
		return;
	}

	*(u32 *)&(runtime->dma_area[MAX_BUFFER_SIZE]) = position;
}

static void dbmdx_pcm_timer(unsigned long _substream)
{
	struct snd_pcm_substream *substream =
				(struct snd_pcm_substream *)_substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dbmdx_runtime_data *prtd = runtime->private_data;
	struct timer_list *timer = prtd->timer;
	unsigned int size = snd_pcm_lib_buffer_bytes(substream);
	u32 pos;
	unsigned long msecs;
	unsigned long to_copy;

	msecs = (runtime->period_size * 1000) / runtime->rate;
	mod_timer(timer, jiffies + msecs_to_jiffies(msecs));
	/* pr_debug("%s\n", __func__); */


	pos = stream_get_position(substream);
	to_copy = frames_to_bytes(runtime, runtime->period_size);

	if (dbmdx_get_samples(runtime->dma_area + pos,
		runtime->channels * runtime->period_size)) {
		memset(runtime->dma_area + pos, 0, to_copy);
		pr_debug("%s Inserting %d bytes of silence\n",
			__func__, (int)to_copy);
	}

	pos += to_copy;
	if (pos >= size)
		pos = 0;

	stream_set_position(substream, pos);

	snd_pcm_period_elapsed(substream);

}

static int dbmdx_pcm_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;


	pr_debug("%s\n", __func__);

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	runtime->channels    = params_channels(hw_params);
	runtime->dma_bytes   = params_buffer_bytes(hw_params);
	runtime->buffer_size = params_buffer_size(hw_params);
	runtime->rate = params_rate(hw_params);

	return 0;
}

static int dbmdx_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t	buf_bytes;
	size_t	period_bytes;

	pr_debug("%s\n", __func__);

	memset(runtime->dma_area, 0, REAL_BUFFER_SIZE);

	buf_bytes = snd_pcm_lib_buffer_bytes(substream);
	period_bytes = snd_pcm_lib_period_bytes(substream);

	pr_debug("%s - buffer size =%d period size = %d\n",
		       __func__, (int)buf_bytes, (int)period_bytes);

	/* We only support buffers that are multiples of the period */
	if (buf_bytes % period_bytes) {
		pr_err("%s - buffer=%d not multiple of period=%d\n",
		       __func__, (int)buf_bytes, (int)period_bytes);
		return -EINVAL;
	}

	return 0;
}

static int dbmdx_start_period_timer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dbmdx_runtime_data *prtd = runtime->private_data;
	struct timer_list *timer = prtd->timer;
	unsigned long msecs;

	pr_debug("%s\n", __func__);
	prtd->timer_is_active = true;

	*(u32 *)&(runtime->dma_area[MAX_BUFFER_SIZE]) = 0;
	msecs = (runtime->period_size * 500) / runtime->rate;
	mod_timer(timer, jiffies + msecs_to_jiffies(msecs));

	return 0;
}

static int dbmdx_stop_period_timer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dbmdx_runtime_data *prtd = runtime->private_data;
	struct timer_list *timer = prtd->timer;

	pr_debug("%s\n", __func__);


	del_timer_sync(timer);

	prtd->timer_is_active = false;

	return 0;
}


int dbmdx_set_pcm_timer_mode(struct snd_pcm_substream *substream,
				bool enable_timer)
{
	int ret;
	struct snd_pcm_runtime *runtime;
	struct snd_dbmdx_runtime_data *prtd;

	if (!substream) {
			pr_debug("%s:Substream is NULL\n", __func__);
			return -1;
	}

	runtime = substream->runtime;

	if (!runtime) {
			pr_debug("%s:Runtime is NULL\n", __func__);
			return -1;
	}

	prtd = runtime->private_data;

	if (!prtd) {
			pr_debug("%s:Runtime Pr. Data is NULL\n", __func__);
			return -1;
	}

	if (enable_timer) {
		if (!(prtd->capture_in_progress)) {
			pr_debug("%s:Capture is not in progress\n", __func__);
			return -1;
		}

		if (prtd->timer_is_active) {
			pr_debug("%s:Timer is active\n", __func__);
			return 0;
		}

		ret = dbmdx_start_period_timer(substream);
		if (ret < 0) {
			pr_err("%s: failed to start capture device\n",
				__func__);
			return -3;
		}
	} else {
		if (!(prtd->timer_is_active)) {
			pr_debug("%s:Timer is not active\n", __func__);
			return 0;
		}

		ret = dbmdx_stop_period_timer(substream);
		if (ret < 0) {
			pr_err("%s: failed to stop capture device\n", __func__);
			return -2;
		}

	}

	return 0;
}


static void  dbmdx_pcm_start_capture_work(struct work_struct *work)
{
	int ret;
	struct snd_dbmdx_runtime_data *prtd = container_of(
			work, struct snd_dbmdx_runtime_data,
			pcm_start_capture_work);
	struct snd_pcm_substream *substream = prtd->substream;

	pr_debug("%s:\n", __func__);

	flush_work(&prtd->pcm_stop_capture_work);

	if (prtd->capture_in_progress) {
		pr_debug("%s:Capture is already in progress\n", __func__);
		return;
	}

	prtd->capture_in_progress = 1;

	ret = dbmdx_start_pcm_streaming(substream);
	if (ret < 0) {
		prtd->capture_in_progress = 0;
		pr_err("%s: failed to start capture device\n", __func__);
		return;
	}
#if 1
	msleep(DBMDX_MSLEEP_PCM_STREAMING_WORK);
#endif
#if 0
	ret = dbmdx_start_period_timer(substream);
	if (ret < 0) {
		pr_err("%s: failed to start capture device\n", __func__);
		prtd->capture_in_progress = 0;
		dbmdx_stop_pcm_streaming();
		return;
	}
#endif
	return;
}

static void dbmdx_pcm_stop_capture_work(struct work_struct *work)
{
	int ret;
	struct snd_dbmdx_runtime_data *prtd = container_of(
			work, struct snd_dbmdx_runtime_data,
			pcm_stop_capture_work);
	struct snd_pcm_substream *substream = prtd->substream;

	pr_debug("%s:\n", __func__);

	flush_work(&prtd->pcm_start_capture_work);

	if (!(prtd->capture_in_progress)) {
		pr_debug("%s:Capture is not in progress\n", __func__);
		return;
	}

	ret = dbmdx_stop_pcm_streaming();
	if (ret < 0)
		pr_err("%s: failed to stop pcm streaming\n", __func__);

	if (prtd->timer_is_active) {

		ret = dbmdx_stop_period_timer(substream);
		if (ret < 0)
			pr_err("%s: failed to stop timer\n", __func__);
	}

	prtd->capture_in_progress = 0;

	return;
}

static int dbmdx_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dbmdx_runtime_data *prtd;
	struct timer_list *timer;
	int ret;

	pr_debug("%s\n", __func__);

	if (dbmdx_codec_lock()) {
		ret = -EBUSY;
		goto out;
	}

	prtd = kzalloc(sizeof(struct snd_dbmdx_runtime_data), GFP_KERNEL);
	if (prtd == NULL) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	timer = kzalloc(sizeof(*timer), GFP_KERNEL);
	if (!timer) {
		ret = -ENOMEM;
		goto out_free_prtd;
	}

	init_timer(timer);
	timer->function = dbmdx_pcm_timer;
	timer->data = (unsigned long)substream;

	prtd->timer = timer;
	prtd->substream = substream;

	INIT_WORK(&prtd->pcm_start_capture_work, dbmdx_pcm_start_capture_work);
	INIT_WORK(&prtd->pcm_stop_capture_work, dbmdx_pcm_stop_capture_work);

	runtime->private_data = prtd;

	snd_soc_set_runtime_hwparams(substream, &dbmdx_pcm_hardware);

	ret = snd_pcm_hw_constraint_integer(runtime,
					SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		pr_debug("%s Error setting pcm constraint int\n", __func__);

	return 0;

out_free_prtd:
	kfree(prtd);
out_unlock:
	dbmdx_codec_unlock();
out:
	return ret;
}

static int dbmdx_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dbmdx_runtime_data *prtd;
	int ret = 0;

	pr_debug("%s: cmd=%d\n", __func__, cmd);

	if (runtime == NULL) {
		pr_err("%s: runtime NULL ptr\n", __func__);
		return -EFAULT;
	}

	prtd = runtime->private_data;

	if (prtd == NULL) {
		pr_err("%s: prtd NULL ptr\n", __func__);
		return -EFAULT;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		schedule_work(&prtd->pcm_start_capture_work);
		break;
	case SNDRV_PCM_TRIGGER_RESUME:
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		schedule_work(&prtd->pcm_stop_capture_work);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return 0;
	}

	return ret;
}

static int dbmdx_pcm_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dbmdx_runtime_data *prtd = runtime->private_data;
	struct timer_list *timer = prtd->timer;

	pr_debug("%s\n", __func__);

	flush_work(&prtd->pcm_start_capture_work);
	schedule_work(&prtd->pcm_stop_capture_work);
	flush_work(&prtd->pcm_stop_capture_work);
	kfree(timer);
	kfree(prtd);
	timer = NULL;
	prtd = NULL;

	dbmdx_codec_unlock();

	return 0;
}

static snd_pcm_uframes_t dbmdx_pcm_pointer(struct snd_pcm_substream *substream)
{
	u32 pos;

	/* pr_debug("%s\n", __func__); */


	pos = stream_get_position(substream);
	return bytes_to_frames(substream->runtime, pos);
}

static struct snd_pcm_ops dbmdx_pcm_ops = {
	.open		= dbmdx_pcm_open,
	.close		= dbmdx_pcm_close,
	.ioctl		= snd_pcm_lib_ioctl,
	.hw_params	= dbmdx_pcm_hw_params,
	.prepare	= dbmdx_pcm_prepare,
	.trigger	= dbmdx_pcm_trigger,
	.pointer	= dbmdx_pcm_pointer,
};

static int dbmdx_pcm_preallocate_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size = MAX_BUFFER_SIZE;

	pr_debug("%s\n", __func__);


	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;
	buf->area = dma_alloc_coherent(pcm->card->dev,
				       REAL_BUFFER_SIZE,
				       &buf->addr,
				       GFP_KERNEL);
	if (!buf->area) {
		pr_err("%s: Failed to allocate dma memory.\n", __func__);
		pr_err("%s: Please increase uncached DMA memory region\n",
			__func__);
		return -ENOMEM;
	}
	buf->bytes = size;

	return 0;
}

static int dbmdx_pcm_probe(struct snd_soc_platform *pt)
{
	struct snd_dbmdx *dbmdx;

	pr_debug("%s\n", __func__);


	dbmdx = kzalloc(sizeof(*dbmdx), GFP_KERNEL);
	if (!dbmdx)
		return -ENOMEM;
#if USE_ALSA_API_3_10_XX
	dbmdx->card = pt->card;
#else
	dbmdx->card = pt->component.card;
#endif
	dbmdx->pcm_hw = dbmdx_pcm_hardware;
	snd_soc_platform_set_drvdata(pt, dbmdx);

	return 0;
}

static int dbmdx_pcm_remove(struct snd_soc_platform *pt)
{
	struct snd_dbmdx *dbmdx;

	pr_debug("%s\n", __func__);


	dbmdx = snd_soc_platform_get_drvdata(pt);
	kfree(dbmdx);

	return 0;
}

static int dbmdx_pcm_new(struct snd_soc_pcm_runtime *runtime)
{
	struct snd_pcm *pcm;
	int ret = 0;

	pr_debug("%s\n", __func__);


	pcm = runtime->pcm;
	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
		ret = dbmdx_pcm_preallocate_dma_buffer(pcm,
			SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			goto out;
	}

	if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
		ret = dbmdx_pcm_preallocate_dma_buffer(pcm,
			SNDRV_PCM_STREAM_CAPTURE);
		if (ret)
			goto out;
	}
out:
	return ret;
}

static void dbmdx_pcm_free(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	int stream;

	pr_debug("%s\n", __func__);


	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		if (!substream)
			continue;

		buf = &substream->dma_buffer;
		if (!buf->area)
			continue;
		dma_free_coherent(pcm->card->dev,
				  REAL_BUFFER_SIZE,
				  (void *)buf->area,
				  buf->addr);
		buf->area = NULL;
	}
}

static struct snd_soc_platform_driver dbmdx_soc_platform = {
	.probe		= &dbmdx_pcm_probe,
	.remove		= &dbmdx_pcm_remove,
	.ops		= &dbmdx_pcm_ops,
	.pcm_new	= dbmdx_pcm_new,
	.pcm_free	= dbmdx_pcm_free,
};

static int dbmdx_pcm_platform_probe(struct platform_device *pdev)
{
	int err;

	pr_debug("%s\n", __func__);

	err = snd_soc_register_platform(&pdev->dev, &dbmdx_soc_platform);
	if (err)
		dev_err(&pdev->dev, "%s: snd_soc_register_platform() failed",
			__func__);

	return err;
}

static int dbmdx_pcm_platform_remove(struct platform_device *pdev)
{
	snd_soc_unregister_platform(&pdev->dev);

	pr_debug("%s\n", __func__);


	return 0;
}

static struct of_device_id snd_soc_platform_of_ids[] = {
	{ .compatible = "dspg,dbmdx-snd-soc-platform" },
	{ },
};

static struct platform_driver dbmdx_pcm_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = snd_soc_platform_of_ids,
	},
	.probe = dbmdx_pcm_platform_probe,
	.remove = dbmdx_pcm_platform_remove,
};

static int __init snd_dbmdx_pcm_init(void)
{
	return platform_driver_register(&dbmdx_pcm_driver);
}
module_init(snd_dbmdx_pcm_init);

static void __exit snd_dbmdx_pcm_exit(void)
{
	platform_driver_unregister(&dbmdx_pcm_driver);
}
module_exit(snd_dbmdx_pcm_exit);

MODULE_DESCRIPTION("DBMDX ASoC platform driver");
MODULE_AUTHOR("DSP Group");
MODULE_LICENSE("GPL");
