#define MINIAUDIO_IMPLEMENTATION // required by miniaudio

// enable Vorbis decoding (provides ogg audio decoding support) for miniaudio
#define STB_VORBIS_HEADER_ONLY
#include "pc/utils/stb_vorbis.c"

#include "types.h"
#include "seq_ids.h"
#include "audio/external.h"
#include "game/camera.h"
#include "engine/math_util.h"
#include "pc/mods/mods.h"
#include "pc/lua/smlua.h"
#include "pc/lua/utils/smlua_audio_utils.h"
#include "pc/mods/mods_utils.h"
#include "pc/utils/misc.h"
#include "pc/debuglog.h"
#include "audio/external.h"

struct AudioOverride {
    bool enabled;
    bool loaded;
    const char* filename;
    u64 length;
    u8 bank;
    u8* buffer;
};

struct AudioOverride sAudioOverrides[MAX_AUDIO_OVERRIDE] = { 0 };

static void smlua_audio_utils_reset(struct AudioOverride* override) {
    if (override == NULL) { return; }

    override->enabled = false;
    override->loaded = false;

    if (override->filename) {
        free((char*)override->filename);
        override->filename = NULL;
    }

    override->length = 0;
    override->bank = 0;

    if (override->buffer != NULL) {
        free((u8*)override->buffer);
        override->buffer = NULL;
    }
}

void smlua_audio_utils_reset_all(void) {
    audio_init();
    for (s32 i = 0; i < MAX_AUDIO_OVERRIDE; i++) {
#ifdef VERSION_EU
        if (sAudioOverrides[i].enabled) {
            if (i >= SEQ_EVENT_CUTSCENE_LAKITU) {
                sBackgroundMusicDefaultVolume[i] = 75;
                return;
            }
            sBackgroundMusicDefaultVolume[i] = sBackgroundMusicDefaultVolumeDefault[i];
        }
#else
        if (sAudioOverrides[i].enabled) { sound_reset_background_music_default_volume(i); }
#endif
        smlua_audio_utils_reset(&sAudioOverrides[i]);
    }
}

bool smlua_audio_utils_override(u8 sequenceId, s32* bankId, void** seqData) {
    if (sequenceId >= MAX_AUDIO_OVERRIDE) { return false; }
    struct AudioOverride* override = &sAudioOverrides[sequenceId];
    if (!override->enabled) { return false; }

    if (override->loaded) {
        *seqData = override->buffer;
        *bankId = override->bank;
        return true;
    }

    static u8* buffer = NULL;
    static long int length = 0;

    FILE* fp = fopen(override->filename, "rb");
    if (!fp) { return false; }
    fseek(fp, 0L, SEEK_END);
    length = ftell(fp);

    buffer = malloc(length+1);
    if (buffer == NULL) {
        LOG_ERROR("Failed to malloc m64 sound file");
        fclose(fp);
        return false;
    }

    fseek(fp, 0L, SEEK_SET);
    fread(buffer, length, 1, fp);

    fclose(fp);

    // cache
    override->loaded = true;
    override->buffer = buffer;
    override->length = length;

    *seqData = buffer;
    *bankId = override->bank;
    return true;
}

void smlua_audio_utils_replace_sequence(u8 sequenceId, u8 bankId, u8 defaultVolume, const char* m64Name) {
    if (gLuaActiveMod == NULL) { return; }
    if (sequenceId >= MAX_AUDIO_OVERRIDE) {
        LOG_LUA_LINE("Invalid sequenceId given to smlua_audio_utils_replace_sequence(): %d", sequenceId);
        return;
    }

    if (bankId >= 64) {
        LOG_LUA_LINE("Invalid bankId given to smlua_audio_utils_replace_sequence(): %d", bankId);
        return;
    }

    char m64path[SYS_MAX_PATH] = { 0 };
    if (snprintf(m64path, SYS_MAX_PATH-1, "sound/%s.m64", m64Name) < 0) {
        LOG_LUA_LINE("Could not concat m64path: %s", m64path);
        return;
    }
    normalize_path(m64path);

    for (s32 i = 0; i < gLuaActiveMod->fileCount; i++) {
        struct ModFile* file = &gLuaActiveMod->files[i];
        char relPath[SYS_MAX_PATH] = { 0 };
        snprintf(relPath, SYS_MAX_PATH-1, "%s", file->relativePath);
        normalize_path(relPath);
        if (str_ends_with(relPath, m64path)) {
            struct AudioOverride* override = &sAudioOverrides[sequenceId];
            if (override->enabled) { audio_init(); }
            smlua_audio_utils_reset(override);
            LOG_INFO("Loading audio: %s", file->cachedPath);
            override->filename = strdup(file->cachedPath);
            override->enabled = true;
            override->bank = bankId;
#ifdef VERSION_EU
            //sBackgroundMusicDefaultVolume[sequenceId] = defaultVolume;
#else
            sound_set_background_music_default_volume(sequenceId, defaultVolume);
#endif
            return;
        }
    }

    LOG_LUA_LINE("Could not find m64 at path: %s", m64path);
}

  ///////////////
 // mod audio //
///////////////

// Optimization: disable spatialization for everything as it's not used
#define MA_SOUND_STREAM_FLAGS (MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_STREAM)
#define MA_SOUND_SAMPLE_FLAGS (MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_DECODE) // No pitch, pre-decode audio samples

ma_engine gModAudioEngine;
static struct DynamicPool *sModAudio;

static struct ModAudio* find_mod_audio(struct ModFile* file) {
    struct DynamicPoolNode* node = sModAudio->tail;
    while (node) {
        struct DynamicPoolNode* prev = node->prev;
        struct ModAudio* audio = node->ptr;
        if (audio->file == file) { return audio; }
        node = prev;
    }
    return NULL;
}

static bool audio_sanity_check(struct ModAudio* audio, bool isStream, const char* action) {
    if (audio == NULL) {
        LOG_LUA_LINE("Tried to %s unloaded audio stream", action);
        return false;
    }
    if (isStream && !audio->isStream) {
        LOG_LUA_LINE("Tried to %s a sample as a stream", action);
        return false;
    }
    if (!isStream && audio->isStream) {
        LOG_LUA_LINE("Tried to %s a stream as a sample", action);
        return false;
    }
    return true;
}

struct ModAudio* audio_load_internal(const char* filename, bool isStream) {
    // check file type
    bool validFileType = false;
    const char* fileTypes[] = { ".mp3", ".aiff", ".ogg", NULL };
    const char** ft = fileTypes;
    while (*ft != NULL) {
        if (str_ends_with((char*)filename, (char*)*ft)) {
            validFileType = true;
            break;
        }
        ft++;
    }
    if (!validFileType) {
        LOG_LUA_LINE("Tried to load audio file with invalid file type: %s", filename);
        return NULL;
    }

    // find mod file in mod list
    bool foundModFile = false;
    struct ModFile* modFile = NULL;
    u16 fileCount = gLuaActiveMod->fileCount;
    for(u16 i = 0; i < fileCount; i++) {
        struct ModFile* file = &gLuaActiveMod->files[i];
        if(str_ends_with(file->relativePath, (char*)filename)) {
            foundModFile = true;
            modFile = file;
            break;
        }
    }
    if(!foundModFile) {
        LOG_LUA_LINE("Could not find audio file: '%s'", filename);
        return NULL;
    }

    // find stream in ModAudio list
    struct ModAudio* audio = find_mod_audio(modFile);
    if (audio) {
        if (isStream == audio->isStream) {
            return audio;
        } else if (isStream) {
            LOG_LUA_LINE("Tried to load a stream, when a sample already exists for '%s'", filename);
            return NULL;
        } else {
            LOG_LUA_LINE("Tried to load a sample, when a stream already exists for '%s'", filename);
            return NULL;
        }
    }

    // allocate in ModAudio pool
    if (audio == NULL) {
        audio = dynamic_pool_alloc(sModAudio, sizeof(struct ModAudio));
        if (!audio) {
            LOG_LUA_LINE("Could not allocate space for new mod audio!");
            return NULL;
        }
    }

    // remember file
    audio->file = modFile;

    // load audio
    ma_result result = ma_sound_init_from_file(
        &gModAudioEngine, modFile->cachedPath,
        isStream ? MA_SOUND_STREAM_FLAGS : MA_SOUND_SAMPLE_FLAGS,
        NULL, NULL, &audio->sound
    );
    if (result != MA_SUCCESS) {
        LOG_ERROR("failed to load audio file '%s': %d", filename, result);
        return NULL;
    }

    audio->isStream = isStream;
    return audio;
}

struct ModAudio* audio_stream_load(const char* filename) {
    return audio_load_internal(filename, true);
}

void audio_stream_destroy(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, true, "destroy")) {
        return;
    }

    ma_sound_uninit(&audio->sound);
}

void audio_stream_play(struct ModAudio* audio, bool restart, f32 volume) {
    if (!audio_sanity_check(audio, true, "play")) {
        return;
    }
    f32 masterVolume = (f32)configMasterVolume / 127.0f;
    f32 musicVolume = (f32)configMusicVolume / 127.0f;
    ma_sound_set_volume(&audio->sound, masterVolume * musicVolume * volume);
    if (restart || !ma_sound_is_playing(&audio->sound)) { ma_sound_seek_to_pcm_frame(&audio->sound, 0); }
    ma_sound_start(&audio->sound);
}

void audio_stream_pause(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, true, "pause")) {
        return;
    }
    ma_sound_stop(&audio->sound);
}

void audio_stream_stop(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, true, "stop")) {
        return;
    }
    ma_sound_stop(&audio->sound);
    ma_sound_seek_to_pcm_frame(&audio->sound, 0);
}

f32 audio_stream_get_position(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, true, "getpos")) {
        return 0;
    }
    // ! This gets the time that the audio has been playing for, but is not reset when the stream loops
    return (f32)ma_sound_get_time_in_milliseconds(&audio->sound) / 1000;
}

void audio_stream_set_position(struct ModAudio* audio, f32 pos) {
    if (!audio_sanity_check(audio, true, "setpos")) {
        return;
    }
    u64 length;
    ma_sound_get_length_in_pcm_frames(&audio->sound, &length);
    ma_sound_seek_to_pcm_frame(&audio->sound, (u64)(length * pos));
}

bool audio_stream_get_looping(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, true, "getloop")) {
        return false;
    }
    return ma_sound_is_looping(&audio->sound);
}

void audio_stream_set_looping(struct ModAudio* audio, bool looping) {
    if (!audio_sanity_check(audio, true, "setloop")) {
        return;
    }
    ma_sound_set_looping(&audio->sound, looping);
}

f32 audio_stream_get_frequency(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, true, "getfreq")) {
        return 0;
    }
    return ma_sound_get_pitch(&audio->sound);
}

void audio_stream_set_frequency(struct ModAudio* audio, f32 freq) {
    if (!audio_sanity_check(audio, true, "setfreq")) {
        return;
    }
    ma_sound_set_pitch(&audio->sound, freq);
}

// f32 audio_stream_get_tempo(struct ModAudio* audio) {
//     if (!audio_sanity_check(audio, true, "gettempo")) {
//         return 0;
//     }
//     return bassh_get_tempo(audio->handle);
// }

// void audio_stream_set_tempo(struct ModAudio* audio, f32 tempo) {
//     if (!audio_sanity_check(audio, true, "settempo")) {
//         return;
//     }
//     bassh_set_tempo(audio->handle, tempo);
// }

f32 audio_stream_get_volume(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, true, "getvol")) {
        return 0;
    }
    return ma_sound_get_volume(&audio->sound);
}

void audio_stream_set_volume(struct ModAudio* audio, f32 volume) {
    if (!audio_sanity_check(audio, true, "setvol")) {
        return;
    }
    f32 masterVolume = (f32)configMasterVolume / 127.0f;
    f32 musicVolume = (f32)configMusicVolume / 127.0f;
    ma_sound_set_volume(&audio->sound, masterVolume * musicVolume * volume);
}

// void audio_stream_set_speed(struct ModAudio* audio, f32 initial_freq, f32 speed, bool pitch) {
//     if (!audio_sanity_check(audio, true, "setspeed")) {
//         return;
//     }
//     bassh_set_speed(audio->handle, initial_freq, speed, pitch);
// }

//////////////////////////////////////

// MA calls the end callback from it's audio thread
// Use mutexes to be sure we don't try to delete the same memory at the same time
#include <pthread.h>
static pthread_mutex_t sSampleCopyMutex = PTHREAD_MUTEX_INITIALIZER;
static struct ModAudioSampleCopies *sSampleCopiesPendingUninitTail = NULL;

// Called whenever a sample copy finishes playback (called from the miniaudio thread)
// removes the copy from it's linked list, and adds it to the pending list
static void audio_sample_copy_end_callback(void* userData, UNUSED ma_sound* sound) {
    pthread_mutex_lock(&sSampleCopyMutex);

    struct ModAudioSampleCopies *copy = userData;
    if (copy->next) { copy->next->prev = copy->prev; }
    if (copy->prev) { copy->prev->next = copy->next; }
    if (!copy->next && !copy->prev) {
        copy->parent->sampleCopiesTail = NULL; // Clear the pointer to this copy
    }
    copy->next = NULL;
    copy->prev = NULL;

    // add copy to list
    if (!sSampleCopiesPendingUninitTail) {
        sSampleCopiesPendingUninitTail = copy;
    } else {
        copy->prev = sSampleCopiesPendingUninitTail;
        sSampleCopiesPendingUninitTail->next = copy;
        sSampleCopiesPendingUninitTail = copy;
    }
    pthread_mutex_unlock(&sSampleCopyMutex);
}

// Called every frame in the main thread from smlua_update()
// Frees all audio sample copies that are in the pending list
void audio_sample_destroy_pending_copies(void) {
    if (sSampleCopiesPendingUninitTail) {
        pthread_mutex_lock(&sSampleCopyMutex);
        for (struct ModAudioSampleCopies *node = sSampleCopiesPendingUninitTail; node;) {
            struct ModAudioSampleCopies *prev = node->prev;
            ma_sound_uninit(&node->sound);
            free(node);
            node = prev;
        }
        sSampleCopiesPendingUninitTail = NULL;
        pthread_mutex_unlock(&sSampleCopyMutex);
    }
}

static void audio_sample_destroy_copies(struct ModAudio* audio) {
    pthread_mutex_lock(&sSampleCopyMutex);
    for (struct ModAudioSampleCopies* node = audio->sampleCopiesTail; node;) {
        struct ModAudioSampleCopies* prev = node->prev;
        ma_sound_uninit(&node->sound);
        free(node);
        node = prev;
    }
    audio->sampleCopiesTail = NULL;
    pthread_mutex_unlock(&sSampleCopyMutex);
}

struct ModAudio* audio_sample_load(const char* filename) {
    return audio_load_internal(filename, false);
}

void audio_sample_destroy(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, false, "destroy")) {
        return;
    }

    if (audio->sampleCopiesTail) {
        audio_sample_destroy_copies(audio);
    }
    ma_sound_uninit(&audio->sound);
}

void audio_sample_stop(struct ModAudio* audio) {
    if (!audio_sanity_check(audio, false, "stop")) {
        return;
    }
    if (audio->sampleCopiesTail) {
        audio_sample_destroy_copies(audio);
    }
    ma_sound_stop(&audio->sound);
    ma_sound_seek_to_pcm_frame(&audio->sound, 0);
}

void audio_sample_play(struct ModAudio* audio, Vec3f position, f32 volume) {
    if (!audio_sanity_check(audio, false, "play")) {
        return;
    }

    ma_sound *sound = &audio->sound;
    if (ma_sound_is_playing(sound)) {
        struct ModAudioSampleCopies* copy = calloc(1, sizeof(struct ModAudioSampleCopies));
        ma_sound_init_copy(&gModAudioEngine, sound, MA_SOUND_SAMPLE_FLAGS, NULL, &copy->sound);
        copy->parent = audio;

        if (!audio->sampleCopiesTail) {
            audio->sampleCopiesTail = copy;
        } else {
            copy->prev = audio->sampleCopiesTail;
            audio->sampleCopiesTail->next = copy;
            audio->sampleCopiesTail = copy;
        }
        sound = &copy->sound;
        ma_sound_set_end_callback(sound, audio_sample_copy_end_callback, copy);
    }

    f32 dist = 0;
    f32 pan = 0.5f;
    if (gCamera) {
        f32 dX = position[0] - gCamera->pos[0];
        f32 dY = position[1] - gCamera->pos[1];
        f32 dZ = position[2] - gCamera->pos[2];
        dist = sqrtf(dX * dX + dY * dY + dZ * dZ);

        Mat4 mtx;
        mtxf_translate(mtx, position);
        mtxf_mul(mtx, mtx, gCamera->mtx);
        f32 factor = 10;
        pan = (get_sound_pan(mtx[3][0] * factor, mtx[3][2] * factor) - 0.5f) * 2.0f;
    }

    f32 intensity = sound_get_level_intensity(dist);
    f32 masterVolume = (f32)configMasterVolume / 127.0f;
    f32 sfxVolume = (f32)configSfxVolume / 127.0f;
    ma_sound_set_volume(sound, masterVolume * sfxVolume * volume * intensity);
    ma_sound_set_pan(sound, pan);

    ma_sound_start(sound);
}

void audio_custom_shutdown(void) {
    if (!sModAudio) { return; }
    struct DynamicPoolNode* node = sModAudio->tail;
    while (node) {
        struct DynamicPoolNode* prev = node->prev;
        struct ModAudio* audio = node->ptr;
        if (audio->isStream) {
            audio_stream_destroy(audio);
        } else {
            audio_sample_destroy(audio);
        }
        node = prev;
    }
    dynamic_pool_free_pool(sModAudio);
}

void smlua_audio_custom_init(void) {
    sModAudio = dynamic_pool_init();

    ma_result result = ma_engine_init(NULL, &gModAudioEngine);
    if (result != MA_SUCCESS) {
        LOG_ERROR("failed to init miniaudio: %d", result);
    }
}

void smlua_audio_custom_deinit(void) {
    if (sModAudio) {
        audio_custom_shutdown();
        free(sModAudio);
        ma_engine_uninit(&gModAudioEngine);
        sModAudio = NULL;
    }
}
