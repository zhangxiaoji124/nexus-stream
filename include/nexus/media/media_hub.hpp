#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nexus/core/media_frame.hpp"
#include "nexus/media/gop_cache.hpp"

namespace nexus::media {

struct StreamStats {
    std::string stream_path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t audio_sample_rate = 0;
    uint8_t audio_channels = 0;
    uint64_t video_frames = 0;
    uint64_t audio_frames = 0;
    uint64_t total_bytes = 0;
    size_t gop_cache_frames = 0;
    uint32_t gop_count = 0;
    size_t subscribers = 0;
};

/**
 * @brief Represents a single live media stream (e.g., "/live/stream1") with a
 *        built-in GOP cache and pub/sub fan-out to RTSP and HTTP-FLV viewers.
 */
class MediaSession {
public:
    using SubscriberId = uint64_t;
    // Return false from callback to auto-unsubscribe (e.g., when client socket closes)
    using FrameCallback = std::function<bool(const core::MediaFrame&)>;

    explicit MediaSession(std::string stream_path);

    const std::string& StreamPath() const noexcept { return stream_path_; }

    void SetVideoTrack(const core::VideoTrackInfo& info);
    void SetAudioTrack(const core::AudioTrackInfo& info);

    std::optional<core::VideoTrackInfo> GetVideoTrack() const { return gop_cache_.GetVideoTrack(); }
    std::optional<core::AudioTrackInfo> GetAudioTrack() const { return gop_cache_.GetAudioTrack(); }

    /**
     * @brief Publishes a MediaFrame into the session, updating the GOP cache
     *        and broadcasting to all active subscribers.
     */
    void PublishFrame(const core::MediaFrame& frame);

    /**
     * @brief Helper that classifies a raw H.264 NALU (SPS/PPS/IDR/P), updates track metadata
     *        if SPS/PPS, and publishes video slices as MediaFrames.
     */
    void PublishH264Nalu(std::span<const uint8_t> raw_nalu, uint64_t pts_ms, uint64_t dts_ms);

    /**
     * @brief Helper that publishes a raw AAC frame (without ADTS header).
     */
    void PublishRawAac(std::span<const uint8_t> raw_aac, uint64_t pts_ms);

    /**
     * @brief Subscribes to live frames. If `replay_gop_cache` is true, immediately invokes
     *        `cb` on the cached GOP frames (instant first-frame playback).
     */
    SubscriberId Subscribe(FrameCallback cb, bool replay_gop_cache = true);
    void Unsubscribe(SubscriberId id);

    StreamStats GetStats() const;

private:
    std::string stream_path_;
    GopCache gop_cache_;

    std::atomic<uint64_t> video_frames_{0};
    std::atomic<uint64_t> audio_frames_{0};
    std::atomic<uint64_t> total_bytes_{0};
    std::atomic<uint32_t> frame_seq_{0};
    std::atomic<SubscriberId> next_sub_id_{1};

    mutable std::mutex sub_mutex_;
    std::unordered_map<SubscriberId, FrameCallback> subscribers_;

    mutable std::mutex sps_pps_mutex_;
    std::vector<uint8_t> cached_sps_;
    std::vector<uint8_t> cached_pps_;
};

using MediaSessionPtr = std::shared_ptr<MediaSession>;

/**
 * @brief Thread-safe central registry of all active MediaSessions.
 */
class MediaHub {
public:
    MediaSessionPtr GetOrCreateSession(std::string_view stream_path);
    MediaSessionPtr FindSession(std::string_view stream_path) const;
    std::vector<StreamStats> GetAllStats() const;

private:
    static std::string NormalizePath(std::string_view path);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, MediaSessionPtr> sessions_;
};

}  // namespace nexus::media
