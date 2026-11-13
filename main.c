#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <naomi/video.h>
#include <naomi/ta.h>
#include <naomi/audio.h>
#include <naomi/maple.h>
#include <naomi/cart.h>
#include <naomi/romfs.h>
#include <naomi/dimmcomms.h>
#include <naomi/timer.h>
#include <naomi/system.h>

/*
 * NAOMI Video Player (Live Network Streamer + ROMFS .nvp Player)
 * --------------------------------------------------------------
 * 1) Live Network Streaming Mode:
 *      Run `python3 stream_client.py <NAOMI_IP> <video.mp4>` on your PC at any
 *      time while the player is running on the NAOMI. Streams full-length movies
 *      into a 64-slot ringbuffer in Net DIMM Cartridge RAM (0x04010000) with
 *      live arcade-joystick seeking (-10s/+10s, -60s/+60s)!
 * 2) Embedded ROMFS Mode:
 *      Also plays `rom://movie.nvp` baked into the ROM when no live stream is active.
 */

#define VID_W 320
#define VID_H 240
#define BLOCK_W (VID_W / 4)            /* 80 blocks wide */
#define BLOCK_H (VID_H / 4)            /* 60 blocks high */
#define NUM_BLOCKS (BLOCK_W * BLOCK_H) /* 4800 blocks */
#define CTRL_BYTES (NUM_BLOCKS / 4)    /* 1200 bytes (2 bits per block) */

#define AUDIO_RATE 16000
#define RINGBUF_SAMPLES 4096

#define NVP_MAGIC 0x3150564E           /* "NVP1" */
#define MAX_KEYFRAMES 2048

/* Live Stream Net DIMM Cartridge RAM Layout (64 MB mark) */
#define STREAM_INFO_ADDR    0x04000000
#define STREAM_RING_BASE    0x04010000
#define STREAM_RING_SLOTS   64
#define STREAM_SLOT_STRIDE  0x00010000 /* 64 KB per frame slot */

#define STREAM_MAGIC_ACTIVE 0x5354524D /* "STRM" */
#define STREAM_MAGIC_RESET  0x52534554 /* "RSET" */

typedef struct __attribute__((packed))
{
    uint32_t magic;            /* NVP_MAGIC ("NVP1") or STREAM_MAGIC_ACTIVE ("STRM") */
    uint16_t width;            /* 320 */
    uint16_t height;           /* 240 */
    uint16_t fps;              /* 15 */
    uint16_t audio_rate;       /* 16000 */
    uint32_t total_frames;     /* Total frames in movie */
    uint32_t start_frame;      /* Starting frame offset (for live stream seeking) */
    char     title[64];        /* Null-terminated movie title */
} nvp_file_header_t;

typedef struct __attribute__((packed))
{
    uint32_t frame_index;
    uint32_t file_offset;
} nvp_keyframe_entry_t;

typedef struct __attribute__((packed))
{
    uint16_t audio_samples;    /* Stereo sample pairs in this frame */
    uint16_t flags;            /* bit 0 = keyframe, bit 1 = 16-bit audio */
    uint32_t video_bytes;      /* Compressed video byte length (even) */
    uint32_t audio_bytes;      /* Audio byte length (even) */
} nvp_frame_header_t;

static nvp_keyframe_entry_t g_keyframes[MAX_KEYFRAMES];
static uint16_t g_framebuf[VID_W * VID_H] __attribute__((aligned(32)));
static uint8_t  g_comp_buf[65536] __attribute__((aligned(32)));
static uint8_t  g_audio_buf[16384] __attribute__((aligned(32)));

/* Virtual registers exposed to host PC via dimm_comms_attach_hooks */
static volatile uint32_t g_stream_state     = 0;
static volatile uint32_t g_stream_write_seq = 0;
static volatile uint32_t g_stream_read_seq  = 0;
static volatile uint32_t g_stream_seek_req  = 0;

static uint32_t stream_peek_hook(unsigned int address, int size)
{
    if (size == 4)
    {
        uint32_t reg = address & 0xFFFFFF;
        if (reg == 0xC0DE10) return g_stream_state;
        if (reg == 0xC0DE20) return g_stream_write_seq;
        if (reg == 0xC0DE30) return g_stream_read_seq;
        if (reg == 0xC0DE40) return g_stream_seek_req;
    }
    return 0xFFFFFFFF;
}

static void stream_poke_hook(unsigned int address, int size, uint32_t data)
{
    if (size == 4)
    {
        uint32_t reg = address & 0xFFFFFF;
        if (reg == 0xC0DE10) g_stream_state     = data;
        if (reg == 0xC0DE20) g_stream_write_seq = data;
        if (reg == 0xC0DE30) g_stream_read_seq  = data;
        if (reg == 0xC0DE40) g_stream_seek_req  = data;
    }
}

/*
 * Full-Resolution 320x240 4x4 Block Color-Cell + Delta Decoder
 *   00 = Unchanged 4x4 block from previous frame (0 bytes)
 *   01 = Solid 4x4 block (1x uint16_t color = 2 bytes)
 *   10 = 2-color 4x4 BTC block (2x uint16_t colors + 16-bit 1bpp mask = 6 bytes)
 *   11 = 4-color 4x4 BTC block (4x uint16_t colors + 32-bit 2bpp mask = 12 bytes)
 */
static void decode_video_frame(const uint8_t *comp_data, uint32_t comp_len)
{
    if (comp_len < CTRL_BYTES)
    {
        return;
    }

    const uint8_t *ctrl = comp_data;
    const uint16_t *payload = (const uint16_t *)(comp_data + CTRL_BYTES);

    int block_idx = 0;
    for (int by = 0; by < BLOCK_H; by++)
    {
        uint16_t *row_base = &g_framebuf[(by * 4) * VID_W];
        for (int bx = 0; bx < BLOCK_W; bx++, block_idx++)
        {
            uint8_t code = (ctrl[block_idx >> 2] >> ((block_idx & 3) * 2)) & 0x3;
            if (code == 0)
            {
                continue;
            }

            uint16_t *dst0 = row_base + (bx * 4);
            uint16_t *dst1 = dst0 + VID_W;
            uint16_t *dst2 = dst1 + VID_W;
            uint16_t *dst3 = dst2 + VID_W;

            if (code == 1)
            {
                uint16_t c = *payload++;
                uint32_t c2 = ((uint32_t)c << 16) | c;
                ((uint32_t *)dst0)[0] = c2; ((uint32_t *)dst0)[1] = c2;
                ((uint32_t *)dst1)[0] = c2; ((uint32_t *)dst1)[1] = c2;
                ((uint32_t *)dst2)[0] = c2; ((uint32_t *)dst2)[1] = c2;
                ((uint32_t *)dst3)[0] = c2; ((uint32_t *)dst3)[1] = c2;
            }
            else if (code == 2)
            {
                uint16_t c0 = payload[0];
                uint16_t c1 = payload[1];
                uint16_t mask = payload[2];
                payload += 3;

                dst0[0] = (mask & 0x0001) ? c1 : c0;
                dst0[1] = (mask & 0x0002) ? c1 : c0;
                dst0[2] = (mask & 0x0004) ? c1 : c0;
                dst0[3] = (mask & 0x0008) ? c1 : c0;

                dst1[0] = (mask & 0x0010) ? c1 : c0;
                dst1[1] = (mask & 0x0020) ? c1 : c0;
                dst1[2] = (mask & 0x0040) ? c1 : c0;
                dst1[3] = (mask & 0x0080) ? c1 : c0;

                dst2[0] = (mask & 0x0100) ? c1 : c0;
                dst2[1] = (mask & 0x0200) ? c1 : c0;
                dst2[2] = (mask & 0x0400) ? c1 : c0;
                dst2[3] = (mask & 0x0800) ? c1 : c0;

                dst3[0] = (mask & 0x1000) ? c1 : c0;
                dst3[1] = (mask & 0x2000) ? c1 : c0;
                dst3[2] = (mask & 0x4000) ? c1 : c0;
                dst3[3] = (mask & 0x8000) ? c1 : c0;
            }
            else
            {
                const uint16_t *pal = payload;
                uint32_t mask = (uint32_t)payload[4] | ((uint32_t)payload[5] << 16);
                payload += 6;

                dst0[0] = pal[mask & 3];         dst0[1] = pal[(mask >> 2) & 3];
                dst0[2] = pal[(mask >> 4) & 3];  dst0[3] = pal[(mask >> 6) & 3];

                dst1[0] = pal[(mask >> 8) & 3];  dst1[1] = pal[(mask >> 10) & 3];
                dst1[2] = pal[(mask >> 12) & 3]; dst1[3] = pal[(mask >> 14) & 3];

                dst2[0] = pal[(mask >> 16) & 3]; dst2[1] = pal[(mask >> 18) & 3];
                dst2[2] = pal[(mask >> 20) & 3]; dst2[3] = pal[(mask >> 22) & 3];

                dst3[0] = pal[(mask >> 24) & 3]; dst3[1] = pal[(mask >> 26) & 3];
                dst3[2] = pal[(mask >> 28) & 3]; dst3[3] = pal[(mask >> 30) & 3];
            }
        }
    }
}

/* Custom quad draw with optional hardware bilinear filtering */
static void draw_video_quad(texture_description_t *tex, int bilinear, int widescreen)
{
    struct polygon_list_quad mypoly;
    struct vertex_list_quad myvertex;

    mypoly.cmd =
        TA_CMD_SPRITE |
        TA_CMD_POLYGON_TYPE_OPAQUE |
        TA_CMD_POLYGON_SUBLIST |
        TA_CMD_POLYGON_PACKED_COLOR |
        TA_CMD_POLYGON_16BIT_UV |
        TA_CMD_POLYGON_TEXTURED;
    mypoly.mode1 =
        TA_POLYMODE1_Z_GREATEREQUAL |
        TA_POLYMODE1_CULL_DISABLED;
    mypoly.mode2 =
        TA_POLYMODE2_MIPMAP_D_1_00 |
        TA_POLYMODE2_TEXTURE_DECAL |
        tex->uvsize |
        TA_POLYMODE2_TEXTURE_CLAMP_U |
        TA_POLYMODE2_TEXTURE_CLAMP_V |
        TA_POLYMODE2_FOG_DISABLED |
        TA_POLYMODE2_DISABLE_TEX_ALPHA |
        TA_POLYMODE2_SRC_BLEND_ONE |
        TA_POLYMODE2_DST_BLEND_ZERO |
        (bilinear ? (TA_POLYMODE2_BILINEAR_FILTER | TA_POLYMODE2_ENABLE_FILTER) : 0);
    mypoly.texture =
        tex->texture_mode |
        TA_TEXTUREMODE_ADDRESS(tex->vram_location);
    mypoly.mult_color = 0xFFFFFFFF;
    mypoly.add_color = 0;
    ta_commit_list(&mypoly, TA_LIST_SHORT);

    float x0 = 0.0f;
    float x1 = (float)video_width();
    float y0 = widescreen ? 60.0f : 0.0f;
    float y1 = widescreen ? ((float)video_height() - 60.0f) : (float)video_height();
    float u_max = (float)VID_W / 512.0f;
    float v_max = (float)VID_H / 512.0f;

    textured_vertex_t verts[4] = {
        { x0, y1, 10.0f, 0.0f,  v_max },
        { x0, y0, 10.0f, 0.0f,  0.0f  },
        { x1, y0, 10.0f, u_max, 0.0f  },
        { x1, y1, 10.0f, u_max, v_max },
    };

    union { float f; uint32_t i; } f2i_u, f2i_v;

    myvertex.cmd = TA_CMD_VERTEX | TA_CMD_VERTEX_END_OF_STRIP;
    if (video_is_vertical())
    {
        float width = (float)video_width() - 1.0f;
        myvertex.ax = width - verts[0].y; myvertex.ay = verts[0].x; myvertex.az = verts[0].z;
        myvertex.bx = width - verts[1].y; myvertex.by = verts[1].x; myvertex.bz = verts[1].z;
        myvertex.cx = width - verts[2].y; myvertex.cy = verts[2].x; myvertex.cz = verts[2].z;
        myvertex.dx = width - verts[3].y; myvertex.dy = verts[3].x;
    }
    else
    {
        myvertex.ax = verts[0].x; myvertex.ay = verts[0].y; myvertex.az = verts[0].z;
        myvertex.bx = verts[1].x; myvertex.by = verts[1].y; myvertex.bz = verts[1].z;
        myvertex.cx = verts[2].x; myvertex.cy = verts[2].y; myvertex.cz = verts[2].z;
        myvertex.dx = verts[3].x; myvertex.dy = verts[3].y;
    }

    f2i_u.f = verts[0].u; f2i_v.f = verts[0].v;
    myvertex.au_av = (f2i_u.i & 0xFFFF0000) | ((f2i_v.i >> 16) & 0xFFFF);
    f2i_u.f = verts[1].u; f2i_v.f = verts[1].v;
    myvertex.bu_bv = (f2i_u.i & 0xFFFF0000) | ((f2i_v.i >> 16) & 0xFFFF);
    f2i_u.f = verts[2].u; f2i_v.f = verts[2].v;
    myvertex.cu_cv = (f2i_u.i & 0xFFFF0000) | ((f2i_v.i >> 16) & 0xFFFF);

    ta_commit_list(&myvertex, TA_LIST_LONG);
}

static void seek_to_target_frame(
    FILE *fp,
    const nvp_file_header_t *fhdr,
    uint32_t target_frame,
    uint32_t *cur_frame_out
)
{
    if (!fp || fhdr->start_frame == 0)
    {
        return;
    }
    if (target_frame >= fhdr->total_frames)
    {
        target_frame = 0;
    }

    uint32_t best_idx = 0;
    for (uint32_t i = 0; i < fhdr->start_frame; i++)
    {
        if (g_keyframes[i].frame_index <= target_frame)
        {
            best_idx = i;
        }
        else
        {
            break;
        }
    }

    fseek(fp, g_keyframes[best_idx].file_offset, SEEK_SET);
    *cur_frame_out = g_keyframes[best_idx].frame_index;
}

void main()
{
    video_init(VIDEO_COLOR_1555);
    ta_set_background_color(rgb(0, 0, 0));

    /* Attach Net DIMM peek/poke hooks for Live Network Streaming */
    dimm_comms_attach_hooks(&stream_peek_hook, &stream_poke_hook);

    romfs_init_default();

    void *vram_tex = ta_texture_malloc(512, 16);
    texture_description_t *video_tex = ta_texture_desc_direct(vram_tex, 512, TA_TEXTUREMODE_RGB565);

    FILE *fp = fopen("rom://movie.nvp", "rb");
    nvp_file_header_t fhdr;
    memset(&fhdr, 0, sizeof(fhdr));

    int has_movie = 0;
    int live_stream_mode = 0;
    uint32_t data_start_offset = sizeof(nvp_file_header_t) + (sizeof(nvp_keyframe_entry_t) * MAX_KEYFRAMES);

    if (fp)
    {
        if (fread(&fhdr, sizeof(fhdr), 1, fp) == 1 && fhdr.magic == NVP_MAGIC)
        {
            fread(g_keyframes, sizeof(nvp_keyframe_entry_t), MAX_KEYFRAMES, fp);
            has_movie = 1;
        }
    }

    uint32_t fps = (has_movie && fhdr.fps > 0) ? fhdr.fps : 15;
    uint32_t audio_rate = (has_movie && fhdr.audio_rate > 0) ? fhdr.audio_rate : AUDIO_RATE;

    audio_init();
    audio_register_ringbuffer(AUDIO_FORMAT_16BIT, audio_rate, RINGBUF_SAMPLES);

    int bilinear = 1;
    int widescreen = 0;
    int show_osd = 1;
    int paused = 0;
    float volume = 1.0f;
    int osd_timer = 240;

    uint32_t cur_frame = 0;
    uint32_t total_frames = has_movie ? fhdr.total_frames : 1;
    uint32_t duration_sec = total_frames / fps;

    uint64_t frame_accum_us = 0;
    int frame_timer = profile_start();

    while (1)
    {
        uint32_t elapsed_us = profile_end(frame_timer);
        frame_timer = profile_start();
        frame_accum_us += elapsed_us;

        /* Check if host PC initiated or seeked a Live Network Stream */
        if (g_stream_state == STREAM_MAGIC_RESET)
        {
            nvp_file_header_t sinfo;
            cart_read(&sinfo, STREAM_INFO_ADDR, sizeof(sinfo));
            if (sinfo.magic == STREAM_MAGIC_ACTIVE)
            {
                memcpy(&fhdr, &sinfo, sizeof(fhdr));
                fps = (fhdr.fps > 0) ? fhdr.fps : 15;
                audio_rate = (fhdr.audio_rate > 0) ? fhdr.audio_rate : AUDIO_RATE;
                total_frames = (fhdr.total_frames > 0) ? fhdr.total_frames : 1;
                duration_sec = total_frames / fps;
                cur_frame = fhdr.start_frame;
                g_stream_read_seq = 0;
                live_stream_mode = 1;
                has_movie = 1;
                osd_timer = 180;
                frame_accum_us = 0;

                /* Reset AICA ringbuffer playhead & write pointer so lip-sync starts at 32ms */
                audio_register_ringbuffer(AUDIO_FORMAT_16BIT, audio_rate, RINGBUF_SAMPLES);
                audio_change_ringbuffer_volume(volume);
            }
            g_stream_state = STREAM_MAGIC_ACTIVE;
        }

        /* Poll NAOMI arcade controls */
        maple_poll_buttons();
        jvs_buttons_t pressed = maple_buttons_pressed();
        jvs_buttons_t held = maple_buttons_held();

        if (pressed.player1.start || pressed.player1.button1)
        {
            paused = !paused;
            if (!paused)
            {
                audio_register_ringbuffer(AUDIO_FORMAT_16BIT, audio_rate, RINGBUF_SAMPLES);
                audio_change_ringbuffer_volume(volume);
            }
            osd_timer = 180;
        }
        if (has_movie && pressed.player1.left)
        {
            uint32_t step = fps * 10;
            uint32_t target = (cur_frame > step) ? (cur_frame - step) : 0;
            if (live_stream_mode)
            {
                g_stream_seek_req = 0x80000000U | target;
            }
            else
            {
                seek_to_target_frame(fp, &fhdr, target, &cur_frame);
            }
            osd_timer = 180;
        }
        if (has_movie && pressed.player1.right)
        {
            uint32_t target = cur_frame + (fps * 10);
            if (target >= total_frames) target = 0;
            if (live_stream_mode)
            {
                g_stream_seek_req = 0x80000000U | target;
            }
            else
            {
                seek_to_target_frame(fp, &fhdr, target, &cur_frame);
            }
            osd_timer = 180;
        }
        if (has_movie && pressed.player1.button5)
        {
            uint32_t step = fps * 60;
            uint32_t target = (cur_frame > step) ? (cur_frame - step) : 0;
            if (live_stream_mode)
            {
                g_stream_seek_req = 0x80000000U | target;
            }
            else
            {
                seek_to_target_frame(fp, &fhdr, target, &cur_frame);
            }
            osd_timer = 180;
        }
        if (has_movie && pressed.player1.button6)
        {
            uint32_t target = cur_frame + (fps * 60);
            if (target >= total_frames) target = 0;
            if (live_stream_mode)
            {
                g_stream_seek_req = 0x80000000U | target;
            }
            else
            {
                seek_to_target_frame(fp, &fhdr, target, &cur_frame);
            }
            osd_timer = 180;
        }
        if (pressed.player1.button2)
        {
            bilinear = !bilinear;
            osd_timer = 180;
        }
        if (pressed.player1.button3)
        {
            widescreen = !widescreen;
            osd_timer = 180;
        }
        if (pressed.player1.button4 || pressed.player1.service)
        {
            show_osd = !show_osd;
            osd_timer = 240;
        }
        if (held.player1.up)
        {
            volume += 0.01f;
            if (volume > 1.0f) volume = 1.0f;
            audio_change_ringbuffer_volume(volume);
            osd_timer = 120;
        }
        if (held.player1.down)
        {
            volume -= 0.01f;
            if (volume < 0.0f) volume = 0.0f;
            audio_change_ringbuffer_volume(volume);
            osd_timer = 120;
        }

        /* Use exact rational microsecond comparison (avoids 1000000/15 truncation drift) */
        if (has_movie && !paused && (frame_accum_us * fps) >= 1000000ULL)
        {
            frame_accum_us -= (1000000ULL / fps);
            if ((frame_accum_us * fps) >= 2000000ULL)
            {
                frame_accum_us = 0;
            }

            if (live_stream_mode)
            {
                /* Read next frame slot from Net DIMM Cartridge RAM ringbuffer */
                uint32_t rseq = g_stream_read_seq;
                uint32_t wseq = g_stream_write_seq;
                if (rseq < wseq)
                {
                    uint32_t slot_addr = STREAM_RING_BASE + ((rseq & (STREAM_RING_SLOTS - 1)) * STREAM_SLOT_STRIDE);
                    nvp_frame_header_t fr_hdr;
                    cart_read(&fr_hdr, slot_addr, sizeof(fr_hdr));

                    uint32_t cur_addr = slot_addr + sizeof(fr_hdr);
                    if (fr_hdr.audio_bytes > 0 && fr_hdr.audio_bytes <= sizeof(g_audio_buf))
                    {
                        cart_read(g_audio_buf, cur_addr, (fr_hdr.audio_bytes + 1) & ~1U);
                        audio_write_stereo_data(g_audio_buf, fr_hdr.audio_samples);
                        cur_addr += (fr_hdr.audio_bytes + 1) & ~1U;
                    }
                    if (fr_hdr.video_bytes > 0 && fr_hdr.video_bytes <= sizeof(g_comp_buf))
                    {
                        cart_read(g_comp_buf, cur_addr, (fr_hdr.video_bytes + 1) & ~1U);
                        decode_video_frame(g_comp_buf, fr_hdr.video_bytes);
                        ta_texture_load_sprite(vram_tex, 512, 16, 0, 0, VID_W, VID_H, g_framebuf);
                    }
                    g_stream_read_seq = rseq + 1;
                    cur_frame++;
                }
            }
            else if (fp)
            {
                if (cur_frame >= total_frames)
                {
                    fseek(fp, data_start_offset, SEEK_SET);
                    cur_frame = 0;
                }

                nvp_frame_header_t fr_hdr;
                if (fread(&fr_hdr, sizeof(fr_hdr), 1, fp) == 1)
                {
                    if (fr_hdr.audio_bytes > 0 && fr_hdr.audio_bytes <= sizeof(g_audio_buf))
                    {
                        fread(g_audio_buf, 1, fr_hdr.audio_bytes, fp);
                        audio_write_stereo_data(g_audio_buf, fr_hdr.audio_samples);
                    }
                    if (fr_hdr.video_bytes > 0 && fr_hdr.video_bytes <= sizeof(g_comp_buf))
                    {
                        fread(g_comp_buf, 1, fr_hdr.video_bytes, fp);
                        decode_video_frame(g_comp_buf, fr_hdr.video_bytes);
                        ta_texture_load_sprite(vram_tex, 512, 16, 0, 0, VID_W, VID_H, g_framebuf);
                    }
                    cur_frame++;
                }
                else
                {
                    fseek(fp, data_start_offset, SEEK_SET);
                    cur_frame = 0;
                }
            }
        }

        /* Render hardware-scaled video quad via PowerVR2 Tile Accelerator */
        ta_commit_begin();
        draw_video_quad(video_tex, bilinear, widescreen);
        ta_commit_end();
        ta_render();

        /* Draw OSD overlay if enabled or recently interacted */
        if (!has_movie || (show_osd && (paused || osd_timer > 0)))
        {
            if (osd_timer > 0 && !paused)
            {
                osd_timer--;
            }

            video_fill_box(16, 16, 624, 52, rgb(16, 16, 24));
            video_draw_box(16, 16, 624, 52, rgb(0, 200, 255));
            video_draw_debug_text(
                28, 24, rgb(0, 255, 255),
                "NAOMI VIDEO PLAYER %s",
                live_stream_mode ? "[LIVE NETWORK STREAM]" : "[ROMFS]"
            );
            video_draw_debug_text(
                28, 38, rgb(255, 255, 255),
                "Movie: %.54s",
                has_movie ? fhdr.title : "Waiting for stream_client.py..."
            );

            video_fill_box(16, 408, 624, 464, rgb(16, 16, 24));
            video_draw_box(16, 408, 624, 464, rgb(0, 200, 255));

            uint32_t current_sec = cur_frame / fps;
            video_draw_box(28, 418, 460, 430, rgb(180, 180, 180));
            if (duration_sec > 0)
            {
                int fill_w = (int)((430ULL * current_sec) / duration_sec);
                if (fill_w > 430) fill_w = 430;
                if (fill_w > 2)
                {
                    video_fill_box(29, 419, 29 + fill_w, 429, rgb(0, 255, 128));
                }
            }

            video_draw_debug_text(
                470, 420, rgb(255, 255, 0),
                "%02lu:%02lu / %02lu:%02lu %s",
                current_sec / 60, current_sec % 60,
                duration_sec / 60, duration_sec % 60,
                paused ? "[PAUSED]" : ""
            );

            video_draw_debug_text(
                28, 438, rgb(200, 200, 200),
                "Vol:%3d%%   Filter:%s   Aspect:%s   FPS:%lu",
                (int)(volume * 100.0f),
                bilinear ? "Bilinear" : "Nearest",
                widescreen ? "16:9" : "4:3",
                fps
            );
            video_draw_debug_text(
                28, 450, rgb(150, 220, 255),
                "START/B1:Pause  L/R:Seek 10s  B5/B6:Seek 60s  U/D:Vol  B2:Filter  B4:OSD"
            );
        }

        video_display_on_vblank();
    }
}

void test()
{
    video_init(VIDEO_COLOR_1555);
    while (1)
    {
        video_fill_screen(rgb(24, 24, 32));
        video_draw_debug_text(220, 230, rgb(0, 255, 255), "NAOMI VIDEO PLAYER - TEST MODE");
        video_display_on_vblank();
    }
}
