// SPDX-License-Identifier: GPL-3.0-only
// macOS session runner for PS5 Remote Play: replaces Veyra's D3D12
// RemotePlaySessionSource. One thread owns the Chiaki backend (connect, controller,
// IDR, login PIN, automatic recovery as Veyra's StreamRecovery), one thread decodes
// video with FFmpeg (VideoToolbox when available) into QVideoFrames for the stage.
#pragma once
#include "veyra/remoteplay/ChiakiBackend.h"
#include <QPointer>
#include <QVideoFrame>
#include <QVideoSink>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace veyra::remoteplay {

class RemotePlaySession {
public:
    enum class DecodeMode { Automatic = 0, Software = 1, Hardware = 2 };  // same ids as the dialog
    struct Status {
        SessionState state = SessionState::Idle;
        bool started = false;      // a backend session is (or was) up
        bool streaming = false;    // a decoded frame reached the picture
        bool failed = false;       // gave up: message says why
        bool recovering = false;   // reconnecting after a drop
        bool hardwareDecode = false;
        int width = 0, height = 0;
        double receivedFps = 0, decodedFps = 0, videoMbps = 0;
        VideoProfile profile;
        std::string message;       // Veyra's Chinese source text (translated by i18n::text)
    };

    RemotePlaySession(NativeConnectRequest request, DecodeMode decode, QVideoSink* sink);
    ~RemotePlaySession();               // stops and joins
    RemotePlaySession(const RemotePlaySession&) = delete;
    RemotePlaySession& operator=(const RemotePlaySession&) = delete;

    // Offline test: plays an H.264 Annex-B elementary stream (with AUD NALs) through the
    // same inbox → decoder → video sink path as a console session. No network.
    static std::unique_ptr<RemotePlaySession> createFileTest(std::string annexBPath, QVideoSink* sink);

    void start();
    void stop();
    bool running() const { return thread_.joinable() && !finished_; }

    void setController(const ControllerState& state);   // any thread; latest wins
    ControllerFeedback takeFeedback();                    // any thread
    void submitLoginPin(std::string pin);                 // any thread
    std::optional<PcmBlock> tryAudio();                   // any thread
    Status status() const;

private:
    void ownerLoop(std::stop_token stop);
    void fileLoop(std::stop_token stop);
    void decodeLoop(std::stop_token stop);
    void publish(QVideoFrame frame);

    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::jthread thread_;
    std::atomic<bool> finished_{false};
};

} // namespace veyra::remoteplay
