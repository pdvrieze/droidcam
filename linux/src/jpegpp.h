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

class UncompressedFrame;

struct Dimension;

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
	void ensureBuffer(const struct jpeg_decompress_struct &dinfo);

public: // TODO make private
	JSAMPLE *buffer = 0;

private:
	unsigned int last_numComponents = 0;
	unsigned int last_compHeight = 0;
	unsigned int lastCompWidths[MAX_COMPONENTS];
	JSAMPROW *componentOffsets[MAX_COMPONENTS];
	size_t bufferAllocSize = 0;

	friend bool Jpeg::decodeFrame(UncompressedFrame &, const Buffer &);
};

#endif //DROIDCAM_JPEGPP_H
