# Principles

The rules the forge is built to, with the reason for each. They were decisions by the project's owner and are the
review checklist for any change: a change that breaks one needs the owner's say-so, not a clever argument. Rules that
came from a failure name it.

## What the bots may know and do

1. **A bot perceives only what a player perceives.** The camera, the entities and map it has seen, the UI a client
   shows (party frames, the target frame, the minimap, the dungeon map), nothing behind walls and no server lists.
   The critic (the value network used in training) may see privileged state; the policy may not. *Why:* the bots are
   meant to play on a real realm beside a human.
2. **Goal places in dungeons are what the bot discovered:** the hostiles it saw (where it last saw them, alive until it
   sees them dead), the edges of its own map and its leader. Never a live pack's or boss's position or the route's order.
   *(Decision 2026-10-07.)*
3. **Movement goes only through the player controller:** held keys and turn rates. No splines, teleports or navmesh
   moves for a bot. *Why:* it must move the way a player can.
4. **Actions on objects go through the same client packets a player sends** (select, use, cast, item use). No
   server-side shortcuts, no auto-opening doors.
5. **Nothing is masked except the physically impossible.** A bad press in a situation (a heal on a full-health friend,
   a refused cast) is judged and priced, so the bot learns not to; it is never taken away. *Why:* masks hide what the
   policy has not learned and break when the situation changes.
6. **No looting.** *(Decision 2026-10-06.)*
7. **A dead seat comes back alive at the dungeon entrance after a short delay and walks back** to the party. No
   graveyard, ghost or corpse run. *(Decision 2026-10-06.)*
8. **Every class and race.** One policy per class with its own build adapter; death knights are the exception in
   level-band dungeons, which they cannot enter at level (they start at 55). *(Decision 2026-10-07.)*

## How stages teach

9. **A stage's purpose is paid as Outcome.** Shaping and noise prices are aids that fade away. *Why:* a lesson paid as
   shaping disappears when the fade ends; stage 3 collapsed that way.
10. **Ladders are curriculum, not pass gates.** A ladder steps on its gate metric alone; it never waits on another
    ladder or a score plateau (`require_plateau: false`), and a ladder whose rungs are difficulty never steps back on
    the score, because a harder rung always scores lower. It raises a warning if its rung collapses instead.
    *Why:* M1 waited about 20M steps on a plateau with its gate long met; M2 was held at its easiest rung for 55M
    steps by a regression rule that read every harder rung as a failure.
11. **A stage ends on convergence signals alone.** Step budgets are ceilings. A gate-stepped stage can converge only at
    its top rung.
12. **No stage ends at the first death.** Wipes are scored and play goes on.
13. **Real content over synthetic arenas:** the real Stockades, Ragefire Chasm and Deadmines.
14. **No scripted teachers, baselines or imitation.** Every behaviour is discovered by the learner; comparisons and
    smoke tests use the learner. The "human" stand-in partner is a frozen learned policy. *(Decision 2026-10-07.)*
    *Why:* writing and fixing the scripts cost more than they gave; the dungeon teacher's first live run did not
    move a single seat.
15. **Seeding is by name.** A stage starts from its parent's checkpoint: columns and actions carry over by name.
    Classes only ever append to the class table; a change in a block's meaning bumps its revision.
16. **A run resumes where it left off.** Cancel saves `latest.pt`; a rebuild is followed by `resume`, never a restart
    from scratch.

## How the code is kept

17. **Dead code is deleted, not gated.** There are no switches for behaviour nobody uses; git history keeps the old code.
18. **Build the whole approved plan, then rebuild once.** The cluster builds from source and the fingerprint forces
    every machine to match, so a rebuild is costly.
19. **Nothing ships to the live realm** without the owner saying so.
20. **Branches:** work and merges go on `forge`; `master` is upstream and never pushed. No pull requests unless asked.
    A change is reviewed before it merges.
