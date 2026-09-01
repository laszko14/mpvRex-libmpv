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
    #include <libswscale/swscale.h>
};

#include "jni_utils.h"
#include "log.h"

static std::string av_err_to_string(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
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

        for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
            AVStream *in_stream = in_fmt_ctx->streams[i];
            AVCodecParameters *in_codecpar = in_stream->codecpar;

            if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                continue;
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

        std::vector<int64_t> first_dts(in_fmt_ctx->nb_streams, AV_NOPTS_VALUE);
        std::vector<int64_t> last_dts(in_fmt_ctx->nb_streams, AV_NOPTS_VALUE);

        while (av_read_frame(in_fmt_ctx, pkt) >= 0) {
            int in_idx = pkt->stream_index;
            if (in_idx < 0 || in_idx >= (int)in_fmt_ctx->nb_streams || stream_mapping[in_idx] < 0) {
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

            if (pkt_sec > end_sec + 0.1) {
                av_packet_unref(pkt);
                break;
            }

            if (first_dts[in_idx] == AV_NOPTS_VALUE) {
                first_dts[in_idx] = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            }

            int64_t base = first_dts[in_idx];
            if (base != AV_NOPTS_VALUE) {
                if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= base;
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= base;
            }

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
// MODE 1: FRAME-ACCURATE HARDWARE ENCODED EXPORT (libavcodec + AMediaCodec + AMediaMuxer)
// ============================================================================

static std::string cut_clip_frame_accurate(const char *input_path, const char *output_path, double start_sec, double end_sec) {
    AVFormatContext *in_fmt_ctx = nullptr;
    AVCodecContext *dec_ctx = nullptr;
    AMediaCodec *encoder = nullptr;
    AMediaMuxer *muxer = nullptr;
    struct SwsContext *sws_ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    int out_fd = -1;
    std::string error_message = "";
    int ret = 0;

    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    ssize_t video_track_idx = -1;
    ssize_t audio_track_idx = -1;
    bool muxer_started = false;

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

        if (video_stream_idx == -1) {
            error_message = "No video stream found in input file";
            break;
        }

        AVStream *in_v_stream = in_fmt_ctx->streams[video_stream_idx];
        AVCodecParameters *v_codecpar = in_v_stream->codecpar;

        // Initialize video decoder
        const AVCodec *decoder = avcodec_find_decoder(v_codecpar->codec_id);
        if (!decoder) {
            error_message = "Unsupported video codec for decoding";
            break;
        }

        dec_ctx = avcodec_alloc_context3(decoder);
        if (!dec_ctx || avcodec_parameters_to_context(dec_ctx, v_codecpar) < 0 || avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
            error_message = "Failed to initialize video decoder";
            break;
        }

        int width = dec_ctx->width > 0 ? dec_ctx->width : v_codecpar->width;
        int height = dec_ctx->height > 0 ? dec_ctx->height : v_codecpar->height;

        // Ensure width and height are even for hardware encoder alignment
        width = (width / 2) * 2;
        height = (height / 2) * 2;

        double fps = av_q2d(in_v_stream->r_frame_rate);
        if (fps <= 0.0 || fps > 120.0) fps = 30.0;

        int bitrate = (int)v_codecpar->bit_rate;
        if (bitrate <= 0) {
            bitrate = (int)(width * height * fps * 0.12);
            if (bitrate < 2500000) bitrate = 2500000;
            if (bitrate > 20000000) bitrate = 20000000;
        }

        // Open output file descriptor for AMediaMuxer
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

        // Initialize Android MediaCodec Hardware Encoder (video/avc - H.264)
        encoder = AMediaCodec_createEncoderByType("video/avc");
        if (!encoder) {
            error_message = "Android hardware H.264 encoder unavailable";
            break;
        }

        AMediaFormat *enc_format = AMediaFormat_new();
        AMediaFormat_setString(enc_format, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(enc_format, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(enc_format, AMEDIAFORMAT_KEY_HEIGHT, height);
        AMediaFormat_setInt32(enc_format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); // COLOR_FormatYUV420SemiPlanar (NV12)
        AMediaFormat_setInt32(enc_format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
        AMediaFormat_setInt32(enc_format, AMEDIAFORMAT_KEY_FRAME_RATE, (int)std::round(fps));
        AMediaFormat_setInt32(enc_format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);

        media_status_t status = AMediaCodec_configure(encoder, enc_format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(enc_format);

        if (status != AMEDIA_OK) {
            error_message = "Failed to configure hardware encoder";
            break;
        }

        status = AMediaCodec_start(encoder);
        if (status != AMEDIA_OK) {
            error_message = "Failed to start hardware encoder";
            break;
        }

        // Query actual input stride and slice-height from hardware encoder if available
        int stride = (width + 15) & ~15;
        int slice_height = (height + 15) & ~15;
        typedef AMediaFormat* (*pfn_getInputFormat)(AMediaCodec*);
        static pfn_getInputFormat fn_getInputFormat = (pfn_getInputFormat)dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat");
        if (fn_getInputFormat) {
            AMediaFormat *input_format = fn_getInputFormat(encoder);
            if (input_format) {
                int32_t val = 0;
                if (AMediaFormat_getInt32(input_format, "stride", &val) && val > 0) {
                    stride = val;
                }
                if (AMediaFormat_getInt32(input_format, "slice-height", &val) && val > 0) {
                    slice_height = val;
                }
                AMediaFormat_delete(input_format);
            }
        }

        if (stride < width) stride = width;
        if (slice_height < height) slice_height = height;

        size_t y_plane_size = (size_t)(stride * slice_height);
        size_t uv_plane_size = (size_t)(stride * (slice_height / 2));
        size_t total_yuv_size = y_plane_size + uv_plane_size;

        std::vector<uint8_t> nv12_buffer(total_yuv_size, 0);
        // Pre-fill UV plane with neutral 0x80 (128) to guarantee zero green padding artifacts
        memset(nv12_buffer.data() + y_plane_size, 0x80, uv_plane_size);

        // Add Audio track to AMediaMuxer if AAC audio is present
        if (audio_stream_idx >= 0) {
            AVStream *in_a_st = in_fmt_ctx->streams[audio_stream_idx];
            if (in_a_st->codecpar->codec_id == AV_CODEC_ID_AAC) {
                AMediaFormat *a_format = AMediaFormat_new();
                AMediaFormat_setString(a_format, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
                int channels = in_a_st->codecpar->ch_layout.nb_channels > 0 ? in_a_st->codecpar->ch_layout.nb_channels : 2;
                AMediaFormat_setInt32(a_format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
                AMediaFormat_setInt32(a_format, AMEDIAFORMAT_KEY_SAMPLE_RATE, in_a_st->codecpar->sample_rate > 0 ? in_a_st->codecpar->sample_rate : 44100);
                if (in_a_st->codecpar->extradata_size > 0) {
                    AMediaFormat_setBuffer(a_format, "csd-0", in_a_st->codecpar->extradata, in_a_st->codecpar->extradata_size);
                }
                audio_track_idx = AMediaMuxer_addTrack(muxer, a_format);
                AMediaFormat_delete(a_format);
            }
        }

        // Seek backward to nearest keyframe before start_sec
        int64_t seek_target = (int64_t)(start_sec * AV_TIME_BASE);
        avformat_seek_file(in_fmt_ctx, -1, INT64_MIN, seek_target, seek_target, 0);
        avcodec_flush_buffers(dec_ctx);

        pkt = av_packet_alloc();
        frame = av_frame_alloc();

        // Helper to drain encoded video output from AMediaCodec to AMediaMuxer
        auto drain_encoder = [&](bool wait_eos) {
            AMediaCodecBufferInfo info;
            while (true) {
                ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(encoder, &info, wait_eos ? 100000 : 0);
                if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                    if (!wait_eos) break;
                    continue;
                }
                if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    AMediaFormat *format = AMediaCodec_getOutputFormat(encoder);
                    video_track_idx = AMediaMuxer_addTrack(muxer, format);
                    AMediaFormat_delete(format);
                    if (!muxer_started) {
                        AMediaMuxer_start(muxer);
                        muxer_started = true;
                    }
                    continue;
                }
                if (out_idx < 0) break;

                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                    AMediaCodec_releaseOutputBuffer(encoder, out_idx, false);
                    break;
                }

                if (info.size > 0 && muxer_started && video_track_idx >= 0) {
                    size_t out_buf_size = 0;
                    uint8_t *out_buf = AMediaCodec_getOutputBuffer(encoder, out_idx, &out_buf_size);
                    if (out_buf) {
                        AMediaMuxer_writeSampleData(muxer, video_track_idx, out_buf, &info);
                    }
                }

                AMediaCodec_releaseOutputBuffer(encoder, out_idx, false);
            }
        };

        bool reached_end = false;

        while (!reached_end && av_read_frame(in_fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == video_stream_idx) {
                if (avcodec_send_packet(dec_ctx, pkt) >= 0) {
                    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                        int64_t pts = frame->best_effort_timestamp;
                        if (pts == AV_NOPTS_VALUE) pts = frame->pts;
                        if (pts == AV_NOPTS_VALUE) pts = frame->pkt_dts;

                        double frame_sec = (pts != AV_NOPTS_VALUE) ? (pts * av_q2d(in_v_stream->time_base)) : 0.0;

                        if (frame_sec < start_sec - 0.001) {
                            continue; // Discard frames prior to exact start frame
                        }
                        if (frame_sec > end_sec + 0.001) {
                            reached_end = true;
                            break;
                        }

                        // Initialize Scaler to convert to NV12
                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(
                                frame->width, frame->height, (AVPixelFormat)frame->format,
                                width, height, AV_PIX_FMT_NV12,
                                SWS_BILINEAR, nullptr, nullptr, nullptr
                            );
                        }

                        uint8_t *dst_data[4] = {
                            nv12_buffer.data(),
                            nv12_buffer.data() + y_plane_size,
                            nullptr, nullptr
                        };
                        int dst_linesize[4] = { stride, stride, 0, 0 };

                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);

                        // Feed to MediaCodec Encoder
                        ssize_t in_buf_idx = AMediaCodec_dequeueInputBuffer(encoder, 50000);
                        if (in_buf_idx >= 0) {
                            size_t in_buf_size = 0;
                            uint8_t *in_buf = AMediaCodec_getInputBuffer(encoder, in_buf_idx, &in_buf_size);
                            if (in_buf) {
                                size_t copy_size = std::min(in_buf_size, total_yuv_size);
                                memcpy(in_buf, nv12_buffer.data(), copy_size);
                                uint64_t pts_us = (uint64_t)((frame_sec - start_sec) * 1000000.0);
                                AMediaCodec_queueInputBuffer(encoder, in_buf_idx, 0, copy_size, pts_us, 0);
                            }
                        }

                        drain_encoder(false);
                    }
                }
            } else if (pkt->stream_index == audio_stream_idx && audio_track_idx >= 0 && muxer_started) {
                double a_sec = 0.0;
                AVStream *in_a_st = in_fmt_ctx->streams[audio_stream_idx];

                if (pkt->pts != AV_NOPTS_VALUE) {
                    a_sec = pkt->pts * av_q2d(in_a_st->time_base);
                } else if (pkt->dts != AV_NOPTS_VALUE) {
                    a_sec = pkt->dts * av_q2d(in_a_st->time_base);
                }

                if (a_sec >= start_sec && a_sec <= end_sec) {
                    int64_t pkt_us = (int64_t)((a_sec - start_sec) * 1000000.0);
                    if (pkt_us >= 0 && pkt->size > 0) {
                        AMediaCodecBufferInfo a_info;
                        a_info.offset = 0;
                        a_info.size = pkt->size;
                        a_info.presentationTimeUs = pkt_us;
                        a_info.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
                        AMediaMuxer_writeSampleData(muxer, audio_track_idx, pkt->data, &a_info);
                    }
                }
            }
            av_packet_unref(pkt);
        }

        // Flush video decoder
        if (avcodec_send_packet(dec_ctx, nullptr) >= 0) {
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                int64_t pts = frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) pts = frame->pts;
                double frame_sec = (pts != AV_NOPTS_VALUE) ? (pts * av_q2d(in_v_stream->time_base)) : 0.0;
                if (frame_sec >= start_sec - 0.001 && frame_sec <= end_sec + 0.001) {
                    uint8_t *dst_data[4] = {
                        nv12_buffer.data(),
                        nv12_buffer.data() + y_plane_size,
                        nullptr, nullptr
                    };
                    int dst_linesize[4] = { stride, stride, 0, 0 };
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);

                    ssize_t in_buf_idx = AMediaCodec_dequeueInputBuffer(encoder, 50000);
                    if (in_buf_idx >= 0) {
                        size_t in_buf_size = 0;
                        uint8_t *in_buf = AMediaCodec_getInputBuffer(encoder, in_buf_idx, &in_buf_size);
                        if (in_buf) {
                            size_t copy_size = std::min(in_buf_size, total_yuv_size);
                            memcpy(in_buf, nv12_buffer.data(), copy_size);
                            uint64_t pts_us = (uint64_t)((frame_sec - start_sec) * 1000000.0);
                            AMediaCodec_queueInputBuffer(encoder, in_buf_idx, 0, copy_size, pts_us, 0);
                        }
                    }
                    drain_encoder(false);
                }
            }
        }

        // Send End of Stream (EOS) to encoder
        ssize_t eos_idx = AMediaCodec_dequeueInputBuffer(encoder, 100000);
        if (eos_idx >= 0) {
            AMediaCodec_queueInputBuffer(encoder, eos_idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        }
        drain_encoder(true);

        if (muxer_started) {
            AMediaMuxer_stop(muxer);
        }
    } while (0);

    // Cleanup
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (encoder) {
        AMediaCodec_stop(encoder);
        AMediaCodec_delete(encoder);
    }
    if (muxer) {
        AMediaMuxer_delete(muxer);
    }
    if (out_fd >= 0) {
        close(out_fd);
    }
    if (dec_ctx) avcodec_free_context(&dec_ctx);
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
        // Mode 1: Frame-accurate hardware transcode (AMediaCodec + AMediaMuxer)
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
