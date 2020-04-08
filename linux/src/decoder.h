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
#include "jpegpp.h"

class Jpeg;
class Scaler;

enum class Transform: unsigned int {
	DEG0 = 0,
	DEG90,
	DEG180,
	DEG270
};

class Decoder {
public:

	Decoder();
	~Decoder();

	unsigned int loopbackWidth();
	unsigned int loopbackHeight();

	OutputMode outputMode() {return _outputMode; };

	void bufferedFramesMax(unsigned int newVal);

	bool initLoopback(unsigned int targetWidth, unsigned int targetHeight);
	bool init();

	bool prepareVideo(const char *header);
	bool prepareVideoFromFrame(Buffer *data);
	bool prepareVideo(unsigned int srcWidth, unsigned int srcHeight);

	void rotate();

	void putNextFrame(Buffer &frame);
	Buffer *getNextFrame();
	void showTestImage();

	void cleanupJpeg();

public: // TODO make this private
	Buffer jpg_frames[JPG_BACKBUF_MAX];
	size_t nextFrameToDisplay;
	size_t nextFrameToStore;
	std::unique_ptr<JpgDecContext> jpg_decoder;
	UncompressedFrame jpegOutput;

	void publishFrameToLoopback(const UncompressedFrame &jpegOutput);
	Transform transform() { return _transform; }
	void setTransform(Transform transform);

private:
	OutputMode _outputMode;
	int _deviceFd;

	unsigned int bufferedFramesMax() { return _bufferedFramesMax; }
	unsigned int bufferedFramesCnt() { return _bufferedFramesCnt; }

	unsigned int _bufferedFramesMax;
	unsigned int _bufferedFramesCnt=0;
	unsigned int _loopbackWidth;
	unsigned int _loopbackHeight;
	Transform _transform = Transform::DEG0;
	float scale_matrix[9];
	float angle_matrix[9];


	UncompressedFrame scalingResult;
	UncompressedFrame scratchBuffer;
	std::unique_ptr<Scaler> scaler;

	Jpeg &jpeg();

	OutputMode findOutputDevice();
	void query_droidcam_v4l();
	void decodeNextFrame();


	void apply_transform(UncompressedFrame &yuv420image, UncompressedFrame &scratch);
	std::unique_ptr<Jpeg> x_jpeg;
};

class Scaler {
public:
	Scaler();
	~Scaler();

	void scale(const UncompressedFrame &src, UncompressedFrame &dst);
private:

	struct SwsContext * swc = nullptr;
};

#endif
