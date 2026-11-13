// StingrayAmp.cpp
// No schematic yet: see StingrayAmp.h. Every constant in the "Estimates"
// block stands in for a value the drawing would give; the Suzz values are
// the Carlsbro Suzz pedal's own (Gottfried Divos's drawing, as in TB Suzz).

#include "StingrayAmp.h"

#include <cmath>
#include <cstring>

namespace cbst
{

namespace
{
    const int kPotUpdateInterval = 16;

    // 741 op-amps on +/-15 V rails
    const double kGbw741   = 1.0e6;    // Hz
    const double kSlew741  = 0.5;      // V/us
    const double kSwing741 = 13.5;     // V
    const double kKnee741  = 0.8;      // V
    const double kRout741  = 75.0;     // ohms

    const double kPickupR  = 6000.0;   // guitar pickup DC resistance (guitar volume on full)
    const double kOpen     = 1.0e9;    // a switch contact that is open
    const double kClosed   = 0.05;     // a switch contact that is closed

    // ===== Estimates (no schematic yet) =======================================
    // Input stage, either channel: the jack feeds a 100K series resistor into
    // a non-inverting 741 (100K bias); the Bright jack puts 1n across the
    // series resistor, lifting everything above about 1.6 kHz by up to 6 dB.
    const double kInSeriesR = 100.0e3;
    const double kInBiasR   = 100.0e3;
    const double kInBrightC = 1.0e-9;
    const double kInCouple  = 100.0e-9;
    const double kInFbR     = 100.0e3;     // gain 1 + 100K / 4K7 = x22
    const double kInGndR    = 4700.0;
    const double kInGndC    = 4.7e-6;

    // Suzz: the input stage's output padded back down to guitar level, then
    // the pedal's circuit, then make-up gain back to the clean channel's level.
    const double kSuzzPadTop = 22000.0, kSuzzPadBot = 2200.0;
    const double kSuzzMakeup = 16.0;

    // Ch 1 active tone controls (Baxandall-type cut/boost, 0 at the top)
    const double kBassF   = 100.0,  kBassDb   = 15.0;    // low shelf
    const double kMidF    = 700.0,  kMidQ     = 0.7, kMidDb = 12.0;   // peak
    const double kTrebF   = 3000.0, kTrebDb   = 15.0;    // high shelf
    const double kPresF   = 5000.0, kPresQ    = 0.9, kPresDb = 10.0;  // peak
    // Ch 2: Bass and Treble the same as Ch 1

    // Recovery 741 after each tone section, then the mixer
    const double kRecGain  = 6.0;
    const double kRecHP    = 8.0;          // Hz, coupling capacitor
    const double kMixInR   = 100.0e3;      // inverting mixer, unity per channel
    const double kMixFbR   = 100.0e3;

    // Power amp (100 W into 8 ohms)
    const double kPaGain     = 22.0;       // about 1.3 V rms after Master for full power
    const double kRailNoLoad = 48.0;       // V
    const double kRailC      = 4700.0e-6;
    const double kRailRs     = 2.5;        // transformer + bridge source resistance
    const double kRailDrop   = 3.0;        // V lost in the output stage
    const double kRailKnee   = 1.5;        // V
    const double kPaSlew     = 2.0;        // V/us at the output
    const double kIMax       = 8.0;        // A, V-I limiting
    const double kIQuiet     = 0.05;       // A per rail at idle

    // Two Carlsbro Powertone 60 12" (8 ohms as a pair), Thiele-Small estimates
    const double kSpkRe = 6.4, kSpkFs = 85.0, kSpkQms = 4.0, kSpkQes = 0.6, kSpkLe = 0.6e-3;
    // ===========================================================================

    // Suzz pedal values (from the drawing, as TB Suzz)
    const double kSzR1 = 120.0e3, kSzR2 = 120.0e3, kSzC1 = 10.0e-9;
    const double kSzR3 = 470.0e3, kSzC3 = 47.0e-12;
    const double kSzR4 = 470.0, kSzC2 = 1.0e-6, kSzP1 = 22.0e3;
    const double kSzC4 = 4.7e-6, kSzR5 = 100.0e3, kSzR6 = 3.3e3, kSzC5 = 220.0e-9;
    const double kSzP2 = 470.0;
    const double kSzSwingUp = 3.0, kSzSwingDown = 2.6, kSzKnee = 0.4;   // 741 on 9 V

    const double kVFullScale = 40.0;       // V peak at the speaker = 1.0 out

    inline double clamp01 (double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
    inline double dbToGain (double dB) { return std::pow (10.0, dB / 20.0); }
    inline double potR (double r) { return r < 10.0 ? 10.0 : r; }
    inline double cutBoost (double v) { return clamp01 (v) * 2.0 - 1.0; }   // -1..+1, 0 at the top

    // A 741 output against its rails: linear, then rounding off over the knee
    inline double rail741 (double v)
    {
        const double lin = kSwing741 - kKnee741;
        const double av = std::fabs (v);
        if (av <= lin) return v;
        const double y = lin + kKnee741 * (1.0 - std::exp (-(av - lin) / kKnee741));
        return v < 0.0 ? -y : y;
    }
}

// Log ("audio") taper: 10% of the track at mid-rotation.
double StingrayAmp::logTaper (double x)
{
    const double k = 4.394449154672439;
    x = clamp01 (x);
    return (std::exp (k * x) - 1.0) / (std::exp (k) - 1.0);
}

StingrayAmp::StingrayAmp()
    : fsBase (44100.0), fsOs (176400.0), built (false),
      railPos (kRailNoLoad), railNeg (kRailNoLoad), vOutPrev (0.0), peakAmps (0.0),
      slewStep (1.0), climHit (false),
      coneVPrev (0.0), cabNorm (1.0), spkMix (1.0),
      suzzOn (false), bright1 (false), bright2 (false), ch1On (true), ch2On (false), speakerOn (true),
      appB1 (false), appB2 (false),
      suzzMix (0.0), suzzRunning (false),
      ch1Gain (1.0), ch2Gain (0.0), ch1Running (true), ch2Running (false),
      inGainT (1.0), inGainC (1.0), outGainT (1.0), outGainC (1.0),
      smoothCoeff (0.001), potCountdown (0)
{
    // Panel defaults: volumes and tone at 5 / 0, Suzz Gain 8.5, Sustain 7.5
    const double defaults[kNumKnobs] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.85, 0.75, 0.5, 0.5, 0.5, 0.5 };
    for (int i = 0; i < kNumKnobs; ++i)
        knobTarget[i] = knobCur[i] = defaults[i];
    buildCircuits();
}

//==============================================================================
void StingrayAmp::buildCircuits()
{
    // ---- Input stages
    for (int ch = 0; ch < 2; ++ch)
    {
        Netlist& n = ch == 0 ? in1 : in2;
        n.clear();
        const int s = n.addNode(), a = n.addNode(), bc = n.addNode(), p = n.addNode(),
                  m = n.addNode(), g = n.addNode(), o = n.addNode();
        n.addInput (s, kPickupR);
        n.addC (s, a, kInCouple);
        const int eN = n.addR (a, p, kInSeriesR);     // series resistor (both jacks)
        n.addC (a, bc, kInBrightC);                   // Bright jack: cap across it
        const int eB = n.addR (bc, p, kOpen);
        n.addR (p, 0, kInBiasR);
        n.addR (o, m, kInFbR);
        n.addR (m, g, kInGndR);
        n.addC (g, 0, kInGndC);
        n.setOpAmp (p, m, o, kGbw741, kSlew741, kSwing741, kKnee741);
        if (ch == 0) { in1Out = o; eN1 = eN; eB1 = eB; }
        else         { in2Out = o; eN2 = eN; eB2 = eB; }
    }

    // ---- Suzz: pad, then the Suzz pedal's circuit (see TB Suzz)
    {
        Netlist& n = suzz; n.clear();
        const int s = n.addNode(), w = n.addNode(), p3 = n.addNode(), p2 = n.addNode(),
                  a = n.addNode(), b = n.addNode(), o = n.addNode(), c = n.addNode(), d = n.addNode();
        n.addInput (s, kRout741);
        n.addR (s, w, kSuzzPadTop);
        n.addR (w, 0, kSuzzPadBot);
        n.addC (w, p3, kSzC1);
        n.addR (p3, 0, kSzR1);
        n.addR (p3, 0, kSzR2);
        n.addR (o, p2, kSzR3);
        n.addC (o, p2, kSzC3);
        n.addR (p2, a, kSzR4);
        n.addC (a, b, kSzC2);
        eSusPot = n.addR (b, 0, kSzP1);
        n.addC (o, c, kSzC4);
        n.addR (c, 0, kSzR5);
        n.addR (c, d, kSzR6);
        n.addC (d, 0, kSzC5);
        n.addR (d, 0, kSzP2);
        n.setOpAmp (p3, p2, o, kGbw741, kSlew741, kSzSwingUp, kSzKnee);
        n.setOpAmpSwing (kSzSwingUp, kSzSwingDown);
        suzzOut = d;
        suzzIc = o;
    }

    // ---- Mixer: inverting 741
    {
        Netlist& n = mix; n.clear();
        const int m = n.addNode(), o = n.addNode();
        n.addInput (m, kMixInR);
        n.addInput (m, kMixInR);
        n.addR (m, o, kMixFbR);
        n.setOpAmp (0, m, o, kGbw741, kSlew741, kSwing741, kKnee741);
        mixOut = o;
    }

    // ---- Output network and loudspeaker load: Zobel, then the 2x12 as Re,
    // lossy voice-coil inductance and the motional impedance (parallel R-L-C).
    {
        Netlist& n = speaker; n.clear();
        const int o = n.addNode(), z = n.addNode(), p = n.addNode(), q = n.addNode();
        n.addInput (o, 0.01);
        n.addR (o, z, 10.0);
        n.addC (z, 0, 0.1e-6);
        const double ws = 2.0 * kPi * kSpkFs;
        const double cmes = kSpkQes / (ws * kSpkRe);
        const double lces = 1.0 / (ws * ws * cmes);
        const double res = kSpkRe * kSpkQms / kSpkQes;
        n.addR (o, p, kSpkRe);
        n.addL (p, q, kSpkLe);
        n.addR (p, q, 20.0);
        n.addR (q, 0, res);
        n.addL (q, 0, lces);
        n.addC (q, 0, cmes);
        spkOut = o;
        spkCone = q;
    }

    built = in1Out > 0 && in2Out > 0 && suzzOut > 0 && mixOut > 0 && spkCone > 0
         && eSusPot >= 0 && eB1 >= 0 && eB2 >= 0;
}

//==============================================================================
void StingrayAmp::prepare (double sr)
{
    fsBase = sr;
    fsOs = sr * Oversampler4x::kFactor;
    smoothCoeff = 1.0 - std::exp (-1.0 / (0.02 * sr));

    in1.prepare (fsOs); in2.prepare (fsOs); suzz.prepare (fsOs);
    mix.prepare (fsOs); speaker.prepare (fsOs);

    rec1HP.setHighPass1 (fsOs, kRecHP);
    rec2HP.setHighPass1 (fsOs, kRecHP);
    paInHP.setHighPass1 (fsOs, 10.0);
    paGbwLP.setLowPass1 (fsOs, kGbw741 / kPaGain * 1.5);
    slewStep = kPaSlew * 1.0e6 / fsOs;
    designTone();

    // Cone breakup and the open-backed head-and-cab voice (estimates)
    cab[0].setPeak (fsBase, 200.0, 1.0, 1.5);
    cab[1].setPeak (fsBase, 1200.0, 1.0, 2.0);
    cab[2].setPeak (fsBase, 2500.0, 1.2, 6.0);
    cab[3].setPeak (fsBase, 4000.0, 1.8, 3.0);
    const double lpF = fsBase * 0.45 < 5500.0 ? fsBase * 0.45 : 5500.0;
    cab[4].setLowPass (fsBase, lpF, 0.7);
    cab[5].setLowPass (fsBase, lpF * 1.15, 1.0);

    // Normalise the miked speaker so 1 kHz matches the speaker-terminal level
    {
        const double fNorm = fsBase * 0.2 < 1000.0 ? fsBase * 0.2 : 1000.0;
        speaker.reset();
        for (int i = 0; i < 6; ++i) cab[i].reset();
        double prev = 0.0, peak = 0.0;
        const int nBase = (int) (0.12 * fsBase);
        const int F = Oversampler4x::kFactor;
        for (int i = 0; i < nBase; ++i)
        {
            double last = 0.0;
            for (int k = 0; k < F; ++k)
            {
                const double tt = (double) (i * F + k) / fsOs;
                const double vin = std::sin (2.0 * kPi * fNorm * tt);
                speaker.process (&vin);
                const double vc = speaker.v (spkCone);
                last = (vc - prev) * fsOs;
                prev = vc;
            }
            double y = last;
            for (int b = 0; b < 6; ++b) y = cab[b].process (y);
            if (i > nBase / 2 && std::fabs (y) > peak) peak = std::fabs (y);
        }
        cabNorm = peak > 0.0 ? 1.0 / peak : 1.0;
    }

    reset();
}

void StingrayAmp::reset()
{
    for (int i = 0; i < kNumKnobs; ++i) knobCur[i] = knobTarget[i];
    inGainC = inGainT;
    outGainC = outGainT;
    applySwitches();
    applyPots();

    resetPreamp();
    speaker.reset();
    osMain.reset(); osSpkDown.reset();
    spkMix = speakerOn ? 1.0 : 0.0;
    suzzMix = suzzOn ? 1.0 : 0.0;
    suzzRunning = suzzOn;
    ch1Gain = ch1On ? 1.0 : 0.0;
    ch2Gain = ch2On ? 1.0 : 0.0;
    ch1Running = ch1On;
    ch2Running = ch2On;

    paInHP.reset(); paGbwLP.reset();
    for (int i = 0; i < 6; ++i) cab[i].reset();

    railPos = railNeg = kRailNoLoad;
    vOutPrev = 0.0; coneVPrev = 0.0;
    climHit = false;
    potCountdown = 0;
}

void StingrayAmp::resetPreamp()
{
    in1.reset(); in2.reset(); suzz.reset(); mix.reset();
    pres1.reset(); treb1.reset(); mid1.reset(); bass1.reset();
    treb2.reset(); bass2.reset();
    rec1HP.reset(); rec2HP.reset();
}

//==============================================================================
void StingrayAmp::designTone()
{
    bass1.setLowShelf (fsOs, kBassF, cutBoost (knobCur[kB1]) * kBassDb);
    mid1.setPeak (fsOs, kMidF, kMidQ, cutBoost (knobCur[kMid]) * kMidDb);
    treb1.setHighShelf (fsOs, kTrebF, cutBoost (knobCur[kT1]) * kTrebDb);
    pres1.setPeak (fsOs, kPresF, kPresQ, cutBoost (knobCur[kPres]) * kPresDb);
    bass2.setLowShelf (fsOs, kBassF, cutBoost (knobCur[kB2]) * kBassDb);
    treb2.setHighShelf (fsOs, kTrebF, cutBoost (knobCur[kT2]) * kTrebDb);
}

void StingrayAmp::applyPots()
{
    // Suzz SUSTAIN: P1 22K reverse log rheostat, clockwise = less resistance, more gain
    suzz.setR (eSusPot, potR (kSzP1 * logTaper (1.0 - knobCur[kSSus])));
    suzz.rebuild();
    designTone();
}

void StingrayAmp::applySwitches()
{
    in1.setR (eB1, bright1 ? kClosed : kOpen);
    in2.setR (eB2, bright2 ? kClosed : kOpen);
    in1.setR (eN1, kInSeriesR);
    in2.setR (eN2, kInSeriesR);
    in1.rebuild(); in2.rebuild();
    appB1 = bright1; appB2 = bright2;
}

//==============================================================================
void StingrayAmp::setCh1Volume (double v)   { knobTarget[kV1]    = clamp01 (v); }
void StingrayAmp::setPresence (double v)    { knobTarget[kPres]  = clamp01 (v); }
void StingrayAmp::setCh1Treble (double v)   { knobTarget[kT1]    = clamp01 (v); }
void StingrayAmp::setMiddle (double v)      { knobTarget[kMid]   = clamp01 (v); }
void StingrayAmp::setCh1Bass (double v)     { knobTarget[kB1]    = clamp01 (v); }
void StingrayAmp::setSuzzGain (double v)    { knobTarget[kSGain] = clamp01 (v); }
void StingrayAmp::setSuzzSustain (double v) { knobTarget[kSSus]  = clamp01 (v); }
void StingrayAmp::setCh2Volume (double v)   { knobTarget[kV2]    = clamp01 (v); }
void StingrayAmp::setCh2Treble (double v)   { knobTarget[kT2]    = clamp01 (v); }
void StingrayAmp::setCh2Bass (double v)     { knobTarget[kB2]    = clamp01 (v); }
void StingrayAmp::setMaster (double v)      { knobTarget[kMast]  = clamp01 (v); }
void StingrayAmp::setSuzzOn (bool on)       { suzzOn = on; }
void StingrayAmp::setCh1Bright (bool b)     { bright1 = b; }
void StingrayAmp::setCh1On (bool on)        { ch1On = on; }
void StingrayAmp::setCh2Bright (bool b)     { bright2 = b; }
void StingrayAmp::setCh2On (bool on)        { ch2On = on; }
void StingrayAmp::setSpeaker (bool on)      { speakerOn = on; }
void StingrayAmp::setInputGainDb (double dB)  { inGainT = dbToGain (dB); }
void StingrayAmp::setOutputGainDb (double dB) { outGainT = dbToGain (dB); }

//==============================================================================
double StingrayAmp::processCh1 (double g)
{
    in1.process (&g);
    const double clean = in1.v (in1Out);
    double v = clean;
    if (suzzRunning)
    {
        suzz.process (&clean);
        const double fz = suzz.v (suzzOut) * logTaper (knobCur[kSGain]) * kSuzzMakeup;
        v = clean + (fz - clean) * suzzMix;
    }
    v *= logTaper (knobCur[kV1]);
    v = bass1.process (v);
    v = mid1.process (v);
    v = treb1.process (v);
    v = pres1.process (v);
    return rail741 (rec1HP.process (v) * kRecGain);
}

double StingrayAmp::processCh2 (double g)
{
    in2.process (&g);
    double v = in2.v (in2Out) * logTaper (knobCur[kV2]);
    v = bass2.process (v);
    v = treb2.process (v);
    return rail741 (rec2HP.process (v) * kRecGain);
}

double StingrayAmp::mixer (double v1, double v2)
{
    const double ins[2] = { v1, v2 };
    mix.process (ins);
    return -mix.v (mixOut) * logTaper (knobCur[kMast]);
}

void StingrayAmp::probePreamp (double vIn, double& c1, double& c2, double& m)
{
    c1 = processCh1 (vIn);
    c2 = processCh2 (vIn);
    m = mixer (c1, c2);
}

double StingrayAmp::powerAmp (double vIn)
{
    double v = paGbwLP.process (paInHP.process (vIn) * kPaGain);

    double d = v - vOutPrev;
    if (d > slewStep) d = slewStep; else if (d < -slewStep) d = -slewStep;
    v = vOutPrev + d;

    const double lim = (v >= 0.0 ? railPos : railNeg) - kRailDrop;
    const double lin = lim - kRailKnee;
    const double av = std::fabs (v);
    if (av > lin)
    {
        const double y = lin + kRailKnee * (1.0 - std::exp (-(av - lin) / kRailKnee));
        v = v < 0.0 ? -y : y;
    }

    double i0, gIn;
    speaker.inputCurrentTerms (i0, gIn);
    double i = i0 + gIn * v;
    climHit = false;
    if (i > kIMax)       { v = (kIMax - i0) / gIn;  i = kIMax;  climHit = true; }
    else if (i < -kIMax) { v = (-kIMax - i0) / gIn; i = -kIMax; climHit = true; }

    speaker.process (&v);
    vOutPrev = v;
    if (std::fabs (i) > peakAmps) peakAmps = std::fabs (i);

    const double dt = 1.0 / fsOs;
    const double iPos = kIQuiet + (i > 0.0 ? i : 0.0);
    const double iNeg = kIQuiet + (i < 0.0 ? -i : 0.0);
    const double chP = railPos < kRailNoLoad ? (kRailNoLoad - railPos) / kRailRs : 0.0;
    const double chN = railNeg < kRailNoLoad ? (kRailNoLoad - railNeg) / kRailRs : 0.0;
    railPos += (chP - iPos) * dt / kRailC;
    railNeg += (chN - iNeg) * dt / kRailC;
    if (railPos < 5.0) railPos = 5.0;
    if (railNeg < 5.0) railNeg = 5.0;

    return speaker.v (spkOut);
}

void StingrayAmp::process (const float* in, float* out, int numSamples)
{
    const int F = Oversampler4x::kFactor;
    const double a = smoothCoeff;
    const double aBlock = 1.0 - std::pow (1.0 - a, (double) kPotUpdateInterval);

    double up[F], term[F], spk[F];

    for (int n = 0; n < numSamples; ++n)
    {
        if (--potCountdown <= 0)
        {
            potCountdown = kPotUpdateInterval;
            if (bright1 != appB1 || bright2 != appB2) applySwitches();
            bool moved = false;
            for (int k = 0; k < kNumKnobs; ++k)
            {
                const double d = knobTarget[k] - knobCur[k];
                if (std::fabs (d) > 1.0e-5)
                {
                    knobCur[k] += d * aBlock;
                    if (std::fabs (knobTarget[k] - knobCur[k]) < 1.0e-5) knobCur[k] = knobTarget[k];
                    moved = true;
                }
            }
            if (moved) applyPots();

            // The Suzz idles while switched out and starts from rest
            if (suzzOn && ! suzzRunning) { suzz.reset(); suzzRunning = true; }
            if (! suzzOn && suzzMix < 1.0e-4) suzzRunning = false;

            if (ch1On && ! ch1Running)
            {
                in1.reset(); suzz.reset(); bass1.reset(); mid1.reset(); treb1.reset();
                pres1.reset(); rec1HP.reset(); ch1Running = true;
            }
            if (ch2On && ! ch2Running)
            {
                in2.reset(); bass2.reset(); treb2.reset(); rec2HP.reset(); ch2Running = true;
            }
            if (! ch1On && ch1Gain < 1.0e-4) ch1Running = false;
            if (! ch2On && ch2Gain < 1.0e-4) ch2Running = false;
        }

        inGainC  += (inGainT - inGainC) * a;
        outGainC += (outGainT - outGainC) * a;
        ch1Gain  += ((ch1On ? 1.0 : 0.0) - ch1Gain) * a;
        ch2Gain  += ((ch2On ? 1.0 : 0.0) - ch2Gain) * a;
        suzzMix  += ((suzzOn ? 1.0 : 0.0) - suzzMix) * a;

        const double x = (double) in[n] * inGainC;
        osMain.upsample (x, up);

        for (int k = 0; k < F; ++k)
        {
            const double v1 = ch1Running ? processCh1 (up[k]) * ch1Gain : 0.0;
            const double v2 = ch2Running ? processCh2 (up[k]) * ch2Gain : 0.0;
            const double vs = powerAmp (mixer (v1, v2));

            const double vc = speaker.v (spkCone);
            spk[k] = (vc - coneVPrev) * fsOs;   // cone acceleration ~ radiated pressure
            coneVPrev = vc;
            term[k] = vs;
        }

        const double yTerm = osMain.downsample (term);
        double yAc = osSpkDown.downsample (spk);
        for (int b = 0; b < 6; ++b) yAc = cab[b].process (yAc);
        yAc *= cabNorm;
        spkMix += ((speakerOn ? 1.0 : 0.0) - spkMix) * a;
        double o = (yTerm + (yAc - yTerm) * spkMix) * (outGainC / kVFullScale);

        if (! (std::fabs (o) < 1.0e6)) { reset(); o = 0.0; }   // NaN or infinity: start again from rest
        out[n] = (float) o;
    }
}

} // namespace cbst
