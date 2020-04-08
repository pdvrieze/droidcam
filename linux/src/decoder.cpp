/* DroidCam & DroidCamX (C) 2010-
 * https://github.com/aramg
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */

#ifdef HAVE_AV_CONFIG_H
#undef HAVE_AV_CONFIG_H
#endif

extern "C" {
#include <unistd.h>
#include <fcntl.h>
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>

extern "C" {
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <turbojpeg.h>

#include <libswscale/swscale.h>
// #include "speex/speex.h"
}

#include "jpegpp.h"
#include "common.h"
#include "decoder.h"
#include "context.h"
#include "util.h"

struct spx_decoder_s {
	void *state;
#if 0
	SpeexBits bits;
#endif
	int audioBoostPerc;
	int frame_size;
};

typedef enum {
	_TJSAMP_444 = TJSAMP_444,
	_TJSAMP_422 = TJSAMP_422,
	_TJSAMP_420 = TJSAMP_420,
	_TJSAMP_GRAY = TJSAMP_GRAY,
	_TJSAMP_440 = TJSAMP_440,
	_TJSAMP_411 = TJSAMP_411,
	TJSAMP_UNK = 1000,
	TJSAMP_NIL = 1001
} TJSAMP_Ext;


static const int pixelsize[TJ_NUMSAMP] = {3, 3, 3, 1, 3, 3};

#define PAD(v, p) ((v+(p)-1)&(~((p)-1)))


static int fatal_error = 0;


class JpgDecContext {
public:

	bool init;;

	unsigned char *m_inBuf;         /* incoming stream */
	unsigned char *m_decodeBuf;     /* decoded individual frames */

};


Decoder::Decoder() : jpg_decoder(std::make_unique<JpgDecContext>()), _bufferedFramesMax(1), nextFrameToDisplay(0),
                     _outputMode(OutputMode::OM_DROIDCAM)
{
//	memset(&jpg_frames, 0, sizeof(jpg_frames));
}

Decoder::~Decoder()
{
	if (_deviceFd) close(_deviceFd);

	if (jpg_decoder->init) {
		jpg_decoder->init = false;
	}
};

static inline void fill_matrix(float sx, float sy, float angle, float scale, float *matrix)
{
	matrix[0] = scale * cos(angle);
	matrix[1] = -sin(angle);
	matrix[2] = sx;
	matrix[3] = -matrix[1];
	matrix[4] = matrix[0];
	matrix[5] = sy;
	matrix[6] = 0;
	matrix[7] = 0;
	matrix[8] = 1;
}

static int xioctl(int fd, int request, void *arg)
{
	int r;
	do r = ioctl(fd, request, arg);
	while (-1 == r && EINTR == errno);
	return r;
}

static inline int clip(int v)
{ return ((v < 0) ? 0 : ((v >= 256) ? 255 : v)); }

Jpeg &Decoder::jpeg()
{
	if (!x_jpeg) { x_jpeg = std::make_unique<Jpeg>(); }
	return *x_jpeg;
}

OutputMode Decoder::findOutputDevice()
{
	int crt_video_dev = 0;
	char device[12];
	struct stat st;
	struct v4l2_capability v4l2cap;

	for (crt_video_dev = 0; crt_video_dev < 99; crt_video_dev++) {
		_deviceFd = -1;
		sprintf(device, "/dev/video%d", crt_video_dev);
		if (-1 == stat(device, &st)) {
			continue;
		}

		if (!S_ISCHR(st.st_mode)) {
			continue;
		}

		_deviceFd = open(device, O_RDWR | O_NONBLOCK, 0);

		if (-1 == _deviceFd) {
			printf("Error opening '%s': %d '%s'\n", device, errno, strerror(errno));
			continue;
		}
		if (-1 == xioctl(_deviceFd, VIDIOC_QUERYCAP, &v4l2cap)) {
			close(_deviceFd);
			continue;
		}
		printf("Device (driver: %s): %s\n", v4l2cap.driver, v4l2cap.card);
		if (0 == strncmp((const char *) v4l2cap.driver, "Droidcam", 8)) {
			printf("Found device: %s (fd:%d)\n", device, _deviceFd);
			return OutputMode::OM_DROIDCAM;
		} else if (0 == strncmp((const char *) v4l2cap.driver, "v4l2 loopback", 8)) {
			printf("Found device: %s (fd:%d)\n", device, _deviceFd);
			return OutputMode::OM_V4LLOOPBACK;
		}
		close(_deviceFd);
	}
	fprintf(stderr, "Device not found (/dev/video[0-9]).\nDid you install it?\n");
	return OutputMode::OM_MISSING;
}

void Decoder::query_droidcam_v4l()
{
	struct v4l2_format vid_format = {0};
	vid_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vid_format.fmt.pix.width = 0;
	vid_format.fmt.pix.height = 0;
	if (xioctl(_deviceFd, VIDIOC_G_FMT, &vid_format) < 0) {
		fprintf(stderr, "Fatal: Unable to query droidcam video device. errno=%d\n", errno);
		return;
	}

	dbgprint("  vid_format->type                =%d\n", vid_format.type);
	dbgprint("  vid_format->fmt.pix.width       =%d\n", vid_format.fmt.pix.width);
	dbgprint("  vid_format->fmt.pix.height      =%d\n", vid_format.fmt.pix.height);
	dbgprint("  vid_format->fmt.pix.pixelformat =%d\n", vid_format.fmt.pix.pixelformat);
	dbgprint("  vid_format->fmt.pix.sizeimage   =%d\n", vid_format.fmt.pix.sizeimage);
	dbgprint("  vid_format->fmt.pix.field       =%d\n", vid_format.fmt.pix.field);
	dbgprint("  vid_format->fmt.pix.bytesperline=%d\n", vid_format.fmt.pix.bytesperline);
	dbgprint("  vid_format->fmt.pix.colorspace  =%d\n", vid_format.fmt.pix.colorspace);
	if (vid_format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
		fprintf(stderr, "Fatal: droidcam video device reported pixel format %d, expected %d\n",
		        vid_format.fmt.pix.pixelformat, V4L2_PIX_FMT_YUV420);
		return;
	}
	if (vid_format.fmt.pix.width <= 0 || vid_format.fmt.pix.height <= 0) {
		fprintf(stderr, "Fatal: droidcam video device reported invalid resolution: %dx%d\n",
		        vid_format.fmt.pix.width, vid_format.fmt.pix.height);
		return;
	}

	_loopbackWidth = vid_format.fmt.pix.width;
	_loopbackHeight = vid_format.fmt.pix.height;
}

void Decoder::bufferedFramesMax(unsigned int newVal)
{
	if (newVal > JPG_BACKBUF_MAX) newVal = JPG_BACKBUF_MAX;
	else if (newVal < 1) newVal = 1;
	_bufferedFramesMax = newVal;
	dbgprint("buffer %d frames\n", _bufferedFramesMax);
}

bool Decoder::initLoopback(unsigned int targetWidth, unsigned int targetHeight)
{
	/* TODO make this close and reopen the file descriptor if the size isn't as desired. The size is only resettable
	 * by closing the device (on all sides).
	 */

	struct v4l2_format vid_format = {0};
	vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	vid_format.fmt.pix.width = targetWidth;
	vid_format.fmt.pix.height = targetHeight;
	vid_format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
//	vid_format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	vid_format.fmt.pix.field = V4L2_FIELD_TOP;
	vid_format.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
	if (xioctl(_deviceFd, VIDIOC_S_FMT, &vid_format) < 0) {
		int e = errno;
		fprintf(stderr, "Fatal: Unable to set droidcam video device. errno=%d, msg=%s\n", e, strerror(e));
		return false;
	}

	dbgprint("  vid_format->type                =%d\n", vid_format.type);
	dbgprint("  vid_format->fmt.pix.width       =%d\n", vid_format.fmt.pix.width);
	dbgprint("  vid_format->fmt.pix.height      =%d\n", vid_format.fmt.pix.height);
	dbgprint("  vid_format->fmt.pix.pixelformat =%d\n", vid_format.fmt.pix.pixelformat);
	dbgprint("  vid_format->fmt.pix.sizeimage   =%d\n", vid_format.fmt.pix.sizeimage);
	dbgprint("  vid_format->fmt.pix.field       =%d\n", vid_format.fmt.pix.field);
	dbgprint("  vid_format->fmt.pix.bytesperline=%d\n", vid_format.fmt.pix.bytesperline);
	dbgprint("  vid_format->fmt.pix.colorspace  =%d\n", vid_format.fmt.pix.colorspace);
	_loopbackWidth = vid_format.fmt.pix.width;
	_loopbackHeight = vid_format.fmt.pix.height;

	return true;
}

bool Decoder::init()
{
	_loopbackWidth = 0;
	_loopbackHeight = 0;

	_outputMode = findOutputDevice();

	if (_outputMode == OutputMode::OM_MISSING)
		return false;


	if (_outputMode == OutputMode::OM_DROIDCAM) { // droidcam mode
		query_droidcam_v4l();
		dbgprint("dstWidth=%d, dstHeight=%d\n", loopbackWidth(), loopbackHeight());
		if (loopbackWidth() < 2 || loopbackHeight() < 2 || loopbackWidth() > 9999 || loopbackHeight() > 9999) {
			MSG_ERROR("Unable to query droidcam device for parameters");
			return false;
		}
	}

	jpg_decoder->init = true;

	bufferedFramesMax(0);

#if 0
	speex_bits_init(&spx_decoder.bits);
	spx_decoder.state = speex_decoder_init(speex_lib_get_mode(SPEEX_MODEID_WB));
	speex_decoder_ctl(spx_decoder.state, SPEEX_GET_FRAME_SIZE, &spx_decoder.frame_size);
	dbgprint("spx_decoder.state=%p\n", spx_decoder.state);
#endif

	return true;
}


bool Decoder::prepareVideoFromFrame(Buffer *data)
{
	auto &jpg = jpeg();
	auto dimensions = jpg.frameDimensions(*data);
	return prepareVideo(dimensions.width, dimensions.height);
}

bool Decoder::prepareVideo(const char *header)
{
	char *header1;
	unsigned int srcWidth, srcHeight;

	return prepareVideo(make_int(header1[0], header1[1]),
	                    make_int(header1[2], header1[3]));
}

bool Decoder::prepareVideo(unsigned int srcWidth, unsigned int srcHeight)
{

	if (srcWidth <= 0 || srcHeight <= 0) {
		MSG_ERROR("Invalid data stream!");
		return false;
	}

	int i;
	const auto &dec = jpg_decoder;

	dbgprint("Stream W=%d H=%d\n", srcWidth, srcHeight);

	auto ySize = srcWidth * srcHeight;
	auto yuv420size = ySize * 3 / 2;
	size_t inBufSize = (yuv420size * JPG_BACKBUF_MAX + 4096) * sizeof(unsigned char);
	dec->m_inBuf = (unsigned char *) realloc(dec->m_inBuf, inBufSize);
	dec->m_decodeBuf = (unsigned char *) realloc(dec->m_decodeBuf, yuv420size * sizeof(unsigned char));

	if (loopbackWidth() == 0 || loopbackHeight() == 0) { // If we have no size, try to set it to the size of the source
		initLoopback(srcWidth, srcHeight);
	}

	dbgprint("jpg: decodebuf: %p\n", dec->m_decodeBuf);
	dbgprint("jpg: inbuf    : %p\n", dec->m_inBuf);

	for (i = 0; i < JPG_BACKBUF_MAX; i++) {
		jpg_frames[i].data = &dec->m_inBuf[i * yuv420size];
		jpg_frames[i].buf_size = yuv420size;
		jpg_frames[i].data_length = 0;
		dbgprint("jpg: jpg_frames[%d]: %p\n", i, jpg_frames[i].data);
	}

	nextFrameToDisplay = nextFrameToStore = 0;
	setTransform(Transform::DEG0);

	return true;
}

void Decoder::cleanupJpeg()
{
	Decoder *jpgCtx = this;
	dbgprint("Cleanup\n");

	for (int i = 0; i < JPG_BACKBUF_MAX; ++i) {
		jpgCtx->jpg_frames->data = nullptr;
	}

	doFree(jpgCtx->jpg_decoder->m_inBuf, std::function(free));
	FREE_OBJECT(jpgCtx->jpg_decoder->m_inBuf, free)
	FREE_OBJECT(jpgCtx->jpg_decoder->m_decodeBuf, free)
	// TODO just have a reset
	memset(&(*(jpgCtx->jpg_decoder)), 0, sizeof(JpgDecContext));
}

void Decoder::decodeNextFrame()
{
	Buffer &frame = jpg_frames[nextFrameToDisplay];

	jpeg().decodeFrame(jpegOutput, frame);
	publishFrameToLoopback(jpegOutput);
}

static void apply_transform_helper(const uint8_t *src, uint8_t *dst,
                                   unsigned int width, unsigned int height, unsigned int transformNull,
                                   const float *matrix)
{
	int x, y;
	float x_s, y_s, d;
#define PIXEL(img, x, y, w, h, stride, def) \
        ((x) < 0 || (y) < 0) ? (def) : \
        (((x) >= (w) || (y) >= (h)) ? (def) : \
        img[(x) + (y) * (stride)])

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			x_s = (float) x * matrix[0] + (float) y * matrix[1] + matrix[2];
			y_s = (float) x * matrix[3] + (float) y * matrix[4] + matrix[5];

			d = PIXEL(src, lround(x_s), lround(y_s), width, height, width, 0);
			dst[y * width + x] = (transformNull > 0 && d == 0) ? static_cast<float>(transformNull) : d;
		}
	}
}

/* scratch is a working buffer of 2*ySize (2 * w * h) length */
void Decoder::apply_transform(UncompressedFrame &yuv420image, UncompressedFrame &scratch)
{
	auto width = yuv420image.size().width;
	auto height = yuv420image.size().height;
	// Transform Y component as is
	apply_transform_helper(yuv420image.buffer, scratch.buffer,
	                       width, height, 0,
	                       scale_matrix);

	apply_transform_helper(scratch.buffer, yuv420image.buffer,
	                       width, height, 0,
	                       angle_matrix);

	// Expand U component, transform, then sub-sample back down
	int row, col;
	unsigned char *p = &yuv420image.buffer[yuv420image.ySize()];
	unsigned char *d = &scratch.buffer[scratch.ySize()];

	for (row = 0; row < height; row += 2) {
		for (col = 0; col < width; col += 2) {
			unsigned char u = *p++;
			scratch.buffer[(row + 0) * width + col + 0] = u;
			scratch.buffer[(row + 0) * width + col + 1] = u;
			scratch.buffer[(row + 1) * width + col + 0] = u;
			scratch.buffer[(row + 1) * width + col + 1] = u;
		}
	}

	apply_transform_helper(scratch.buffer, d,
	                       width, height, 0,
	                       scale_matrix);

	apply_transform_helper(d, scratch.buffer,
	                       width, height, 128,
	                       angle_matrix);

	p = &yuv420image.buffer[yuv420image.ySize()];
	for (row = 0; row < height; row += 2) {
		for (col = 0; col < width; col += 2) {
			*p++ = scratch.buffer[row * width + col];
		}
	}

	// Expand V component, transform, then sub-sample back down
	p = &yuv420image.buffer[yuv420image.frameSize()];
	d = &scratch.buffer[yuv420image.ySize()];

	for (row = 0; row < height; row += 2) {
		for (col = 0; col < width; col += 2) {
			unsigned char v = *p++;
			scratch.buffer[(row + 0) * width + col + 0] = v;
			scratch.buffer[(row + 0) * width + col + 1] = v;
			scratch.buffer[(row + 1) * width + col + 0] = v;
			scratch.buffer[(row + 1) * width + col + 1] = v;
		}
	}
	apply_transform_helper(scratch.buffer, d,
	                       width, height, 0,
	                       scale_matrix);

	apply_transform_helper(d, scratch.buffer,
	                       width, height, 128,
	                       angle_matrix);

	p = &yuv420image.buffer[yuv420image.frameSize()];
	for (row = 0; row < height; row += 2) {
		for (col = 0; col < width; col += 2) {
			*p++ = scratch.buffer[row * loopbackWidth() + col];
		}
	}
}

void Decoder::publishFrameToLoopback(const UncompressedFrame &jpegOutput)
{
	scalingResult.ensureYuv420Buffer(Dimension{loopbackWidth(), loopbackHeight()});
	if (!scaler) { scaler = std::make_unique<Scaler>(); }
	scaler->scale(jpegOutput, scalingResult);

	// TODO: This is currently super inefficient unfortunately :(
	if (_transform != Transform::DEG0) {
		scratchBuffer.ensureYuv420Buffer(scalingResult.size());
		apply_transform(scalingResult, scratchBuffer); // this changes in place
	}

	write(_deviceFd, scalingResult.buffer, scalingResult.frameSize());

}

void Decoder::showTestImage()
{
	int i, j;
	if (outputMode() == OutputMode::OM_V4LLOOPBACK) {
		if (!initLoopback(1280, 720)) return;
	}

	unsigned int srcWidth = loopbackWidth() * 2;
	unsigned int srcHeight = loopbackHeight() * 2;

	if (!prepareVideo(srcWidth, srcHeight)) return;
	jpegOutput.ensureYuv420Buffer(Dimension(srcWidth, srcHeight));

	// [ jpg ] -> [ yuv420 ] -> [ yuv420 scaled ] -> [ yuv420 webcam transformed ]

	// fill in "decoded" data
	unsigned char *p = jpegOutput.buffer;
	memset(p, 128, jpegOutput.frameSize());
	for (j = 0; j < srcHeight; j++) {
		unsigned char *line_end = p + srcWidth;
		for (i = 0; i < (srcWidth / 4); i++) {
			*p++ = 0;
		}
		for (i = 0; i < (srcWidth / 4); i++) {
			*p++ = 64;
		}
		for (i = 0; i < (srcWidth / 4); i++) {
			*p++ = 128;
		}
		for (i = 0; i < (srcWidth / 4); i++) {
			*p++ = rand() % 250;
		}
		while (p < line_end) p++;
	}

	publishFrameToLoopback(jpegOutput);
	rotate();

}

void Decoder::setTransform(Transform transform)
{
	float scale = 1.0f;
	float moveX = 0;
	float moveY = 0;
	float rot = 0;

	auto width = static_cast<float>(loopbackWidth());
	auto height = static_cast<float>(loopbackHeight());

	_transform = transform;
	switch (transform) {

		case Transform::DEG0:
			rot = 0;
			break;
		case Transform::DEG90:
			rot = 0.5f * M_PIf32;
			scale = width / height;
			moveX = height;
			moveY = (height / scale - width) / 2.0f;
			break;
		case Transform::DEG180:
			rot = M_PIf32;
			moveX = width;
			moveY = height;
			break;
		case Transform::DEG270:
			rot = 1.5f * M_PIf32;
			scale = width / height;
			moveY = height;
			break;
	}

	fill_matrix(0, 0, 0, scale, scale_matrix);
	fill_matrix(moveX, moveY, rot, 1.0f, angle_matrix);
}

void Decoder::rotate()
{
	Transform t;
	switch (_transform) {
		case Transform::DEG0:
			t = Transform::DEG90;
			break;
		case Transform::DEG90:
			t = Transform::DEG180;
			break;
		case Transform::DEG180:
			t = Transform::DEG270;
			break;
		case Transform::DEG270:
			t = Transform::DEG0;
			break;
	}
	setTransform(t);
}


void Decoder::putNextFrame(Buffer &frame)
{
	while (bufferedFramesCnt() > bufferedFramesMax()) {
		--_bufferedFramesCnt;
		nextFrameToDisplay = (nextFrameToDisplay + 1) % JPG_BACKBUF_MAX;
	}
	if (bufferedFramesCnt() == bufferedFramesMax()) {
		dbgprint("\ndecoding #%2lud (have buffered: %d)\n", nextFrameToDisplay, bufferedFramesCnt());
		decodeNextFrame();
		--_bufferedFramesCnt;
		nextFrameToDisplay = (nextFrameToDisplay + 1) % JPG_BACKBUF_MAX;
	}

	/*
	 * a call to this function assumes we are about to get a full frame (or exit on failure).
	 * so increment the # of buffered frames. do this after the while() loop above to
	 * take care of the initial case:
	 */
	++_bufferedFramesCnt;

	swap(jpg_frames[nextFrameToStore], frame);

	nextFrameToStore = (nextFrameToStore + 1) % JPG_BACKBUF_MAX;
	return

}

/**
 * Move the context to the next frame
 * @return the frame to write to
 */
Buffer *Decoder::getNextFrame()
{
	while (bufferedFramesCnt() > bufferedFramesMax()) {
		--_bufferedFramesCnt;
		nextFrameToDisplay = (nextFrameToDisplay + 1) % JPG_BACKBUF_MAX;
	}
	if (bufferedFramesCnt() == bufferedFramesMax()) {
		dbgprint("\ndecoding #%2lud (have buffered: %d)\n", nextFrameToDisplay, bufferedFramesCnt());
		decodeNextFrame();
		--_bufferedFramesCnt;
		nextFrameToDisplay = (nextFrameToDisplay + 1) % JPG_BACKBUF_MAX;
	}

	/*
	 * a call to this function assumes we are about to get a full frame (or exit on failure).
	 * so increment the # of buffered frames. do this after the while() loop above to
	 * take care of the initial case:
	 */
	++_bufferedFramesCnt;

	int nextSlotSaved = nextFrameToStore;
	nextFrameToStore = (nextFrameToStore + 1) % JPG_BACKBUF_MAX;
	// dbgprint("next image going to #%2d (have buffered: %d)\n", nextSlotSaved, (jpg_decoder.m_BufferedFrames-1));
	return &jpg_frames[nextSlotSaved];
}

unsigned int Decoder::loopbackWidth()
{ return _loopbackWidth; }

unsigned int Decoder::loopbackHeight()
{ return _loopbackHeight; }


Scaler::Scaler()
{

}

Scaler::~Scaler()
{
	if (swc) sws_freeContext(swc);
}

void Scaler::scale(const UncompressedFrame &src, UncompressedFrame &dst)
{
	const int srcWidth = (int) src.size().width;
	const int srcHeight = (int) src.size().height;
	const int dstWidth = (int) dst.size().width;
	const int dstHeight = (int) dst.size().height;
	if (!swc) {
		swc = sws_getContext(srcWidth, srcHeight, AV_PIX_FMT_YUV420P, /* src */
		                     dstWidth, dstHeight, AV_PIX_FMT_YUV420P, /* dst */
		                     SWS_FAST_BILINEAR /* flags */, nullptr, nullptr, nullptr);

	} else {
		swc = sws_getCachedContext(swc,
		                           srcWidth, srcHeight, AV_PIX_FMT_YUV420P, /* src */
		                           dstWidth, dstHeight, AV_PIX_FMT_YUV420P, /* dst */
		                           SWS_FAST_BILINEAR /* flags */, nullptr, nullptr, nullptr);

	}
	if (true) {
		uint8_t *srcSlice[4];
		uint8_t *dstSlice[4];

		int srcStride[4] = {
			static_cast<int>(srcWidth),
			static_cast<int>(srcWidth >> 1),
			static_cast<int>(srcWidth >> 1),
			0};
		int dstStride[4] = {
			static_cast<int>(dstWidth),
			static_cast<int>(dstWidth >> 1),
			static_cast<int>(dstWidth >> 1),
			0};

		srcSlice[0] = src.buffer;
		srcSlice[1] = srcSlice[0] + src.ySize();
		srcSlice[2] = srcSlice[1] + src.uvSize();
		srcSlice[3] = nullptr;
		dstSlice[0] = dst.buffer;
		dstSlice[1] = dstSlice[0] + dst.ySize();
		dstSlice[2] = dstSlice[1] + dst.uvSize();
		dstSlice[3] = nullptr;


		sws_scale(swc, srcSlice, srcStride, 0, srcHeight, dstSlice,
		          dstStride);
	}

}
