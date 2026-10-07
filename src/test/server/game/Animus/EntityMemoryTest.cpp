/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#include "EntityMemory.h"
#include "gtest/gtest.h"
#include <cmath>

namespace Vi = Animus::Vision;

namespace
{
    /// An entity as a frame lists it.
    Vi::EntityInfo Entity(uint64_t guid, Vi::Vec3 at, Vi::Class what = Vi::Class::HostileCreature,
        float health = 1.0f)
    {
        Vi::EntityInfo info;
        info.Guid = guid;
        info.Id.What = what;
        info.Entry = uint32_t(guid % 1000);
        info.Centre = at;
        info.Health = health;
        info.Level = 10.0f;
        info.Reaction = -1;
        return info;
    }

    /// A frame showing `entities`, seen from the origin.
    Vi::SeenList Frame(std::initializer_list<Vi::EntityInfo> entities)
    {
        Vi::SeenList seen;
        for (Vi::EntityInfo const& info : entities)
            seen.Info[seen.Count++] = info;
        return seen;
    }
}

// Amendment 4: the memory is written from the frame's visible list and nothing else. An entity the frame does not
// show keeps every last-seen value -- whatever has become of it -- and nothing that was never listed is remembered.
TEST(EntityMemoryTest, OnlyWhatTheFrameShowsIsWritten)
{
    Vi::EntityMemory memory;
    memory.Write(Frame({ Entity(1, { 10.0f, 0.0f, 0.0f }, Vi::Class::HostileCreature, 0.8f),
        Entity(2, { 0.0f, 20.0f, 0.0f }) }));
    ASSERT_EQ(memory.Count(), 2u);

    // The second has since died and moved: the server knows, the frame does not show it.
    memory.Advance(5.0f);
    memory.Write(Frame({ Entity(1, { 11.0f, 0.0f, 0.0f }, Vi::Class::HostileCreature, 0.5f) }));
    Vi::Remembered const* unseen = memory.Find(2);
    ASSERT_NE(unseen, nullptr);
    EXPECT_FLOAT_EQ(unseen->Position.Y, 20.0f);
    EXPECT_FALSE(unseen->Dead);
    EXPECT_FLOAT_EQ(unseen->Health, 1.0f);
    EXPECT_FALSE(memory.VisibleNow(*unseen));
    EXPECT_FLOAT_EQ(memory.AgeOf(*unseen), 5.0f);

    // The one shown is brought up to date.
    Vi::Remembered const* seen = memory.Find(1);
    ASSERT_NE(seen, nullptr);
    EXPECT_FLOAT_EQ(seen->Position.X, 11.0f);
    EXPECT_FLOAT_EQ(seen->Health, 0.5f);
    EXPECT_TRUE(memory.VisibleNow(*seen));
    EXPECT_FLOAT_EQ(memory.AgeOf(*seen), 0.0f);

    // A frame showing its corpse is what tells the memory it died.
    Vi::EntityInfo corpse = Entity(2, { 0.0f, 21.0f, 0.0f }, Vi::Class::Corpse, 0.0f);
    corpse.Dead = true;
    memory.Write(Frame({ corpse }));
    EXPECT_TRUE(memory.Find(2)->Dead);
    EXPECT_EQ(memory.Find(2)->Id.What, Vi::Class::Corpse);

    // An entity with no GUID (none in the frame) and one never listed are not remembered.
    memory.Write(Frame({ Entity(0, { 1.0f, 1.0f, 0.0f }) }));
    EXPECT_EQ(memory.Count(), 2u);
    EXPECT_EQ(memory.Find(3), nullptr);
    EXPECT_EQ(memory.IdOf(3), 0u);
}

// A write with nothing in view leaves nothing "visible now", and every sighting ages by the clock.
TEST(EntityMemoryTest, SightingsAgeWithTheClock)
{
    Vi::EntityMemory memory;
    memory.Write(Frame({ Entity(7, { 3.0f, 0.0f, 0.0f }) }));
    EXPECT_TRUE(memory.VisibleNow(*memory.Find(7)));
    memory.Advance(0.25f);
    memory.Write(Frame({}));
    EXPECT_FALSE(memory.VisibleNow(*memory.Find(7)));
    EXPECT_FLOAT_EQ(memory.AgeOf(*memory.Find(7)), 0.25f);
    memory.Advance(60.0f);
    EXPECT_FLOAT_EQ(memory.AgeOf(*memory.Find(7)), 60.25f);
    // The clock never runs backwards.
    memory.Advance(-10.0f);
    EXPECT_FLOAT_EQ(memory.AgeOf(*memory.Find(7)), 60.25f);
}

// The memory id is the entity's for as long as it is remembered: through other entities coming and going, through
// frames that do not show it, and through a kept reset; a newcomer takes a free id, never one in use.
TEST(EntityMemoryTest, MemoryIdsAreStableWhileRemembered)
{
    Vi::EntityMemory memory;
    memory.Write(Frame({ Entity(100, { 1.0f, 0.0f, 0.0f }), Entity(200, { 2.0f, 0.0f, 0.0f }) }));
    uint16_t const first = memory.IdOf(100);
    uint16_t const second = memory.IdOf(200);
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);

    for (uint64_t guid = 300; guid < 320; ++guid)
    {
        memory.Advance(1.0f);
        memory.Write(Frame({ Entity(guid, { 5.0f, float(guid), 0.0f }), Entity(200, { 2.0f, 0.0f, 0.0f }) }));
        EXPECT_EQ(memory.IdOf(100), first);
        EXPECT_EQ(memory.IdOf(200), second);
        EXPECT_NE(memory.IdOf(guid), first);
        EXPECT_NE(memory.IdOf(guid), second);
    }

    // A kept reset (amendment 1): older by the offset, every id the same.
    memory.Advance(300.0f);
    EXPECT_EQ(memory.IdOf(100), first);
    EXPECT_GE(memory.AgeOf(*memory.Find(100)), 300.0f);

    // A cleared one forgets.
    memory.Clear();
    EXPECT_EQ(memory.Count(), 0u);
    EXPECT_EQ(memory.IdOf(100), 0u);
    EXPECT_DOUBLE_EQ(memory.Clock(), 0.0);
}

// Full, the least recently seen entity goes -- never one this frame shows -- and its id goes to the newcomer.
TEST(EntityMemoryTest, TheOldestSightingIsForgottenFirst)
{
    Vi::EntityMemory memory;
    memory.Configure(Vi::MemorySettings{ 3 });
    ASSERT_EQ(memory.Cap(), 3u);
    memory.Write(Frame({ Entity(1, { 1.0f, 0.0f, 0.0f }) }));
    memory.Advance(1.0f);
    memory.Write(Frame({ Entity(2, { 2.0f, 0.0f, 0.0f }) }));
    memory.Advance(1.0f);
    memory.Write(Frame({ Entity(3, { 3.0f, 0.0f, 0.0f }) }));
    uint16_t const oldest = memory.IdOf(1);
    memory.Advance(1.0f);
    memory.Write(Frame({ Entity(4, { 4.0f, 0.0f, 0.0f }) }));
    EXPECT_EQ(memory.Count(), 3u);
    EXPECT_EQ(memory.Find(1), nullptr);
    EXPECT_EQ(memory.IdOf(4), oldest);
    EXPECT_NE(memory.Find(2), nullptr);
    EXPECT_NE(memory.Find(3), nullptr);

    // The training cap is the default; a realm's may be larger.
    Vi::EntityMemory training;
    EXPECT_EQ(training.Cap(), Vi::MEMORY_TRAINING_CAP);
    training.Configure(Vi::MemorySettings{ 256 });
    EXPECT_EQ(training.Cap(), 256u);
}

// A patrol's course comes from its last two close sightings; one seen once stands still.
TEST(EntityMemoryTest, APatrolsCourseIsRemembered)
{
    Vi::EntityMemory memory;
    memory.Write(Frame({ Entity(9, { 0.0f, 0.0f, 0.0f }) }));
    EXPECT_FALSE(memory.Find(9)->Moving);
    memory.Advance(0.25f);
    memory.Write(Frame({ Entity(9, { 1.0f, 0.0f, 0.0f }) }));
    Vi::Remembered const* patrol = memory.Find(9);
    EXPECT_TRUE(patrol->Moving);
    EXPECT_NEAR(patrol->Velocity.X, 4.0f, 1e-4f);
    EXPECT_NEAR(patrol->Velocity.Y, 0.0f, 1e-4f);
    // Out of sight it keeps that course.
    memory.Advance(10.0f);
    memory.Write(Frame({}));
    EXPECT_TRUE(memory.Find(9)->Moving);
    EXPECT_NEAR(memory.Find(9)->Velocity.X, 4.0f, 1e-4f);
}

// The recalled list: only what the frame does not show, the most relevant first (recent, near, a quest's, a thing to
// use, a living hostile), at most the count asked for.
TEST(EntityMemoryTest, RecallTakesTheMostRelevantUnseen)
{
    Vi::EntityMemory memory;
    Vi::EntityInfo far = Entity(1, { 200.0f, 0.0f, 0.0f });
    Vi::EntityInfo near = Entity(2, { 5.0f, 0.0f, 0.0f });
    Vi::EntityInfo lever = Entity(3, { 60.0f, 0.0f, 0.0f }, Vi::Class::Door);
    lever.GameObject = true;
    memory.Write(Frame({ far, near, lever }));
    memory.Advance(30.0f);
    memory.Write(Frame({ Entity(4, { 1.0f, 0.0f, 0.0f }) }));

    Vi::Remembered const* out[8] = {};
    uint32_t const count = memory.Recall({ 0.0f, 0.0f, 0.0f }, out, 8);
    ASSERT_EQ(count, 3u);       // the visible one is not recalled
    EXPECT_EQ(out[0]->Guid, 2u);
    EXPECT_EQ(out[1]->Guid, 3u);
    EXPECT_EQ(out[2]->Guid, 1u);
    EXPECT_EQ(memory.Recall({ 0.0f, 0.0f, 0.0f }, out, 1), 1u);
}
