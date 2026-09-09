#include "sunbridge_runner.h"
#include "Bridge.h"

// core sunshine logic
#include "../src/video.h"
#include "../src/audio.h"
#include "../src/config.h"
#include "../src/globals.h"
#include "../src/logging.h"
#include "../src/thread_safe.h"
#include "../src/input.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <thread>

namespace sunbridge_runner {

    static SunshineCallTable g_Table = {0};
    
    // To allow dynamic control, we spin up independent mail sessions
    // instead of relying on the global mail::man router.
    static std::mutex g_VidMutex;
    static safe::mail_t g_VideoMail = nullptr;
    static std::mutex g_VideoLifecycle;
    static std::thread g_VideoCapture;
    static std::thread g_VideoDrain;
    static std::mutex g_AudMutex;
    static safe::mail_t g_AudioMail = nullptr;
    static std::mutex g_AudioLifecycle;
    static std::thread g_AudioCapture;
    static std::thread g_AudioDrain;

    static std::shared_ptr<input::input_t> g_InputCtx = nullptr;
    static std::mutex g_InputMutex;

    void StopVideoLocked() {
        safe::mail_t session;
        {
            std::lock_guard<std::mutex> lock(g_VidMutex);
            session = std::move(g_VideoMail);
        }
        if (session) session->event<bool>(mail::shutdown)->raise(true);
        if (g_VideoCapture.joinable()) g_VideoCapture.join();
        if (session) session->queue<video::packet_t>(mail::video_packets)->stop();
        if (g_VideoDrain.joinable()) g_VideoDrain.join();
    }

    bool StartVideo(const char* display, int width, int height, int fps, int bitrate) {
        std::lock_guard<std::mutex> lifecycle(g_VideoLifecycle);
        StopVideoLocked();
        BOOST_LOG(info) << "[SunbridgeRunner] StartVideo: " << (display ? display : "default") 
                        << " " << width << "x" << height << "@" << fps << " fps " << bitrate << "kbps";

        video::config_t cfg{};
        cfg.width = width;
        cfg.height = height;
        cfg.framerate = fps;
        cfg.framerateX100 = fps * 100;
        cfg.bitrate = bitrate;
        cfg.slicesPerFrame = 1;
        cfg.numRefFrames = 1;

        if (display && display[0] != '\0') {
            config::video.output_name = display;
        }

        // Capture and drain share one owned session. Stopping it does not shut
        // down Sunshine's process-global mail bus, so a stream can restart.
        auto session = std::make_shared<safe::mail_raw_t>();
        {
            std::lock_guard<std::mutex> lock(g_VidMutex);
            g_VideoMail = session;
        }
        // Absolute input depends on the touch-port geometry published by
        // video::capture(). Keep the input context on this same per-stream
        // mail session so browser coordinates can be mapped to the display.
        {
            std::lock_guard<std::mutex> lock(g_InputMutex);
            g_InputCtx = input::alloc(session);
        }

        // Hold the consumer queue before capture starts. The mail registry uses
        // weak references, so this also guarantees the encoder and drain thread
        // share the exact same queue from the first IDR onward.
        auto packets = session->queue<video::packet_t>(mail::video_packets);
        auto shutdown = session->event<bool>(mail::shutdown);

        g_VideoCapture = std::thread([cfg, session, shutdown]() {
            platf::set_thread_name("sunbridge::video");
            video::capture(session, cfg, nullptr);
            BOOST_LOG(warning) << "[SunbridgeRunner] Video capture worker exited";
        });

        // NAL draining thread
        g_VideoDrain = std::thread([packets]() {
            platf::set_thread_name("sunbridge::video_drain");

            // Drain from the first encoded packet. Delaying this consumer used
            // to accumulate roughly one second of video and then burst it into
            // Sunbridge, which forced dependency-chain drops and extra IDRs.
            // video::capture() already requests an IDR before encoding starts.
            bool reportedFirstFrame = false;
            while (auto packetOpt = packets->pop()) {
                if (!packetOpt) break;
                auto& packet = *packetOpt;
                if (!reportedFirstFrame) {
                    BOOST_LOG(info) << "[SunbridgeRunner] First encoded frame: bytes="
                                    << packet.data_size() << " idr=" << packet.is_idr()
                                    << " index=" << packet.frame_index();
                    reportedFirstFrame = true;
                }
                if (g_Table.OnVideoFrame) {
                    g_Table.OnVideoFrame(packet.data(), (int)packet.data_size(), packet.is_idr(), packet.frame_index());
                }
            }
        });

        return true;
    }

    void StopVideo() {
        BOOST_LOG(info) << "[SunbridgeRunner] StopVideo requested";
        std::lock_guard<std::mutex> lifecycle(g_VideoLifecycle);
        StopVideoLocked();
    }

    void StopAudioLocked() {
        if (g_AudioMail) g_AudioMail->event<bool>(mail::shutdown)->raise(true);
        if (g_AudioCapture.joinable()) g_AudioCapture.join();
        if (g_AudioDrain.joinable()) {
            mail::man->queue<audio::packet_t>(mail::audio_packets)->stop();
            g_AudioDrain.join();
        }
        g_AudioMail.reset();
    }

    bool StartAudio(const char* audioSink) {
        std::lock_guard<std::mutex> lifecycle(g_AudioLifecycle);
        StopAudioLocked();
        BOOST_LOG(info) << "[SunbridgeRunner] StartAudio: " << (audioSink ? audioSink : "default");

        audio::config_t cfg{};
        cfg.packetDuration = 5;
        cfg.channels = 2;
        cfg.flags[audio::config_t::HOST_AUDIO] = true;

        if (audioSink && audioSink[0] != '\0') {
            config::audio.sink = audioSink;
        }
        config::audio.stream = true; // Ensure Sunshine actually allows audio capture

        safe::mail_t mailSession;
        {
            std::lock_guard<std::mutex> lock(g_AudMutex);
            g_AudioMail = std::make_shared<safe::mail_raw_t>();
            mailSession = g_AudioMail;
        }

        auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);
        auto shutdown = mailSession->event<bool>(mail::shutdown);
        g_AudioCapture = std::thread([cfg, mailSession, shutdown]() {
            platf::set_thread_name("sunbridge::audio");
            audio::capture(mailSession, cfg, nullptr);
        });

        g_AudioDrain = std::thread([mailSession, packets]() {
            platf::set_thread_name("sunbridge::audio_drain");
            while (auto packetOpt = packets->pop()) {
                if (!packetOpt) break;
                auto& packet = *packetOpt;
                if (g_Table.OnAudioPacket) {
                    auto& [channel_data, packet_data] = packet;
                    g_Table.OnAudioPacket(packet_data.begin(), (int)packet_data.size(), 0);
                }
            }
        });

        return true;
    }

    void StopAudio() {
        BOOST_LOG(info) << "[SunbridgeRunner] StopAudio requested";
        std::lock_guard<std::mutex> lifecycle(g_AudioLifecycle);
        StopAudioLocked();
    }

    void StopProcessing() {
        BOOST_LOG(info) << "[SunbridgeRunner] Stop requested";
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        shutdown_event->raise(true);
    }

    int InjectInput(const uint8_t* pEventData, int cbSize) {
        if (cbSize == 0 || !pEventData) return -1;

        std::shared_ptr<input::input_t> inputCtx;
        {
            std::lock_guard<std::mutex> lock(g_InputMutex);
            inputCtx = g_InputCtx;
        }
        if (!inputCtx) return -1;
        
        // Advanced input forwarding: the native binary payload from WebRTC 
        // DataChannel bypasses standard network streams.
        std::vector<uint8_t> data(pEventData, pEventData + cbSize);
        input::passthrough(inputCtx, std::move(data));
        
        return 0;
    }

#include <stdio.h>

    int Start(const char* bridge_dll_path, int lrpc_port) {
        printf("[SunbridgeRunner] Loading plugin DLL: %s on port %d\n", bridge_dll_path, lrpc_port);
        fflush(stdout);
        BOOST_LOG(info) << "[SunbridgeRunner] Loading plugin DLL: " << bridge_dll_path;

        typedef int (*f_LoadBridge)(void*, const char*, int);
        typedef void (*f_UnloadBridge)();
#ifdef _WIN32
        HMODULE hMod = LoadLibraryA(bridge_dll_path);
        if (!hMod) {
            DWORD last_err = GetLastError();
            printf("[SunbridgeRunner] Failed to load DLL: %s. GetLastError=%lu\n", bridge_dll_path, last_err);
            fflush(stdout);
            BOOST_LOG(error) << "[SunbridgeRunner] Failed to load DLL: " << bridge_dll_path << " err=" << last_err;
            return -1;
        }

        f_LoadBridge loadFunc = (f_LoadBridge)GetProcAddress(hMod, "LoadBridge");
        f_UnloadBridge unloadFunc = (f_UnloadBridge)GetProcAddress(hMod, "UnloadBridge");
        if (!loadFunc || !unloadFunc) {
            printf("[SunbridgeRunner] Failed to find LoadBridge in DLL\n");
            fflush(stdout);
            BOOST_LOG(error) << "[SunbridgeRunner] Failed to find LoadBridge in DLL";
            FreeLibrary(hMod);
            return -1;
        }
#else
        void* hMod = dlopen(bridge_dll_path, RTLD_NOW | RTLD_LOCAL);
        if (!hMod) {
            const char* dlError = dlerror();
            BOOST_LOG(error) << "[SunbridgeRunner] Failed to load bridge: "
                             << bridge_dll_path << " err="
                             << (dlError ? dlError : "unknown");
            return -1;
        }
        dlerror();
        f_LoadBridge loadFunc = reinterpret_cast<f_LoadBridge>(dlsym(hMod, "LoadBridge"));
        f_UnloadBridge unloadFunc = reinterpret_cast<f_UnloadBridge>(dlsym(hMod, "UnloadBridge"));
        if (const char* dlError = dlerror(); dlError || !loadFunc || !unloadFunc) {
            BOOST_LOG(error) << "[SunbridgeRunner] Failed to find LoadBridge in bridge: "
                             << (dlError ? dlError : "unknown");
            dlclose(hMod);
            return -1;
        }
#endif

        g_Table.StartVideo = StartVideo;
        g_Table.StopVideo = StopVideo;
        g_Table.StartAudio = StartAudio;
        g_Table.StopAudio = StopAudio;
        g_Table.StopProcessing = StopProcessing;
        g_Table.InjectInput = InjectInput;
        g_Table.RequestIdr = []() {
            std::lock_guard<std::mutex> lock(g_VidMutex);
            if (g_VideoMail) {
                g_VideoMail->event<bool>(mail::idr)->raise(true);
            } else {
                mail::man->event<bool>(mail::idr)->raise(true); // fallback
            }
        };

        int res = loadFunc(&g_Table, bridge_dll_path, lrpc_port);
        // LoadBridge owns the connection loop. It returns on manager loss;
        // stop producers before unloading their callback code.
        StopAudio();
        StopVideo();
        unloadFunc();
        g_InputCtx.reset();
#ifdef _WIN32
        FreeLibrary(hMod);
#else
        dlclose(hMod);
#endif
        return res;
    }
}
