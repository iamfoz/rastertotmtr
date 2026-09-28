/*
 * rastertotmtr - CUPS raster -> ESC/POS filter for EPSON TM series printers.
 *
 * Clean-room, behaviour-compatible reimplementation of the x86_64-only
 * "rastertotmtr" filter shipped in Epson's TMPrinter.pkg (bundle id
 * com.epson.tmprinter.rastertotmtr, 2019).  Built as a universal
 * (arm64 + x86_64) binary so it runs natively without Rosetta 2.
 *
 * Usage (standard CUPS filter interface):
 *   rastertotmtr job user title copies options [file]
 *
 * The PPD (from $PPD) supplies:
 *   *TmxMotionUnitHori / *TmxMotionUnitVert   (1..255)
 *   *TmxPaperReduction  Off | Top | Bottom | Both
 *   *TmxPaperCut        NoCut | CutPerJob | CutPerPage
 *   *TmxBuzzerAndDrawer NotUsed | InternalBuzzer | ExternalBuzzer |
 *                       OpenDrawer1 | OpenDrawer2
 *
 * Optional user command files are injected verbatim from
 *   /Library/Caches/Epson/TerminalPrinter/<printer>_{StartJob,StartPage,EndPage,EndJob}.prn
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USER_FILE_DIR   "/Library/Caches/Epson/TerminalPrinter"
#define MAX_BAND_LINES  256u

/* Error codes reported as "ERROR: Error Code=%d" (same values as the original). */
enum {
    ERR_BAD_ARGS            = 1001,   /* 0x3e9 */
    ERR_OPEN_INPUT          = 1002,   /* 0x3ea */
    ERR_RASTER_OPEN         = 1003,   /* 0x3eb */
    ERR_SIGPROCMASK_BLOCK   = 1103,   /* 0x44f */
    ERR_SIGACTION_GET       = 1104,   /* 0x450 */
    ERR_SIGACTION_SET       = 1105,   /* 0x451 */
    ERR_SIGPROCMASK_UNBLOCK = 1106,   /* 0x452 */
    ERR_BITS_PER_PIXEL      = 2001,   /* 0x7d1 */
    ERR_PAGE_ALLOC          = 2002,   /* 0x7d2 */
    ERR_W_INIT              = 2101,   /* 0x835 */
    ERR_W_PAPER0            = 2102,   /* 0x836 */
    ERR_W_PAPER1            = 2103,   /* 0x837 */
    ERR_W_SENSOR            = 2104,   /* 0x838 */
    ERR_W_MOTION            = 2105,   /* 0x839 */
    ERR_W_DRAWER            = 2106,   /* 0x83a */
    ERR_W_BUZZER            = 2107,   /* 0x83b */
    ERR_W_STARTJOB          = 2108,   /* 0x83c */
    ERR_W_ENDJOB            = 2201,   /* 0x899 */
    ERR_W_JOBCUT            = 2202,   /* 0x89a */
    ERR_W_JOBCUT_FEED       = 2203,   /* 0x89b */
    ERR_W_STARTPAGE         = 3102,   /* 0xc1e */
    ERR_W_ENDPAGE           = 3201,   /* 0xc81 */
    ERR_W_PAGECUT           = 3202,   /* 0xc82 */
    ERR_W_PAGECUT_FEED      = 3203,   /* 0xc83 */
    ERR_LINE_ALLOC          = 3301,   /* 0xce5 */
    ERR_READ_PIXELS         = 3302,   /* 0xce6 */
    ERR_W_FEED_BLANK        = 3401,   /* 0xd49 */
    ERR_W_FEED_TOP          = 3402,   /* 0xd4a */
    ERR_W_BAND              = 3403,   /* 0xd4b */
    ERR_W_BAND_LAST         = 3404,   /* 0xd4c */
    ERR_W_FEED_BOTTOM       = 3405,   /* 0xd4d */
    ERR_PPD_OPEN            = 4001,   /* 0xfa1 */
    ERR_PPD_MARK            = 4002,   /* 0xfa2 */
    ERR_PPD_HORI_MISSING    = 4101,   /* 0x1005 */
    ERR_PPD_HORI_RANGE      = 4102,   /* 0x1006 */
    ERR_PPD_VERT_MISSING    = 4103,   /* 0x1007 */
    ERR_PPD_VERT_RANGE      = 4104,   /* 0x1008 */
    ERR_PPD_REDUCTION_MISS  = 4201,   /* 0x1069 */
    ERR_PPD_REDUCTION_VALUE = 4202,   /* 0x106a */
    ERR_PPD_BUZZER_MISSING  = 4301,   /* 0x10cd */
    ERR_PPD_BUZZER_VALUE    = 4302,   /* 0x10ce */
    ERR_PPD_CUT_MISSING     = 4401,   /* 0x1131 */
    ERR_PPD_CUT_VALUE       = 4402,   /* 0x1132 */
    ERR_CANCELED            = -2
};

enum { REDUCE_OFF = 0, REDUCE_TOP = 1, REDUCE_BOTTOM = 2, REDUCE_BOTH = 3 };
enum { CUT_NONE = 0, CUT_PER_JOB = 1, CUT_PER_PAGE = 2 };
enum { BUZZER_NONE = 0, BUZZER_INTERNAL = 1, BUZZER_EXTERNAL = 2 };

typedef struct {
    const char *printerName;
    unsigned    hMotionUnit;
    unsigned    vMotionUnit;
    int         paperReduction;
    int         buzzerControl;
    int         drawerControl;
    int         cutControl;
    unsigned    maxBandLines;
} tm_settings_t;

static volatile sig_atomic_t g_TmCanceled = 0;

static void SignalCallback(int sig)
{
    (void)sig;
    g_TmCanceled = 1;
}

/* Write len bytes to stdout, retrying on EINTR / short writes. 0 on success, -1 on error. */
static int WriteData(const void *data, unsigned len)
{
    const unsigned char *p = data;
    unsigned done = 0;

    while (done < len) {
        ssize_t n = write(1, p + done, len - done);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno != EINTR)
                return -1;
            continue;
        }
        done += (unsigned)n;
    }
    return done == len ? 0 : -1;
}

/* Copy <dir>/<printer>_<name> to stdout if it exists. Missing file is not an error. */
static int WriteUserFile(const char *printerName, const char *name)
{
    char          path[512];
    unsigned char buf[1024];
    int           fd;

    snprintf(path, sizeof(path), "%s/%s_%s", USER_FILE_DIR, printerName, name);

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;

    for (;;) {
        int total = 0;

        memset(buf, 0, sizeof(buf));
        for (;;) {
            ssize_t n = read(fd, buf + total, sizeof(buf) - (size_t)total);
            if (n < 0) {
                close(fd);
                return -1;
            }
            total += (int)n;
            if (n == 0 || total >= (int)sizeof(buf))
                break;
        }

        if (total == 0)
            return close(fd) < 0 ? -1 : 0;

        if (WriteData(buf, (unsigned)total) != 0) {
            close(fd);
            return -1;
        }
    }
}

/* Feed "lines" raster lines, expressed in vertical motion units (ESC J n, n <= 255 per command). */
static int FeedPaper(const tm_settings_t *s, const cups_page_header_t *h, unsigned lines)
{
    unsigned char cmd[3] = { 0x1b, 0x4a, 0xff };
    double        ipart = 0.0;
    unsigned      units;

    /*
     * After the final page cupsRasterReadHeader() leaves the header zeroed,
     * so the job-end feed divides by HWResolution[1] == 0.  The original
     * x86_64 build converted the resulting inf/NaN to 0 (cvttsd2si), i.e.
     * "no feed".  arm64 saturates instead, so make the behaviour explicit.
     */
    if (h->HWResolution[1] == 0)
        return 0;

    (void)modf((double)(lines * s->vMotionUnit) / (double)h->HWResolution[1], &ipart);
    units = (unsigned)(long long)ipart;

    if (units == 0)
        return 0;

    while (units >= 256) {
        if (WriteData(cmd, 3) != 0)
            return -1;
        units -= 255;
    }
    cmd[2] = (unsigned char)units;
    return WriteData(cmd, 3);
}

/* Send one band of 1-bpp rows using GS 8 L (store) + GS ( L fn=50 (print). */
static int WriteBand(const cups_page_header_t *h, const unsigned char *data, unsigned lines)
{
    static const unsigned char setpos[4]  = { 0x1b, 0x24, 0x00, 0x00 };          /* ESC $ 0 0 */
    static const unsigned char print[7]   = { 0x1d, 0x28, 0x4c, 0x02, 0x00, 0x30, 0x32 }; /* GS ( L 2 0 48 50 */
    unsigned char cmd[17] = {
        0x1d, 0x38, 0x4c,            /* GS 8 L                       */
        0x00, 0x00, 0x00, 0x00,      /* p1..p4 (payload length + 10) */
        0x30, 0x70, 0x30,            /* m=48 fn=112 a=48             */
        0x01, 0x01,                  /* bx=1 by=1                    */
        0x31,                        /* c=49                         */
        0x00, 0x00,                  /* xL xH                        */
        0x00, 0x00                   /* yL yH                        */
    };
    unsigned      widthBytes = (h->cupsWidth + 7) >> 3;
    unsigned long dataLen    = (unsigned long)lines * widthBytes;
    unsigned long p          = dataLen + 10;

    if (WriteData(setpos, sizeof(setpos)) != 0)
        return -1;

    cmd[3]  = (unsigned char)(p);
    cmd[4]  = (unsigned char)(p >> 8);
    cmd[5]  = (unsigned char)(p >> 16);
    cmd[6]  = (unsigned char)(p >> 24);
    cmd[13] = (unsigned char)(h->cupsWidth);
    cmd[14] = (unsigned char)(h->cupsWidth >> 8);
    cmd[15] = (unsigned char)(lines);
    cmd[16] = (unsigned char)(lines >> 8);

    if (WriteData(cmd, sizeof(cmd)) != 0)
        return -1;
    if (WriteData(data, (unsigned)dataLen) != 0)
        return -1;
    return WriteData(print, sizeof(print));
}

static int InstallSignalHandler(void)
{
    sigset_t         set;
    struct sigaction act;

    sigemptyset(&set);
    sigaddset(&set, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
        return ERR_SIGPROCMASK_BLOCK;
    if (sigaction(SIGTERM, NULL, &act) != 0)
        return ERR_SIGACTION_GET;
    act.sa_handler = SignalCallback;
    act.sa_flags  |= SA_RESTART;
    if (sigaction(SIGTERM, &act, NULL) != 0)
        return ERR_SIGACTION_SET;
    if (sigprocmask(SIG_UNBLOCK, &set, NULL) != 0)
        return ERR_SIGPROCMASK_UNBLOCK;
    return 0;
}

static int ReadSettings(tm_settings_t *s, const char *optionString)
{
    ppd_file_t    *ppd;
    ppd_attr_t    *attr;
    ppd_choice_t  *choice;
    cups_option_t *options = NULL;
    int            num;
    int            err = 0;

    ppd = ppdOpenFile(getenv("PPD"));
    if (!ppd)
        return ERR_PPD_OPEN;

    ppdMarkDefaults(ppd);
    num = cupsParseOptions(optionString, 0, &options);
    if (num > 0 && cupsMarkOptions(ppd, num, options) != 0) {
        ppdClose(ppd);
        cupsFreeOptions(num, options);
        return ERR_PPD_MARK;
    }
    cupsFreeOptions(num, options);

    do {
        if (!(attr = ppdFindAttr(ppd, "TmxMotionUnitHori", NULL))) { err = ERR_PPD_HORI_MISSING; break; }
        s->hMotionUnit = (unsigned)atol(attr->value);
        if (s->hMotionUnit - 1 > 0xfe) { err = ERR_PPD_HORI_RANGE; break; }

        if (!(attr = ppdFindAttr(ppd, "TmxMotionUnitVert", NULL))) { err = ERR_PPD_VERT_MISSING; break; }
        s->vMotionUnit = (unsigned)atol(attr->value);
        if (s->vMotionUnit - 1 > 0xfe) { err = ERR_PPD_VERT_RANGE; break; }

        if (!(choice = ppdFindMarkedChoice(ppd, "TmxPaperReduction"))) { err = ERR_PPD_REDUCTION_MISS; break; }
        if      (!strcmp(choice->choice, "Off"))    s->paperReduction = REDUCE_OFF;
        else if (!strcmp(choice->choice, "Top"))    s->paperReduction = REDUCE_TOP;
        else if (!strcmp(choice->choice, "Bottom")) s->paperReduction = REDUCE_BOTTOM;
        else if (!strcmp(choice->choice, "Both"))   s->paperReduction = REDUCE_BOTH;
        else { err = ERR_PPD_REDUCTION_VALUE; break; }

        if (!(choice = ppdFindMarkedChoice(ppd, "TmxPaperCut"))) { err = ERR_PPD_CUT_MISSING; break; }
        if      (!strcmp(choice->choice, "NoCut"))      s->cutControl = CUT_NONE;
        else if (!strcmp(choice->choice, "CutPerJob"))  s->cutControl = CUT_PER_JOB;
        else if (!strcmp(choice->choice, "CutPerPage")) s->cutControl = CUT_PER_PAGE;
        else { err = ERR_PPD_CUT_VALUE; break; }

        if (!(choice = ppdFindMarkedChoice(ppd, "TmxBuzzerAndDrawer"))) { err = ERR_PPD_BUZZER_MISSING; break; }
        if      (!strcmp(choice->choice, "NotUsed"))        s->buzzerControl = BUZZER_NONE;
        else if (!strcmp(choice->choice, "InternalBuzzer")) s->buzzerControl = BUZZER_INTERNAL;
        else if (!strcmp(choice->choice, "ExternalBuzzer")) s->buzzerControl = BUZZER_EXTERNAL;
        else if (!strcmp(choice->choice, "OpenDrawer1"))    s->drawerControl = 1;
        else if (!strcmp(choice->choice, "OpenDrawer2"))    s->drawerControl = 2;
        else { err = ERR_PPD_BUZZER_VALUE; break; }
    } while (0);

    ppdClose(ppd);
    return err;
}

/* ESC J 0 + GS V B 0: feed to cut position and partial cut. */
static const unsigned char kCutCmd[7] = { 0x1b, 0x4a, 0x00, 0x1d, 0x56, 0x42, 0x00 };

static int WriteJobHeader(const tm_settings_t *s)
{
    static const unsigned char init[5]    = { 0x1b, 0x3d, 0x01, 0x1b, 0x40 };       /* ESC = 1, ESC @      */
    static const unsigned char paper0[4]  = { 0x1b, 0x63, 0x30, 0x02 };             /* ESC c 0 2           */
    static const unsigned char paper1[4]  = { 0x1b, 0x63, 0x31, 0x02 };             /* ESC c 1 2           */
    static const unsigned char sensor[4]  = { 0x1b, 0x63, 0x33, 0x00 };             /* ESC c 3 0           */
    static const unsigned char intBuzz[5] = { 0x1b, 0x70, 0x01, 0x32, 0xc8 };       /* ESC p 1 50 200      */
    static const unsigned char extBuzz[10]= { 0x1b, 0x28, 0x41, 0x05, 0x00, 0x61, 0x64, 0x01, 0x32, 0xc8 }; /* ESC ( A ... */
    unsigned char motion[4] = { 0x1d, 0x50, 0x00, 0x00 };                            /* GS P x y            */
    unsigned char drawer[5] = { 0x1b, 0x70, 0x00, 0x32, 0xc8 };                      /* ESC p m 50 200      */

    if (WriteData(init,   sizeof(init))   != 0) return ERR_W_INIT;
    if (WriteData(paper0, sizeof(paper0)) != 0) return ERR_W_PAPER0;
    if (WriteData(paper1, sizeof(paper1)) != 0) return ERR_W_PAPER1;
    if (WriteData(sensor, sizeof(sensor)) != 0) return ERR_W_SENSOR;

    motion[2] = (unsigned char)s->hMotionUnit;
    motion[3] = (unsigned char)s->vMotionUnit;
    if (WriteData(motion, sizeof(motion)) != 0) return ERR_W_MOTION;

    if (s->drawerControl != 0) {
        drawer[2] = (unsigned char)(s->drawerControl - 1);
        if (WriteData(drawer, sizeof(drawer)) != 0) return ERR_W_DRAWER;
    }
    if (s->buzzerControl == BUZZER_INTERNAL) {
        if (WriteData(intBuzz, sizeof(intBuzz)) != 0) return ERR_W_BUZZER;
    } else if (s->buzzerControl == BUZZER_EXTERNAL) {
        if (WriteData(extBuzz, sizeof(extBuzz)) != 0) return ERR_W_BUZZER;
    }

    if (WriteUserFile(s->printerName, "StartJob.prn") != 0) return ERR_W_STARTJOB;
    return 0;
}

/*
 * Neutralise byte sequences inside the image data that the printer would
 * otherwise interpret as real-time commands: ESC '=' and DLE EOT/ENQ/DC4.
 */
static void SanitizeImageData(unsigned char *p, unsigned long len)
{
    unsigned long i;

    for (i = 1; i < len; i++) {
        unsigned char prev = p[i - 1], cur = p[i];
        if (prev == 0x1b) {
            if (cur == 0x3d)
                p[i - 1] = 0x3b;
        } else if (prev == 0x10) {
            if (cur == 0x04 || cur == 0x05 || cur == 0x14)
                p[i - 1] = 0x30;
        }
    }
}

static int WritePage(const tm_settings_t *s, const cups_page_header_t *h, unsigned char *page)
{
    unsigned rowBytes = (h->cupsWidth + 7) >> 3;
    unsigned height   = h->cupsHeight;
    unsigned first, last, y, r;

    /* First non-blank row (== height when the page is empty). */
    for (first = 0; first < height; first++) {
        const unsigned char *row = page + (unsigned long)first * rowBytes;
        for (r = 0; r < rowBytes; r++)
            if (row[r]) break;
        if (r < rowBytes) break;
    }

    if (first == height) {
        if (s->paperReduction == REDUCE_OFF && FeedPaper(s, h, height) != 0)
            return ERR_W_FEED_BLANK;
        return 0;
    }

    /* One past the last non-blank row. */
    last = 1;
    for (y = 0; y < height; y++) {
        const unsigned char *row = page + (unsigned long)(height - 1 - y) * rowBytes;
        for (r = 0; r < rowBytes; r++)
            if (row[r]) break;
        if (r < rowBytes) { last = height - y; break; }
    }

    SanitizeImageData(page + (unsigned long)first * rowBytes, (unsigned long)(last - first) * rowBytes);

    if (s->paperReduction != REDUCE_TOP && s->paperReduction != REDUCE_BOTH) {
        if (FeedPaper(s, h, first) != 0)
            return ERR_W_FEED_TOP;
    }

    for (y = first; y + s->maxBandLines < last; y += s->maxBandLines) {
        if (WriteBand(h, page + (unsigned long)y * rowBytes, s->maxBandLines) != 0)
            return ERR_W_BAND;
        if (g_TmCanceled)
            return ERR_CANCELED;
    }
    if (last > y) {
        if (WriteBand(h, page + (unsigned long)y * rowBytes, last - y) != 0)
            return ERR_W_BAND_LAST;
    }

    if (s->paperReduction != REDUCE_BOTTOM && s->paperReduction != REDUCE_BOTH) {
        if (FeedPaper(s, h, height - last) != 0)
            return ERR_W_FEED_BOTTOM;
    }
    return 0;
}

static int ProcessJob(const tm_settings_t *s, cups_raster_t *ras, cups_page_header_t *h, unsigned char **pageBuf)
{
    unsigned page = 0;
    int      err;

    if (g_TmCanceled)
        return ERR_CANCELED;
    if ((err = WriteJobHeader(s)) != 0)
        return err;

    while (cupsRasterReadHeader(ras, h)) {
        unsigned       rowBytes, y, bytesPerLine;
        unsigned char *line;

        page++;
        fprintf(stderr, "PAGE: %u %d\n", page, h->NumCopies);
        fprintf(stderr, "DEBUG: cupsBytesPerLine = %u\n", h->cupsBytesPerLine);
        fprintf(stderr, "DEBUG: cupsBitsPerPixel = %u\n", h->cupsBitsPerPixel);
        fprintf(stderr, "DEBUG: cupsBitsPerColor = %u\n", h->cupsBitsPerColor);
        fprintf(stderr, "DEBUG:       cupsHeight = %u\n", h->cupsHeight);
        fprintf(stderr, "DEBUG:        cupsWidth = %u\n", h->cupsWidth);

        if (h->cupsBitsPerPixel != 1)
            return ERR_BITS_PER_PIXEL;

        rowBytes = (h->cupsWidth + 7) >> 3;
        if (*pageBuf == NULL) {
            *pageBuf = calloc((size_t)rowBytes * h->cupsHeight, 1);
            if (*pageBuf == NULL)
                return ERR_PAGE_ALLOC;
        }

        if (WriteUserFile(s->printerName, "StartPage.prn") != 0)
            return ERR_W_STARTPAGE;

        bytesPerLine = h->cupsBytesPerLine;
        line = calloc(bytesPerLine, 1);
        if (line == NULL)
            return ERR_LINE_ALLOC;

        for (y = 1; y <= h->cupsHeight; y++) {
            unsigned n;
            if (g_TmCanceled) { free(line); return ERR_CANCELED; }
            n = cupsRasterReadPixels(ras, line, bytesPerLine);
            if (n < bytesPerLine) {
                fprintf(stderr, "DEBUG: cupsRasterReadPixels() = %u:%u/%u\n", y, n, bytesPerLine);
                free(line);
                return ERR_READ_PIXELS;
            }
            memcpy(*pageBuf + (unsigned long)(y - 1) * rowBytes, line, bytesPerLine);
        }
        free(line);

        if ((err = WritePage(s, h, *pageBuf)) != 0)
            return err;
        if (g_TmCanceled)
            return ERR_CANCELED;

        if (WriteUserFile(s->printerName, "EndPage.prn") != 0)
            return ERR_W_ENDPAGE;

        if (s->cutControl == CUT_PER_PAGE) {
            if (WriteData(kCutCmd, sizeof(kCutCmd)) != 0)
                return ERR_W_PAGECUT;
            if (FeedPaper(s, h, s->vMotionUnit * 10 / 254) != 0)
                return ERR_W_PAGECUT_FEED;
        }
    }
    return 0;
}

static int FinishJob(const tm_settings_t *s, const cups_page_header_t *h)
{
    if (g_TmCanceled)
        return ERR_CANCELED;
    if (WriteUserFile(s->printerName, "EndJob.prn") != 0)
        return ERR_W_ENDJOB;
    if (s->cutControl == CUT_PER_JOB) {
        if (WriteData(kCutCmd, sizeof(kCutCmd)) != 0)
            return ERR_W_JOBCUT;
        if (FeedPaper(s, h, s->vMotionUnit * 10 / 254) != 0)
            return ERR_W_JOBCUT_FEED;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    tm_settings_t      settings;
    cups_page_header_t header;
    cups_raster_t     *ras     = NULL;
    unsigned char     *pageBuf = NULL;
    int                fd      = -1;
    int                err     = ERR_BAD_ARGS;
    int                jobErr;

    memset(&settings, 0, sizeof(settings));
    memset(&header, 0, sizeof(header));
    g_TmCanceled = 0;

    if (argv == NULL || (argc != 6 && argc != 7))
        goto cleanup;

    if ((err = InstallSignalHandler()) != 0)
        goto cleanup;

    if (argc == 7) {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0) { err = ERR_OPEN_INPUT; goto cleanup; }
    } else {
        fd = 0;
    }

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (ras == NULL) { err = ERR_RASTER_OPEN; goto cleanup; }

    if ((err = ReadSettings(&settings, argv[5])) != 0)
        goto cleanup;

    settings.printerName  = argv[0];
    settings.maxBandLines = MAX_BAND_LINES;

    jobErr = ProcessJob(&settings, ras, &header, &pageBuf);
    free(pageBuf);
    pageBuf = NULL;

    err = FinishJob(&settings, &header);
    if (jobErr != 0)
        err = jobErr;

cleanup:
    if (ras)
        cupsRasterClose(ras);
    if (fd > 0)
        close(fd);
    if (err != 0)
        fprintf(stderr, "ERROR: Error Code=%d\n", err);

    fprintf(stderr, "DEBUG:       p_printerName = %s\n", settings.printerName);
    fprintf(stderr, "DEBUG:        v_motionUnit = %u\n", settings.vMotionUnit);
    fprintf(stderr, "DEBUG:        h_motionUnit = %u\n", settings.hMotionUnit);
    fprintf(stderr, "DEBUG:      paperReduction = %d\n", settings.paperReduction);
    fprintf(stderr, "DEBUG:       buzzerControl = %d\n", settings.buzzerControl);
    fprintf(stderr, "DEBUG:       drawerControl = %d\n", settings.drawerControl);
    fprintf(stderr, "DEBUG:          cutControl = %d\n", settings.cutControl);
    fprintf(stderr, "DEBUG:        maxBandLines = %u\n", settings.maxBandLines);

    if (err == 0)
        return 0;
    return err == ERR_CANCELED ? -2 : -1;
}
