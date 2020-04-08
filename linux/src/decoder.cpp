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
	struct jpeg_decompress_struct dinfo;
	struct jpeg_error_mgr jerr;

	bool init;
	TJSAMP_Ext subsamp;
	unsigned int srcHeight;
	unsigned int srcWidth;
	unsigned int m_uvSize;
	unsigned int m_ySize;
	unsigned int m_Yuv420Size;
	int m_webcam_uvSize;
	int m_webcam_ySize;
	int m_webcamYuvSize;;
	int m_BufferedFrames;
	int m_NextFrame;
	int m_NextSlot;

	unsigned char *m_inBuf;         /* incoming stream */
	unsigned char *m_decodeBuf;     /* decoded individual frames */
	unsigned char *m_webcamBuf;     /* optional, scale incoming stream for the webcam */
	unsigned char *scratchBuf;

	// xxx: better way to do all the scaling/rotation/etc?
	struct SwsContext *swc;
	float scale_matrix[9];
	float angle_matrix[9];

	/* these should be alloced for each frame but sunce the stream
	 * from the app will be consistent, we'll optimize by only allocing
	 * once */
	int cw[MAX_COMPONENTS], ch[MAX_COMPONENTS], iw[MAX_COMPONENTS], th[MAX_COMPONENTS];
	JSAMPROW *outbuf[MAX_COMPONENTS];

	int transform;
};


Decoder::Decoder() : jpg_decoder(std::make_unique<JpgDecContext>()), _bufferedFramesMax(1), nextFrame(0),
                     _outputMode(OutputMode::OM_DROIDCAM)
{
//	memset(&jpg_frames, 0, sizeof(jpg_frames));
}

Decoder::~Decoder()
{
	if (_deviceFd) close(_deviceFd);

	if (jpg_decoder->init) {
		jpeg_destroy_decompress(&jpg_decoder->dinfo);
		jpg_decoder->init = FALSE;
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
	fprintf(stderr,"Device not found (/dev/video[0-9]).\nDid you install it?\n");
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

	jpg_decoder->m_webcamYuvSize = static_cast<int>(targetWidth * targetHeight * 3 / 2);
	jpg_decoder->m_webcam_ySize = static_cast<int>(targetWidth * targetHeight);
	jpg_decoder->m_webcam_uvSize = jpg_decoder->m_webcam_ySize / 4;

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

	fatal_error = 0;
	jpg_decoder->dinfo.err = jpeg_std_error(&jpg_decoder->jerr);
	jpg_decoder->jerr.output_message = joutput_message;
	jpg_decoder->jerr.error_exit = jerror_exit;
	jpeg_create_decompress(&jpg_decoder->dinfo);

	if (fatal_error) return false;
	jpg_decoder->init = TRUE;
	jpg_decoder->subsamp = TJSAMP_NIL;
	jpg_decoder->m_webcamYuvSize = loopbackWidth() * loopbackHeight() * 3 / 2;
	jpg_decoder->m_webcam_ySize = loopbackWidth() * loopbackHeight();
	jpg_decoder->m_webcam_uvSize = jpg_decoder->m_webcam_ySize / 4;
	jpg_decoder->transform = 0;
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
	auto &dec = *jpg_decoder;
	struct jpeg_decompress_struct *dinfo = &(dec.dinfo);
	jpeg_mem_src(dinfo, data->data, data->data_length);
	jpeg_read_header(dinfo, TRUE);
	jpeg_abort_decompress(dinfo);
	return prepareVideo(dinfo->image_width, dinfo->image_height);
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

	dec->srcWidth = srcWidth;
	dec->srcHeight = srcHeight;

	dbgprint("Stream W=%d H=%d\n", srcWidth, srcHeight);

	dec->m_ySize = srcWidth * srcHeight;
	dec->m_uvSize = dec->m_ySize / 4;
	dec->m_Yuv420Size = dec->m_ySize * 3 / 2;
	size_t inBufSize = (dec->m_Yuv420Size * JPG_BACKBUF_MAX + 4096) * sizeof(unsigned char);
	dec->m_inBuf = (unsigned char *) realloc(dec->m_inBuf, inBufSize);
	dec->m_decodeBuf = (unsigned char *) realloc(dec->m_decodeBuf, dec->m_Yuv420Size * sizeof(unsigned char));
	dec->scratchBuf = (unsigned char *) realloc(dec->scratchBuf, dec->m_webcam_ySize * 2 * sizeof(unsigned char));

	if (loopbackWidth() == 0 || loopbackHeight() == 0) { // If we have no size, try to set it to the size of the source
		initLoopback(srcWidth, srcHeight);
	}

	if (dec->m_webcamYuvSize != dec->m_Yuv420Size) {
		dbgprint("Updating cached sws context from %p to ", dec->swc);
		dec->m_webcamBuf = (unsigned char *) malloc(dec->m_webcamYuvSize * sizeof(unsigned char));
		dec->swc = sws_getCachedContext(dec->swc,
		                                (int) srcWidth, (int) srcHeight, AV_PIX_FMT_YUV420P, /* src */
		                                (int) _loopbackWidth, (int) _loopbackHeight, AV_PIX_FMT_YUV420P, /* dst */
		                                SWS_FAST_BILINEAR /* flags */, NULL, NULL, NULL);
		dbgprint("%p\n", dec->swc);
	}

	dbgprint("jpg: webcambuf: %p\n", dec->m_webcamBuf);
	dbgprint("jpg: decodebuf: %p\n", dec->m_decodeBuf);
	dbgprint("jpg: inbuf    : %p\n", dec->m_inBuf);

	for (i = 0; i < JPG_BACKBUF_MAX; i++) {
		jpg_frames[i].data = &dec->m_inBuf[i * dec->m_Yuv420Size];
		jpg_frames[i].buf_size = dec->m_Yuv420Size;
		jpg_frames[i].data_length = 0;
		dbgprint("jpg: jpg_frames[%d]: %p\n", i, jpg_frames[i].data);
	}

	dec->m_BufferedFrames = nextFrame = dec->m_NextSlot = 0;
	setTransform(dec->transform);

	for (i = 0; i < MAX_COMPONENTS; i++) {
		dec->outbuf[i] = nullptr;
	}

	return true;
}

void Decoder::cleanupJpeg()
{
	Decoder *jpgCtx = this;
	dbgprint("Cleanup\n");
	for (auto &i : jpgCtx->jpg_decoder->outbuf) {
		FREE_OBJECT(i, free);
	}

	for (int i = 0; i < JPG_BACKBUF_MAX; ++i) {
		jpgCtx->jpg_frames->data = nullptr;
	}

	doFree(jpgCtx->jpg_decoder->m_inBuf, std::function(free));
	FREE_OBJECT(jpgCtx->jpg_decoder->m_inBuf, free)
	FREE_OBJECT(jpgCtx->jpg_decoder->m_decodeBuf, free)
	FREE_OBJECT(jpgCtx->jpg_decoder->m_webcamBuf, free)
	FREE_OBJECT(jpgCtx->jpg_decoder->scratchBuf, free)
	FREE_OBJECT(jpgCtx->jpg_decoder->swc, sws_freeContext)
	// TODO just have a reset
	memset(&(*(jpgCtx->jpg_decoder)), 0, sizeof(JpgDecContext));
}

static void decode_next_frame(Decoder *jpgCtx);

void Decoder::decodeNextFrame()
{
	Buffer &frame = jpg_frames[nextFrame];
	if (!_jpeg) _jpeg = std::make_unique<Jpeg>();

	_jpeg->decodeFrame(jpegOutput, frame);
	publishFrameToLoopback(jpegOutput);
//	decode_next_frame(this);
}

static void decode_next_frame(Decoder *jpgCtx)
{
	struct jpeg_decompress_struct *dinfo = &jpgCtx->jpg_decoder->dinfo;
	Buffer &frame = jpgCtx->jpg_frames[jpgCtx->nextFrame];
	unsigned char *p = frame.data;
	unsigned long len = frame.data_length;

//	int k, row =0;
	bool usetmpbuf = false;
	JSAMPLE *ptr = jpgCtx->jpg_decoder->m_decodeBuf;

	jpeg_mem_src(dinfo, p, len);
	jpeg_read_header(dinfo, TRUE);
	if (fatal_error) return;
	dinfo->raw_data_out = TRUE;
	dinfo->do_fancy_upsampling = FALSE;
	dinfo->dct_method = JDCT_FASTEST;
	dinfo->out_color_space = JCS_YCbCr;

	if (jpgCtx->jpg_decoder->subsamp == TJSAMP_NIL) {
		TJSAMP_Ext retval = TJSAMP_NIL;
		for (int i = 0; i < TJ_NUMSAMP; i++) {
			if (dinfo->num_components == pixelsize[i]) {
				if (dinfo->comp_info[0].h_samp_factor == tjMCUWidth[i] / 8
				    && dinfo->comp_info[0].v_samp_factor == tjMCUHeight[i] / 8) {
					int match = 0;
					for (int k = 1; k < dinfo->num_components; k++) {
						if (dinfo->comp_info[k].h_samp_factor == 1
						    && dinfo->comp_info[k].v_samp_factor == 1)
							match++;
					}
					if (match == dinfo->num_components - 1) {
						retval = static_cast<TJSAMP_Ext>(i);
						break;
					}
				}
			}
		}
		dbgprint("subsampling=%d\n", retval);
		if (retval >= 0 && retval < TJSAMP_NIL) {
			jpgCtx->jpg_decoder->subsamp = retval;
		} else {
			jpgCtx->jpg_decoder->subsamp = TJSAMP_UNK;
		}
	}

	if (jpgCtx->jpg_decoder->subsamp != _TJSAMP_420) {
		fprintf(stderr, "Error: Unexpected video image stream subsampling\n");
		jpeg_abort_decompress(dinfo);
		return;
	}

	if (jpgCtx->jpg_decoder->outbuf[0] == nullptr) {
		int ih;
		int *cw = jpgCtx->jpg_decoder->cw;
		int *ch = jpgCtx->jpg_decoder->ch;
		int *iw = jpgCtx->jpg_decoder->iw;
		int *th = jpgCtx->jpg_decoder->th;
		JSAMPROW **outbuf = jpgCtx->jpg_decoder->outbuf;
		for (int i = 0; i < dinfo->num_components; i++) {
			jpeg_component_info *compptr = &dinfo->comp_info[i];
			iw[i] = compptr->width_in_blocks * DCTSIZE;
			ih = compptr->height_in_blocks * DCTSIZE;
			cw[i] =
				PAD(dinfo->image_width, dinfo->max_h_samp_factor) * compptr->h_samp_factor /
				dinfo->max_h_samp_factor;
			ch[i] =
				PAD(dinfo->image_height, dinfo->max_v_samp_factor) * compptr->v_samp_factor /
				dinfo->max_v_samp_factor;
			if (iw[i] != cw[i] || ih != ch[i]) {
				usetmpbuf = true;
				fprintf(stderr, "error: need a temp buffer, this shouldnt happen!\n");
				jpgCtx->jpg_decoder->subsamp = TJSAMP_UNK;
			}
			th[i] = compptr->v_samp_factor * DCTSIZE;

			dbgprint("extra alloc: %d\n", (int) (sizeof(JSAMPROW) * ch[i]));
			if ((outbuf[i] = (JSAMPROW *) malloc(sizeof(JSAMPROW) * ch[i])) == nullptr) {
				fprintf(stderr, "error: malloc failure\n");
				jpeg_abort_decompress(dinfo);
				return;
			}
			for (int row = 0; row < ch[i]; row++) {
				outbuf[i][row] = ptr;
				ptr += PAD(cw[i], 4);
			}
		}
	}

	if (usetmpbuf) {
		fprintf(stderr, "error: Unexpected video image dimensions\n");
		jpeg_abort_decompress(dinfo);
		return;
	}

	jpeg_start_decompress(dinfo);
	if (fatal_error) {
		jpeg_abort_decompress(dinfo);
		return;
	}

	if ((int) dinfo->output_width != jpgCtx->jpg_decoder->srcWidth ||
	    (int) dinfo->output_height != jpgCtx->jpg_decoder->srcHeight) {
		dbgprint("error: jpgCtx output %dx%d differs from expected %dx%d size\n",
		         dinfo->output_width, dinfo->output_height, jpgCtx->jpg_decoder->srcWidth,
		         jpgCtx->jpg_decoder->srcHeight);
		jpeg_abort_decompress(&jpgCtx->jpg_decoder->dinfo);
		return;
	}

	for (int row = 0; row < (int) dinfo->output_height; row += dinfo->max_v_samp_factor * DCTSIZE) {
		JSAMPARRAY yuvptr[MAX_COMPONENTS];
		int crow[MAX_COMPONENTS];
		for (int i = 0; i < dinfo->num_components; i++) {
			jpeg_component_info *compptr = &dinfo->comp_info[i];
			crow[i] = row * compptr->v_samp_factor / dinfo->max_v_samp_factor;
			yuvptr[i] = &jpgCtx->jpg_decoder->outbuf[i][crow[i]];
		}
		jpeg_read_raw_data(dinfo, yuvptr, dinfo->max_v_samp_factor * DCTSIZE);
	}
	jpeg_finish_decompress(dinfo);
	jpgCtx->publishFrameToLoopback(jpgCtx->jpegOutput);
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
void Decoder::apply_transform(unsigned char *yuv420image, unsigned char *scratch)
{
	// Transform Y component as is
	apply_transform_helper(yuv420image, scratch,
	                       loopbackWidth(), loopbackHeight(), 0,
	                       jpg_decoder->scale_matrix);

	apply_transform_helper(scratch, yuv420image,
	                       loopbackWidth(), loopbackHeight(), 0,
	                       jpg_decoder->angle_matrix);

	// Expand U component, transform, then sub-sample back down
	int row, col;
	unsigned char *p = &yuv420image[jpg_decoder->m_webcam_ySize];
	unsigned char *d = &scratch[jpg_decoder->m_webcam_ySize];

	for (row = 0; row < loopbackHeight(); row += 2) {
		for (col = 0; col < loopbackWidth(); col += 2) {
			unsigned char u = *p++;
			scratch[(row + 0) * loopbackWidth() + col + 0] = u;
			scratch[(row + 0) * loopbackWidth() + col + 1] = u;
			scratch[(row + 1) * loopbackWidth() + col + 0] = u;
			scratch[(row + 1) * loopbackWidth() + col + 1] = u;
		}
	}

	apply_transform_helper(scratch, d,
	                       loopbackWidth(), loopbackHeight(), 0,
	                       jpg_decoder->scale_matrix);

	apply_transform_helper(d, scratch,
	                       loopbackWidth(), loopbackHeight(), 128,
	                       jpg_decoder->angle_matrix);

	p = &yuv420image[jpg_decoder->m_webcam_ySize];
	for (row = 0; row < loopbackHeight(); row += 2) {
		for (col = 0; col < loopbackWidth(); col += 2) {
			*p++ = scratch[row * loopbackWidth() + col];
		}
	}

	// Expand V component, transform, then sub-sample back down
	p = &yuv420image[jpg_decoder->m_webcam_ySize + jpg_decoder->m_webcam_uvSize];
	d = &scratch[jpg_decoder->m_webcam_ySize];

	for (row = 0; row < loopbackHeight(); row += 2) {
		for (col = 0; col < loopbackWidth(); col += 2) {
			unsigned char v = *p++;
			scratch[(row + 0) * loopbackWidth() + col + 0] = v;
			scratch[(row + 0) * loopbackWidth() + col + 1] = v;
			scratch[(row + 1) * loopbackWidth() + col + 0] = v;
			scratch[(row + 1) * loopbackWidth() + col + 1] = v;
		}
	}
	apply_transform_helper(scratch, d,
	                       loopbackWidth(), loopbackHeight(), 0,
	                       jpg_decoder->scale_matrix);

	apply_transform_helper(d, scratch,
	                       loopbackWidth(), loopbackHeight(), 128,
	                       jpg_decoder->angle_matrix);

	p = &yuv420image[jpg_decoder->m_webcam_ySize + jpg_decoder->m_webcam_uvSize];
	for (row = 0; row < loopbackHeight(); row += 2) {
		for (col = 0; col < loopbackWidth(); col += 2) {
			*p++ = scratch[row * loopbackWidth() + col];
		}
	}
}

void Decoder::publishFrameToLoopback(const UncompressedFrame &jpegOutput)
{
	scalingResult.ensureYuv420Buffer(Dimension{loopbackWidth(), loopbackHeight()});
	if (!scaler) { scaler = std::make_unique<Scaler>(); }
	scaler->scale(jpegOutput, scalingResult);

	if (false && jpg_decoder->swc != nullptr) {
		uint8_t *srcSlice[4];
		uint8_t *dstSlice[4];

		int srcStride[4] = {
			static_cast<int>(jpg_decoder->srcWidth),
			static_cast<int>(jpg_decoder->srcWidth >> 1u),
			static_cast<int>(jpg_decoder->srcWidth >> 1u),
			0};
		int dstStride[4] = {
			static_cast<int>(loopbackWidth()),
			static_cast<int>(loopbackWidth() >> 1u),
			static_cast<int>(loopbackWidth() >> 1u),
			0};

		srcSlice[0] = jpegOutput.buffer;
		srcSlice[1] = srcSlice[0] + jpg_decoder->m_ySize;
		srcSlice[2] = srcSlice[1] + jpg_decoder->m_uvSize;
		srcSlice[3] = nullptr;
		dstSlice[0] = &jpg_decoder->m_webcamBuf[0];
		dstSlice[1] = dstSlice[0] + jpg_decoder->m_webcam_ySize;
		dstSlice[2] = dstSlice[1] + jpg_decoder->m_webcam_uvSize;
		dstSlice[3] = nullptr;


		sws_scale(jpg_decoder->swc, srcSlice, srcStride, 0, jpg_decoder->srcHeight, dstSlice,
		          dstStride);
//		p = jpg_decoder->m_webcamBuf;
	}

	// todo: This is currently super inefficient unfortunately :(
/*
	if (jpg_decoder->transform != 0) {
		apply_transform(p, jpg_decoder->scratchBuf);
	}
*/

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

void Decoder::setTransform(int transform)
{
	float scale = 1.0f;
	float moveX = 0;
	float moveY = 0;
	float rot = 0;

	auto width = static_cast<float>(loopbackWidth());
	auto height = static_cast<float>(loopbackHeight());
	/*
	 * FILE *fp = fopen("/tmp/specs", "r");
	 * if (fp) {
	 *     char buf[96];
	 *     if (fgets(buf, sizeof(buf), fp)) {
	 *         buf[strlen(buf)-1] = '\0';
	 *         sscanf(buf, "%f,%f,%f,%f", &rot, &moveX, &moveY, &scale);
	 *     }
	 *     fclose (fp);
	 * }
	 * printf("r=%f,sx=%f,sy=%f,sc=%f\n", rot, moveX, moveY, scale);
	 */

	jpg_decoder->transform = transform;
	if (transform == 1) {
		rot = 90;
		scale = width / height;
		moveX = height;
		moveY = (height / scale - width) / 2.0f;
	} else if (transform == 2) {
		rot = 180;
		moveX = width;
		moveY = height;
	} else if (transform == 3) {
		rot = 270;
		scale = width / height;
		moveY = height;
	} else {
		jpg_decoder->transform = 0;
	}

	rot = rot * M_PIf32 / 180.0f; // deg -> rad

	fill_matrix(0, 0, 0, scale, jpg_decoder->scale_matrix);
	fill_matrix(moveX, moveY, rot, 1.0f, jpg_decoder->angle_matrix);
}

void Decoder::rotate()
{
	setTransform(jpg_decoder->transform + 1);
}

/**
 * Move the context to the next frame
 * @return the frame to write to
 */
Buffer *Decoder::getNextFrame()
{
	while (jpg_decoder->m_BufferedFrames > bufferedFramesMax()) {
		jpg_decoder->m_BufferedFrames--;
		nextFrame = (nextFrame + 1) % JPG_BACKBUF_MAX;
	}
	if (jpg_decoder->m_BufferedFrames == bufferedFramesMax()) {
		dbgprint("\ndecoding #%2lud (have buffered: %d)\n", nextFrame, jpg_decoder->m_BufferedFrames);
		decodeNextFrame();
		jpg_decoder->m_BufferedFrames--;
		nextFrame = (nextFrame + 1) % JPG_BACKBUF_MAX;
	}

	/*
	 * a call to this function assumes we are about to get a full frame (or exit on failure).
	 * so increment the # of buffered frames. do this after the while() loop above to
	 * take care of the initial case:
	 */
	jpg_decoder->m_BufferedFrames++;

	int nextSlotSaved = jpg_decoder->m_NextSlot;
	jpg_decoder->m_NextSlot = (jpg_decoder->m_NextSlot < (JPG_BACKBUF_MAX - 1)) ? (jpg_decoder->m_NextSlot + 1) : 0;
	// dbgprint("next image going to #%2d (have buffered: %d)\n", nextSlotSaved, (jpg_decoder.m_BufferedFrames-1));
	return &jpg_frames[nextSlotSaved];
}

unsigned int Decoder::loopbackWidth()
{ return _loopbackWidth; }

unsigned int Decoder::loopbackHeight()
{ return _loopbackHeight; }

unsigned int Decoder::srcWidth()
{ return jpg_decoder->dinfo.image_width; }

unsigned int Decoder::srcHeight()
{ return jpg_decoder->dinfo.image_height; }


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
		                     SWS_FAST_BILINEAR /* flags */, NULL, NULL, NULL);

	} else {
		swc = sws_getCachedContext(swc,
		                           srcWidth, srcHeight, AV_PIX_FMT_YUV420P, /* src */
		                           dstWidth, dstHeight, AV_PIX_FMT_YUV420P, /* dst */
		                           SWS_FAST_BILINEAR /* flags */, NULL, NULL, NULL);

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
