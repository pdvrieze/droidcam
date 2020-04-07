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
#include <climits>

extern "C" {
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <turbojpeg.h>

#include <jpeglib.h>
#include <libswscale/swscale.h>
// #include "speex/speex.h"
}

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


#define WEBCAM_Wf ((float)WEBCAM_W)
#define WEBCAM_Hf ((float)WEBCAM_H)
static unsigned int WEBCAM_W, WEBCAM_H;

#undef MAX_COMPONENTS
#define MAX_COMPONENTS  4

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


Decoder::Decoder() : _jpg_decoder(std::make_unique<JpgDecContext>()), _bufferedFramesMax(1), jpg_decoder(*_jpg_decoder)
{

}

Decoder::~Decoder()
{
	if (_deviceFd) close(_deviceFd);

	if (_jpg_decoder->init) {
		jpeg_destroy_decompress(&_jpg_decoder->dinfo);
		_jpg_decoder->init = FALSE;
	}
};

void joutput_message(j_common_ptr cinfo)
{
	char buffer[JMSG_LENGTH_MAX];
	(*cinfo->err->format_message)(cinfo, buffer);
	dbgprint("JERR: %s\n", buffer);
}

void jerror_exit(j_common_ptr cinfo)
{
	dbgprint("jerror_exit(), fatal error\n");
	fatal_error = 1;
	(*cinfo->err->output_message)(cinfo);
}

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
	MSG_ERROR("Device not found (/dev/video[0-9]).\nDid you install it?\n");
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

	WEBCAM_W = vid_format.fmt.pix.width;
	WEBCAM_H = vid_format.fmt.pix.height;
}

void Decoder::bufferedFramesMax(unsigned int newVal)
{
	if (newVal > JPG_BACKBUF_MAX) newVal = JPG_BACKBUF_MAX;
	else if (newVal < 1) newVal = 1;
	_bufferedFramesMax = newVal;
	dbgprint("buffer %d frames\n", _bufferedFramesMax);
}

bool Decoder::initLoopback(unsigned int width, unsigned int height)
{
	WEBCAM_W = width;
	WEBCAM_H = height;
	_jpg_decoder->m_webcamYuvSize = width * height * 3 / 2;
	_jpg_decoder->m_webcam_ySize = width * height;
	_jpg_decoder->m_webcam_uvSize = _jpg_decoder->m_webcam_ySize / 4;

	struct v4l2_format vid_format = {0};
	vid_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	vid_format.fmt.pix.width = width;
	vid_format.fmt.pix.height = height;
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
	return true;
}

bool Decoder::init()
{
	WEBCAM_W = 0;
	WEBCAM_H = 0;

	_outputMode = findOutputDevice();

	if (_outputMode == OutputMode::OM_MISSING)
		return false;


	if (_outputMode == OutputMode::OM_DROIDCAM) { // droidcam mode
		query_droidcam_v4l();
		dbgprint("WEBCAM_W=%d, WEBCAM_H=%d\n", WEBCAM_W, WEBCAM_H);
		if (WEBCAM_W < 2 || WEBCAM_H < 2 || WEBCAM_W > 9999 || WEBCAM_H > 9999) {
			MSG_ERROR("Unable to query droidcam device for parameters");
			return false;
		}
	}

	fatal_error = 0;
	_jpg_decoder->dinfo.err = jpeg_std_error(&_jpg_decoder->jerr);
	_jpg_decoder->jerr.output_message = joutput_message;
	_jpg_decoder->jerr.error_exit = jerror_exit;
	jpeg_create_decompress(&_jpg_decoder->dinfo);

	if (fatal_error) return false;
	_jpg_decoder->init = TRUE;
	_jpg_decoder->subsamp = TJSAMP_NIL;
	_jpg_decoder->m_webcamYuvSize = WEBCAM_W * WEBCAM_H * 3 / 2;
	_jpg_decoder->m_webcam_ySize = WEBCAM_W * WEBCAM_H;
	_jpg_decoder->m_webcam_uvSize = jpg_decoder.m_webcam_ySize / 4;
	_jpg_decoder->transform = 0;
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
	auto &dec = *_jpg_decoder;
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

bool Decoder::prepareVideo(unsigned int srcWidth, int srcHeight)
{

	if (srcWidth <= 0 || srcHeight <= 0) {
		MSG_ERROR("Invalid data stream!");
		return false;
	}

	int i;
	const auto &dec = _jpg_decoder;

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

	unsigned long dstWidth, dstHeight;
	if (WEBCAM_W == 0 || WEBCAM_H == 0) {
		WEBCAM_W = srcWidth;
		WEBCAM_H = srcHeight;
		dstWidth = srcWidth;
		dstHeight = dstHeight;
	} else {
		dstWidth = WEBCAM_W;
		dstHeight = WEBCAM_H;
	}

	if (dec->m_webcamYuvSize != dec->m_Yuv420Size) {
		dbgprint("Updating cached sws context from %p to ", dec->swc);
		dec->m_webcamBuf = (unsigned char *) malloc(dec->m_webcamYuvSize * sizeof(unsigned char));
		dec->swc = sws_getCachedContext(dec->swc,
		                                (int) srcWidth, (int) srcHeight, AV_PIX_FMT_YUV420P, /* src */
		                                (int) dstWidth, (int) dstHeight, AV_PIX_FMT_YUV420P, /* dst */
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

	dec->m_BufferedFrames = dec->m_NextFrame = dec->m_NextSlot = 0;
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
	for (auto & i : jpgCtx->jpg_decoder.outbuf) {
		free(i);
	}

	for (int i = 0; i < JPG_BACKBUF_MAX; ++i) {
		jpgCtx->jpg_frames->data = nullptr;
	}

	doFree(jpgCtx->_jpg_decoder->m_inBuf, std::function(free));
	FREE_OBJECT(jpgCtx->_jpg_decoder->m_inBuf, free)
	FREE_OBJECT(jpgCtx->_jpg_decoder->m_decodeBuf, free)
	FREE_OBJECT(jpgCtx->_jpg_decoder->m_webcamBuf, free)
	FREE_OBJECT(jpgCtx->_jpg_decoder->scratchBuf, free)
	FREE_OBJECT(jpgCtx->_jpg_decoder->swc, sws_freeContext)
	// TODO just have a reset
	memset(&(*(jpgCtx->_jpg_decoder)), 0, sizeof(JpgDecContext));
}

static void decode_next_frame(Decoder *jpgCtx)
{
	struct jpeg_decompress_struct *dinfo = &jpgCtx->_jpg_decoder->dinfo;
	unsigned char *p = jpgCtx->jpg_frames[jpgCtx->_jpg_decoder->m_NextFrame].data;
	unsigned long len = (unsigned long) jpgCtx->jpg_frames[jpgCtx->_jpg_decoder->m_NextFrame].data_length;

//	int k, row =0;
	gboolean usetmpbuf = FALSE;
	JSAMPLE *ptr = jpgCtx->jpg_decoder.m_decodeBuf;

	jpeg_mem_src(dinfo, p, len);
	jpeg_read_header(dinfo, TRUE);
	if (fatal_error) return;
	dinfo->raw_data_out = TRUE;
	dinfo->do_fancy_upsampling = FALSE;
	dinfo->dct_method = JDCT_FASTEST;
	dinfo->out_color_space = JCS_YCbCr;

	if (jpgCtx->jpg_decoder.subsamp == TJSAMP_NIL) {
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
			jpgCtx->_jpg_decoder->subsamp = retval;
		} else {
			jpgCtx->_jpg_decoder->subsamp = TJSAMP_UNK;
		}
	}

	if (jpgCtx->jpg_decoder.subsamp != TJSAMP_420) {
		fprintf(stderr, "Error: Unexpected video image stream subsampling\n");
		jpeg_abort_decompress(dinfo);
		return;
	}

	if (jpgCtx->jpg_decoder.outbuf[0] == NULL) {
		int ih;
		int *cw = jpgCtx->_jpg_decoder->cw;
		int *ch = jpgCtx->_jpg_decoder->ch;
		int *iw = jpgCtx->_jpg_decoder->iw;
		int *th = jpgCtx->_jpg_decoder->th;
		JSAMPROW **outbuf = jpgCtx->_jpg_decoder->outbuf;
		for (int i = 0; i < dinfo->num_components; i++) {
			jpeg_component_info *compptr = &dinfo->comp_info[i];
			iw[i] = compptr->width_in_blocks * DCTSIZE;
			ih = compptr->height_in_blocks * DCTSIZE;
			cw[i] =
				PAD(dinfo->image_width, dinfo->max_h_samp_factor) * compptr->h_samp_factor / dinfo->max_h_samp_factor;
			ch[i] =
				PAD(dinfo->image_height, dinfo->max_v_samp_factor) * compptr->v_samp_factor / dinfo->max_v_samp_factor;
			if (iw[i] != cw[i] || ih != ch[i]) {
				usetmpbuf = 1;
				fprintf(stderr, "error: need a temp buffer, this shouldnt happen!\n");
				jpgCtx->_jpg_decoder->subsamp = TJSAMP_UNK;
			}
			th[i] = compptr->v_samp_factor * DCTSIZE;

			dbgprint("extra alloc: %d\n", (int) (sizeof(JSAMPROW) * ch[i]));
			if ((outbuf[i] = (JSAMPROW *) malloc(sizeof(JSAMPROW) * ch[i])) == NULL) {
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

	if ((int) dinfo->output_width != jpgCtx->jpg_decoder.srcWidth ||
	    (int) dinfo->output_height != jpgCtx->jpg_decoder.srcHeight) {
		dbgprint("error: jpgCtx output %dx%d differs from expected %dx%d size\n",
		         dinfo->output_width, dinfo->output_height, jpgCtx->jpg_decoder.srcWidth,
		         jpgCtx->jpg_decoder.srcHeight);
		jpeg_abort_decompress(&jpgCtx->_jpg_decoder->dinfo);
		return;
	}

	for (int row = 0; row < (int) dinfo->output_height; row += dinfo->max_v_samp_factor * DCTSIZE) {
		JSAMPARRAY yuvptr[MAX_COMPONENTS];
		int crow[MAX_COMPONENTS];
		for (int i = 0; i < dinfo->num_components; i++) {
			jpeg_component_info *compptr = &dinfo->comp_info[i];
			crow[i] = row * compptr->v_samp_factor / dinfo->max_v_samp_factor;
			yuvptr[i] = &jpgCtx->jpg_decoder.outbuf[i][crow[i]];
		}
		jpeg_read_raw_data(dinfo, yuvptr, dinfo->max_v_samp_factor * DCTSIZE);
	}
	jpeg_finish_decompress(dinfo);
	jpgCtx->publishFrameToLoopback();
}

static void apply_transform_helper(const uint8_t *src, uint8_t *dst,
                                   unsigned int width, unsigned int height, int transformNull, const float *matrix)
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

			d = PIXEL(src, (int) (x_s + 0.5), (int) (y_s + 0.5), width, height, width, 0);
			dst[y * width + x] = (transformNull > 0 && d == 0) ? transformNull : d;
		}
	}
}

/* scratch is a working buffer of 2*ySize (2 * w * h) length */
static void apply_transform(Decoder *jpgCtx, unsigned char *yuv420image, unsigned char *scratch)
{

	// Transform Y component as is
	apply_transform_helper(yuv420image, scratch,
	                       WEBCAM_W, WEBCAM_H, 0,
	                       jpgCtx->jpg_decoder.scale_matrix);

	apply_transform_helper(scratch, yuv420image,
	                       WEBCAM_W, WEBCAM_H, 0,
	                       jpgCtx->jpg_decoder.angle_matrix);

	// Expand U component, transform, then sub-sample back down
	int row, col;
	unsigned char *p = &yuv420image[jpgCtx->jpg_decoder.m_webcam_ySize];
	unsigned char *d = &scratch[jpgCtx->jpg_decoder.m_webcam_ySize];

	for (row = 0; row < WEBCAM_H; row += 2) {
		for (col = 0; col < WEBCAM_W; col += 2) {
			unsigned char u = *p++;
			scratch[(row + 0) * WEBCAM_W + col + 0] = u;
			scratch[(row + 0) * WEBCAM_W + col + 1] = u;
			scratch[(row + 1) * WEBCAM_W + col + 0] = u;
			scratch[(row + 1) * WEBCAM_W + col + 1] = u;
		}
	}

	apply_transform_helper(scratch, d,
	                       WEBCAM_W, WEBCAM_H, 0,
	                       jpgCtx->jpg_decoder.scale_matrix);

	apply_transform_helper(d, scratch,
	                       WEBCAM_W, WEBCAM_H, 128,
	                       jpgCtx->jpg_decoder.angle_matrix);

	p = &yuv420image[jpgCtx->jpg_decoder.m_webcam_ySize];
	for (row = 0; row < WEBCAM_H; row += 2) {
		for (col = 0; col < WEBCAM_W; col += 2) {
			*p++ = scratch[row * WEBCAM_W + col];
		}
	}

	// Expand V component, transform, then sub-sample back down
	p = &yuv420image[jpgCtx->jpg_decoder.m_webcam_ySize + jpgCtx->jpg_decoder.m_webcam_uvSize];
	d = &scratch[jpgCtx->jpg_decoder.m_webcam_ySize];

	for (row = 0; row < WEBCAM_H; row += 2) {
		for (col = 0; col < WEBCAM_W; col += 2) {
			unsigned char v = *p++;
			scratch[(row + 0) * WEBCAM_W + col + 0] = v;
			scratch[(row + 0) * WEBCAM_W + col + 1] = v;
			scratch[(row + 1) * WEBCAM_W + col + 0] = v;
			scratch[(row + 1) * WEBCAM_W + col + 1] = v;
		}
	}
	apply_transform_helper(scratch, d,
	                       WEBCAM_W, WEBCAM_H, 0,
	                       jpgCtx->jpg_decoder.scale_matrix);

	apply_transform_helper(d, scratch,
	                       WEBCAM_W, WEBCAM_H, 128,
	                       jpgCtx->jpg_decoder.angle_matrix);

	p = &yuv420image[jpgCtx->jpg_decoder.m_webcam_ySize + jpgCtx->jpg_decoder.m_webcam_uvSize];
	for (row = 0; row < WEBCAM_H; row += 2) {
		for (col = 0; col < WEBCAM_W; col += 2) {
			*p++ = scratch[row * WEBCAM_W + col];
		}
	}
}

void Decoder::publishFrameToLoopback()
{
	unsigned char *p = jpg_decoder.m_decodeBuf;
	if (jpg_decoder.swc != nullptr) {
		uint8_t *srcSlice[4];
		uint8_t *dstSlice[4];

		int srcStride[4] = {
			static_cast<int>(jpg_decoder.srcWidth),
			static_cast<int>(jpg_decoder.srcWidth >> 1u),
			static_cast<int>(jpg_decoder.srcWidth >> 1u),
			0};
		int dstStride[4] = {
			static_cast<int>(dstWidth()),
			static_cast<int>(dstWidth() >> 1u),
			static_cast<int>(dstWidth() >> 1u),
			0};

		srcSlice[0] = &jpg_decoder.m_decodeBuf[0];
		srcSlice[1] = srcSlice[0] + jpg_decoder.m_ySize;
		srcSlice[2] = srcSlice[1] + jpg_decoder.m_uvSize;
		srcSlice[3] = nullptr;
		dstSlice[0] = &jpg_decoder.m_webcamBuf[0];
		dstSlice[1] = dstSlice[0] + jpg_decoder.m_webcam_ySize;
		dstSlice[2] = dstSlice[1] + jpg_decoder.m_webcam_uvSize;
		dstSlice[3] = nullptr;


		sws_scale(jpg_decoder.swc, srcSlice, srcStride, 0, jpg_decoder.srcHeight, dstSlice,
		          dstStride);
		p = jpg_decoder.m_webcamBuf;
	}

	// todo: This is currently super inefficient unfortunately :(
	if (jpg_decoder.transform != 0) {
		apply_transform(this, p, jpg_decoder.scratchBuf);
	}

	write(_deviceFd, p, jpg_decoder.m_webcamYuvSize);

}

void Decoder::showTestImage()
{
	int i, j;
	if (outputMode() == OutputMode::OM_V4LLOOPBACK) {
		WEBCAM_H = 480;
		WEBCAM_W = 640;
		if (!initLoopback(WEBCAM_W, WEBCAM_H)) return;
	}

	unsigned int m_height = WEBCAM_H * 2;
	unsigned int m_width = WEBCAM_W * 2;

	if (!prepareVideo(m_width, m_height)) return;

	// [ jpg ] -> [ yuv420 ] -> [ yuv420 scaled ] -> [ yuv420 webcam transformed ]

	// fill in "decoded" data
	unsigned char *p = jpg_decoder.m_decodeBuf;
	memset(p, 128, jpg_decoder.m_Yuv420Size);
	for (j = 0; j < m_height; j++) {
		unsigned char *line_end = p + m_width;
		for (i = 0; i < (m_width / 4); i++) {
			*p++ = 0;
		}
		for (i = 0; i < (m_width / 4); i++) {
			*p++ = 64;
		}
		for (i = 0; i < (m_width / 4); i++) {
			*p++ = 128;
		}
		for (i = 0; i < (m_width / 4); i++) {
			*p++ = rand() % 250;
		}
		while (p < line_end) p++;
	}

	publishFrameToLoopback();
	rotate();

}

void Decoder::setTransform(int transform) {
	float scale = 1.0f;
	float moveX = 0;
	float moveY = 0;
	float rot = 0;

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

	_jpg_decoder->transform = transform;
	if (transform == 1) {
		rot = 90;
		scale = WEBCAM_Wf / WEBCAM_Hf;
		moveX = WEBCAM_Hf;
		moveY = (WEBCAM_Hf / scale - WEBCAM_Wf) / 2.0f;
	} else if (transform == 2) {
		rot = 180;
		moveX = WEBCAM_Wf;
		moveY = WEBCAM_Hf;
	} else if (transform == 3) {
		rot = 270;
		scale = WEBCAM_Wf / WEBCAM_Hf;
		moveY = WEBCAM_Hf;
	} else {
		_jpg_decoder->transform = 0;
	}

	rot = rot * M_PIf32 / 180.0f; // deg -> rad

	fill_matrix(0, 0, 0, scale, _jpg_decoder->scale_matrix);
	fill_matrix(moveX, moveY, rot, 1.0f, _jpg_decoder->angle_matrix);
}

void Decoder::rotate()
{
	setTransform(jpg_decoder.transform + 1);
}

/**
 * Move the context to the next frame
 * @return the frame to write to
 */
Buffer *Decoder::getNextFrame()
{
	while (_jpg_decoder->m_BufferedFrames > bufferedFramesMax()) {
		_jpg_decoder->m_BufferedFrames--;
		_jpg_decoder->m_NextFrame = (_jpg_decoder->m_NextFrame < (JPG_BACKBUF_MAX - 1)) ? (_jpg_decoder->m_NextFrame + 1)
		                                                                                : 0;
	}
	if (_jpg_decoder->m_BufferedFrames == bufferedFramesMax()) {
		dbgprint("decoding #%2d (have buffered: %d)\n", _jpg_decoder->m_NextFrame, _jpg_decoder->m_BufferedFrames);
		decode_next_frame(this);
		_jpg_decoder->m_BufferedFrames--;
		_jpg_decoder->m_NextFrame = (_jpg_decoder->m_NextFrame < (JPG_BACKBUF_MAX - 1)) ? (_jpg_decoder->m_NextFrame + 1)
		                                                                                : 0;
	}

	/*
	 * a call to this function assumes we are about to get a full frame (or exit on failure).
	 * so increment the # of buffered frames. do this after the while() loop above to
	 * take care of the initial case:
	 */
	_jpg_decoder->m_BufferedFrames++;

	int nextSlotSaved = _jpg_decoder->m_NextSlot;
	_jpg_decoder->m_NextSlot = (_jpg_decoder->m_NextSlot < (JPG_BACKBUF_MAX - 1)) ? (_jpg_decoder->m_NextSlot + 1) : 0;
	// dbgprint("next image going to #%2d (have buffered: %d)\n", nextSlotSaved, (jpg_decoder.m_BufferedFrames-1));
	return &jpg_frames[nextSlotSaved];
}

unsigned int Decoder::dstWidth() { return WEBCAM_W; }

unsigned int Decoder::dstHeight() { return WEBCAM_H; }

unsigned int Decoder::srcWidth() { return jpg_decoder.dinfo.image_width; }

unsigned int Decoder::srcHeight() { return jpg_decoder.dinfo.image_height; }


