#pragma once

#include "../audio_system.h"

#include <stdint.h>
#include <stdbool.h>

#define X360_MAX_SOUND_INSTANCES 64
#define X360_SOUND_INSTANCE_ID_BASE 100000
#define X360_MIX_BUFFER_SAMPLES 512
#define X360_OUTPUT_FREQ 48000

// ===[ Sound Instance ]===

typedef struct {
    bool active;
    int32_t soundIndex;   // SOND resource index
    int32_t instanceId;   // unique ID returned to GML
    int32_t priority;
    bool loop;
    bool paused;

    // Decoded PCM data (fully loaded into RAM)
    int16_t* pcmData;
    uint32_t pcmSampleCount;
    uint16_t sampleRate;
    uint8_t channels;

    // Playback position (32.32 fixed-point for fractional sample stepping)
    uint32_t positionInt;
    uint32_t positionFrac;

    // Pitch
    float pitch;
    float sondPitch;

    // Gain / volume
    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float sondVolume;
} X360SoundInstance;

// ===[ Xbox 360 Audio System ]===

typedef struct {
    AudioSystem base;

    // Sound instances
    X360SoundInstance instances[X360_MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;

    // Mixer output buffer (stereo interleaved)
    int16_t mixBuffer[X360_MIX_BUFFER_SAMPLES * 2];

    float masterGain;
    bool initialized;
} X360AudioSystem;

X360AudioSystem* X360AudioSystem_create(void);
