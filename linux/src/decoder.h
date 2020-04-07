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

class Decoder {
public:

	Decoder();
	~Decoder();

	unsigned int dstWidth();
	unsigned int dstHeight();

	unsigned int srcWidth();
	unsigned int srcHeight();

	OutputMode outputMode() {return _outputMode; };

	unsigned int bufferedFramesMax() { return _bufferedFramesMax; }
	void bufferedFramesMax(unsigned int newVal);

	bool initLoopback(unsigned int width, unsigned int height);
	bool init();

	bool prepareVideo(const char *header);
	bool prepareVideoFromFrame(Buffer *data);
	bool prepareVideo(unsigned int srcWidth, int srcHeight);

	void rotate();

	Buffer *getNextFrame();
	void showTestImage();

	void cleanupJpeg();

public: // TODO make this private
	Buffer jpg_frames[JPG_BACKBUF_MAX];
	const JpgDecContext &jpg_decoder;
	std::unique_ptr<JpgDecContext> _jpg_decoder;

	void publishFrameToLoopback();
	void setTransform(int transform);

private:
	OutputMode _outputMode;
	int _deviceFd;
	unsigned int _bufferedFramesMax;

	OutputMode findOutputDevice();
	void query_droidcam_v4l();

};


#endif
