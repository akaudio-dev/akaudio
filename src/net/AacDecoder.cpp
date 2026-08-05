// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrei Kozlov

#include "AacDecoder.hpp"

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>

#include <cstring>
#include <vector>

namespace akaudio {

struct AacDecoder::Impl {
	AudioFileStreamID stream = nullptr;
	AudioConverterRef converter = nullptr;
	AudioStreamBasicDescription srcFmt{};
	AudioStreamBasicDescription dstFmt{};
	bool ready = false;  // converter built
	bool failed = false;

	// The current batch of input packets handed to the converter callback.
	std::vector<uint8_t> inData;
	std::vector<AudioStreamPacketDescription> inDescs;
	bool consumed = false; // batch already returned by the input callback?
	std::vector<float> out; // reused decode output buffer (capacity stabilizes after first batch)

	std::function<void(const float*, int, double)>* onPCM = nullptr;

	void makeConverter() {
		// Output: native packed Float32, interleaved stereo, at the source rate.
		dstFmt = AudioStreamBasicDescription{};
		dstFmt.mSampleRate = srcFmt.mSampleRate;
		dstFmt.mFormatID = kAudioFormatLinearPCM;
		dstFmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
		dstFmt.mChannelsPerFrame = 2;
		dstFmt.mBitsPerChannel = 32;
		dstFmt.mFramesPerPacket = 1;
		dstFmt.mBytesPerFrame = dstFmt.mChannelsPerFrame * sizeof(float);
		dstFmt.mBytesPerPacket = dstFmt.mBytesPerFrame;
		if (AudioConverterNew(&srcFmt, &dstFmt, &converter) != noErr || !converter) {
			failed = true;
			return;
		}
		// Mono source → stereo out: without an explicit channel map CoreAudio
		// routes the single input channel to output channel 0 only, leaving the
		// right channel silent (a mono AAC/HLS stream would play hard-left). Map
		// both output channels to input channel 0 so it plays centered — matching
		// the MP3 path's `curL = curR = pcm[i]` mono fan-out.
		if (srcFmt.mChannelsPerFrame == 1) {
			SInt32 channelMap[2] = {0, 0};
			AudioConverterSetProperty(converter, kAudioConverterChannelMap,
				sizeof(channelMap), channelMap);
		}
		ready = true;
	}
};

// Converter pull callback: hand the whole queued batch once, then report empty.
static OSStatus inputProc(AudioConverterRef, UInt32* ioNumPackets, AudioBufferList* ioData,
		AudioStreamPacketDescription** outDescs, void* userData) {
	AacDecoder::Impl* impl = static_cast<AacDecoder::Impl*>(userData);
	if (impl->consumed || impl->inDescs.empty()) {
		*ioNumPackets = 0;
		ioData->mNumberBuffers = 1;
		ioData->mBuffers[0].mData = nullptr;
		ioData->mBuffers[0].mDataByteSize = 0;
		if (outDescs)
			*outDescs = nullptr;
		return noErr; // no more input this fill
	}
	*ioNumPackets = (UInt32) impl->inDescs.size();
	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mNumberChannels = impl->srcFmt.mChannelsPerFrame;
	ioData->mBuffers[0].mDataByteSize = (UInt32) impl->inData.size();
	ioData->mBuffers[0].mData = impl->inData.data();
	if (outDescs)
		*outDescs = impl->inDescs.data();
	impl->consumed = true;
	return noErr;
}

// AudioFileStream property callback: capture the source format, build converter.
static void propProc(void* userData, AudioFileStreamID stream, AudioFileStreamPropertyID propID, UInt32*) {
	AacDecoder::Impl* impl = static_cast<AacDecoder::Impl*>(userData);
	if (propID == kAudioFileStreamProperty_DataFormat) {
		AudioStreamBasicDescription prev = impl->srcFmt;
		UInt32 size = sizeof(impl->srcFmt);
		AudioFileStreamGetProperty(stream, propID, &size, &impl->srcFmt);
		// The format can be (re)reported after the converter was built — a
		// mid-stream format change, or HE-AAC/SBR recognized only once more
		// data arrived. Converting with the stale format means the wrong
		// rate/pitch, so rebuild; onPCM picks up the new dstFmt rate on the
		// next batch.
		// NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c) — ASBD is padding-free (one Float64 + eight UInt32s)
		if (impl->converter && std::memcmp(&prev, &impl->srcFmt, sizeof(prev)) != 0) {
			AudioConverterDispose(impl->converter);
			impl->converter = nullptr;
			impl->ready = false;
			impl->makeConverter();
		}
	}
	else if (propID == kAudioFileStreamProperty_ReadyToProducePackets) {
		if (impl->srcFmt.mSampleRate > 0 && !impl->converter && !impl->failed)
			impl->makeConverter();
	}
}

// AudioFileStream packets callback: convert this batch to PCM and emit it.
static void packetsProc(void* userData, UInt32 numBytes, UInt32 numPackets,
		const void* inData, AudioStreamPacketDescription* descs) {
	AacDecoder::Impl* impl = static_cast<AacDecoder::Impl*>(userData);
	if (impl->failed || !descs || numPackets == 0)
		return;
	if (!impl->ready) {
		if (impl->srcFmt.mSampleRate > 0 && !impl->converter)
			impl->makeConverter();
		if (!impl->ready)
			return; // not enough info to decode yet
	}

	impl->inData.assign(static_cast<const uint8_t*>(inData),
		static_cast<const uint8_t*>(inData) + numBytes);
	impl->inDescs.assign(descs, descs + numPackets);
	impl->consumed = false;

	UInt32 framesPerPacket = impl->srcFmt.mFramesPerPacket ? impl->srcFmt.mFramesPerPacket : 1024;
	UInt32 wantFrames = numPackets * framesPerPacket;

	std::vector<float>& out = impl->out;
	out.resize(static_cast<size_t>(wantFrames) * 2);
	AudioBufferList abl;
	abl.mNumberBuffers = 1;
	abl.mBuffers[0].mNumberChannels = 2;
	abl.mBuffers[0].mDataByteSize = (UInt32)(out.size() * sizeof(float));
	abl.mBuffers[0].mData = out.data();

	UInt32 outPackets = wantFrames; // for PCM, 1 packet == 1 frame
	OSStatus st = AudioConverterFillComplexBuffer(impl->converter, inputProc, impl, &outPackets, &abl, nullptr);
	if (st != noErr && outPackets == 0)
		return; // produced nothing (priming / transient)

	if (outPackets > 0 && impl->onPCM && *impl->onPCM)
		(*impl->onPCM)(out.data(), (int) outPackets, impl->dstFmt.mSampleRate);
}

AacDecoder::~AacDecoder() {
	close();
}

bool AacDecoder::available() {
	return true;
}

bool AacDecoder::init() {
	if (impl)
		close(); // re-init must not leak the previous Impl
	impl = new Impl;
	impl->onPCM = &onPCM;
	OSStatus st = AudioFileStreamOpen(impl, propProc, packetsProc, kAudioFileAAC_ADTSType, &impl->stream);
	if (st != noErr) {
		close();
		return false;
	}
	return true;
}

bool AacDecoder::feed(const uint8_t* data, size_t n) {
	if (!impl || !impl->stream || impl->failed)
		return false;
	OSStatus st = AudioFileStreamParseBytes(impl->stream, (UInt32) n, data, 0);
	return st == noErr && !impl->failed;
}

void AacDecoder::close() {
	if (!impl)
		return;
	if (impl->converter)
		AudioConverterDispose(impl->converter);
	if (impl->stream)
		AudioFileStreamClose(impl->stream);
	delete impl;
	impl = nullptr;
}

} // namespace akaudio

#else // !__APPLE__ — portable FAAD2 path (Windows / Linux)

#include "../dep/faad2/include/neaacdec.h"

#include <cstring>
#include <vector>

namespace akaudio {

struct AacDecoder::Impl {
	NeAACDecHandle dec = nullptr;
	bool inited = false;      // NeAACDecInit has locked onto the stream config
	std::vector<uint8_t> buf; // raw ADTS bytes accumulated across feed() calls
	std::vector<float> out;   // reused stereo scratch for mono/multichannel fan-out
	std::function<void(const float*, int, double)>* onPCM = nullptr;
};

namespace {

// ADTS syncword (0xFFF) + MPEG layer '00'. True if p starts an ADTS header.
inline bool adtsSync(const uint8_t* p) {
	return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
}

// 13-bit aac_frame_length (header included). Caller ensures 6 bytes available.
inline size_t adtsFrameLen(const uint8_t* p) {
	return ((size_t)(p[3] & 0x03) << 11) | ((size_t) p[4] << 3) | ((size_t)(p[5] >> 5));
}

} // namespace

AacDecoder::~AacDecoder() {
	close();
}

bool AacDecoder::available() {
	return true;
}

bool AacDecoder::init() {
	if (impl)
		close(); // re-init must not leak the previous Impl
	impl = new Impl;
	impl->onPCM = &onPCM;
	impl->dec = NeAACDecOpen();
	if (!impl->dec) {
		close();
		return false;
	}
	// Float output, interleaved — matches onPCM and the MP3 path's PCM shape.
	// downMatrix folds >2ch (rare in radio) down to stereo so the fan-out below
	// only has to special-case mono.
	NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(impl->dec);
	cfg->outputFormat = FAAD_FMT_FLOAT;
	cfg->downMatrix = 1;
	NeAACDecSetConfiguration(impl->dec, cfg);
	return true;
}

bool AacDecoder::feed(const uint8_t* data, size_t n) {
	if (!impl || !impl->dec)
		return false;

	std::vector<uint8_t>& b = impl->buf;
	b.insert(b.end(), data, data + n);

	// First ADTS header at or after `from`. A syncword needs 2 bytes, so when no
	// sync exists this parks on a trailing 0xFF (it may be the first byte of a
	// header split across feeds) so the final erase below preserves it; otherwise
	// b.size().
	auto findSync = [&](size_t from) -> size_t {
		for (size_t i = from; i + 1 < b.size(); i++)
			if (adtsSync(&b[i]))
				return i;
		if (from < b.size() && b.back() == 0xFF)
			return b.size() - 1;
		return b.size();
	};

	size_t pos = 0;

	// Lock onto the stream (samplerate/channels/profile) from the first header.
	// NeAACDecInit only reads the ~7-byte ADTS header and returns 0 for ADTS, so we
	// stay parked on the syncword and let the decode loop consume the whole frame.
	// Try EVERY sync candidate now buffered, not one per feed(): a mislabeled
	// non-AAC stream dense with false 0xFFFx patterns must burn through them all
	// here, or `b` grows without bound (each feed appends bytes but would only
	// retire one candidate).
	while (!impl->inited) {
		pos = findSync(pos);
		if (pos + 7 > b.size())
			break;                         // not enough for a full ADTS header yet
		unsigned long sr = 0;
		unsigned char ch = 0;
		long used = NeAACDecInit(impl->dec, &b[pos], (unsigned long)(b.size() - pos), &sr, &ch);
		if (used < 0) {                    // false sync / undecodable header — skip it
			pos++;
			continue;
		}
		pos += (size_t) used;
		impl->inited = true;
	}

	// Decode every complete ADTS frame now buffered.
	while (impl->inited) {
		pos = findSync(pos);
		if (pos + 7 > b.size())
			break;                         // need the next header
		size_t flen = adtsFrameLen(&b[pos]);
		if (flen < 7) {                    // malformed length — resync one byte on
			pos++;
			continue;
		}
		if (pos + flen > b.size())
			break;                         // wait for the whole frame to arrive

		NeAACDecFrameInfo fi;
		std::memset(&fi, 0, sizeof(fi));
		const void* pcm = NeAACDecDecode(impl->dec, &fi, &b[pos], (unsigned long)(b.size() - pos));

		if (fi.error != 0) {               // corrupt frame — skip it, keep decoding
			pos += flen;
			continue;
		}
		pos += fi.bytesconsumed ? fi.bytesconsumed : flen;

		if (!pcm || fi.samples == 0 || !impl->onPCM || !*impl->onPCM)
			continue;

		const float* src = static_cast<const float*>(pcm);
		int ch = fi.channels ? fi.channels : 1;
		int frames = (int)(fi.samples / (unsigned) ch);
		double rate = (double) fi.samplerate;

		if (ch == 2) {
			(*impl->onPCM)(src, frames, rate);   // already interleaved L,R
		}
		else {
			// Mono → centered stereo (matches the MP3 path's mono fan-out); any
			// >2ch that slipped past downMatrix → take the front pair.
			std::vector<float>& o = impl->out;
			o.resize((size_t) frames * 2);
			if (ch == 1) {
				for (int i = 0; i < frames; i++)
					o[2 * i] = o[2 * i + 1] = src[i];
			}
			else {
				for (int i = 0; i < frames; i++) {
					o[2 * i] = src[(size_t) i * ch];
					o[2 * i + 1] = src[(size_t) i * ch + 1];
				}
			}
			(*impl->onPCM)(o.data(), frames, rate);
		}
	}

	if (pos > 0)
		b.erase(b.begin(), b.begin() + pos);
	return true;
}

void AacDecoder::close() {
	if (!impl)
		return;
	if (impl->dec)
		NeAACDecClose(impl->dec);
	delete impl;
	impl = nullptr;
}

} // namespace akaudio

#endif
