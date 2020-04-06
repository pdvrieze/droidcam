/* DroidCam & DroidCamX (C) 2010-
 * https://github.com/aramg
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */

#ifndef __DECODR_H__
#define __DECODR_H__

#include "context.h"

typedef unsigned char BYTE;



int loopback_init(JpgCtx *jpgCtx, unsigned int width, unsigned int height);
int decoder_init(JpgCtx *jpgCtx, OutputMode *droidcam_output_mode);
void decoder_fini(JpgCtx *jpgCtx);

int decoder_prepare_video(JpgCtx *jpgCtx, unsigned char *header);
void decoder_cleanup(JpgCtx *jpgCtx);

JpgFrame * decoder_get_next_frame(JpgCtx *jpgCtx);

void decoder_set_video_delay(JpgCtx *jpgCtx, unsigned v);
unsigned int decoder_get_video_width();
unsigned int decoder_get_video_height();
void decoder_rotate(JpgCtx *jpgCtx);
void decoder_show_test_image(JpgCtx *jpgCtx, const OutputMode *droidcam_output_mode);

void decoder_source_dimensions(JpgCtx *jpgCtx, unsigned int *width, unsigned int *height);

/* 20ms 16hkz 16 bit */
#define DROIDCAM_CHUNK_MS_2           20
#define DROIDCAM_SPX_CHUNK_BYTES_2    70
#define DROIDCAM_PCM_CHUNK_BYTES_2    640
#define DROIDCAM_PCM_CHUNK_SAMPLES_2  320

#define DROIDCAM_SPEEX_BACKBUF_MAX_COUNT 2

#define FORMAT_C 0

#define VIDEO_FMT_DROIDCAM 3
#define VIDEO_FMT_DROIDCAMX 18


#endif
