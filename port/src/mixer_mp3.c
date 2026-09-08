#include "mixer_mp3.h"

#ifdef PD_PSP_AUDIO_ME
#include <string.h>
#include "mixer_cmd.h"
#include "romdata.h"
#include "external/minimp3.h"

#define MP3_STAGE_BYTES (32 * 1024)
#define MP3_FRAME_BYTES 2304 /* minimp3's maximum free-format frame size */
#define MP3_PACKETS_PER_LIST 8
#define MP3_PCM_SAMPLES 580

/* Command-list slots are recycled only after their ME jobs complete. Every
 * packet is immutable from submission until completion. */
typedef struct __attribute__((aligned(64))) {
    u32 size;
    int reset;
    int free_format_bytes;
    u8 bytes[MP3_FRAME_BYTES];
} MixerMp3Packet;
static MixerMp3Packet g_Mp3Packets[AMGR_CMDLIST_COUNT][MP3_PACKETS_PER_LIST];
static u32 g_Mp3ListSlot;
static u32 g_Mp3PacketCount;

/* Allegrex owns the reader and header scanner. No filesystem calls run on ME. */
static struct __attribute__((aligned(64))) {
    mp3dec_t scanner;
    const void *source;
    u32 size, read_offset, head, count;
    int reset_pending;
    u8 bytes[MP3_STAGE_BYTES];
} g_Mp3Reader;

/* ME owns synthesis state and PCM. Align/pad the entire objects so main-CPU
 * parser writes cannot share a dirty cache line with decoded output. Two
 * contiguous mp3thing blocks per slot preserve the legacy channel addressing. */
static struct __attribute__((aligned(64))) {
    mp3dec_t decoder;
    mp3d_sample_t scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];
} g_Mp3Decoder;
static struct __attribute__((aligned(64))) {
    s16 samples[2 * MP3_PCM_SAMPLES];
} g_Mp3Pcm[6];

void *mixerMp3OutputBuffer(u32 slot)
{
    return g_Mp3Pcm[slot % 6].samples;
}

void mixerMp3BeginList(u32 slot)
{
    g_Mp3ListSlot = slot % AMGR_CMDLIST_COUNT;
    g_Mp3PacketCount = 0;
}

static void mixerMp3Refill(void)
{
    if (g_Mp3Reader.head) {
        memmove(g_Mp3Reader.bytes, g_Mp3Reader.bytes + g_Mp3Reader.head, g_Mp3Reader.count);
        g_Mp3Reader.head = 0;
    }
    while (g_Mp3Reader.count < MP3_STAGE_BYTES && g_Mp3Reader.read_offset < g_Mp3Reader.size) {
        u32 count = g_Mp3Reader.size - g_Mp3Reader.read_offset;
        if (count > MP3_STAGE_BYTES - g_Mp3Reader.count) count = MP3_STAGE_BYTES - g_Mp3Reader.count;
        s32 got;
        if (ROMPTR_IS_FILE((romptr_t)g_Mp3Reader.source)) {
            got = romdataReadFromRom(ROMPTR_TO_OFFSET((romptr_t)g_Mp3Reader.source) +
                g_Mp3Reader.read_offset, g_Mp3Reader.bytes + g_Mp3Reader.count, count);
        } else {
            memcpy(g_Mp3Reader.bytes + g_Mp3Reader.count,
                (const u8 *)g_Mp3Reader.source + g_Mp3Reader.read_offset, count);
            got = (s32)count;
        }
        if (got <= 0) break;
        g_Mp3Reader.count += (u32)got;
        g_Mp3Reader.read_offset += (u32)got;
    }
}

static void mixerMp3Stage(MixerMp3Packet *packet, const void *source, u32 size, int reset)
{
    if (reset || source != g_Mp3Reader.source || size != g_Mp3Reader.size) {
        g_Mp3Reader.source = source;
        g_Mp3Reader.size = size;
        g_Mp3Reader.read_offset = g_Mp3Reader.head = g_Mp3Reader.count = 0;
        g_Mp3Reader.reset_pending = 1;
        mp3dec_init(&g_Mp3Reader.scanner);
    }
    packet->size = 0;
    packet->reset = g_Mp3Reader.reset_pending;
    packet->free_format_bytes = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (g_Mp3Reader.count < MP3_STAGE_BYTES / 2) mixerMp3Refill();
        if (!g_Mp3Reader.count) return;
        mp3dec_frame_info_t info = {0};
        const u8 *input = g_Mp3Reader.bytes + g_Mp3Reader.head;
        /* NULL PCM only finds the frame boundary; synthesis happens in Exec. */
        const int samples = mp3dec_decode_frame(&g_Mp3Reader.scanner, input,
            (int)g_Mp3Reader.count, NULL, &info);
        if (samples > 0 && info.frame_bytes > info.frame_offset &&
                (u32)info.frame_bytes <= g_Mp3Reader.count) {
            u32 bytes = (u32)(info.frame_bytes - info.frame_offset);
            if (bytes <= MP3_FRAME_BYTES) {
                memcpy(packet->bytes, input + info.frame_offset, bytes);
                packet->size = bytes;
                /* Frame resynchronization also discards the old bit reservoir. */
                packet->reset = g_Mp3Reader.reset_pending || info.frame_offset != 0;
                packet->free_format_bytes = g_Mp3Reader.scanner.free_format_bytes;
                g_Mp3Reader.reset_pending = 0;
            }
            g_Mp3Reader.head += info.frame_bytes;
            g_Mp3Reader.count -= info.frame_bytes;
            return;
        }
        /* Retain a possible partial frame when scanning past damaged data. */
        u32 skip = g_Mp3Reader.count;
        if (g_Mp3Reader.read_offset < g_Mp3Reader.size) {
            skip = skip > MP3_FRAME_BYTES ? skip - MP3_FRAME_BYTES : 0;
        }
        g_Mp3Reader.head += skip;
        g_Mp3Reader.count -= skip;
        if (skip) {
            g_Mp3Reader.reset_pending = 1;
            packet->reset = 1;
        }
    }
}

void mixerMp3Queue(Acmd *cmd, const void *source, u32 size, void *out, int reset)
{
    cmd->words.w0 = MIXER_CMD_MP3 << 24;
    cmd->words.w1 = (uintptr_t)out;
    /* A PAL job emits at most three 184-sample subframes (normally one MP3
     * frame per channel). Keep a bounded silence fallback for invalid lists. */
    if (g_Mp3PacketCount >= MP3_PACKETS_PER_LIST) {
        mixerCmdSetAux(cmd, 0);
        return;
    }
    MixerMp3Packet *packet = &g_Mp3Packets[g_Mp3ListSlot][g_Mp3PacketCount++];
    mixerMp3Stage(packet, source, size, reset);
    mixerCmdSetAux(cmd, (uintptr_t)packet);
}

void mixerMp3Exec(const void *data, void *out)
{
    const MixerMp3Packet *packet = data;
    s32 samples = 0;
    if (packet) {
        if (packet->reset) mp3dec_init(&g_Mp3Decoder.decoder);
        if (packet->size) {
            mp3dec_frame_info_t info = {0};
            if (packet->free_format_bytes) {
                g_Mp3Decoder.decoder.free_format_bytes = packet->free_format_bytes;
                memcpy(g_Mp3Decoder.decoder.header, packet->bytes, 4);
            }
            samples = mp3dec_decode_frame(&g_Mp3Decoder.decoder, packet->bytes,
                (int)packet->size, g_Mp3Decoder.scratch, &info);
        }
    }
    if (samples > MP3_PCM_SAMPLES) samples = MP3_PCM_SAMPLES;
    if (samples > 0) memcpy(out, g_Mp3Decoder.scratch, samples * sizeof(s16));
    memset((s16 *)out + samples, 0, (MP3_PCM_SAMPLES - samples) * sizeof(s16));
}
#endif
