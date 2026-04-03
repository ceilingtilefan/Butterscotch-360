#pragma once

#include "audio_system.h"

#define XDK_MAX_SOUND_INSTANCES 32
#define XDK_SOUND_INSTANCE_ID_BASE 100000

typedef struct {
    AudioSystem base;

    void* pXAudio2;         // IXAudio2*
    void* pMasterVoice;     // IXAudio2MasteringVoice*
    float masterGain;
    bool initialized;

    FileSystem* fileSystem; // for loading external audio files

    // Sound instance tracking (managed in C++ implementation)
    void* instanceData;     // opaque pointer to C++ instance array
    int nextInstanceCounter;
} XdkAudioSystem;

XdkAudioSystem* XdkAudioSystem_create(void);
