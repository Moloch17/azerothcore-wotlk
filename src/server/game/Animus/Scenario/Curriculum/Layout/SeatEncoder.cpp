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
    void Charge(std::size_t slot, std::chrono::steady_clock::time_point& mark)
    {
        Animus::Curriculum::SeatEncoder::ChargeObserve(slot, mark);
    }
}

void Animus::Curriculum::SeatEncoder::Observe(SeatView const& view, float* obs, uint8* mask)
{
    Layout const& layout = *view.L;
    // `obs` and `mask` arrive zeroed with the no-op allowed (StageScenario::ObserveSeat, the one caller, which also
    // zeroes the padding past this layout): zeroing them again here was a second pass over every row.

    // A block's slice of the mask, or null when no mask is wanted.
    auto const blockMask = [mask](BlockSlice const& slice) { return mask ? mask + slice.ActionFirst : nullptr; };

    auto mark = std::chrono::steady_clock::now();
    CoreBlock::ObserveCharacter(view, obs);
    Charge(std::size_t(BlockId::Core), mark);

    Player* bot = view.Bot;
    if (bot && !bot->IsAlive())
    {
        // Dead: whether it can resurrect itself, and the action that does.
        if (layout.Has(BlockId::Duel))
        {
            BlockSlice const& duel = layout.Slice(BlockId::Duel);
            DuelBlock::ObserveDead(view, obs + duel.ObsFirst, blockMask(duel));
        }
        // And where death runs on, what it can do about being dead, and the goals it can hold while it is.
        for (BlockId id : { BlockId::Death, BlockId::Goal })
            if (layout.Has(id))
            {
                BlockSlice const& slice = layout.Slice(id);
                GetBlock(id).Observe(view, obs + slice.ObsFirst, blockMask(slice));
            }
        if (mask)
            mask[0] = 1;
        return;
    }

    // A hidden target still counts as one: the blocks see no target, and the duel block searches for it.
    if (!bot || (!view.Target && !view.HiddenTarget && !ActsWithoutTarget(layout)))
        return;

    for (BlockId id : layout.Blocks)
    {
        BlockSlice const& slice = layout.Slice(id);
        GetBlock(id).Observe(view, obs + slice.ObsFirst, blockMask(slice));
        Charge(std::size_t(id), mark);
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

    // Dead: only its own resurrection, and what death running on offers (DeathBlock).
    if (!view.Bot->IsAlive())
    {
        BlockSlice const* duel = layout.Has(BlockId::Duel) ? &layout.Slice(BlockId::Duel) : nullptr;
        if (duel && action == int32(duel->ActionFirst + DuelBlock::ACTION_SELF_RESURRECT))
            GetBlock(BlockId::Duel).Apply(view, DuelBlock::ACTION_SELF_RESURRECT, result);
        if (layout.Has(BlockId::Death) && action > 0 && layout.Slice(BlockId::Death).ContainsAction(uint32(action)))
            GetBlock(BlockId::Death).Apply(view, uint32(action) - layout.Slice(BlockId::Death).ActionFirst, result);
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
    // except a standby (IsStandby), which survives everything, since waiting for the target's cast is not a thing the
    // seat stops fighting to do. (The move block's held keys are not options: they are held until the seat changes
    // them.)
    if (action > 0 && view.Option)
        for (SeatOption& option : view.Option->Slots)
            if (option.Kind != SeatOptionKind::None && !IsStandby(option.Kind))
                option = SeatOption();

    // A spell press first (Block::PressesFirst), against the world the seat observed; then every decision's upkeep,
    // whatever the action (the no-op included): this is where a running option acts.
    for (BlockId id : layout.Blocks)
        GetBlock(id).BeforePress(view);
    bool const first = block && GetBlock(*block).PressesFirst(layout, local);
    if (first)
        GetBlock(*block).Apply(view, local, result);

    for (BlockId id : layout.Blocks)
        GetBlock(id).BeforeApply(view, result);

    if (action <= 0)
        return;

    if (block && !first)
        GetBlock(*block).Apply(view, local, result);
}
