#pragma once

// XeFG's frame IDs. No D3D, no Config: a host test can include it.
//
// Intel: XeLL's markers and xefgSwapChainSetPresentId carry the same frame ID, and XeFG tags the frame's resources and
// constants with it too. XeFG follows XeLL: a frame takes the XeLL ID of the frame being rendered (0 when nobody feeds
// XeLL), picked at the frame's first use and kept for all of its tags and its present. xefg_swapchain.h asks for an ID
// unique among the frames XeFG holds, not a rising one (frames may even reach it out of order), so only an ID it gave
// one of its newest frames is refused.

#include <cstdint>

namespace xefg
{
// The IDs XeFG gave its newest frames
struct FrameIdHistory
{
    static constexpr int kSize = 8; // well past the frames XeFG holds (IFGFeature's 4 slots)

    uint32_t ids[kSize] = {};
    int count = 0;
    int next = 0;

    bool Contains(uint32_t id) const
    {
        for (int i = 0; i < count; ++i)
        {
            if (ids[i] == id)
                return true;
        }

        return false;
    }

    void Add(uint32_t id)
    {
        ids[next] = id;
        next = (next + 1) % kSize;

        if (count < kSize)
            ++count;
    }
};

// IDs for frames XeLL gives no new ID for (one it already gave, or one XeFG still holds): a range XeLL's IDs do not
// reach, so XeLL's next IDs stay free (counting on from XeFG's last ID could stay just ahead of XeLL's for good)
inline constexpr uint32_t kSpareFrameIdBase = 0x80000000u;

struct FrameIdPick
{
    uint32_t id = 0;
    bool followsXell = false;
    bool steppedBack = false; // below the last XeLL ID followed (the game restarted its IDs): XeFG's history is reset
    bool picked = false;      // picked by this call (not the frame's cached ID)
};

// Slots match IFGFeature's frame slots (frame number % kSlots)
template <int kSlots> class FrameIds
{
  public:
    // The ID of FG frame `fgFrame`: the cached one when the frame already has one, else picked now with `xellId`
    FrameIdPick For(uint64_t fgFrame, uint32_t xellId)
    {
        const auto slot = (int) (fgFrame % kSlots);

        if (_slotFrame[slot] == fgFrame && _slot[slot].id != 0)
            return _slot[slot];

        auto pick = Pick(xellId);
        _slotFrame[slot] = fgFrame;
        _slot[slot] = pick;
        _lastId = pick.id;
        _recent.Add(pick.id);
        pick.picked = true;
        return pick;
    }

    uint32_t LastFollowedXell() const { return _lastFollowedXell; }

  private:
    FrameIdPick Pick(uint32_t xellId)
    {
        // A new XeLL ID XeFG does not hold: followed. One that stays the same (XeLL's frame did not advance) is not
        // a new frame of XeLL's and gets a spare.
        if (xellId != 0 && xellId != _lastFollowedXell && !_recent.Contains(xellId))
        {
            const bool steppedBack = _lastFollowedXell != 0 && xellId < _lastFollowedXell;
            _lastFollowedXell = xellId;
            return { xellId, true, steppedBack };
        }

        uint32_t id = 0;

        do
        {
            id = xellId == 0 ? ++_lastId : _spareId++;
        } while (id == 0 || _recent.Contains(id));

        return { id, false, false };
    }

    uint64_t _slotFrame[kSlots] = {};
    FrameIdPick _slot[kSlots] = {};
    FrameIdHistory _recent;
    uint32_t _lastId = 0;
    uint32_t _lastFollowedXell = 0;
    uint32_t _spareId = kSpareFrameIdBase;
};
} // namespace xefg
