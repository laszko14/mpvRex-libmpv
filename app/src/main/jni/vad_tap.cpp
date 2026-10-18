// MPV Alpha Auto Audio: bounded, read-only PCM bridge.
// LGPL-2.1-or-later code additions for the mpv-compatible native module.
// Do not do Java calls, disk I/O, network or blocking waits from mpv audio.
#include <jni.h>
#include <stdint.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "jni_utils.h"
#include "globals.h"

extern "C" {
typedef void (*rex_pcm_sink_t)(const uint8_t *const *planes, int format,
                              int channels, int rate, int samples, double pts);
// Exported by patched player/audio.c in the SAME native libmpv build.
void mpv_rex_set_pcm_sink(rex_pcm_sink_t callback);
}

namespace {
constexpr int kPacketSamples = 1024;
constexpr int kPacketCount = 64;
// mpv-player/mpv audio/format.h, pinned by buildscripts/include/depinfo.sh.
// Packed S16=2, packed float=5, planar S16=8, planar float=11.
constexpr int kS16 = 2, kFloat = 5, kS16P = 8, kFloatP = 11;

struct Packet {
    int rate = 0;
    int count = 0;
    double pts = 0;
    int16_t values[kPacketSamples] = {};
};

Packet queue[kPacketCount];
int rd = 0, wr = 0, queued = 0;
int lastRate = 0;
std::mutex guard;
std::atomic<bool> active(false);
std::atomic<int> drops(0);

bool is_supported(int fmt) {
    return fmt == kS16 || fmt == kFloat || fmt == kS16P || fmt == kFloatP;
}

float one_sample(const uint8_t *const *planes, int fmt, int channels, int ch, int idx) {
    const bool planar = fmt == kS16P || fmt == kFloatP;
    const uint8_t *plane = planes[planar ? ch : 0];
    if (!plane)
        return 0.0f;
    const int offset = planar ? idx : idx * channels + ch;
    if (fmt == kS16 || fmt == kS16P) {
        int16_t sample;
        memcpy(&sample, plane + offset * sizeof(sample), sizeof(sample));
        return sample / 32768.0f;
    }
    float sample;
    memcpy(&sample, plane + offset * sizeof(sample), sizeof(sample));
    return std::isfinite(sample) ? sample : 0.0f;
}

// Runs inside the decoder/filter thread. Never locks while another thread
// owns the queue. Dropped packets INVALIDATE that scan; never silently fill.
void rex_on_decoded_pcm(const uint8_t *const *planes, int fmt, int channels,
                        int rate, int samples, double pts) {
    if (!active.load(std::memory_order_relaxed) || !planes ||
        !is_supported(fmt) || channels < 1 || channels > 8 ||
        rate < 8000 || rate > 192000 || samples <= 0) {
        if (active.load(std::memory_order_relaxed))
            drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::unique_lock<std::mutex> lock(guard, std::try_to_lock);
    if (!lock.owns_lock()) {
        drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A source format change invalidates pending data and triggers a
    // fail-closed report in Kotlin via droppedChunks.
    if (lastRate && rate != lastRate) {
        rd = wr = queued = 0;
        drops.fetch_add(1, std::memory_order_relaxed);
    }
    lastRate = rate;

    for (int at = 0; at < samples; at += kPacketSamples) {
        if (queued == kPacketCount) {
            drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Packet &p = queue[wr];
        p.count = std::min(kPacketSamples, samples - at);
        p.rate = rate;
        p.pts = pts + static_cast<double>(at) / rate;
        for (int n = 0; n < p.count; ++n) {
            const int idx = at + n;
            // Mid/mono speech: use stereo mean; surround center channel when
            // available (common layout, but not a channel-layout guarantee).
            const float v = channels >= 3
                ? one_sample(planes, fmt, channels, 2, idx)
                : channels == 2
                    ? (one_sample(planes, fmt, channels, 0, idx) +
                       one_sample(planes, fmt, channels, 1, idx)) * 0.5f
                    : one_sample(planes, fmt, channels, 0, idx);
            const float safe = std::isfinite(v) ? std::max(-1.0f, std::min(1.0f, v)) : 0.0f;
            p.values[n] = static_cast<int16_t>(std::lrint(safe * 32767.0f));
        }
        wr = (wr + 1) % kPacketCount;
        ++queued;
    }
}
}

void rex_vad_tap_stop() {
    active.store(false, std::memory_order_release);
    mpv_rex_set_pcm_sink(nullptr);
    std::lock_guard<std::mutex> lock(guard);
    rd = wr = queued = lastRate = 0;
    drops.store(0, std::memory_order_relaxed);
}

extern "C" {
jni_func(void, vadTapStart) {
    if (!g_mpv) return;
    rex_vad_tap_stop();
    // Enabled only on explicit user request, never silently during playback.
    active.store(true, std::memory_order_release);
    mpv_rex_set_pcm_sink(rex_on_decoded_pcm);
}

jni_func(void, vadTapStop) {
    rex_vad_tap_stop();
}

jni_func(jobject, vadTapPoll) {
    Packet packet;
    int missed = 0;
    {
        std::lock_guard<std::mutex> lock(guard);
        if (!queued || !active.load(std::memory_order_acquire))
            return nullptr;
        packet = queue[rd];
        rd = (rd + 1) % kPacketCount;
        --queued;
        missed = drops.exchange(0, std::memory_order_relaxed);
    }
    jclass cls = env->FindClass("is/xyz/mpv/MPVLib$VadPcmChunk");
    if (!cls) return nullptr;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "([SIDI)V");
    if (!ctor) { env->DeleteLocalRef(cls); return nullptr; }
    jshortArray samples = env->NewShortArray(packet.count);
    if (!samples) { env->DeleteLocalRef(cls); return nullptr; }
    env->SetShortArrayRegion(samples, 0, packet.count,
                             reinterpret_cast<const jshort *>(packet.values));
    jobject out = env->NewObject(cls, ctor, samples, (jint)packet.rate,
                                  (jdouble)packet.pts, (jint)missed);
    env->DeleteLocalRef(samples);
    env->DeleteLocalRef(cls);
    return out;
}
}
