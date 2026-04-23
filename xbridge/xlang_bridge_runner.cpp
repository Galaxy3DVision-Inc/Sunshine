#include "xlang_bridge_runner.h"
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

namespace xlang_bridge_runner {

    static SunshineCallTable g_Table = {0};
    
    // To allow dynamic control, we spin up independent mail sessions
    // instead of relying on the global mail::man router.
    static std::mutex g_VidMutex;
    static safe::mail_t g_VideoMail = nullptr;
    static std::mutex g_AudMutex;
    static safe::mail_t g_AudioMail = nullptr;

    static std::shared_ptr<input::input_t> g_InputCtx = nullptr;

    bool StartVideo(const char* display, int width, int height, int fps, int bitrate) {
        BOOST_LOG(info) << "[XBridge] StartVideo: " << (display ? display : "default") 
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

        safe::mail_t mailSession;
        {
            std::lock_guard<std::mutex> lock(g_VidMutex);
            g_VideoMail = std::make_shared<safe::mail_raw_t>();
            mailSession = g_VideoMail;
        }

        std::thread([cfg, mailSession]() {
            platf::set_thread_name("xbridge::video");
            // capture() takes a mail object natively
            video::capture(mailSession, cfg, nullptr);
        }).detach();

        // NAL draining thread
        std::thread([mailSession]() {
            platf::set_thread_name("xbridge::video_drain");

            // MUST subscribe to video_packets IMMEDIATELY before sleeping
            // so that we don't miss the initial IDR containing SPS/PPS
            auto packets = mailSession->queue<video::packet_t>(mail::video_packets);

            // Give the encoder ~1 second to spin up and compile shaders
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            BOOST_LOG(info) << "[XBridge] Bootstrapping IDR request to spin up encoder...";
            auto ev_idr = mailSession->event<bool>(mail::idr);
            ev_idr->raise(true);
            while (auto packetOpt = packets->pop()) {
                if (!packetOpt) break;
                auto& packet = *packetOpt;
                if (g_Table.OnVideoFrame) {
                    g_Table.OnVideoFrame(packet.data(), (int)packet.data_size(), packet.is_idr(), packet.frame_index());
                }
            }
        }).detach();

        return true;
    }

    void StopVideo() {
        BOOST_LOG(info) << "[XBridge] StopVideo requested";
        std::lock_guard<std::mutex> lock(g_VidMutex);
        if (g_VideoMail) {
            if (auto q = g_VideoMail->queue<video::packet_t>(mail::video_packets)) {
                q->stop();
            }
            g_VideoMail.reset();
        }
    }

    bool StartAudio(const char* audioSink) {
        BOOST_LOG(info) << "[XBridge] StartAudio: " << (audioSink ? audioSink : "default");

        audio::config_t cfg{};
        cfg.packetDuration = 5;
        cfg.channels = 2;

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

        std::thread([cfg, mailSession]() {
            platf::set_thread_name("xbridge::audio");
            audio::capture(mailSession, cfg, nullptr);
        }).detach();

        std::thread([mailSession]() {
            platf::set_thread_name("xbridge::audio_drain");
            auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);
            while (auto packetOpt = packets->pop()) {
                if (!packetOpt) break;
                auto& packet = *packetOpt;
                if (g_Table.OnAudioPacket) {
                    auto& [channel_data, packet_data] = packet;
                    g_Table.OnAudioPacket(packet_data.begin(), (int)packet_data.size(), 0);
                }
            }
        }).detach();

        return true;
    }

    void StopAudio() {
        BOOST_LOG(info) << "[XBridge] StopAudio requested";
        std::lock_guard<std::mutex> lock(g_AudMutex);
        if (g_AudioMail) {
            if (auto q = mail::man->queue<audio::packet_t>(mail::audio_packets)) {
                q->stop();
            }
            if (auto ev = g_AudioMail->event<bool>(mail::shutdown)) { ev->raise(true); }
            g_AudioMail.reset();
        }
    }

    void StopProcessing() {
        BOOST_LOG(info) << "[XBridge] Stop requested";
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        shutdown_event->raise(true);
    }

    int InjectInput(const uint8_t* pEventData, int cbSize) {
        if (!g_InputCtx || cbSize == 0 || !pEventData) return -1;
        
        // Advanced input forwarding: the native binary payload from WebRTC 
        // DataChannel bypasses standard network streams.
        std::vector<uint8_t> data(pEventData, pEventData + cbSize);
        input::passthrough(g_InputCtx, std::move(data));
        
        return 0;
    }

#include <stdio.h>

    int Start(const char* bridge_dll_path, int lrpc_port) {
        printf("[XBridge] Loading plugin DLL: %s on port %d\n", bridge_dll_path, lrpc_port);
        fflush(stdout);
        BOOST_LOG(info) << "[XBridge] Loading plugin DLL: " << bridge_dll_path;

        if (!g_InputCtx) {
            g_InputCtx = input::alloc(mail::man);
        }

#ifdef _WIN32
        HMODULE hMod = LoadLibraryA(bridge_dll_path);
        if (!hMod) {
            DWORD last_err = GetLastError();
            printf("[XBridge] Failed to load DLL: %s. GetLastError=%lu\n", bridge_dll_path, last_err);
            fflush(stdout);
            BOOST_LOG(error) << "[XBridge] Failed to load DLL: " << bridge_dll_path << " err=" << last_err;
            return -1;
        }

        typedef int (*f_LoadBridge)(void*, const char*, int);
        f_LoadBridge loadFunc = (f_LoadBridge)GetProcAddress(hMod, "LoadBridge");
        if (!loadFunc) {
            printf("[XBridge] Failed to find LoadBridge in DLL\n");
            fflush(stdout);
            BOOST_LOG(error) << "[XBridge] Failed to find LoadBridge in DLL";
            return -1;
        }
#else
        return -1; // Fallback
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
        if (res == 0) {
            BOOST_LOG(info) << "[XBridge] Sunshine Bridge Loaded Successfully. Host yield active.";

            // Block the main thread until shutdown is triggered
            auto shutdown_event = mail::man->event<bool>(mail::shutdown);
            while (!shutdown_event->peek()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            BOOST_LOG(info) << "[XBridge] Shutdown triggered. Exiting host loop.";
        }
        return res;
    }
}
