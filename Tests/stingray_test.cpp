// stingray_test.cpp
// Offline checks for the TB Carlsbro Stingray DSP. Builds on Linux or macOS:
//   make -C Tests run      (writes demo WAVs to Tests/renders)
//
// 1. Small-signal behaviour of the preamp: every tone control cuts and boosts
//    its band, Volumes and Master are monotonic, Bright inputs, the mixer
// 2. Suzz: fuzz, compression, Sustain and Gain knobs, the f/switch
// 3. Channel switches
// 4. Power amp: clipping level, current limit, supply sag and recovery
// 5. Stability sweep: control settings x 10 mV-4 V x 41 Hz-15 kHz, plus loud noise
// 6. Automation of every control while playing
// 7. CPU per second of audio
// 8. Demo renders (32-bit float WAV)

#include "../Source/DSP/StingrayAmp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

using namespace cbst;

static const double kPiT = 3.14159265358979323846;
static const int kSr = 44100;
static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok) ++failures;
}

static bool allFinite (const std::vector<float>& x, double limit)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (! (x[i] == x[i]) || std::fabs (x[i]) > limit) return false;
    return true;
}

static double peakFrom (const std::vector<float>& x, size_t from)
{
    double p = 0.0;
    for (size_t i = from; i < x.size(); ++i) p = std::max (p, (double) std::fabs (x[i]));
    return p;
}

static double rmsFrom (const std::vector<float>& x, size_t from)
{
    double s = 0.0;
    for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
    return std::sqrt (s / (double) (x.size() - from));
}

static std::vector<float> sine (double f, double amp, double sec)
{
    std::vector<float> v ((size_t) (sec * kSr));
    for (size_t i = 0; i < v.size(); ++i) v[i] = (float) (amp * std::sin (2.0 * kPiT * f * i / kSr));
    return v;
}

static std::vector<float> run (StingrayAmp& a, const std::vector<float>& in)
{
    std::vector<float> out (in.size());
    const int block = 256;
    for (size_t pos = 0; pos < in.size(); pos += block)
    {
        const int n = (int) ((in.size() - pos) < (size_t) block ? (in.size() - pos) : block);
        a.process (&in[pos], &out[pos], n);
    }
    return out;
}

// Preamp small-signal gains (dB) at f, run at the oversampled rate:
// Ch 1 output, Ch 2 output, mixer output.
struct PreGains { double ch1, ch2, mix; };

static PreGains pre (StingrayAmp& a, double f, double amp = 0.01)
{
    a.resetPreamp();
    const double fs = a.oversampledRate();
    const int n = (int) (fs * 0.5);
    double p1 = 0.0, p2 = 0.0, pm = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double c1, c2, m;
        a.probePreamp (amp * std::sin (2.0 * kPiT * f * i / fs), c1, c2, m);
        if (i > n / 2)
        {
            p1 = std::max (p1, std::fabs (c1));
            p2 = std::max (p2, std::fabs (c2));
            pm = std::max (pm, std::fabs (m));
        }
    }
    PreGains g;
    g.ch1 = 20.0 * std::log10 (p1 / amp + 1e-30);
    g.ch2 = 20.0 * std::log10 (p2 / amp + 1e-30);
    g.mix = 20.0 * std::log10 (pm / amp + 1e-30);
    return g;
}

static void writeWav (const std::string& path, const std::vector<float>& x, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) { std::printf ("  could not write %s\n", path.c_str()); return; }
    const unsigned int dataBytes = (unsigned int) (x.size() * 4);
    const unsigned int riff = 36 + dataBytes;
    const unsigned short fmt = 3, ch = 1, bits = 32, align = 4;
    const unsigned int rate = (unsigned int) sr, bps = rate * 4, fmtLen = 16;
    std::fwrite ("RIFF", 1, 4, f); std::fwrite (&riff, 4, 1, f);
    std::fwrite ("WAVEfmt ", 1, 8, f); std::fwrite (&fmtLen, 4, 1, f);
    std::fwrite (&fmt, 2, 1, f); std::fwrite (&ch, 2, 1, f);
    std::fwrite (&rate, 4, 1, f); std::fwrite (&bps, 4, 1, f);
    std::fwrite (&align, 2, 1, f); std::fwrite (&bits, 2, 1, f);
    std::fwrite ("data", 1, 4, f); std::fwrite (&dataBytes, 4, 1, f);
    std::fwrite (&x[0], 4, x.size(), f);
    std::fclose (f);
}

// Karplus-Strong plucked string as a stand-in for a DI guitar track.
static void pluck (std::vector<float>& buf, size_t start, double freq, double amp, double dur)
{
    const int period = (int) (kSr / freq + 0.5);
    std::vector<double> line (period);
    unsigned int seed = (unsigned int) (freq * 1000.0);
    for (int i = 0; i < period; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        line[i] = ((seed >> 8) / 16777216.0 * 2.0 - 1.0) * amp;
    }
    const size_t n = (size_t) (dur * kSr);
    int pos = 0;
    for (size_t i = 0; i < n && start + i < buf.size(); ++i)
    {
        const int nx = (pos + 1) % period;
        const double v = line[pos];
        line[pos] = 0.4985 * (line[pos] + line[nx]);
        pos = nx;
        buf[start + i] += (float) v;
    }
}

static std::vector<float> riff (double amp)
{
    std::vector<float> g ((size_t) (6.0 * kSr), 0.0f);
    const double notes[] = { 82.4, 110.0, 146.8, 110.0, 82.4, 123.5, 164.8, 98.0 };
    for (int k = 0; k < 8; ++k)
    {
        pluck (g, (size_t) (k * 0.5 * kSr), notes[k], amp, 0.9);
        pluck (g, (size_t) (k * 0.5 * kSr), notes[k] * 1.5, amp * 0.6, 0.9);
    }
    pluck (g, (size_t) (4.2 * kSr), 196.0, amp, 1.8);
    pluck (g, (size_t) (4.2 * kSr), 246.9, amp * 0.8, 1.8);
    pluck (g, (size_t) (4.2 * kSr), 293.7, amp * 0.7, 1.8);
    return g;
}

//==============================================================================
static double toneAt (int which, double v, double f)
{
    StingrayAmp a;
    a.setCh2On (true);
    switch (which)
    {
        case 0: a.setCh1Bass (v); break;
        case 1: a.setMiddle (v); break;
        case 2: a.setCh1Treble (v); break;
        case 3: a.setPresence (v); break;
        case 4: a.setCh2Bass (v); break;
        default: a.setCh2Treble (v); break;
    }
    a.prepare (kSr);
    const PreGains g = pre (a, f);
    return which < 4 ? g.ch1 : g.ch2;
}

static void testPreamp()
{
    std::printf ("1. Preamp small-signal behaviour\n");
    {
        StingrayAmp a;
        a.prepare (kSr);
        check (a.circuitsBuilt(), "every netlist built with all its nodes");
    }

    // Each tone control: cut at -, flat at 0, boost at + in its own band,
    // and it leaves a far-away frequency nearly alone.
    struct T { int which; const char* name; double f; double far; double minRange; };
    const T ts[] = {
        { 0, "Ch 1 Bass",     80.0,   3000.0, 20.0 },
        { 1, "Ch 1 Middle",   700.0,  60.0,   18.0 },
        { 2, "Ch 1 Treble",   8000.0, 100.0,  20.0 },
        { 3, "Ch 1 Presence", 5000.0, 200.0,  15.0 },
        { 4, "Ch 2 Bass",     80.0,   3000.0, 20.0 },
        { 5, "Ch 2 Treble",   8000.0, 100.0,  20.0 },
    };
    for (size_t i = 0; i < sizeof (ts) / sizeof (ts[0]); ++i)
    {
        const double c = toneAt (ts[i].which, 0.0, ts[i].f), m = toneAt (ts[i].which, 0.5, ts[i].f),
                     b = toneAt (ts[i].which, 1.0, ts[i].f);
        const double fc = toneAt (ts[i].which, 0.0, ts[i].far), fb = toneAt (ts[i].which, 1.0, ts[i].far);
        std::printf ("    %-13s -/0/+ @%.0f Hz: %.1f %.1f %.1f dB (@%.0f Hz moves %.1f dB)\n",
                     ts[i].name, ts[i].f, c, m, b, ts[i].far, fb - fc);
        char msg[160];
        std::snprintf (msg, sizeof (msg), "%s cuts and boosts its band (> %.0f dB range), far band moves < 4 dB",
                       ts[i].name, ts[i].minRange);
        check (b - m > ts[i].minRange * 0.4 && m - c > ts[i].minRange * 0.4 && b - c > ts[i].minRange
               && std::fabs (fb - fc) < 4.0, msg);
    }

    // Volumes and Master
    for (int w = 0; w < 3; ++w)
    {
        double prev = -1e9, g0 = 0.0, g10 = 0.0;
        bool mono = true;
        for (int s = 0; s <= 10; ++s)
        {
            StingrayAmp a;
            if (w == 0) a.setCh1Volume (s / 10.0);
            else if (w == 1) { a.setCh1On (false); a.setCh2On (true); a.setCh2Volume (s / 10.0); }
            else a.setMaster (s / 10.0);
            a.prepare (kSr);
            const PreGains g = pre (a, 1000.0);
            const double v = w == 0 ? g.ch1 : (w == 1 ? g.ch2 : g.mix);
            if (v <= prev) mono = false;
            prev = v;
            if (s == 0) g0 = v;
            if (s == 10) g10 = v;
        }
        const char* nm = w == 0 ? "Ch 1 Volume" : (w == 1 ? "Ch 2 Volume" : "Master Volume");
        std::printf ("    %-13s 0 -> 10 @1 kHz: %.1f -> %.1f dB\n", nm, g0, g10);
        char msg[96];
        std::snprintf (msg, sizeof (msg), "%s monotonic over 0..10, range > 50 dB", nm);
        check (mono && g10 - g0 > 50.0, msg);
    }

    // Bright inputs: more top, the same lows
    for (int ch = 0; ch < 2; ++ch)
    {
        StingrayAmp n, b;
        n.setCh2On (true); b.setCh2On (true);
        if (ch == 0) b.setCh1Bright (true); else b.setCh2Bright (true);
        n.prepare (kSr); b.prepare (kSr);
        const PreGains n200 = pre (n, 200.0), b200 = pre (b, 200.0), n6k = pre (n, 6000.0), b6k = pre (b, 6000.0);
        const double d200 = ch == 0 ? b200.ch1 - n200.ch1 : b200.ch2 - n200.ch2;
        const double d6k = ch == 0 ? b6k.ch1 - n6k.ch1 : b6k.ch2 - n6k.ch2;
        std::printf ("    Ch %d Bright vs Normal: 200 Hz %+.1f dB, 6 kHz %+.1f dB\n", ch + 1, d200, d6k);
        char msg[96];
        std::snprintf (msg, sizeof (msg), "Ch %d Bright input lifts 6 kHz by > 3 dB, 200 Hz within 1 dB", ch + 1);
        check (d6k > 3.0 && std::fabs (d200) < 1.0, msg);
    }

    // Mixer: both channels add
    {
        StingrayAmp both;
        both.setCh2On (true);
        both.prepare (kSr);
        const PreGains g = pre (both, 400.0);
        std::printf ("    mixer at 400 Hz: Ch 1 %.1f, Ch 2 %.1f, mixed (Master 5) %.1f dB\n", g.ch1, g.ch2, g.mix);
        check (std::fabs (g.mix - (g.ch1 + 6.02 - 20.0)) < 1.0, "mixer sums both channels at unity, then Master");
    }
}

static double ch1Level (StingrayAmp& a, double amp, double f = 200.0)
{
    return std::pow (10.0, pre (a, f, amp).ch1 / 20.0) * amp;
}

// Fraction of the energy that is not at the fundamental (a rough THD)
static double distortion (StingrayAmp& a, double amp, double f)
{
    a.resetPreamp();
    const double fs = a.oversampledRate();
    const int n = (int) (fs * 0.4);
    double sc = 0.0, ss = 0.0, tot = 0.0;
    int cnt = 0;
    for (int i = 0; i < n; ++i)
    {
        double c1, c2, m;
        a.probePreamp (amp * std::sin (2.0 * kPiT * f * i / fs), c1, c2, m);
        if (i >= n / 2)
        {
            const double ph = 2.0 * kPiT * f * i / fs;
            sc += c1 * std::cos (ph); ss += c1 * std::sin (ph); tot += c1 * c1; ++cnt;
        }
    }
    const double fund = 2.0 * (sc * sc + ss * ss) / ((double) cnt * cnt);
    const double all = tot / cnt;
    return all > 0.0 ? std::sqrt (std::max (0.0, all - fund) / all) : 0.0;
}

static void testSuzz()
{
    std::printf ("2. Suzz\n");
    StingrayAmp clean, fz;
    fz.setSuzzOn (true);
    clean.prepare (kSr); fz.prepare (kSr);
    const double dc = distortion (clean, 0.1, 200.0), df = distortion (fz, 0.1, 200.0);
    std::printf ("    100 mV at 200 Hz: distortion clean %.3f, Suzz %.3f\n", dc, df);
    check (dc < 0.02 && df > 0.25, "Suzz on: heavy fuzz where the clean channel is clean");

    const double lo = ch1Level (fz, 0.01), hi = ch1Level (fz, 1.0), cl = ch1Level (clean, 0.1);
    std::printf ("    Ch 1 out: Suzz 10 mV -> %.3f V, 1 V -> %.3f V; clean 100 mV -> %.3f V\n", lo, hi, cl);
    check (20.0 * std::log10 (hi / lo) < 8.0, "Suzz compresses: 40 dB more input, < 8 dB more output");
    check (hi > cl * 0.3 && hi < cl * 4.0, "Suzz at Gain 8.5 sits near the clean channel's level");

    double s[3], g[3];
    for (int k = 0; k < 3; ++k)
    {
        StingrayAmp a, b;
        a.setSuzzOn (true); a.setSuzzSustain (k * 0.5);
        b.setSuzzOn (true); b.setSuzzGain (k * 0.5);
        a.prepare (kSr); b.prepare (kSr);
        s[k] = ch1Level (a, 0.002);
        g[k] = ch1Level (b, 0.3);
    }
    std::printf ("    2 mV in, Sustain 0/5/10: %.3f %.3f %.3f V\n", s[0], s[1], s[2]);
    check (s[2] > s[1] && s[1] > s[0], "Sustain raises the drive (more output from a quiet note)");
    std::printf ("    300 mV in, Gain 0/5/10: %.4f %.3f %.3f V\n", g[0], g[1], g[2]);
    check (g[2] > g[1] && g[1] > g[0] && g[0] < g[2] * 0.01, "Gain sets the Suzz level, from silent to full");
}

static void testSwitches()
{
    std::printf ("3. Channel switches\n");
    {
        StingrayAmp a, b;
        b.setCh2Volume (1.0); b.setCh2Treble (1.0);
        a.prepare (kSr); b.prepare (kSr);
        const std::vector<float> in = sine (330.0, 0.1, 0.5);
        const std::vector<float> oa = run (a, in), ob = run (b, in);
        double d = 0.0;
        for (size_t i = 0; i < oa.size(); ++i) d = std::max (d, (double) std::fabs (oa[i] - ob[i]));
        check (d < 1.0e-6, "Ch 2 switched off: its controls have no effect");

        StingrayAmp c;
        c.setCh1On (false);
        c.prepare (kSr);
        const std::vector<float> oc = run (c, in);
        check (peakFrom (oc, oc.size() / 2) < 1.0e-6, "both channels off: silence");

        StingrayAmp e;
        e.setCh1On (false); e.setCh2On (true);
        e.prepare (kSr);
        const std::vector<float> oe = run (e, in);
        check (peakFrom (oe, oe.size() / 2) > 0.005, "Ch 2 alone plays");
    }
    {
        StingrayAmp a;
        a.prepare (kSr);
        const std::vector<float> in = sine (196.0, 0.3, 3.0);
        std::vector<float> out (in.size());
        const int block = 128;
        for (size_t pos = 0, k = 0; pos < in.size(); pos += block, ++k)
        {
            if (k % 37 == 0) a.setSuzzOn ((k / 37) % 2 == 1);
            if (k % 53 == 0) a.setCh2On ((k / 53) % 2 == 1);
            if (k % 71 == 0) a.setCh1Bright ((k / 71) % 2 == 1);
            if (k % 79 == 0) a.setCh2Bright ((k / 79) % 2 == 1);
            if (k % 89 == 0) a.setCh1On ((k / 89) % 3 != 2);
            if (k % 97 == 0) a.setSpeaker ((k / 97) % 2 == 0);
            const int n = (int) std::min ((size_t) block, in.size() - pos);
            a.process (&in[pos], &out[pos], n);
        }
        check (allFinite (out, 4.0), "every switch toggled while playing: finite, bounded");
    }
}

static void testPowerAmp()
{
    std::printf ("4. Power amp\n");
    StingrayAmp a;
    a.setCh1Volume (1.0);
    a.setMaster (1.0);
    a.setSpeaker (false);
    a.prepare (kSr);
    const std::vector<float> in = sine (220.0, 0.3, 1.0);
    a.clearPeakAmps();
    const std::vector<float> o = run (a, in);
    const double vpk = peakFrom (o, o.size() / 2) * 40.0;
    std::printf ("    flat out: %.1f V peak at the speaker, %.2f A peak, rails %.1f V\n", vpk, a.peakSpeakerAmps(), a.railPositive());
    check (vpk > 36.0 && vpk < 47.0, "clips at 36-47 V peak into the 8 ohm 2x12 (about 100 W)");
    check (a.peakSpeakerAmps() < 8.01, "speaker current within the V-I limit");
    check (a.railPositive() < 47.5, "rails sag under full load");
    std::vector<float> quiet (kSr, 0.0f);
    run (a, quiet);
    check (a.railPositive() > 46.0, "rails recover after the note");

    StingrayAmp b;
    b.setSpeaker (false);
    b.prepare (kSr);
    const std::vector<float> ob = run (b, sine (220.0, 0.05, 0.5));
    const double vb = peakFrom (ob, ob.size() / 2) * 40.0;
    std::printf ("    50 mV guitar, Volume 5, Master 5: %.2f V peak\n", vb);
    check (vb > 0.1 && vb < 30.0, "moderate settings play cleanly below clipping");
}

static void setAll (StingrayAmp& a, const double* v)
{
    a.setCh1Volume (v[0]); a.setPresence (v[1]); a.setCh1Treble (v[2]); a.setMiddle (v[3]);
    a.setCh1Bass (v[4]); a.setSuzzGain (v[5]); a.setSuzzSustain (v[6]); a.setCh2Volume (v[7]);
    a.setCh2Treble (v[8]); a.setCh2Bass (v[9]); a.setMaster (v[10]);
}

static void testStability()
{
    std::printf ("5. Stability sweep\n");
    unsigned int seed = 12345u;
    bool ok = true;
    const double amps[] = { 0.01, 0.3, 1.5, 4.0 };
    const double freqs[] = { 41.0, 330.0, 2500.0, 15000.0 };
    int runs = 0;
    for (int k = 0; k < 24; ++k)
    {
        StingrayAmp a;
        double v[11];
        for (int i = 0; i < 11; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            v[i] = (k < 2) ? (double) k : (seed >> 8) / 16777216.0;
        }
        setAll (a, v);
        a.setSuzzOn (k % 2 == 0); a.setCh2On (k % 3 != 0);
        a.setCh1Bright (k % 5 == 0); a.setCh2Bright (k % 7 == 0); a.setSpeaker (k % 2 == 1);
        a.prepare (k % 3 == 0 ? 96000.0 : kSr);
        for (int ai = 0; ai < 4; ++ai)
            for (int fi = 0; fi < 4; ++fi)
            {
                const std::vector<float> o = run (a, sine (freqs[fi], amps[ai], 0.15));
                if (! allFinite (o, 3.0)) ok = false;
                ++runs;
            }
        std::vector<float> noise ((size_t) (0.3 * kSr));
        for (size_t i = 0; i < noise.size(); ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            noise[i] = (float) (((seed >> 8) / 16777216.0 * 2.0 - 1.0) * 4.0);
        }
        if (! allFinite (run (a, noise), 3.0)) ok = false;
    }
    {
        const double rates[] = { 1000.0, 8000.0, 11025.0, 22050.0, 192000.0, 384000.0, 768000.0 };
        for (int k = 0; k < 7; ++k)
        {
            StingrayAmp a;
            a.setCh2On (true); a.setSuzzOn (k % 2 == 0); a.setCh1Volume (1.0); a.setMaster (1.0);
            a.prepare (rates[k]);
            std::vector<float> noise ((size_t) (rates[k] * 0.05) + 64);
            for (size_t i = 0; i < noise.size(); ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                noise[i] = (float) (((seed >> 8) / 16777216.0 * 2.0 - 1.0) * 0.5);
            }
            if (! allFinite (run (a, noise), 3.0)) { ok = false; std::printf ("    unstable at %.0f Hz\n", rates[k]); }
        }
    }
    std::printf ("    %d sine runs + 24 loud noise runs + 7 sample rates from 1 kHz to 768 kHz\n", runs);
    check (ok, "no NaN, no runaway, output under +10 dBFS for every setting");
}

static void testAutomation()
{
    std::printf ("6. Automation\n");
    StingrayAmp a;
    a.setCh2On (true);
    a.setSuzzOn (true);
    a.prepare (kSr);
    const std::vector<float> in = riff (0.3);
    std::vector<float> out (in.size());
    const int block = 64;
    for (size_t pos = 0; pos < in.size(); pos += block)
    {
        const double t = (double) pos / kSr;
        const double s = 0.5 + 0.5 * std::sin (t * 7.0);
        double v[11];
        for (int i = 0; i < 11; ++i) v[i] = (i % 2) ? s : 1.0 - s;
        setAll (a, v);
        a.setInputGainDb (-12.0 + 24.0 * s); a.setOutputGainDb (-6.0 * s);
        const int n = (int) std::min ((size_t) block, in.size() - pos);
        a.process (&in[pos], &out[pos], n);
    }
    check (allFinite (out, 3.0), "every knob swept while playing: finite, bounded");
}

static void testCpu()
{
    std::printf ("7. CPU\n");
    StingrayAmp a;
    a.setCh2On (true);
    a.setSuzzOn (true);
    a.prepare (kSr);
    const std::vector<float> in = riff (0.3);
    const std::clock_t t0 = std::clock();
    run (a, in);
    const double sec = (double) (std::clock() - t0) / CLOCKS_PER_SEC;
    const double perSec = sec / ((double) in.size() / kSr);
    std::printf ("    %.1f ms per second of audio (both channels and the Suzz on, one side)\n", perSec * 1000.0);
#if defined(__SANITIZE_ADDRESS__)
    std::printf ("    (sanitizer build: timing not checked)\n");
#else
    check (perSec < 0.5, "runs well within real time");
#endif
}

static void renders (const char* dir)
{
    std::printf ("8. Demo renders\n");
    const std::vector<float> g = riff (0.25);
    struct R { const char* name; double vol1; bool suzz; bool ch2; double vol2; bool bright; double master; };
    const R rs[] = {
        { "ch1_clean",        0.4, false, false, 0.0, false, 0.6 },
        { "ch1_bright_input", 0.4, false, false, 0.0, true,  0.6 },
        { "ch1_cranked",      1.0, false, false, 0.0, false, 0.5 },
        { "ch1_suzz",         0.5, true,  false, 0.0, false, 0.6 },
        { "ch2_clean",        0.0, false, true,  0.5, false, 0.6 },
        { "both_channels",    0.4, false, true,  0.4, false, 0.6 },
    };
    for (size_t i = 0; i < sizeof (rs) / sizeof (rs[0]); ++i)
    {
        StingrayAmp a;
        a.setCh1Volume (rs[i].vol1); a.setCh1On (rs[i].vol1 > 0.0); a.setSuzzOn (rs[i].suzz);
        a.setCh2On (rs[i].ch2); a.setCh2Volume (rs[i].vol2); a.setCh1Bright (rs[i].bright);
        a.setMaster (rs[i].master);
        a.prepare (kSr);
        const std::vector<float> o = run (a, g);
        const std::string path = std::string (dir) + "/" + rs[i].name + ".wav";
        writeWav (path, o, kSr);
        std::printf ("    %-18s peak %.3f  rms %.3f\n", rs[i].name, peakFrom (o, 0), rmsFrom (o, 0));
    }
    writeWav (std::string (dir) + "/dry_guitar.wav", g, kSr);
}

int main (int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "renders";
    testPreamp();
    testSuzz();
    testSwitches();
    testPowerAmp();
    testStability();
    testAutomation();
    testCpu();
    renders (dir);
    std::printf ("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
