// Pepefier - Frog Sound Emulator for MPC
// by Pepe Labs
//
// Polished procedural croak synthesizer.
// Models real frog vocalisation: shaped glottal pulses → dual high-Q formants
// with mild sweeps and per-call variation → mode-specific envelopes.
// Modes: Bullfrog, Ribbit, Peeper, Trill, Honk, Pepe Special.
// Effects: Doppler, stereo chorus, depth filtering, pond bed with bubbles.

#include "mpc_plugin.h"
#include "dsp/svf.h"
#include "dsp/noise.h"
#include "dsp/delay_line.h"
#include "dsp/lfo.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
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
    kAutoDensity,       // generative croak density
    kNumParams
};

constexpr int   kNumVoices = 8;
constexpr float kPi        = 3.14159265358979f;

struct ModePreset
{
    float formant1;
    float formant2;
    float pulseHz;
    float decayMs;
    float attackMs;
    float sweepAmt;
    float pulseDuty;
    float formantQ;
    bool  dualFormant;
    float satAmount;
    bool  diphasic;         // two-part call (Ribbit)
    float phase2Start;      // fraction of call where second phase begins
    float phase2FormantMul; // formant multiplier in second phase
    float phase2PulseMul;   // pulse rate multiplier in second phase
};

// Tuned targets from bioacoustic research + authenticity pass
static const ModePreset kModes[6] = {
    // Bullfrog - deep, slow, highly resonant dual formant
    { 260.0f, 1420.0f,  8.5f, 380.0f, 55.0f, 0.03f, 0.25f, 0.90f, true,  0.25f, false, 0.0f, 1.0f, 1.0f },
    // Ribbit - true diphasic: short first part + rising brighter second part
    { 1900.0f, 3100.0f, 65.0f, 140.0f,  8.0f, 0.22f, 0.16f, 0.78f, true,  0.45f, true,  0.42f, 1.35f, 1.25f },
    // Peeper - pure short rising peep, single formant
    { 3550.0f, 3550.0f,  1.2f,  48.0f,  3.0f, 0.25f, 0.45f, 0.72f, false, 0.15f, false, 0.0f, 1.0f, 1.0f },
    // Trill - even musical pulses
    { 2200.0f, 2700.0f, 28.0f, 420.0f, 22.0f, 0.02f, 0.32f, 0.76f, true,  0.22f, false, 0.0f, 1.0f, 1.0f },
    // Honk - nasal, longer, steady
    { 1650.0f, 2450.0f,  5.0f, 480.0f, 70.0f, 0.015f, 0.42f, 0.84f, true,  0.38f, false, 0.0f, 1.0f, 1.0f },
    // Pepe Special - chaotic, wider sweeps, stronger sat
    { 750.0f, 2100.0f, 42.0f, 200.0f, 10.0f, 0.55f, 0.18f, 0.92f, true,  0.85f, true,  0.35f, 1.6f, 1.4f }
};

// Soft exponential envelope with mode-aware attack
struct Envelope
{
    enum Stage { Idle, Attack, Decay, Release };
    Stage stage = Idle;
    float level = 0.0f;
    float attackRate = 0.0f, decayRate = 0.0f, releaseRate = 0.0f;
    float attackCurve = 1.0f;

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

struct Voice
{
    int      note = -1;
    float    velocity = 0.0f;
    float    formantScale = 1.0f;
    float    pulsePhase = 0.0f;
    float    pulseInc = 0.0f;
    float    randPitch = 1.0f;
    float    randFormant = 1.0f;
    float    randDuty = 1.0f;
    float    sweepPhase = 0.0f;
    float    age = 0.0f;          // samples since note-on, for sweep
    Envelope env;
    mpc::dsp::SVF formant1;
    mpc::dsp::SVF formant2;
    mpc::dsp::OnePoleLP excSmoother;
    uint32_t startOrder = 0;

    bool isActive() const { return env.isActive(); }
    void reset()
    {
        env.stage = Envelope::Idle;
        env.level = 0.0f;
        note = -1;
        pulsePhase = 0.0f;
        age = 0.0f;
        formant1.reset();
        formant2.reset();
        excSmoother.reset();
    }
};

} // namespace

class Pepefier : public mpc::Plugin
{
public:
    Pepefier() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::choiceParam("mode",      "Mode",      {"Bullfrog","Ribbit","Peeper","Trill","Honk","Pepe"}, 0));
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

        // Factory presets (values in parameter order)
        addProgram("Init",         { 0, 1.0f, 0.75f, 18.0f, 220.0f, 0.3f, 0.15f, 0.4f, 0.6f, 0.45f, 0.2f, 0.25f, -3.0f, 0.0f });
        addProgram("Bullfrog",     { 0, 1.1f, 0.85f,  9.0f, 360.0f, 0.1f, 0.1f,  0.25f, 0.4f, 0.3f,  0.15f, 0.35f, -2.0f, 0.0f });
        addProgram("Ribbit",       { 1, 0.95f, 0.8f, 70.0f, 140.0f, 0.4f, 0.2f,  0.45f, 0.7f, 0.5f,  0.25f, 0.2f,  -1.0f, 0.0f });
        addProgram("Peeper",       { 2, 0.7f,  0.65f, 2.0f,  50.0f, 0.5f, 0.05f, 0.3f,  0.5f, 0.35f, 0.1f,  0.15f, -4.0f, 0.0f });
        addProgram("Trill",        { 3, 1.0f,  0.7f,  30.0f, 400.0f, 0.15f, 0.1f, 0.5f,  0.55f, 0.4f,  0.2f,  0.2f,  -2.0f, 0.0f });
        addProgram("Honk",         { 4, 1.05f, 0.75f, 5.5f, 450.0f, 0.05f, 0.08f, 0.35f, 0.45f, 0.4f,  0.18f, 0.3f,  -2.5f, 0.0f });
        addProgram("Pepe Special", { 5, 1.2f,  0.9f,  40.0f, 200.0f, 0.7f, 0.35f, 0.6f,  0.9f, 0.65f, 0.3f,  0.4f,  0.0f,  0.0f });
        addProgram("Night Pond",   { 0, 1.15f, 0.6f,  8.0f, 400.0f, 0.2f, 0.25f, 0.55f, 0.35f, 0.5f,  0.65f, 0.55f, -4.0f, 0.45f });
        addProgram("Chorus Ribbit",{ 1, 0.9f,  0.75f, 65.0f, 160.0f, 0.35f, 0.15f, 0.7f,  0.8f, 0.6f,  0.35f, 0.25f, -1.5f, 0.0f });
    }

    void prepare(double sampleRate, int maxBlock) override
    {
        mpc::Plugin::prepare(sampleRate, maxBlock);
        sr_ = (float)sampleRate;
        invSr_ = 1.0f / sr_;
        noise_.reseed(0xC0FFEE01u);
        pondNoise_.reseed(0xB0BB0B02u);
        bubbleNoise_.reseed(0xB0BB1E03u);

        chorusLfo_.setSampleRate(sampleRate);
        chorusLfo_.setShape(mpc::dsp::LFO::Shape::Sine);
        dopplerLfo_.setSampleRate(sampleRate);
        dopplerLfo_.setShape(mpc::dsp::LFO::Shape::Triangle);

        chorusDelayL_.prepare((int)(sampleRate * 0.06f));
        chorusDelayR_.prepare((int)(sampleRate * 0.06f));
        dopplerDelay_.prepare((int)(sampleRate * 0.12f));

        pondFilter_.setSampleRate(sampleRate);
        depthFilter_.setSampleRate(sampleRate);
        pondMod_.setSampleRate(sampleRate);
        pondMod_.setShape(mpc::dsp::LFO::Shape::Sine);

        // Smooth key continuous parameters
        volSmoother_.setTime(15.0f, sampleRate);
        chorusSmoother_.setTime(25.0f, sampleRate);
        pondSmoother_.setTime(30.0f, sampleRate);
        depthSmoother_.setTime(20.0f, sampleRate);
        dopplerSmoother_.setTime(25.0f, sampleRate);
        volSmoother_.snap(mpc::dsp::dbToGain(-3.0f));
        chorusSmoother_.snap(0.4f);
        pondSmoother_.snap(0.2f);
        depthSmoother_.snap(0.25f);
        dopplerSmoother_.snap(0.15f);

        for (auto& v : voices_)
        {
            v.formant1.setSampleRate(sampleRate);
            v.formant2.setSampleRate(sampleRate);
            v.excSmoother.setCutoff(1800.0f, sampleRate);
        }
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) v.reset();
        chorusDelayL_.reset();
        chorusDelayR_.reset();
        dopplerDelay_.reset();
        chorusLfo_.reset(0.0f);
        dopplerLfo_.reset(0.25f);
        pondMod_.reset(0.0f);
        nextStart_ = 1;
        bubbleTimer_ = 0.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : outL;

        const int modeIdx = mpc::clamp(getInt(kMode), 0, 5);
        const ModePreset& mode = kModes[modeIdx];

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
        pondMod_.setFrequency(0.08f + pondMix * 0.25f);

        const float pondCutoff = 280.0f + depthAmt * 600.0f;
        pondFilter_.set(pondCutoff, 0.25f);
        depthFilter_.set(2200.0f - depthAmt * 1400.0f, 0.35f);

        // Handle MIDI
        for (int i = 0; i < ctx.numMidiEvents; ++i)
        {
            const auto& e = ctx.midi[i];
            if (e.isNoteOn())
                noteOn(e.note(), e.velocity() / 127.0f, mode, size, pulseRateParam, decayMs);
            else if (e.isNoteOff())
                noteOff(e.note());
            else if (e.isAllNotesOff())
                for (auto& v : voices_) v.env.noteOff();
        }

        // Generative auto-croaks
        if (autoDensity > 0.02f)
        {
            autoTimer_ += n * invSr_;
            const float interval = 2.8f - autoDensity * 2.4f; // denser = shorter interval
            if (autoTimer_ > interval)
            {
                autoTimer_ = 0.0f;
                const int autoNote = 48 + (int)(noise_.nextFloat() * 24.0f);
                noteOn(autoNote, 0.45f + noise_.nextFloat() * 0.4f, mode, size, pulseRateParam, decayMs);
            }
        }

        for (int s = 0; s < n; ++s)
        {
            float croakSum = 0.0f;

            for (auto& v : voices_)
            {
                if (!v.isActive()) continue;

                const float env = v.env.next();
                v.age += 1.0f;

                // Shaped pulse excitation
                v.pulsePhase += v.pulseInc;
                if (v.pulsePhase >= 1.0f) v.pulsePhase -= 1.0f;

                // Diphasic handling (Ribbit / Pepe Special)
                float phaseMul = 1.0f;
                float formantMul = 1.0f;
                const float callProgress = v.env.level > 0.01f ? (1.0f - v.env.level) : 0.0f; // rough progress proxy
                if (mode.diphasic && callProgress > mode.phase2Start)
                {
                    phaseMul = mode.phase2PulseMul;
                    formantMul = mode.phase2FormantMul;
                    // temporary pulse rate boost in second phase
                    v.pulseInc = std::fmax(0.001f, (mode.pulseHz * v.randPitch * phaseMul) * invSr_);
                }

                float pulseWin = 0.0f;
                const float duty = mode.pulseDuty * v.randDuty;
                if (v.pulsePhase < duty)
                {
                    const float t = v.pulsePhase / duty;
                    pulseWin = 0.5f - 0.5f * std::cos(t * kPi); // raised cosine
                }
                float exc = noise_.nextBipolar() * pulseWin * intensity * v.velocity;
                exc = v.excSmoother.process(exc);

                // Formant sweep + diphasic formant shift
                const float ageSec = v.age * invSr_;
                float sweep = 1.0f + std::sin(ageSec * 5.5f) * mode.sweepAmt * sweepAmt * 0.18f;
                if (mode.diphasic && callProgress > mode.phase2Start)
                    sweep *= formantMul;

                const float f1 = mode.formant1 * v.formantScale * v.randFormant * sweep;
                const float f2 = mode.formant2 * v.formantScale * v.randFormant * sweep;
                v.formant1.set(f1, mode.formantQ);
                if (mode.dualFormant) v.formant2.set(f2, mode.formantQ * 0.92f);

                float lp1, bp1, hp1;
                v.formant1.processAll(exc, lp1, bp1, hp1);
                float f2out = 0.0f;
                if (mode.dualFormant)
                {
                    float lp2, bp2, hp2;
                    v.formant2.processAll(exc * 0.7f, lp2, bp2, hp2);
                    f2out = bp2 * 0.55f;
                }

                float voice = (bp1 * 0.75f + f2out) * env;

                // Mild mode-dependent saturation
                if (mode.satAmount > 0.01f)
                    voice = mpc::dsp::fastTanh(voice * (1.0f + mode.satAmount * 2.5f)) * 0.7f;

                croakSum += voice;
            }

            // Pond bed with richer modulation and occasional bubbles
            float pondRaw = pondNoise_.nextBipolar();
            float plp, pbp, php;
            pondFilter_.processAll(pondRaw, plp, pbp, php);
            float pondModVal = pondMod_.process();
            float pond = plp * (0.55f + 0.45f * pondModVal) * pondSmoother_.process() * 0.38f;

            // Occasional bubble with short decay
            bubbleTimer_ += invSr_;
            if (bubbleTimer_ > 1.4f + bubbleNoise_.nextFloat() * 2.2f)
            {
                bubbleTimer_ = 0.0f;
                float bubble = bubbleNoise_.nextBipolar() * 0.22f * pondMix;
                pond += bubble;
            }

            float dry = croakSum + pond;

            // Depth filtering
            float dlp, dbp, dhp;
            depthFilter_.processAll(dry, dlp, dbp, dhp);
            float depthVal = depthSmoother_.process();
            dry = dry * (1.0f - depthVal * 0.65f) + dlp * depthVal * 0.65f;

            // Doppler — pitch + level movement
            float doppLfo = dopplerLfo_.process();
            float doppAmt = dopplerSmoother_.process();
            float doppDelaySamples = (0.0065f + doppLfo * doppAmt * 0.0075f) * sr_;
            float doppWet = dopplerDelay_.process(dry, std::fmax(1.0f, doppDelaySamples));
            float doppLevel = 1.0f + doppLfo * doppAmt * 0.25f;
            dry = (dry * (1.0f - doppAmt * 0.35f) + doppWet * doppAmt * 0.45f) * doppLevel;

            // Stereo chorus
            float clfo = chorusLfo_.process();
            float dL = (0.015f + clfo * chorusDepth * 0.010f) * sr_;
            float dR = (0.019f - clfo * chorusDepth * 0.010f) * sr_;
            float wetL = chorusDelayL_.process(dry, std::fmax(1.0f, dL));
            float wetR = chorusDelayR_.process(dry, std::fmax(1.0f, dR));

            float cMix = chorusSmoother_.process();
            float outl = dry * (1.0f - cMix) + wetL * cMix;
            float outr = dry * (1.0f - cMix) + wetR * cMix;

            // Soft master saturation + smoothed volume
            float v = volSmoother_.process();
            outl = mpc::dsp::fastTanh(outl * 1.15f) * 0.88f * v;
            outr = mpc::dsp::fastTanh(outr * 1.15f) * 0.88f * v;

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

    void noteOn(int note, float vel, const ModePreset& mode, float size, float pulseRateParam, float decayMs)
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
        voice->velocity = vel * vel; // slight velocity curve
        voice->startOrder = nextStart_++;

        // Per-note organic variation
        voice->formantScale = size * (1.0f + noise_.nextBipolar() * 0.035f);
        voice->randPitch = 1.0f + noise_.nextBipolar() * 0.03f;
        voice->randFormant = 1.0f + noise_.nextBipolar() * 0.06f;
        voice->randDuty = 0.85f + noise_.nextFloat() * 0.3f;

        const float basePulse = mode.pulseHz * voice->randPitch;
        const float finalPulse = basePulse * (pulseRateParam / 18.0f); // param scales around default
        voice->pulseInc = std::fmax(0.001f, finalPulse) * invSr_;
        voice->pulsePhase = noise_.nextFloat(); // random start phase

        voice->env.setTimes(mode.attackMs, decayMs * (mode.decayMs / 220.0f), 140.0f, sr_);
        voice->env.noteOn();
    }

    void noteOff(int note)
    {
        for (auto& v : voices_)
            if (v.note == note) v.env.noteOff();
    }

    float sr_ = 44100.0f;
    float invSr_ = 1.0f / 44100.0f;
    Voice voices_[kNumVoices];
    uint32_t nextStart_ = 1;
    mpc::dsp::Xorshift32 noise_;
    mpc::dsp::Xorshift32 pondNoise_;
    mpc::dsp::Xorshift32 bubbleNoise_;
    mpc::dsp::LFO chorusLfo_;
    mpc::dsp::LFO dopplerLfo_;
    mpc::dsp::LFO pondMod_;
    mpc::dsp::DelayLine chorusDelayL_, chorusDelayR_, dopplerDelay_;
    mpc::dsp::SVF pondFilter_, depthFilter_;
    mpc::dsp::Smoother volSmoother_, chorusSmoother_, pondSmoother_, depthSmoother_, dopplerSmoother_;
    float bubbleTimer_ = 0.0f;
    float autoTimer_ = 0.0f;
};

MPC_PLUGIN_ENTRY(Pepefier)
