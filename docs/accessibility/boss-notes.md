# Boss notes for blind play

What each fight asks of the player, and what a blind player needs to know to do it. The strategy comes from fan guides and wikis (sources at the end), not from the game's code, so treat details as unconfirmed until tested in play. IGN's guide could not be read by the tools.

## Built so far

`bossSpeech` in `access_world.cpp`, from what the game shows: the boss's name and health, each quarter of health lost, vanishing and returning, "Hooked. Pull away." on message 9, Boos left in map 11, and King Boo below 200 health. Exposed hearts and Bowser's inhale are not known to the mod yet.

Phases are worked out from what is in play, since the game does not name them:

- Chauncey in the Nursery: before the fight he is not in the actor list, but his ball, type 68, is. That ball in the room with no boss means vacuum the rocking horse. Type 63 is him awake: balls in play means dodge, then throw one back. A hit sends Luigi to the arena (event 50, then 64).
- Chauncey: rocking horses in play, then balls, then one ball left, then him high in the air jumping.
- Bogmire: a shadow in play.
- Boolossus: small Boos in play means split, none means whole.
- Bowser: King Boo showing means the head is off, a bomb in play means one can be thrown back.

## The lock (Lock on to bosses, on by default)

With the vacuum held in a boss fight: Luigi turns to the nearest ball, or to the boss when a ball is held or it is open. Left trigger throws straight at the boss from anywhere. When Chauncey is open, leaving the left stick alone walks Luigi to the spot in front of him. Moving the right stick takes over. Details and limits are in `project_status.md`. Sidestep is not verified, and only Chauncey has been tested.

## What every fight has in common

The same steps repeat in most fights:

1. Find the object that hurts the boss.
2. Vacuum it up.
3. Fire it at the boss, which exposes its heart.
4. Vacuum the boss while pulling against it, until its health runs out or it escapes.

So a blind player needs these, in order of value:

- Where the object is: the thrown object beacon, built.
- Where the boss is: the ghost beacon, built.
- Whether the object is in the vacuum, ready to fire: not built.
- Whether the shot is lined up with the boss: the aim cue covers the flashlight and facing, not the nozzle. The nozzle direction is at `Luigi+0x80C+0x700`.
- Whether the flashlight has touched the ghost, and whether the vacuum has hooked it: built, from the game's own messages 2 and 9 to the ghost's AI. Whether a portrait ghost's heart is exposed: not built, and the most useful next step.
- Which way to pull during a capture, and how much health the boss has left: not built.

## Chauncey (Area 1, Nursery)

- Suck up the rocking horse to start the fight.
- Vacuum a bouncing ball, then fire it at him to expose his heart.
- He throws giant rocking horses, which must be dodged.
- Useful to a blind player: the ball's position, and a cue when he can be vacuumed. Both built.
- He can be hooked only from the front, within about 400 units, and hooks come as the vacuum starts. Each hook took about 33 health in testing.

## Bogmire (Area 2, Graveyard)

- Clear the skeletons, then approach the glowing grave.
- He teleports around a round arena and pulls hard. Shadows lurch at Luigi and at Bogmire.
- Vacuum a shadow until it is a large glob, then fire it at him to expose his heart.
- Clearing shadows and cornering him makes it easier. In the vacuum, he tries to drag Luigi into them.
- Useful to a blind player: a spoken cue when a shadow is fully charged, and where Bogmire reappears. Shadows are in the thrown object list, types 211 and 212.

## Boolossus (Area 3, Balcony)

- Needs 20 Boos caught first. Fifteen small Boos combine into one large Boo, with no health bar.
- Its attack is slamming into Luigi, so the ghost damage setting removes nearly all its danger.
- Vacuum it and pull it into the horn of one of the two ice unicorn statues. That splits it into 15 small Boos.
- Take ice from the statue, freeze the small Boos, and vacuum them. They reform smaller and faster, and flee later on.
- Near the end it is too small to reach the horn alone: steer it with the C-stick.
- Useful to a blind player: a beacon on the two statues, how many Boos remain, a cue when a Boo is frozen, and the ice meter.

## King Boo and Bowser (Area 4, roof)

- Bowser breathes flame (10 damage), throws spiked balls (10) and inhales Luigi (15).
- Vacuum a spiked ball and fire it into his mouth as he inhales. The head comes off and King Boo comes out.
- King Boo has 500 health. The head floats about firing icy blasts that freeze Luigi.
- Below 200 health the head goes back on backward, and the suit runs around at random. Guides say to stay against a wall.
- Useful to a blind player: a warning when he starts to inhale, since that is the only moment a shot works; King Boo's health in steps; where the head is.

## Portrait ghosts that use the same idea

- Nana: knock yarn balls from the bowl, vacuum them, fire them at her. Three hits expose her heart. Yarn is in the thrown list.
- Slim Bankshot: vacuum the billiard balls he hits and fire them back. Three hits. Billiard balls are in the list.
- Biff Atlas: punch the punching bags toward him, three times.
- Mr. Luggs: light the candles, vacuum his food, dodge the fireballs until he tires.
- Miss Petunia: pull back the curtain, dodge the hot water, spray ice.
- Neville: look away until he yawns, then turn and flash him.
- Lydia: open the curtain so she feels the cold and gets up.

Several of these ask the player to notice a moment, such as a yawn. Each ghost's own moment is worth a spoken cue, where the state can be read.

## Sources

- [Super Mario Wiki: Chauncey](https://www.mariowiki.com/Chauncey)
- [Super Mario Wiki: Bogmire](https://www.mariowiki.com/Bogmire)

- [GameFAQs walkthrough by Dev](https://gamefaqs.gamespot.com/gamecube/516494-luigis-mansion/faqs/14725)
- [Almar's Guides: portrait ghosts](https://almarsguides.com/retro/walkthroughs/gamecube/games/luigismansion/portraitghosts/)
- [Wikibooks: Bogmire](https://en.wikibooks.org/wiki/Luigi's_Mansion/Bosses/Bogmire)
- [Super Mario Wiki: Boolossus](https://www.mariowiki.com/Boolossus)
- [Super Mario Wiki: King Boo](https://www.mariowiki.com/King_Boo_(Luigi%27s_Mansion))
