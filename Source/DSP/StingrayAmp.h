// StingrayAmp.h
// TB Carlsbro Stingray: model of the Carlsbro Stingray guitar amplifier head
// (the original 1974 family, all solid state), the whole amplifier, one mono path.
//
// No schematic has been found yet, so this is built from the front panel and
// what a 1974 Carlsbro transistor/op-amp head of this kind does. Every value
// that would normally come from the drawing is an estimate, named in
// StingrayAmp.cpp and listed in the README. The Suzz section is the circuit of
// Carlsbro's own Suzz pedal (as in TB Suzz), which shares its name and its two
// controls (GAIN, SUSTAIN) with the amp's built-in Suzz.
//
//   guitar -+-> Ch 1: Normal/Bright input -> 741 preamp -+-> clean ----------+
//           |                                             +-> Suzz (pedal) ----+- f/switch
//           |      -> VOLUME -> PRESENCE / TREBLE / MIDDLE / BASS (active, cut/boost)
//           |      -> recovery 741 ---------------------------------------------+
//           +-> Ch 2: Normal/Bright input -> 741 preamp -> VOLUME               |
//                  -> TREBLE / BASS (active) -> recovery 741 --------------------+
//   mixer 741 -> MASTER VOLUME -> 100 W power amp (+/-48 V rails that sag)
//   -> 8 ohm 2x12 (two Carlsbro Powertone 60 12" speakers)
//
// Input stages, the Suzz, the mixer and the loudspeaker load are circuits
// (the Netlist class, 4x oversampled); the active tone controls are filters
// with the response of a Baxandall-type stage; the recovery stages clip like a
// 741 on +/-15 V rails. The power amp is modelled from its behaviour.
//
// Calibration: 1.0 at the input is 1 V peak from the guitar. Output 1.0 is
// 40 V peak at the speaker terminals (100 W into 8 ohms); with Speaker on, the
// cab-miked signal is scaled to match at 1 kHz.
// Plain C++98, no allocation or locks in process(). One instance per channel.

#ifndef TBCS_STINGRAYAMP_H
#define TBCS_STINGRAYAMP_H

#include "Netlist.h"
#include "Oversampler4x.h"
#include "Biquad.h"

namespace cbst
{

class StingrayAmp
{
public:
    StingrayAmp();

    void prepare (double sampleRate);
    void reset();

    // Front panel, left to right. Knobs are 0..1 (min to max, or - to +
    // with 0 at the top for the tone controls). Changes are smoothed.
    void setCh1Volume (double v);
    void setPresence (double v);
    void setCh1Treble (double v);
    void setMiddle (double v);
    void setCh1Bass (double v);
    void setSuzzGain (double v);
    void setSuzzSustain (double v);
    void setCh2Volume (double v);
    void setCh2Treble (double v);
    void setCh2Bass (double v);
    void setMaster (double v);

    void setSuzzOn (bool on);           // the f/switch
    void setCh1Bright (bool bright);    // which Ch 1 input jack the guitar is in
    void setCh1On (bool on);            // plugin: feed Ch 1
    void setCh2Bright (bool bright);
    void setCh2On (bool on);            // plugin: feed Ch 2

    // Plugin-only controls
    void setSpeaker (bool on);
    void setInputGainDb (double dB);
    void setOutputGainDb (double dB);

    void process (const float* in, float* out, int numSamples);

    static int latencySamples() { return Oversampler4x::latencySamples(); }

    // For tests
    double railPositive() const { return railPos; }
    double railNegative() const { return railNeg; }
    double peakSpeakerAmps() const { return peakAmps; }
    void   clearPeakAmps() { peakAmps = 0.0; }
    bool   currentLimitHit() const { return climHit; }
    bool   circuitsBuilt() const { return built; }
    bool   suzzClipping() const { return suzz.opAmpLimited(); }

    // Probe: one oversampled sample of the preamp alone; returns Ch 1 output,
    // Ch 2 output and the mixer output after Master. Used by the tests.
    void   probePreamp (double vIn, double& ch1, double& ch2, double& mix);
    void   resetPreamp();
    double oversampledRate() const { return fsOs; }

    static double logTaper (double x);
    static double reverseLogTaper (double x) { return 1.0 - logTaper (1.0 - x); }

private:
    enum Knob { kV1, kPres, kT1, kMid, kB1, kSGain, kSSus, kV2, kT2, kB2, kMast, kNumKnobs };

    void buildCircuits();
    void applyPots();
    void applySwitches();
    void designTone();
    double processCh1 (double g);
    double processCh2 (double g);
    double mixer (double v1, double v2);
    double powerAmp (double vIn);

    double fsBase, fsOs;
    bool built;

    // Circuits
    Netlist in1, in2, suzz, mix, speaker;
    int in1Out, in2Out, suzzOut, suzzIc, mixOut, spkOut, spkCone;
    int eN1, eB1, eN2, eB2;         // Bright caps across the input resistors
    int eSusPot;                    // Suzz SUSTAIN (rheostat)

    // Active tone stages (oversampled rate)
    Biquad pres1, treb1, mid1, bass1, treb2, bass2;
    Biquad rec1HP, rec2HP;          // coupling into the recovery stages

    // Power amp
    Biquad paInHP, paGbwLP;
    double railPos, railNeg, vOutPrev, peakAmps, slewStep;
    bool climHit;

    Oversampler4x osMain, osSpkDown;

    // Speaker acoustics
    double coneVPrev;
    Biquad cab[6];
    double cabNorm;
    double spkMix;       // 0 = speaker terminals, 1 = miked speaker (crossfaded)

    // Controls
    double knobTarget[kNumKnobs], knobCur[kNumKnobs];
    bool suzzOn, bright1, bright2, ch1On, ch2On, speakerOn;
    bool appB1, appB2;
    double suzzMix;                  // smoothed f/switch (0 clean, 1 Suzz)
    bool suzzRunning;
    double ch1Gain, ch2Gain;         // smoothed channel feeds (click-free on/off)
    bool ch1Running, ch2Running;
    double inGainT, inGainC, outGainT, outGainC;
    double smoothCoeff;
    int potCountdown;
};

} // namespace cbst

#endif
