// Pepefier - Frog Sound Emulator for MPC
// by Pepe Labs
//
// Pulse-train / harmonic-stack frog synthesis based on measured advertisement
// calls: bullfrog jug-o-rum (glottal ~100 Hz, dominant ~200-300 Hz harmonic
// stack), Pacific chorus diphasic "ribbit" (~86 pps, ~2.2 kHz), spring peeper
// rising whistle (~2.9 kHz, 100-200 ms), gray-treefrog musical trill (~22 pps,
// ~2.45 kHz), green-treefrog nasal honk (~3.3 kHz), plus a chaotic Pepe mode.
// Pond bed is a spatialised chorus of mixed calls, water and insects — not noise.

#include "mpc_plugin.h"
#include "dsp/svf.h"
#include "dsp/noise.h"
#include "dsp/delay_line.h"
#include "dsp/lfo.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/oscillator.h"
#include "dsp/smoother.h"

#include <cmath>
#include <cstdint>

namespace {

enum ParamIndex
{
    kMode = 0,
    kSize,
    kIntensity,
    kPulseRate,
    kDecay,
    kSweep,
    kDoppler,
    kChorusMix,
    kChorusRate,
    kChorusDepth,
    kPondMix,
    kDepth,
    kVolume,
    kAutoDensity,
    kNumParams
};

constexpr int   kNumVoices   = 8;
constexpr int   kNumAmbient  = 6;
constexpr float kPi          = 3.14159265358979f;
constexpr float kTwoPi       = 6.28318530717958f;
constexpr float kPulseRef    = 18.0f;   // Pulse Rate param = 18 -> species-typical rate
// New source is a pitched pulse/harmonic train, so less makeup than the old noise-formant engine.
constexpr float kVoiceGain   = 3.2f;

enum class CallKind
{
    Bullfrog = 0,  // Rana catesbeiana "jug-o-rum"
    Ribbit,        // Pseudacris regilla diphasic advertisement
    Peeper,        // Pseudacris crucifer rising whistle
    Trill,         // Hyla versicolor musical trill
    Honk,          // Hyla cinerea nasal "qonk"
    Pepe,          // exaggerated / chaotic
    WoodCluck      // Lithobates sylvaticus, used in the pond chorus only
};

struct ModeSpec
{
    CallKind kind;
    float pulseHz;       // natural pulse / glottal rate
    float pulseDurMs;    // one glottal/syllable pulse window
    float pulseRiseMs;
    float pulseFallMs;
    float f0;            // carrier / glottal fundamental
    float subLevel;      // amplitude of f0/2 (or f0 if bullfrog's first harmonic)
    float dominantMul;   // dominant / f0 (2 = octave, typical bullfrog)
    float formant1;      // vocal-sac / dominant resonance (Hz)
    float formant2;
    float formant1Q;
    float formant2Q;
    float formant2Mix;
    float attackMs;
    float decayMs;
    float releaseMs;
    float naturalSweep;  // 0-1, scaled by the Sweep param
    float sweepDir;      // +1 rising, -1 falling
    float satAmount;
    int   p1Pulses;      // diphasic / cluck counts; 0 = run for the envelope duration
    int   p2Pulses;
    float gapMs;
    float p2FreqMul;
    float noiseAmt;      // rasp in the glottal source
};

// Measured / literature-typical advertisement-call targets (see comments).
static const ModeSpec kModes[7] = {
    // Bullfrog: f0 ~100-150 Hz, dominant ~200-300 Hz (2nd harmonic),
    // glottal pulse rate ~90-100/s, harmonic stack to ~1.4-2 kHz, croak ~0.5-0.8 s.
    { CallKind::Bullfrog, 100.0f, 6.0f, 0.6f, 4.5f,
      100.0f, 0.12f, 2.0f, 200.0f, 1450.0f, 1.20f, 0.55f, 0.40f,
      45.0f, 520.0f, 160.0f, 0.08f, -1.0f, 0.22f,
      0, 0, 0.0f, 1.0f, 0.12f },

    // Pacific chorus "ribbit": two pulse bursts, ~11 then 5 pulses at ~86-90 pps,
    // ~11 ms pulses (3 ms rise, 8 ms fall), ~57 ms gap, dominant 2.0-2.3 kHz
    // rising to ~2.3-2.5 kHz, fundamental ~1.1 kHz at about -13 dB, call ~230 ms.
    { CallKind::Ribbit, 86.0f, 11.0f, 3.0f, 8.0f,
      1100.0f, 0.22f, 2.0f, 2200.0f, 0.0f, 0.72f, 0.0f, 0.0f,
      4.0f, 240.0f, 80.0f, 0.14f, 1.0f, 0.18f,
      11, 5, 57.0f, 1.08f, 0.04f },

    // Spring peeper: nearly tonal rising whistle, dominant ~2.9 kHz (2.3-3.5),
    // 100-200 ms, little harmonic structure.
    { CallKind::Peeper, 8.0f, 150.0f, 5.0f, 80.0f,
      2900.0f, 0.06f, 1.0f, 2900.0f, 0.0f, 0.55f, 0.0f, 0.0f,
      4.0f, 150.0f, 50.0f, 0.12f, 1.0f, 0.08f,
      1, 0, 0.0f, 1.0f, 0.01f },

    // Gray treefrog trill: ~18-25 pps, ~0.5-1 s, ~16 pulses, dominant ~2.45 kHz,
    // fundamental ~1.2 kHz, pulse ~22 ms (rise 14, fall 7), duty ~0.3.
    { CallKind::Trill, 22.0f, 22.0f, 14.0f, 7.0f,
      1225.0f, 0.20f, 2.0f, 2450.0f, 0.0f, 0.70f, 0.0f, 0.0f,
      12.0f, 800.0f, 120.0f, 0.03f, 1.0f, 0.12f,
      0, 0, 0.0f, 1.0f, 0.03f },

    // Green treefrog honk: nasal "qonk", ~150-220 ms, dominant ~3.3 kHz,
    // lower nasal resonance ~0.9 kHz, slight downward inflection, mild flutter.
    { CallKind::Honk, 38.0f, 200.0f, 18.0f, 80.0f,
      3300.0f, 0.28f, 1.0f, 3300.0f, 900.0f, 0.80f, 0.70f, 0.32f,
      18.0f, 200.0f, 90.0f, 0.08f, -1.0f, 0.28f,
      1, 0, 0.0f, 1.0f, 0.05f },

    // Pepe Special: chaotic exaggerated ribbit/trill with wide sweeps and sat.
    { CallKind::Pepe, 64.0f, 9.0f, 2.0f, 6.0f,
      700.0f, 0.30f, 2.0f, 1400.0f, 2800.0f, 0.85f, 0.60f, 0.55f,
      8.0f, 260.0f, 100.0f, 0.55f, 1.0f, 0.80f,
      8, 7, 40.0f, 1.45f, 0.15f },

    // Wood frog cluck (pond only): short descending duck-like quacks in a series.
    { CallKind::WoodCluck, 14.0f, 55.0f, 6.0f, 25.0f,
      1250.0f, 0.15f, 1.0f, 1250.0f, 600.0f, 0.65f, 0.40f, 0.25f,
      5.0f, 420.0f, 80.0f, 0.10f, -1.0f, 0.20f,
      6, 0, 0.0f, 1.0f, 0.06f }
};

inline float pulseWindow(float tMs, float durMs, float riseMs, float fallMs)
{
    if (tMs < 0.0f || tMs >= durMs) return 0.0f;
    riseMs = std::fmax(0.2f, riseMs);
    fallMs = std::fmax(0.2f, fallMs);
    if (riseMs + fallMs > durMs)
    {
        const float s = durMs / (riseMs + fallMs);
        riseMs *= s;
        fallMs *= s;
    }
    if (tMs < riseMs)
    {
        const float x = tMs / riseMs;
        return x * x;                          // concave-up attack
    }
    if (tMs > durMs - fallMs)
    {
        const float x = (tMs - (durMs - fallMs)) / fallMs;
        return 0.5f + 0.5f * std::cos(kPi * x); // concave-down release
    }
    return 1.0f;
}

struct Envelope
{
    enum Stage { Idle, Attack, Decay, Release };
    Stage stage = Idle;
    float level = 0.0f;
    float attackRate = 0.0f, decayRate = 0.0f, releaseRate = 0.0f;

    void setTimes(float attackMs, float decayMs, float releaseMs, float sampleRate)
    {
        attackRate  = 1.0f / std::fmax(1.0f, attackMs  * 0.001f * sampleRate);
        decayRate   = 1.0f / std::fmax(1.0f, decayMs   * 0.001f * sampleRate);
        releaseRate = 1.0f / std::fmax(1.0f, releaseMs * 0.001f * sampleRate);
    }
    void noteOn()  { stage = Attack; level = 0.0f; }
    void noteOff() { if (stage != Idle) stage = Release; }
    bool isActive() const { return stage != Idle; }

    float next()
    {
        switch (stage)
        {
            case Attack:
                level += attackRate * (1.15f - level * 0.3f);
                if (level >= 1.0f) { level = 1.0f; stage = Decay; }
                break;
            case Decay:
                level *= (1.0f - decayRate * 0.85f);
                if (level <= 0.002f) { level = 0.0f; stage = Idle; }
                break;
            case Release:
                level *= (1.0f - releaseRate);
                if (level <= 0.001f) { level = 0.0f; stage = Idle; }
                break;
            case Idle:
                level = 0.0f;
                break;
        }
        return level;
    }
};

// Shared call renderer used by MIDI voices and the pond chorus.
struct CallEngine
{
    const ModeSpec* spec = &kModes[0];
    float    sr = 44100.0f, invSr = 1.0f / 44100.0f;
    float    velocity = 1.0f;
    float    freqScale = 1.0f;     // size * key * random
    float    randPitch = 1.0f;
    float    pulseHz = 100.0f;
    float    pulsePhase = 0.0f;
    float    pulseAge = 0.0f;
    bool     pulseOn = false;
    int      pulseCount = 0;
    bool     inGap = false;
    float    gapLeft = 0.0f;
    float    age = 0.0f;
    float    callLen = 1.0f;
    float    carrierPhase = 0.0f;
    float    subPhase = 0.0f;
    Envelope env;
    mpc::dsp::SVF formant1, formant2;
    mpc::dsp::OnePoleLP raspLp;
    mpc::dsp::PolyBlepOsc glottal;   // bullfrog harmonic-rich source

    void prepare(double sampleRate)
    {
        sr = (float)sampleRate;
        invSr = 1.0f / sr;
        formant1.setSampleRate(sampleRate);
        formant2.setSampleRate(sampleRate);
        raspLp.setCutoff(6000.0f, sampleRate);
        glottal.setSampleRate(sampleRate);
        glottal.setWave(mpc::dsp::Wave::Saw);
        reset();
    }

    void reset()
    {
        env.stage = Envelope::Idle;
        env.level = 0.0f;
        pulsePhase = pulseAge = 0.0f;
        pulseOn = inGap = false;
        pulseCount = 0;
        gapLeft = age = 0.0f;
        callLen = 1.0f;
        carrierPhase = subPhase = 0.0f;
        formant1.reset();
        formant2.reset();
        raspLp.reset();
        glottal.resetPhase(0.0f);
    }

    bool isActive() const { return env.isActive(); }

    void start(const ModeSpec& mode, float vel, float scale, float pulseRateParam,
               float decayMs, float startPhase, mpc::dsp::Xorshift32& rng)
    {
        reset();
        spec = &mode;
        velocity = vel;
        freqScale = scale;
        randPitch = 1.0f + rng.nextBipolar() * 0.025f;
        pulseHz = std::fmax(0.5f, mode.pulseHz * randPitch * (pulseRateParam / kPulseRef));
        pulsePhase = (mode.kind == CallKind::Peeper || mode.kind == CallKind::Honk) ? 0.0f : startPhase;
        const float decayScaled = decayMs * (mode.decayMs / 220.0f);
        env.setTimes(mode.attackMs, decayScaled, mode.releaseMs, sr);
        if (mode.kind == CallKind::Ribbit || mode.kind == CallKind::Pepe)
        {
            const float pps = pulseHz;
            const float callMs = mode.p1Pulses / pps * 1000.0f + mode.gapMs
                               + mode.p2Pulses / std::fmax(1.0f, pps * mode.p2FreqMul) * 1000.0f
                               + mode.releaseMs;
            callLen = std::fmax(1.0f, callMs * 0.001f * sr);
        }
        else if (mode.kind == CallKind::WoodCluck)
        {
            callLen = std::fmax(1.0f, (mode.p1Pulses / pulseHz) * sr + mode.releaseMs * 0.001f * sr);
        }
        else
        {
            callLen = std::fmax(1.0f, (mode.attackMs + decayScaled) * 0.001f * sr);
        }
        glottal.setWave(mode.kind == CallKind::Bullfrog ? mpc::dsp::Wave::Saw : mpc::dsp::Wave::Sine);
        glottal.resetPhase(0.0f);
        env.noteOn();

        // Fire the first pulse immediately so the call attacks on the note.
        if (mode.kind != CallKind::Peeper && mode.kind != CallKind::Honk)
            beginPulse();
        else
        {
            pulseOn = true;
            pulseAge = 0.0f;
        }
    }

    void beginPulse()
    {
        pulseOn = true;
        pulseAge = 0.0f;
        carrierPhase = 0.0f;
        subPhase = 0.0f;
        glottal.resetPhase(0.0f);
        ++pulseCount;
    }

    float process(float intensity, float sweepAmt, mpc::dsp::Xorshift32& rng)
    {
        if (!isActive()) return 0.0f;
        const ModeSpec& mode = *spec;
        const float envLvl = env.next();
        age += 1.0f;

        const float callProgress = mpc::clamp(age / std::fmax(1.0f, callLen), 0.0f, 1.0f);
        const bool phase2 = (mode.p2Pulses > 0 && pulseCount > mode.p1Pulses);
        const float phaseMul = phase2 ? mode.p2FreqMul : 1.0f;
        const float sweep = 1.0f + sweepAmt * mode.naturalSweep * mode.sweepDir * callProgress;
        const float scale = freqScale * randPitch * sweep * phaseMul;

        float jitter = 1.0f;
        if (mode.kind == CallKind::Pepe)
            jitter = 1.0f + 0.12f * rng.nextBipolar();

        // Default: formants / carrier follow Size and key; pulse rate is independent
        // (the Pulse Rate knob). Bullfrog is the exception: the glottal fundamental
        // IS the pulse rate, and the advertised dominant is the 2nd harmonic.
        float pps = std::fmax(0.5f, pulseHz * phaseMul * jitter);
        float fGlottal, fDom, f2;
        if (mode.kind == CallKind::Bullfrog)
        {
            pps = std::fmax(0.5f, pulseHz * freqScale * randPitch * sweep * jitter);
            fGlottal = pps;
            fDom = 2.0f * fGlottal;
            f2 = mode.formant2 > 0.0f
                ? std::fmax(40.0f, mode.formant2 * (fGlottal / std::fmax(20.0f, mode.f0)))
                : 0.0f;
        }
        else
        {
            fGlottal = std::fmax(20.0f, mode.f0 * scale);
            fDom     = std::fmax(40.0f, mode.formant1 * scale);
            f2       = mode.formant2 > 0.0f ? std::fmax(40.0f, mode.formant2 * scale) : 0.0f;
        }

        // Pulse clock (skipped for the continuous peeper / honk bodies).
        const bool continuous = (mode.kind == CallKind::Peeper || mode.kind == CallKind::Honk);
        if (!continuous)
        {
            if (inGap)
            {
                gapLeft -= 1.0f;
                if (gapLeft <= 0.0f) inGap = false;
            }
            else
            {
                pulsePhase += pps * invSr;
                if (pulsePhase >= 1.0f)
                {
                    pulsePhase -= 1.0f;
                    const bool haveQuota = (mode.p1Pulses + mode.p2Pulses) > 0;
                    const int total = mode.p1Pulses + mode.p2Pulses;
                    if (!haveQuota || pulseCount < total)
                    {
                        if (mode.p2Pulses > 0 && pulseCount == mode.p1Pulses && mode.gapMs > 0.0f)
                        {
                            inGap = true;
                            gapLeft = mode.gapMs * 0.001f * sr;
                        }
                        else
                            beginPulse();
                    }
                }
            }
        }

        float pulseWin = 0.0f;
        if (continuous)
        {
            // Peeper: one window over the whole call. Honk: same, plus light flutter.
            const float durMs = (mode.kind == CallKind::Peeper)
                ? (callLen * invSr * 1000.0f)
                : mode.pulseDurMs;
            pulseWin = pulseWindow(age * invSr * 1000.0f, durMs, mode.pulseRiseMs, mode.pulseFallMs);
            if (mode.kind == CallKind::Honk)
            {
                const float flutter = 0.5f + 0.5f * std::sin(kTwoPi * pps * age * invSr);
                pulseWin *= 0.72f + 0.28f * flutter;
            }
            else if (mode.kind == CallKind::Peeper && pulseHz > 25.0f)
            {
                // High Pulse Rate turns the peep into a slightly pulsed aggressive call.
                const float am = 0.5f + 0.5f * std::sin(kTwoPi * pulseHz * age * invSr);
                pulseWin *= 1.0f - 0.35f * am;
            }
        }
        else if (pulseOn)
        {
            const float tMs = pulseAge * invSr * 1000.0f;
            pulseWin = pulseWindow(tMs, mode.pulseDurMs, mode.pulseRiseMs, mode.pulseFallMs);
            pulseAge += 1.0f;
            if (tMs >= mode.pulseDurMs) pulseOn = false;
        }

        // --- source ---
        float source = 0.0f;
        if (mode.kind == CallKind::Bullfrog)
        {
            // Additive glottal stack, amplitudes from published synthetic
            // bullfrog calls (Capranica / Bee): f0 ~-15 dB, 2nd harmonic dominant.
            carrierPhase += fGlottal * invSr;
            if (carrierPhase >= 1.0f) carrierPhase -= 1.0f;
            const float ph = carrierPhase;
            constexpr float kAmp[12] = {
                0.12f, 1.00f, 0.32f, 0.22f, 0.14f, 0.10f,
                0.07f, 0.05f, 0.04f, 0.03f, 0.022f, 0.016f
            };
            source = 0.0f;
            for (int h = 1; h <= 12; ++h)
            {
                float p = ph * (float)h;
                p -= std::floor(p);
                source += kAmp[h - 1] * std::sin(kTwoPi * p);
            }
            formant1.set(fDom, mode.formant1Q);
            formant2.set(f2, mode.formant2Q);
            float lp1, bp1, hp1, lp2, bp2, hp2;
            formant1.processAll(source, lp1, bp1, hp1);
            formant2.processAll(source, lp2, bp2, hp2);
            source = 0.20f * source + 1.15f * bp1 + mode.formant2Mix * bp2;
        }
        else
        {
            // Two-sine carrier: fundamental (subLevel) + dominant.
            const float incSub = fGlottal * invSr;
            const float incDom = fDom * invSr;
            subPhase += incSub;
            if (subPhase >= 1.0f) subPhase -= 1.0f;
            carrierPhase += incDom;
            if (carrierPhase >= 1.0f) carrierPhase -= 1.0f;
            source = mode.subLevel * std::sin(kTwoPi * subPhase)
                   + 1.0f         * std::sin(kTwoPi * carrierPhase);
            if (f2 > 0.0f && mode.formant2Mix > 0.01f)
            {
                // Cheap 3rd partial / nasal resonator: a sine at formant2, lightly filtered.
                const float p2 = std::fmod(carrierPhase * (f2 / std::fmax(40.0f, fDom)), 1.0f);
                source += mode.formant2Mix * std::sin(kTwoPi * p2);
            }
            if (mode.formant1Q > 0.01f)
            {
                formant1.set(fDom, mode.formant1Q);
                float lp, bp, hp;
                formant1.processAll(source, lp, bp, hp);
                source = 0.45f * source + 0.55f * bp;
            }
        }

        if (mode.noiseAmt > 0.001f)
        {
            const float rasp = raspLp.process(rng.nextBipolar());
            source += rasp * mode.noiseAmt * pulseWin;
        }

        float y = source * pulseWin * envLvl * velocity * (0.45f + 0.70f * intensity) * kVoiceGain;
        if (mode.satAmount > 0.01f)
            y = mpc::dsp::fastTanh(y * (1.0f + mode.satAmount * 2.2f)) * 0.78f;
        return y;
    }
};

struct Voice
{
    int        note = -1;
    uint32_t   startOrder = 0;
    CallEngine eng;

    bool isActive() const { return eng.isActive(); }
    void reset() { note = -1; eng.reset(); }
};

struct AmbientFrog
{
    CallEngine eng;
    float pan = 0.5f;
    float gain = 0.4f;
    float timer = 0.0f;
    float interval = 1.5f;
    int   specIndex = 0;
};

} // namespace

class Pepefier : public mpc::Plugin
{
public:
    Pepefier() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::choiceParam("mode",      "Mode",      {"Bullfrog","Ribbit","Peeper","Trill","Honk","Pepe Special"}, 0));
        addParam(mpc::floatParam ("size",      "Size",      0.4f, 2.2f, 1.0f, "", 2));
        addParam(mpc::floatParam ("intensity", "Intensity", 0.0f, 1.0f, 0.75f, "", 2));
        addParam(mpc::logParam   ("pulse",     "Pulse Rate",0.5f, 140.0f, 18.0f, "Hz", 1));
        addParam(mpc::logParam   ("decay",     "Decay",     15.0f, 900.0f, 220.0f, "ms", 0));
        addParam(mpc::floatParam ("sweep",     "Sweep",     0.0f, 1.0f, 0.3f, "", 2));
        addParam(mpc::floatParam ("doppler",   "Doppler",   0.0f, 1.0f, 0.15f, "", 2));
        addParam(mpc::floatParam ("chorus",    "Chorus",    0.0f, 1.0f, 0.4f, "", 2));
        addParam(mpc::logParam   ("chrate",    "Chor Rate", 0.05f, 3.5f, 0.6f, "Hz", 2));
        addParam(mpc::floatParam ("chdepth",   "Chor Depth",0.0f, 1.0f, 0.45f, "", 2));
        addParam(mpc::floatParam ("pond",      "Pond",      0.0f, 1.0f, 0.2f, "", 2));
        addParam(mpc::floatParam ("depth",     "Depth",     0.0f, 1.0f, 0.25f, "", 2));
        addParam(mpc::floatParam ("volume",    "Volume",    -30.0f, 6.0f, -3.0f, "dB", 1));
        addParam(mpc::floatParam ("auto",      "Auto",      0.0f, 1.0f, 0.0f, "", 2));

        // Pulse Rate = 18 is "species typical" (internal rate = mode.pulseHz * param/18).
        addProgram("Init",         { 0, 1.0f, 0.80f, 18.0f, 220.0f, 0.25f, 0.10f, 0.30f, 0.50f, 0.35f, 0.15f, 0.20f, -3.0f, 0.0f });
        addProgram("Bullfrog",     { 0, 1.15f, 0.85f, 18.0f, 240.0f, 0.15f, 0.08f, 0.22f, 0.35f, 0.30f, 0.12f, 0.30f, -2.0f, 0.0f });
        addProgram("Ribbit",       { 1, 1.00f, 0.82f, 18.0f, 220.0f, 0.35f, 0.12f, 0.40f, 0.65f, 0.45f, 0.18f, 0.18f, -1.5f, 0.0f });
        addProgram("Peeper",       { 2, 1.00f, 0.70f, 18.0f, 220.0f, 0.55f, 0.04f, 0.25f, 0.45f, 0.30f, 0.10f, 0.12f, -3.0f, 0.0f });
        addProgram("Trill",        { 3, 1.00f, 0.75f, 18.0f, 220.0f, 0.10f, 0.08f, 0.45f, 0.55f, 0.40f, 0.16f, 0.18f, -2.0f, 0.0f });
        addProgram("Honk",         { 4, 1.00f, 0.78f, 18.0f, 220.0f, 0.20f, 0.06f, 0.30f, 0.45f, 0.35f, 0.14f, 0.22f, -2.5f, 0.0f });
        addProgram("Pepe Special", { 5, 1.20f, 0.92f, 26.0f, 240.0f, 0.75f, 0.30f, 0.55f, 0.90f, 0.60f, 0.22f, 0.35f, -1.0f, 0.0f });
        addProgram("Night Pond",   { 0, 1.10f, 0.62f, 18.0f, 260.0f, 0.18f, 0.22f, 0.50f, 0.35f, 0.45f, 0.72f, 0.50f, -4.0f, 0.50f });
        addProgram("Chorus Ribbit",{ 1, 0.95f, 0.78f, 18.0f, 220.0f, 0.30f, 0.14f, 0.72f, 0.80f, 0.60f, 0.28f, 0.22f, -1.5f, 0.0f });
    }

    void prepare(double sampleRate, int maxBlock) override
    {
        mpc::Plugin::prepare(sampleRate, maxBlock);
        sr_ = (float)sampleRate;
        invSr_ = 1.0f / sr_;
        noise_.reseed(0xC0FFEE01u);
        pondRng_.reseed(0xB0BB0B02u);
        insectRng_.reseed(0xB0BB1E03u);

        chorusLfo_.setSampleRate(sampleRate);
        chorusLfo_.setShape(mpc::dsp::LFO::Shape::Sine);
        dopplerLfo_.setSampleRate(sampleRate);
        dopplerLfo_.setShape(mpc::dsp::LFO::Shape::Triangle);

        chorusDelayL_.prepare((int)(sampleRate * 0.06f));
        chorusDelayR_.prepare((int)(sampleRate * 0.06f));
        dopplerDelay_.prepare((int)(sampleRate * 0.12f));

        waterFilter_.setSampleRate(sampleRate);
        depthFilter_.setSampleRate(sampleRate);
        bubbleOsc_.setSampleRate(sampleRate);
        insectOsc_.setSampleRate(sampleRate);
        waterLp_.setCutoff(900.0f, sampleRate);

        volSmoother_.setTime(15.0f, sampleRate);
        chorusSmoother_.setTime(25.0f, sampleRate);
        pondSmoother_.setTime(30.0f, sampleRate);
        depthSmoother_.setTime(20.0f, sampleRate);
        dopplerSmoother_.setTime(25.0f, sampleRate);
        volSmoother_.snap(mpc::dsp::dbToGain(-3.0f));
        chorusSmoother_.snap(0.30f);
        pondSmoother_.snap(0.15f);
        depthSmoother_.snap(0.20f);
        dopplerSmoother_.snap(0.10f);

        for (auto& v : voices_) v.eng.prepare(sampleRate);
        for (int i = 0; i < kNumAmbient; ++i)
        {
            ambient_[i].eng.prepare(sampleRate);
            ambient_[i].interval = 0.6f + 0.7f * (float)i;
            ambient_[i].timer = ambient_[i].interval * 0.3f * (float)(i + 1);
            ambient_[i].pan = (float)i / (float)(kNumAmbient - 1);
        }
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) v.reset();
        for (auto& a : ambient_) { a.eng.reset(); a.timer = a.interval; }
        chorusDelayL_.reset();
        chorusDelayR_.reset();
        dopplerDelay_.reset();
        chorusLfo_.reset(0.0f);
        dopplerLfo_.reset(0.25f);
        waterLp_.reset();
        nextStart_ = 1;
        bubbleTimer_ = 0.8f;
        bubbleEnv_ = 0.0f;
        insectTimer_ = 0.4f;
        insectEnv_ = 0.0f;
        insectPhase_ = 0.0f;
        autoTimer_ = 0.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : outL;

        const int modeIdx = mpc::clamp(getInt(kMode), 0, 5);
        const ModeSpec& mode = kModes[modeIdx];

        const float size = get(kSize);
        const float intensity = get(kIntensity);
        const float pulseRateParam = get(kPulseRate);
        const float decayMs = get(kDecay);
        const float sweepAmt = get(kSweep);
        const float dopplerAmt = get(kDoppler);
        const float chorusMix = get(kChorusMix);
        const float chorusRate = get(kChorusRate);
        const float chorusDepth = get(kChorusDepth);
        const float pondMix = get(kPondMix);
        const float depthAmt = get(kDepth);
        const float volume = mpc::dsp::dbToGain(get(kVolume));
        const float autoDensity = get(kAutoDensity);

        volSmoother_.setTarget(volume);
        chorusSmoother_.setTarget(chorusMix);
        pondSmoother_.setTarget(pondMix);
        depthSmoother_.setTarget(depthAmt);
        dopplerSmoother_.setTarget(dopplerAmt);

        chorusLfo_.setFrequency(chorusRate);
        dopplerLfo_.setFrequency(0.15f + dopplerAmt * 0.4f);

        waterFilter_.set(420.0f + depthAmt * 280.0f, 0.25f);
        depthFilter_.set(2600.0f - depthAmt * 1600.0f, 0.35f);

        for (int i = 0; i < ctx.numMidiEvents; ++i)
        {
            const auto& e = ctx.midi[i];
            if (e.isNoteOn())
                noteOn(e.note(), e.velocity() / 127.0f, mode, size, pulseRateParam, decayMs);
            else if (e.isNoteOff())
                noteOff(e.note());
            else if (e.isAllNotesOff())
                for (auto& v : voices_) v.eng.env.noteOff();
        }

        if (autoDensity > 0.02f)
        {
            autoTimer_ += n * invSr_;
            const float interval = 2.6f - autoDensity * 2.2f;
            if (autoTimer_ > interval)
            {
                autoTimer_ = 0.0f;
                const int autoNote = 48 + (int)(noise_.nextFloat() * 24.0f);
                noteOn(autoNote, 0.45f + noise_.nextFloat() * 0.35f, mode, size, pulseRateParam, decayMs);
            }
        }

        for (int s = 0; s < n; ++s)
        {
            float croakL = 0.0f, croakR = 0.0f;

            for (auto& v : voices_)
            {
                if (!v.isActive()) continue;
                const float y = v.eng.process(intensity, sweepAmt, noise_);
                croakL += y;
                croakR += y;
            }

            const float pondLvl = pondSmoother_.process();
            if (pondLvl > 0.01f)
                renderPond(pondLvl, size, pulseRateParam, decayMs, intensity, sweepAmt, croakL, croakR);

            float dryL = croakL;
            float dryR = croakR;
            float dryM = 0.5f * (dryL + dryR);

            // Depth filtering on the mono sum, then re-apply to L/R by residual.
            float dlp, dbp, dhp;
            depthFilter_.processAll(dryM, dlp, dbp, dhp);
            const float depthVal = depthSmoother_.process();
            const float dryM2 = dryM * (1.0f - depthVal * 0.65f) + dlp * depthVal * 0.65f;
            const float depthScale = (std::fabs(dryM) > 1.0e-8f) ? (dryM2 / dryM) : 1.0f;
            dryL *= depthScale;
            dryR *= depthScale;
            dryM = dryM2;

            // Doppler on the mid, then spread back.
            const float doppLfo = dopplerLfo_.process();
            const float doppAmt = dopplerSmoother_.process();
            const float doppDelaySamples = (0.0065f + doppLfo * doppAmt * 0.0075f) * sr_;
            const float doppWet = dopplerDelay_.process(dryM, std::fmax(1.0f, doppDelaySamples));
            const float doppLevel = 1.0f + doppLfo * doppAmt * 0.25f;
            const float mid = (dryM * (1.0f - doppAmt * 0.35f) + doppWet * doppAmt * 0.45f) * doppLevel;
            const float sideL = dryL - dryM;
            const float sideR = dryR - dryM;
            dryL = mid + sideL;
            dryR = mid + sideR;

            const float clfo = chorusLfo_.process();
            const float dL = (0.015f + clfo * chorusDepth * 0.010f) * sr_;
            const float dR = (0.019f - clfo * chorusDepth * 0.010f) * sr_;
            const float wetL = chorusDelayL_.process(0.5f * (dryL + dryR), std::fmax(1.0f, dL));
            const float wetR = chorusDelayR_.process(0.5f * (dryL + dryR), std::fmax(1.0f, dR));
            const float cMix = chorusSmoother_.process();
            float outl = dryL * (1.0f - cMix) + wetL * cMix;
            float outr = dryR * (1.0f - cMix) + wetR * cMix;

            const float v = volSmoother_.process();
            outl = mpc::dsp::fastTanh(outl * 1.10f) * 0.90f * v;
            outr = mpc::dsp::fastTanh(outr * 1.10f) * 0.90f * v;

            outL[s] = outl;
            if (outR != outL) outR[s] = outr;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Pepefier";
        info.vendor = "Pepe Labs";
        info.uniqueId = MPC_FOURCC('P','A','I','7');
        info.numInputs = 0;
        info.numOutputs = 2;
        info.isInstrument = true;
        return info;
    }

    void noteOn(int note, float vel, const ModeSpec& mode, float size, float pulseRateParam, float decayMs)
    {
        Voice* voice = nullptr;
        uint32_t oldest = 0xFFFFFFFFu;
        for (auto& v : voices_)
        {
            if (!v.isActive()) { voice = &v; break; }
            if (v.startOrder < oldest) { oldest = v.startOrder; voice = &v; }
        }
        if (!voice) return;

        voice->reset();
        voice->note = note;
        voice->startOrder = nextStart_++;
        const float velocity = 0.3f + 0.7f * vel;
        const float keyScale = std::pow(2.0f, (note - 60) / 24.0f);
        const float scale = size * keyScale * (1.0f + noise_.nextBipolar() * 0.03f);
        voice->eng.start(mode, velocity, scale, pulseRateParam, decayMs, 0.0f, noise_);
    }

    void noteOff(int note)
    {
        for (auto& v : voices_)
            if (v.note == note) v.eng.env.noteOff();
    }

    void triggerAmbient(AmbientFrog& a, float size, float pulseRateParam, float decayMs, float intensity)
    {
        // Mixed-species pond: peeper, ribbit, bullfrog, trill, wood cluck.
        static const int kChoices[] = { 2, 2, 1, 1, 0, 3, 6, 6, 2, 1 };
        const int pick = kChoices[(int)(pondRng_.nextFloat() * 10.0f) % 10];
        a.specIndex = pick;
        a.pan = 0.08f + pondRng_.nextFloat() * 0.84f;
        a.gain = 0.18f + pondRng_.nextFloat() * 0.28f;
        const float key = 0.85f + pondRng_.nextFloat() * 0.40f; // size-ish variation
        const float scale = size * key * (0.85f + pondRng_.nextFloat() * 0.3f);
        const float vel = 0.35f + pondRng_.nextFloat() * 0.40f;
        a.eng.start(kModes[pick], vel * (0.5f + 0.5f * intensity), scale,
                    pulseRateParam, decayMs, pondRng_.nextFloat(), pondRng_);
        a.interval = 0.7f + pondRng_.nextFloat() * (2.8f - 1.6f * std::fmin(1.0f, a.gain * 2.0f));
        a.timer = 0.0f;
    }

    void renderPond(float pondLvl, float size, float pulseRateParam, float decayMs,
                    float intensity, float sweepAmt, float& l, float& r)
    {
        // Water: very quiet pink-ish bed (not the main sound).
        const float white = pondRng_.nextBipolar();
        const float water = waterLp_.process(white);
        float lp, bp, hp;
        waterFilter_.processAll(water, lp, bp, hp);
        const float waterBed = lp * pondLvl * 0.045f;

        // Occasional bubble: decaying sine ping.
        bubbleTimer_ -= invSr_;
        if (bubbleTimer_ <= 0.0f)
        {
            bubbleTimer_ = 1.2f + pondRng_.nextFloat() * 2.8f;
            if (pondLvl > 0.22f)
            {
                bubbleEnv_ = 0.7f + pondRng_.nextFloat() * 0.3f;
                bubbleOsc_.setFrequency(500.0f + pondRng_.nextFloat() * 1400.0f);
                bubbleOsc_.resetPhase(0.0f);
                bubblePan_ = pondRng_.nextFloat();
            }
        }
        float bubble = 0.0f;
        if (bubbleEnv_ > 0.001f)
        {
            bubble = bubbleOsc_.process() * bubbleEnv_ * pondLvl * 0.22f;
            bubbleEnv_ *= 0.9992f; // ~50-80 ms
            if (bubbleEnv_ < 0.001f) bubbleEnv_ = 0.0f;
        }

        // Insects: sparse 4.5-7 kHz AM chirps.
        insectTimer_ -= invSr_;
        if (insectTimer_ <= 0.0f)
        {
            insectTimer_ = 1.6f + insectRng_.nextFloat() * 3.5f;
            insectEnv_ = (pondLvl > 0.22f) ? 1.0f : 0.0f;
            insectHz_ = 4500.0f + insectRng_.nextFloat() * 2500.0f;
            insectPan_ = insectRng_.nextFloat();
            insectPhase_ = 0.0f;
        }
        float insect = 0.0f;
        if (insectEnv_ > 0.001f)
        {
            insectPhase_ += insectHz_ * invSr_;
            if (insectPhase_ >= 1.0f) insectPhase_ -= 1.0f;
            insectAmPhase_ += 32.0f * invSr_;
            if (insectAmPhase_ >= 1.0f) insectAmPhase_ -= 1.0f;
            const float tick = (0.5f + 0.5f * std::sin(kTwoPi * insectAmPhase_));
            insect = std::sin(kTwoPi * insectPhase_) * tick * insectEnv_ * pondLvl * 0.08f;
            insectEnv_ *= 0.9994f;
            if (insectEnv_ < 0.001f) insectEnv_ = 0.0f;
        }

        // Mixed-species overlapping callers.
        for (auto& a : ambient_)
        {
            a.timer += invSr_;
            const float densityInterval = a.interval / std::fmax(0.25f, 0.35f + 1.40f * pondLvl);
            if (pondLvl > 0.22f && !a.eng.isActive() && a.timer >= densityInterval)
                triggerAmbient(a, size, pulseRateParam, decayMs, intensity);

            if (a.eng.isActive())
            {
                const float y = a.eng.process(intensity, sweepAmt, pondRng_) * a.gain * pondLvl * 1.15f;
                const float panL = std::cos(a.pan * 0.5f * kPi);
                const float panR = std::sin(a.pan * 0.5f * kPi);
                l += y * panL;
                r += y * panR;
            }
        }

        l += waterBed + bubble * (1.0f - bubblePan_) + insect * (1.0f - insectPan_);
        r += waterBed + bubble * bubblePan_         + insect * insectPan_;
    }

    float sr_ = 44100.0f;
    float invSr_ = 1.0f / 44100.0f;
    Voice voices_[kNumVoices];
    AmbientFrog ambient_[kNumAmbient];
    uint32_t nextStart_ = 1;
    mpc::dsp::Xorshift32 noise_;
    mpc::dsp::Xorshift32 pondRng_;
    mpc::dsp::Xorshift32 insectRng_;
    mpc::dsp::LFO chorusLfo_;
    mpc::dsp::LFO dopplerLfo_;
    mpc::dsp::DelayLine chorusDelayL_, chorusDelayR_, dopplerDelay_;
    mpc::dsp::SVF waterFilter_, depthFilter_;
    mpc::dsp::OnePoleLP waterLp_;
    mpc::dsp::WavetableSine bubbleOsc_, insectOsc_;
    mpc::dsp::Smoother volSmoother_, chorusSmoother_, pondSmoother_, depthSmoother_, dopplerSmoother_;
    float bubbleTimer_ = 0.8f, bubbleEnv_ = 0.0f, bubblePan_ = 0.5f;
    float insectTimer_ = 0.4f, insectEnv_ = 0.0f, insectHz_ = 5500.0f;
    float insectPhase_ = 0.0f, insectAmPhase_ = 0.0f, insectPan_ = 0.5f;
    float autoTimer_ = 0.0f;
};

MPC_PLUGIN_ENTRY(Pepefier)
