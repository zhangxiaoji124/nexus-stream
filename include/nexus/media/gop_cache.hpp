#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "nexus/core/media_frame.hpp"

namespace nexus::media {

/**
 * @brief GOP (Group of Pictures) Cache for instant first-frame playback (首屏秒开优化).
 *        Caches codec sequence headers (SPS/PPS & AudioSpecificConfig) plus all frames
 *        from the most recent H.264 IDR KeyFrame up to the latest frame.
 */
class GopCache {
public:
    explicit GopCache(size_t max_gop_frames = 150) : max_gop_frames_(max_gop_frames) {}

    void SetVideoTrack(const core::VideoTrackInfo& info);
    void SetAudioTrack(const core::AudioTrackInfo& info);

    std::optional<core::VideoTrackInfo> GetVideoTrack() const;
    std::optional<core::AudioTrackInfo> GetAudioTrack() const;

    /**
     * @brief Appends a frame to the GOP cache. If `frame` is a video IDR KeyFrame,
     *        evicts the previous GOP and starts a fresh GOP.
     */
    void PushFrame(const core::MediaFrame& frame);

    /**
     * @brief Returns a snapshot of the current cached GOP starting with an IDR keyframe.
     */
    std::vector<core::MediaFrame> GetCachedGop() const;

    size_t CachedFrameCount() const;
    uint32_t GopCount() const;

private:
    const size_t max_gop_frames_;
    mutable std::mutex mutex_;
    std::optional<core::VideoTrackInfo> video_track_;
    std::optional<core::AudioTrackInfo> audio_track_;
    std::vector<core::MediaFrame> gop_frames_;
    uint32_t gop_count_ = 0;
};

}  // namespace nexus::media
