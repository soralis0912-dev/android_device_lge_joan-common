/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * An android 9 shaped AudioTrack.
 *
 * MobitPlayerService::AudioOutput drives one AudioTrack, and it was compiled
 * when the class still read
 *
 *     class AudioTrack : public RefBase
 *
 * Since then RefBase became a *virtual* base - the MemoryHeapBase break of
 * memoryheap_shim.cpp, running the other way - and the object grew from 840
 * (0x348) to 1008 (0x3f0) bytes. Three things follow, and the blob has all
 * three burned in:
 *
 *   allocation   `mov.w r0,#0x348; bl operator new` at 0x4a2e0 and 0x4a362,
 *                168 bytes short of the current sizeof.
 *
 *   upcast       sp<AudioTrack>::operator= is a weak definition inside the blob
 *                (0x47878) and hands the raw pointer to RefBase::incStrong with
 *                no adjustment at all, because in android 9 there was none to
 *                make. Today RefBase sits at +1000, so that reads mAudioTrack
 *                as if it were mRefs. Unlike MemoryHeapBase there is no
 *                `ldr rN,[vptr,#-16]` to repoint - the code was never emitted.
 *
 *   fields       four of AudioTrack's inline accessors are expanded into the
 *                blob at the android 9 offsets:
 *                  +0x48  frameCount()   (bufferSize, frameCount, open)
 *                  +0x64  channelCount() (channelCount, open)
 *                  +0x1bc frameSize()    (frameSize)
 *                  +0x1c0 initCheck()    (open, decides whether the track took)
 *                All four moved: +0x4c, +0x78, +0x1d8, +0x1dc.
 *
 * Constructing a current AudioTrack in the blob's storage - which is what this
 * file used to do - loses on every one of those. So this is the memoryheap_shim
 * treatment instead: a class laid out the way the blob remembers, holding a
 * real AudioTrack behind a pointer, with extract-files.py renaming the
 * fourteen symbols the blob looks up - the constructor and thirteen methods,
 * all of which resolve to libaudioclient today:
 *
 *     android10AudioTrack  ->  android10LegacyTrak
 *
 * Same length, so it is a byte-for-byte edit of .dynstr entries and nothing
 * moves. The pattern carries the `android` prefix on purpose: `10AudioTrack`
 * alone would also hit the blob's own two weak sp<AudioTrack>::operator=
 * *definitions*, and those are indexed by .gnu.hash - renaming a defined symbol
 * without rebuilding the hash makes it unfindable, and since the blob's
 * reference to it is weak the linker would quietly bind it to address 0.
 * Leaving them alone is also what we want: nothing else in the process defines
 * either name, so they keep resolving to the blob's own copies, and those hand
 * the raw pointer to incStrong with no adjustment - which is correct here,
 * because RefBase is back at offset 0.
 *
 * The blob never dispatches virtually on the track (its vtable is not in the
 * blob and there are no vcalls through it), so only the four mirrored fields
 * and the total size are load bearing. Both are checked below.
 */

#define LOG_TAG "mtv_audiotrack_shim"

#include <stddef.h>
#include <stdint.h>

#include <binder/Binder.h>
#include <media/AudioTrack.h>
#include <utils/Log.h>
#include <utils/RefBase.h>

namespace android {

namespace {

// The function-pointer callback the blob hands its constructor, in the shape
// AudioTrack takes callbacks today. AudioTrack carried exactly this as
// LegacyCallbackWrapper, behind a set() overload for vendor code, until
// android 16 removed both; this is that class.
class LegacyCallbackWrapper : public AudioTrack::IAudioTrackCallback {
  public:
    typedef void (*callback_t)(int event, void* user, void* info);

    LegacyCallbackWrapper(callback_t callback, void* user) : mCallback(callback), mData(user) {}

    size_t onMoreData(const AudioTrack::Buffer& buffer) override {
        AudioTrack::Buffer copy = buffer;
        mCallback(AudioTrack::EVENT_MORE_DATA, mData, static_cast<void*>(&copy));
        return copy.size();
    }
    void onUnderrun() override { mCallback(AudioTrack::EVENT_UNDERRUN, mData, nullptr); }
    void onLoopEnd(int32_t loopsRemaining) override {
        mCallback(AudioTrack::EVENT_LOOP_END, mData, &loopsRemaining);
    }
    void onMarker(uint32_t markerPosition) override {
        mCallback(AudioTrack::EVENT_MARKER, mData, &markerPosition);
    }
    void onNewPos(uint32_t newPos) override {
        mCallback(AudioTrack::EVENT_NEW_POS, mData, &newPos);
    }
    void onBufferEnd() override { mCallback(AudioTrack::EVENT_BUFFER_END, mData, nullptr); }
    void onNewIAudioTrack() override {
        mCallback(AudioTrack::EVENT_NEW_IAUDIOTRACK, mData, nullptr);
    }
    void onStreamEnd() override { mCallback(AudioTrack::EVENT_STREAM_END, mData, nullptr); }
    size_t onCanWriteMoreData(const AudioTrack::Buffer& buffer) override {
        AudioTrack::Buffer copy = buffer;
        mCallback(AudioTrack::EVENT_CAN_WRITE_MORE_DATA, mData, static_cast<void*>(&copy));
        return copy.size();
    }

  private:
    const callback_t mCallback;
    void* const mData;
};

}  // anonymous namespace

class LegacyTrak : public RefBase {
public:
    // Not a typedef to AudioTrack::transfer_type: the constructor the blob
    // looks up spells this parameter NS0_13transfer_typeE, i.e. a type nested
    // in this class, and a typedef would mangle as AudioTrack's. Values 0-4 are
    // the same on both sides; current AOSP only appends to the list.
    enum transfer_type {
        TRANSFER_DEFAULT,
        TRANSFER_CALLBACK,
        TRANSFER_OBTAIN,
        TRANSFER_SYNC,
        TRANSFER_SHARED,
    };

    typedef void (*callback_t)(int event, void* user, void* info);

    // The android 9 constructor, parameter for parameter. channelMask/frameCount
    // are uint32_t and uid/pid are uint32_t/int32_t because that is what the
    // blob's mangled name says (j,j,...,j,i) - typedefs that are enums today
    // would mangle differently and the name would stop matching.
    LegacyTrak(audio_stream_type_t streamType, uint32_t sampleRate, audio_format_t format,
               uint32_t channelMask, uint32_t frameCount, audio_output_flags_t flags,
               callback_t cbf, void* user, int32_t notificationFrames, audio_session_t sessionId,
               transfer_type transferType, const audio_offload_info_t* offloadInfo, uint32_t uid,
               int32_t pid, const audio_attributes_t* pAttributes, bool doNotReconnect,
               float maxRequiredSpeed, int32_t selectedDeviceId);

    // The thirteen methods the blob calls by name. All defined out of line: they
    // are never called from inside this library, so in-class definitions would
    // be inline, never emitted, and the blob would fail to link at dlopen.
    status_t start();
    void stop();
    void flush();
    void pause();
    ssize_t write(const void* buffer, size_t size, bool blocking);
    uint32_t latency();
    uint32_t getSampleRate() const;
    status_t setSampleRate(uint32_t sampleRate);
    status_t setVolume(float left, float right);
    status_t getPosition(uint32_t* position);
    status_t getTimestamp(AudioTimestamp& timestamp);
    status_t getTimestamp(ExtendedTimestamp* timestamp);
    status_t getBufferDurationInUs(int64_t* duration);

protected:
    // Out of line, and the key function that gets the vtable emitted.
    ~LegacyTrak() override;

private:
    // Refresh the four fields the blob reads directly. Cheap - they are plain
    // loads on the other side - so every entry point does it rather than
    // reasoning about which of them AudioTrack can change (frameCount does move
    // when the track is restored after an AudioFlinger death).
    void mirror();

    // The track itself.
    sp<AudioTrack> mReal;  // 0x08
    uint8_t mPad0[0x48 - 0x0c];
    uint32_t mFrameCount;   // 0x48   AudioTrack::frameCount()
    uint8_t mPad1[0x64 - 0x4c];
    uint32_t mChannelCount; // 0x64   AudioTrack::channelCount()
    uint8_t mPad2[0x1bc - 0x68];
    uint32_t mFrameSize;    // 0x1bc  AudioTrack::frameSize()
    int32_t mStatus;        // 0x1c0  AudioTrack::initCheck()

    // AudioTrack only keeps a weak reference to its callback, so the wrapper
    // around the blob's function pointer is held here, for as long as the
    // track is.
    sp<AudioTrack::IAudioTrackCallback> mCallback;
};

// `mov.w r0, #0x348` ahead of both constructor calls in AudioOutput::open().
static_assert(sizeof(LegacyTrak) <= 0x348, "the blob allocates 0x348 bytes for this");

LegacyTrak::LegacyTrak(audio_stream_type_t streamType, uint32_t sampleRate, audio_format_t format,
                       uint32_t channelMask, uint32_t frameCount, audio_output_flags_t flags,
                       callback_t cbf, void* user, int32_t notificationFrames,
                       audio_session_t sessionId, transfer_type transferType,
                       const audio_offload_info_t* offloadInfo, uint32_t uid, int32_t pid,
                       const audio_attributes_t* pAttributes, bool doNotReconnect,
                       float maxRequiredSpeed, int32_t selectedDeviceId)
    : mFrameCount(0), mChannelCount(0), mFrameSize(0), mStatus(NO_INIT) {
    // What android 9's constructor did: the no-argument constructor leaves the
    // track unconfigured and set() fills it in. The uid/pid pair and the
    // function-pointer callback go in the way the removed legacy set() overload
    // used to pass them on.
    AttributionSourceState attributionSource;
    attributionSource.uid = static_cast<int32_t>(uid);
    attributionSource.pid = pid;
    attributionSource.token = sp<BBinder>::make();
    if (cbf != nullptr) {
        mCallback = sp<LegacyCallbackWrapper>::make(cbf, user);
    }

    mReal = sp<AudioTrack>::make();
    mReal->set(streamType, sampleRate, format, static_cast<audio_channel_mask_t>(channelMask),
               frameCount, flags, mCallback, notificationFrames, nullptr /* sharedBuffer */,
               false /* threadCanCallJava */, sessionId,
               static_cast<AudioTrack::transfer_type>(transferType), offloadInfo,
               attributionSource, pAttributes, doNotReconnect, maxRequiredSpeed,
               static_cast<audio_port_handle_t>(selectedDeviceId));

    mirror();
    ALOGI("track %p: status=%d rate=%u fmt=%#x mask=%#x frames=%u size=%u", mReal.get(), mStatus,
          sampleRate, format, channelMask, mFrameCount, mFrameSize);
}

LegacyTrak::~LegacyTrak() {}

void LegacyTrak::mirror() {
    // Here rather than at namespace scope because these are private: the whole
    // point of the padding is that they land where the blob reads them. The
    // class has virtual functions so it is not standard layout and offsetof is
    // only conditionally supported - clang computes it correctly.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(offsetof(LegacyTrak, mFrameCount) == 0x48, "blob reads frameCount() at +0x48");
    static_assert(offsetof(LegacyTrak, mChannelCount) == 0x64,
                  "blob reads channelCount() at +0x64");
    static_assert(offsetof(LegacyTrak, mFrameSize) == 0x1bc, "blob reads frameSize() at +0x1bc");
    static_assert(offsetof(LegacyTrak, mStatus) == 0x1c0, "blob reads initCheck() at +0x1c0");
#pragma clang diagnostic pop

    if (mReal == nullptr) {
        return;
    }
    mFrameCount = mReal->frameCount();
    mChannelCount = mReal->channelCount();
    mFrameSize = mReal->frameSize();
    mStatus = mReal->initCheck();
}

status_t LegacyTrak::start() {
    const status_t err = mReal->start();
    mirror();
    return err;
}

void LegacyTrak::stop() {
    mReal->stop();
    mirror();
}

void LegacyTrak::flush() {
    mReal->flush();
    mirror();
}

void LegacyTrak::pause() {
    mReal->pause();
    mirror();
}

ssize_t LegacyTrak::write(const void* buffer, size_t size, bool blocking) {
    const ssize_t written = mReal->write(buffer, size, blocking);
    mirror();
    return written;
}

uint32_t LegacyTrak::latency() {
    return mReal->latency();
}

uint32_t LegacyTrak::getSampleRate() const {
    return mReal->getSampleRate();
}

status_t LegacyTrak::setSampleRate(uint32_t sampleRate) {
    const status_t err = mReal->setSampleRate(sampleRate);
    mirror();
    return err;
}

status_t LegacyTrak::setVolume(float left, float right) {
    return mReal->setVolume(left, right);
}

status_t LegacyTrak::getPosition(uint32_t* position) {
    return mReal->getPosition(position);
}

status_t LegacyTrak::getTimestamp(AudioTimestamp& timestamp) {
    return mReal->getTimestamp(timestamp);
}

status_t LegacyTrak::getTimestamp(ExtendedTimestamp* timestamp) {
    return mReal->getTimestamp(timestamp);
}

status_t LegacyTrak::getBufferDurationInUs(int64_t* duration) {
    return mReal->getBufferDurationInUs(duration);
}

}  // namespace android
