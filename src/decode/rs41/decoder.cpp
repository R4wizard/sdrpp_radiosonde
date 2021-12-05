#include <string.h>
#include <iostream>
#include "decoder.hpp"
#include "decode/gps/ecef.h"
#include "rs41.h"
#include "utils.h"

/* Pseudorandom sequence, obtained by autocorrelating the extra data found at the end of frames
 * from a radiosonde with ozone sensor */
static const uint8_t _prn[RS41_PRN_PERIOD] = {
	0x96, 0x83, 0x3e, 0x51, 0xb1, 0x49, 0x08, 0x98,
	0x32, 0x05, 0x59, 0x0e, 0xf9, 0x44, 0xc6, 0x26,
	0x21, 0x60, 0xc2, 0xea, 0x79, 0x5d, 0x6d, 0xa1,
	0x54, 0x69, 0x47, 0x0c, 0xdc, 0xe8, 0x5c, 0xf1,
	0xf7, 0x76, 0x82, 0x7f, 0x07, 0x99, 0xa2, 0x2c,
	0x93, 0x7c, 0x30, 0x63, 0xf5, 0x10, 0x2e, 0x61,
	0xd0, 0xbc, 0xb4, 0xb6, 0x06, 0xaa, 0xf4, 0x23,
	0x78, 0x6e, 0x3b, 0xae, 0xbf, 0x7b, 0x4c, 0xc1
};


RS41Decoder::RS41Decoder()
{
	_rs = correct_reed_solomon_create(RS41_REEDSOLOMON_POLY,
	                                  RS41_REEDSOLOMON_FIRST_ROOT,
	                                  RS41_REEDSOLOMON_ROOT_SKIP,
	                                  RS41_REEDSOLOMON_T);
}

RS41Decoder::RS41Decoder(dsp::stream<uint8_t> *in, void (*handler)(SondeData *data, void *ctx), void *ctx)
{
	init(in, handler, ctx);
}

RS41Decoder::~RS41Decoder()
{
	if (!generic_block<RS41Decoder>::_block_init) return;
	generic_block<RS41Decoder>::stop();
	generic_block<RS41Decoder>::_block_init = false;
}

void
RS41Decoder::init(dsp::stream<uint8_t> *in, void (*handler)(SondeData *data, void *ctx), void *ctx)
{
	_in = in;
	_ctx = ctx;
	_handler = handler;
	_rs = correct_reed_solomon_create(RS41_REEDSOLOMON_POLY,
	                                  RS41_REEDSOLOMON_FIRST_ROOT,
	                                  RS41_REEDSOLOMON_ROOT_SKIP,
	                                  RS41_REEDSOLOMON_T);
	memset(_calibDataBitmap, 0xFF, sizeof(_calibDataBitmap));
	_calibDataBitmap[sizeof(_calibDataBitmap)-1] &= ~(1 << (7 - RS41_CALIB_FRAGCOUNT%8)) - 1;


	generic_block<RS41Decoder>::registerInput(_in);
	generic_block<RS41Decoder>::_block_init = true;
}

void
RS41Decoder::setInput(dsp::stream<uint8_t>* in)
{
	generic_block<RS41Decoder>::tempStop();
	generic_block<RS41Decoder>::unregisterInput(_in);
	_in = in;
	generic_block<RS41Decoder>::registerInput(_in);
	generic_block<RS41Decoder>::tempStart();
}

int
RS41Decoder::run()
{
	SondeData sondeData;
	RS41Frame *frame;
	RS41Subframe *subframe;
	int offset, outCount, numFrames, bytesLeft;

	assert(generic_block<RS41Decoder>::_block_init);
	if ((numFrames = _in->read()) < 0) return -1;

	outCount = 0;
	numFrames /= sizeof(*frame);

	/* For each frame that was received */
	for (int i=0; i<numFrames; i++) {
		frame = (RS41Frame*)(_in->readBuf + i*sizeof(*frame));

		/* Descramble and error correct */
		descramble(frame);
		if (_rs) rsCorrect(frame);

		bytesLeft = RS41_DATA_LEN + (frame->extended_flag == RS41_FLAG_EXTENDED ? RS41_XDATA_LEN : 0);
		offset = 0;
		while (offset < bytesLeft) {
			subframe = (RS41Subframe*)&frame->data[offset];
			offset += subframe->len + 4;

			/* Verify that end of the subframe is still within bounds */
			if (offset > bytesLeft) break;

			/* Check subframe checksum */
			if (!crcCheck(subframe)) continue;

			/* Update the generic info struct with the data inside the subframe */
			updateSondeData(&sondeData, subframe);
		}

		_handler(&sondeData, _ctx);
		outCount++;
	}

	_in->flush();
	return outCount;
}

/* Private methods {{{ */
void
RS41Decoder::descramble(RS41Frame *frame)
{
	int i, j;
	uint8_t tmp;
	uint8_t *rawFrame = (uint8_t*) frame;

	/* Reorder bits in the frame and XOR with PRN sequence */
	for (i=0; i<sizeof(*frame); i++) {
		tmp = 0;
		for (int j=0; j<8; j++) {
			tmp |= ((rawFrame[i] >> (7-j)) & 0x1) << j;
		}
		rawFrame[i] = 0xFF ^ tmp ^ _prn[i % RS41_PRN_PERIOD];
	}
}

bool
RS41Decoder::rsCorrect(RS41Frame *frame)
{
	bool valid;
	int i, block, chunk_len;
	uint8_t rsBlock[RS41_REEDSOLOMON_N];

	if (frame->extended_flag != RS41_FLAG_EXTENDED) {
		chunk_len = (RS41_DATA_LEN + 1) / RS41_REEDSOLOMON_INTERLEAVING;
		memset(rsBlock, 0, RS41_REEDSOLOMON_N);
	} else {
		chunk_len = RS41_REEDSOLOMON_K;
	}

	valid = true;
	for (block=0; block<RS41_REEDSOLOMON_INTERLEAVING; block++) {
		/* Deinterleave */
		for (i=0; i<chunk_len; i++) {
			rsBlock[i] = frame->data[RS41_REEDSOLOMON_INTERLEAVING*i + block - 1];
		}
		for (i=0; i<RS41_REEDSOLOMON_T; i++) {
			rsBlock[RS41_REEDSOLOMON_K+i] = frame->rs_checksum[i + RS41_REEDSOLOMON_T*block];
		}

		/* Error correct */
		if (correct_reed_solomon_decode(_rs, rsBlock, RS41_REEDSOLOMON_N, rsBlock) < 0) valid = false;

		/* Reinterleave */
		for (i=0; i<chunk_len; i++) {
			frame->data[RS41_REEDSOLOMON_INTERLEAVING*i + block - 1] = rsBlock[i];
		}
		for (i=0; i<RS41_REEDSOLOMON_T; i++) {
			frame->rs_checksum[i + RS41_REEDSOLOMON_T*block] = rsBlock[RS41_REEDSOLOMON_K+i];
		}
	}

	return valid;
}

bool
RS41Decoder::crcCheck(RS41Subframe *subframe)
{
	uint16_t checksum = crc16(CCITT_FALSE_POLY, CCITT_FALSE_INIT, subframe->data, subframe->len);
	uint16_t expected = subframe->data[subframe->len] | subframe->data[subframe->len+1] << 8;

	return checksum == expected;
}

void
RS41Decoder::updateSondeData(SondeData *info, RS41Subframe *subframe)
{
	RS41Subframe_Status *status;
	RS41Subframe_PTU *ptu;
	RS41Subframe_GPSInfo *gpsinfo;
	RS41Subframe_GPSPos *gpspos;

	float x, y, z, dx, dy, dz;

	switch (subframe->type) {
		case RS41_SFTYPE_INFO:
			status = (RS41Subframe_Status*)subframe;
			updateCalibData(status);

			info->calibrated = _calibrated;
			info->serial = status->serial;
			info->serial[RS41_SERIAL_LEN] = 0;
			info->burstkill = _calibData.burstkill_timer == 0xFFFF ? -1 : _calibData.burstkill_timer;
			info->seq = status->frame_seq;
			break;
		case RS41_SFTYPE_PTU:
			ptu = (RS41Subframe_PTU*)subframe;
			/* TODO */
			break;
		case RS41_SFTYPE_GPSPOS:
			gpspos = (RS41Subframe_GPSPos*)subframe;
			x = gpspos->x / 100.0;
			y = gpspos->y / 100.0;
			z = gpspos->z / 100.0;
			dx = gpspos->dx / 100.0;
			dy = gpspos->dy / 100.0;
			dz = gpspos->dz / 100.0;

			ecef_to_lla(&info->lat, &info->lon, &info->alt, x, y, z);
			ecef_to_spd_hdg(&info->spd, &info->hdg, &info->climb, info->lat, info->lon, dx, dy, dz);

			break;
		case RS41_SFTYPE_GPSINFO:
			gpsinfo = (RS41Subframe_GPSInfo*)subframe;
			/* TODO */
			break;
		case RS41_SFTYPE_XDATA:
			break;
		case RS41_SFTYPE_GPSRAW:
		case RS41_SFTYPE_EMPTY:
		default:
			break;
	}
}

void
RS41Decoder::updateCalibData(RS41Subframe_Status* status)
{
	size_t frag_offset;
	int num_segments;
	size_t i;

	/* Copy the fragment and update the bitmap of the fragments left */
	frag_offset = status->frag_seq * RS41_CALIB_FRAGSIZE;
	memcpy((uint8_t*)&_calibData + frag_offset, status->frag_data, RS41_CALIB_FRAGSIZE);
	_calibDataBitmap[status->frag_seq/8] &= ~(1 << status->frag_seq%8);

	/* Check if we have all the sub-segments populated */
	for (i=0; i<sizeof(_calibDataBitmap); i++) {
		if (_calibDataBitmap[i]) return;
	}
	_calibrated = true;
}
/* }}} */
