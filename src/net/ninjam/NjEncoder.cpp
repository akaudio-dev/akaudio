// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "NjEncoder.hpp"

#include <vorbis/vorbisenc.h>

namespace akaudio {
namespace nj {

bool OggIntervalEncoder::begin(int channels, int sampleRate, float quality, int serial) {
	reset();
	if (channels < 1 || sampleRate <= 0)
		return false;

	vorbis_info_init(&vi_);
	if (vorbis_encode_init_vbr(&vi_, channels, sampleRate, quality) != 0) {
		vorbis_info_clear(&vi_);
		return false;
	}
	channels_ = channels;

	vorbis_comment_init(&vc_);
	vorbis_comment_add_tag(&vc_, "ENCODER", "akaudio");

	vorbis_analysis_init(&vd_, &vi_);
	vorbis_block_init(&vd_, &vb_);
	ogg_stream_init(&os_, serial);
	active_ = true;

	// Header packets (identification, comment, codebooks) -> their own page(s),
	// flushed immediately so the stream is identifiable from the first chunk.
	ogg_packet h, hc, hb;
	vorbis_analysis_headerout(&vd_, &vc_, &h, &hc, &hb);
	ogg_stream_packetin(&os_, &h);
	ogg_stream_packetin(&os_, &hc);
	ogg_stream_packetin(&os_, &hb);
	pageOut(/*flush=*/true);
	return true;
}

void OggIntervalEncoder::pageOut(bool flush) {
	ogg_page og;
	for (;;) {
		int got = flush ? ogg_stream_flush(&os_, &og) : ogg_stream_pageout(&os_, &og);
		if (!got)
			break;
		pending_.insert(pending_.end(), og.header, og.header + og.header_len);
		pending_.insert(pending_.end(), og.body, og.body + og.body_len);
	}
}

void OggIntervalEncoder::feed(const float* interleaved, int frames) {
	if (!active_ || frames <= 0)
		return;
	// libvorbis prefers modest chunks (<= ~1024 samples per call).
	const int CHUNK = 1024;
	int pos = 0;
	while (pos < frames) {
		int n = frames - pos;
		if (n > CHUNK) n = CHUNK;
		float** buffer = vorbis_analysis_buffer(&vd_, n);
		for (int ch = 0; ch < channels_; ch++)
			for (int i = 0; i < n; i++)
				buffer[ch][i] = interleaved[(size_t)(pos + i) * channels_ + ch];
		vorbis_analysis_wrote(&vd_, n);
		pos += n;

		ogg_packet op;
		while (vorbis_analysis_blockout(&vd_, &vb_) == 1) {
			vorbis_analysis(&vb_, NULL);
			vorbis_bitrate_addblock(&vb_);
			while (vorbis_bitrate_flushpacket(&vd_, &op)) {
				ogg_stream_packetin(&os_, &op);
				pageOut(/*flush=*/false);
			}
		}
	}
}

void OggIntervalEncoder::finish() {
	if (!active_)
		return;
	// Signal end of stream, drain the remaining packets, then flush the EOS pages.
	vorbis_analysis_wrote(&vd_, 0);
	ogg_packet op;
	while (vorbis_analysis_blockout(&vd_, &vb_) == 1) {
		vorbis_analysis(&vb_, NULL);
		vorbis_bitrate_addblock(&vb_);
		while (vorbis_bitrate_flushpacket(&vd_, &op)) {
			ogg_stream_packetin(&os_, &op);
			pageOut(/*flush=*/false);
		}
	}
	pageOut(/*flush=*/true);

	ogg_stream_clear(&os_);
	vorbis_block_clear(&vb_);
	vorbis_dsp_clear(&vd_);
	vorbis_comment_clear(&vc_);
	vorbis_info_clear(&vi_);
	active_ = false;
	channels_ = 0;
}

std::vector<uint8_t> OggIntervalEncoder::take() {
	std::vector<uint8_t> out;
	out.swap(pending_);
	return out;
}

void OggIntervalEncoder::reset() {
	if (active_) {
		ogg_stream_clear(&os_);
		vorbis_block_clear(&vb_);
		vorbis_dsp_clear(&vd_);
		vorbis_comment_clear(&vc_);
		vorbis_info_clear(&vi_);
		active_ = false;
		channels_ = 0;
	}
	pending_.clear();
}

std::vector<uint8_t> encodeOggInterval(const float* interleaved, int frames, int channels,
                                       int sampleRate, float quality, int serial) {
	OggIntervalEncoder enc;
	if (!enc.begin(channels, sampleRate, quality, serial))
		return {};
	enc.feed(interleaved, frames);
	enc.finish();
	return enc.take();
}

} // namespace nj
} // namespace akaudio
