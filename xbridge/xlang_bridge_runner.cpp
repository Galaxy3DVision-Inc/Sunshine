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
    
    // To maintain a session context, usually Sunshine spins up a stream session
    // We will use the global mail manager since Sunshine hardcodes frame dispatch to mail::man.

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

        std::thread([cfg]() {
            platf::set_thread_name("xbridge::video");
            // capture() takes a mail object, though it internally pumps to mail::man for some queues
            // We pass the global mail manager's interface.
            video::capture(mail::man, cfg, nullptr);
        }).detach();

        // NAL draining thread
        std::thread([]() {
            platf::set_thread_name("xbridge::video_drain");

            // MUST subscribe to video_packets IMMEDIATELY before sleeping
            // so that we don't miss the initial IDR containing SPS/PPS
            auto packets = mail::man->queue<video::packet_t>(mail::video_packets);

            // Give the encoder ~1 second to spin up and compile shaders
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            BOOST_LOG(info) << "[XBridge] Bootstrapping IDR request to spin up encoder...";
            auto ev_idr = mail::man->event<bool>(mail::idr);
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

    bool StartAudio(const char* audioSink) {
        BOOST_LOG(info) << "[XBridge] StartAudio: " << (audioSink ? audioSink : "default");

        audio::config_t cfg{};
        cfg.packetDuration = 5;
        cfg.channels = 2;

        if (audioSink && audioSink[0] != '\0') {
            config::audio.sink = audioSink;
        }

        std::thread([cfg]() {
            platf::set_thread_name("xbridge::audio");
            audio::capture(mail::man, cfg, nullptr);
        }).detach();

        std::thread([]() {
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

    void StopProcessing() {
        BOOST_LOG(info) << "[XBridge] Stop requested";
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        shutdown_event->raise(true);
    }

    int InjectInput(const uint8_t* pEventData, int cbSize) {
        // Advanced input forwarding
        return 0;
    }

#include <stdio.h>

    int Start(const char* bridge_dll_path) {
        printf("[XBridge] Loading plugin DLL: %s\n", bridge_dll_path);
        fflush(stdout);
        BOOST_LOG(info) << "[XBridge] Loading plugin DLL: " << bridge_dll_path;

#ifdef _WIN32
        HMODULE hMod = LoadLibraryA(bridge_dll_path);
        if (!hMod) {
            DWORD last_err = GetLastError();
            printf("[XBridge] Failed to load DLL: %s. GetLastError=%lu\n", bridge_dll_path, last_err);
            fflush(stdout);
            BOOST_LOG(error) << "[XBridge] Failed to load DLL: " << bridge_dll_path << " err=" << last_err;
            return -1;
        }

        typedef int (*f_LoadBridge)(void*, const char*);
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
        g_Table.StartAudio = StartAudio;
        g_Table.StopProcessing = StopProcessing;
        g_Table.InjectInput = InjectInput;
        g_Table.RequestIdr = []() {
            mail::man->event<bool>(mail::idr)->raise(true);
        };

        int res = loadFunc(&g_Table, bridge_dll_path);
        if (res == 0) {
            BOOST_LOG(info) << "[XBridge] Sunshine Bridge Loaded Successfully. Host yield active.";

            // Block the main thread indefinitely to simulate service daemon
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        return res;
    }
}
