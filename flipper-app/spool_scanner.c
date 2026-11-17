// Spool Scanner — press OK, tap a Bambu Lab filament spool on the Flipper, it decodes and saves it.
// The NFC reader is only on during a scan (OK starts it, it stops after one roll / 15 s / Back).
// Saved list: /ext/apps_data/spool_scanner/spools.csv (synced to Spool Inventory over USB).
#include <furi.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <nfc/nfc.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller_sync.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>
#include <stdio.h>
#include <string.h>

#include "bambu.h"

#define DATA_DIR EXT_PATH("apps_data/spool_scanner")
#define DATA_FILE DATA_DIR "/spools.csv"
#define MAX_KNOWN 800
#define SCAN_SECONDS 15
#define FLAG_SCAN (1 << 0)
#define FLAG_EXIT (1 << 1)

typedef enum {
    ScreenIdle, // reader off
    ScreenScanning, // reader on, waiting for a tag
    ScreenReading,
    ScreenSpool,
    ScreenError,
} Screen;

typedef struct {
    FuriMutex* mutex;
    ViewPort* view_port;
    NotificationApp* notifications;
    Storage* storage;
    Nfc* nfc; // only allocated during a scan
    volatile bool running;
    volatile bool scanning; // cleared by Back to cancel a scan
    uint32_t scan_deadline;

    // Tray UIDs already saved (binary, 16 bytes each) so rescans aren't double-counted.
    uint8_t (*known)[16];
    size_t known_count;

    // Shown on screen (guarded by mutex)
    Screen screen;
    BambuSpool spool;
    bool spool_new;
    char error[48];
} App;

// ---------- storage ----------

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_tray(const char* s, uint8_t out[16]) {
    for(int i = 0; i < 16; i++) {
        int hi = hexval(s[i * 2]), lo = hexval(s[i * 2 + 1]);
        if(hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static void load_known(App* app) {
    app->known_count = 0;
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, DATA_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[40];
        size_t len = 0;
        bool skip = false; // rest of a long line
        uint8_t buf[128];
        size_t n;
        while((n = storage_file_read(f, buf, sizeof(buf))) > 0) {
            for(size_t i = 0; i < n; i++) {
                char c = (char)buf[i];
                if(c == '\n') {
                    if(len >= 32 && app->known_count < MAX_KNOWN &&
                       parse_tray(line, app->known[app->known_count]))
                        app->known_count++;
                    len = 0;
                    skip = false;
                } else if(!skip) {
                    if(len < sizeof(line) - 1)
                        line[len++] = c;
                    else
                        skip = true;
                    if(c == ',') skip = true; // only need the first column
                }
            }
        }
    }
    storage_file_close(f);
    storage_file_free(f);
}

static bool is_known(App* app, const uint8_t tray[16]) {
    for(size_t i = 0; i < app->known_count; i++)
        if(memcmp(app->known[i], tray, 16) == 0) return true;
    return false;
}

static bool save_spool(App* app, const BambuSpool* s) {
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(app->storage, DATA_DIR);
    File* f = storage_file_alloc(app->storage);
    bool ok = storage_file_open(f, DATA_FILE, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok) {
        if(storage_file_size(f) == 0) {
            const char* h = BAMBU_CSV_HEADER;
            storage_file_write(f, h, strlen(h));
        }
        char line[256];
        size_t n = bambu_csv_line(s, furi_hal_rtc_get_timestamp(), line, sizeof(line));
        ok = storage_file_write(f, line, n) == n;
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

// ---------- NFC ----------

typedef enum {
    ReadOk,
    ReadNotBambu,
    ReadLost,
} ReadResult;

static ReadResult read_spool(App* app, const Iso14443_3aData* iso, BambuRaw* raw) {
    memset(raw, 0, sizeof(*raw));
    raw->uid_len = iso->uid_len > sizeof(raw->uid) ? sizeof(raw->uid) : iso->uid_len;
    memcpy(raw->uid, iso->uid, raw->uid_len);

    uint8_t keys[BAMBU_SECTORS][6];
    bambu_derive_keys(raw->uid, raw->uid_len, keys);

    static const uint8_t blocks[BAMBU_BLOCK_COUNT] = BAMBU_BLOCK_LIST;
    for(int i = 0; i < BAMBU_BLOCK_COUNT; i++) {
        uint8_t sector = blocks[i] / 4;
        MfClassicKey key;
        memcpy(key.data, keys[sector], 6);
        MfClassicBlock block;
        MfClassicError err = MfClassicErrorNone;
        // A couple of retries — spools are often moving when tapped.
        for(int attempt = 0; attempt < 3; attempt++) {
            err = mf_classic_poller_sync_read_block(
                app->nfc, blocks[i], &key, MfClassicKeyTypeA, &block);
            if(err == MfClassicErrorNone || err == MfClassicErrorAuth) break;
        }
        if(err == MfClassicErrorAuth) return ReadNotBambu;
        if(err != MfClassicErrorNone) return ReadLost;
        memcpy(raw->blocks[i], block.data, 16);
    }
    return ReadOk;
}

static void show(App* app, Screen screen) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->screen = screen;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

static void show_error(App* app, const char* msg) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strlcpy(app->error, msg, sizeof(app->error));
    app->screen = ScreenError;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
    notification_message(app->notifications, &sequence_error);
}

// One scan session: reader on until a tag is read, the timeout passes, or Back is pressed.
static void scan_once(App* app) {
    Iso14443_3aData iso;
    app->scan_deadline = furi_get_tick() + furi_ms_to_ticks(SCAN_SECONDS * 1000);
    show(app, ScreenScanning);

    while(app->running && app->scanning) {
        if((int32_t)(furi_get_tick() - app->scan_deadline) >= 0) {
            app->scanning = false;
            show_error(app, "No spool found");
            return;
        }
        if(iso14443_3a_poller_sync_read(app->nfc, &iso) != Iso14443_3aErrorNone) {
            view_port_update(app->view_port); // refresh the countdown
            furi_delay_ms(100);
            continue;
        }

        show(app, ScreenReading);
        BambuRaw raw;
        ReadResult r = read_spool(app, &iso, &raw);
        if(r == ReadLost) {
            show(app, ScreenScanning); // tag moved away mid-read: keep trying until the timeout
            continue;
        }
        app->scanning = false;

        BambuSpool spool;
        if(r == ReadNotBambu || !bambu_parse(&raw, &spool)) {
            show_error(app, "Not a Bambu Lab spool tag");
            return;
        }

        uint8_t tray[16];
        memcpy(tray, raw.blocks[4], 16);
        bool is_new = !is_known(app, tray);
        if(is_new) {
            if(!save_spool(app, &spool)) {
                show_error(app, "SD card write failed");
                return;
            }
            if(app->known_count < MAX_KNOWN) memcpy(app->known[app->known_count++], tray, 16);
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->spool = spool;
        app->spool_new = is_new;
        app->screen = ScreenSpool;
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
        notification_message(app->notifications, is_new ? &sequence_success : &sequence_single_vibro);
        return;
    }
    if(app->running) show(app, ScreenIdle); // cancelled with Back
}

static int32_t worker(void* ctx) {
    App* app = ctx;
    while(app->running) {
        uint32_t flags =
            furi_thread_flags_wait(FLAG_SCAN | FLAG_EXIT, FuriFlagWaitAny, FuriWaitForever);
        if(!app->running || (flags & FuriFlagError) || (flags & FLAG_EXIT)) break;
        if(flags & FLAG_SCAN) {
            app->nfc = nfc_alloc(); // reader is only powered during a scan
            scan_once(app);
            nfc_free(app->nfc);
            app->nfc = NULL;
        }
    }
    return 0;
}

// ---------- UI ----------

static void draw(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    char buf[64];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Spool Scanner");
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%u saved", (unsigned)app->known_count);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    const char* hint = "OK = scan   Back = exit";
    switch(app->screen) {
    case ScreenIdle:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignBottom, "Press OK to scan");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 45, AlignCenter, AlignBottom, "NFC reader is off");
        break;
    case ScreenScanning: {
        int32_t left = (int32_t)(app->scan_deadline - furi_get_tick());
        if(left < 0) left = 0;
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignBottom, "Scanning...");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignBottom, "Hold the spool tag to");
        canvas_draw_str_aligned(canvas, 64, 50, AlignCenter, AlignBottom, "the back of the Flipper");
        long ms = (long)left * 1000L / (long)furi_kernel_get_tick_frequency();
        snprintf(buf, sizeof(buf), "%lds", (ms + 999) / 1000);
        canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, buf);
        hint = "Back = cancel";
        break;
    }
    case ScreenReading:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignBottom, "Reading... hold still");
        hint = "";
        break;
    case ScreenSpool: {
        const BambuSpool* s = &app->spool;
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 24, s->detail[0] ? s->detail : s->type);
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            buf, sizeof(buf), "%s  #%02X%02X%02X%s", bambu_color_name(s->rgba), s->rgba[0],
            s->rgba[1], s->rgba[2], s->color_count > 1 ? " +" : "");
        canvas_draw_str(canvas, 2, 34, buf);
        int d100 = (int)(s->diameter_mm * 100.0f + 0.5f);
        snprintf(
            buf, sizeof(buf), "%ug  %d.%02dmm  %um", s->weight_g, d100 / 100, d100 % 100,
            s->length_m);
        canvas_draw_str(canvas, 2, 44, buf);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 55, app->spool_new ? "Saved - new roll" : "Already saved");
        hint = "OK = scan next";
        break;
    }
    case ScreenError:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignBottom, app->error);
        hint = "OK = try again   Back = exit";
        break;
    }
    canvas_set_font(canvas, FontSecondary);
    if(hint[0]) canvas_draw_str(canvas, 2, 63, hint);
    furi_mutex_release(app->mutex);
}

static void input(InputEvent* event, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, event, FuriWaitForever);
}

int32_t spool_scanner_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->known = malloc(MAX_KNOWN * 16);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->screen = ScreenIdle;
    load_known(app);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw, app);
    view_port_input_callback_set(app->view_port, input, queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    app->running = true;
    FuriThread* thread = furi_thread_alloc_ex("SpoolScanWorker", 4096, worker, app);
    furi_thread_start(thread);
    FuriThreadId worker_id = furi_thread_get_id(thread);

    InputEvent ev;
    while(true) {
        if(furi_message_queue_get(queue, &ev, FuriWaitForever) != FuriStatusOk) continue;
        if(ev.type != InputTypeShort) continue;
        if(ev.key == InputKeyBack) {
            if(app->scanning) {
                app->scanning = false; // cancel; worker turns the reader off
                continue;
            }
            break;
        }
        if(ev.key == InputKeyOk && !app->scanning) {
            app->scanning = true;
            furi_thread_flags_set(worker_id, FLAG_SCAN);
        }
    }

    app->running = false;
    app->scanning = false;
    furi_thread_flags_set(worker_id, FLAG_EXIT);
    furi_thread_join(thread);
    furi_thread_free(thread);

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(queue);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_mutex_free(app->mutex);
    free(app->known);
    free(app);
    return 0;
}
