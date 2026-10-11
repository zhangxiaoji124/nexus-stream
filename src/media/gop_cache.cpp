#include "nexus/media/gop_cache.hpp"

namespace nexus::media {

void GopCache::SetVideoTrack(const core::VideoTrackInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    video_track_ = info;
}

void GopCache::SetAudioTrack(const core::AudioTrackInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_track_ = info;
}

std::optional<core::VideoTrackInfo> GopCache::GetVideoTrack() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return video_track_;
}

std::optional<core::AudioTrackInfo> GopCache::GetAudioTrack() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audio_track_;
}

void GopCache::PushFrame(const core::MediaFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame.IsVideo() && frame.IsKeyFrame()) {
        gop_frames_.clear();
        ++gop_count_;
    }

    // Only cache frames once we have seen at least one video keyframe
    if (frame.IsVideo() && frame.IsKeyFrame()) {
        gop_frames_.push_back(frame);
    } else if (!gop_frames_.empty()) {
        if (gop_frames_.size() < max_gop_frames_) {
            gop_frames_.push_back(frame);
        }
    }
}

std::vector<core::MediaFrame> GopCache::GetCachedGop() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gop_frames_;
}

size_t GopCache::CachedFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gop_frames_.size();
}

uint32_t GopCache::GopCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gop_count_;
}

}  // namespace nexus::media
