#include "NjEncoder.hpp"

#include <vorbis/vorbisenc.h>

namespace akaudio {
namespace nj {

std::vector<uint8_t> encodeOggInterval(const float* interleaved, int frames, int channels,
                                       int sampleRate, float quality, int serial) {
	std::vector<uint8_t> out;
	if (channels < 1 || sampleRate <= 0)
		return out;

	vorbis_info vi;
	vorbis_info_init(&vi);
	if (vorbis_encode_init_vbr(&vi, channels, sampleRate, quality) != 0) {
		vorbis_info_clear(&vi);
		return out;
	}

	vorbis_comment vc;
	vorbis_comment_init(&vc);
	vorbis_comment_add_tag(&vc, "ENCODER", "akaudio");

	vorbis_dsp_state vd;
	vorbis_analysis_init(&vd, &vi);
	vorbis_block vb;
	vorbis_block_init(&vd, &vb);

	ogg_stream_state os;
	ogg_stream_init(&os, serial);

	ogg_page og;
	ogg_packet op;

	auto appendPage = [&]() {
		out.insert(out.end(), og.header, og.header + og.header_len);
		out.insert(out.end(), og.body, og.body + og.body_len);
	};

	// Header packets (identification, comment, codebooks) -> their own page(s).
	{
		ogg_packet h, hc, hb;
		vorbis_analysis_headerout(&vd, &vc, &h, &hc, &hb);
		ogg_stream_packetin(&os, &h);
		ogg_stream_packetin(&os, &hc);
		ogg_stream_packetin(&os, &hb);
		while (ogg_stream_flush(&os, &og))
			appendPage();
	}

	// Feed audio in small chunks (libvorbis prefers <= ~1024 samples per call).
	const int CHUNK = 1024;
	int pos = 0;
	bool done = false;
	while (!done) {
		int n = frames - pos;
		if (n > CHUNK) n = CHUNK;

		if (n > 0) {
			float** buffer = vorbis_analysis_buffer(&vd, n);
			for (int ch = 0; ch < channels; ch++)
				for (int i = 0; i < n; i++)
					buffer[ch][i] = interleaved[(size_t)(pos + i) * channels + ch];
			vorbis_analysis_wrote(&vd, n);
			pos += n;
		} else {
			// Signal end of stream.
			vorbis_analysis_wrote(&vd, 0);
			done = true;
		}

		while (vorbis_analysis_blockout(&vd, &vb) == 1) {
			vorbis_analysis(&vb, NULL);
			vorbis_bitrate_addblock(&vb);
			while (vorbis_bitrate_flushpacket(&vd, &op)) {
				ogg_stream_packetin(&os, &op);
				while (ogg_stream_pageout(&os, &og))
					appendPage();
			}
		}
	}

	// Flush any remaining (EOS) pages.
	while (ogg_stream_flush(&os, &og))
		appendPage();

	ogg_stream_clear(&os);
	vorbis_block_clear(&vb);
	vorbis_dsp_clear(&vd);
	vorbis_comment_clear(&vc);
	vorbis_info_clear(&vi);
	return out;
}

} // namespace nj
} // namespace akaudio
