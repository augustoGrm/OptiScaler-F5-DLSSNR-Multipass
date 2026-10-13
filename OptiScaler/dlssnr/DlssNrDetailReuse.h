#pragma once

#include <algorithm>
#include <cmath>

// Reuse detail between frames: run the model on one frame, and on the next add the saved detail of that frame
// (model answer minus model input), moved with the motion vectors, to the new input instead of running
// the model again. The method is reverse reprojection caching (Nehab et al. 2007) with TAA-style history
// rejection (see dlssnr_detail_reuse.hlsl).
//
// How wrong a reused frame can be is the share of the picture with no detail to move times how much the model changes
// the picture. Passes build on each other, so the second factor grows with the pass count and reuse suits one pass best:
// with two or three, the same dropped areas flicker visibly in fast motion (The Witcher 3, 2026-09-30; not seen there
// with frame generation on). MotionGuard below bounds the first factor, not the second.
//
// This header is the cadence only: which frame runs the model, which reuses, and what each has to record.
// It has no GPU or game dependency so tests/nr_detail_reuse_smoke.cpp can check it on the host.
//
// Every frame that is processed saves its detail (captureHistory), reuse frames included, so the saved history
// is always one frame old when the next frame reads it:
//   - a reuse frame moves it onto its input;
//   - a full frame may pull the model's new detail toward it (steadiness), so full and reuse frames next to each
//     other differ less.
// The model keeps its own history (its previous output). After a reuse frame that history is two frames old,
// so the next full frame hands the model motion composed over both frames (composeMotion), unless the model
// starts over anyway (reset).
//
// Every 3rd frame (DetailReuseInterval 3): two reused frames follow each full one. The second keeps its vectors
// accumulated over both reused frames (accumulateMotion), so the next full frame composes back to the model's last run.
//
// The spread (DetailReuseSpread): the model's new detail differs from the moved old detail by however much the model
// itself drifted over the frames it was skipped (its regional light moves a little every frame), and the full frame
// shows that drift at once: the light pulses each time the model runs, more at every 3rd frame. Spread over S frames,
// the full frame and the reused frames after it show what was shown before it, moved along, blended toward the new
// detail by (k + 1) / S on the k-th of them. It is Steady's blend (Steady below), but on the display only: what is
// saved for the next frame is the unblended detail, so the old detail is not carried on through the run (Steady's pull
// is saved, and trails). Measured offline (DLSS-TOOLS editcache/reuse_sim.py, NBA 2K27, slow pan): every 3rd frame
// stepped the regional light 0.022 stop when the model ran, against 0.009 from frame to frame when it runs on every
// frame; spread over 3, 0.008 (no pulse), at slightly stronger trails than every other frame unspread (0.395 vs 0.258
// stop, p99.5) and the model on a third of the frames instead of half.

namespace DlssNrDetailReuse
{
enum class Kind
{
    Full,  // run the model
    Reuse, // skip the model; input + reprojected saved detail
};

// What the caller knows about this frame.
struct FrameFacts
{
    bool enabled = false;            // option on and this route supports it
    bool reset = false;              // the model starts over this frame (camera cut, new feature, resize)
    bool blocked = false;            // a frame that must see the real model: calibration, frame hold
    bool hold = false;               // the picture moves too fast to move detail across (MotionGuard): run the model,
                                     // but the saved history stays good and reuse resumes as soon as it calms down
    unsigned long long frame = 0;    // frame number; a step above maxStep means NR did not run in between
    unsigned long long maxStep = 1;  // the largest step between two NR frames that is not a gap (frame generation
                                     // presents in between, so a present counter steps by more than one)
    unsigned long long revision = 0; // changes whenever anything that changes the model's answer changes
    unsigned int reuseRun = 1;       // reused frames after each full one: 1 (every other frame) or 2 (every 3rd)
    unsigned int spread = 1;         // frames the full frame's jump is spread over (1 = none)
};

struct Decision
{
    Kind kind = Kind::Full;
    bool historyUsable = false;  // the saved detail is from the previous frame and may be reprojected
    bool captureHistory = false; // save answer - input, the input colour and the depth for the next frame
    bool composeMotion = false;  // full frame: give the model motion composed over the last two frames
    bool saveMotion = false;     // reuse frame: keep this frame's motion for the next full frame to compose
    bool accumulateMotion = false; // ... added to what the reuse frame before it kept (the second reused frame in a row)
    int spreadStep = -1;         // the k-th frame of a spread (0 = the full frame), or -1: none on this frame
    float spreadAmount = 0.0f;   // how far it pulls toward the moved detail shown before (Steady's amount)
    bool saveShown = false;      // keep what this frame shows (its spread result) for the spread's next frame
};

// The k-th frame of a spread over `spread` frames shows (k + 1) / spread of the new detail. What it blends with is the
// last frame's shown detail moved here, which is itself already partly new from k = 1 on, so the pull toward it is
// the share of old that remains over the share that was left: k = 0, 1 - 1 / spread; then (1 - (k + 1) / spread) /
// (1 - k / spread). Over 3: 2/3, then 1/2.
inline float SpreadAmount(unsigned int k, unsigned int spread)
{
    if (spread <= 1 || k + 1 >= spread)
        return 0.0f;
    const float s = (float) spread;
    if (k == 0)
        return 1.0f - 1.0f / s;
    return (1.0f - (float) (k + 1) / s) / (1.0f - (float) k / s);
}

// How far the frame number steps between two NR frames when NR runs on every rendered frame. With frame generation
// the present counter also counts the generated frames, so the step is the multiplier: 2 at 2x, 4 at 4x, more with an
// unlocked multiplier. A fixed limit of 4 made every frame above 4x look like a gap, and reuse fell back on all of
// them (Witcher 3, 2026-10-08). The normal step is learnt instead: the median of the last 16 forward steps, so the odd
// skipped frame does not move it. A frame NR really skipped doubles the step, which stays a gap.
class StepTracker
{
    static constexpr unsigned kKept = 16;
    unsigned long long steps[kKept] = {};
    unsigned count = 0, next = 0;
    unsigned long long last = 0;
    bool hasLast = false;

public:
    void Add(unsigned long long frame)
    {
        if (hasLast && frame > last)
        {
            steps[next++ % kKept] = frame - last;
            count = (std::min)(count + 1, kKept);
        }
        last = frame;
        hasLast = true;
    }

    // The normal step, 1 before any was seen.
    unsigned long long Typical() const
    {
        if (count == 0)
            return 1;
        unsigned long long sorted[kKept];
        std::copy(steps, steps + count, sorted);
        std::sort(sorted, sorted + count);
        return sorted[count / 2];
    }
};

// The largest step between two NR frames that is not a gap. Without frame generation every present is a rendered
// frame. With it, at least 4 (what the fixed limit was), and half again the normal step, which a skipped frame (twice
// the step) still passes.
inline unsigned long long MaxStepFor(bool withFg, unsigned long long typical)
{
    return withFg ? (std::max)(4ull, typical + typical / 2) : 1ull;
}

// Why reuse frames fell back to the model (Cadence::Fallback), for the log: the first of these that held.
struct FallbackCauses
{
    unsigned long long reset = 0;       // the model started over
    unsigned long long blocked = 0;     // a frame that must see the real model, or the one after it
    unsigned long long gap = 0;         // NR did not run in between (the frame number stepped past maxStep)
    unsigned long long changed = 0;     // something that changes the model's answer changed
    unsigned long long notRecorded = 0; // decided, but the caller ran the model instead (ReuseFailed)
    unsigned long long lastGapStep = 0; // the frame number's step at the last gap (0: it did not go forward)
    unsigned long long lastMaxStep = 0; // ... and the largest step allowed then
};

class Cadence
{
    bool historyValid = false; // the previous frame's detail is saved
    bool pending = false;      // Next asked for a capture; Captured has not confirmed it yet
    unsigned int reusedInRow = 0; // reused frames since the last full one
    int spreadStep = -1;          // the last frame's step in a spread, -1: none
    bool lastBlocked = false; // the history was saved on a blocked frame (a held frame saves the frozen picture)
    bool hasLast = false;
    unsigned long long lastFrame = 0;
    unsigned long long lastRevision = 0;
    unsigned long long full = 0, reused = 0, fallback = 0, held = 0;
    FallbackCauses causes;

public:
    Decision Next(const FrameFacts& f)
    {
        Decision d;

        if (!f.enabled)
        {
            historyValid = false;
            pending = false;
            reusedInRow = 0;
            spreadStep = -1;
            lastBlocked = false;
            hasLast = false;
            return d;
        }

        // A capture that was never confirmed leaves no usable history.
        if (pending)
        {
            historyValid = false;
            pending = false;
        }

        const bool gap = hasLast && (f.frame <= lastFrame || f.frame - lastFrame > f.maxStep);
        const bool changed = hasLast && f.revision != lastRevision;
        // The frame after a blocked one runs the model too: what was saved then may be a frozen picture.
        const bool invalid = f.reset || f.blocked || lastBlocked || gap || changed;
        const bool usable = historyValid && !invalid;
        const unsigned int run = std::clamp(f.reuseRun, 1u, 2u);
        const bool wouldReuse = historyValid && reusedInRow < run;
        const bool afterReuse = reusedInRow > 0;

        if (wouldReuse && !invalid && !f.hold)
        {
            d.kind = Kind::Reuse;
            d.saveMotion = true;
            d.accumulateMotion = reusedInRow > 0;
            ++reused;
            ++reusedInRow;
            // The spread goes on through the reused frames after a full one
            if (spreadStep >= 0 && (unsigned int) spreadStep + 2 < f.spread)
                d.spreadStep = spreadStep + 1;
        }
        else
        {
            if (wouldReuse && invalid)
            {
                ++fallback;
                if (f.reset)
                    ++causes.reset;
                else if (f.blocked || lastBlocked)
                    ++causes.blocked;
                else if (gap)
                {
                    ++causes.gap;
                    causes.lastGapStep = f.frame > lastFrame ? f.frame - lastFrame : 0;
                    causes.lastMaxStep = f.maxStep;
                }
                else
                    ++causes.changed;
            }
            else if (wouldReuse && f.hold)
                ++held; // counted only where a reuse was given up: comparable with Reused()

            d.kind = Kind::Full;
            // After a gap the model's history is older still; composing two frames would not cover it,
            // and the saved motion belongs to a frame before the gap.
            d.composeMotion = afterReuse && !f.reset && !gap;
            ++full;
            reusedInRow = 0;
            // A spread starts on a full frame after reused ones (the model was skipped, so its detail jumps), with
            // last frame's detail to move
            if (afterReuse && usable && f.spread > 1)
                d.spreadStep = 0;
        }
        if (d.spreadStep >= 0)
        {
            d.spreadAmount = SpreadAmount((unsigned int) d.spreadStep, f.spread);
            d.saveShown = (unsigned int) d.spreadStep + 2 < f.spread;
        }
        spreadStep = d.spreadStep;

        d.historyUsable = usable;
        d.captureHistory = true;
        historyValid = false;
        pending = true;

        hasLast = true;
        lastBlocked = f.blocked;
        lastFrame = f.frame;
        lastRevision = f.revision;
        return d;
    }

    // The frame Next asked to capture was processed and its history saved (ok), or not.
    void Captured(bool ok)
    {
        if (!pending)
            return;
        pending = false;
        historyValid = ok;
    }

    // The frame could not be processed as decided (the caller ran the model instead of reusing, or a pass
    // failed to record). Next frame starts clean: full, nothing reprojected, no composed motion.
    void Drop()
    {
        historyValid = false;
        pending = false;
        reusedInRow = 0;
        spreadStep = -1;
    }

    // The spread's saved shown detail could not be written: the spread ends here (the frames after show the unblended
    // detail, as without it).
    void EndSpread() { spreadStep = -1; }

    // The reuse frame Next asked for could not be recorded (the caller ran the model instead): counted as a fallback,
    // not a reuse, and the next frame starts clean.
    void ReuseFailed()
    {
        if (reusedInRow > 0 && reused > 0)
        {
            --reused;
            ++fallback;
            ++causes.notRecorded;
        }
        Drop();
    }

    unsigned long long Full() const { return full; }
    unsigned long long Reused() const { return reused; }
    unsigned long long Fallback() const { return fallback; }
    unsigned long long Held() const { return held; }
    const FallbackCauses& Causes() const { return causes; }
};

// Why reuse cannot run where NR runs, or null when it can. Before SR it is not offered. On a finished picture it needs
// vectors that describe that picture: a game's own vectors are rendered for its scene, and the HUD and post-processing
// on top of it are not in them, so detail would be moved where nothing moved. Optical F5Low's native input measures the
// motion on the finished picture itself, which is what motionMatchesPicture says.
inline const char* UnavailableOnRoute(bool beforeUpscale, bool finishedPicture, bool motionMatchesPicture)
{
    if (beforeUpscale)
        return "unavailable while NR runs before SR";
    if (finishedPicture && !motionMatchesPicture)
        return "unavailable in Finished Picture without Optical F5Low";
    return nullptr;
}

// Whether the real frame rate is high enough for reuse. Moved detail errs by how far things move between two real
// frames, so at a low frame rate the trails around moving bodies grow. Fed the time between two NR frames (one per
// rendered frame, so frame generation's frames do not count). The rate is smoothed over about half a second; reuse
// stops below the minimum and comes back only 15% above it, since stopping it lowers the frame rate further.
class FrameRateGate
{
    double interval = 0.0; // smoothed seconds per rendered frame; 0 = no reading yet
    bool allowed = true;

public:
    static constexpr double kResumeFactor = 1.15;

    // minimumFps <= 0: no minimum. Returns whether reuse may run.
    bool Update(double seconds, double minimumFps)
    {
        if (seconds > 0.0 && seconds < 1.0) // longer is a pause (loading, a menu), not a frame rate
        {
            const double alpha = interval > 0.0 ? 1.0 - std::exp(-seconds / 0.5) : 1.0;
            interval += (seconds - interval) * alpha;
        }
        if (!(minimumFps > 0.0) || interval <= 0.0)
            allowed = true;
        else if (allowed && Fps() < minimumFps)
            allowed = false;
        else if (!allowed && Fps() >= minimumFps * kResumeFactor)
            allowed = true;
        return allowed;
    }

    double Fps() const { return interval > 0.0 ? 1.0 / interval : 0.0; }
};

// Whether the picture is moving slowly enough for reuse. Moved detail only exists where the picture was on the previous
// frame: what comes in from off-screen, and what a body uncovers, has none, and Fill reaches only about 26 pixels into
// it. Running or turning at a low frame rate brings in a strip much wider than that every frame, so a reused frame
// shows the game's own picture there while the full frame beside it shows NR's -- the edges flicker at half the frame
// rate (The Witcher 3 at 30 rendered fps, 2026-09-30). Fed the share of the last measured frame that had no detail to
// move (mean of 1 - trust, measured on the GPU), it holds reuse off while that share is high: those frames cost the
// model's full time, which is exactly the frames where reuse looked wrong.
//
// Asymmetric on purpose, and in time rather than in level: the hold starts on one reading over the threshold, since a
// single flickering frame is seen, and ends only once the readings have stayed at or under the threshold for
// kResumeAfter, since coming back while the camera is still moving starts the flicker again.
//
// One threshold, not two. A second, lower level to resume at (as FrameRateGate's 15% does for a frame rate) would latch
// for ever here: a share that settles between the two is neither a reason to hold nor a reason to resume, and steady
// moderate motion does exactly that -- 24 of 149 windows of a Witcher 3 run sat between 5% and 10%. A frame rate cannot
// sit in its band because FrameRateGate smooths it over half a second first; a per-frame share is not smoothed.
class MotionGuard
{
    bool holding = false;
    double calm = 0.0;        // seconds the readings have stayed at or under the threshold
    double sinceSample = 0.0; // seconds since the last reading

public:
    static constexpr double kResumeAfter = 0.3; // seconds of a calm picture before reuse comes back
    static constexpr double kStale = 1.0;       // no reading for this long: the measurement is not arriving

    // share: the share of the last measured frame with no detail to move (0..1), or negative when there is no new
    // reading this frame. maxDropped: the largest share reuse still runs at (0..1); 0 = never held. seconds: since the
    // last call. Returns whether reuse is held off.
    bool Update(float share, float maxDropped, double seconds)
    {
        if (!(maxDropped > 0.0f))
        {
            holding = false;
            calm = sinceSample = 0.0;
            return false;
        }
        // Anything longer than a frame is a pause (loading, a menu), not a calm picture, and must not count towards
        // coming back -- FrameRateGate rejects it the same way. The hold is simply kept across it.
        const double step = seconds > 0.0 && seconds < 1.0 ? seconds : 0.0;
        if (share >= 0.0f)
        {
            sinceSample = 0.0;
            if (share > maxDropped)
            {
                holding = true;
                calm = 0.0;
            }
            else
            {
                calm += step;
                if (calm >= kResumeAfter)
                    holding = false;
            }
        }
        else
        {
            // A frame with no reading keeps the state; readings that stop coming altogether do not keep reuse off.
            sinceSample += step;
            if (sinceSample > kStale)
            {
                holding = false;
                calm = 0.0;
            }
        }
        return holding;
    }

    bool Holding() const { return holding; }
};
} // namespace DlssNrDetailReuse
