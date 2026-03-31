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
    // Because we bypassed stream.cpp, we create the mail channels globally for the bridge.
    static safe::mail_t g_bridge_mail;

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

        if(!g_bridge_mail) {
            g_bridge_mail = std::make_shared<safe::mail_raw_t>();
        }

        std::thread([cfg]() {
            platf::set_thread_name("xbridge::video");
            video::capture(g_bridge_mail, cfg, nullptr);
        }).detach();

        // NAL draining thread
        std::thread([]() {
            platf::set_thread_name("xbridge::video_drain");
            auto packets = g_bridge_mail->queue<video::packet_t>(mail::video_packets);
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

        if(!g_bridge_mail) {
            g_bridge_mail = std::make_shared<safe::mail_raw_t>();
        }
        
        std::thread([cfg]() {
            platf::set_thread_name("xbridge::audio");
            audio::capture(g_bridge_mail, cfg, nullptr);
        }).detach();

        std::thread([]() {
            platf::set_thread_name("xbridge::audio_drain");
            auto packets = g_bridge_mail->queue<audio::packet_t>(mail::audio_packets);
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
        if(g_bridge_mail) {
            auto shutdown_event = g_bridge_mail->event<bool>(mail::shutdown);
            shutdown_event->raise(true);
        }
    }

    int InjectInput(const uint8_t* pEventData, int cbSize) {
        // Advanced input forwarding
        return 0;
    }

    int Start(const char* bridge_dll_path) {
        BOOST_LOG(info) << "[XBridge] Loading plugin DLL: " << bridge_dll_path;

#ifdef _WIN32
        HMODULE hMod = LoadLibraryA(bridge_dll_path);
        if (!hMod) {
            BOOST_LOG(error) << "[XBridge] Failed to load DLL: " << bridge_dll_path;
            return -1;
        }

        typedef int (*f_LoadBridge)(void*, const char*);
        f_LoadBridge loadFunc = (f_LoadBridge)GetProcAddress(hMod, "LoadBridge");
        if (!loadFunc) {
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
