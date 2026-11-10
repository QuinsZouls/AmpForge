#pragma once
#include "Denormal.hpp"
#include <algorithm>
#include <cmath>

/*
 * Biquad - the base filter block used for EQ modules (low-shelf, peak,
 * high-shelf), i.e. our Bass/Mid/Treble controls.
 *
 * The coefficient formulas come from the "RBJ Audio EQ Cookbook", the
 * standard reference everyone in audio DSP uses. You don't need to
 * memorize the math - what matters is the concept: each filter type
 * (bass/mid/treble) solves the same equation shape with different
 * coefficients.
 */

namespace ampforge {

class Biquad
{
public:
    enum class Type { LowShelf, Peak, HighShelf, HighPass };

    // Guarded because the coefficients below divide by it: a 0 here (an
    // uninitialised host field, or a block whose setSampleRate() is called
    // before the chain's) makes w0 infinite and every coefficient NaN, which
    // process() would then smear over the rest of the session. Falling back to
    // the usual rate is the least surprising thing to do - the filter still
    // works, just at the wrong centre frequency until the real rate arrives.
    void setSampleRate(double sr)
    {
        sampleRate = (sr > 0.0 && std::isfinite(sr)) ? sr : 44100.0;
    }

    // freq: center/cutoff frequency (Hz)
    // gainDB: how much this band is boosted/cut (dB)
    // q: how "narrow/wide" the band is (matters for Peak, kept fixed for shelves)
    void setParams(Type type, float freq, float gainDB, float q)
    {
        // Sanitised before any trigonometry, for two separate reasons:
        //
        //  - Nyquist. The RBJ formulas only produce a stable filter for
        //    0 < w0 < pi. Above Nyquist w0 > pi, alpha goes negative and the
        //    shelves' poles leave the unit circle - at an 8 kHz project rate
        //    this class's own 5 kHz Presence band has poles at radius 1.5,
        //    i.e. a boost knob that runs away to full-scale noise instead of
        //    boosting. 0.45*sr keeps every band inside the stable region with
        //    a little room for the shelves' transition width.
        //  - Non-finite input. A NaN gain or Q (a corrupt preset, a host
        //    sending garbage) would otherwise poison the coefficients for the
        //    rest of the session; std::clamp and friends pass NaN straight
        //    through, so it has to be tested explicitly.
        const double sr = sampleRate;
        const double safeFreq = std::isfinite(freq)
            ? std::clamp(static_cast<double>(freq), 1.0, sr * 0.45)
            : 1000.0;
        // Q and gain are bounded too, for the same class of reason:
        //  - Q below ~0.05 or above ~20 is not a filter anyone asked for, and
        //    at extreme Q the denominator's rounding decides stability (Q=1e6
        //    leaves a 1e-6 tail that never decays; Q=1e30 only survives because
        //    of coefficient rounding, which is not a property to rely on).
        //  - an absurd gain is the "1e26 times the input" case: an EQ block fed
        //    200 dB would otherwise ask for it, and only NamBlock's own setters
        //    clamp today. Bounding it here is the backstop every caller gets,
        //    including the ones that don't clamp (AmpBlock, EqBlock).
        // Both limits sit well outside what the UI offers (±24 dB, Q <= 2).
        const double safeQ    = std::isfinite(q) ? std::clamp(static_cast<double>(q), 0.05, 20.0) : 0.707;
        const double safeGain = std::isfinite(gainDB) ? std::clamp(static_cast<double>(gainDB), -30.0, 30.0) : 0.0;

        const double A     = std::pow(10.0, safeGain / 40.0);
        const double w0    = 2.0 * M_PI * safeFreq / sr;
        const double alpha = std::sin(w0) / (2.0 * safeQ);
        const double cosw0 = std::cos(w0);

        double b0, b1, b2, a0, a1, a2;

        switch (type)
        {
        case Type::LowShelf:
        {
            const double sqrtA = std::sqrt(A);
            b0 =    A*((A+1) - (A-1)*cosw0 + 2*sqrtA*alpha);
            b1 =  2*A*((A-1) - (A+1)*cosw0);
            b2 =    A*((A+1) - (A-1)*cosw0 - 2*sqrtA*alpha);
            a0 =        (A+1) + (A-1)*cosw0 + 2*sqrtA*alpha;
            a1 =   -2*((A-1) + (A+1)*cosw0);
            a2 =        (A+1) + (A-1)*cosw0 - 2*sqrtA*alpha;
            break;
        }
        case Type::HighShelf:
        {
            const double sqrtA = std::sqrt(A);
            b0 =    A*((A+1) + (A-1)*cosw0 + 2*sqrtA*alpha);
            b1 = -2*A*((A-1) + (A+1)*cosw0);
            b2 =    A*((A+1) + (A-1)*cosw0 - 2*sqrtA*alpha);
            a0 =        (A+1) - (A-1)*cosw0 + 2*sqrtA*alpha;
            a1 =    2*((A-1) - (A+1)*cosw0);
            a2 =        (A+1) - (A-1)*cosw0 - 2*sqrtA*alpha;
            break;
        }
        case Type::HighPass:
        {
            // Standard RBJ 2nd-order high-pass - unlike the shelf/peak
            // types above, this one doesn't boost or cut, only rolls off
            // everything below freq, so it ignores gainDB/A entirely.
            b0 =  (1 + cosw0) / 2;
            b1 = -(1 + cosw0);
            b2 =  (1 + cosw0) / 2;
            a0 =   1 + alpha;
            a1 =  -2 * cosw0;
            a2 =   1 - alpha;
            break;
        }
        case Type::Peak:
        default:
        {
            b0 = 1 + alpha*A;
            b1 = -2*cosw0;
            b2 = 1 - alpha*A;
            a0 = 1 + alpha/A;
            a1 = -2*cosw0;
            a2 = 1 - alpha/A;
            break;
        }
        }

        // Normalize coefficients by a0 so processing only needs
        // multiply/add operations.
        c_b0 = (float)(b0/a0); c_b1 = (float)(b1/a0); c_b2 = (float)(b2/a0);
        c_a1 = (float)(a1/a0); c_a2 = (float)(a2/a0);
    }

    // The filter's magnitude response in dB at a given frequency - not
    // part of the real-time process() path, purely for UI visualization
    // (e.g. drawing an EQ curve graph). Evaluates the normalized transfer
    // function H(e^jw) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
    // directly from the coefficients setParams() already computed, at
    // z = e^jw, w = 2*pi*freq/sampleRate - the standard way to read a
    // digital filter's frequency response off its coefficients (see the
    // RBJ cookbook this class's coefficients already come from).
    float magnitudeDB(float freq) const
    {
        const double w = 2.0 * M_PI * static_cast<double>(freq) / sampleRate;
        const double cosw = std::cos(w), sinw = std::sin(w);
        const double cos2w = std::cos(2.0 * w), sin2w = std::sin(2.0 * w);

        const double numReal = c_b0 + c_b1 * cosw + c_b2 * cos2w;
        const double numImag = -(c_b1 * sinw + c_b2 * sin2w);
        const double denReal = 1.0 + c_a1 * cosw + c_a2 * cos2w;
        const double denImag = -(c_a1 * sinw + c_a2 * sin2w);

        const double numMag = std::sqrt(numReal * numReal + numImag * numImag);
        const double denMag = std::sqrt(denReal * denReal + denImag * denImag);
        const double mag = (denMag > 1e-12) ? numMag / denMag : 0.0;
        return static_cast<float>(20.0 * std::log10(std::max(mag, 1e-9)));
    }

    // Direct Form I - the classic biquad equation:
    // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    float process(float x)
    {
        const float y = c_b0*x + c_b1*x1 + c_b2*x2 - c_a1*y1 - c_a2*y2;

        // A single non-finite sample would otherwise poison this filter for
        // good: Direct Form I feeds y back into itself, so a NaN (or an Inf)
        // makes every subsequent output NaN - and flushDenormal() below does
        // not catch either (all comparisons against NaN are false). Recovering
        // by clearing the history is the only option that keeps the rest of the
        // session working, and it is what a hardware EQ would do if it could.
        // Reachable in practice: a host can send a NaN parameter, and a corrupt
        // preset file is parsed with strtof, which happily accepts "nan".
        if (!std::isfinite(y))
        {
            reset();
            return 0.0f;
        }

        x2 = x1; x1 = flushDenormal(x);
        // Flushed so the filter's ring-down after the input goes silent
        // can't leave y1/y2 stuck as denormals (see Denormal.hpp).
        y2 = y1; y1 = flushDenormal(y);
        return y;
    }

    // Clears the sample history (x1/x2/y1/y2) but keeps the coefficients
    // setParams() computed. Same reasoning as AudioBlock::reset() (see its
    // comment): while the block owning this filter is disabled it simply
    // isn't advanced, so this filter's history freezes mid-ring-down - the
    // moment it's switched back on, process() would resume that stale
    // tail (a leftover chunk of an entirely different preset's audio)
    // before any fresh signal got through. Zeroing the history instead
    // makes the filter start from silence, behaving exactly like a freshly
    // constructed one.
    void reset()
    {
        x1 = 0.0f; x2 = 0.0f; y1 = 0.0f; y2 = 0.0f;
    }

private:
    double sampleRate = 44100.0;
    float c_b0 = 1.0f, c_b1 = 0.0f, c_b2 = 0.0f, c_a1 = 0.0f, c_a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
};

} // namespace ampforge
