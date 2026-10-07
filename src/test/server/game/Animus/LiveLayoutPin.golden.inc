R"PIN(
constants MAX_ARENAS=16 RAID_GROUPS=8 GROUP_SEATS=5 MAX_SEATS=40 TEAM_SEATS=10 TEAM_COUNT=2 GROUP_MEMBERS=4 SPOTLIGHT_SLOTS=3 PARTY_MEMBERS=7 PACK_SLOTS=24 NAMED_ENEMY_SLOTS=4 CROWD_SLOTS=4 SIGHT_SLOTS=64 TRAIL_SAMPLES=8 STABLE_SLOTS=4 FRIEND_SLOTS=9 RANK_TIERS=3 GOAL_COUNT=12 GOAL_TARGETS=29 GOAL_JOINT_COUNT=348 BLOCK_COUNT=27 STATE_GLOBAL_COUNT=30 STATE_SEAT_FEATURES=26 STATE_ENEMY_FEATURES=37 STATE_DIM=1958 core.OBS_GLOBAL_COUNT=91 core.ACTION_FEATURES=8 core.ACTION_RANK_TIERS=3 TREE_COUNT=3 duel.OBS_COUNT_WITHOUT_STABLE=111 duel.ACTION_COUNT_WITHOUT_STABLE=10 duel.STABLE_FEATURES=5 pet.OBS_SLOT_FIRST=26 pet.ABILITY_SLOTS=6 pet.SLOT_FEATURES=8 pet.ACTION_COUNT=11 pack.OBS_GLOBAL_COUNT=2 pack.SLOT_FEATURES=43 pack.SLOT_PRESENT=0 pack.ACTION_SLOT_FIRST=0 duel.actions=start_attack,pet_attack,stop_casting,cancel_form,health_potion,mana_potion,healthstone,bandage,soulstone_self,self_resurrect,
classes warrior:1/arms/fury/protection paladin:2/holy/protection/retribution hunter:3/beast_mastery/marksmanship/survival rogue:4/assassination/combat/subtlety priest:5/discipline/holy/shadow deathknight:6/blood/frost/unholy shaman:7/elemental/enhancement/restoration mage:8/arcane/fire/frost warlock:9/affliction/demonology/destruction druid:11/balance/feral_cat/feral_bear/restoration
stage move1_controls suffix=_controls
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  compass id=2 rev=1 obs=6 actions=0 columns=6 hash=2f0a81dc4fcc185a
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=842 actions=25
stage move2_seek suffix=_seek
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=840 actions=25
stage move3_interact suffix=_interact
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2071 actions=321 columns=0 hash=a20af2d3f3fbd385
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=2911 actions=346
stage move4_follow suffix=_follow
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  party_frames id=24 rev=2 obs=84 actions=12 columns=84 hash=8171e495dd99a2ad
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=924 actions=37
stage combat1_fight suffix=_fight
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=3779 actions=350
stage combat2_packs suffix=_packs
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=3779 actions=350
stage combat3_survive suffix=_survive
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  gauntlet id=5 rev=0 obs=11 actions=3 columns=0 hash=7edc6ead75d0551b
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=3790 actions=353
stage group1_roles suffix=_roles
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  gauntlet id=5 rev=0 obs=11 actions=3 columns=0 hash=7edc6ead75d0551b
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  party_frames id=24 rev=2 obs=84 actions=12 columns=84 hash=8171e495dd99a2ad
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=3874 actions=365
stage group2_corridor suffix=_corridor
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  pack id=4 rev=0 obs=1034 actions=25 columns=0 hash=a102ef2571ed2826
  gauntlet id=5 rev=0 obs=11 actions=3 columns=0 hash=7edc6ead75d0551b
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  party_frames id=24 rev=2 obs=84 actions=12 columns=84 hash=8171e495dd99a2ad
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=4908 actions=390
stage dungeon1_pulls suffix=_pulls
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  pack id=4 rev=0 obs=1034 actions=25 columns=0 hash=a102ef2571ed2826
  gauntlet id=5 rev=0 obs=11 actions=3 columns=0 hash=7edc6ead75d0551b
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  party_frames id=24 rev=2 obs=84 actions=12 columns=84 hash=8171e495dd99a2ad
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=4908 actions=390
stage dungeon2_ragefire suffix=_ragefire
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  pack id=4 rev=0 obs=1034 actions=25 columns=0 hash=a102ef2571ed2826
  gauntlet id=5 rev=0 obs=11 actions=3 columns=0 hash=7edc6ead75d0551b
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  party_frames id=24 rev=2 obs=84 actions=12 columns=84 hash=8171e495dd99a2ad
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=4908 actions=390
stage dungeon3_deadmines suffix=_deadmines
  core id=0 rev=1 class-dependent
  move id=1 rev=5 obs=57 actions=25 columns=57 hash=a9e15899154cf795
  duel id=3 rev=0 class-dependent
  pet id=11 rev=0 class-dependent
  pack id=4 rev=0 obs=1034 actions=25 columns=0 hash=a102ef2571ed2826
  gauntlet id=5 rev=0 obs=11 actions=3 columns=0 hash=7edc6ead75d0551b
  vision id=20 rev=5 obs=11 actions=0 columns=0 hash=61f0d1db91a89c4f
  entities id=21 rev=1 obs=640 actions=0 columns=0 hash=9263e53f949f6262
  map id=22 rev=1 obs=4 actions=0 columns=0 hash=3b386d926f2e373d
  sight id=23 rev=2 obs=2903 actions=321 columns=0 hash=7c6b3a3e9f9c22d0
  party_frames id=24 rev=2 obs=84 actions=12 columns=84 hash=8171e495dd99a2ad
  combat id=25 rev=1 obs=36 actions=4 columns=36 hash=399e10a249f4718b
  goal id=26 rev=0 obs=128 actions=0 columns=0 hash=e953a7669f39d633
  class-independent totals obs=4908 actions=390
)PIN"
