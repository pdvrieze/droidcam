//
// Created by pdvrieze on 07/04/2020.
//

#ifndef DROIDCAM_JPEGPP_H
#define DROIDCAM_JPEGPP_H

#include <cstddef>
#include <cstdio>

extern "C" {
//#include <turbojpeg.h>

#include <jpeglib.h>
}

#include "common.h"

#undef MAX_COMPONENTS
#define MAX_COMPONENTS 4

class UncompressedFrame;

class Buffer;

void jerror_exit(j_common_ptr cinfo);

void joutput_message(j_common_ptr cinfo);

class Jpeg {
public:
	Jpeg();

	~Jpeg();

	Dimension frameDimensions(const Buffer &frame);

	bool decodeFrame(UncompressedFrame &out, const Buffer &in);

private:
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	int fatal_error;

	void jerror_exit(j_common_ptr cinfo);

	void joutput_message(j_common_ptr cinfo);

	friend void::jerror_exit(j_common_ptr);

	friend void::joutput_message(j_common_ptr);
};

class UncompressedFrame {
public:
	UncompressedFrame();
	explicit UncompressedFrame(Dimension size);
	~UncompressedFrame();

	void ensureBuffer(const struct jpeg_decompress_struct &dinfo);
	void ensureBuffer(Dimension size, unsigned int numComponents, Dimension componentBlocks[]);
	void ensureYuv420Buffer(Dimension size);

	[[nodiscard]] unsigned int frameSize() const { return _frameSize; };
	[[nodiscard]] Dimension size() const { return _size; }

	[[nodiscard]] size_t ySize() const { return _size.width * _size.height; }

	[[nodiscard]] size_t uvSize() const { return ySize() / 4u; };

public: // TODO make private
	JSAMPLE *buffer = nullptr;

private:
	unsigned int last_numComponents = 0;
	unsigned int last_compHeight = 0;
	unsigned int lastCompWidths[MAX_COMPONENTS];
	JSAMPROW *componentOffsets[MAX_COMPONENTS];
	size_t bufferAllocSize = 0;
	unsigned int _frameSize = 0;
	Dimension _size;

	friend bool Jpeg::decodeFrame(UncompressedFrame &, const Buffer &);
};

#endif //DROIDCAM_JPEGPP_H
