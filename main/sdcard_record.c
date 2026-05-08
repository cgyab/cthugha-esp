// SD card AVI recording for Cthugha ESP32-P4.
//
// Records the 240x240 indexed framebuffer as an 8-bit palettised AVI file.
// Files are written to /sdcard/cthNNNN.avi, numbered 0001..MAX_RECORDINGS.
// When the count exceeds MAX_RECORDINGS, numbering wraps and old files are
// overwritten.
//
// Format: RIFF AVI / BI_RGB / 8bpp / top-down / palette snapshot at start.
// At 57,600 bytes per frame (vs 172,800 for BGR24) the SD write load is 3x
// lower, which is the limiting factor on consumer SD cards.
//
// Convert for YouTube:
//   ffmpeg -i cth0001.avi -vf scale=720:720 out.mp4

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_heap_caps.h"
#include "cthugha.h"
#include "sdcard_record.h"

static const char *TAG = "sdcard_rec";

#define MOUNT_POINT "/sdcard"
#define FRAME_W     BUFF_WIDTH
#define FRAME_H     BUFF_HEIGHT
#define FRAME_SIZE  BUFF_SIZE       // 8-bit indexed: 57,600 bytes per frame
#define RECORD_FPS  CONFIG_CTHUGHA_SDCARD_FPS
// Record every Nth render frame so recording FPS ~ RECORD_FPS at 60 Hz render.
#define RECORD_STRIDE (60 / RECORD_FPS)

// 8-bit AVI header is larger than BGR24 because strf includes the 1024-byte
// RGBQUAD palette.  Offsets to patch at stop time:
#define AVI_OFF_RIFF_SIZE    4
#define AVI_OFF_TOTAL_FRAMES 48
#define AVI_OFF_STRH_LENGTH  140
#define AVI_OFF_MOVI_SIZE    1240   // movi LIST size field (header = 1248 bytes)

// movi LIST 'LIST' tag at offset 1236, 'movi' fourcc at 1244.
// Frame N chunk at: 1248 + N*CHUNK_SIZE
// idx1 dwChunkOffset = frame_pos - 1244 = 4 + N*CHUNK_SIZE
#define FIRST_FRAME_POS 1248u

// ---

static sdmmc_card_t *sd_card = NULL;
static bool          mounted  = false;

static FILE             *avi_file       = NULL;
static int               frame_count    = 0;
static int               frames_dropped = 0;
static int               render_count   = 0;  // every render call while active
static int               render_tick    = 0;
static bool              active         = false;
static uint8_t           rec_palette[LUTSIZE];  // palette snapshot at recording start

// Ring buffer pool: NUM_BUFS PSRAM frames flow between two queues.
// Each buffer is CHUNK_SIZE bytes: 8-byte AVI chunk header pre-filled
// at allocation time, followed by FRAME_SIZE bytes of indexed pixel data.
// The writer issues a single fwrite(buf, CHUNK_SIZE) per frame.
#define NUM_BUFS   CONFIG_CTHUGHA_SDCARD_NUM_BUFS
#define CHUNK_SIZE (FRAME_SIZE + 8)   // "00dc" + LE32 size + 8-bit pixels
static uint8_t       *frame_pool[16];  // max 16, actual count = NUM_BUFS
static QueueHandle_t  free_queue  = NULL;
static QueueHandle_t  write_queue = NULL;
static TaskHandle_t   sd_task_h   = NULL;

// --- Little-endian write helpers ---

static void wle32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    fwrite(b, 1, 4, f);
}

static void wle16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v>>8) };
    fwrite(b, 1, 2, f);
}

static void wcc(FILE *f, const char *cc)  // write 4-byte fourcc
{
    fwrite(cc, 1, 4, f);
}

static void patch32(FILE *f, long offset, uint32_t v)
{
    long saved = ftell(f);
    fseek(f, offset, SEEK_SET);
    wle32(f, v);
    fseek(f, saved, SEEK_SET);
}

// --- AVI header (1248 bytes) ---
// 8-bit palettised format.  The strf chunk includes a 256-entry RGBQUAD
// palette snapshotted from LUTbuffer at recording start.
// Offsets 4/48/140/1240 are patched at stop time.

static void write_avi_header(FILE *f)
{
    const uint32_t fps            = RECORD_FPS;
    const uint32_t usec_per_frame = 1000000u / fps;
    const uint32_t max_bps        = FRAME_SIZE * fps;

    // strf chunk = BITMAPINFOHEADER(40) + RGBQUAD[256](1024) = 1064 bytes
    const uint32_t strf_data_sz = 40u + 1024u;
    // strl content = 'strl'(4) + strh_chunk(64) + strf_chunk(8+1064) = 1140
    const uint32_t strl_sz = 4u + 64u + (8u + strf_data_sz);
    // hdrl content = 'hdrl'(4) + avih_chunk(64) + LIST_strl(8+strl_sz) = 1216
    const uint32_t hdrl_sz = 4u + 64u + (8u + strl_sz);

    // RIFF 'AVI '
    wcc(f, "RIFF");  wle32(f, 0);           // offset 4: patched at stop
    wcc(f, "AVI ");

    // LIST 'hdrl'
    wcc(f, "LIST");  wle32(f, hdrl_sz);
    wcc(f, "hdrl");

    // 'avih' (MainAVIHeader, 56 bytes)  — starts at file offset 24
    wcc(f, "avih");  wle32(f, 56);
    wle32(f, usec_per_frame);
    wle32(f, max_bps);
    wle32(f, 0);             // padding granularity
    wle32(f, 0x10);          // flags: AVIF_HASINDEX
    wle32(f, 0);             // offset 48: total frames — patched at stop
    wle32(f, 0);             // initial frames
    wle32(f, 1);             // streams
    wle32(f, FRAME_SIZE);    // suggested buffer size
    wle32(f, FRAME_W);
    wle32(f, FRAME_H);
    wle32(f, 0); wle32(f, 0); wle32(f, 0); wle32(f, 0);  // reserved

    // LIST 'strl'
    wcc(f, "LIST");  wle32(f, strl_sz);
    wcc(f, "strl");

    // 'strh' (AVIStreamHeader, 56 bytes)  — starts at file offset 100
    wcc(f, "strh");  wle32(f, 56);
    wcc(f, "vids");          // fccType
    wle32(f, 0);             // fccHandler (uncompressed)
    wle32(f, 0);             // flags
    wle16(f, 0);             // priority
    wle16(f, 0);             // language
    wle32(f, 0);             // initial frames
    wle32(f, 1);             // scale
    wle32(f, fps);           // rate
    wle32(f, 0);             // start
    wle32(f, 0);             // offset 140: length — patched at stop
    wle32(f, FRAME_SIZE);    // suggested buffer size
    wle32(f, 0xFFFFFFFF);    // quality
    wle32(f, 0);             // sample size
    wle16(f, 0); wle16(f, 0);
    wle16(f, FRAME_W); wle16(f, FRAME_H);

    // 'strf' (BITMAPINFOHEADER + RGBQUAD palette)  — starts at file offset 164
    wcc(f, "strf");  wle32(f, strf_data_sz);
    wle32(f, 40);            // biSize
    wle32(f, FRAME_W);       // biWidth
    wle32(f, (uint32_t)(-((int32_t)FRAME_H)));  // negative = top-down
    wle16(f, 1);             // biPlanes
    wle16(f, 8);             // biBitCount (8-bit palettised)
    wle32(f, 0);             // biCompression (BI_RGB)
    wle32(f, FRAME_SIZE);    // biSizeImage
    wle32(f, 0);             // biXPelsPerMeter
    wle32(f, 0);             // biYPelsPerMeter
    wle32(f, 256);           // biClrUsed
    wle32(f, 0);             // biClrImportant

    // RGBQUAD[256] palette — snapshotted from LUTbuffer at recording start.
    // RGBQUAD order: Blue, Green, Red, Reserved.
    for (int i = 0; i < 256; i++) {
        fputc(rec_palette[i * 3 + 2], f);  // B
        fputc(rec_palette[i * 3 + 1], f);  // G
        fputc(rec_palette[i * 3 + 0], f);  // R
        fputc(0, f);                        // reserved
    }

    // LIST 'movi'  — starts at file offset 1236
    wcc(f, "LIST");  wle32(f, 0);    // offset 1240: patched at stop
    wcc(f, "movi");
    // file position is now 1248 — first '00dc' chunk written here
}

// --- Background SD writer task ---
// Drains write_queue: writes each frame to the AVI file then returns
// the buffer to free_queue so the render task can reuse it.

static void sd_writer_task(void *arg)
{
    uint8_t *buf;
    while (1) {
        xQueueReceive(write_queue, &buf, portMAX_DELAY);
        if (avi_file) {
            size_t written = fwrite(buf, 1, CHUNK_SIZE, avi_file);
            if (written != CHUNK_SIZE) {
                ESP_LOGE(TAG, "Write failed (%u/%u) — disk full? Stopping.", written, CHUNK_SIZE);
                active = false;  // render task will not queue more frames
            }
        }
        xQueueSend(free_queue, &buf, portMAX_DELAY);
    }
}

// --- File numbering ---
// Scans /sdcard once (at first call) to find the highest existing cthNNNN.avi,
// then increments an in-memory counter.  Uses tolower() so the match works
// regardless of whether FAT32 returns filenames in upper or lower case.

static int s_next_num = -1;  // -1 = not yet initialised

static int next_file_number(void)
{
    if (s_next_num < 0) {
        s_next_num = 1;
        DIR *dir = opendir(MOUNT_POINT);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                char lower[32] = {0};
                for (int i = 0; i < (int)sizeof(lower) - 1 && ent->d_name[i]; i++)
                    lower[i] = (char)tolower((unsigned char)ent->d_name[i]);
                int n = 0;
                if (sscanf(lower, "cth%d.avi", &n) == 1 && n >= s_next_num)
                    s_next_num = n + 1;
            }
            closedir(dir);
            if (s_next_num > CONFIG_CTHUGHA_SDCARD_MAX_RECORDINGS)
                s_next_num = 1;
        }
    }

    int num = s_next_num++;
    if (s_next_num > CONFIG_CTHUGHA_SDCARD_MAX_RECORDINGS)
        s_next_num = 1;
    return num;
}

// --- Public API ---

void sdcard_record_init(void)
{
    // On ESP32-P4, SDMMC IO voltage is an external supply managed via sd_pwr_ctrl.
    // The driver acquires the on-chip LDO and the host applies it automatically.
    sd_pwr_ctrl_ldo_config_t pwr_cfg = {
        .ldo_chan_id = CONFIG_CTHUGHA_SDCARD_LDO_CHAN,
    };
    sd_pwr_ctrl_handle_t pwr_handle = NULL;
    esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&pwr_cfg, &pwr_handle);
    if (pwr_err != ESP_OK) {
        ESP_LOGW(TAG, "SD power ctrl init failed (%s) -- recording disabled",
                 esp_err_to_name(pwr_err));
        return;
    }

    // SDMMC_HOST_DEFAULT() selects Slot 1 (GPIO Matrix), which is correct for
    // this board.  Slot 0 is reserved for UHS-I.  SDMMC_SLOT_CONFIG_DEFAULT()
    // for ESP32-P4 already contains the right pin numbers (CLK=43, CMD=44,
    // D0-D3=GPIO39-42) per the Waveshare board wiring and Espressif defaults.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz    = SDMMC_FREQ_DEFAULT;   // 20 MHz — more reliable across cards
    host.pwr_ctrl_handle = pwr_handle;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width  = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 32 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mnt, &sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) — recording disabled", esp_err_to_name(err));
        return;
    }
    mounted = true;
    ESP_LOGI(TAG, "SD card mounted: %s %.1f GB",
             sd_card->cid.name,
             (double)((uint64_t)sd_card->csd.capacity * sd_card->csd.sector_size) / 1e9);

    free_queue  = xQueueCreate(NUM_BUFS, sizeof(uint8_t *));
    write_queue = xQueueCreate(NUM_BUFS, sizeof(uint8_t *));
    if (!free_queue || !write_queue) {
        ESP_LOGE(TAG, "Failed to create frame queues");
        mounted = false;
        return;
    }

    for (int i = 0; i < NUM_BUFS; i++) {
        frame_pool[i] = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
        if (!frame_pool[i]) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer %d in PSRAM", i);
            mounted = false;
            return;
        }
        // Pre-fill the AVI chunk header — it never changes.
        memcpy(frame_pool[i], "00dc", 4);
        uint32_t sz = FRAME_SIZE;
        frame_pool[i][4] = (uint8_t)(sz);
        frame_pool[i][5] = (uint8_t)(sz >> 8);
        frame_pool[i][6] = (uint8_t)(sz >> 16);
        frame_pool[i][7] = (uint8_t)(sz >> 24);
        uint8_t *p = frame_pool[i];
        xQueueSend(free_queue, &p, 0);
    }

    xTaskCreatePinnedToCore(sd_writer_task, "sd_writer", 4096, NULL, 3,
                            &sd_task_h, 1);

    ESP_LOGI(TAG, "Recording ready. Toggle with four-finger tap while locked.");
}

void sdcard_record_toggle(void)
{
    if (!mounted) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }

    if (!active) {
        // Start recording
        int num = next_file_number();
        char path[32];
        snprintf(path, sizeof(path), MOUNT_POINT "/cth%04d.avi", num);

        avi_file = fopen(path, "wb");
        if (!avi_file) {
            ESP_LOGE(TAG, "Cannot open %s for writing", path);
            return;
        }
        memcpy(rec_palette, LUTbuffer, LUTSIZE);  // palette snapshot
        write_avi_header(avi_file);
        frame_count    = 0;
        frames_dropped = 0;
        render_count   = 0;
        render_tick    = 0;
        active         = true;
        ESP_LOGI(TAG, "Recording started: %s (max %d frames @ %d fps)",
                 path, CONFIG_CTHUGHA_SDCARD_MAX_FRAMES, RECORD_FPS);

    } else {
        // Stop recording — wait until every buffer has been returned to free_queue.
        // This is the only correct drain: all NUM_BUFS buffers back in free_queue
        // means the writer has returned from every fwrite() call.
        active = false;
        {
            uint8_t *drained[16];
            int n = 0;
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
            while (n < NUM_BUFS) {
                if (xQueueReceive(free_queue, &drained[n], pdMS_TO_TICKS(500)) == pdTRUE) {
                    n++;
                } else if (xTaskGetTickCount() > deadline) {
                    ESP_LOGW(TAG, "Drain timeout (%d/%d buffers)", n, NUM_BUFS);
                    break;
                }
            }
            for (int i = 0; i < n; i++)
                xQueueSend(free_queue, &drained[i], 0);
        }

        // Write idx1 index — offsets are deterministic so no bookkeeping needed.
        uint32_t chunk_sz = (uint32_t)frame_count * 16;
        wcc(avi_file, "idx1");
        wle32(avi_file, chunk_sz);
        for (int i = 0; i < frame_count; i++) {
            wcc(avi_file, "00dc");
            wle32(avi_file, 0x10);  // AVIIF_KEYFRAME
            wle32(avi_file, (uint32_t)(4 + (uint32_t)i * CHUNK_SIZE));
            wle32(avi_file, FRAME_SIZE);
        }

        // Patch header fields.
        long total = ftell(avi_file);
        uint32_t movi_data_sz = 4u + (uint32_t)frame_count * CHUNK_SIZE;
        patch32(avi_file, AVI_OFF_RIFF_SIZE,    (uint32_t)(total - 8));
        patch32(avi_file, AVI_OFF_TOTAL_FRAMES, (uint32_t)frame_count);
        patch32(avi_file, AVI_OFF_STRH_LENGTH,  (uint32_t)frame_count);
        patch32(avi_file, AVI_OFF_MOVI_SIZE,    movi_data_sz);

        fclose(avi_file);
        avi_file = NULL;

        ESP_LOGI(TAG, "Recording stopped: %d frames, %.1f s, %.1f MB",
                 frame_count,
                 (float)frame_count / RECORD_FPS,
                 (float)frame_count * FRAME_SIZE / 1e6f);
    }
}

bool sdcard_record_is_active(void)
{
    return active;
}

void sdcard_record_frame(const uint8_t *indexed_buf)
{
    if (!active) return;

    // Progress log every 300 render frames (~5 s at 60 fps) regardless of drops.
    if (++render_count % 300 == 0)
        ESP_LOGI(TAG, "Recording: %d/%d frames captured, %d dropped",
                 frame_count, CONFIG_CTHUGHA_SDCARD_MAX_FRAMES, frames_dropped);

    if (++render_tick < RECORD_STRIDE) return;
    render_tick = 0;

    if (frame_count >= CONFIG_CTHUGHA_SDCARD_MAX_FRAMES) {
        sdcard_record_toggle();  // auto-stop
        return;
    }

    // Grab a free buffer; if all are in use (writer falling behind) drop the frame.
    uint8_t *buf;
    if (xQueueReceive(free_queue, &buf, 0) != pdTRUE) {
        frames_dropped++;
        return;
    }

    memcpy(buf + 8, indexed_buf, FRAME_SIZE);  // 8-bit indexed, no conversion needed
    frame_count++;

    xQueueSend(write_queue, &buf, portMAX_DELAY);
}
