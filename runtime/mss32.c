/* Miles Sound System 6.5 stand-in (mss32.dll): the 55 AIL functions Enter the Matrix imports,
   played through the runtime's mixer (dsound.c's runtime voices, sound.h). Started from
   li2-native's stand-in (LEGO Island 2 uses Miles 6 differently: files and MIDI).

   What the game does (its sound code, 0x792818 start-up, 0x79723b banks, 0x79cf9b streams,
   0x79d7f6 effects, 0x79ec9f interactive music) [verified: Ghidra decompiles through REA]:
   - AIL_mem_use_malloc/free (its own allocator), AIL_startup, AIL_set_preference(1, 64),
     AIL_open_digital_driver(44100, 16, 2, 0), 64 2D sample handles, then AIL_enumerate_3D_providers
     looking for "Creative Labs EAX 3 (TM)", "... EAX 2 (TM)", "... EAX (TM)" and "Miles Fast 2D
     Positional Audio", opening the first that works in that order (EAX level 3..0 at 0x9955a8),
     64 3D sample handles, a listener at the origin.
   - Sound banks (.wad): IMA ADPCM (36-byte blocks, mono) decompressed once with
     AIL_decompress_ADPCM into WAV images (44-byte header + 16-bit PCM); effects play the PCM
     after the header with AIL_set_sample_address (2D) or AIL_set_3D_sample_info (3D).
   - Streams and the interactive music: whole files read with AIL_file_read(name, -1) (the size
     first, then the data) and played as IMA ADPCM straight from memory (sample type 5 mono /
     7 stereo, block 36 / 72, 44.1 kHz); the music watches AIL_sample_ms_position.
   - The car engine: a looping 48 kHz 16-bit sample over a buffer the game keeps writing.
   - Volumes and pans are floats 0..1 (pan 0.5 centre); 3D distances (max, min).

   So PCM plays straight from the program's memory (voice_new_guest: read as it plays) and IMA
   ADPCM is decoded into a voice of its own when a sample starts. Handles are small blocks in
   program memory ([magic][index]); the state is here. 3D samples are placed by the listener
   each time a position changes and once a frame: DirectSound3D's rolloff from the minimum
   distance, silent past the maximum, panned by the listener's right (left-handed).
   EAX settings (rooms, reverb mixes) are accepted and not heard (no reverb yet). */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "sound.h"

#define LOGS(...) LOG(LOG_SOUND, __VA_ARGS__)

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
enum { K_DRIVER = 1, K_SAMPLE, K_3DSAMPLE, K_PROVIDER, K_LISTENER };
/* Miles sample types (AIL_set_sample_type) */
enum { DIG_F_16BITS = 1, DIG_F_STEREO = 2, DIG_F_ADPCM = 4 };
#define MAGIC 0x53534d4cu                    /* "LMSS" */
#define MAXOBJ 512

typedef struct Obj {
    int kind, used;
    uint32_t handle;                    /* the program's handle */
    int voice;                          /* runtime voice, -1 none */
    int ended;                          /* played to its end: the position stays at the end */
    int16_t *decoded;                   /* ADPCM decoded for the voice (owned) */
    uint32_t dec_frames;
    int status;
    /* what to play */
    uint32_t addr, len;                 /* the data in program memory */
    int type;                           /* DIG_F_* */
    uint32_t block;                     /* ADPCM block size */
    uint32_t rate;                      /* playback rate */
    int loops;                          /* 1 once, 0 forever, n passes */
    float volume, pan;                  /* 0..1, pan 0.5 centre */
    /* 3D */
    float pos[3], front[3], up[3];
    float maxd, mind;
} Obj;

static Obj objs[MAXOBJ];
static uint32_t redist_dir, last_error_str;
static int listener = -1;
static float rolloff = 1.0f, master = 1.0f;
static uint32_t prefs[64];

/* ------------------------------------------------------------------ handles */
static uint32_t obj_new(int kind)
{
    for (int i = 1; i < MAXOBJ; i++)
        if (!objs[i].used) {
            Obj *o = &objs[i];
            uint32_t h = o->handle;
            memset(o, 0, sizeof *o);
            if (!h)
                h = galloc(64);
            memset(gptr(h), 0, 64);
            W32(h, MAGIC);
            W32(h + 4, (uint32_t)i);
            o->handle = h;
            o->used = 1;
            o->kind = kind;
            o->voice = -1;
            o->status = SMP_DONE;
            o->volume = 1.0f;
            o->pan = 0.5f;
            o->loops = 1;
            o->rate = 22050;
            o->type = DIG_F_16BITS;
            o->maxd = 100.0f;
            o->mind = 1.0f;
            o->front[2] = 1.0f;
            o->up[1] = 1.0f;
            return h;
        }
    fatal("Miles stand-in: out of handles");
}

static Obj *obj_get(uint32_t h, int kind)
{
    if (!h || R32(h) != MAGIC)
        return NULL;
    uint32_t i = R32(h + 4);
    if (i == 0 || i >= MAXOBJ || !objs[i].used || (kind && objs[i].kind != kind))
        return NULL;
    return &objs[i];
}
/* a 2D or 3D sample */
static Obj *smp_get(uint32_t h)
{
    Obj *o = obj_get(h, 0);
    return o && (o->kind == K_SAMPLE || o->kind == K_3DSAMPLE) ? o : NULL;
}

static float fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ------------------------------------------------------------------ IMA ADPCM */
static const int ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static int16_t ima_nibble(int nib, int *pred, int *idx)
{
    int step = ima_step[*idx], diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    *pred += nib & 8 ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += ima_index[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

/* frames a block holds: the header's sample and two a byte after the headers */
static uint32_t ima_per_block(uint32_t align, uint32_t ch)
{
    return align > 4 * ch ? (align - 4 * ch) * 2 / ch + 1 : 0;
}

/* IMA ADPCM (Microsoft's block layout) to 16-bit PCM; returns the frames written (a last partial
   block is decoded as far as it goes) */
static uint32_t ima_decode(const uint8_t *data, uint32_t n, uint32_t align, uint32_t ch, int16_t *d)
{
    uint32_t per = ima_per_block(align, ch);
    if (!per || ch < 1 || ch > 2)
        return 0;
    uint32_t frames = 0;
    for (uint32_t off = 0; off + 4 * ch <= n; off += align) {
        const uint8_t *q = data + off, *qend = data + (off + align < n ? off + align : n);
        int pred[2], idx[2];
        for (uint32_t c = 0; c < ch; c++) {
            pred[c] = (int16_t)(q[4 * c] | q[4 * c + 1] << 8);
            idx[c] = q[4 * c + 2] > 88 ? 88 : q[4 * c + 2];
            d[c] = (int16_t)pred[c];
        }
        d += ch;
        frames++;
        q += 4 * ch;
        /* then groups of 4 bytes (8 frames) per channel, the channels taking turns */
        for (uint32_t k = 0; k < (per - 1) / 8 && q + 4 * ch <= qend; k++) {
            for (uint32_t c = 0; c < ch; c++) {
                for (int j = 0; j < 4; j++) {
                    uint8_t v = q[j];
                    d[(2 * j) * ch + c] = ima_nibble(v & 15, &pred[c], &idx[c]);
                    d[(2 * j + 1) * ch + c] = ima_nibble(v >> 4, &pred[c], &idx[c]);
                }
                q += 4;
            }
            d += 8 * ch;
            frames += 8;
        }
    }
    return frames;
}

static uint32_t ima_frames(uint32_t n, uint32_t align, uint32_t ch)
{
    return align ? (n / align + 1) * ima_per_block(align, ch) : 0;
}

/* ------------------------------------------------------------------ playing */
static void free_voice(Obj *o)
{
    if (o->voice >= 0)
        voice_free(o->voice);
    o->voice = -1;
    free(o->decoded);
    o->decoded = NULL;
    o->dec_frames = 0;
}

static int channels_of(const Obj *o) { return o->type & DIG_F_STEREO ? 2 : 1; }

/* a voice for what the sample was given (at start) */
static void make_voice(Obj *o)
{
    free_voice(o);
    if (!o->addr || !o->len)
        return;
    int ch = channels_of(o);
    if (o->type & DIG_F_ADPCM) {
        uint32_t cap = ima_frames(o->len, o->block, (uint32_t)ch);
        if (!cap)
            return;
        o->decoded = malloc((size_t)cap * ch * 2 + 16);
        o->dec_frames = ima_decode((const uint8_t *)gptr(o->addr), o->len, o->block, (uint32_t)ch, o->decoded);
        if (o->dec_frames)
            o->voice = voice_new(o->decoded, o->dec_frames, ch, (int)o->rate);
    } else {
        o->voice = voice_new_guest(o->addr, o->len, ch, o->type & DIG_F_16BITS ? 16 : 8, (int)o->rate);
    }
    if (o->voice >= 0 && o->rate)
        voice_set_rate(o->voice, o->rate);
}

/* the 3D listener's view of a sample: gain and the left/right balance */
static void place_3d(const Obj *o, float *g, float *bal)
{
    *g = 1.0f;
    *bal = 0.0f;
    const Obj *l = listener > 0 && objs[listener].used ? &objs[listener] : NULL;
    if (!l)
        return;
    float d[3] = {o->pos[0] - l->pos[0], o->pos[1] - l->pos[1], o->pos[2] - l->pos[2]};
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (o->maxd > 0 && dist > o->maxd) {
        *g = 0.0f;
        return;
    }
    float mind = o->mind > 0.001f ? o->mind : 0.001f;
    *g = dist > mind ? mind / (mind + rolloff * (dist - mind)) : 1.0f;
    if (dist > 1e-4f) {
        const float *f = l->front, *u = l->up;
        float r[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl > 1e-6f)
            *bal = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (rl * dist);
    }
}

static void apply_gain(Obj *o)
{
    if (o->voice < 0)
        return;
    float v = o->volume < 0 ? 0 : o->volume > 1 ? 1 : o->volume;
    float p = (o->pan < 0 ? 0 : o->pan > 1 ? 1 : o->pan) * 2.0f - 1.0f;   /* -1..1 */
    if (o->kind == K_3DSAMPLE) {
        float g, bal;
        place_3d(o, &g, &bal);
        v *= g;
        p = bal * 0.8f;                 /* (a 3D sound is never fully in one ear) */
    }
    v *= master;
    /* equal-power pan, unity in the centre */
    float l = p > 0 ? 1.0f - p : 1.0f, r = p < 0 ? 1.0f + p : 1.0f;
    voice_set_gain(o->voice, v * l, v * r);
}

static void start(Obj *o)
{
    make_voice(o);
    if (o->voice < 0) {
        o->status = SMP_DONE;
        return;
    }
    apply_gain(o);
    voice_play(o->voice, o->loops);
    o->status = SMP_PLAYING;
    o->ended = 0;
}

static int status_of(Obj *o)
{
    if (o->status == SMP_PLAYING && (o->voice < 0 || !voice_playing(o->voice, NULL))) {
        o->status = SMP_DONE;
        o->ended = 1;
    }
    return o->status;
}

static void stop(Obj *o)
{
    if (status_of(o) == SMP_PLAYING) {
        voice_pause(o->voice);
        o->status = SMP_STOPPED;
    }
}

static void resume(Obj *o)
{
    if (o->status == SMP_STOPPED && o->voice >= 0) {
        voice_play(o->voice, o->loops);
        o->status = SMP_PLAYING;
    } else if (o->status == SMP_DONE && o->addr) {
        start(o);                       /* (Miles: resuming a sample never started starts it) */
    }
}

static void end(Obj *o)
{
    free_voice(o);
    o->status = SMP_DONE;
    o->ended = 0;
}

static void release(Obj *o)
{
    free_voice(o);
    o->used = 0;
    W32(o->handle, 0);
}

static uint32_t total_frames(const Obj *o);

/* frames played so far (in the data's own frames). A sample played to its end stays at its end:
   the movie player takes current == total as "the sound is over" (0x7bddcb) [verified: Ghidra
   decompile; with 0 after the end the 1.5 s transition movie, whose sound is 1.0 s, never ended] */
static uint32_t played_frames(Obj *o)
{
    uint32_t pos = 0;
    if (o->voice >= 0 && status_of(o) != SMP_DONE)
        voice_playing(o->voice, &pos);
    else if (o->ended)
        return total_frames(o);
    return pos;
}

static uint32_t total_frames(const Obj *o)
{
    int ch = channels_of(o);
    if (o->type & DIG_F_ADPCM)
        return o->decoded ? o->dec_frames : ima_frames(o->len, o->block, (uint32_t)ch);
    return o->len / (uint32_t)(ch * (o->type & DIG_F_16BITS ? 2 : 1));
}

/* the game thread, once a frame: 3D gains follow the listener */
void mss_frame(void)
{
    for (int i = 1; i < MAXOBJ; i++) {
        Obj *o = &objs[i];
        if (o->used && o->kind == K_3DSAMPLE && o->status == SMP_PLAYING)
            apply_gain(o);
    }
}

/* ------------------------------------------------------------------ the AIL functions */
#define AIL(name, n) HOSTFN(w_MSS32__AIL_##name##_##n, (n) / 4)

static void set_error(const char *s)
{
    if (!last_error_str)
        last_error_str = galloc(256);
    gstrncpy(last_error_str, s, 256);
}

AIL(startup, 0) { LOGS("AIL_startup"); set_error(""); return 1; }
AIL(shutdown, 0)
{
    LOGS("AIL_shutdown");
    for (int i = 1; i < MAXOBJ; i++)
        if (objs[i].used)
            free_voice(&objs[i]);
    return 0;
}
AIL(set_redist_directory, 4)
{
    uint32_t prev = redist_dir;
    redist_dir = ARG(0);
    return prev;
}
AIL(last_error, 0) { if (!last_error_str) set_error(""); return last_error_str; }
/* (the program's allocator: our own allocations go through the runtime's) */
AIL(mem_use_malloc, 4) { return 0; }
AIL(mem_use_free, 4) { return 0; }
AIL(mem_free_lock, 4) { if (ARG(0)) gfree(ARG(0)); return 0; }
/* (number, value): the previous value */
AIL(set_preference, 8)
{
    uint32_t n = ARG(0), prev = n < 64 ? prefs[n] : 0;
    if (n < 64)
        prefs[n] = ARG(1);
    return prev;
}
/* (frequency, bits, channels, flags) */
AIL(open_digital_driver, 16)
{
    LOGS("AIL_open_digital_driver(%u Hz, %u bits, %u ch)", ARG(0), ARG(1), ARG(2));
    return obj_new(K_DRIVER);
}
/* (driver, level 0..1) */
AIL(set_digital_master_volume_level, 8)
{
    master = fl(ARG(1));
    for (int i = 1; i < MAXOBJ; i++)
        if (objs[i].used && objs[i].status == SMP_PLAYING)
            apply_gain(&objs[i]);
    return 0;
}

/* 2D samples */
AIL(allocate_sample_handle, 4) { return obj_new(K_SAMPLE); }
AIL(release_sample_handle, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) release(o); return 0; }
AIL(init_sample, 4)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) {
        free_voice(o);
        o->status = SMP_DONE;
        o->ended = 0;
        o->addr = o->len = 0;
        o->type = DIG_F_16BITS;
        o->block = 0;
        o->rate = 22050;
        o->loops = 1;
        o->volume = 1.0f;
        o->pan = 0.5f;
    }
    return 0;
}
/* (sample, start, length) */
AIL(set_sample_address, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->addr = ARG(1); o->len = ARG(2); o->ended = 0; }
    return 0;
}
/* (sample, format, flags) */
AIL(set_sample_type, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) o->type = (int)ARG(1);
    return 0;
}
AIL(set_sample_adpcm_block_size, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) o->block = ARG(1);
    return 0;
}
AIL(set_sample_playback_rate, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->rate = ARG(1); if (o->voice >= 0 && o->rate) voice_set_rate(o->voice, o->rate); }
    return 0;
}
/* (sample, volume 0..1, pan 0..1) */
AIL(set_sample_volume_pan, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->volume = fl(ARG(1)); o->pan = fl(ARG(2)); apply_gain(o); }
    return 0;
}
AIL(sample_volume_pan, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (ARG(1)) W32(ARG(1), fu(o ? o->volume : 0));
    if (ARG(2)) W32(ARG(2), fu(o ? o->pan : 0.5f));
    return 0;
}
/* (sample, dry, wet): EAX reverb levels (no reverb yet) */
AIL(set_sample_reverb_levels, 12) { return 0; }
AIL(set_sample_loop_count, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) o->loops = (int)ARG(1);
    return 0;
}
AIL(start_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) start(o); return 0; }
AIL(stop_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) stop(o); return 0; }
AIL(resume_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) resume(o); return 0; }
AIL(end_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) end(o); return 0; }
AIL(sample_status, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); return o ? (uint32_t)status_of(o) : SMP_FREE; }
/* (sample): the byte offset reached in the data */
AIL(sample_position, 4)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (!o)
        return 0;
    uint32_t f = played_frames(o), ch = (uint32_t)channels_of(o);
    if (o->type & DIG_F_ADPCM) {
        uint32_t per = ima_per_block(o->block, ch);
        return per ? f / per * o->block + (f % per) * ch / 2 : 0;
    }
    return f * ch * (o->type & DIG_F_16BITS ? 2 : 1);
}
/* (sample, &total ms, &current ms) */
AIL(sample_ms_position, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    uint32_t rate = o && o->rate ? o->rate : 22050;
    uint32_t tot = o ? (uint32_t)((uint64_t)total_frames(o) * 1000 / rate) : 0;
    uint32_t cur = o ? (uint32_t)((uint64_t)played_frames(o) * 1000 / rate) : 0;
    if (ARG(1)) W32(ARG(1), tot);
    if (ARG(2)) W32(ARG(2), cur);
    return 0;
}

/* 3D providers, in the game's order of preference (0x792818) */
static const char *const provider_names[] = {
    "Creative Labs EAX 3 (TM)", "Creative Labs EAX 2 (TM)", "Creative Labs EAX (TM)",
    "Miles Fast 2D Positional Audio"};
static uint32_t provider_handles[4], provider_strs[4];

/* (&next, &provider, &name): 0 when there are no more */
AIL(enumerate_3D_providers, 12)
{
    uint32_t k = ARG(0) ? R32(ARG(0)) : 0;
    if (k >= sizeof provider_names / sizeof *provider_names)
        return 0;
    if (!provider_handles[k]) {
        provider_handles[k] = obj_new(K_PROVIDER);
        provider_strs[k] = gstrdup(provider_names[k]);
    }
    if (ARG(1)) W32(ARG(1), provider_handles[k]);
    if (ARG(2)) W32(ARG(2), provider_strs[k]);
    if (ARG(0)) W32(ARG(0), k + 1);
    return 1;
}
AIL(open_3D_provider, 4)
{
    LOGS("AIL_open_3D_provider(%08x)", ARG(0));
    return obj_get(ARG(0), K_PROVIDER) ? 0 : 1;        /* M3D_NOERR / M3D_NOTFOUND */
}
/* (provider, name, &value) */
AIL(3D_provider_attribute, 12)
{
    const char *n = gstr(ARG(1));
    if (ARG(2)) W32(ARG(2), n && !strcasecmp(n, "Maximum supported samples") ? 64 : 0);
    return 0;
}
AIL(set_3D_provider_preference, 12) { return 0; }
AIL(3D_speaker_type, 4) { return 0; }                   /* AIL_3D_2_SPEAKER */
AIL(set_3D_room_type, 8) { return 0; }
AIL(set_3D_distance_factor, 8) { return 0; }
AIL(set_3D_rolloff_factor, 8)
{
    float r = fl(ARG(1));
    rolloff = r >= 0 && r < 100 ? r : 1.0f;
    return 0;
}
AIL(open_3D_listener, 4)
{
    uint32_t h = obj_new(K_LISTENER);
    listener = (int)R32(h + 4);
    return h;
}
AIL(close_3D_listener, 4)
{
    Obj *o = obj_get(ARG(0), K_LISTENER);
    if (o) {
        if (listener == (int)R32(ARG(0) + 4))
            listener = -1;
        release(o);
    }
    return 0;
}

/* 3D samples */
AIL(allocate_3D_sample_handle, 4) { return obj_new(K_3DSAMPLE); }
AIL(release_3D_sample_handle, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) release(o); return 0; }
/* (sample, AILSOUNDINFO *): format, data, length, rate, bits, channels, samples, block size,
   initial pointer. 1 if it can play it. */
AIL(set_3D_sample_info, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    uint32_t i = ARG(1);
    if (!o || !i)
        return 0;
    end(o);
    uint32_t fmt = R32(i), bits = R32(i + 16), ch = R32(i + 20);
    o->addr = R32(i + 4);
    o->len = R32(i + 8);
    o->rate = R32(i + 12);
    o->block = R32(i + 28);
    if (fmt == 1 && (bits == 8 || bits == 16) && (ch == 1 || ch == 2))
        o->type = (bits == 16 ? DIG_F_16BITS : 0) | (ch == 2 ? DIG_F_STEREO : 0);
    else if (fmt == 0x11 && (ch == 1 || ch == 2) && o->block)
        o->type = DIG_F_ADPCM | DIG_F_16BITS | (ch == 2 ? DIG_F_STEREO : 0);
    else {
        LOGS("AIL_set_3D_sample_info: format %u, %u bits, %u ch not supported", fmt, bits, ch);
        set_error("unsupported format");
        o->addr = o->len = 0;
        return 0;
    }
    return 1;
}
AIL(set_3D_sample_playback_rate, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) { o->rate = ARG(1); if (o->voice >= 0 && o->rate) voice_set_rate(o->voice, o->rate); }
    return 0;
}
AIL(set_3D_sample_volume, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) { o->volume = fl(ARG(1)); apply_gain(o); }
    return 0;
}
AIL(set_3D_sample_loop_count, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) o->loops = (int)ARG(1);
    return 0;
}
/* (sample, max, min) */
AIL(set_3D_sample_distances, 12)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) { o->maxd = fl(ARG(1)); o->mind = fl(ARG(2)); apply_gain(o); }
    return 0;
}
/* (sample, name, &value): EAX settings (not heard) */
AIL(set_3D_sample_preference, 12) { return 0; }
AIL(start_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) start(o); return 0; }
AIL(stop_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) stop(o); return 0; }
AIL(resume_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) resume(o); return 0; }
AIL(end_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) end(o); return 0; }
AIL(3D_sample_status, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); return o ? (uint32_t)status_of(o) : SMP_FREE; }
/* (object, x, y, z): a 3D sample or the listener */
AIL(set_3D_position, 16)
{
    Obj *o = obj_get(ARG(0), 0);
    if (o) {
        o->pos[0] = fl(ARG(1)); o->pos[1] = fl(ARG(2)); o->pos[2] = fl(ARG(3));
        if (o->kind == K_3DSAMPLE) apply_gain(o);
    }
    return 0;
}
/* (object, front x y z, up x y z) */
AIL(set_3D_orientation, 28)
{
    Obj *o = obj_get(ARG(0), 0);
    if (o)
        for (int k = 0; k < 3; k++) { o->front[k] = fl(ARG(1 + k)); o->up[k] = fl(ARG(4 + k)); }
    return 0;
}

/* ------------------------------------------------------------------ files and decompression */
/* (file name, destination): -1 = allocate, the size in the first dword and the data after it
   (FILE_READ_WITH_SIZE); NULL = allocate, data only */
AIL(file_read, 8)
{
    char host[1200];
    const char *win = gstr(ARG(0));
    FILE *f = win && path_to_host(win, host, sizeof host, 0) ? fopen(host, "rb") : NULL;
    if (!f) {
        LOGS("AIL_file_read(%s): not found", win ? win : "NULL");
        set_error("file not found");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t dst = ARG(1), data;
    if (dst == 0xffffffffu) {
        dst = galloc((uint32_t)n + 4);
        W32(dst, (uint32_t)n);
        data = dst + 4;
    } else {
        if (!dst)
            dst = galloc((uint32_t)n ? (uint32_t)n : 1);
        data = dst;
    }
    size_t got = fread(gptr(data), 1, (size_t)n, f);
    fclose(f);
    LOGS("AIL_file_read(%s): %ld bytes at %08x", win, n, dst);
    (void)got;
    return dst;
}

/* (AILSOUNDINFO *, &out, &out size): IMA ADPCM to a 16-bit PCM WAV image (allocated) */
AIL(decompress_ADPCM, 12)
{
    uint32_t i = ARG(0);
    uint32_t fmt = R32(i), data = R32(i + 4), len = R32(i + 8), rate = R32(i + 12), ch = R32(i + 20),
             block = R32(i + 28);
    if (fmt != 0x11 || !block || (ch != 1 && ch != 2) || !data) {
        set_error("not IMA ADPCM");
        return 0;
    }
    uint32_t cap = ima_frames(len, block, ch);
    int16_t *pcm = malloc((size_t)cap * ch * 2 + 16);
    uint32_t frames = ima_decode((const uint8_t *)gptr(data), len, block, ch, pcm);
    uint32_t bytes = frames * ch * 2, out = galloc(44 + bytes);
    uint8_t *w = (uint8_t *)gptr(out);
    uint32_t v;
    memcpy(w, "RIFF", 4); v = 36 + bytes; memcpy(w + 4, &v, 4); memcpy(w + 8, "WAVEfmt ", 8);
    v = 16; memcpy(w + 16, &v, 4);
    uint16_t s = 1; memcpy(w + 20, &s, 2); s = (uint16_t)ch; memcpy(w + 22, &s, 2);
    memcpy(w + 24, &rate, 4); v = rate * ch * 2; memcpy(w + 28, &v, 4);
    s = (uint16_t)(ch * 2); memcpy(w + 32, &s, 2); s = 16; memcpy(w + 34, &s, 2);
    memcpy(w + 36, "data", 4); memcpy(w + 40, &bytes, 4);
    memcpy(w + 44, pcm, bytes);
    free(pcm);
    if (ARG(1)) W32(ARG(1), out);
    if (ARG(2)) W32(ARG(2), 44 + bytes);
    return 1;
}
