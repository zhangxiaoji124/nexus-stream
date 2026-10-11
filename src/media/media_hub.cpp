#include "nexus/media/media_hub.hpp"
#include "nexus/codec/h264_parser.hpp"

namespace nexus::media {

MediaSession::MediaSession(std::string stream_path)
    : stream_path_(std::move(stream_path)) {}

void MediaSession::SetVideoTrack(const core::VideoTrackInfo& info) {
    {
        std::lock_guard<std::mutex> lock(sps_pps_mutex_);
        if (!info.sps.empty()) cached_sps_ = info.sps;
        if (!info.pps.empty()) cached_pps_ = info.pps;
    }
    gop_cache_.SetVideoTrack(info);
}

void MediaSession::SetAudioTrack(const core::AudioTrackInfo& info) {
    gop_cache_.SetAudioTrack(info);
}

void MediaSession::PublishFrame(const core::MediaFrame& frame) {
    if (frame.IsVideo()) {
        video_frames_.fetch_add(1, std::memory_order_relaxed);
    } else if (frame.IsAudio()) {
        audio_frames_.fetch_add(1, std::memory_order_relaxed);
    }
    total_bytes_.fetch_add(frame.Size(), std::memory_order_relaxed);

    gop_cache_.PushFrame(frame);

    std::vector<std::pair<SubscriberId, FrameCallback>> subs_snapshot;
    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        subs_snapshot.reserve(subscribers_.size());
        for (const auto& kv : subscribers_) {
            subs_snapshot.push_back(kv);
        }
    }

    std::vector<SubscriberId> dead_subs;
    for (const auto& [sub_id, cb] : subs_snapshot) {
        if (!cb || !cb(frame)) {
            dead_subs.push_back(sub_id);
        }
    }

    if (!dead_subs.empty()) {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        for (SubscriberId id : dead_subs) {
            subscribers_.erase(id);
        }
    }
}

void MediaSession::PublishH264Nalu(std::span<const uint8_t> raw_nalu,
                                   uint64_t pts_ms,
                                   uint64_t dts_ms) {
    if (raw_nalu.empty()) return;
    auto type = codec::H264Parser::GetNaluType(raw_nalu[0]);

    if (type == codec::H264NaluType::kSps) {
        core::VideoTrackInfo vinfo;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(sps_pps_mutex_);
            cached_sps_.assign(raw_nalu.begin(), raw_nalu.end());
            vinfo.sps = cached_sps_;
            vinfo.pps = cached_pps_;
            ready = !cached_sps_.empty();
        }
        if (ready) {
            auto sps_meta = codec::H264Parser::ParseSps(raw_nalu);
            if (sps_meta) {
                vinfo.width = sps_meta->width;
                vinfo.height = sps_meta->height;
                if (sps_meta->fps > 0) vinfo.fps = sps_meta->fps;
                vinfo.profile_idc = sps_meta->profile_idc;
                vinfo.level_idc = sps_meta->level_idc;
            }
            gop_cache_.SetVideoTrack(vinfo);
        }
        return;
    }

    if (type == codec::H264NaluType::kPps) {
        core::VideoTrackInfo vinfo;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(sps_pps_mutex_);
            cached_pps_.assign(raw_nalu.begin(), raw_nalu.end());
            vinfo.sps = cached_sps_;
            vinfo.pps = cached_pps_;
            ready = !cached_sps_.empty() && !cached_pps_.empty();
        }
        if (ready) {
            auto sps_meta = codec::H264Parser::ParseSps(vinfo.sps);
            if (sps_meta) {
                vinfo.width = sps_meta->width;
                vinfo.height = sps_meta->height;
                if (sps_meta->fps > 0) vinfo.fps = sps_meta->fps;
                vinfo.profile_idc = sps_meta->profile_idc;
                vinfo.level_idc = sps_meta->level_idc;
            }
            gop_cache_.SetVideoTrack(vinfo);
        }
        return;
    }

    if (type == codec::H264NaluType::kAud || type == codec::H264NaluType::kSei) {
        return;
    }

    bool is_idr = (type == codec::H264NaluType::kIdrSlice);
    auto frame = core::MediaFrame::Create(
        core::CodecType::kH264,
        is_idr ? core::FrameType::kKeyFrame : core::FrameType::kInterFrame,
        pts_ms,
        dts_ms,
        raw_nalu,
        90000);
    frame.sequence_id = frame_seq_.fetch_add(1, std::memory_order_relaxed);
    PublishFrame(frame);
}

void MediaSession::PublishRawAac(std::span<const uint8_t> raw_aac, uint64_t pts_ms) {
    if (raw_aac.empty()) return;
    uint32_t sample_rate = 44100;
    if (auto atrack = gop_cache_.GetAudioTrack()) {
        sample_rate = atrack->sample_rate;
    }
    auto frame = core::MediaFrame::Create(
        core::CodecType::kAAC,
        core::FrameType::kAudioFrame,
        pts_ms,
        pts_ms,
        raw_aac,
        sample_rate);
    frame.sequence_id = frame_seq_.fetch_add(1, std::memory_order_relaxed);
    PublishFrame(frame);
}

MediaSession::SubscriberId MediaSession::Subscribe(FrameCallback cb, bool replay_gop_cache) {
    SubscriberId id = next_sub_id_.fetch_add(1, std::memory_order_relaxed);
    if (replay_gop_cache) {
        auto gop = gop_cache_.GetCachedGop();
        for (const auto& frame : gop) {
            if (!cb(frame)) {
                return 0;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        subscribers_[id] = std::move(cb);
    }
    return id;
}

void MediaSession::Unsubscribe(SubscriberId id) {
    std::lock_guard<std::mutex> lock(sub_mutex_);
    subscribers_.erase(id);
}

StreamStats MediaSession::GetStats() const {
    StreamStats st;
    st.stream_path = stream_path_;
    if (auto vt = gop_cache_.GetVideoTrack()) {
        st.width = vt->width;
        st.height = vt->height;
        st.fps = vt->fps;
    }
    if (auto at = gop_cache_.GetAudioTrack()) {
        st.audio_sample_rate = at->sample_rate;
        st.audio_channels = at->channels;
    }
    st.video_frames = video_frames_.load(std::memory_order_relaxed);
    st.audio_frames = audio_frames_.load(std::memory_order_relaxed);
    st.total_bytes = total_bytes_.load(std::memory_order_relaxed);
    st.gop_cache_frames = gop_cache_.CachedFrameCount();
    st.gop_count = gop_cache_.GopCount();
    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        st.subscribers = subscribers_.size();
    }
    return st;
}

std::string MediaHub::NormalizePath(std::string_view path) {
    std::string p(path);
    if (p.empty() || p[0] != '/') {
        p = "/" + p;
    }
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    return p;
}

MediaSessionPtr MediaHub::GetOrCreateSession(std::string_view stream_path) {
    std::string norm = NormalizePath(stream_path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(norm);
    if (it != sessions_.end()) {
        return it->second;
    }
    auto session = std::make_shared<MediaSession>(norm);
    sessions_[norm] = session;
    return session;
}

MediaSessionPtr MediaHub::FindSession(std::string_view stream_path) const {
    std::string norm = NormalizePath(stream_path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(norm);
    return (it != sessions_.end()) ? it->second : nullptr;
}

std::vector<StreamStats> MediaHub::GetAllStats() const {
    std::vector<MediaSessionPtr> list;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, v] : sessions_) {
            list.push_back(v);
        }
    }
    std::vector<StreamStats> stats;
    stats.reserve(list.size());
    for (const auto& s : list) {
        stats.push_back(s->GetStats());
    }
    return stats;
}

}  // namespace nexus::media
