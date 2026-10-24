// Music player for the Nintendo 3DS family (MP3 + WAV).
// Put .mp3 or 16-bit PCM .wav files in the "music" folder on the SD card.
//
// Controls:  Up/Down = choose   A = play   X = pause/resume
//            B = stop           START = exit

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <mpg123.h>

#define MUSIC_DIR   "sdmc:/music"
#define MAX_FILES   256
#define NAME_LEN    128
#define NUM_BUFS    4
#define BUF_FRAMES  8192
#define CHANNEL     0
#define VISIBLE     20

enum { TYPE_WAV, TYPE_MP3 };

typedef struct {
    int   type;
    u16   channels;
    u32   rate;
    bool  eof;
    // WAV
    FILE *f;
    u32   remaining; // bytes of audio data left
    // MP3
    mpg123_handle *mh;
} Track;

// ---------- WAV ----------

static bool wavOpen(Track *t, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    u8 hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f);
        return false;
    }

    bool gotFmt = false;
    for (;;) {
        u8 ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        u32 size;
        memcpy(&size, ch + 4, 4);

        if (!memcmp(ch, "fmt ", 4)) {
            u8 fmt[16];
            if (size < 16 || fread(fmt, 1, 16, f) != 16) break;
            u16 tag, chans, bits;
            u32 rate;
            memcpy(&tag,   fmt,      2);
            memcpy(&chans, fmt + 2,  2);
            memcpy(&rate,  fmt + 4,  4);
            memcpy(&bits,  fmt + 14, 2);
            if ((tag != 1 && tag != 0xFFFE) || bits != 16 || (chans != 1 && chans != 2)) break;
            t->channels = chans;
            t->rate = rate;
            gotFmt = true;
            if (size > 16) fseek(f, (long)(size - 16) + (long)(size & 1), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (!gotFmt) break;
            t->type = TYPE_WAV;
            t->f = f;
            t->remaining = size;
            t->eof = false;
            return true;
        } else {
            fseek(f, (long)size + (long)(size & 1), SEEK_CUR);
        }
    }

    fclose(f);
    return false;
}

static u32 wavRead(Track *t, s16 *dst, u32 maxFrames)
{
    u32 frameBytes = t->channels * 2;
    u32 want = maxFrames * frameBytes;
    if (want > t->remaining) want = t->remaining;
    size_t got = fread(dst, 1, want, t->f);
    got -= got % frameBytes;
    if (got == 0) {
        t->eof = true;
        return 0;
    }
    t->remaining -= (u32)got;
    if (t->remaining == 0) t->eof = true;
    return (u32)(got / frameBytes);
}

// ---------- MP3 (via libmpg123) ----------

static bool mp3Open(Track *t, const char *path)
{
    int err = 0;
    mpg123_handle *mh = mpg123_new(NULL, &err);
    if (!mh) return false;

    // Always decode to 16-bit stereo, at whatever sample rate the file uses.
    mpg123_param(mh, MPG123_FLAGS, MPG123_FORCE_STEREO, 0.0);
    mpg123_format_none(mh);
    const long *rates = NULL;
    size_t nrates = 0;
    mpg123_rates(&rates, &nrates);
    for (size_t i = 0; i < nrates; i++)
        mpg123_format(mh, rates[i], MPG123_STEREO, MPG123_ENC_SIGNED_16);

    if (mpg123_open(mh, path) != MPG123_OK) {
        mpg123_delete(mh);
        return false;
    }

    long rate = 0;
    int chans = 0, enc = 0;
    if (mpg123_getformat(mh, &rate, &chans, &enc) != MPG123_OK || enc != MPG123_ENC_SIGNED_16) {
        mpg123_close(mh);
        mpg123_delete(mh);
        return false;
    }

    t->type = TYPE_MP3;
    t->mh = mh;
    t->channels = 2;
    t->rate = (u32)rate;
    t->eof = false;
    return true;
}

static u32 mp3Read(Track *t, s16 *dst, u32 maxFrames)
{
    size_t done = 0;
    int r = mpg123_read(t->mh, (unsigned char *)dst, (size_t)maxFrames * 4, &done);
    if (r == MPG123_DONE) {
        t->eof = true;
    } else if (r != MPG123_OK && r != MPG123_NEW_FORMAT && done == 0) {
        t->eof = true; // read error: treat as end of song
    }
    return (u32)(done / 4);
}

// ---------- Track (either type) ----------

static bool hasExt(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool trackOpen(Track *t, const char *path)
{
    memset(t, 0, sizeof(*t));
    if (hasExt(path, ".mp3")) return mp3Open(t, path);
    if (hasExt(path, ".wav")) return wavOpen(t, path);
    return false;
}

static u32 trackRead(Track *t, s16 *dst, u32 maxFrames)
{
    return t->type == TYPE_MP3 ? mp3Read(t, dst, maxFrames) : wavRead(t, dst, maxFrames);
}

static void trackClose(Track *t)
{
    if (t->f) fclose(t->f);
    if (t->mh) {
        mpg123_close(t->mh);
        mpg123_delete(t->mh);
    }
    memset(t, 0, sizeof(*t));
}

// ---------- Playback ----------

static Track       track;
static ndspWaveBuf bufs[NUM_BUFS];
static s16        *audioMem = NULL;
static bool        playing = false;
static bool        paused = false;

static void stopPlayback(void)
{
    if (playing) {
        ndspChnReset(CHANNEL);
        trackClose(&track);
    }
    playing = false;
    paused = false;
}

static bool startPlayback(const char *path)
{
    stopPlayback();
    if (!trackOpen(&track, path)) return false;

    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHANNEL, (float)track.rate);
    ndspChnSetFormat(CHANNEL, track.channels == 2 ? NDSP_FORMAT_STEREO_PCM16
                                                  : NDSP_FORMAT_MONO_PCM16);
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(CHANNEL, mix);
    ndspChnSetPaused(CHANNEL, false);

    memset(bufs, 0, sizeof(bufs));
    for (int i = 0; i < NUM_BUFS; i++) {
        bufs[i].data_vaddr = audioMem + (size_t)i * BUF_FRAMES * 2;
        bufs[i].status = NDSP_WBUF_DONE; // marks it as ready to be filled
    }

    playing = true;
    paused = false;
    return true;
}

// Called every frame: refills any buffer the DSP has finished with.
static void updatePlayback(void)
{
    if (!playing) return;

    bool anyActive = false;
    for (int i = 0; i < NUM_BUFS; i++) {
        ndspWaveBuf *b = &bufs[i];
        if (b->status == NDSP_WBUF_QUEUED || b->status == NDSP_WBUF_PLAYING) {
            anyActive = true;
            continue;
        }
        if (track.eof) continue;

        u32 n = trackRead(&track, (s16 *)b->data_vaddr, BUF_FRAMES);
        if (n == 0) continue;
        b->nsamples = n;
        DSP_FlushDataCache(b->data_vaddr, n * track.channels * 2);
        ndspChnWaveBufAdd(CHANNEL, b);
        anyActive = true;
    }

    if (!anyActive && track.eof) stopPlayback(); // song finished
}

// ---------- File list ----------

static char files[MAX_FILES][NAME_LEN];
static int  fileCount = 0;

static int cmpNames(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void scanFiles(void)
{
    fileCount = 0;
    DIR *d = opendir(MUSIC_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && fileCount < MAX_FILES) {
        if (strlen(e->d_name) >= NAME_LEN) continue;
        if (!hasExt(e->d_name, ".mp3") && !hasExt(e->d_name, ".wav")) continue;
        strcpy(files[fileCount++], e->d_name);
    }
    closedir(d);
    qsort(files, fileCount, NAME_LEN, cmpNames);
}

// ---------- Screen ----------

static void draw(int sel, int top, const char *nowPlaying, const char *msg)
{
    printf("\x1b[2J\x1b[1;1H");
    printf("3DS Music Player\n");
    printf("A play  X pause  B stop  START exit\n");
    printf("--------------------------------------------------\n");

    if (fileCount == 0) {
        printf("No .mp3 or .wav files found.\n");
        printf("Put them in the \"music\" folder\n");
        printf("on your SD card.\n");
    }

    for (int i = top; i < fileCount && i < top + VISIBLE; i++) {
        printf("%c %.46s\n", i == sel ? '>' : ' ', files[i]);
    }

    printf("\x1b[26;1H--------------------------------------------------\n");
    if (playing) {
        printf("%s: %.40s\n", paused ? "Paused" : "Playing", nowPlaying);
    } else {
        printf("Stopped\n");
    }
    if (msg && msg[0]) printf("%s\n", msg);
}

// ---------- Main ----------

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    osSetSpeedupEnable(true); // faster CPU on New 3DS, ignored on old models

    if (R_FAILED(ndspInit())) {
        printf("Could not start the audio system.\n\n");
        printf("You need the DSP firmware file on your\n");
        printf("SD card: sdmc:/3ds/dspfirm.cdc\n");
        printf("Run the \"DSP1\" homebrew app to create it.\n\n");
        printf("Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
        gfxExit();
        return 0;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    audioMem = (s16 *)linearAlloc((size_t)NUM_BUFS * BUF_FRAMES * 2 * sizeof(s16));
    mpg123_init();

    scanFiles();

    int  sel = 0, top = 0;
    char nowPlaying[NAME_LEN] = "";
    char msg[64] = "";
    bool dirty = true;
    bool wasPlaying = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;

        if ((down & KEY_DOWN) && sel < fileCount - 1) { sel++; dirty = true; }
        if ((down & KEY_UP) && sel > 0)               { sel--; dirty = true; }
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;

        if ((down & KEY_A) && fileCount > 0) {
            char path[NAME_LEN + 16];
            snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, files[sel]);
            if (startPlayback(path)) {
                strcpy(nowPlaying, files[sel]);
                msg[0] = 0;
            } else {
                snprintf(msg, sizeof(msg), "Can't play that file");
            }
            dirty = true;
        }
        if ((down & KEY_X) && playing) {
            paused = !paused;
            ndspChnSetPaused(CHANNEL, paused);
            dirty = true;
        }
        if (down & KEY_B) {
            stopPlayback();
            dirty = true;
        }

        updatePlayback();
        if (wasPlaying && !playing) dirty = true; // song ended on its own
        wasPlaying = playing;

        if (dirty) {
            draw(sel, top, nowPlaying, msg);
            dirty = false;
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    stopPlayback();
    mpg123_exit();
    ndspExit();
    linearFree(audioMem);
    gfxExit();
    return 0;
}

