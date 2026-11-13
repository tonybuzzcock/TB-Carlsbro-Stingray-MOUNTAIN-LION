// Params.h
// Parameter table shared by the VST wrapper and the editor.
//
// Order follows the Stingray's front panel left to right: Channel 1's knobs,
// the Suzz section (Gain, Sustain, and its f/switch), Channel 2's knobs and
// Master Volume. With the owner's one-input-per-channel layout each channel
// has a Normal/Bright rocker and an On switch in place of the two input
// jacks. Then the plugin-only controls.

#ifndef TBCS_PARAMS_H
#define TBCS_PARAMS_H

namespace tbcs
{

enum Param
{
    // Knobs, left to right
    kCh1Volume = 0, kPresence, kCh1Treble, kMiddle, kCh1Bass, kSuzzGain, kSuzzSustain,
    kCh2Volume, kCh2Treble, kCh2Bass, kMaster,
    // Switches, left to right
    kCh1Bright, kCh1On, kSuzzOn, kCh2Bright, kCh2On,
    // Plugin only (bottom strip of the editor)
    kSpeaker, kInput, kOutput,
    kNumParams
};

enum Kind { kKnob, kTone, kSwitch, kBrightSw, kDb12, kDb24 };

const char* const kParamNames[kNumParams] = {
    "1 Volume", "Presence", "1 Treble", "Middle", "1 Bass", "Suzz Gn", "Sustain",
    "2 Volume", "2 Treble", "2 Bass", "Master",
    "1 Input", "Ch 1 On", "Suzz", "2 Input", "Ch 2 On",
    "Speaker", "Input", "Output"
};

const int kParamKind[kNumParams] = {
    kKnob, kTone, kTone, kTone, kTone, kKnob, kKnob,
    kKnob, kTone, kTone, kKnob,
    kBrightSw, kSwitch, kSwitch, kBrightSw, kSwitch,
    kSwitch, kDb12, kDb24
};

const float kDefaults[kNumParams] = {
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.85f, 0.75f,
    0.5f, 0.5f, 0.5f, 0.5f,
    0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 0.5f, 0.5f
};

const double kInputRangeDb  = 12.0;
const double kOutputRangeDb = 24.0;

inline bool isOn (float v) { return v >= 0.5f; }

} // namespace tbcs

#endif
