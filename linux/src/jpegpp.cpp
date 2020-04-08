//
// Created by pdvrieze on 07/04/2020.
//

#include "jpegpp.h"

extern "C" {
#include <turbojpeg.h>
}

#include "util.h"
#include "common.h"
#include "context.h"

static Jpeg *currentJpeg;

#define PAD(v, p) ((v+(p)-1)&(~((p)-1)))


Jpeg::Jpeg() : cinfo(), jerr{}, fatal_error(0)
{
	currentJpeg = this;
	cinfo.err = jpeg_std_error(&jerr);
	jerr.output_message = ::joutput_message;
	jerr.error_exit = ::jerror_exit;
	jpeg_create_decompress(&cinfo);
}

Jpeg::~Jpeg()
{
	jpeg_destroy_decompress(&cinfo);
}


Dimension Jpeg::frameDimensions(const Buffer &frame)
{
	jpeg_mem_src(&cinfo, frame.data, frame.data_length);
	jpeg_read_header(&cinfo, 1);
	jpeg_abort_decompress(&cinfo);

	return Dimension{cinfo.image_width, cinfo.image_height};
}

void joutput_message(j_common_ptr cinfo)
{
	currentJpeg->joutput_message(cinfo);
}

void Jpeg::joutput_message(j_common_ptr cinfo)
{
	char buffer[JMSG_LENGTH_MAX];
	(*cinfo->err->format_message)(cinfo, buffer);
	dbgprint("JERR: %s\n", buffer);
}

void jerror_exit(j_common_ptr cinfo)
{
	currentJpeg->jerror_exit(cinfo);
}

void Jpeg::jerror_exit(j_common_ptr cinfo)
{
	dbgprint("jerror_exit(), fatal error\n");
	fatal_error = 1;
	(*cinfo->err->output_message)(cinfo);
}

bool Jpeg::decodeFrame(UncompressedFrame &out, const Buffer &in)
{
	struct jpeg_decompress_struct *dinfo = &cinfo;
	const Buffer &frame = in;
	unsigned char *p = frame.data;
	unsigned long len = frame.data_length;


	jpeg_mem_src(dinfo, p, len);
	jpeg_read_header(dinfo, true);
	if (fatal_error) return false;

	dinfo->raw_data_out = 1;
	dinfo->do_fancy_upsampling = false;
	dinfo->dct_method = JDCT_FASTEST;
	dinfo->out_color_space = JCS_YCbCr;


	// Get our dimensions
	jpeg_calc_output_dimensions(dinfo);
	if (fatal_error) {
		jpeg_abort_decompress(dinfo);
		return false;
	}

	out.ensureBuffer(cinfo);

	jpeg_start_decompress(dinfo);
	if (fatal_error) {
		jpeg_abort_decompress(dinfo);
		return false;
	}

	for (int row = 0; row < (int) dinfo->output_height; row += dinfo->max_v_samp_factor * DCTSIZE) {
		JSAMPARRAY yuvptr[dinfo->num_components];
		int componentRow[dinfo->num_components];

		for (int i = 0; i < dinfo->num_components; i++) {
			jpeg_component_info *compptr = &dinfo->comp_info[i];
			componentRow[i] = row * compptr->v_samp_factor / dinfo->max_v_samp_factor;
			yuvptr[i] = &out.componentOffsets[i][componentRow[i]];
		}
		jpeg_read_raw_data(dinfo, yuvptr, dinfo->max_v_samp_factor * DCTSIZE);
	}
	jpeg_finish_decompress(dinfo);


	return true;
}

UncompressedFrame::UncompressedFrame(Dimension size)
{
	_size = size;
	size_t ySize = size.width * size.height;
	size_t uvSize = ySize/4;
	size_t yuv420size = (ySize*3)/2;
	_frameSize = yuv420size* sizeof(JSAMPLE);
}


void UncompressedFrame::ensureYuv420Buffer(Dimension size)
{
	_size = size;
	size_t neededBuffer = (size.width * size.height *3)/2;
	if (_frameSize!=neededBuffer || bufferAllocSize<neededBuffer) {
		buffer = new JSAMPLE[neededBuffer];
		bufferAllocSize = neededBuffer;
		_frameSize=neededBuffer;
		last_numComponents=0;
		last_compHeight=0;
	}
}


void UncompressedFrame::ensureBuffer(Dimension size, unsigned int numComponents, Dimension componentBlocks[])
{
	_size = size;
	unsigned int compWidths[MAX_COMPONENTS]; memset(compWidths, 0, sizeof(compWidths));
	unsigned int compHeight[MAX_COMPONENTS]; memset(compHeight, 0, sizeof(compHeight));
	size_t neededBuffer = 0;


	{
		size_t ySize = size.width * size.height;
		size_t uvSize = ySize/4;
		size_t yuv420size = (ySize*3)/2;
		neededBuffer = yuv420size* sizeof(JSAMPLE);
		_frameSize = neededBuffer;
	}

	for (int i = 0; i < numComponents; i++) {
		compWidths[i] = componentBlocks[i].width * DCTSIZE;
		compHeight[i] = componentBlocks[i].height * DCTSIZE;
		for (int row = 0; row < compHeight[i]; row++) {
			neededBuffer += PAD(compWidths[i], 4);
		}
	}


	boolean needToReconfigure = last_numComponents != numComponents ||
	                            last_compHeight != compHeight[0];

	if (neededBuffer > bufferAllocSize) {
		delete buffer;

		// Get bigger amount just to avoid repeated reallocation
		unsigned int toAllocate = 1024;
		while (toAllocate < neededBuffer) { toAllocate <<= 1u; }

		buffer = new JSAMPLE[toAllocate];
		bufferAllocSize = toAllocate;
		needToReconfigure = true;
	}

	for (int component = 0; !needToReconfigure && component < numComponents; ++component) {
		needToReconfigure = needToReconfigure || lastCompWidths[component] != compWidths[component];
	}


	if (needToReconfigure) {
		JSAMPLE *ptr = buffer;
		for (int component = 0; component < numComponents; component++) {
			delete componentOffsets[component];
			componentOffsets[component] = new JSAMPROW[compHeight[component]];
			for (int row = 0; row < compHeight[component]; row++) {
				componentOffsets[component][row] = ptr;
				ptr += PAD(compWidths[component], 4);
			}
		}
		for(int i=0; i<MAX_COMPONENTS; ++i) { lastCompWidths[i] = compWidths[i]; }
		last_compHeight = compHeight[0];
		last_numComponents = numComponents;
	}

}

void UncompressedFrame::ensureBuffer(const struct jpeg_decompress_struct &dinfo)
{
	Dimension componentBlocks[dinfo.num_components];
	for (int i=0; i<dinfo.num_components; ++i) {
		componentBlocks[i] = { dinfo.comp_info[i].width_in_blocks, dinfo.comp_info[i].height_in_blocks };
	}
	ensureBuffer(Dimension{dinfo.output_width, dinfo.output_height}, dinfo.num_components, componentBlocks);
/*
	unsigned int compWidths[MAX_COMPONENTS]; memset(compWidths, 0, sizeof(compWidths));
	unsigned int compHeight[MAX_COMPONENTS]; memset(compHeight, 0, sizeof(compHeight));
	size_t neededBuffer = 0;


	{
		size_t ySize = dinfo.output_width * dinfo.output_height;
		size_t uvSize = ySize/4;
		size_t yuv420size = (ySize*3)/2;
		neededBuffer = yuv420size* sizeof(JSAMPLE);
		_frameSize = neededBuffer;
	}

	for (int i = 0; i < dinfo.num_components; i++) {
		compWidths[i] = dinfo.comp_info[i].width_in_blocks * DCTSIZE;
		compHeight[i] = dinfo.comp_info[i].height_in_blocks * DCTSIZE;
		for (int row = 0; row < compHeight[i]; row++) {
			neededBuffer += PAD(compWidths[i], 4);
		}
	}


	boolean needToReconfigure = last_numComponents != dinfo.num_components ||
	                            last_compHeight != compHeight[0];

	if (neededBuffer > bufferAllocSize) {
		delete buffer;

		// Get bigger amount just to avoid repeated reallocation
		unsigned int toAllocate = 1024;
		while (toAllocate < neededBuffer) { toAllocate <<= 1u; }

		buffer = new JSAMPLE[toAllocate];
		bufferAllocSize = toAllocate;
		needToReconfigure = true;
	}

	for (int component = 0; !needToReconfigure && component < dinfo.num_components; ++component) {
		needToReconfigure = needToReconfigure || lastCompWidths[component] != compWidths[component];
	}


	if (needToReconfigure) {
		JSAMPLE *ptr = buffer;
		for (int component = 0; component < dinfo.num_components; component++) {
			delete componentOffsets[component];
			componentOffsets[component] = new JSAMPROW[compHeight[component]];
			for (int row = 0; row < compHeight[component]; row++) {
				componentOffsets[component][row] = ptr;
				ptr += PAD(compWidths[component], 4);
			}
		}
		for(int i=0; i<MAX_COMPONENTS; ++i) { lastCompWidths[i] = compWidths[i]; }
		last_compHeight = compHeight[0];
		last_numComponents = dinfo.num_components;
	}
*/

}

UncompressedFrame::UncompressedFrame()
{
	memset(componentOffsets, 0, sizeof(componentOffsets));
	memset(lastCompWidths, 0, sizeof(lastCompWidths));
}

UncompressedFrame::~UncompressedFrame()
{
	delete buffer;
	memset(componentOffsets, 0, sizeof(componentOffsets));
}

