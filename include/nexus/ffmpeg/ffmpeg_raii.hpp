#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "nexus/core/media_frame.hpp"

#if defined(NEXUS_WITH_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace nexus::ffmpeg {

constexpr bool HasFFmpegSupport() noexcept {
#if defined(NEXUS_WITH_FFMPEG)
    return true;
#else
    return false;
#endif
}

#if defined(NEXUS_WITH_FFMPEG)

/**
 * @brief Modern C++20 RAII custom deleters for FFmpeg C structures.
 *        Eliminates manual av_free / av_packet_free / avformat_close_input leaks.
 */
struct AVFormatInputDeleter {
    void operator()(AVFormatContext* ctx) const noexcept {
        if (ctx) avformat_close_input(&ctx);
    }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const noexcept {
        if (ctx) avcodec_free_context(&ctx);
    }
};

struct AVPacketDeleter {
    void operator()(AVPacket* pkt) const noexcept {
        if (pkt) av_packet_free(&pkt);
    }
};

struct AVFrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        if (frame) av_frame_free(&frame);
    }
};

struct AVBSFContextDeleter {
    void operator()(AVBSFContext* bsf) const noexcept {
        if (bsf) av_bsf_free(&bsf);
    }
};

using UniqueAVFormatInput = std::unique_ptr<AVFormatContext, AVFormatInputDeleter>;
using UniqueAVCodecContext = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using UniqueAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;
using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;
using UniqueAVBSFContext = std::unique_ptr<AVBSFContext, AVBSFContextDeleter>;

inline UniqueAVPacket MakeUniquePacket() {
    return UniqueAVPacket(av_packet_alloc());
}

inline UniqueAVFrame MakeUniqueFrame() {
    return UniqueAVFrame(av_frame_alloc());
}

/**
 * @brief RAII FFmpeg Media Demuxer that opens any local media file (MP4/MKV/FLV/TS)
 *        or network URL, applies h264_mp4toannexb bitstream filtering if needed,
 *        and converts AVPacket timestamps via av_rescale_q into millisecond MediaFrames.
 */
class FFmpegDemuxer {
public:
    bool Open(std::string_view url) {
        AVFormatContext* raw_ctx = nullptr;
        std::string url_str(url);
        if (avformat_open_input(&raw_ctx, url_str.c_str(), nullptr, nullptr) < 0) {
            return false;
        }
        fmt_ctx_.reset(raw_ctx);

        if (avformat_find_stream_info(fmt_ctx_.get(), nullptr) < 0) {
            return false;
        }

        video_stream_idx_ = av_find_best_stream(
            fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audio_stream_idx_ = av_find_best_stream(
            fmt_ctx_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        if (video_stream_idx_ >= 0) {
            AVStream* vs = fmt_ctx_->streams[video_stream_idx_];
            if (vs->codecpar->codec_id == AV_CODEC_ID_H264) {
                const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
                if (filter) {
                    AVBSFContext* raw_bsf = nullptr;
                    if (av_bsf_alloc(filter, &raw_bsf) == 0) {
                        avcodec_parameters_copy(raw_bsf->par_in, vs->codecpar);
                        av_bsf_init(raw_bsf);
                        h264_bsf_.reset(raw_bsf);
                    }
                }
            }
        }
        return (video_stream_idx_ >= 0 || audio_stream_idx_ >= 0);
    }

    bool ReadNextFrames(const std::function<void(const core::MediaFrame&)>& on_frame) {
        if (!fmt_ctx_) return false;
        auto pkt = MakeUniquePacket();
        if (av_read_frame(fmt_ctx_.get(), pkt.get()) < 0) {
            return false;
        }

        AVRational ms_tb{1, 1000};
        if (pkt->stream_index == video_stream_idx_) {
            AVStream* st = fmt_ctx_->streams[video_stream_idx_];
            if (h264_bsf_) {
                if (av_bsf_send_packet(h264_bsf_.get(), pkt.get()) == 0) {
                    while (av_bsf_receive_packet(h264_bsf_.get(), pkt.get()) == 0) {
                        uint64_t pts = (pkt->pts != AV_NOPTS_VALUE)
                                           ? static_cast<uint64_t>(av_rescale_q(pkt->pts, st->time_base, ms_tb))
                                           : 0;
                        uint64_t dts = (pkt->dts != AV_NOPTS_VALUE)
                                           ? static_cast<uint64_t>(av_rescale_q(pkt->dts, st->time_base, ms_tb))
                                           : pts;
                        bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                        on_frame(core::MediaFrame::Create(
                            core::CodecType::kH264,
                            is_key ? core::FrameType::kKeyFrame : core::FrameType::kInterFrame,
                            pts, dts,
                            std::span<const uint8_t>(pkt->data, pkt->size)));
                        av_packet_unref(pkt.get());
                    }
                }
            }
        } else if (pkt->stream_index == audio_stream_idx_) {
            AVStream* st = fmt_ctx_->streams[audio_stream_idx_];
            uint64_t pts = (pkt->pts != AV_NOPTS_VALUE)
                               ? static_cast<uint64_t>(av_rescale_q(pkt->pts, st->time_base, ms_tb))
                               : 0;
            on_frame(core::MediaFrame::Create(
                core::CodecType::kAAC,
                core::FrameType::kAudioFrame,
                pts, pts,
                std::span<const uint8_t>(pkt->data, pkt->size),
                st->codecpar->sample_rate > 0 ? st->codecpar->sample_rate : 44100));
        }
        return true;
    }

private:
    UniqueAVFormatInput fmt_ctx_;
    UniqueAVBSFContext h264_bsf_;
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;
};

#endif  // defined(NEXUS_WITH_FFMPEG)

}  // namespace nexus::ffmpeg
