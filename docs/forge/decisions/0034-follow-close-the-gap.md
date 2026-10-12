# 0034: A follower is paid to close the gap, and charged for watching the leader walk away

**Date:** 2026-10-12 (owner: "Yes do all 3 then begin training again"). Resumed, not restarted.

**Why.** move4_follow at 40M kept 37% with the leader, flat since 30M. The 40M videos showed two kinds of follower:
ones that keep with the party (kept 60-86%) and ones that stand in one spot the whole episode with the party in view
(Ragefire's median follower kept 9%). Nothing paid a frozen follower to move: Kept pays only inside 3-10 yd, and Drift
charged only past 20 yd, so standing 10-20 yd off while the leader walked away cost nothing, and a follower that never
moved never met the reward that would teach it to. The policy is seeded from move2_seek, whose bots stop when a search
ends.

**Change.**
- `Approach` (Shaping, so the fade takes it away up the ladder): `PartyFollow.Approach` 0.005 per yard of the gap past
  BandMax closed since the last decision, charged per yard opened, the gap capped at `ApproachCapYards` 60. It is the
  difference of one capped gap, so over an episode it sums to the gap at the start less the gap at the end: going back
  and forth earns nothing. The gap is measured afresh after a death or the leader's.
- `Drift` from `DriftMovingYards` 10 (the band's edge) while the leader walks, from `DriftYards` 20 while it stands.
- Measures: `approach_yards` (the gap closed less opened: a frozen follower reads about minus the leader's lead),
  `follower_yards` (yards walked: a frozen follower reads ~0), `reward_approach`.
- The party frames nudge was not applied: `apps/forge/tools/nudge_block.py` found the block's adapter columns at a
  third of the others' spread in every fielded layout -- learning, not stuck at zero -- and redrawing them would have
  thrown that away. The tool stays for a block that does stay at zero.

**Judge it by** the share of followers with `follower_yards` near 0, Ragefire's median kept share, and the kept share
climbing past 0.75 so the ladder steps (where Approach starts fading).

**Revert:** `PartyFollow.Approach` 0 and `DriftMovingYards` = `DriftYards`.
