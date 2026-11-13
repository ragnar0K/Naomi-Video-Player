/*
 * encode_nvp.c - Fast Host Transcoder & Live Stream Encoder for NAOMI Video Player
 *
 * Modes:
 *   1) File mode (for ROMFS rom://movie.nvp):
 *        ./encode_nvp <input_video> <output.nvp> [max_seconds] [fps]
 *   2) Live Stream mode (writes frame packets to stdout for stream_client.py):
 *        ./encode_nvp --stream <input_video> [start_sec] [fps]
 *
 * Block Coding (Full-Resolution 320x240 RGB565, zero 2x2 subsampling):
 *   00 = Unchanged 4x4 block from previous frame (0 bytes)
 *   01 = Solid 4x4 block (1x uint16_t color = 2 bytes)
 *   10 = 2-color 4x4 BTC block (2x uint16_t colors + 16-bit 1bpp mask = 6 bytes)
 *   11 = 4-color 4x4 BTC block (4x uint16_t colors + 32-bit 2bpp mask = 12 bytes)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define POPEN_READ(cmd) _popen((cmd), "rb")
#define PCLOSE(fp)      _pclose(fp)
#else
#define POPEN_READ(cmd) popen((cmd), "r")
#define PCLOSE(fp)      pclose(fp)
#endif

#define VID_W 320
#define VID_H 240
#define BLOCK_W (VID_W / 4)
#define BLOCK_H (VID_H / 4)
#define NUM_BLOCKS (BLOCK_W * BLOCK_H)
#define CTRL_BYTES (NUM_BLOCKS / 4)

#define AUDIO_RATE 16000
/*
 * Yamaha AICA hardware pitch register for 16000 Hz uses OCT=-2, FNS=463:
 *   f_actual = 44100 * 0.25 * (1024 + 463) / 1024 = 16009.40 Hz.
 * Matching the host resample rate and per-frame sample count to the exact
 * AICA DAC rate prevents the AICA playhead from overtaking the ringbuffer
 * write pointer and causing audio lag.
 */
#define AICA_HW_RATE    16009
#define AICA_EXACT_RATE 16009.40
#define AUDIO_LEAD_SEC  0.16

#define NVP_MAGIC 0x3150564E /* "NVP1" */
#define MAX_KEYFRAMES 2048

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t audio_rate;
    uint32_t total_frames;
    uint32_t num_keyframes;
    char     title[64];
} nvp_file_header_t;

typedef struct __attribute__((packed))
{
    uint32_t frame_index;
    uint32_t file_offset;
} nvp_keyframe_entry_t;

typedef struct __attribute__((packed))
{
    uint16_t audio_samples;
    uint16_t flags;
    uint32_t video_bytes;
    uint32_t audio_bytes;
} nvp_frame_header_t;

static inline uint16_t rgb888_to_rgb565(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void rgb565_to_rgb888(uint16_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >> 5)  & 0x3F) << 2;
    *b = (c & 0x1F) << 3;
}

static inline int color_dist_sq(int r0, int g0, int b0, int r1, int g1, int b1)
{
    int dr = r0 - r1;
    int dg = g0 - g1;
    int db = b0 - b1;
    return dr * dr + dg * dg + db * db;
}

static uint32_t encode_frame_btc(
    const uint8_t *rgb24,
    uint8_t *prev_rgb24,
    int has_prev,
    int force_keyframe,
    uint8_t *out_buf
)
{
    uint8_t *ctrl = out_buf;
    memset(ctrl, 0, CTRL_BYTES);
    uint16_t *payload = (uint16_t *)(out_buf + CTRL_BYTES);

    int block_idx = 0;
    for (int by = 0; by < BLOCK_H; by++)
    {
        for (int bx = 0; bx < BLOCK_W; bx++, block_idx++)
        {
            int r[16], g[16], b[16], lum[16];
            int max_diff = 0;
            int sum_r = 0, sum_g = 0, sum_b = 0;
            int min_l = 1000000, max_l = -1;

            int idx = 0;
            for (int py = 0; py < 4; py++)
            {
                int y = by * 4 + py;
                const uint8_t *row = rgb24 + (y * VID_W + bx * 4) * 3;
                const uint8_t *prow = prev_rgb24 + (y * VID_W + bx * 4) * 3;
                for (int px = 0; px < 4; px++, idx++)
                {
                    int pr = row[px * 3 + 0];
                    int pg = row[px * 3 + 1];
                    int pb = row[px * 3 + 2];
                    r[idx] = pr;
                    g[idx] = pg;
                    b[idx] = pb;
                    sum_r += pr;
                    sum_g += pg;
                    sum_b += pb;

                    int l = pr * 2 + pg * 5 + pb;
                    lum[idx] = l;
                    if (l < min_l) min_l = l;
                    if (l > max_l) max_l = l;

                    if (has_prev && !force_keyframe)
                    {
                        int d = color_dist_sq(pr, pg, pb, prow[px * 3 + 0], prow[px * 3 + 1], prow[px * 3 + 2]);
                        if (d > max_diff) max_diff = d;
                    }
                }
            }

            /* 00: Unchanged block from previous frame (tight threshold for crisp video) */
            if (has_prev && !force_keyframe && max_diff <= 36)
            {
                continue;
            }

            /* Copy block to prev_rgb24 when updated */
            for (int py = 0; py < 4; py++)
            {
                int y = by * 4 + py;
                memcpy(
                    prev_rgb24 + (y * VID_W + bx * 4) * 3,
                    rgb24 + (y * VID_W + bx * 4) * 3,
                    12
                );
            }

            /* 01: Check if solid 4x4 block */
            int avg_r = (sum_r + 8) >> 4;
            int avg_g = (sum_g + 8) >> 4;
            int avg_b = (sum_b + 8) >> 4;
            int max_solid_err = 0;
            for (int i = 0; i < 16; i++)
            {
                int err = color_dist_sq(r[i], g[i], b[i], avg_r, avg_g, avg_b);
                if (err > max_solid_err) max_solid_err = err;
            }

            if (max_solid_err <= 64)
            {
                ctrl[block_idx >> 2] |= (1 << ((block_idx & 3) * 2));
                *payload++ = rgb888_to_rgb565(avg_r, avg_g, avg_b);
                continue;
            }

            /* 10: Check 2-color 4x4 BTC block */
            int mid_l = (min_l + max_l) >> 1;
            int r0 = 0, g0 = 0, b0 = 0, cnt0 = 0;
            int r1 = 0, g1 = 0, b1 = 0, cnt1 = 0;
            uint16_t mask2 = 0;

            for (int i = 0; i < 16; i++)
            {
                if (lum[i] > mid_l)
                {
                    mask2 |= (1 << i);
                    r1 += r[i]; g1 += g[i]; b1 += b[i];
                    cnt1++;
                }
                else
                {
                    r0 += r[i]; g0 += g[i]; b0 += b[i];
                    cnt0++;
                }
            }
            if (cnt0 > 0) { r0 /= cnt0; g0 /= cnt0; b0 /= cnt0; }
            else { r0 = avg_r; g0 = avg_g; b0 = avg_b; }
            if (cnt1 > 0) { r1 /= cnt1; g1 /= cnt1; b1 /= cnt1; }
            else { r1 = avg_r; g1 = avg_g; b1 = avg_b; }

            int max_btc2_err = 0;
            for (int i = 0; i < 16; i++)
            {
                int cr = (mask2 & (1 << i)) ? r1 : r0;
                int cg = (mask2 & (1 << i)) ? g1 : g0;
                int cb = (mask2 & (1 << i)) ? b1 : b0;
                int err = color_dist_sq(r[i], g[i], b[i], cr, cg, cb);
                if (err > max_btc2_err) max_btc2_err = err;
            }

            if (max_btc2_err <= 225)
            {
                ctrl[block_idx >> 2] |= (2 << ((block_idx & 3) * 2));
                *payload++ = rgb888_to_rgb565(r0, g0, b0);
                *payload++ = rgb888_to_rgb565(r1, g1, b1);
                *payload++ = mask2;
                continue;
            }

            /*
             * 11: Full-Resolution 4-Color 4x4 BTC Block (12 bytes: 4x uint16_t + uint32_t mask)
             * Every single pixel of the 16 pixels gets its own 2-bit palette index!
             */
            ctrl[block_idx >> 2] |= (3 << ((block_idx & 3) * 2));

            int span = max_l - min_l;
            int q1 = min_l + (span >> 2);
            int q2 = min_l + (span >> 1);
            int q3 = min_l + ((span * 3) >> 2);

            int cr[4] = {0}, cg[4] = {0}, cb[4] = {0}, ccnt[4] = {0};
            for (int i = 0; i < 16; i++)
            {
                int k = (lum[i] <= q1) ? 0 : (lum[i] <= q2) ? 1 : (lum[i] <= q3) ? 2 : 3;
                cr[k] += r[i];
                cg[k] += g[i];
                cb[k] += b[i];
                ccnt[k]++;
            }
            for (int k = 0; k < 4; k++)
            {
                if (ccnt[k] > 0)
                {
                    cr[k] /= ccnt[k];
                    cg[k] /= ccnt[k];
                    cb[k] /= ccnt[k];
                }
                else
                {
                    cr[k] = avg_r;
                    cg[k] = avg_g;
                    cb[k] = avg_b;
                }
            }

            /* 1 pass of k-means cluster refinement in RGB space + RGB565 quantization */
            int nr[4] = {0}, ng[4] = {0}, nb[4] = {0}, ncnt[4] = {0};
            for (int i = 0; i < 16; i++)
            {
                int best_k = 0;
                int best_d = 100000000;
                for (int k = 0; k < 4; k++)
                {
                    int d = color_dist_sq(r[i], g[i], b[i], cr[k], cg[k], cb[k]);
                    if (d < best_d) { best_d = d; best_k = k; }
                }
                nr[best_k] += r[i];
                ng[best_k] += g[i];
                nb[best_k] += b[i];
                ncnt[best_k]++;
            }

            uint16_t pal565[4];
            for (int k = 0; k < 4; k++)
            {
                if (ncnt[k] > 0)
                {
                    cr[k] = nr[k] / ncnt[k];
                    cg[k] = ng[k] / ncnt[k];
                    cb[k] = nb[k] / ncnt[k];
                }
                pal565[k] = rgb888_to_rgb565(cr[k], cg[k], cb[k]);
                rgb565_to_rgb888(pal565[k], &cr[k], &cg[k], &cb[k]);
            }

            uint32_t mask4 = 0;
            for (int i = 0; i < 16; i++)
            {
                int best_k = 0;
                int best_d = 100000000;
                for (int k = 0; k < 4; k++)
                {
                    int d = color_dist_sq(r[i], g[i], b[i], cr[k], cg[k], cb[k]);
                    if (d < best_d) { best_d = d; best_k = k; }
                }
                mask4 |= ((uint32_t)best_k << (i * 2));
            }

            payload[0] = pal565[0];
            payload[1] = pal565[1];
            payload[2] = pal565[2];
            payload[3] = pal565[3];
            payload[4] = (uint16_t)(mask4 & 0xFFFF);
            payload[5] = (uint16_t)(mask4 >> 16);
            payload += 6;
        }
    }

    uint32_t total_bytes = (uint32_t)((uint8_t *)payload - out_buf);
    if (total_bytes & 1)
    {
        out_buf[total_bytes++] = 0;
    }
    return total_bytes;
}

int main(int argc, char **argv)
{
    int stream_mode = 0;
    const char *input_video = NULL;
    const char *output_nvp = NULL;
    double start_sec = 0.0;
    int max_seconds = 300;
    int fps = 15;

    if (argc >= 3 && strcmp(argv[1], "--stream") == 0)
    {
        stream_mode = 1;
        input_video = argv[2];
        if (argc >= 4) start_sec = atof(argv[3]);
        if (argc >= 5) fps = atoi(argv[4]);
    }
    else if (argc >= 3)
    {
        input_video = argv[1];
        output_nvp = argv[2];
        if (argc >= 4) max_seconds = atoi(argv[3]);
        if (argc >= 5) fps = atoi(argv[4]);
    }
    else
    {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  File mode:   %s <input_video> <output.nvp> [max_seconds=300] [fps=15]\n", argv[0]);
        fprintf(stderr, "  Stream mode: %s --stream <input_video> [start_sec=0] [fps=15]\n", argv[0]);
        return 1;
    }

    if (fps < 5) fps = 5;
    if (fps > 30) fps = 30;

    char vcmd[2048];
    char acmd[2048];
    if (stream_mode)
    {
        snprintf(
            vcmd, sizeof(vcmd),
            "ffmpeg -v error -ss %.3f -i \"%s\" -vf \"fps=%d,scale=%d:%d:flags=lanczos\" -f rawvideo -pix_fmt rgb24 -",
            start_sec, input_video, fps, VID_W, VID_H
        );
        snprintf(
            acmd, sizeof(acmd),
            "ffmpeg -v error -ss %.3f -i \"%s\" -vn -af \"dynaudnorm=f=150:g=15:p=0.95,volume=6dB,alimiter=limit=0.98\" -ac 2 -ar %d -f s16le -",
            start_sec + AUDIO_LEAD_SEC, input_video, AICA_HW_RATE
        );
    }
    else
    {
        char time_arg[64] = "";
        if (max_seconds > 0)
        {
            snprintf(time_arg, sizeof(time_arg), "-t %d", max_seconds);
        }
        snprintf(
            vcmd, sizeof(vcmd),
            "ffmpeg -v error %s -i \"%s\" -vf \"fps=%d,scale=%d:%d:flags=lanczos\" -f rawvideo -pix_fmt rgb24 -",
            time_arg, input_video, fps, VID_W, VID_H
        );
        snprintf(
            acmd, sizeof(acmd),
            "ffmpeg -v error -ss %.3f %s -i \"%s\" -vn -af \"dynaudnorm=f=150:g=15:p=0.95,volume=6dB,alimiter=limit=0.98\" -ac 2 -ar %d -f s16le -",
            AUDIO_LEAD_SEC, time_arg, input_video, AICA_HW_RATE
        );
    }

    FILE *vpipe = POPEN_READ(vcmd);
    FILE *apipe = POPEN_READ(acmd);
    if (!vpipe || !apipe)
    {
        fprintf(stderr, "Error: failed to launch ffmpeg pipes.\n");
        return 1;
    }

    FILE *out = stdout;
#ifdef _WIN32
    if (stream_mode)
    {
        _setmode(_fileno(stdout), _O_BINARY);
    }
#endif
    nvp_file_header_t fhdr;
    nvp_keyframe_entry_t keyframes[MAX_KEYFRAMES];
    memset(&fhdr, 0, sizeof(fhdr));
    memset(keyframes, 0, sizeof(keyframes));

    if (!stream_mode)
    {
        out = fopen(output_nvp, "wb");
        if (!out)
        {
            perror("fopen output");
            return 1;
        }
        fhdr.magic = NVP_MAGIC;
        fhdr.width = VID_W;
        fhdr.height = VID_H;
        fhdr.fps = (uint16_t)fps;
        fhdr.audio_rate = AUDIO_RATE;

        const char *slash1 = strrchr(input_video, '/');
        const char *slash2 = strrchr(input_video, '\\');
        const char *slash = (slash1 > slash2) ? slash1 : slash2;
        const char *base = slash ? (slash + 1) : input_video;
        strncpy(fhdr.title, base, sizeof(fhdr.title) - 1);

        fwrite(&fhdr, sizeof(fhdr), 1, out);
        fwrite(keyframes, sizeof(nvp_keyframe_entry_t), MAX_KEYFRAMES, out);
    }

    uint8_t *rgb24 = (uint8_t *)malloc(VID_W * VID_H * 3);
    uint8_t *prev_rgb24 = (uint8_t *)calloc(VID_W * VID_H * 3, 1);
    uint8_t *comp_buf = (uint8_t *)malloc(131072);
    uint8_t *audio_buf = (uint8_t *)malloc(16384);

    uint32_t frame_idx = 0;
    uint32_t kf_count = 0;
    double exact_samples_per_frame = AICA_EXACT_RATE / (double)fps;
    double sample_accum = 0.0;
    int keyframe_interval = fps * 3;

    if (!stream_mode)
    {
        printf("Encoding '%s' -> '%s' (320x240 @ %d fps, %d Hz 16-bit stereo)...\n",
               fhdr.title, output_nvp, fps, AUDIO_RATE);
    }

    while (fread(rgb24, 1, VID_W * VID_H * 3, vpipe) == (size_t)(VID_W * VID_H * 3))
    {
        int is_keyframe = (frame_idx % keyframe_interval == 0);
        if (!stream_mode && is_keyframe && kf_count < MAX_KEYFRAMES)
        {
            keyframes[kf_count].frame_index = frame_idx;
            keyframes[kf_count].file_offset = (uint32_t)ftell(out);
            kf_count++;
        }

        sample_accum += exact_samples_per_frame;
        int samples_this_frame = (int)sample_accum;
        /* Keep stereo 16-bit samples a multiple of 2 for AICA 32-bit ringbuffer alignment */
        samples_this_frame &= ~1;
        sample_accum -= samples_this_frame;

        uint32_t audio_bytes = (uint32_t)(samples_this_frame * 4); /* 16-bit stereo = 4 bytes/sample */
        size_t got_audio = fread(audio_buf, 1, audio_bytes, apipe);
        if (got_audio < audio_bytes)
        {
            memset(audio_buf + got_audio, 0, audio_bytes - got_audio);
        }

        uint32_t video_bytes = encode_frame_btc(
            rgb24, prev_rgb24, frame_idx > 0, is_keyframe, comp_buf
        );

        nvp_frame_header_t fr_hdr;
        fr_hdr.audio_samples = (uint16_t)samples_this_frame;
        fr_hdr.flags = (is_keyframe ? 1 : 0) | 2; /* bit 1 = 16-bit stereo audio */
        fr_hdr.video_bytes = video_bytes;
        fr_hdr.audio_bytes = audio_bytes;

        if (fwrite(&fr_hdr, sizeof(fr_hdr), 1, out) != 1 ||
            fwrite(audio_buf, 1, audio_bytes, out) != audio_bytes ||
            fwrite(comp_buf, 1, video_bytes, out) != video_bytes)
        {
            break;
        }

        if (stream_mode)
        {
            fflush(out);
        }
        else if (frame_idx % (fps * 10) == 0 && frame_idx > 0)
        {
            printf("  Encoded %u frames (%u sec, %.1f MB)...\r",
                   frame_idx, frame_idx / fps, (double)ftell(out) / (1024.0 * 1024.0));
            fflush(stdout);
        }

        frame_idx++;
    }

    PCLOSE(vpipe);
    PCLOSE(apipe);

    if (!stream_mode)
    {
        uint32_t total_bytes = (uint32_t)ftell(out);
        fhdr.total_frames = frame_idx;
        fhdr.num_keyframes = kf_count;
        fseek(out, 0, SEEK_SET);
        fwrite(&fhdr, sizeof(fhdr), 1, out);
        fwrite(keyframes, sizeof(nvp_keyframe_entry_t), MAX_KEYFRAMES, out);
        fclose(out);

        printf("\nDone! Encoded %u frames (%u seconds) into %.2f MB.\n",
               frame_idx, frame_idx / fps, (double)total_bytes / (1024.0 * 1024.0));
    }

    free(rgb24);
    free(prev_rgb24);
    free(comp_buf);
    free(audio_buf);
    return 0;
}
