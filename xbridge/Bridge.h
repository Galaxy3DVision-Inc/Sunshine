#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

    // Outbound hook types (XLang -> Sunshine)
    typedef bool (*f_StartVideo)(const char* display, int width, int height, int fps, int bitrate);
    typedef bool (*f_StartAudio)(const char* audioSink);
    typedef void (*f_StopProcessing)();
    typedef int (*f_InjectInput)(const uint8_t* pEventData, int cbSize);

    // Inbound listener types (Sunshine -> XLang)
    typedef void (*f_OnVideoFrame)(const uint8_t* data, int size, bool isIdr, int64_t frameIndex);
    typedef void (*f_OnAudioPacket)(const uint8_t* data, int size, int64_t pts);

#ifdef __cplusplus
}
#endif

// Shared ABI Table
struct SunshineCallTable
{
    f_StartVideo StartVideo;
    f_StartAudio StartAudio;
    f_StopProcessing StopProcessing;
    f_InjectInput InjectInput;

    f_OnVideoFrame OnVideoFrame;
    f_OnAudioPacket OnAudioPacket;
};
