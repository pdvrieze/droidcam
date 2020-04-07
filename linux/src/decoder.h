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


class Decoder {
public:

	Decoder();
	~Decoder();

	OutputMode outputMode() {return _outputMode; };

	bool initLoopback(unsigned int width, unsigned int height);
	bool init();

	bool prepareVideo(const char *header);
	bool prepareVideoFromFrame(Buffer *data);
	bool prepareVideo(unsigned int srcWidth, int srcHeight);

	void cleanupJpeg();

public: // TODO make this private
	Buffer jpg_frames[JPG_BACKBUF_MAX];
	std::unique_ptr<JpgDecContext> jpg_decoder;

	void publishFrameToLoopback();

private:
	OutputMode _outputMode;
	int deviceFd;

	OutputMode findOutputDevice();
	void query_droidcam_v4l();
};


Buffer * decoder_get_next_frame(Decoder *jpgCtx);

void decoder_set_video_delay(Decoder *jpgCtx, unsigned v);
unsigned int decoder_get_video_width();
unsigned int decoder_get_video_height();
void decoder_rotate(Decoder *jpgCtx);
void decoder_show_test_image(Decoder *jpgCtx, const OutputMode *droidcam_output_mode);

void decoder_source_dimensions(Decoder *jpgCtx, unsigned int *width, unsigned int *height);

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
