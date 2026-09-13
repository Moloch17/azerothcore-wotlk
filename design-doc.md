GOAL

The goal of this project is to add intelligent AI bots to world of warcraft to make all group play in the game able to
be played by a single human player.

HYPOTHESIS

The current understanding is that a hybrid approach will be needed. Reinforcement learning will be needed to train the
bots how to do combat most efficiently in a generalized way. It will be able to play every class, every spec, at every
level with level appropiate gear, in every group configuration, in every dungeon or group quest setting, and in PVP.
What will likely not be handled by a neural net is the out of combat decisions which will be handled by a traditional
decision tree. Things like moving with the player and pathfinding.

DECISIONS

These are the decisions the neural net will have to make in combat:

- When to move
- Where to move
- What target to select
- What ability to use (including all ranks)
- When to use them
- What trinkets to use
- When to use them
- What consumables to use
- When to use them

Certain out of combat actions are typically taken to prepare or recover from combat. Buffs, food summoning, player
summoning, and following the player outside of combat, etc will likely have to be done with a typical hand scripted
decision tree.

INPUTS

Nonexhaustive (yet) of all the input that will be needed to train the AI for this:

- Group member relative positions
- Enemy NPC relative positions
- Status effects of every unit in the area
- Health/resource status of every unit in the area
- Targets of all nearby units
- Cast bars of every unit in the area
- Filtered list of avaiable actions to the bot
    - Abilities on cooldown will be pre-filtered from consideration
- Hazardous terrain or terrain about to become hazardous

REWARDS

The ultimate win codition is dungeon clear which is very sparse and can't be the only reward. We will have break it into
smaller rewards to get to the same conclusion (but also grade successful fights)

Rewards:

- Damage dealt
- Non-overheal heals
- Damage mitigated
- Creature killed
- Successful interrupts
- Successful CC
- Successful dispels
-

Punishments:

- Death
- Leaving the dungeon
- Avoidable damage

MODELS

The model used will be MAPPO.
It is yet uncertain how the model(s) will be structured. I would prefer a monolith but realistically the best approach
would be to have a model for each class or even class/role. There would not be a group orchestrator. Like human players,
each agent will have it's own observations and it will make decisions independently.

METHODS

We will need to heavily bootstrap the entire thing because of the huge action space the bots will live in. It will start
with rotations at dummies to maximize dps and healing. Then will it will move on to small groups and learn threat and
group mechanics. Then move on to larger groups. Then probably end with PVP.

IDEAS

Instead of using commands, players can just use the normal chat system. For example, with a mage in the group, they can
say food? or water? and have the mage conjure and trade them some. SW tele? would have a mage open portal to stormwind.
Player can initiate a ready check and the bots can accept it and when the group is ready the main tank can engage the
fight. Summon $PLAYER_NAME will have a warlock begin a summoning ritual. Bots will automatically assist with a
summoning stone portal the player opens. Etc.
