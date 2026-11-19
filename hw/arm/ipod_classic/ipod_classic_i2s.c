/*
 * S5L8702 I2S transmitter, 0x3CA00000.
 *
 * See include/hw/arm/ipod_classic/ipod_classic_i2s.h for the register map and
 * wiki/14-classic6g-audio.md for the whole audio path and how it was measured.
 *
 * WHAT THIS MODEL DOES
 *
 *  - A register file with honest readback. The retailOS writes CLKCON=1,
 *    TXCON=0x0b100019, TXCOM=0x6 (start), RXCON=0x1000, CLKDIV=272 and reads
 *    I2STXCOM back to confirm the transmitter started; while this block was
 *    create_unimplemented_device every one of those readbacks was zero.
 *
 *  - A TX FIFO sink at +0x10. DMAC0 (a PL080) streams 16-bit PCM samples here.
 *    While the block was unimplemented ~6144 samples per boot fell to the floor.
 *
 *  - PCM capture. With AUDIOWAV=/path.wav set, every sample written to the FIFO
 *    is appended to a 16-bit-stereo WAV whose sample rate is taken from CLKDIV.
 *    That WAV is the hito-3 artefact.
 *
 *  - Pacing (hito 2), when armed. See ipod_classic_i2s_drain(): the FIFO is
 *    drained at the sample rate by a QEMUTimer that gates DMAC0's request line,
 *    so the PL080 hands over one burst at a time and its terminal-count IRQ (16)
 *    reaches the firmware at real playback tempo instead of all at once. This is
 *    what makes the Now Playing progress bar advance with the clock.
 *
 * INSTRUMENTATION: I2STRACE=1 prints every access with its value and guest PC,
 * same pattern as DISPTRACE=1/ATATRACE=1. Independently of the trace, the FIRST
 * non-zero PCM sample of a run is announced on stderr -- that is the event the
 * whole audio investigation waited for.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "audio/audio.h"
#include "hw/arm/ipod_classic/ipod_classic_i2s.h"
#include "hw/arm/ipod_classic/ipod_classic_hle_audio.h"
#include "hw/arm/ipod_classic/ipod_classic_hle_np.h"
#include "hw/arm/ipod_classic/ipod_classic_np_overlay.h"

/* ------------------------------------------------------------- tracing */

static int i2strace = -1;

static bool i2s_trace_on(void)
{
    if (i2strace < 0) {
        const char *e = getenv("I2STRACE");
        i2strace = (e && *e && *e != '0') ? 1 : 0;
    }
    return i2strace == 1;
}

static uint32_t i2s_trace_pc(void)
{
    CPUState *cs = current_cpu;
    return cs ? (uint32_t)CPU_GET_CLASS(cs)->get_pc(cs) : 0;
}

static const char *i2s_reg_name(hwaddr off)
{
    switch (off) {
    case S5L8702_I2S_CLKCON: return "CLKCON";
    case S5L8702_I2S_TXCON:  return "TXCON";
    case S5L8702_I2S_TXCOM:  return "TXCOM";
    case S5L8702_I2S_TXDB0:  return "TXDB0";
    case S5L8702_I2S_RXCON:  return "RXCON";
    case S5L8702_I2S_RXCOM:  return "RXCOM";
    case S5L8702_I2S_RXDB:   return "RXDB";
    case S5L8702_I2S_STATUS: return "STATUS";
    case S5L8702_I2S_CLKDIV: return "CLKDIV";
    default:                 return "";
    }
}

/* --------------------------------------------------------- WAV capture */

/*
 * One WAV file for the run, opened lazily on the first FIFO write. A file-scope
 * pointer lets the atexit handler patch the RIFF/data sizes back into the
 * header; there is only ever one I2S instance, so a static is enough (same shape
 * as DISPTRACE's atexit totals).
 */
static FILE *i2s_wav;
static uint32_t i2s_wav_data_bytes;
static bool i2s_wav_announced;

static void i2s_wav_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/*
 * Snap 12e6/CLKDIV to the nearest standard sample rate for the header. The PCM
 * samples themselves are stored verbatim, so an off-by-17-Hz label (272 gives
 * 44117, snapped to 44100) never touches the audio. CONVENIENCE: the 32 kHz
 * case, which the SoC clocks as 48 kHz with MCLK dropped to 8 MHz, is labelled
 * 48000 here because this helper assumes a 12 MHz MCLK; no test uses it.
 */
static uint32_t i2s_wav_rate_from_div(uint32_t clkdiv)
{
    static const uint32_t std[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
    };
    uint32_t hz, best = 44100, bestd = 0xffffffff;

    if (clkdiv == 0) {
        return 44100;
    }
    hz = S5L8702_I2S_MCLK_HZ / clkdiv;
    for (size_t i = 0; i < ARRAY_SIZE(std); i++) {
        uint32_t d = hz > std[i] ? hz - std[i] : std[i] - hz;
        if (d < bestd) {
            bestd = d;
            best = std[i];
        }
    }
    return best;
}

static void i2s_wav_close(void)
{
    uint8_t hdr[8];

    if (!i2s_wav) {
        return;
    }
    /* Patch RIFF chunk size (+4) and data chunk size (+40). */
    if (fseek(i2s_wav, 4, SEEK_SET) == 0) {
        i2s_wav_put_le32(hdr, 36 + i2s_wav_data_bytes);
        fwrite(hdr, 1, 4, i2s_wav);
    }
    if (fseek(i2s_wav, 40, SEEK_SET) == 0) {
        i2s_wav_put_le32(hdr, i2s_wav_data_bytes);
        fwrite(hdr, 1, 4, i2s_wav);
    }
    fclose(i2s_wav);
    i2s_wav = NULL;
    fprintf(stderr, "AUDIOWAV: wrote %u PCM bytes (%u stereo frames)\n",
            i2s_wav_data_bytes, i2s_wav_data_bytes / 4);
}

static void i2s_wav_open(uint32_t rate)
{
    const char *path = getenv("AUDIOWAV");
    uint8_t h[44];
    uint32_t byte_rate = rate * 2 /*chan*/ * 2 /*bytes*/;

    if (!path || !*path) {
        return;
    }
    i2s_wav = fopen(path, "wb");
    if (!i2s_wav) {
        fprintf(stderr, "AUDIOWAV: cannot open %s\n", path);
        return;
    }
    /* Canonical 44-byte PCM WAV header; sizes patched at close. */
    memcpy(h + 0,  "RIFF", 4);
    i2s_wav_put_le32(h + 4, 36);            /* patched */
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    i2s_wav_put_le32(h + 16, 16);           /* fmt chunk size */
    h[20] = 1; h[21] = 0;                   /* PCM */
    h[22] = 2; h[23] = 0;                   /* stereo */
    i2s_wav_put_le32(h + 24, rate);
    i2s_wav_put_le32(h + 28, byte_rate);
    h[32] = 4; h[33] = 0;                   /* block align = 2ch * 2 bytes */
    h[34] = 16; h[35] = 0;                  /* bits per sample */
    memcpy(h + 36, "data", 4);
    i2s_wav_put_le32(h + 40, 0);            /* patched */
    fwrite(h, 1, sizeof(h), i2s_wav);
    i2s_wav_data_bytes = 0;
    atexit(i2s_wav_close);
    fprintf(stderr, "AUDIOWAV: capturing %u Hz 16-bit stereo to %s\n",
            rate, path);
}

/* Append the low `size` bytes of a FIFO write to the WAV, little-endian. */
static void i2s_wav_sample(IPodClassicI2SState *s, uint64_t value, unsigned size)
{
    uint8_t b[4];

    if (getenv("AUDIOWAV") == NULL) {
        return;
    }
    if (!i2s_wav) {
        i2s_wav_open(i2s_wav_rate_from_div(s->clkdiv));
        if (!i2s_wav) {
            return;
        }
    }
    if (size > 4) {
        size = 4;
    }
    i2s_wav_put_le32(b, (uint32_t)value);
    fwrite(b, 1, size, i2s_wav);
    i2s_wav_data_bytes += size;

    if (!i2s_wav_announced && value != 0) {
        i2s_wav_announced = true;
        fprintf(stderr, "s5l8702.i2s: FIRST NON-ZERO PCM sample 0x%0*x "
                "(pc~0x%08x) -- real audio is reaching the FIFO\n",
                size * 2, (uint32_t)value, i2s_trace_pc());
    }
}

/*
 * I2SCOUNT=1: tally accesses per register instead of printing every one.
 *
 * I2STRACE prints a line per access, which is unusable for exactly the question
 * that matters here -- whether the firmware SPINS on a register. The CLKCON bug
 * above was found that way and needed 161 million reads to show itself; a
 * per-line trace of that is not something anyone can read. The STATUS register
 * at +0x3c is the remaining unknown in this device: its bits are undocumented
 * and the model answers 0 forever, so if the audio driver polls it for a "FIFO
 * has room" or "TX ready" condition it would write one burst and wait for ever
 * -- which is the shape of the bug being chased. Counting answers that in one
 * run. The tally prints on a doubling schedule so a genuine spin is loud and a
 * handful of accesses stays quiet.
 */
static int i2scount = -1;

static bool i2s_count_on(void)
{
    if (i2scount < 0) {
        const char *e = getenv("I2SCOUNT");
        i2scount = (e && *e && *e != '0') ? 1 : 0;
    }
    return i2scount == 1;
}

static void i2s_count_hit(hwaddr off, const char *dir, uint32_t val)
{
    /* One slot per 4-byte register in the low 0x44 bytes; the rest share [17]. */
    static uint64_t rd[18], wr[18];
    uint64_t *tab = (*dir == 'r') ? rd : wr;
    unsigned slot = (off < 0x44) ? (unsigned)(off >> 2) : 17;
    uint64_t n = ++tab[slot];

    /* Powers of two: 1, 2, 4, ... plus every millionth once past that. */
    if ((n & (n - 1)) == 0 || n % 1000000 == 0) {
        fprintf(stderr, "I2SCOUNT %s +0x%02" HWADDR_PRIx " %-7s n=%llu "
                "last=0x%08x\n", dir, off, i2s_reg_name(off),
                (unsigned long long)n, val);
    }
}

/* -------------------------------------------------------------- pacing */

/*
 * Hito 2. Draining the audio DMA at the sample rate so the DMAC's terminal-count
 * IRQ (16) reaches the firmware at real playback tempo instead of all at once,
 * which is what makes a progress bar advance with the clock rather than jump to
 * the end. Gated by AUDIOPACE=1 because it changes the timing of the audio DMA
 * and, until a playback path that produces non-silence exists (the retailOS
 * decodes on the unmodelled CalmADM2E DSP, see wiki/14 §4), its only observable
 * effect is on the boot silence push -- so it is off by default and the default
 * build is byte-for-byte the old behaviour.
 */
static int audiopace = -1;

static bool i2s_pace_on(void)
{
    if (audiopace < 0) {
        const char *e = getenv("AUDIOPACE");
        audiopace = (e && *e && *e != '0') ? 1 : 0;
    }
    return audiopace == 1;
}

static uint32_t i2s_sample_rate(IPodClassicI2SState *s)
{
    return s->clkdiv ? S5L8702_I2S_MCLK_HZ / s->clkdiv : 44100;
}

/* ----------------------------------------------------- live audio out */
/*
 * AUDIOLIVE=1: besides (or instead of) the WAV, feed the same FIFO PCM to a
 * QEMU SWVoiceOut so playback comes out of the host speakers through whatever
 * -audio/-audiodev backend is active (pa/PulseAudio under WSLg). Template:
 * hw/audio/lm4549.c -- register a card, AUD_open_out a 16-bit-stereo voice,
 * push PCM with AUD_write.
 *
 * TWO rules copied from lm4549 that are load-bearing:
 *  (1) The voice MUST be opened at device realize, BEFORE the VM starts running.
 *      AUD_set_active_out() only arms the mixer timer when s->vm_running is set
 *      (audio/audio.c AUD_set_active_out, "if (s->vm_running) audio_reset_timer")
 *      and that flag is set by the audio vm-change-state handler, which fires
 *      once at VM start. A card first registered LAZILY during playback misses
 *      that transition, its timer never starts, and not one sample is clocked
 *      out (the wav backend then holds a 44-byte header; pa stays silent).
 *  (2) Push with AUD_write from the write path (i2s_live_sample) and from the
 *      out callback; AUD_write takes only what the mixer can hold now, the rest
 *      waits in the ring for the next push.
 * One I2S instance exists, so a file-scope struct is enough (like i2s_wav).
 */
static struct {
    QEMUSoundCard card;
    SWVoiceOut *voice;
    bool registered;
    bool primed;             /* jitter cushion built; mixer output active */
    uint32_t rate;
    uint8_t buf[1u << 20];   /* 1 MiB ~= 5.4 s @ 48000 stereo 16-bit */
    uint32_t rd, wr;         /* free-running byte counters */
    uint8_t frame[4];        /* partial 16-bit stereo frame */
    unsigned frame_used;
} i2s_live;

#define I2S_LIVE_MASK ((uint32_t)(sizeof(i2s_live.buf) - 1))
#define I2S_LIVE_DEFAULT_RATE 44100u
/*
 * Startup jitter cushion. The audio DMA is paced on the VIRTUAL clock (one burst
 * per drain tick) while the host mixer pulls on the REAL clock, so their rates
 * match only on average and per-burst jitter would starve the mixer the instant
 * the ring runs dry. We hold the first I2S_LIVE_PRIME_MS of PCM in the ring
 * WITHOUT activating output (an inactive voice does not pull, so the ring keeps
 * filling); once the cushion is built we activate and then let the out callback
 * drain the ring, keeping this much slack permanently in hand. 300 ms adds 300 ms
 * of audio latency (inaudible for music) and the 1 MiB ring holds ~5 s, far above
 * the cushion. Measured: without priming the mixer underran ~90 times/s on this
 * host; with priming a clean run shows 0 underruns and 0 drops.
 *
 * The cushion absorbs JITTER, not a sustained rate deficit: it cannot conjure
 * samples the ARM has not decoded yet. On a host that cannot decode this MP3 at
 * >=1x real time the cushion drains over a few seconds and underruns resume --
 * that is a host-performance ceiling, not a bug (the same PCM still reaches the
 * backend, byte-identical to the WAV; see wiki/16).
 */
#define I2S_LIVE_PRIME_MS 300u

static uint32_t i2s_live_prime_bytes(void)
{
    uint32_t rate = i2s_live.rate ? i2s_live.rate : I2S_LIVE_DEFAULT_RATE;
    uint32_t bytes = (uint32_t)((uint64_t)rate * 4 * I2S_LIVE_PRIME_MS / 1000);
    /* Never ask for a cushion the ring cannot hold. */
    if (bytes > sizeof(i2s_live.buf) / 2) {
        bytes = sizeof(i2s_live.buf) / 2;
    }
    return bytes;
}

static int audiolive = -1;

static bool i2s_live_on(void)
{
    if (audiolive < 0) {
        const char *e = getenv("AUDIOLIVE");
        audiolive = (e && *e && *e != '0') ? 1 : 0;
    }
    return audiolive == 1;
}

static uint32_t i2s_live_used(void)
{
    return i2s_live.wr - i2s_live.rd;
}

/* Hand as much of the ring as the mixer will take right now. */
static void i2s_live_drain(void)
{
    while (i2s_live.voice && i2s_live_used() > 0) {
        uint32_t rdpos = i2s_live.rd & I2S_LIVE_MASK;
        uint32_t contig = (uint32_t)sizeof(i2s_live.buf) - rdpos;
        uint32_t used = i2s_live_used();
        uint32_t chunk = used < contig ? used : contig;
        size_t wrote = AUD_write(i2s_live.voice, &i2s_live.buf[rdpos], chunk);

        if (wrote == 0) {
            break;      /* mixer full; the rest waits for the next push */
        }
        i2s_live.rd += (uint32_t)wrote;
    }
}

/* The mixer has room; hand it what we have. */
static void i2s_live_out_cb(void *opaque, int avail)
{
    (void)avail;
    i2s_live_drain();
}

/* Open (or reconfigure) the voice for `rate`. Called at realize and on a rate
 * change. No-op unless AUDIOLIVE is set. */
static void i2s_live_open(uint32_t rate)
{
    struct audsettings as = {
        .freq = (int)rate,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0,
    };

    if (!i2s_live_on()) {
        return;
    }
    if (!i2s_live.registered) {
        if (!AUD_register_card("s5l8702.i2s", &i2s_live.card, NULL)) {
            return;
        }
        i2s_live.registered = true;
    }
    i2s_live.voice = AUD_open_out(&i2s_live.card, i2s_live.voice,
                                  "s5l8702.i2s.out", NULL,
                                  i2s_live_out_cb, &as);
    if (i2s_live.voice) {
        i2s_live.rate = rate;
    }
}

/* Append the low `size` bytes of a FIFO write to the ring, same bytes as the
 * WAV, then push. The voice was already opened at realize. */
static void i2s_live_sample(IPodClassicI2SState *s, uint64_t value, unsigned size)
{
    uint32_t rate;

    if (!i2s_live_on() || !i2s_live.voice) {
        return;
    }
    rate = i2s_sample_rate(s);
    if (rate != i2s_live.rate) {
        /* Do not combine partial frames across a format change. */
        i2s_live.frame_used = 0;
        i2s_live_open(rate);          /* retune to the real sample rate */
        if (!i2s_live.voice) {
            return;
        }
    }
    if (size > 4) {
        size = 4;
    }
    for (unsigned i = 0; i < size; i++) {
        i2s_live.frame[i2s_live.frame_used++] = (value >> (8 * i)) & 0xff;
        if (i2s_live.frame_used == sizeof(i2s_live.frame)) {
            /*
             * Keep stereo frames intact even when the ring is full. Dropping
             * only one channel would swap left/right in subsequent output.
             * The write counter and ring size are frame-aligned, so a frame
             * never straddles the end of the ring.
             */
            if (i2s_live_used() <= sizeof(i2s_live.buf) -
                                   sizeof(i2s_live.frame)) {
                memcpy(&i2s_live.buf[i2s_live.wr & I2S_LIVE_MASK],
                       i2s_live.frame, sizeof(i2s_live.frame));
                i2s_live.wr += sizeof(i2s_live.frame);
            }
            i2s_live.frame_used = 0;
        }
    }
    /*
     * Priming: while building the startup cushion, hold PCM in the ring and do
     * NOT activate the mixer (an inactive voice does not pull, so the ring keeps
     * filling). Once the cushion is built, activate output ONCE. From then on the
     * mixer's out callback drains the ring at its own real-time cadence.
     *
     * The hot path here runs once per PCM frame (~48000/s), so it must stay cheap:
     * we only append to the ring and never call AUD_write or AUD_set_active_out
     * per sample. Draining from the callback instead keeps this write handler
     * light, which matters because MP3 decode on the emulated ARM is already close
     * to the real-time budget -- extra per-sample work here directly steals the
     * decoder's headroom and causes underruns.
     */
    if (!i2s_live.primed) {
        if (i2s_live_used() < i2s_live_prime_bytes()) {
            return;
        }
        i2s_live.primed = true;
        AUD_set_active_out(i2s_live.voice, 1);
    }
}

/*
 * One drain tick: let DMAC0 hand over one chunk to the FIFO, then schedule the
 * next tick after the wall-clock time that chunk represents at the sample rate.
 * Counting the samples the chunk actually moved (tx_samples delta) makes the
 * cadence self-correcting regardless of the chunk size the firmware chose.
 */
static void ipod_classic_i2s_drain(void *opaque)
{
    IPodClassicI2SState *s = opaque;
    uint64_t before, moved;
    int64_t delay_ns;
    uint32_t rate;

    if (!s->tx_running || !s->dmac0) {
        return;
    }
    /*
     * PAUSE (player control): when the data-path HLE is paused, skip BOTH the
     * fill AND the paced request+commit. Skipping only the fill is a bug -- the
     * DMA would re-clock the stale source buffer, tx_samples/commit/cursor would
     * keep advancing and the codec would buzz the last chunk on loop. Freezing
     * the drain instead stops tx_samples, emits no TXDB0 writes (the live ring
     * drains to silence), and keeps the PCM cursor put -- a clean pause.
     */
    if (ipod_classic_hle_audio_paused()) {
        s->next_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 4000000; /* 4ms */
        timer_mod(s->drain_timer, s->next_tick_ns);
        return;
    }
    /*
     * DATA-PATH HLE: fill the audio DMA channel's current source segment with
     * the next chunk of the host-decoded real song, one burst ahead of the
     * drain, so the paced request below transports the real PCM to the FIFO.
     * A no-op unless HLE_AUDIO_DATAPATH is set and a track has been decoded.
     */
    ipod_classic_hle_audio_fill_dma();

    before = s->tx_samples;
    pl080_pace_request(s->dmac0, S5L8702_I2S_DMA_IIS0_TX);
    moved = s->tx_samples - before;   /* 16-bit samples this chunk */

    /* Advance the HLE PCM cursor by what the burst actually consumed. */
    ipod_classic_hle_audio_commit((uint32_t)moved);

    rate = i2s_sample_rate(s);
    if (moved >= 2) {
        /* stereo: two 16-bit samples per frame */
        uint64_t frames = moved / 2;
        delay_ns = (int64_t)(frames * 1000000000ULL / rate);
        if (delay_ns < 1000) {
            delay_ns = 1000;
        }
    } else {
        /* Nothing moved: channel not enabled yet or drained. Poll again soon. */
        delay_ns = 2000000;   /* 2 ms */
    }
    if (moved > 0 && i2s_trace_on()) {
        fprintf(stderr, "I2STRACE drain: chunk %" PRIu64 " samples "
                "(%u Hz) -> next tick in %" PRId64 " us\n",
                moved, rate, delay_ns / 1000);
    }
    s->next_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns;
    timer_mod(s->drain_timer, s->next_tick_ns);
}

static void i2s_pace_arm(IPodClassicI2SState *s)
{
    if (!i2s_pace_on() || !s->drain_timer) {
        return;
    }
    /* First tick soon; the drain callback then paces itself. */
    timer_mod(s->drain_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
}

static void i2s_pace_disarm(IPodClassicI2SState *s)
{
    if (s->drain_timer) {
        timer_del(s->drain_timer);
    }
}

void ipod_classic_i2s_set_dmac0(IPodClassicI2SState *s, PL080State *dmac0)
{
    s->dmac0 = dmac0;
    /*
     * Mark the IIS0_TX request line paced on DMAC0 so mem->peri transfers to the
     * FIFO wait for our drain ticks. Only under AUDIOPACE; otherwise the line is
     * never marked and the PL080 behaves exactly as before.
     */
    if (dmac0 && i2s_pace_on()) {
        pl080_set_paced_line(dmac0, S5L8702_I2S_DMA_IIS0_TX, true);
    }
}

/* MMIO ------------------------------------------------------------------ */

static uint64_t ipod_classic_i2s_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IPodClassicI2SState *s = opaque;
    uint32_t val;

    switch (offset) {
    case S5L8702_I2S_CLKCON:
        /*
         * Bit 0 is the clock-enable the retailOS writes (see the header). Bit 1
         * is a read-only hardware STATUS bit: "the I2S clock has stopped". The
         * retailOS's IRAM audio driver, when it (re)configures the clock for a
         * real play, DISABLES it first -- `str 0,[CLKCON]` -- and then spins
         *     loop: ldr r0,[CLKCON]; tst r0,#2; beq loop
         * waiting for that status bit before it reprograms CLKDIV/TXCON and
         * re-enables. On real silicon the bit rises as soon as the gated clock
         * settles; our register file only ever returned the written 0, so bit 1
         * stayed clear and the driver span forever at IRAM 0x2200881c -- the
         * exact reason a Shuffle-Songs play reached this driver but never got
         * past it to decode a single PCM frame (measured: 161e6 CLKCON reads,
         * all 0). Model the bit: the clock is "stopped" whenever the enable bit
         * is clear. Boot's silence-init uses the enable path and never polls it;
         * Rockbox drives its own I2S and never reads this bit -- both unaffected.
         */
        val = s->clkcon;
        if (!(s->clkcon & 1)) {
            val |= 2;
        }
        break;
    case S5L8702_I2S_TXCON:  val = s->txcon;  break;
    case S5L8702_I2S_TXCOM:  val = s->txcom;  break;
    case S5L8702_I2S_RXCON:  val = s->rxcon;  break;
    case S5L8702_I2S_RXCOM:  val = s->rxcom;  break;
    case S5L8702_I2S_RXDB:   val = s->rxdb;   break;
    case S5L8702_I2S_CLKDIV: val = s->clkdiv; break;
    case S5L8702_I2S_STATUS:
        /*
         * The status register. Its bit meanings are UNDETERMINED -- no source
         * we have documents them and Rockbox never reads it. Returning 0 (what
         * the unimplemented window did) never hung the boot, so we keep 0 as
         * the base and let I2STRACE during a real play tell us which bit, if
         * any, the Now Playing screen waits on. See wiki/14.
         */
        val = 0;
        break;
    default:
        val = 0;
        break;
    }

    if (i2s_count_on()) {
        i2s_count_hit(offset, "rd", val);
    }
    if (i2s_trace_on()) {
        fprintf(stderr, "I2STRACE rd +0x%02" HWADDR_PRIx " -> 0x%08x  %-7s "
                "pc~0x%08x\n", offset, val, i2s_reg_name(offset),
                i2s_trace_pc());
    }
    return val;
}

static void ipod_classic_i2s_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IPodClassicI2SState *s = opaque;

    switch (offset) {
    case S5L8702_I2S_CLKCON: {
        bool was_en = s->clkcon & 1;
        s->clkcon = value;
        /* Disable edge = the retailOS re-configuring the clock for a real play
         * (boot's silence init never disables). Signal the HLE play trigger. */
        if (was_en && !(value & 1)) {
            ipod_classic_hle_audio_notify_clock_reconfig();
        }
        break;
    }
    case S5L8702_I2S_TXCON:  s->txcon = value;  break;
    case S5L8702_I2S_TXCOM: {
        bool was_running = s->tx_running;
        s->txcom = value;
        s->tx_running = (value & S5L8702_I2S_TXCOM_RUN) != 0;
        if (s->tx_running && !was_running) {
            i2s_pace_arm(s);
            ipod_classic_hle_audio_notify_play_start();
            ipod_classic_hle_np_notify_play_start();
            ipod_classic_np_overlay_notify_play_start();
        } else if (!s->tx_running && was_running) {
            i2s_pace_disarm(s);
        }
        break;
    }
    case S5L8702_I2S_RXCON:  s->rxcon = value;  break;
    case S5L8702_I2S_RXCOM:  s->rxcom = value;  break;
    case S5L8702_I2S_CLKDIV: s->clkdiv = value; break;
    case S5L8702_I2S_TXDB0:
        /* A PCM sample from the DMA (or the CPU). Clock it out. */
        s->tx_samples++;
        i2s_wav_sample(s, value, size);
        i2s_live_sample(s, value, size);
        break;
    default:
        break;
    }

    if (i2s_count_on()) {
        i2s_count_hit(offset, "wr", (uint32_t)value);
    }
    if (i2s_trace_on() && offset != S5L8702_I2S_TXDB0) {
        /* The FIFO gets thousands of writes a boot; count, do not print. */
        fprintf(stderr, "I2STRACE wr +0x%02" HWADDR_PRIx " = 0x%08x  %-7s "
                "pc~0x%08x\n", offset, (uint32_t)value, i2s_reg_name(offset),
                i2s_trace_pc());
    }
}

static const MemoryRegionOps ipod_classic_i2s_ops = {
    .read = ipod_classic_i2s_read,
    .write = ipod_classic_i2s_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void ipod_classic_i2s_reset(DeviceState *dev)
{
    IPodClassicI2SState *s = IPOD_CLASSIC_I2S(dev);

    s->clkcon = 0;
    s->txcon = 0;
    s->txcom = 0;
    s->rxcon = 0;
    s->rxcom = 0;
    s->rxdb = 0;
    s->clkdiv = 0;
    s->tx_running = false;
    s->tx_samples = 0;
    if (s->drain_timer) {
        timer_del(s->drain_timer);
    }
    /* Silence the live-audio voice on reset and re-arm priming; the cushion
     * rebuilds from the next PCM samples. */
    if (i2s_live.voice) {
        AUD_set_active_out(i2s_live.voice, 0);
    }
    i2s_live.primed = false;
    i2s_live.rd = 0;
    i2s_live.wr = 0;
    i2s_live.frame_used = 0;
}

static void ipod_classic_i2s_realize(DeviceState *dev, Error **errp)
{
    IPodClassicI2SState *s = IPOD_CLASSIC_I2S(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &ipod_classic_i2s_ops, s,
                          TYPE_IPOD_CLASSIC_I2S, S5L8702_I2S_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->drain_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  ipod_classic_i2s_drain, s);

    /*
     * Open the live-audio voice HERE, at realize, before the VM starts running,
     * so the audio subsystem's vm-change-state handler sets s->vm_running and
     * AUD_set_active_out() later arms the mixer timer (see i2s_live_open). A
     * default rate is fine; i2s_live_sample retunes to the real I2SCLKDIV rate
     * on the first PCM sample. No-op unless AUDIOLIVE is set.
     */
    i2s_live_open(I2S_LIVE_DEFAULT_RATE);
}

static const VMStateDescription vmstate_ipod_classic_i2s = {
    .name = TYPE_IPOD_CLASSIC_I2S,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(clkcon, IPodClassicI2SState),
        VMSTATE_UINT32(txcon, IPodClassicI2SState),
        VMSTATE_UINT32(txcom, IPodClassicI2SState),
        VMSTATE_UINT32(rxcon, IPodClassicI2SState),
        VMSTATE_UINT32(rxcom, IPodClassicI2SState),
        VMSTATE_UINT32(rxdb, IPodClassicI2SState),
        VMSTATE_UINT32(clkdiv, IPodClassicI2SState),
        VMSTATE_BOOL(tx_running, IPodClassicI2SState),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_i2s_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 I2S transmitter";
    dc->realize = ipod_classic_i2s_realize;
    dc->reset = ipod_classic_i2s_reset;
    dc->vmsd = &vmstate_ipod_classic_i2s;
    dc->user_creatable = false;
}

static const TypeInfo ipod_classic_i2s_info = {
    .name          = TYPE_IPOD_CLASSIC_I2S,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicI2SState),
    .class_init    = ipod_classic_i2s_class_init,
};

static void ipod_classic_i2s_register_types(void)
{
    type_register_static(&ipod_classic_i2s_info);
}

type_init(ipod_classic_i2s_register_types)
