// SPDX-License-Identifier: GPL-3.0-only
// See RemotePlaySession.h. Recovery policy and user messages follow Veyra's
// RemotePlaySessionSource; decoding follows RemotePlaySource (FFmpeg), with
// VideoToolbox in place of D3D12VA.
#include "RemotePlaySession.h"
#include "veyra/Log.h"
#include "veyra/remoteplay/SessionInbox.h"
#include "veyra/remoteplay/StreamRecovery.h"

#include <QMetaObject>
#include <QVideoFrameFormat>

#include <chrono>
#include <cstring>
#include <deque>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace veyra::remoteplay {
namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
NativeConnectRequest clone(const NativeConnectRequest& r) {
    NativeConnectRequest out;
    out.host = r.host;
    out.consoleId = r.consoleId;
    out.video = r.video;
    out.viewOnly = r.viewOnly;
    out.credentials.accountId = r.credentials.accountId;
    out.credentials.registrationKey = r.credentials.registrationKey;
    out.credentials.sessionKey = r.credentials.sessionKey;
    return out;
}
std::string withCount(const char* pattern, unsigned n) {
    std::string s = pattern;
    const auto at = s.find("{}");
    if (at != std::string::npos) s.replace(at, 2, std::to_string(n));
    return s;
}

// ---------------------------------------------------------------------------
// FFmpeg decoder
// ---------------------------------------------------------------------------
class Decoder {
public:
    ~Decoder() { close(); }
    bool open(Codec codec, RemotePlaySession::DecodeMode mode) {
        close();
        const AVCodec* decoder = avcodec_find_decoder(codec == Codec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
        if (!decoder) { veyra::log::error("remoteplay-decode", "FFmpeg has no decoder for this codec"); return false; }
        ctx_ = avcodec_alloc_context3(decoder);
        if (!ctx_) return false;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
        // Slice threads add no frame latency; frame threads would.
        ctx_->thread_type = FF_THREAD_SLICE;
        ctx_->thread_count = 0;
        hardware_ = false;
#ifdef __APPLE__
        if (mode != RemotePlaySession::DecodeMode::Software && !hardwareFailed_) {
            AVBufferRef* device = nullptr;
            const int r = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
            if (r >= 0) {
                ctx_->hw_device_ctx = device;
                ctx_->get_format = [](AVCodecContext*, const AVPixelFormat* formats) {
                    for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p) if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
                    return formats[0];
                };
                hardware_ = true;
            } else {
                veyra::log::warn("remoteplay-decode", "VideoToolbox unavailable code=" + std::to_string(r) + "; software decode");
                if (mode == RemotePlaySession::DecodeMode::Hardware) { close(); return false; }
            }
        }
#else
        (void)mode;
#endif
        if (avcodec_open2(ctx_, decoder, nullptr) < 0) {
            const bool retry = hardware_;
            close();
            if (retry && mode == RemotePlaySession::DecodeMode::Automatic) { hardwareFailed_ = true; return open(codec, mode); }
            veyra::log::error("remoteplay-decode", "FFmpeg decoder open failed");
            return false;
        }
        frame_ = av_frame_alloc(); sw_ = av_frame_alloc(); packet_ = av_packet_alloc();
        codec_ = codec;
        veyra::log::info("remoteplay-decode", std::string(hardware_ ? "VideoToolbox" : "software") + " decoder opened " + decoder->name);
        return frame_ && sw_ && packet_;
    }
    void close() {
        if (ctx_) avcodec_free_context(&ctx_);
        if (frame_) av_frame_free(&frame_);
        if (sw_) av_frame_free(&sw_);
        if (packet_) av_packet_free(&packet_);
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    }
    bool ready() const { return ctx_ != nullptr; }
    bool hardware() const { return hardware_; }
    void markHardwareFailed() { hardwareFailed_ = true; }
    void flush() { if (ctx_) avcodec_flush_buffers(ctx_); }

    // Decodes one access unit; the newest output picture (if any) goes to `out`.
    // Returns false on a decode error (caller requests a keyframe).
    bool decode(const std::uint8_t* data, std::size_t size, const std::function<void(QVideoFrame)>& out) {
        if (!ctx_) return false;
        // Payload owner guarantees FFmpeg padding (PaddedBytes / combined buffer).
        packet_->data = const_cast<std::uint8_t*>(data);
        packet_->size = int(size);
        const int sent = avcodec_send_packet(ctx_, packet_);
        packet_->data = nullptr; packet_->size = 0;
        if (sent < 0 && sent != AVERROR(EAGAIN)) return false;
        bool any = false;
        for (;;) {
            const int r = avcodec_receive_frame(ctx_, frame_);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) return false;
            any = true;
            QVideoFrame converted = toQt(frame_);
            av_frame_unref(frame_);
            if (converted.isValid()) out(std::move(converted));
        }
        (void)any;
        return true;
    }

private:
    QVideoFrame toQt(AVFrame* in) {
        AVFrame* f = in;
        if (in->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            av_frame_unref(sw_);
            if (av_hwframe_transfer_data(sw_, in, 0) < 0) return {};
            sw_->color_range = in->color_range; sw_->colorspace = in->colorspace; sw_->color_trc = in->color_trc;
            f = sw_;
        }
        const int w = f->width, h = f->height;
        if (w <= 0 || h <= 0) return {};
        auto fmt = AVPixelFormat(f->format);
        QVideoFrameFormat::PixelFormat qt = QVideoFrameFormat::Format_Invalid;
        switch (fmt) {
        case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: qt = QVideoFrameFormat::Format_YUV420P; break;
        case AV_PIX_FMT_NV12: qt = QVideoFrameFormat::Format_NV12; break;
        case AV_PIX_FMT_P010LE: qt = QVideoFrameFormat::Format_P010; break;
        default: break;
        }
        AVFrame* src = f;
        if (qt == QVideoFrameFormat::Format_Invalid) {
            // 10-bit planar and anything else: convert. 10-bit goes to P010 to keep HDR depth.
            const bool deep = av_pix_fmt_desc_get(fmt) && av_pix_fmt_desc_get(fmt)->comp[0].depth > 8;
            const AVPixelFormat target = deep ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P;
            sws_ = sws_getCachedContext(sws_, w, h, fmt, w, h, target, SWS_POINT, nullptr, nullptr, nullptr);
            if (!sws_) return {};
            if (!conv_ || conv_->width != w || conv_->height != h || conv_->format != target) {
                if (conv_) av_frame_free(&conv_);
                conv_ = av_frame_alloc();
                conv_->width = w; conv_->height = h; conv_->format = target;
                if (av_frame_get_buffer(conv_, 32) < 0) { av_frame_free(&conv_); return {}; }
            }
            sws_scale(sws_, f->data, f->linesize, 0, h, conv_->data, conv_->linesize);
            conv_->color_range = f->color_range; conv_->colorspace = f->colorspace; conv_->color_trc = f->color_trc;
            src = conv_;
            fmt = target;
            qt = deep ? QVideoFrameFormat::Format_P010 : QVideoFrameFormat::Format_YUV420P;
        }
        QVideoFrameFormat format(QSize(w, h), qt);
        const bool full = src->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P;
        format.setColorRange(full ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
        if (src->colorspace == AVCOL_SPC_BT2020_NCL || src->colorspace == AVCOL_SPC_BT2020_CL) {
            format.setColorSpace(QVideoFrameFormat::ColorSpace_BT2020);
            format.setColorTransfer(src->color_trc == AVCOL_TRC_SMPTE2084 ? QVideoFrameFormat::ColorTransfer_ST2084 : QVideoFrameFormat::ColorTransfer_BT709);
        } else {
            format.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
            format.setColorTransfer(QVideoFrameFormat::ColorTransfer_BT709);
        }
        QVideoFrame frame(format);
        if (!frame.map(QVideoFrame::WriteOnly)) return {};
        const int planes = frame.planeCount();
        for (int p = 0; p < planes && p < 3; ++p) {
            int rows = p == 0 ? h : (h + 1) / 2;
            int bytes = 0;
            if (qt == QVideoFrameFormat::Format_YUV420P) bytes = p == 0 ? w : (w + 1) / 2;
            else if (qt == QVideoFrameFormat::Format_NV12) bytes = p == 0 ? w : ((w + 1) / 2) * 2;
            else bytes = p == 0 ? w * 2 : ((w + 1) / 2) * 4; // P010
            bytes = std::min({bytes, frame.bytesPerLine(p), std::abs(src->linesize[p])});
            uchar* dst = frame.bits(p);
            const int dstStride = frame.bytesPerLine(p);
            for (int y = 0; y < rows; ++y) std::memcpy(dst + y * dstStride, src->data[p] + y * src->linesize[p], size_t(bytes));
        }
        frame.unmap();
        return frame;
    }

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* sw_ = nullptr;
    AVFrame* conv_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    Codec codec_ = Codec::H264;
    bool hardware_ = false, hardwareFailed_ = false;
};
} // namespace

// ---------------------------------------------------------------------------

struct RemotePlaySession::Shared {
    NativeConnectRequest request;
    DecodeMode decode = DecodeMode::Automatic;
    QPointer<QVideoSink> sink;
    SessionInbox inbox;

    mutable std::mutex mutex;
    Status status;
    ControllerState controller;
    bool controllerDirty = false;
    ControllerFeedback feedback;
    std::string pin;
    bool pinPending = false;

    std::mutex frameMutex;
    QVideoFrame pendingFrame;
    bool framePosted = false;
    std::atomic<bool> firstFrame{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> hardwareActive{false};
    std::atomic<int> frameWidth{0}, frameHeight{0};
    std::string testFile;
};

RemotePlaySession::RemotePlaySession(NativeConnectRequest request, DecodeMode decode, QVideoSink* sink)
    : shared_(std::make_shared<Shared>()) {
    shared_->request = std::move(request);
    shared_->decode = decode;
    shared_->sink = sink;
    shared_->status.profile = shared_->request.video;
}

RemotePlaySession::~RemotePlaySession() { stop(); }

std::unique_ptr<RemotePlaySession> RemotePlaySession::createFileTest(std::string annexBPath, QVideoSink* sink) {
    NativeConnectRequest request;
    request.host = "127.0.0.1";
    auto session = std::make_unique<RemotePlaySession>(std::move(request), DecodeMode::Automatic, sink);
    session->shared_->testFile = std::move(annexBPath);
    return session;
}

void RemotePlaySession::fileLoop(std::stop_token stop) {
    auto& s = *shared_;
    std::jthread decodeThread([this](std::stop_token st) { decodeLoop(st); });
    FILE* f = std::fopen(s.testFile.c_str(), "rb");
    std::vector<std::uint8_t> data;
    if (f) { std::uint8_t buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n); std::fclose(f); }
    // Split at access unit delimiters (NAL type 9).
    std::vector<size_t> cuts;
    for (size_t i = 0; i + 4 < data.size(); ++i)
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 9) cuts.push_back(i > 0 && data[i - 1] == 0 ? i - 1 : i);
    cuts.push_back(data.size());
    auto token = s.inbox.begin(monotonic100ns());
    token.connected();
    veyra::log::info("remoteplay-test", "file units=" + std::to_string(cuts.size() - 1) + " bytes=" + std::to_string(data.size()));
    size_t sent = 0;
    for (size_t k = 0; !stop.stop_requested() && cuts.size() > 1; k = (k + 2 < cuts.size()) ? k + 1 : 0) {
        if (k == 0 && sent) veyra::log::info("remoteplay-test", "file loop accepted=" + std::to_string(sent) + " decoded=" + std::to_string(s.inbox.snapshot().decodedFrames));
        // Chiaki hands over SPS/PPS as a separate configuration sample: do the same.
        std::vector<std::uint8_t> config, picture;
        const size_t begin = cuts[k], end = cuts[k + 1];
        std::vector<size_t> starts;
        for (size_t i = begin; i + 3 < end; ++i)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) starts.push_back(i);
        for (size_t n = 0; n < starts.size(); ++n) {
            const size_t from = starts[n], to = n + 1 < starts.size() ? starts[n + 1] : end;
            const int type = data[from + 3] & 0x1f;
            if (type == 9) continue;
            auto& target = (type == 7 || type == 8) ? config : picture;
            target.insert(target.end(), {0, 0, 0, 1});
            target.insert(target.end(), data.begin() + from + 3, data.begin() + to);
        }
        auto push = [&](std::vector<std::uint8_t>& bytes, SampleKind kind) {
            while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
            if (bytes.empty()) return false;
            VideoSample sample;
            sample.generation = token.generation(); sample.kind = kind; sample.codec = Codec::H264;
            sample.payload = PaddedBytes::copy(bytes);
            sample.arrival100ns = monotonic100ns();
            sample.width = 1280; sample.height = 720;
            return token.video(std::move(sample));
        };
        push(config, SampleKind::CodecConfig);
        if (push(picture, SampleKind::AccessUnit)) ++sent;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    s.inbox.invalidate();
    s.inbox.finishStop();
    decodeThread.request_stop();
    decodeThread.join();
}

void RemotePlaySession::start() {
    if (thread_.joinable()) return;
    finished_ = false;
    {
        std::lock_guard lock(shared_->mutex);
        shared_->status.state = SessionState::Connecting;
        shared_->status.message = "正在连接 PS5 · " + shared_->request.host;
    }
    thread_ = std::jthread([this](std::stop_token stop) {
        if (shared_->testFile.empty()) ownerLoop(stop); else fileLoop(stop);
        finished_ = true;
    });
}

void RemotePlaySession::stop() {
    if (!thread_.joinable()) return;
    shared_->stopping = true;
    thread_.request_stop();
    thread_.join();
}

void RemotePlaySession::setController(const ControllerState& state) {
    std::lock_guard lock(shared_->mutex);
    if (!(shared_->controller == state)) { shared_->controller = state; shared_->controllerDirty = true; }
}

ControllerFeedback RemotePlaySession::takeFeedback() {
    std::lock_guard lock(shared_->mutex);
    auto out = std::move(shared_->feedback);
    shared_->feedback = {};
    return out;
}

void RemotePlaySession::submitLoginPin(std::string pin) {
    std::lock_guard lock(shared_->mutex);
    shared_->pin = std::move(pin);
    shared_->pinPending = true;
}

std::optional<PcmBlock> RemotePlaySession::tryAudio() { return shared_->inbox.tryAudio(); }

RemotePlaySession::Status RemotePlaySession::status() const {
    Status out;
    { std::lock_guard lock(shared_->mutex); out = shared_->status; }
    const auto s = shared_->inbox.snapshot();
    if (out.state != SessionState::Failed && !out.recovering) out.state = s.state;
    out.streaming = shared_->firstFrame.load() && s.state == SessionState::Streaming;
    out.receivedFps = s.receivedFps; out.decodedFps = s.decodedFps; out.videoMbps = s.videoMbps;
    out.hardwareDecode = shared_->hardwareActive.load();
    out.width = shared_->frameWidth.load(); out.height = shared_->frameHeight.load();
    return out;
}

void RemotePlaySession::publish(QVideoFrame frame) {
    auto shared = shared_;
    shared->frameWidth = frame.width();
    shared->frameHeight = frame.height();
    bool post = false;
    {
        std::lock_guard lock(shared->frameMutex);
        shared->pendingFrame = std::move(frame);   // latest wins: a late frame is dropped, never queued
        if (!shared->framePosted) { shared->framePosted = true; post = true; }
    }
    QVideoSink* sink = shared->sink.data();
    if (!post || !sink) return;
    QMetaObject::invokeMethod(sink, [shared] {
        QVideoFrame latest;
        { std::lock_guard lock(shared->frameMutex); latest = std::move(shared->pendingFrame); shared->pendingFrame = {}; shared->framePosted = false; }
        if (shared->sink && latest.isValid() && !shared->stopping) {
            shared->sink->setVideoFrame(latest);
            static int shown = 0;
            if (shown == 30 && qEnvironmentVariableIsSet("VEYRA_TEST_PS5_DUMP")) latest.toImage().save(qEnvironmentVariable("VEYRA_TEST_PS5_DUMP"));
            if (++shown % 3600 == 1) veyra::log::info("remoteplay", "frame shown " + std::to_string(shown) + " sink=" + std::to_string(shared->sink->videoSize().width()) + "x" + std::to_string(shared->sink->videoSize().height()) + " fmt=" + std::to_string(int(latest.pixelFormat())));
        }
    }, Qt::QueuedConnection);
}

// Video: one decoder per session generation; a reset/new generation reopens it.
void RemotePlaySession::decodeLoop(std::stop_token stop) {
    Decoder decoder;
    Generation generation = 0;
    std::vector<std::uint8_t> combined;
    while (!stop.stop_requested()) {
        auto queued = shared_->inbox.tryVideo(monotonic100ns());
        if (!queued) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        const auto& sample = queued->sample;
        if (sample.generation != generation || queued->resetDecoder || !decoder.ready()) {
            if (sample.generation != generation || !decoder.ready()) {
                if (!decoder.open(sample.codec, shared_->decode)) { shared_->inbox.decodeFailed(sample.generation); std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
                shared_->hardwareActive = decoder.hardware();
                shared_->inbox.decoderBackend(decoder.hardware(), false);
            } else {
                decoder.flush();
            }
            generation = sample.generation;
        }
        const std::uint8_t* data = sample.payload.data();
        std::size_t size = sample.payload.size();
        if (queued->configBefore) {
            const auto config = queued->configBefore->payload.bytes();
            combined.assign(config.begin(), config.end());
            combined.insert(combined.end(), sample.payload.bytes().begin(), sample.payload.bytes().end());
            const auto used = combined.size();
            combined.resize(used + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            data = combined.data(); size = used;
        }
        shared_->inbox.decodeStarted(monotonic100ns(), sample.arrival100ns);
        bool produced = false;
        const bool ok = decoder.decode(data, size, [&](QVideoFrame f) {
            produced = true;
            shared_->inbox.frameDecoded(monotonic100ns());
            if (!shared_->firstFrame.exchange(true)) veyra::log::info("remoteplay", "first PS5 frame decoded " + std::to_string(f.width()) + "x" + std::to_string(f.height()));
            publish(std::move(f));
        });
        shared_->inbox.decodeFinished(monotonic100ns());
        if (!ok) {
            veyra::log::warn("remoteplay-decode", "decode error; requesting keyframe");
            if (decoder.hardware() && shared_->decode == DecodeMode::Automatic) { decoder.markHardwareFailed(); decoder.close(); }
            shared_->inbox.decodeFailed(sample.generation);
            continue;
        }
        if (produced) shared_->inbox.decodedFrameReady(sample.generation);
    }
}

// Backend owner: start, feed controller/PIN/IDR, watch progress, recover like Veyra.
void RemotePlaySession::ownerLoop(std::stop_token stop) {
    auto& s = *shared_;
    auto setStatus = [&](auto&& fn) { std::lock_guard lock(s.mutex); fn(s.status); };
    std::jthread decodeThread([this](std::stop_token st) { decodeLoop(st); });
    StreamRecovery recovery;
    ChiakiBackend backend;
    bool failedFinal = false;
    for (;;) {
        if (stop.stop_requested()) break;
        auto token = s.inbox.begin(monotonic100ns());
        s.inbox.streamProfile(s.request.video);
        recovery.beginAttempt(nowMs());
        auto request = clone(s.request);
        const auto started = backend.start(request, token);
        veyra::log::info("remoteplay", std::string(started.operation) + " ok=" + (started.ok ? "1" : "0") + " code=" + std::to_string(started.code));
        bool retry = false, retryWasRpInUse = false;
        if (!started.ok) {
            const auto native = backend.snapshot();
            retryWasRpInUse = native.startupRetryAllowed;
            retry = recovery.poll(nowMs(), false, true, native.automaticRetryAllowed, native.startupRetryAllowed) == StreamRecovery::Action::Reconnect;
            if (!retry) {
                failedFinal = true;
                setStatus([&](Status& st) { st.failed = true; st.message = "PS5 串流连接失败，请检查主机及网络；已保存的配对无需重输。"; });
            }
        } else {
            setStatus([&](Status& st) { st.started = true; st.recovering = false; st.message.clear(); });
            uint64_t lastDecoded = 0;
            int64_t lastControllerSend = 0, lastPoll = 0;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                const int64_t now = nowMs();
                // Login PIN
                std::string pin; bool pinPending = false;
                { std::lock_guard lock(s.mutex); if (s.pinPending) { pin = std::move(s.pin); s.pinPending = false; pinPending = true; } }
                if (pinPending) {
                    const auto r = backend.submitLoginPin(pin);
                    std::fill(pin.begin(), pin.end(), '\0');
                    veyra::log::info("remoteplay", std::string("login pin submitted ok=") + (r.ok ? "1" : "0"));
                }
                // Controller: changes at once, otherwise a 33 ms heartbeat.
                if (!s.request.viewOnly) {
                    ControllerState state; bool dirty = false;
                    { std::lock_guard lock(s.mutex); dirty = s.controllerDirty; state = s.controller; s.controllerDirty = false; }
                    if (dirty || now - lastControllerSend >= 33) { backend.submitController(state); lastControllerSend = now; }
                    auto fb = backend.takeFeedback();
                    if (fb.rumble || fb.triggers || fb.led || fb.playerIndex || fb.motionReset || !fb.haptics.empty()) {
                        std::lock_guard lock(s.mutex); s.feedback.merge(std::move(fb));
                    }
                }
                if (s.inbox.takeIdrRequest(monotonic100ns())) {
                    const auto idr = backend.requestIdr();
                    if (!idr.ok) veyra::log::warn("remoteplay", idr.operation + " code=" + std::to_string(idr.code));
                }
                if (now - lastPoll < 100) continue;
                lastPoll = now;
                const auto snap = s.inbox.snapshot();
                const auto native = backend.snapshot();
                if (snap.decodedFrames > lastDecoded) { lastDecoded = snap.decodedFrames; recovery.frame(now); }
                const auto action = recovery.poll(now, snap.state == SessionState::LoginPinRequired, snap.state == SessionState::Failed,
                                                  native.automaticRetryAllowed, native.startupRetryAllowed);
                if (action == StreamRecovery::Action::Keyframe) {
                    setStatus([&](Status& st) { st.message = "画面中断，正在请求关键帧恢复…"; });
                    (void)backend.requestIdr();
                } else if (action == StreamRecovery::Action::Reconnect) {
                    retryWasRpInUse = native.startupRetryAllowed; retry = true; break;
                } else if (action == StreamRecovery::Action::Fail) {
                    failedFinal = true;
                    setStatus([&](Status& st) {
                        st.failed = true;
                        st.message = snap.state == SessionState::LoginPinRequired ? "等待 PS5 登录 PIN 超时，请重新连接。"
                                   : native.startupRetryAllowed ? "PS5 仍被串流会话占用，请结束其他串流或稍后重试；无需重新配对。"
                                   : !native.automaticRetryAllowed ? "PS5 已结束或拒绝串流，请检查主机状态后重新连接。"
                                   : "PS5 串流未恢复，请检查主机及网络后点击连接；无需重新配对。";
                    });
                    veyra::log::warn("remoteplay", "session failed quit=" + std::to_string(native.lastQuitReason) + " error=" + std::to_string(snap.errorCode));
                    break;
                } else if (snap.state == SessionState::Streaming) {
                    setStatus([&](Status& st) { if (!st.message.empty() && !st.failed) st.message.clear(); });
                }
            }
        }
        // Teardown: invalidate before stop/join, finishStop after.
        s.inbox.invalidate();
        const auto stopped = backend.stop();
        if (!stopped.ok) veyra::log::error("remoteplay", "backend stop failed " + stopped.operation);
        s.inbox.finishStop();
        if (stop.stop_requested() || failedFinal || !retry) break;
        const unsigned n = recovery.episodeRetries() ? recovery.episodeRetries() : 1;
        const unsigned backoff = retryWasRpInUse ? (10u << (n - 1)) : (1u << (n - 1));
        setStatus([&](Status& st) {
            st.recovering = true;
            st.message = retryWasRpInUse ? withCount("PS5 正在释放上一个串流会话，等待后自动重试（{}/3）…", n) : withCount("串流中断，正在重新连接（{}/3）…", n);
        });
        veyra::log::warn("remoteplay", "reconnecting in " + std::to_string(backoff) + "s attempt " + std::to_string(n));
        for (unsigned i = 0; i < backoff * 10 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    decodeThread.request_stop();
    decodeThread.join();
    setStatus([&](Status& st) { st.recovering = false; if (!st.failed && !stop.stop_requested()) st.failed = true; });
    std::lock_guard lock(s.mutex);
    s.status.state = s.status.failed && !stop.stop_requested() ? SessionState::Failed : SessionState::Idle;
}

} // namespace veyra::remoteplay
