#include <stdlib.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/timestamp.h>
    #include <libavutil/opt.h>
    #include <libavutil/error.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/channel_layout.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
};

#include "jni_utils.h"
#include "log.h"

static std::string av_err_to_string(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}

// ============================================================================
// AUDIO FORMAT CONVERSION UTILITIES
// Converts any FFmpeg decoded audio frame (FLTP, S16P, S32P, etc., mono/stereo/surround)
// into 16-bit signed interleaved PCM expected by Android MediaCodec AAC encoder.
// ============================================================================

static inline float get_audio_sample_float(const AVFrame *frame, int ch, int sample_idx, int in_channels) {
    switch (frame->format) {
        case AV_SAMPLE_FMT_FLTP: {
            const float *data = (const float*)frame->data[ch];
            return data ? data[sample_idx] : 0.0f;
        }
        case AV_SAMPLE_FMT_FLT: {
            const float *data = (const float*)frame->data[0];
            return data ? data[sample_idx * in_channels + ch] : 0.0f;
        }
        case AV_SAMPLE_FMT_S16P: {
            const int16_t *data = (const int16_t*)frame->data[ch];
            return data ? (data[sample_idx] / 32768.0f) : 0.0f;
        }
        case AV_SAMPLE_FMT_S16: {
            const int16_t *data = (const int16_t*)frame->data[0];
            return data ? (data[sample_idx * in_channels + ch] / 32768.0f) : 0.0f;
        }
        case AV_SAMPLE_FMT_S32P: {
            const int32_t *data = (const int32_t*)frame->data[ch];
            return data ? (data[sample_idx] / 2147483648.0f) : 0.0f;
        }
        case AV_SAMPLE_FMT_S32: {
            const int32_t *data = (const int32_t*)frame->data[0];
            return data ? (data[sample_idx * in_channels + ch] / 2147483648.0f) : 0.0f;
        }
        case AV_SAMPLE_FMT_DBLP: {
            const double *data = (const double*)frame->data[ch];
            return data ? (float)data[sample_idx] : 0.0f;
        }
        case AV_SAMPLE_FMT_DBL: {
            const double *data = (const double*)frame->data[0];
            return data ? (float)data[sample_idx * in_channels + ch] : 0.0f;
        }
        case AV_SAMPLE_FMT_U8P: {
            const uint8_t *data = (const uint8_t*)frame->data[ch];
            return data ? ((data[sample_idx] - 128) / 128.0f) : 0.0f;
        }
        case AV_SAMPLE_FMT_U8: {
            const uint8_t *data = (const uint8_t*)frame->data[0];
            return data ? ((data[sample_idx * in_channels + ch] - 128) / 128.0f) : 0.0f;
        }
        default:
            return 0.0f;
    }
}

static void convert_audio_frame_to_pcm_s16(const AVFrame *frame, std::vector<int16_t> &out_pcm, int out_channels) {
    if (!frame || frame->nb_samples <= 0) return;

    int in_channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 2;
    int nb_samples = frame->nb_samples;

    out_pcm.resize(nb_samples * out_channels);

    for (int s = 0; s < nb_samples; s++) {
        if (out_channels == 1) {
            float val = 0.0f;
            if (in_channels == 1) {
                val = get_audio_sample_float(frame, 0, s, in_channels);
            } else {
                val = 0.5f * (get_audio_sample_float(frame, 0, s, in_channels) +
                              get_audio_sample_float(frame, 1, s, in_channels));
            }
            val = std::max(-1.0f, std::min(1.0f, val));
            out_pcm[s] = (int16_t)std::round(val * 32767.0f);
        } else {
            float left = 0.0f;
            float right = 0.0f;
            if (in_channels == 1) {
                left = right = get_audio_sample_float(frame, 0, s, in_channels);
            } else if (in_channels == 2) {
                left = get_audio_sample_float(frame, 0, s, in_channels);
                right = get_audio_sample_float(frame, 1, s, in_channels);
            } else {
                // 5.1 / 7.1 surround downmix to stereo
                float center = (in_channels > 2) ? get_audio_sample_float(frame, 2, s, in_channels) : 0.0f;
                float l_surround = (in_channels > 4) ? get_audio_sample_float(frame, 4, s, in_channels) : 0.0f;
                float r_surround = (in_channels > 5) ? get_audio_sample_float(frame, 5, s, in_channels) : 0.0f;

                left = get_audio_sample_float(frame, 0, s, in_channels) + 0.707f * center + 0.707f * l_surround;
                right = get_audio_sample_float(frame, 1, s, in_channels) + 0.707f * center + 0.707f * r_surround;
            }
            left = std::max(-1.0f, std::min(1.0f, left));
            right = std::max(-1.0f, std::min(1.0f, right));
            out_pcm[s * 2 + 0] = (int16_t)std::round(left * 32767.0f);
            out_pcm[s * 2 + 1] = (int16_t)std::round(right * 32767.0f);
        }
    }
}

// ============================================================================
// MODE 0: FAST LOSSLESS STREAM COPY (-c copy)
// ============================================================================

static std::string cut_clip_fast(const char *input_path, const char *output_path, double start_sec, double end_sec) {
    AVFormatContext *in_fmt_ctx = nullptr;
    AVFormatContext *out_fmt_ctx = nullptr;
    AVPacket *pkt = nullptr;
    int ret = 0;
    std::string error_message = "";

    do {
        ret = avformat_open_input(&in_fmt_ctx, input_path, nullptr, nullptr);
        if (ret < 0) {
            error_message = "Failed to open input: " + av_err_to_string(ret);
            ALOGE("Clipper | %s", error_message.c_str());
            break;
        }

        ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
        if (ret < 0) {
            error_message = "Failed to retrieve input stream info: " + av_err_to_string(ret);
            ALOGE("Clipper | %s", error_message.c_str());
            break;
        }

        ret = avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr, output_path);
        if (!out_fmt_ctx) {
            error_message = "Failed to allocate output format context: " + av_err_to_string(ret);
            ALOGE("Clipper | %s", error_message.c_str());
            break;
        }

        std::vector<int> stream_mapping(in_fmt_ctx->nb_streams, -1);
        int out_stream_idx = 0;
        bool has_video = false;

        for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
            AVStream *in_stream = in_fmt_ctx->streams[i];
            AVCodecParameters *in_codecpar = in_stream->codecpar;

            if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                continue;
            }

            if (in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                has_video = true;
            }

            AVStream *out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
            if (!out_stream) {
                error_message = "Failed to create new output stream";
                ALOGE("Clipper | %s", error_message.c_str());
                ret = AVERROR_UNKNOWN;
                break;
            }

            ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
            if (ret < 0) {
                error_message = "Failed to copy codec parameters: " + av_err_to_string(ret);
                ALOGE("Clipper | %s", error_message.c_str());
                break;
            }

            out_stream->codecpar->codec_tag = 0;
            out_stream->time_base = in_stream->time_base;
            stream_mapping[i] = out_stream_idx++;
        }

        if (ret < 0 || out_stream_idx == 0) {
            if (error_message.empty()) {
                error_message = "No valid video, audio, or subtitle streams found to copy";
            }
            break;
        }

        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&out_fmt_ctx->pb, output_path, AVIO_FLAG_WRITE);
            if (ret < 0) {
                error_message = "Failed to open output file for writing: " + av_err_to_string(ret);
                ALOGE("Clipper | %s", error_message.c_str());
                break;
            }
        }

        ret = avformat_write_header(out_fmt_ctx, nullptr);
        if (ret < 0) {
            error_message = "Failed to write header to output: " + av_err_to_string(ret);
            ALOGE("Clipper | %s", error_message.c_str());
            break;
        }

        // Seek backward to nearest keyframe before start_sec
        int64_t seek_target = (int64_t)(start_sec * AV_TIME_BASE);
        ret = avformat_seek_file(in_fmt_ctx, -1, INT64_MIN, seek_target, seek_target, 0);
        if (ret < 0) {
            ret = av_seek_frame(in_fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD);
        }

        pkt = av_packet_alloc();
        if (!pkt) {
            error_message = "Failed to allocate AVPacket";
            break;
        }

        double common_start_sec = -1.0;
        std::vector<int64_t> last_dts(out_stream_idx, AV_NOPTS_VALUE);
        std::vector<bool> stream_ended(in_fmt_ctx->nb_streams, false);

        while (av_read_frame(in_fmt_ctx, pkt) >= 0) {
            int in_idx = pkt->stream_index;
            if (in_idx < 0 || in_idx >= (int)in_fmt_ctx->nb_streams || stream_mapping[in_idx] < 0) {
                av_packet_unref(pkt);
                continue;
            }

            if (stream_ended[in_idx]) {
                av_packet_unref(pkt);
                continue;
            }

            AVStream *in_st = in_fmt_ctx->streams[in_idx];
            AVStream *out_st = out_fmt_ctx->streams[stream_mapping[in_idx]];

            double pkt_sec = 0.0;
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt_sec = pkt->pts * av_q2d(in_st->time_base);
            } else if (pkt->dts != AV_NOPTS_VALUE) {
                pkt_sec = pkt->dts * av_q2d(in_st->time_base);
            }

            // In audio-only streams, we can drop packets prior to exact start_sec
            if (!has_video && pkt_sec < start_sec - 0.001) {
                av_packet_unref(pkt);
                continue;
            }

            if (pkt_sec > end_sec + 0.05) {
                stream_ended[in_idx] = true;
                av_packet_unref(pkt);

                // Check if all active streams have reached end boundary
                bool all_ended = true;
                for (unsigned int s = 0; s < in_fmt_ctx->nb_streams; s++) {
                    if (stream_mapping[s] >= 0 && !stream_ended[s]) {
                        all_ended = false;
                        break;
                    }
                }
                if (all_ended) break;
                continue;
            }

            // Record common global origin timestamp on first written packet
            if (common_start_sec < 0.0) {
                common_start_sec = pkt_sec;
            }

            int64_t base_ts = (int64_t)(common_start_sec / av_q2d(in_st->time_base));
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= base_ts;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= base_ts;

            av_packet_rescale_ts(pkt, in_st->time_base, out_st->time_base);
            pkt->stream_index = stream_mapping[in_idx];
            pkt->pos = -1;

            int out_idx = pkt->stream_index;
            if (pkt->dts != AV_NOPTS_VALUE && last_dts[out_idx] != AV_NOPTS_VALUE) {
                if (pkt->dts <= last_dts[out_idx]) {
                    pkt->dts = last_dts[out_idx] + 1;
                }
            }
            if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
                pkt->pts = pkt->dts;
            }
            last_dts[out_idx] = pkt->dts;

            ret = av_interleaved_write_frame(out_fmt_ctx, pkt);
            av_packet_unref(pkt);

            if (ret < 0) break;
        }

        av_write_trailer(out_fmt_ctx);
    } while (0);

    if (pkt) av_packet_free(&pkt);
    if (out_fmt_ctx) {
        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE) && out_fmt_ctx->pb) {
            avio_closep(&out_fmt_ctx->pb);
        }
        avformat_free_context(out_fmt_ctx);
    }
    if (in_fmt_ctx) avformat_close_input(&in_fmt_ctx);

    return error_message;
}

// ============================================================================
// MODE 1: FRAME-ACCURATE HARDWARE EXPORT (Video: AMediaCodec H.264, Audio: AMediaCodec AAC)
// ============================================================================

struct MuxerPendingSample {
    int track_type; // 0 = video, 1 = audio
    std::vector<uint8_t> data;
    AMediaCodecBufferInfo info;
};

static std::string cut_clip_frame_accurate(const char *input_path, const char *output_path, double start_sec, double end_sec) {
    AVFormatContext *in_fmt_ctx = nullptr;
    AVCodecContext *v_dec_ctx = nullptr;
    AVCodecContext *a_dec_ctx = nullptr;
    AMediaCodec *v_encoder = nullptr;
    AMediaCodec *a_encoder = nullptr;
    AMediaMuxer *muxer = nullptr;
    struct SwsContext *sws_ctx = nullptr;
    AVFrame *v_frame = nullptr;
    AVFrame *a_frame = nullptr;
    AVPacket *pkt = nullptr;
    int out_fd = -1;
    std::string error_message = "";
    int ret = 0;

    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    ssize_t video_track_idx = -1;
    ssize_t audio_track_idx = -1;
    bool muxer_started = false;

    int a_out_channels = 2;
    int a_sample_rate = 44100;
    double a_current_time_sec = 0.0;

    std::vector<MuxerPendingSample> pending_samples;

    do {
        ret = avformat_open_input(&in_fmt_ctx, input_path, nullptr, nullptr);
        if (ret < 0) {
            error_message = "Failed to open input: " + av_err_to_string(ret);
            break;
        }

        ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
        if (ret < 0) {
            error_message = "Failed to retrieve input stream info: " + av_err_to_string(ret);
            break;
        }

        for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
            if (in_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx == -1) {
                video_stream_idx = i;
            } else if (in_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx == -1) {
                audio_stream_idx = i;
            }
        }

        if (video_stream_idx == -1 && audio_stream_idx == -1) {
            error_message = "No video or audio stream found in input file";
            break;
        }

        bool has_video = (video_stream_idx >= 0);
        bool has_audio = (audio_stream_idx >= 0);

        AVStream *in_v_stream = has_video ? in_fmt_ctx->streams[video_stream_idx] : nullptr;
        AVStream *in_a_stream = has_audio ? in_fmt_ctx->streams[audio_stream_idx] : nullptr;

        // 1. Initialize Video Decoder & Hardware Encoder
        int v_width = 0, v_height = 0;
        int v_stride = 0, v_slice_height = 0;
        size_t v_y_plane_size = 0, v_uv_plane_size = 0, v_total_yuv_size = 0;
        std::vector<uint8_t> v_nv12_buffer;

        if (has_video) {
            AVCodecParameters *v_codecpar = in_v_stream->codecpar;
            const AVCodec *v_decoder = avcodec_find_decoder(v_codecpar->codec_id);
            if (!v_decoder) {
                error_message = "Unsupported video codec for decoding";
                break;
            }

            v_dec_ctx = avcodec_alloc_context3(v_decoder);
            if (!v_dec_ctx || avcodec_parameters_to_context(v_dec_ctx, v_codecpar) < 0 || avcodec_open2(v_dec_ctx, v_decoder, nullptr) < 0) {
                error_message = "Failed to initialize video decoder";
                break;
            }

            v_width = v_dec_ctx->width > 0 ? v_dec_ctx->width : v_codecpar->width;
            v_height = v_dec_ctx->height > 0 ? v_dec_ctx->height : v_codecpar->height;

            v_width = (v_width / 2) * 2;
            v_height = (v_height / 2) * 2;

            double fps = av_q2d(in_v_stream->r_frame_rate);
            if (fps <= 0.0 || fps > 120.0) fps = 30.0;

            int bitrate = (int)v_codecpar->bit_rate;
            if (bitrate <= 0) {
                bitrate = (int)(v_width * v_height * fps * 0.12);
                if (bitrate < 2500000) bitrate = 2500000;
                if (bitrate > 20000000) bitrate = 20000000;
            }

            v_encoder = AMediaCodec_createEncoderByType("video/avc");
            if (!v_encoder) {
                error_message = "Android hardware H.264 encoder unavailable";
                break;
            }

            AMediaFormat *v_enc_format = AMediaFormat_new();
            AMediaFormat_setString(v_enc_format, AMEDIAFORMAT_KEY_MIME, "video/avc");
            AMediaFormat_setInt32(v_enc_format, AMEDIAFORMAT_KEY_WIDTH, v_width);
            AMediaFormat_setInt32(v_enc_format, AMEDIAFORMAT_KEY_HEIGHT, v_height);
            AMediaFormat_setInt32(v_enc_format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); // COLOR_FormatYUV420SemiPlanar (NV12)
            AMediaFormat_setInt32(v_enc_format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
            AMediaFormat_setInt32(v_enc_format, AMEDIAFORMAT_KEY_FRAME_RATE, (int)std::round(fps));
            AMediaFormat_setInt32(v_enc_format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);

            media_status_t v_status = AMediaCodec_configure(v_encoder, v_enc_format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
            AMediaFormat_delete(v_enc_format);

            if (v_status != AMEDIA_OK || AMediaCodec_start(v_encoder) != AMEDIA_OK) {
                error_message = "Failed to configure/start video encoder";
                break;
            }

            v_stride = (v_width + 15) & ~15;
            v_slice_height = (v_height + 15) & ~15;
            typedef AMediaFormat* (*pfn_getInputFormat)(AMediaCodec*);
            static pfn_getInputFormat fn_getInputFormat = (pfn_getInputFormat)dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat");
            if (fn_getInputFormat) {
                AMediaFormat *input_format = fn_getInputFormat(v_encoder);
                if (input_format) {
                    int32_t val = 0;
                    if (AMediaFormat_getInt32(input_format, "stride", &val) && val > 0) v_stride = val;
                    if (AMediaFormat_getInt32(input_format, "slice-height", &val) && val > 0) v_slice_height = val;
                    AMediaFormat_delete(input_format);
                }
            }

            if (v_stride < v_width) v_stride = v_width;
            if (v_slice_height < v_height) v_slice_height = v_height;

            v_y_plane_size = (size_t)(v_stride * v_slice_height);
            v_uv_plane_size = (size_t)(v_stride * (v_slice_height / 2));
            v_total_yuv_size = v_y_plane_size + v_uv_plane_size;

            v_nv12_buffer.assign(v_total_yuv_size, 0);
            memset(v_nv12_buffer.data() + v_y_plane_size, 0x80, v_uv_plane_size);
        }

        // 2. Initialize Audio Decoder & AAC Encoder
        if (has_audio) {
            AVCodecParameters *a_codecpar = in_a_stream->codecpar;
            const AVCodec *a_decoder = avcodec_find_decoder(a_codecpar->codec_id);
            if (a_decoder) {
                a_dec_ctx = avcodec_alloc_context3(a_decoder);
                if (a_dec_ctx && avcodec_parameters_to_context(a_dec_ctx, a_codecpar) >= 0 && avcodec_open2(a_dec_ctx, a_decoder, nullptr) >= 0) {
                    int in_ch = a_dec_ctx->ch_layout.nb_channels > 0 ? a_dec_ctx->ch_layout.nb_channels : (a_codecpar->ch_layout.nb_channels > 0 ? a_codecpar->ch_layout.nb_channels : 2);
                    a_out_channels = (in_ch == 1) ? 1 : 2;
                    a_sample_rate = a_dec_ctx->sample_rate > 0 ? a_dec_ctx->sample_rate : 44100;

                    // Clamp to standard AAC sample rates supported by MediaCodec
                    static const int standard_rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
                    bool valid_rate = false;
                    for (int rate : standard_rates) {
                        if (a_sample_rate == rate) { valid_rate = true; break; }
                    }
                    if (!valid_rate) a_sample_rate = 44100;

                    int a_bitrate = (a_out_channels == 1) ? 96000 : 160000;

                    a_encoder = AMediaCodec_createEncoderByType("audio/mp4a-latm");
                    if (a_encoder) {
                        AMediaFormat *a_enc_format = AMediaFormat_new();
                        AMediaFormat_setString(a_enc_format, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
                        AMediaFormat_setInt32(a_enc_format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, a_out_channels);
                        AMediaFormat_setInt32(a_enc_format, AMEDIAFORMAT_KEY_SAMPLE_RATE, a_sample_rate);
                        AMediaFormat_setInt32(a_enc_format, AMEDIAFORMAT_KEY_BIT_RATE, a_bitrate);
                        AMediaFormat_setInt32(a_enc_format, AMEDIAFORMAT_KEY_AAC_PROFILE, 2); // AACObjectLC

                        media_status_t a_status = AMediaCodec_configure(a_encoder, a_enc_format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
                        AMediaFormat_delete(a_enc_format);

                        if (a_status != AMEDIA_OK || AMediaCodec_start(a_encoder) != AMEDIA_OK) {
                            ALOGW("Clipper | Failed to start AAC audio encoder, audio will be skipped");
                            AMediaCodec_delete(a_encoder);
                            a_encoder = nullptr;
                            has_audio = false;
                        }
                    } else {
                        ALOGW("Clipper | AAC audio encoder unavailable, audio will be skipped");
                        has_audio = false;
                    }
                } else {
                    ALOGW("Clipper | Failed to open audio decoder, audio will be skipped");
                    has_audio = false;
                }
            } else {
                ALOGW("Clipper | Audio decoder not found for codec %d", a_codecpar->codec_id);
                has_audio = false;
            }
        }

        // 3. Initialize Output Muxer
        out_fd = open(output_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (out_fd < 0) {
            error_message = "Failed to create output file: " + std::string(output_path);
            break;
        }

        muxer = AMediaMuxer_new(out_fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!muxer) {
            error_message = "Failed to initialize native MediaMuxer";
            break;
        }

        // 4. Drain & Mux Helper
        auto drain_encoder = [&](int track_type, bool wait_eos) {
            AMediaCodec *enc = (track_type == 0) ? v_encoder : a_encoder;
            if (!enc) return;

            AMediaCodecBufferInfo info;
            int try_count = 0;
            while (true) {
                ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(enc, &info, wait_eos ? 30000 : 0);
                if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                    if (!wait_eos) break;
                    if (++try_count > 30) break; // Timeout after ~1s wait on EOS
                    continue;
                }
                if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    AMediaFormat *format = AMediaCodec_getOutputFormat(enc);
                    if (track_type == 0) {
                        video_track_idx = AMediaMuxer_addTrack(muxer, format);
                        ALOGI("Clipper | Video track registered (idx=%zd)", video_track_idx);
                    } else {
                        audio_track_idx = AMediaMuxer_addTrack(muxer, format);
                        ALOGI("Clipper | Audio track registered (idx=%zd)", audio_track_idx);
                    }
                    AMediaFormat_delete(format);

                    bool video_ready = (v_encoder == nullptr || video_track_idx >= 0);
                    bool audio_ready = (a_encoder == nullptr || audio_track_idx >= 0);

                    if (video_ready && audio_ready && !muxer_started) {
                        media_status_t m_stat = AMediaMuxer_start(muxer);
                        if (m_stat == AMEDIA_OK) {
                            muxer_started = true;
                            ALOGI("Clipper | AMediaMuxer started; flushing %zu pending samples", pending_samples.size());
                            for (const auto &sample : pending_samples) {
                                ssize_t target = (sample.track_type == 0) ? video_track_idx : audio_track_idx;
                                if (target >= 0 && !sample.data.empty()) {
                                    AMediaCodecBufferInfo b_info = sample.info;
                                    AMediaMuxer_writeSampleData(muxer, target, (const uint8_t*)sample.data.data(), &b_info);
                                }
                            }
                            pending_samples.clear();
                        } else {
                            ALOGE("Clipper | AMediaMuxer_start failed (%d)", m_stat);
                        }
                    }
                    continue;
                }
                if (out_idx < 0) break;

                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                    AMediaCodec_releaseOutputBuffer(enc, out_idx, false);
                    break;
                }

                if (info.size > 0) {
                    size_t out_buf_size = 0;
                    uint8_t *out_buf = AMediaCodec_getOutputBuffer(enc, out_idx, &out_buf_size);
                    if (out_buf) {
                        if (muxer_started) {
                            ssize_t target = (track_type == 0) ? video_track_idx : audio_track_idx;
                            if (target >= 0) {
                                AMediaMuxer_writeSampleData(muxer, target, out_buf, &info);
                            }
                        } else {
                            MuxerPendingSample sample;
                            sample.track_type = track_type;
                            sample.data.assign(out_buf + info.offset, out_buf + info.offset + info.size);
                            sample.info = info;
                            sample.info.offset = 0;
                            pending_samples.push_back(std::move(sample));
                        }
                    }
                }

                AMediaCodec_releaseOutputBuffer(enc, out_idx, false);
            }
        };

        // 5. Audio Feeding Helper
        auto feed_audio_pcm = [&](const int16_t *pcm_data, int sample_count, uint64_t pts_us) {
            if (!a_encoder || sample_count <= 0 || !pcm_data) return;

            size_t bytes_to_feed = (size_t)sample_count * a_out_channels * sizeof(int16_t);
            size_t bytes_offset = 0;

            while (bytes_offset < bytes_to_feed) {
                ssize_t in_buf_idx = AMediaCodec_dequeueInputBuffer(a_encoder, 30000);
                if (in_buf_idx < 0) {
                    drain_encoder(1, false);
                    continue;
                }

                size_t in_buf_size = 0;
                uint8_t *in_buf = AMediaCodec_getInputBuffer(a_encoder, in_buf_idx, &in_buf_size);
                if (!in_buf || in_buf_size == 0) {
                    AMediaCodec_queueInputBuffer(a_encoder, in_buf_idx, 0, 0, 0, 0);
                    break;
                }

                size_t chunk_bytes = std::min(bytes_to_feed - bytes_offset, in_buf_size);
                memcpy(in_buf, ((const uint8_t*)pcm_data) + bytes_offset, chunk_bytes);

                uint64_t chunk_pts = pts_us + (uint64_t)((bytes_offset / (double)(a_out_channels * sizeof(int16_t))) * 1000000.0 / a_sample_rate);
                AMediaCodec_queueInputBuffer(a_encoder, in_buf_idx, 0, chunk_bytes, chunk_pts, 0);
                bytes_offset += chunk_bytes;

                drain_encoder(1, false);
            }
        };

        auto process_audio_frame = [&](AVFrame *frame) {
            if (!a_encoder || !frame || frame->nb_samples <= 0) return;

            int64_t pts = frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame->pts;
            if (pts == AV_NOPTS_VALUE) pts = frame->pkt_dts;

            double frame_sec = (pts != AV_NOPTS_VALUE) ? (pts * av_q2d(in_a_stream->time_base)) : a_current_time_sec;
            double frame_dur = (double)frame->nb_samples / (double)(frame->sample_rate > 0 ? frame->sample_rate : a_sample_rate);
            double frame_end_sec = frame_sec + frame_dur;
            a_current_time_sec = frame_end_sec;

            if (frame_end_sec <= start_sec) return;
            if (frame_sec >= end_sec) return;

            std::vector<int16_t> pcm_buf;
            convert_audio_frame_to_pcm_s16(frame, pcm_buf, a_out_channels);
            int total_samples = (int)pcm_buf.size() / a_out_channels;
            if (total_samples <= 0) return;

            int start_offset = 0;
            if (frame_sec < start_sec) {
                start_offset = (int)std::round((start_sec - frame_sec) * a_sample_rate);
                if (start_offset < 0) start_offset = 0;
                if (start_offset >= total_samples) return;
            }

            int valid_samples = total_samples - start_offset;
            if (frame_end_sec > end_sec) {
                int max_samples = (int)std::round((end_sec - std::max(start_sec, frame_sec)) * a_sample_rate);
                if (max_samples < valid_samples) {
                    valid_samples = std::max(0, max_samples);
                }
            }

            if (valid_samples <= 0) return;

            double effective_sec = std::max(start_sec, frame_sec + (start_offset / (double)a_sample_rate));
            uint64_t pts_us = (uint64_t)((effective_sec - start_sec) * 1000000.0);

            const int16_t *pcm_ptr = pcm_buf.data() + (start_offset * a_out_channels);
            feed_audio_pcm(pcm_ptr, valid_samples, pts_us);
        };

        // 6. Seek to Start Keyframe
        int64_t seek_target = (int64_t)(start_sec * AV_TIME_BASE);
        avformat_seek_file(in_fmt_ctx, -1, INT64_MIN, seek_target, seek_target, 0);
        if (v_dec_ctx) avcodec_flush_buffers(v_dec_ctx);
        if (a_dec_ctx) avcodec_flush_buffers(a_dec_ctx);

        pkt = av_packet_alloc();
        v_frame = av_frame_alloc();
        a_frame = av_frame_alloc();

        bool video_done = !has_video;
        bool audio_done = !has_audio;

        while ((!video_done || !audio_done) && av_read_frame(in_fmt_ctx, pkt) >= 0) {
            if (has_video && pkt->stream_index == video_stream_idx && !video_done) {
                if (avcodec_send_packet(v_dec_ctx, pkt) >= 0) {
                    while (avcodec_receive_frame(v_dec_ctx, v_frame) == 0) {
                        int64_t pts = v_frame->best_effort_timestamp;
                        if (pts == AV_NOPTS_VALUE) pts = v_frame->pts;
                        if (pts == AV_NOPTS_VALUE) pts = v_frame->pkt_dts;

                        double frame_sec = (pts != AV_NOPTS_VALUE) ? (pts * av_q2d(in_v_stream->time_base)) : 0.0;

                        if (frame_sec < start_sec - 0.001) {
                            continue;
                        }
                        if (frame_sec > end_sec + 0.001) {
                            video_done = true;
                            break;
                        }

                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(
                                v_frame->width, v_frame->height, (AVPixelFormat)v_frame->format,
                                v_width, v_height, AV_PIX_FMT_NV12,
                                SWS_BILINEAR, nullptr, nullptr, nullptr
                            );
                        }

                        uint8_t *dst_data[4] = {
                            v_nv12_buffer.data(),
                            v_nv12_buffer.data() + v_y_plane_size,
                            nullptr, nullptr
                        };
                        int dst_linesize[4] = { v_stride, v_stride, 0, 0 };

                        sws_scale(sws_ctx, v_frame->data, v_frame->linesize, 0, v_frame->height, dst_data, dst_linesize);

                        ssize_t in_buf_idx = AMediaCodec_dequeueInputBuffer(v_encoder, 50000);
                        if (in_buf_idx >= 0) {
                            size_t in_buf_size = 0;
                            uint8_t *in_buf = AMediaCodec_getInputBuffer(v_encoder, in_buf_idx, &in_buf_size);
                            if (in_buf) {
                                size_t copy_size = std::min(in_buf_size, v_total_yuv_size);
                                memcpy(in_buf, v_nv12_buffer.data(), copy_size);
                                uint64_t pts_us = (uint64_t)((frame_sec - start_sec) * 1000000.0);
                                AMediaCodec_queueInputBuffer(v_encoder, in_buf_idx, 0, copy_size, pts_us, 0);
                            }
                        }

                        drain_encoder(0, false);
                    }
                }
            } else if (has_audio && pkt->stream_index == audio_stream_idx && !audio_done) {
                if (avcodec_send_packet(a_dec_ctx, pkt) >= 0) {
                    while (avcodec_receive_frame(a_dec_ctx, a_frame) == 0) {
                        process_audio_frame(a_frame);
                        if (a_current_time_sec > end_sec + 0.1) {
                            audio_done = true;
                            break;
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        }

        // 7. Flush Decoders & Drain Encoders
        if (has_video && v_dec_ctx) {
            if (avcodec_send_packet(v_dec_ctx, nullptr) >= 0) {
                while (avcodec_receive_frame(v_dec_ctx, v_frame) == 0) {
                    int64_t pts = v_frame->best_effort_timestamp;
                    if (pts == AV_NOPTS_VALUE) pts = v_frame->pts;
                    double frame_sec = (pts != AV_NOPTS_VALUE) ? (pts * av_q2d(in_v_stream->time_base)) : 0.0;
                    if (frame_sec >= start_sec - 0.001 && frame_sec <= end_sec + 0.001) {
                        uint8_t *dst_data[4] = {
                            v_nv12_buffer.data(),
                            v_nv12_buffer.data() + v_y_plane_size,
                            nullptr, nullptr
                        };
                        int dst_linesize[4] = { v_stride, v_stride, 0, 0 };
                        sws_scale(sws_ctx, v_frame->data, v_frame->linesize, 0, v_frame->height, dst_data, dst_linesize);

                        ssize_t in_buf_idx = AMediaCodec_dequeueInputBuffer(v_encoder, 50000);
                        if (in_buf_idx >= 0) {
                            size_t in_buf_size = 0;
                            uint8_t *in_buf = AMediaCodec_getInputBuffer(v_encoder, in_buf_idx, &in_buf_size);
                            if (in_buf) {
                                size_t copy_size = std::min(in_buf_size, v_total_yuv_size);
                                memcpy(in_buf, v_nv12_buffer.data(), copy_size);
                                uint64_t pts_us = (uint64_t)((frame_sec - start_sec) * 1000000.0);
                                AMediaCodec_queueInputBuffer(v_encoder, in_buf_idx, 0, copy_size, pts_us, 0);
                            }
                        }
                        drain_encoder(0, false);
                    }
                }
            }

            ssize_t eos_idx = AMediaCodec_dequeueInputBuffer(v_encoder, 100000);
            if (eos_idx >= 0) {
                AMediaCodec_queueInputBuffer(v_encoder, eos_idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            }
            drain_encoder(0, true);
        }

        if (has_audio && a_dec_ctx) {
            if (avcodec_send_packet(a_dec_ctx, nullptr) >= 0) {
                while (avcodec_receive_frame(a_dec_ctx, a_frame) == 0) {
                    process_audio_frame(a_frame);
                }
            }

            if (a_encoder) {
                ssize_t a_eos_idx = AMediaCodec_dequeueInputBuffer(a_encoder, 100000);
                if (a_eos_idx >= 0) {
                    AMediaCodec_queueInputBuffer(a_encoder, a_eos_idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                }
                drain_encoder(1, true);
            }
        }

        if (muxer_started) {
            AMediaMuxer_stop(muxer);
        }
    } while (0);

    // 8. Cleanup
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (v_frame) av_frame_free(&v_frame);
    if (a_frame) av_frame_free(&a_frame);
    if (pkt) av_packet_free(&pkt);

    if (v_encoder) {
        AMediaCodec_stop(v_encoder);
        AMediaCodec_delete(v_encoder);
    }
    if (a_encoder) {
        AMediaCodec_stop(a_encoder);
        AMediaCodec_delete(a_encoder);
    }
    if (muxer) {
        AMediaMuxer_delete(muxer);
    }
    if (out_fd >= 0) {
        close(out_fd);
    }
    if (v_dec_ctx) avcodec_free_context(&v_dec_ctx);
    if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
    if (in_fmt_ctx) avformat_close_input(&in_fmt_ctx);

    return error_message;
}

// ============================================================================
// JNI INTERFACE
// ============================================================================

extern "C" {
    jni_func(jstring, cutClipNative, jstring jinput, jstring joutput, jdouble start_sec, jdouble end_sec, jint mode);
};

jni_func(jstring, cutClipNative, jstring jinput, jstring joutput, jdouble start_sec, jdouble end_sec, jint mode) {
    if (!jinput || !joutput) {
        return env->NewStringUTF("Input or output path is null");
    }

    if (start_sec < 0.0) {
        start_sec = 0.0;
    }
    if (end_sec <= start_sec) {
        return env->NewStringUTF("End time must be greater than start time");
    }

    const char *input_path = env->GetStringUTFChars(jinput, NULL);
    const char *output_path = env->GetStringUTFChars(joutput, NULL);

    ALOGI("Clipper | Start cut: mode=%d, input=%s, output=%s, start=%.3f, end=%.3f",
          mode, input_path, output_path, start_sec, end_sec);

    std::string error_message;
    if (mode == 1) {
        // Mode 1: Frame-accurate hardware transcode (AMediaCodec H.264 + AAC + AMediaMuxer)
        error_message = cut_clip_frame_accurate(input_path, output_path, start_sec, end_sec);
        if (!error_message.empty()) {
            ALOGW("Clipper | Frame-accurate cut failed (%s)", error_message.c_str());
        }
    } else {
        // Mode 0: Fast lossless stream copy
        error_message = cut_clip_fast(input_path, output_path, start_sec, end_sec);
    }

    env->ReleaseStringUTFChars(jinput, input_path);
    env->ReleaseStringUTFChars(joutput, output_path);

    if (!error_message.empty()) {
        return env->NewStringUTF(error_message.c_str());
    }

    return NULL;
}
