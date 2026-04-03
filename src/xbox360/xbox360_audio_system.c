#include "xbox360_audio_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xenon_sound/sound.h>

// ===[ IMA ADPCM Tables (for decoding embedded audio) ]===

static const int16_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int8_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// ===[ Helpers ]===

static X360SoundInstance* findFreeSlot(X360AudioSystem* xa) {
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        if (!xa->instances[i].active) {
            return &xa->instances[i];
        }
    }

    // Evict lowest-priority finished sound
    X360SoundInstance* best = nullptr;
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        X360SoundInstance* inst = &xa->instances[i];
        if (inst->positionInt >= inst->pcmSampleCount && !inst->loop) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        free(best->pcmData);
        best->pcmData = nullptr;
        best->active = false;
    }

    return best;
}

static X360SoundInstance* findInstanceById(X360AudioSystem* xa, int32_t instanceId) {
    int32_t slotIndex = instanceId - X360_SOUND_INSTANCE_ID_BASE;
    if (0 > slotIndex || slotIndex >= X360_MAX_SOUND_INSTANCES) return nullptr;
    X360SoundInstance* inst = &xa->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}

// Resolve a Sound resource to either embedded AUDO data or external file
// Returns PCM data (caller must free) and sets outSampleCount, outSampleRate, outChannels
static int16_t* decodeSound(DataWin* dataWin, FileSystem* fileSystem, int32_t soundIndex, uint32_t* outSampleCount, uint16_t* outSampleRate, uint8_t* outChannels) {
    if (0 > soundIndex || (uint32_t) soundIndex >= dataWin->sond.count) return nullptr;

    Sound* sound = &dataWin->sond.sounds[soundIndex];

    // Try embedded AUDO data first
    if (sound->audioFile >= 0 && (uint32_t) sound->audioFile < dataWin->audo.count) {
        AudioEntry* audo = &dataWin->audo.entries[sound->audioFile];
        if (audo->data != nullptr && audo->dataSize > 0) {
            // The embedded data is OGG Vorbis - we'd need stb_vorbis to decode
            // For now, return nullptr and fall back to external file
            // TODO: Integrate stb_vorbis for embedded OGG decoding
        }
    }

    // Try external file
    if (sound->file != nullptr && sound->file[0] != '\0') {
        char filename[512];
        bool hasExtension = (strchr(sound->file, '.') != nullptr);
        if (hasExtension) {
            snprintf(filename, sizeof(filename), "%s", sound->file);
        } else {
            snprintf(filename, sizeof(filename), "%s.ogg", sound->file);
        }

        char* fullPath = fileSystem->vtable->resolvePath(fileSystem, filename);
        if (fullPath != nullptr) {
            // TODO: Load and decode OGG/WAV from file
            // For now, stub - audio will be silent
            free(fullPath);
        }
    }

    return nullptr;
}

// ===[ Vtable Implementations ]===

static void x360AudioInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    xa->base.dataWin = dataWin;
    xa->masterGain = 1.0f;

    // Initialize libxenon audio
    xenon_sound_init();

    xa->initialized = true;
    fprintf(stderr, "X360Audio: Initialized (%u sounds, %u audio entries)\n", dataWin->sond.count, dataWin->audo.count);

    (void) fileSystem;
}

static void x360AudioDestroy(AudioSystem* audio) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;

    repeat(X360_MAX_SOUND_INSTANCES, i) {
        if (xa->instances[i].active && xa->instances[i].pcmData != nullptr) {
            free(xa->instances[i].pcmData);
        }
    }

    free(xa);
}

static void x360AudioUpdate(AudioSystem* audio, float deltaTime) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (!xa->initialized) return;

    // Update gain fading for all active instances
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        X360SoundInstance* inst = &xa->instances[i];
        if (!inst->active) continue;

        // Gain fading
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (0.0f >= inst->fadeTimeRemaining) {
                inst->currentGain = inst->targetGain;
                inst->fadeTimeRemaining = 0.0f;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
        }

        // Check if playback finished
        if (inst->pcmData != nullptr && inst->positionInt >= inst->pcmSampleCount) {
            if (inst->loop) {
                inst->positionInt = 0;
                inst->positionFrac = 0;
            }
        }
    }

    // Software mix all active sounds into the mix buffer
    memset(xa->mixBuffer, 0, sizeof(xa->mixBuffer));
    int activeSounds = 0;

    repeat(X360_MAX_SOUND_INSTANCES, i) {
        X360SoundInstance* inst = &xa->instances[i];
        if (!inst->active || inst->paused || inst->pcmData == nullptr) continue;
        if (inst->positionInt >= inst->pcmSampleCount && !inst->loop) continue;

        float gain = inst->currentGain * inst->sondVolume * xa->masterGain;
        float pitch = inst->pitch * inst->sondPitch;
        uint32_t step = (uint32_t) (pitch * (float) inst->sampleRate / (float) X360_OUTPUT_FREQ * 65536.0f);

        for (int s = 0; X360_MIX_BUFFER_SAMPLES > s; s++) {
            if (inst->positionInt >= inst->pcmSampleCount) {
                if (inst->loop) {
                    inst->positionInt = 0;
                    inst->positionFrac = 0;
                } else {
                    break;
                }
            }

            int32_t sample = (int32_t) inst->pcmData[inst->positionInt];
            int32_t scaled = (int32_t) ((float) sample * gain);

            // Mix into stereo buffer (mono -> both channels)
            int32_t left = xa->mixBuffer[s * 2 + 0] + scaled;
            int32_t right = xa->mixBuffer[s * 2 + 1] + scaled;

            // Clamp
            if (left > 32767) left = 32767;
            if (-32768 > left) left = -32768;
            if (right > 32767) right = 32767;
            if (-32768 > right) right = -32768;

            xa->mixBuffer[s * 2 + 0] = (int16_t) left;
            xa->mixBuffer[s * 2 + 1] = (int16_t) right;

            // Advance position with fractional stepping
            uint32_t newFrac = inst->positionFrac + (step & 0xFFFF);
            inst->positionInt += (step >> 16) + (newFrac < inst->positionFrac ? 1 : 0);
            inst->positionFrac = newFrac;
        }

        activeSounds++;
    }

    // Submit mixed audio to hardware
    if (activeSounds > 0) {
        xenon_sound_submit(xa->mixBuffer, sizeof(xa->mixBuffer));
    }
}

static int32_t x360PlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    DataWin* dw = xa->base.dataWin;

    if (0 > soundIndex || (uint32_t) soundIndex >= dw->sond.count) return -1;

    X360SoundInstance* inst = findFreeSlot(xa);
    if (inst == nullptr) return -1;

    // TODO: Actually decode the sound data
    // For now, create a silent instance
    memset(inst, 0, sizeof(X360SoundInstance));
    inst->active = true;
    inst->soundIndex = soundIndex;

    int32_t slotIndex = (int32_t) (inst - xa->instances);
    inst->instanceId = X360_SOUND_INSTANCE_ID_BASE + slotIndex;
    xa->nextInstanceCounter++;

    inst->priority = priority;
    inst->loop = loop;
    inst->pitch = 1.0f;
    inst->sondPitch = 1.0f;
    inst->currentGain = 1.0f;
    inst->targetGain = 1.0f;
    inst->sondVolume = 1.0f;

    Sound* sound = &dw->sond.sounds[soundIndex];
    if (sound->volume >= 0) {
        inst->sondVolume = (float) sound->volume / 100.0f;
    }

    return inst->instanceId;
}

static void x360StopSound(AudioSystem* audio, int32_t soundOrInstance) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;

    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        if (inst != nullptr) {
            inst->active = false;
            free(inst->pcmData);
            inst->pcmData = nullptr;
        }
    } else {
        repeat(X360_MAX_SOUND_INSTANCES, i) {
            if (xa->instances[i].active && xa->instances[i].soundIndex == soundOrInstance) {
                xa->instances[i].active = false;
                free(xa->instances[i].pcmData);
                xa->instances[i].pcmData = nullptr;
            }
        }
    }
}

static void x360StopAll(AudioSystem* audio) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        if (xa->instances[i].active) {
            xa->instances[i].active = false;
            free(xa->instances[i].pcmData);
            xa->instances[i].pcmData = nullptr;
        }
    }
}

static bool x360IsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        return inst != nullptr && !inst->paused;
    }
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        if (xa->instances[i].active && xa->instances[i].soundIndex == soundOrInstance && !xa->instances[i].paused)
            return true;
    }
    return false;
}

static void x360PauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        if (inst != nullptr) inst->paused = true;
    } else {
        repeat(X360_MAX_SOUND_INSTANCES, i) {
            if (xa->instances[i].active && xa->instances[i].soundIndex == soundOrInstance)
                xa->instances[i].paused = true;
        }
    }
}

static void x360ResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        if (inst != nullptr) inst->paused = false;
    } else {
        repeat(X360_MAX_SOUND_INSTANCES, i) {
            if (xa->instances[i].active && xa->instances[i].soundIndex == soundOrInstance)
                xa->instances[i].paused = false;
        }
    }
}

static void x360PauseAll(AudioSystem* audio) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        if (xa->instances[i].active) xa->instances[i].paused = true;
    }
}

static void x360ResumeAll(AudioSystem* audio) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        if (xa->instances[i].active) xa->instances[i].paused = false;
    }
}

static void x360SetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    X360SoundInstance* inst = nullptr;

    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        inst = findInstanceById(xa, soundOrInstance);
    } else {
        repeat(X360_MAX_SOUND_INSTANCES, i) {
            if (xa->instances[i].active && xa->instances[i].soundIndex == soundOrInstance) {
                inst = &xa->instances[i];
                break;
            }
        }
    }

    if (inst == nullptr) return;

    if (timeMs == 0) {
        inst->currentGain = gain;
        inst->targetGain = gain;
        inst->fadeTimeRemaining = 0.0f;
    } else {
        inst->startGain = inst->currentGain;
        inst->targetGain = gain;
        inst->fadeTotalTime = (float) timeMs / 1000.0f;
        inst->fadeTimeRemaining = inst->fadeTotalTime;
    }
}

static float x360GetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        return inst != nullptr ? inst->currentGain : 0.0f;
    }
    repeat(X360_MAX_SOUND_INSTANCES, i) {
        if (xa->instances[i].active && xa->instances[i].soundIndex == soundOrInstance)
            return xa->instances[i].currentGain;
    }
    return 0.0f;
}

static void x360SetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        if (inst != nullptr) inst->pitch = pitch;
    } else {
        repeat(X360_MAX_SOUND_INSTANCES, i) {
            if (xa->instances[i].active && xa->instances[i].soundIndex == soundOrInstance)
                xa->instances[i].pitch = pitch;
        }
    }
}

static float x360GetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        return inst != nullptr ? inst->pitch : 1.0f;
    }
    return 1.0f;
}

static float x360GetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        if (inst != nullptr && inst->sampleRate > 0) {
            return (float) inst->positionInt / (float) inst->sampleRate;
        }
    }
    return 0.0f;
}

static void x360SetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    if (soundOrInstance >= X360_SOUND_INSTANCE_ID_BASE) {
        X360SoundInstance* inst = findInstanceById(xa, soundOrInstance);
        if (inst != nullptr && inst->sampleRate > 0) {
            inst->positionInt = (uint32_t) (positionSeconds * (float) inst->sampleRate);
            inst->positionFrac = 0;
            if (inst->positionInt > inst->pcmSampleCount) {
                inst->positionInt = inst->pcmSampleCount;
            }
        }
    }
}

static void x360SetMasterGain(AudioSystem* audio, float gain) {
    X360AudioSystem* xa = (X360AudioSystem*) audio;
    xa->masterGain = gain;
}

static void x360SetChannelCount(AudioSystem* audio, int32_t count) {
    (void) audio; (void) count;
}

static void x360GroupLoad(AudioSystem* audio, int32_t groupIndex) {
    (void) audio; (void) groupIndex;
}

static bool x360GroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    (void) audio; (void) groupIndex;
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable x360AudioVtable = {
    .init = x360AudioInit,
    .destroy = x360AudioDestroy,
    .update = x360AudioUpdate,
    .playSound = x360PlaySound,
    .stopSound = x360StopSound,
    .stopAll = x360StopAll,
    .isPlaying = x360IsPlaying,
    .pauseSound = x360PauseSound,
    .resumeSound = x360ResumeSound,
    .pauseAll = x360PauseAll,
    .resumeAll = x360ResumeAll,
    .setSoundGain = x360SetSoundGain,
    .getSoundGain = x360GetSoundGain,
    .setSoundPitch = x360SetSoundPitch,
    .getSoundPitch = x360GetSoundPitch,
    .getTrackPosition = x360GetTrackPosition,
    .setTrackPosition = x360SetTrackPosition,
    .setMasterGain = x360SetMasterGain,
    .setChannelCount = x360SetChannelCount,
    .groupLoad = x360GroupLoad,
    .groupIsLoaded = x360GroupIsLoaded,
};

// ===[ Public API ]===

X360AudioSystem* X360AudioSystem_create(void) {
    X360AudioSystem* xa = safeCalloc(1, sizeof(X360AudioSystem));
    xa->base.vtable = &x360AudioVtable;
    xa->masterGain = 1.0f;
    return xa;
}
