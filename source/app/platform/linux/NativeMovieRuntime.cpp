#include "NativeMovieRuntime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace {
std::uint64_t Hash(const std::uint8_t* bytes, std::size_t size,
    std::uint64_t value = 1469598103934665603ull) {
    for (std::size_t i = 0; i < size; ++i) { value ^= bytes[i]; value *= 1099511628211ull; }
    return value;
}
struct FormatDeleter { void operator()(AVFormatContext* v) const { avformat_close_input(&v); } };
struct CodecDeleter { void operator()(AVCodecContext* v) const { avcodec_free_context(&v); } };
struct PacketDeleter { void operator()(AVPacket* v) const { av_packet_free(&v); } };
struct FrameDeleter { void operator()(AVFrame* v) const { av_frame_free(&v); } };

std::unique_ptr<AVCodecContext, CodecDeleter> OpenCodec(AVFormatContext* format, int stream,
    std::string& name, std::string& error) {
    const AVCodec* codec = avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
    if (!codec) { error = "movie codec is unavailable"; return {}; }
    std::unique_ptr<AVCodecContext, CodecDeleter> context(avcodec_alloc_context3(codec));
    if (!context || avcodec_parameters_to_context(context.get(), format->streams[stream]->codecpar) < 0 ||
        avcodec_open2(context.get(), codec, nullptr) < 0) {
        error = "movie codec initialization failed";
        return {};
    }
    name = codec->name ? codec->name : "unknown";
    return context;
}
}

bool NativeMovieRuntime_Decode(const char* gameDir, const char* file,
    NativeMovieClip& out, std::string& error) {
    if (!gameDir || !*gameDir || !file || !*file) { error = "movie path is invalid"; return false; }
    const auto path = std::filesystem::path(gameDir) / "movies" / file;
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0 || !raw) {
        error = "movie open failed"; return false;
    }
    std::unique_ptr<AVFormatContext, FormatDeleter> format(raw);
    if (avformat_find_stream_info(format.get(), nullptr) < 0) { error = "movie stream info failed"; return false; }
    const int videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (videoIndex < 0 || audioIndex < 0) { error = "movie requires video and audio streams"; return false; }
    NativeMovieClip candidate;
    candidate.Name = file;
    auto video = OpenCodec(format.get(), videoIndex, candidate.VideoCodec, error);
    auto audio = OpenCodec(format.get(), audioIndex, candidate.AudioCodec, error);
    if (!video || !audio || video->width <= 0 || video->height <= 0 || video->width > 4096 || video->height > 4096) {
        if (error.empty()) error = "movie video dimensions are invalid";
        return false;
    }
    candidate.Width = video->width; candidate.Height = video->height;
    if (format->duration > 0) {
        const auto ms = (format->duration + AV_TIME_BASE / 1000 - 1) / (AV_TIME_BASE / 1000);
        if (ms > std::numeric_limits<std::uint32_t>::max()) { error = "movie duration overflow"; return false; }
        candidate.DurationMs = std::uint32_t(ms);
    }
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) { error = "movie packet allocation failed"; return false; }
    bool gotVideo = false, gotAudio = false;
    while ((!gotVideo || !gotAudio) && av_read_frame(format.get(), packet.get()) >= 0) {
        AVCodecContext* codec = packet->stream_index == videoIndex ? video.get() :
            packet->stream_index == audioIndex ? audio.get() : nullptr;
        if (codec && avcodec_send_packet(codec, packet.get()) >= 0) {
            while (avcodec_receive_frame(codec, frame.get()) >= 0) {
                if (codec == video.get() && !gotVideo) {
                    std::vector<std::uint8_t> rgba(std::size_t(candidate.Width) * candidate.Height * 4);
                    auto* scaler = sws_getContext(candidate.Width, candidate.Height, video->pix_fmt,
                        candidate.Width, candidate.Height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!scaler) { error = "movie scaler unavailable"; return false; }
                    std::uint8_t* planes[]{rgba.data()}; int strides[]{candidate.Width * 4};
                    const auto rows = sws_scale(scaler, frame->data, frame->linesize, 0,
                        candidate.Height, planes, strides);
                    sws_freeContext(scaler);
                    if (rows != candidate.Height) { error = "movie frame conversion failed"; return false; }
                    candidate.VideoFrameHash = Hash(rgba.data(), rgba.size()); gotVideo = true;
                } else if (codec == audio.get() && !gotAudio) {
                    const auto bytes = av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));
                    const auto channels = frame->ch_layout.nb_channels;
                    if (bytes <= 0 || channels <= 0 || frame->nb_samples <= 0 || !frame->extended_data) {
                        error = "movie audio frame is invalid"; return false;
                    }
                    candidate.AudioRate = frame->sample_rate; candidate.AudioChannels = channels;
                    candidate.AudioSamples = frame->nb_samples;
                    const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format));
                    std::uint64_t hash = 1469598103934665603ull;
                    if (planar) for (int channel = 0; channel < channels; ++channel)
                        hash = Hash(frame->extended_data[channel], std::size_t(frame->nb_samples) * bytes, hash);
                    else hash = Hash(frame->extended_data[0],
                        std::size_t(frame->nb_samples) * channels * bytes, hash);
                    candidate.AudioFrameHash = hash; gotAudio = true;
                }
                av_frame_unref(frame.get());
            }
        }
        av_packet_unref(packet.get());
    }
    if (!gotVideo || !gotAudio || !candidate.DurationMs || !candidate.VideoFrameHash || !candidate.AudioFrameHash) {
        error = "movie did not decode both first frames"; return false;
    }
    out = std::move(candidate); error.clear(); return true;
}
