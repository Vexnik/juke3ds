// Music player for the Nintendo 3DS family (MP3 + WAV).
// Put .mp3 or 16-bit PCM .wav files in the "music" folder on the SD card.
// Subfolders are supported.
//
// Controls:
//   Up/Down  choose            A  open folder / play song
//   B        go up a folder    X  pause / resume (A on the playing song too)
//   Left/Right  skip 10 sec    L/R  previous / next song
//   Y        shuffle on/off    SELECT  repeat: off / all / one
//   START    exit

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <mpg123.h>

#define MUSIC_DIR   "sdmc:/music"
#define MAX_FILES   256
#define NAME_LEN    128
#define PATH_LEN    256
#define NUM_BUFS    4
#define BUF_FRAMES  8192
#define CHANNEL     0
#define VISIBLE     24

enum { TYPE_WAV, TYPE_MP3 };
enum { REP_OFF, REP_ALL, REP_ONE };

typedef struct {
    int   type;
    u16   channels;
    u32   rate;
    bool  eof;
    u32   totalSec;      // 0 = unknown
    char  title[96];
    char  artist[96];
    // WAV
    FILE *f;
    long  dataStart;     // where the audio data begins in the file
    u32   dataSize;      // total bytes of audio data
    u32   remaining;     // bytes of audio data left
    // MP3
    mpg123_handle *mh;
} Track;

// ---------- helpers ----------

static bool hasExt(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static void copyTrimmed(char *dst, size_t dstSize, const char *src, size_t srcLen)
{
    size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
}

static void fmtTime(char *out, u32 s)
{
    snprintf(out, 16, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

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
            if (!gotFmt || t->rate == 0) break;
            t->type = TYPE_WAV;
            t->f = f;
            t->dataStart = ftell(f);
            t->dataSize = size;
            t->remaining = size;
            t->eof = false;
            t->totalSec = size / (t->rate * t->channels * 2);
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
    // FUZZY + a growing index make seeking much faster (it no longer has to
    // read through the whole file to find the exact spot).
    mpg123_param(mh, MPG123_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET | MPG123_FUZZY, 0.0);
    mpg123_param(mh, MPG123_INDEX_SIZE, -1, 0.0);
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
    if (mpg123_getformat(mh, &rate, &chans, &enc) != MPG123_OK || enc != MPG123_ENC_SIGNED_16 || rate <= 0) {
        mpg123_close(mh);
        mpg123_delete(mh);
        return false;
    }

    t->type = TYPE_MP3;
    t->mh = mh;
    t->channels = 2;
    t->rate = (u32)rate;
    t->eof = false;

    // Length (estimate) and tags
    off_t len = mpg123_length(mh);
    if (len > 0) t->totalSec = (u32)(len / rate);

    mpg123_id3v1 *v1 = NULL;
    mpg123_id3v2 *v2 = NULL;
    if (mpg123_id3(mh, &v1, &v2) == MPG123_OK) {
        if (v2 && v2->title && v2->title->p && v2->title->fill > 1)
            copyTrimmed(t->title, sizeof(t->title), v2->title->p, strlen(v2->title->p));
        else if (v1 && v1->title[0])
            copyTrimmed(t->title, sizeof(t->title), v1->title, 30);

        if (v2 && v2->artist && v2->artist->p && v2->artist->fill > 1)
            copyTrimmed(t->artist, sizeof(t->artist), v2->artist->p, strlen(v2->artist->p));
        else if (v1 && v1->artist[0])
            copyTrimmed(t->artist, sizeof(t->artist), v1->artist, 30);
    }
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

static bool trackOpen(Track *t, const char *path)
{
    memset(t, 0, sizeof(*t));
    bool ok = false;
    if (hasExt(path, ".mp3")) ok = mp3Open(t, path);
    else if (hasExt(path, ".wav")) ok = wavOpen(t, path);

    if (ok && t->title[0] == 0) {
        // No tag: use the file name without folders or extension
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        copyTrimmed(t->title, sizeof(t->title), name, strlen(name));
        char *dot = strrchr(t->title, '.');
        if (dot) *dot = 0;
    }
    return ok;
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

// ---------- Playback engine ----------

static Track       track;
static ndspWaveBuf bufs[NUM_BUFS];
static bool        filled[NUM_BUFS];
static s16        *audioMem = NULL;
static bool        playing = false;
static bool        paused = false;
static u64         doneFrames = 0; // frames that have finished playing

static void stopPlayback(void)
{
    if (playing) {
        ndspChnReset(CHANNEL);
        trackClose(&track);
    }
    playing = false;
    paused = false;
}

static void configureChannel(void)
{
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
}

static bool startPlayback(const char *path)
{
    stopPlayback();
    if (!trackOpen(&track, path)) {
        trackClose(&track);
        return false;
    }

    configureChannel();
    ndspChnSetPaused(CHANNEL, false);

    memset(bufs, 0, sizeof(bufs));
    memset(filled, 0, sizeof(filled));
    for (int i = 0; i < NUM_BUFS; i++) {
        bufs[i].data_vaddr = audioMem + (size_t)i * BUF_FRAMES * 2;
        bufs[i].status = NDSP_WBUF_DONE; // marks it as ready to be filled
    }

    doneFrames = 0;
    playing = true;
    paused = false;
    return true;
}

// Called every frame: refills buffers the DSP has finished with.
// Returns true when the song has just finished playing by itself.
static bool updatePlayback(void)
{
    if (!playing) return false;

    bool anyActive = false;
    for (int i = 0; i < NUM_BUFS; i++) {
        ndspWaveBuf *b = &bufs[i];
        if (b->status == NDSP_WBUF_QUEUED || b->status == NDSP_WBUF_PLAYING) {
            anyActive = true;
            continue;
        }
        if (filled[i]) {
            doneFrames += b->nsamples;
            filled[i] = false;
        }
        if (track.eof) continue;

        u32 n = trackRead(&track, (s16 *)b->data_vaddr, BUF_FRAMES);
        if (n == 0) continue;
        b->nsamples = n;
        DSP_FlushDataCache(b->data_vaddr, n * track.channels * 2);
        ndspChnWaveBufAdd(CHANNEL, b);
        filled[i] = true;
        anyActive = true;
    }

    if (!anyActive && track.eof) {
        stopPlayback();
        return true;
    }
    return false;
}

static u32 elapsedSeconds(void)
{
    if (!playing || track.rate == 0) return 0;
    return (u32)(doneFrames / track.rate);
}

// Jump to a position in the current song. Returns false if that isn't possible.
static bool seekToSecond(u32 sec)
{
    if (!playing || track.rate == 0) return false;
    u64 frame = (u64)sec * track.rate;

    if (track.type == TYPE_MP3) {
        if (mpg123_seek(track.mh, (off_t)frame, SEEK_SET) < 0) return false;
    } else {
        u64 byteOff = frame * track.channels * 2;
        if (byteOff >= track.dataSize) return false;
        fseek(track.f, track.dataStart + (long)byteOff, SEEK_SET);
        track.remaining = track.dataSize - (u32)byteOff;
    }
    track.eof = false;

    // Throw away the audio that was already queued and start fresh from here.
    configureChannel();
    ndspChnSetPaused(CHANNEL, paused);
    memset(bufs, 0, sizeof(bufs));
    memset(filled, 0, sizeof(filled));
    for (int i = 0; i < NUM_BUFS; i++) {
        bufs[i].data_vaddr = audioMem + (size_t)i * BUF_FRAMES * 2;
        bufs[i].status = NDSP_WBUF_DONE;
    }
    doneFrames = frame;
    return true;
}

// ---------- Folder browser ----------

typedef struct {
    char name[NAME_LEN];
    bool dir;
} Entry;

static Entry entries[MAX_FILES];
static int   entryCount = 0;
static char  curDir[PATH_LEN] = MUSIC_DIR;

static int cmpEntries(const void *a, const void *b)
{
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scanDir(void)
{
    entryCount = 0;
    int start = 0;
    if (strcmp(curDir, MUSIC_DIR) != 0) {
        strcpy(entries[0].name, "..");
        entries[0].dir = true;
        entryCount = 1;
        start = 1;
    }

    DIR *d = opendir(curDir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && entryCount < MAX_FILES) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= NAME_LEN) continue;
        if (strlen(curDir) + strlen(e->d_name) + 2 >= PATH_LEN) continue;

        char full[PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", curDir, e->d_name);
        struct stat st;
        bool isDir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));

        if (!isDir && !hasExt(e->d_name, ".mp3") && !hasExt(e->d_name, ".wav")) continue;
        strcpy(entries[entryCount].name, e->d_name);
        entries[entryCount].dir = isDir;
        entryCount++;
    }
    closedir(d);
    qsort(entries + start, entryCount - start, sizeof(Entry), cmpEntries);
}

// ---------- Playlist (the songs of the folder you started playing from) ----------

static char plNames[MAX_FILES][NAME_LEN];
static char plDir[PATH_LEN];
static int  plCount = 0;
static int  order[MAX_FILES];
static int  pos = 0;
static bool shuffle = false;
static int  repeat = REP_OFF;
static char msg[64] = "";

static void buildOrder(int curIdx)
{
    for (int i = 0; i < plCount; i++) order[i] = i;
    if (shuffle && plCount > 1) {
        int tmp = order[0];
        order[0] = order[curIdx];
        order[curIdx] = tmp;
        for (int i = plCount - 1; i > 1; i--) {
            int j = 1 + rand() % i;
            tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
        pos = 0;
    } else {
        pos = curIdx;
    }
}

static bool playPos(void)
{
    char path[PATH_LEN + NAME_LEN];
    snprintf(path, sizeof(path), "%s/%s", plDir, plNames[order[pos]]);
    return startPlayback(path);
}

// dir = +1 next, -1 previous. automatic = the song ended by itself.
static void advance(int dir, bool automatic)
{
    if (plCount == 0) return;
    for (int tries = 0; tries < plCount; tries++) {
        if (dir > 0) {
            if (pos + 1 < plCount) pos++;
            else if (repeat == REP_ALL || !automatic) pos = 0;
            else return; // end of the list: stop
        } else {
            if (pos > 0) pos--;
            else pos = plCount - 1;
        }
        if (playPos()) {
            msg[0] = 0;
            return;
        }
    }
    snprintf(msg, sizeof(msg), "No playable files");
}

static void startFromList(int sel)
{
    plCount = 0;
    int idx = 0;
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].dir) continue;
        if (i == sel) idx = plCount;
        strcpy(plNames[plCount++], entries[i].name);
    }
    strcpy(plDir, curDir);
    buildOrder(idx);
    if (playPos()) msg[0] = 0;
    else snprintf(msg, sizeof(msg), "Can't play that file");
}

// ---------- Screens ----------

static PrintConsole topScreen, bottomScreen;

static void drawTop(int sel, int top)
{
    consoleSelect(&topScreen);
    consoleClear();
    printf("3DS Music Player\n");
    printf("%.49s\n", curDir + 5); // skip the "sdmc:" prefix
    printf("--------------------------------------------------\n");

    if (entryCount == 0) {
        printf("No .mp3 or .wav files here.\n");
    }
    for (int i = top; i < entryCount && i < top + VISIBLE; i++) {
        if (entries[i].dir) printf("%c [%.44s]\n", i == sel ? '>' : ' ', entries[i].name);
        else                printf("%c %.46s\n",   i == sel ? '>' : ' ', entries[i].name);
    }
    printf("\x1b[29;1H%d items", entryCount);
}

static void drawBottom(void)
{
    consoleSelect(&bottomScreen);
    consoleClear();
    printf("NOW PLAYING\n");
    printf("----------------------------------------\n");

    if (playing) {
        printf("%.39s\n", track.title);
        printf("%.39s\n\n", track.artist);

        char a[16], b[16];
        u32 el = elapsedSeconds();
        fmtTime(a, el);
        char bar[32];
        int width = 28;
        int filledChars = 0;
        if (track.totalSec > 0) {
            u64 f = (u64)el * width / track.totalSec;
            filledChars = f > (u64)width ? width : (int)f;
            fmtTime(b, track.totalSec);
        } else {
            strcpy(b, "--:--");
        }
        for (int i = 0; i < width; i++) bar[i] = i < filledChars ? '#' : '-';
        bar[width] = 0;
        printf("[%s]\n", bar);
        printf("%s / %s%s\n\n", a, b, paused ? "   (paused)" : "");
    } else {
        printf("Nothing playing\n\n\n\n\n\n");
    }

    printf("Shuffle: %s\n", shuffle ? "on" : "off");
    printf("Repeat:  %s\n", repeat == REP_OFF ? "off" : (repeat == REP_ALL ? "all" : "one"));

    printf("\x1b[22;1H----------------------------------------\n");
    printf("A play/open/pause  B back  X pause\n");
    printf("L/R prev/next Y shuffle SELECT repeat\n");
    printf("Left/Right skip 10s     START exit\n");
    if (msg[0]) printf("%s\n", msg);
}

// ---------- Main ----------

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);
    consoleSelect(&topScreen);
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
    srand((unsigned)osGetTime());

    scanDir();

    int  sel = 0, top = 0;
    bool topDirty = true, bottomDirty = true;
    u32  lastSecond = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;

        // --- browsing ---
        if ((down & KEY_DOWN) && sel < entryCount - 1) { sel++; topDirty = true; }
        if ((down & KEY_UP) && sel > 0)                { sel--; topDirty = true; }

        if ((down & KEY_A) && entryCount > 0) {
            Entry *e = &entries[sel];
            if (e->dir && strcmp(e->name, "..") != 0) {
                if (strlen(curDir) + strlen(e->name) + 2 < PATH_LEN) {
                    strcat(curDir, "/");
                    strcat(curDir, e->name);
                    scanDir();
                    sel = 0;
                    top = 0;
                }
            } else if (e->dir) {
                down |= KEY_B; // ".." acts like B
            } else if (playing && plCount > 0 && strcmp(plDir, curDir) == 0 &&
                       strcmp(plNames[order[pos]], e->name) == 0) {
                // A on the song that is already loaded: pause / resume it
                paused = !paused;
                ndspChnSetPaused(CHANNEL, paused);
            } else {
                startFromList(sel);
            }
            topDirty = bottomDirty = true;
        }

        if ((down & KEY_B) && strcmp(curDir, MUSIC_DIR) != 0) {
            char *slash = strrchr(curDir, '/');
            if (slash) {
                char last[NAME_LEN];
                strncpy(last, slash + 1, NAME_LEN - 1);
                last[NAME_LEN - 1] = 0;
                *slash = 0;
                scanDir();
                sel = 0;
                for (int i = 0; i < entryCount; i++)
                    if (strcasecmp(entries[i].name, last) == 0) sel = i;
                top = sel - VISIBLE / 2;
                if (top < 0) top = 0;
            }
            topDirty = true;
        }

        // --- playback controls ---
        if ((down & KEY_X) && playing) {
            paused = !paused;
            ndspChnSetPaused(CHANNEL, paused);
            bottomDirty = true;
        }
        if ((down & KEY_LEFT) && playing) {
            u32 el = elapsedSeconds();
            seekToSecond(el > 10 ? el - 10 : 0);
            bottomDirty = true;
        }
        if ((down & KEY_RIGHT) && playing) {
            u32 target = elapsedSeconds() + 10;
            if (track.totalSec == 0 || target < track.totalSec) seekToSecond(target);
            bottomDirty = true;
        }

        if (down & KEY_Y) {
            shuffle = !shuffle;
            if (plCount > 0) buildOrder(order[pos]);
            bottomDirty = true;
        }
        if (down & KEY_SELECT) {
            repeat = (repeat + 1) % 3;
            bottomDirty = true;
        }
        if ((down & KEY_R) && plCount > 0) { advance(+1, false); bottomDirty = true; }
        if ((down & KEY_L) && plCount > 0) {
            // More than 3 seconds in: restart the song. Otherwise go to the previous one.
            if (!(playing && elapsedSeconds() > 3 && seekToSecond(0))) advance(-1, false);
            bottomDirty = true;
        }

        // --- keep the audio fed ---
        if (updatePlayback()) {
            // song ended by itself: go on to the next one
            if (repeat == REP_ONE) {
                if (!playPos()) snprintf(msg, sizeof(msg), "Can't play that file");
            } else {
                advance(+1, true);
            }
            bottomDirty = true;
        }

        // Let the system sleep only when nothing is playing. This is what
        // allows music to keep going when you close the lid.
        aptSetSleepAllowed(!(playing && !paused));

        // Re-draw the progress bar once per second
        u32 sec = elapsedSeconds();
        if (playing && sec != lastSecond) bottomDirty = true;
        lastSecond = sec;

        // --- scrolling window ---
        if (sel < top) { top = sel; topDirty = true; }
        if (sel >= top + VISIBLE) { top = sel - VISIBLE + 1; topDirty = true; }

        if (topDirty)    { drawTop(sel, top);      topDirty = false; }
        if (bottomDirty) { drawBottom();     bottomDirty = false; }

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
