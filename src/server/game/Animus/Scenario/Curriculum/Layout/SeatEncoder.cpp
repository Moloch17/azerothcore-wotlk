/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "SeatEncoder.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "Player.h"
#include <algorithm>
#include <chrono>

namespace
{
    /// Adds the time since `mark` to `slot` and moves `mark` to now.
    void Charge(std::atomic<uint64>& slot, std::chrono::steady_clock::time_point& mark)
    {
        auto const now = std::chrono::steady_clock::now();
        slot.fetch_add(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count()),
            std::memory_order_relaxed);
        mark = now;
    }
}

void Animus::Curriculum::SeatEncoder::Observe(SeatView const& view, float* obs, uint8* mask)
{
    Layout const& layout = *view.L;
    std::fill(obs, obs + layout.ObsDim, 0.0f);
    if (mask)
    {
        std::fill(mask, mask + layout.NumActions, 0);
        mask[0] = 1;
    }

    // A block's slice of the mask, or null when no mask is wanted.
    auto const blockMask = [mask](BlockSlice const& slice) { return mask ? mask + slice.ActionFirst : nullptr; };

    auto mark = std::chrono::steady_clock::now();
    CoreBlock::ObserveCharacter(view, obs);
    Charge(ObserveNs[std::size_t(BlockId::Core)], mark);

    Player* bot = view.Bot;
    if (bot && !bot->IsAlive())
    {
        // Dead: whether it can resurrect itself, and the action that does.
        if (layout.Has(BlockId::Duel))
        {
            BlockSlice const& duel = layout.Slice(BlockId::Duel);
            DuelBlock::ObserveDead(view, obs + duel.ObsFirst, blockMask(duel));
        }
        return;
    }

    // A hidden target still counts as one: the blocks see no target, and the duel block searches for it.
    if (!bot || (!view.Target && !view.HiddenTarget && !ActsWithoutTarget(layout)))
        return;

    for (BlockId id : layout.Blocks)
    {
        BlockSlice const& slice = layout.Slice(id);
        GetBlock(id).Observe(view, obs + slice.ObsFirst, blockMask(slice));
        Charge(ObserveNs[std::size_t(id)], mark);
    }

    // The no-op stays allowed whatever the core block decided.
    if (mask)
        mask[0] = 1;
}

void Animus::Curriculum::SeatEncoder::Apply(SeatView& view, int32 action, SeatActionResult& result)
{
    if (!view.Bot || !view.L)
        return;

    Layout const& layout = *view.L;

    // Dead: only its own resurrection.
    if (!view.Bot->IsAlive())
    {
        BlockSlice const* duel = layout.Has(BlockId::Duel) ? &layout.Slice(BlockId::Duel) : nullptr;
        if (duel && action == int32(duel->ActionFirst + DuelBlock::ACTION_SELF_RESURRECT))
            GetBlock(BlockId::Duel).Apply(view, DuelBlock::ACTION_SELF_RESURRECT, result);
        return;
    }

    if (!view.Target && !view.HiddenTarget && !ActsWithoutTarget(layout))
        return;

    std::optional<BlockId> const block = action > 0 ? layout.BlockOfAction(uint32(action)) : std::nullopt;
    uint32 const local = block ? uint32(action) - layout.Slice(*block).ActionFirst : 0;

    // An option whose clock has run out is over. Nothing else clears it -- every block that runs one checks Running()
    // and simply stops acting -- so a stale Kind counted as a running option in the seat's statistics (stage1_duel
    // 2026-09-18: paladin_heal reported one option started and 32.5 s held against a 10 s clock).
    if (view.Option)
        for (SeatOption& option : view.Option->Slots)
            if (option.Kind != SeatOptionKind::None && view.NowMs >= option.UntilMs)
                option = SeatOption();

    // A durative action runs until the policy does something else: anything but the no-op takes over from it,
    // except that a positioning option (IsPositioning: the held bearing) survives everything but the seat moving
    // its feet another way -- casting and swinging are what it keeps walking through -- an aiming option
    // (IsAiming: a held turn or pitch) survives everything but its own contradiction, and a standby (IsStandby)
    // survives everything, since waiting for the target's cast is not a thing the seat stops fighting to do. Each
    // option's own action is masked while it runs, so this cannot cancel a fresh press.
    if (action > 0 && view.Option)
    {
        bool const movement = block && GetBlock(*block).IsMovement(local);
        bool const aiming = block && GetBlock(*block).IsAiming(local);
        for (SeatOption& option : view.Option->Slots)
        {
            if (option.Kind == SeatOptionKind::None || IsStandby(option.Kind))
                continue;

            // A held turn or pitch ends only on the press that contradicts it -- its opposite, or levelling off --
            // which the move block does itself when it applies that press. Ending it on any press meant a seat
            // could not turn while it did anything else, and turning while walking is the one gait this design
            // exists to allow.
            if (IsAiming(option.Kind))
                continue;

            if (!IsPositioning(option.Kind))
            {
                option = SeatOption();
                continue;
            }

            // A bearing is the seat steering, so any other movement of the feet ends it -- there is no direction
            // that agrees with a compass point the seat chose for itself. A fresh bearing ends it here and Apply
            // starts the new one straight after, which is how one bearing replaces another. Aiming is not the
            // feet: a turn under a held bearing curves the walk rather than stopping it.
            if (movement && !aiming)
                option = SeatOption();
        }
    }

    // Every decision, whatever the action (the no-op included): this is where a running option acts.
    for (BlockId id : layout.Blocks)
        GetBlock(id).BeforeApply(view, result);

    if (action <= 0)
        return;

    if (block)
        GetBlock(*block).Apply(view, local, result);
}
