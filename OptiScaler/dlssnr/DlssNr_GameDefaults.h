#pragma once

// Per-game defaults for the running game: Automatic exposure (DlssNr_AutoTrimDefault.h) -- the Trim (one default for
// every game) and following the game's exposure (from the known-game list) -- and detail reuse's depth tolerance
// (DlssNr_ReuseDepthDefault.h), which is one shipped default for every game except a short list measured to need
// more. Unless the user set them. Every D3D12, Vulkan and menu reader goes through here.

#include <Config.h>
#include <State.h>
#include <shaders/dlssnr/DlssNr_AutoTrimDefault.h>
#include <shaders/dlssnr/DlssNr_DetailReuseConstants.h>
#include <shaders/dlssnr/DlssNr_ExposureDefault.h>
#include <shaders/dlssnr/DlssNr_FollowGame.h>
#include <shaders/dlssnr/DlssNr_ReuseDepthDefault.h>

#include <atomic>

namespace DlssNr
{
// Whether the running game is on the list of known unexposed games. The exe does not change within a session.
inline bool KnownUnexposedGame()
{
    static const bool known = DlssNrAutoTrim::IsKnownUnexposedGame(State::Instance().gameExe);
    return known;
}

// The Automatic Trim in force: the user's, else the default.
inline float AutoTrimEffective(const Config& cfg) { return DlssNrAutoTrim::Effective(cfg.DlssNrAutoExposureTrim); }

// Whether the running game is on the list of games measured to need a relaxed detail-reuse depth tolerance.
inline bool KnownRelaxedDepthGame()
{
    static const bool known = DlssNrReuseDepth::IsKnownRelaxedDepthGame(State::Instance().gameExe);
    return known;
}

// Detail reuse's depth tolerance in force: the user's own when they set one, else the shipped default, unless this
// is a known relaxed game, which gets its own measured value instead (DlssNr_ReuseDepthDefault.h).
inline float DetailReuseDepthToleranceEffective(const Config& cfg)
{
    return DlssNrReuseDepth::Effective(cfg.DlssNrDetailReuseDepthTolerance, KnownRelaxedDepthGame(),
                                        kDlssNrDetailReuseDepthTolerance);
}

// Whether the running game is on the list of games measured to look right with Game exposure "as is".
inline bool KnownAsIsGame()
{
    static const bool known = DlssNrExposureDefault::IsKnownAsIsGame(State::Instance().gameExe);
    return known;
}

// What the game hands over for exposure this session (DlssNr_ExposureDefault.h), updated once per NR frame by the
// backend running (NoteExposureFrame).
inline DlssNrExposureDefault::Offer& GameExposureOffer()
{
    static DlssNrExposureDefault::Offer offer;
    return offer;
}

// The white point source in force: the user's, else Game exposure while the game hands over a usable one, else
// Automatic.
inline uint32_t WhitePointSourceEffective(const Config& cfg)
{
    return DlssNrExposureDefault::Source(cfg.DlssNrWhitePointSource.value_or_default(), GameExposureOffer().Usable());
}

// Game exposure's scale in force: the user's, else "as is" for a known game, else the game's setting.
inline uint32_t GameExposureScaleEffective(const Config& cfg)
{
    return DlssNrExposureDefault::Scale(cfg.DlssNrGameExposureScale.value_or_default(), KnownAsIsGame());
}

// Once per NR frame, before the white point is resolved. `offered`: the game's upscaler call carries an exposure
// texture. `reading`: the backend holds a clean value of it. Says once what the default became, and when it gave up.
inline void NoteExposureFrame(const Config& cfg, bool offered, bool reading)
{
    auto& offer = GameExposureOffer();
    const bool usableBefore = offer.Usable();
    const bool unusableBefore = offer.Unusable();
    offer.Update(offered, reading, WhitePointSourceEffective(cfg) == DlssNrExposureDefault::kSourceGame);

    if (cfg.DlssNrWhitePointSource.value_or_default() != DlssNrExposureDefault::kSourceAuto)
        return;

    if (!usableBefore && offer.Usable())
        LOG_INFO("DLSS-NR exposure: the game hands over its exposure -> Game exposure by default, {} scale ({})",
                 GameExposureScaleEffective(cfg) == DlssNrExposureDefault::kScaleAsIs ? "Native look"
                                                                                      : "Game's setting",
                 KnownAsIsGame() ? "measured for this game" : "the default");

    if (!unusableBefore && offer.Unusable())
        LOG_WARN("DLSS-NR exposure: the game's exposure gave no clean value in {} frames -> Automatic exposure by "
                 "default for this session",
                 DlssNrExposureDefault::kGiveUpFrames);
}

// Whether Automatic follows the game's exposure: the user's choice, else on for a known unexposed game.
inline bool FollowGameOn(const Config& cfg)
{
    return DlssNrAutoTrim::FollowGame(cfg.DlssNrAutoExposureFollowGame, KnownUnexposedGame());
}

// Says once which defaults the game gets and why. Call it where Automatic is metered.
inline void ReportAutoExposureDefaults()
{
    static std::atomic<bool> said { false };

    if (said.exchange(true))
        return;

    const bool known = KnownUnexposedGame();
    LOG_INFO("DLSS-NR automatic exposure: {} ({}) -> following the game's exposure {} by default; default Trim {:.3g} "
             "(+{:.1f} EV)",
             known ? "known unexposed game" : "not a known unexposed game", State::Instance().gameExe,
             known ? "on" : "off", DlssNrAutoTrim::kDefaultTrim, DlssNrAutoTrim::kDefaultEv);
}

// Logs what Follow's Track() did (DlssNr_FollowGame.h): the offset starting to ease toward Automatic, and arriving.
inline void SayFollowTrack(const DlssNrFollowGame::TrackEvent& e)
{
    if (e.started)
        LOG_INFO("DLSS-NR automatic exposure: Automatic and the game's exposure have stayed apart; easing the "
                 "calibration from {:+.2f} EV toward {:+.2f} EV",
                 e.fromEv, e.toEv);
    if (e.settled)
        LOG_INFO("DLSS-NR automatic exposure: calibration eased from {:+.2f} EV to {:+.2f} EV", e.fromEv, e.toEv);
    if (e.stopped)
        LOG_INFO("DLSS-NR automatic exposure: easing stopped at {:+.2f} EV (from {:+.2f} EV): {}", e.toEv, e.fromEv,
                 e.why);
    if (e.gameMoves)
        LOG_INFO("DLSS-NR automatic exposure: the game's own exposure moves, so the learned calibration stays as it is "
                 "(no easing) until it is learned again");
}
} // namespace DlssNr
