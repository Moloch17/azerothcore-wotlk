"""Standard 3.3.5 talent builds and glyphs per class/role spec.

Each build is its trees in the order a player fills them (spec tree first), each tree its talents in the order they
are taken. The learner gives every point to the first talent in that order that still wants ranks and may take one.
"""


import re


def tree(page, text):
    out = []
    for item in re.split(r"(?<=:\d),", text):
        item = item.strip()
        if item:
            name, ranks = item.rsplit(":", 1)
            out.append((page, name.strip(), int(ranks)))
    return out


def build(*trees):
    return [entry for t in trees for entry in t]


BUILDS = {
    ("warrior", "arms", 0): build(
        tree(0, "Improved Heroic Strike:3, Improved Rend:2, Improved Charge:2, Tactical Mastery:3, Anger Management:1,"
                " Impale:2, Deep Wounds:3, Improved Overpower:2, Two-Handed Weapon Specialization:3,"
                " Taste for Blood:3, Sweeping Strikes:1, Poleaxe Specialization:5, Weapon Mastery:2, Trauma:2,"
                " Mortal Strike:1, Strength of Arms:2, Improved Slam:2, Improved Mortal Strike:3, Endless Rage:1,"
                " Sudden Death:3, Blood Frenzy:2, Wrecking Crew:5, Bladestorm:1"),
        tree(1, "Armored to the Teeth:3, Booming Voice:2, Cruelty:5, Unbridled Wrath:5, Commanding Presence:2")),
    ("warrior", "fury", 1): build(
        tree(1, "Armored to the Teeth:3, Cruelty:5, Unbridled Wrath:5, Commanding Presence:2,"
                " Dual Wield Specialization:5, Enrage:5, Piercing Howl:1, Precision:3, Death Wish:1, Flurry:5,"
                " Bloodthirst:1,"
                " Intensify Rage:3, Improved Berserker Stance:5, Rampage:1, Bloodsurge:3, Heroic Fury:1,"
                " Unending Fury:5, Titan's Grip:1"),
        tree(0, "Improved Heroic Strike:3, Improved Rend:2, Improved Charge:2, Tactical Mastery:3, Anger Management:1,"
                " Impale:2, Deep Wounds:3")),
    ("warrior", "protection", 2): build(
        tree(2, "Improved Thunder Clap:3, Shield Specialization:2, Incite:3, Anticipation:2, Toughness:5,"
                " Last Stand:1, Improved Revenge:2, Shield Mastery:2, Improved Spell Reflection:2, Puncture:3,"
                " Improved Disciplines:2, Concussion Blow:1, Gag Order:2, One-Handed Weapon Specialization:5,"
                " Vigilance:1, Focused Rage:3, Improved Defensive Stance:2, Vitality:3, Safeguard:2, Warbringer:1,"
                " Devastate:1, Critical Block:3, Sword and Board:3, Damage Shield:2, Shockwave:1"),
        tree(0, "Improved Heroic Strike:3, Deflection:3, Tactical Mastery:3"),
        tree(1, "Armored to the Teeth:3, Booming Voice:2")),
    ("paladin", "holy", 0): build(
        tree(0, "Spiritual Focus:5, Healing Light:3, Divine Intellect:5, Aura Mastery:1, Illumination:5,"
                " Improved Lay on Hands:1, Improved Concentration Aura:1, Blessed Hands:2, Divine Favor:1,"
                " Sanctified Light:3, Purifying Power:2, Holy Power:5, Light's Grace:3, Holy Shock:1,"
                " Holy Guidance:5, Divine Illumination:1, Judgements of the Pure:5, Infusion of Light:2,"
                " Enlightened Judgements:2, Beacon of Light:1"),
        tree(1, "Divinity:5, Stoicism:3, Guardian's Favor:2, Toughness:4, Divine Sacrifice:1, Divine Guardian:2")),
    ("paladin", "protection", 1): build(
        tree(1, "Divinity:5, Stoicism:3, Guardian's Favor:2, Divine Sacrifice:1, Improved Righteous Fury:3,"
                " Toughness:5, Divine Guardian:2, Improved Hammer of Justice:2, Improved Devotion Aura:2,"
                " Blessing of Sanctuary:1, Sacred Duty:2, One-Handed Weapon Specialization:3, Holy Shield:1,"
                " Ardent Defender:3, Redoubt:3, Combat Expertise:3, Touched by the Light:3, Avenger's Shield:1,"
                " Guarded by the Light:2, Shield of the Templar:3, Judgements of the Just:2,"
                " Hammer of the Righteous:1"),
        tree(2, "Deflection:5, Improved Judgements:2, Heart of the Crusader:3, Vindication:2, Seal of Command:1,"
                " Pursuit of Justice:2, Crusade:3")),
    ("paladin", "retribution", 2): build(
        tree(2, "Benediction:5, Improved Judgements:2, Heart of the Crusader:3, Conviction:5, Seal of Command:1,"
                " Pursuit of Justice:2, Sanctity of Battle:3, Crusade:3, Two-Handed Weapon Specialization:3,"
                " Sanctified Retribution:1, Vengeance:3, Divine Purpose:2, The Art of War:2, Repentance:1,"
                " Judgements of the Wise:3, Fanaticism:3, Sanctified Wrath:2, Swift Retribution:3, Crusader Strike:1,"
                " Sheath of Light:3, Righteous Vengeance:3, Divine Storm:1"),
        tree(1, "Divinity:5, Divine Strength:5, Guardian's Favor:1"),
        tree(0, "Seals of the Pure:5")),
    ("hunter", "beast_mastery", 0): build(
        tree(0, "Improved Aspect of the Hawk:5, Focused Fire:2, Thick Hide:3, Aspect Mastery:1, Unleashed Fury:5,"
                " Ferocity:5, Intimidation:1, Spirit Bond:2, Bestial Discipline:2, Animal Handler:2, Frenzy:5,"
                " Ferocious Inspiration:3, Bestial Wrath:1, Invigoration:2, Serpent's Swiftness:5,"
                " The Beast Within:1, Cobra Strikes:3, Kindred Spirits:5, Beast Mastery:1"),
        tree(1, "Lethal Shots:5, Careful Aim:3, Mortal Shots:5, Go for the Throat:2, Aimed Shot:1,"
                " Improved Arcane Shot:1")),
    ("hunter", "marksmanship", 1): build(
        tree(1, "Focused Aim:2, Lethal Shots:5, Careful Aim:3, Mortal Shots:5, Go for the Throat:2,"
                " Improved Arcane Shot:3, Aimed Shot:1, Rapid Killing:2, Improved Stings:3, Efficiency:5,"
                " Readiness:1, Concussive Barrage:2, Barrage:3, Ranged Weapon Specialization:3, Piercing Shots:3,"
                " Trueshot Aura:1,"
                " Master Marksman:5, Silencing Shot:1, Improved Steady Shot:3, Marked for Death:5, Chimera Shot:1"),
        tree(0, "Improved Aspect of the Hawk:5, Focused Fire:1"),
        tree(2, "Improved Tracking:5, Survival Instincts:1")),
    ("hunter", "survival", 2): build(
        tree(2, "Improved Tracking:5, Survival Instincts:2, Trap Mastery:3, Entrapment:3, Survivalist:5,"
                " Survival Tactics:2,"
                " T.N.T.:3, Lock and Load:3, Hunter vs. Wild:3, Killer Instinct:3, Lightning Reflexes:5,"
                " Expose Weakness:3, Wyvern Sting:1, Thrill of the Hunt:3, Master Tactician:5, Noxious Stings:3,"
                " Black Arrow:1, Sniper Training:3, Hunting Party:3, Explosive Shot:1"),
        tree(1, "Lethal Shots:5, Careful Aim:3, Mortal Shots:2, Go for the Throat:1")),
    ("rogue", "assassination", 0): build(
        tree(0, "Improved Eviscerate:2, Malice:5, Ruthlessness:3, Puncturing Wounds:3, Lethality:5, Vigor:1,"
                " Vile Poisons:3, Improved Poisons:5, Cold Blood:1, Quick Recovery:2, Seal Fate:5, Murder:2,"
                " Overkill:1, Focused Attacks:3, Find Weakness:3, Master Poisoner:3, Mutilate:1, Cut to the Chase:5,"
                " Hunger For Blood:1"),
        tree(1, "Dual Wield Specialization:5, Improved Slice and Dice:2, Precision:5"),
        tree(2, "Relentless Strikes:5")),
    ("rogue", "combat", 1): build(
        tree(1, "Improved Sinister Strike:2, Dual Wield Specialization:5, Improved Slice and Dice:2, Precision:5,"
                " Deflection:2, Endurance:2, Lightning Reflexes:3, Aggression:5, Blade Flurry:1, Hack and Slash:5,"
                " Weapon Expertise:2, Blade Twisting:1, Vitality:3, Adrenaline Rush:1, Combat Potency:5,"
                " Surprise Attacks:1, Savage Combat:2, Prey on the Weak:5, Killing Spree:1"),
        tree(0, "Malice:5, Ruthlessness:3, Puncturing Wounds:2, Lethality:5, Vigor:1, Improved Poisons:2")),
    ("rogue", "subtlety", 2): build(
        tree(2, "Relentless Strikes:5, Opportunity:2, Camouflage:3, Elusiveness:2, Serrated Blades:3, Initiative:3,"
                " Improved Ambush:2, Preparation:1, Dirty Deeds:2, Hemorrhage:1, Heightened Senses:2,"
                " Master of Subtlety:3, Deadliness:5, Premeditation:1, Enveloping Shadows:3, Cheat Death:3,"
                " Sinister Calling:5, Honor Among Thieves:3, Shadowstep:1, Filthy Tricks:2, Waylay:2,"
                " Slaughter from the Shadows:5, Shadow Dance:1"),
        tree(0, "Malice:5, Ruthlessness:3, Puncturing Wounds:3")),
    ("priest", "discipline", 0): build(
        tree(0, "Twin Disciplines:5, Improved Inner Fire:3, Improved Power Word: Fortitude:2, Meditation:3,"
                " Inner Focus:1, Improved Power Word: Shield:3, Mental Agility:3, Mental Strength:5, Soul Warding:1,"
                " Focused Power:2, Enlightenment:3, Focused Will:3, Power Infusion:1, Renewed Hope:2, Rapture:3,"
                " Aspiration:2, Divine Aegis:3, Pain Suppression:1, Grace:2, Borrowed Time:5, Penance:1"),
        tree(1, "Improved Renew:2, Holy Specialization:5, Divine Fury:5, Desperate Prayer:1, Inspiration:3,"
                " Improved Healing:1")),
    ("priest", "holy", 1): build(
        tree(1, "Improved Renew:3, Holy Specialization:5, Divine Fury:5, Desperate Prayer:1, Inspiration:3,"
                " Improved Healing:3, Spirit of Redemption:1, Spiritual Guidance:5, Surge of Light:2,"
                " Spiritual Healing:5, Holy Concentration:3, Lightwell:1, Empowered Healing:5, Serendipity:3,"
                " Empowered Renew:3, Circle of Healing:1, Divine Providence:5, Guardian Spirit:1"),
        tree(0, "Twin Disciplines:5, Improved Inner Fire:3, Improved Power Word: Fortitude:2, Meditation:3,"
                " Inner Focus:1, Improved Power Word: Shield:2")),
    ("priest", "shadow", 2): build(
        tree(2, "Spirit Tap:3, Improved Spirit Tap:2, Darkness:5, Shadow Affinity:2, Improved Shadow Word: Pain:2,"
                " Shadow Focus:3, Improved Mind Blast:5, Mind Flay:1, Shadow Reach:2, Shadow Weaving:3,"
                " Vampiric Embrace:1, Improved Psychic Scream:2, Silence:1, Focused Mind:3, Mind Melt:2,"
                " Improved Devouring Plague:3, Shadowform:1,"
                " Shadow Power:5, Improved Shadowform:2, Misery:3, Vampiric Touch:1, Pain and Suffering:3,"
                " Twisted Faith:5, Dispersion:1"),
        tree(0, "Twin Disciplines:5, Improved Inner Fire:3, Improved Power Word: Fortitude:2")),
    ("deathknight", "blood", 0): build(
        tree(0, "Butchery:2, Blade Barrier:3, Bladed Armor:5, Rune Tap:1, Death Rune Mastery:3,"
                " Two-Handed Weapon Specialization:1, Improved Rune Tap:3, Spell Deflection:3, Vendetta:3,"
                " Veteran of the Third War:3, Mark of Blood:1, Bloody Strikes:2, Abomination's Might:2, Hysteria:1,"
                " Improved Blood Presence:2, Bloodworms:3, Improved Death Strike:2, Vampiric Blood:1,"
                " Sudden Doom:2, Will of the Necropolis:3, Heart Strike:1, Might of Mograine:3, Blood Gorged:2,"
                " Dancing Rune Weapon:1"),
        tree(1, "Improved Icy Touch:3, Runic Power Mastery:2, Toughness:5, Icy Talons:5, Lichborne:1,"
                " Annihilation:2")),
    ("deathknight", "frost", 1): build(
        tree(1, "Improved Icy Touch:3, Runic Power Mastery:2, Black Ice:5, Nerves of Cold Steel:3, Icy Talons:5,"
                " Lichborne:1, Annihilation:3, Killing Machine:5, Chill of the Grave:2, Endless Winter:2,"
                " Deathchill:1, Improved Icy Talons:1, Merciless Combat:2, Hungering Cold:1, Rime:3,"
                " Blood of the North:3,"
                " Unbreakable Armor:1, Threat of Thassarian:3, Frost Strike:1, Guile of Gorefiend:3,"
                " Tundra Stalker:5, Howling Blast:1, Glacier Rot:1"),
        tree(0, "Subversion:3, Blade Barrier:2, Bladed Armor:5, Dark Conviction:4")),
    ("deathknight", "unholy", 2): build(
        tree(2, "Vicious Strikes:2, Virulence:3, Epidemic:2, Morbidity:3, Ravenous Dead:1, Outbreak:3, Necrosis:5,"
                " Blood-Caked Blade:3, Night of the Dead:2, Unholy Blight:1, Impurity:5, Reaping:3,"
                " Master of Ghouls:1, Desolation:5, Ghoul Frenzy:1, Crypt Fever:3, Bone Shield:1,"
                " Ebon Plaguebringer:3, Scourge Strike:1, Rage of Rivendare:5, Summon Gargoyle:1, Dirge:0"),
        tree(0, "Butchery:2, Subversion:3, Bladed Armor:5, Two-Handed Weapon Specialization:2, Dark Conviction:5")),
    ("shaman", "elemental", 0): build(
        tree(0, "Convection:5, Concussion:5, Call of Flame:3, Reverberation:5, Elemental Focus:1, Elemental Fury:5,"
                " Call of Thunder:1, Unrelenting Storm:3, Elemental Precision:3, Lightning Mastery:5,"
                " Elemental Mastery:1, Storm, Earth and Fire:3, Elemental Oath:2, Lightning Overload:3,"
                " Totem of Wrath:1, Lava Flows:3, Shamanism:5, Thunderstorm:1"),
        tree(1, "Ancestral Knowledge:5, Improved Shields:3, Improved Ghost Wolf:2, Shamanistic Focus:1"),
        tree(2, "Totemic Focus:5")),
    ("shaman", "enhancement", 1): build(
        tree(1, "Enhancing Totems:3, Ancestral Knowledge:2, Thundering Strikes:5, Improved Shields:3,"
                " Elemental Weapons:3, Shamanistic Focus:1, Flurry:5, Improved Windfury Totem:2, Spirit Weapons:1,"
                " Mental Dexterity:3, Unleashed Rage:3, Weapon Mastery:3, Dual Wield:1, Dual Wield Specialization:3,"
                " Stormstrike:1, Static Shock:3, Lava Lash:1, Improved Stormstrike:2, Mental Quickness:3,"
                " Shamanistic Rage:1, Maelstrom Weapon:5, Feral Spirit:1"),
        tree(0, "Concussion:5, Call of Flame:3, Elemental Devastation:3, Elemental Focus:1, Reverberation:4")),
    ("shaman", "restoration", 2): build(
        tree(2, "Improved Healing Wave:5, Healing Grace:3, Tidal Focus:2, Improved Water Shield:3, Tidal Force:1,"
                " Ancestral Healing:3, Restorative Totems:3, Tidal Mastery:5, Healing Way:3, Nature's Swiftness:1,"
                " Focused Mind:3, Purification:5, Nature's Guardian:3, Mana Tide Totem:1, Cleanse Spirit:1,"
                " Blessing of the Eternals:2, Improved Chain Heal:2, Nature's Blessing:3, Ancestral Awakening:3,"
                " Earth Shield:1, Improved Earth Shield:2, Tidal Waves:5, Riptide:1"),
        tree(1, "Ancestral Knowledge:5, Improved Shields:3, Improved Ghost Wolf:2")),
    ("mage", "arcane", 0): build(
        tree(0, "Arcane Subtlety:2, Arcane Focus:3, Arcane Concentration:5, Spell Impact:3, Student of the Mind:3,"
                " Focus Magic:1, Arcane Meditation:3, Torment the Weak:3, Presence of Mind:1, Arcane Mind:5,"
                " Arcane Instability:3, Arcane Potency:2, Arcane Empowerment:3, Arcane Power:1, Arcane Flows:2,"
                " Mind Mastery:5, Slow:1, Missile Barrage:5, Netherwind Presence:3, Spell Power:2,"
                " Arcane Barrage:1"),
        tree(1, "Incineration:3"),
        tree(2, "Frostbite:2, Ice Floes:3, Precision:3, Ice Shards:2, Icy Veins:1")),
    ("mage", "fire", 1): build(
        tree(1, "Incineration:3, Improved Fireball:5, Ignite:5, World in Flames:3, Pyroblast:1, Improved Scorch:3,"
                " Playing with Fire:3, Critical Mass:3, Blast Wave:1, Fire Power:5, Pyromaniac:3, Combustion:1,"
                " Molten Fury:2, Empowered Fire:3, Dragon's Breath:1, Firestarter:2, Hot Streak:3, Burnout:5,"
                " Living Bomb:1"),
        tree(0, "Arcane Subtlety:2, Arcane Focus:3, Magic Absorption:2, Arcane Concentration:3, Spell Impact:3,"
                " Student of the Mind:1, Focus Magic:1, Torment the Weak:3")),
    ("mage", "frost", 2): build(
        tree(2, "Frostbite:2, Improved Frostbolt:5, Ice Shards:3, Precision:3, Piercing Ice:3, Icy Veins:1,"
                " Frost Channeling:3, Shatter:3, Cold Snap:1, Frozen Core:3, Winter's Chill:3, Ice Barrier:1,"
                " Arctic Winds:5, Empowered Frostbolt:2, Fingers of Frost:2, Brain Freeze:3,"
                " Summon Water Elemental:1, Enduring Winter:3, Chilled to the Bone:5, Deep Freeze:1"),
        tree(0, "Arcane Subtlety:2, Arcane Focus:3, Arcane Concentration:5, Spell Impact:3, Focus Magic:1,"
                " Student of the Mind:1, Torment the Weak:3")),
    ("warlock", "affliction", 0): build(
        tree(0, "Improved Curse of Agony:2, Suppression:3, Improved Corruption:5, Improved Life Tap:2, Soul Siphon:2,"
                " Amplify Curse:1, Grim Reach:1, Nightfall:2, Empowered Corruption:3, Curse of Exhaustion:1,"
                " Shadow Embrace:5,"
                " Siphon Life:1, Improved Felhunter:2, Shadow Mastery:5, Eradication:3, Contagion:5, Malediction:3,"
                " Death's Embrace:3, Unstable Affliction:1, Pandemic:1, Everlasting Affliction:5, Haunt:1"),
        tree(2, "Improved Shadow Bolt:5, Bane:5, Ruin:4")),
    ("warlock", "demonology", 1): build(
        tree(1, "Demonic Embrace:3, Fel Synergy:2, Demonic Brutality:3, Fel Vitality:3, Soul Link:1,"
                " Fel Domination:1, Demonic Aegis:3, Unholy Power:5, Master Summoner:2, Mana Feed:1,"
                " Master Conjuror:2, Master Demonologist:5, Molten Core:3, Demonic Empowerment:1,"
                " Demonic Knowledge:3, Demonic Tactics:5, Decimation:2, Improved Demonic Tactics:3,"
                " Summon Felguard:1, Nemesis:3, Demonic Pact:5, Metamorphosis:1"),
        tree(2, "Improved Shadow Bolt:5, Bane:5, Ruin:3")),
    ("warlock", "destruction", 2): build(
        tree(2, "Improved Shadow Bolt:5, Bane:5, Demonic Power:2, Shadowburn:1, Ruin:5, Intensity:2,"
                " Backlash:3, Improved Immolate:3, Devastation:1, Emberstorm:4, Conflagrate:1, Soul Leech:3,"
                " Pyroclasm:3, Shadow and Flame:5, Improved Soul Leech:2, Backdraft:3, Shadowfury:1,"
                " Empowered Imp:3, Fire and Brimstone:5, Chaos Bolt:1"),
        tree(1, "Improved Imp:3, Demonic Embrace:3, Fel Vitality:3, Demonic Brutality:2, Soul Link:1,"
                " Fel Domination:1")),
    ("druid", "balance", 0): build(
        tree(0, "Starlight Wrath:5, Moonglow:1, Nature's Majesty:2, Improved Moonfire:2, Nature's Grace:3,"
                " Nature's Splendor:1, Nature's Reach:2, Vengeance:5, Celestial Focus:3, Lunar Guidance:3,"
                " Insect Swarm:1, Improved Insect Swarm:3, Moonfury:3, Balance of Power:2, Moonkin Form:1,"
                " Improved Moonkin Form:3, Improved Faerie Fire:3, Wrath of Cenarius:5, Eclipse:3, Typhoon:1,"
                " Force of Nature:1, Earth and Moon:3, Starfall:1, Dreamstate:1"),
        tree(2, "Improved Mark of the Wild:2, Nature's Focus:3, Natural Shapeshifter:3, Subtlety:2,"
                " Omen of Clarity:1, Master Shapeshifter:2")),
    ("druid", "feral_cat", 1): build(
        tree(1, "Ferocity:5, Feral Instinct:3, Savage Fury:2, Survival Instincts:1, Sharpened Claws:3,"
                " Feral Swiftness:2, Shredding Attacks:2, Predatory Strikes:3, Primal Fury:2, Primal Precision:2,"
                " Feral Charge:1, Heart of the Wild:5, Survival of the Fittest:3, Leader of the Pack:1,"
                " Improved Leader of the Pack:2, Predatory Instincts:3, King of the Jungle:3, Mangle:1,"
                " Improved Mangle:3, Infected Wounds:3, Rend and Tear:5, Primal Gore:1, Berserk:1, Feral Aggression:1"),
        tree(2, "Improved Mark of the Wild:2, Furor:5, Natural Shapeshifter:3, Omen of Clarity:1,"
                " Master Shapeshifter:2")),
    ("druid", "feral_bear", 1): build(
        tree(1, "Ferocity:5, Feral Instinct:3, Savage Fury:2, Thick Hide:3, Survival Instincts:1,"
                " Sharpened Claws:3, Predatory Strikes:3, Primal Fury:2, Primal Precision:2, Feral Charge:1,"
                " Natural Reaction:3, Heart of the Wild:5, Survival of the Fittest:3, Leader of the Pack:1,"
                " Improved Leader of the Pack:2, Protector of the Pack:3, Primal Tenacity:3, King of the Jungle:3,"
                " Mangle:1, Improved Mangle:3, Rend and Tear:5, Berserk:1"),
        tree(2, "Improved Mark of the Wild:2, Furor:3, Natural Shapeshifter:3, Naturalist:2, Omen of Clarity:1,"
                " Master Shapeshifter:2")),
    ("druid", "restoration", 2): build(
        tree(2, "Improved Mark of the Wild:2, Nature's Focus:3, Furor:1, Naturalist:5, Intensity:3, Omen of Clarity:1,"
                " Tranquil Spirit:2, Improved Rejuvenation:3, Nature's Swiftness:1, Gift of Nature:5,"
                " Empowered Touch:2, Nature's Bounty:5, Living Spirit:2, Swiftmend:1, Natural Perfection:3,"
                " Empowered Rejuvenation:5, Living Seed:3, Revitalize:3, Tree of Life:1,"
                " Gift of the Earthmother:5, Wild Growth:1"),
        tree(0, "Genesis:5, Moonglow:3, Nature's Majesty:2, Nature's Grace:3, Nature's Splendor:1")),
}

# (class, spec) -> (major glyphs, minor glyphs), best first; item names without the "Glyph of " prefix.
GLYPHS = {
    ("warrior", "arms"): (["Rending", "Mortal Strike", "Execution"], ["Battle", "Command", "Charge"]),
    ("warrior", "fury"): (["Whirlwind", "Heroic Strike", "Execution"], ["Battle", "Command", "Bloodrage"]),
    ("warrior", "protection"): (["Blocking", "Vigilance", "Devastate"], ["Battle", "Charge", "Thunder Clap"]),
    ("paladin", "holy"): (["Holy Light", "Seal of Wisdom", "Beacon of Light"],
                          ["Lay on Hands", "Blessing of Kings", "Blessing of Wisdom"]),
    ("paladin", "protection"): (["Seal of Vengeance", "Righteous Defense", "Divine Plea"],
                                ["Sense Undead", "Blessing of Kings", "Lay on Hands"]),
    ("paladin", "retribution"): (["Seal of Command", "Judgement", "Exorcism"],
                                 ["Sense Undead", "Blessing of Might", "Lay on Hands"]),
    ("hunter", "beast_mastery"): (["Bestial Wrath", "Serpent Sting", "Steady Shot"],
                                  ["Mend Pet", "Revive Pet", "Feign Death"]),
    ("hunter", "marksmanship"): (["Serpent Sting", "Chimera Shot", "Steady Shot"],
                                 ["Mend Pet", "Revive Pet", "Feign Death"]),
    ("hunter", "survival"): (["Explosive Shot", "Serpent Sting", "Kill Shot"],
                             ["Mend Pet", "Revive Pet", "Feign Death"]),
    ("rogue", "assassination"): (["Mutilate", "Hunger for Blood", "Tricks of the Trade"],
                                 ["Pick Pocket", "Distract", "Vanish"]),
    ("rogue", "combat"): (["Killing Spree", "Slice and Dice", "Adrenaline Rush"], ["Pick Pocket", "Distract", "Vanish"]),
    ("rogue", "subtlety"): (["Hemorrhage", "Shadow Dance", "Slice and Dice"], ["Pick Pocket", "Distract", "Vanish"]),
    ("priest", "discipline"): (["Power Word: Shield", "Penance", "Flash Heal"],
                               ["Fortitude", "Shadow Protection", "Levitate"]),
    ("priest", "holy"): (["Prayer of Healing", "Circle of Healing", "Renew"],
                         ["Fortitude", "Shadow Protection", "Levitate"]),
    ("priest", "shadow"): (["Shadow", "Mind Flay", "Shadow Word: Pain"], ["Fortitude", "Shadow Protection", "Shadowfiend"]),
    ("deathknight", "blood"): (["Vampiric Blood", "Disease", "Dark Command"], ["Horn of Winter", "Blood Tap", "Raise Dead"]),
    ("deathknight", "frost"): (["Frost Strike", "Obliterate", "Disease"], ["Horn of Winter", "Pestilence", "Raise Dead"]),
    ("deathknight", "unholy"): (["Scourge Strike", "the Ghoul", "Icy Touch"], ["Horn of Winter", "Pestilence", "Raise Dead"]),
    ("shaman", "elemental"): (["Flame Shock", "Lightning Bolt", "Totem of Wrath"],
                              ["Thunderstorm", "Water Shield", "Renewed Life"]),
    ("shaman", "enhancement"): (["Stormstrike", "Flametongue Weapon", "Feral Spirit"],
                                ["Thunderstorm", "Water Shield", "Renewed Life"]),
    ("shaman", "restoration"): (["Earth Shield", "Chain Heal", "Earthliving Weapon"],
                                ["Water Shield", "Renewed Life", "Ghost Wolf"]),
    ("mage", "arcane"): (["Arcane Missiles", "Arcane Blast", "Molten Armor"], ["Arcane Intellect", "Slow Fall", "Frost Armor"]),
    ("mage", "fire"): (["Fireball", "Molten Armor", "Living Bomb"], ["Arcane Intellect", "Slow Fall", "Frost Armor"]),
    ("mage", "frost"): (["Frostbolt", "Molten Armor", "Water Elemental"], ["Arcane Intellect", "Slow Fall", "Frost Armor"]),
    ("warlock", "affliction"): (["Haunt", "Life Tap", "Quick Decay"], ["Drain Soul", "Unending Breath", "Souls"]),
    ("warlock", "demonology"): (["Life Tap", "Felguard", "Corruption"], ["Drain Soul", "Unending Breath", "Souls"]),
    ("warlock", "destruction"): (["Conflagrate", "Life Tap", "Imp"], ["Drain Soul", "Unending Breath", "Souls"]),
    ("druid", "balance"): (["Starfire", "Moonfire", "Starfall"], ["Unburdened Rebirth", "Typhoon", "the Wild"]),
    ("druid", "feral_cat"): (["Shred", "Rip", "Savage Roar"], ["Unburdened Rebirth", "Dash", "the Wild"]),
    ("druid", "feral_bear"): (["Maul", "Survival Instincts", "Growl"], ["Unburdened Rebirth", "Challenging Roar", "the Wild"]),
    ("druid", "restoration"): (["Swiftmend", "Wild Growth", "Rejuvenation"], ["Unburdened Rebirth", "the Wild", "Dash"]),
}
