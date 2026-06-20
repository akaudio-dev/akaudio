#include "AacDecoder.hpp"

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>

#include <cstring>
#include <vector>

namespace akozlov {

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
		if (AudioConverterNew(&srcFmt, &dstFmt, &converter) != noErr) {
			failed = true;
			return;
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
		UInt32 size = sizeof(impl->srcFmt);
		AudioFileStreamGetProperty(stream, propID, &size, &impl->srcFmt);
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

	std::vector<float> out(static_cast<size_t>(wantFrames) * 2);
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

} // namespace akozlov

#else // !__APPLE__

namespace akozlov {
AacDecoder::~AacDecoder() {}
bool AacDecoder::available() { return false; }
bool AacDecoder::init() { return false; }
bool AacDecoder::feed(const uint8_t*, size_t) { return false; }
void AacDecoder::close() {}
} // namespace akozlov

#endif
