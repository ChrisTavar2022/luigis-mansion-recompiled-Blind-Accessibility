# Obstacles for a blind player, and the way past each

Where a blind player can get stuck, from the guides (IGN's area pages, Super Mario Wiki's room pages, read 2026-11-18), the game's tables and silent test runs. Each item says what the mod does now and what is left. Areas 1 and 2 were cleared blind by the user; Area 3 is being played now. Keep this list current as items are built or confirmed in play.

## Getting started

- Disc setup: spoken, and driven by the controller (tested 2026-11-18). Browse opens Windows' own file window, which the screen reader reads; the spoken help says to type or paste the path there.
- Updates: the game offers new versions itself, spoken.

## Finding the way

- Doors that open with no door scene: the walk says "Through the door" (fixed 2026-11-18).
- Locked doors and keys: the hint button (left shoulder) says which locked doors your keys open, with the floor.
- Burning doors (Kitchen, Tea Room): under Elements in Go to, and the stairwell below the Tea Room says where water is.
- Fake doors hurt: warned near them.
- Floor and ceiling emblems (Tea Room, Cellar): under Furniture in Go to; the walk says where the flip put you (2026-11-18). Open: one Cellar floor emblem is out of the route's reach.
- Walks that stall against something now end "Blocked" instead of pushing forever (2026-11-18).

## Doors the Boos seal

- The Balcony (20 Boos) and the Secret Altar (40 Boos): the Go to menu says "sealed by the Boos until you have caught 20" (or 40) on that door until you have been through (2026-11-18). A Boo at the Balcony door holds Luigi and sends him back to the Foyer with too few Boos; in a silent test it drained health a point at a time with nothing spoken. Open: speak that event, and the count of Boos caught.
- The count of Boos caught: not found yet. The save record's bits (+0x368) were all zero in the user's save, which had already reached the Courtyard, so they may not be the caught count. The hallway door to the Courtyard (door 37, key 21) has the third Boo seal (eldoor08) at it; the guides' "5 Boos for the washroom" may be this door. Not spoken until confirmed.

## Area 3, room by room

- Courtyard: hint, the well under Furniture, Sparks warned about.
- Rec Room: "Hit N of 3" for Biff Atlas; the treadmill key hint.
- Tea Room: flying plates no longer hurt with the thrown damage setting on (2026-11-18). Hints: grabbing ghosts (wiggle the stick), then the white chest on the left table holds the ice medal, reached by the floor emblem to the ceiling and the ceiling emblem down onto the table.
- Astral Hall: hint says to light all five candelabras, then walk to the middle; until the room is lit its Observatory door loops back.
- Observatory: hint covers the telescope (search view), the balcony that opens, vacuuming the shining meteor and throwing it at the moon. Open: aiming at the moon. The moon is part of the backdrop model, not a placed object, so the lock cannot aim there yet; find the meteor's hit test in the game code, then add the moon as a lock target.
- Twins' Room: hint for the mobile and hide and seek. Open: which boxes hold the twins (types 124 to 128, `ibox`).
- Projection Room: hint (press A at the projector, seven invisible ghosts). Open: whether the ghost tone follows them while invisible.
- Safari Room: hint (shake the heads, ice for the water ghosts). Its chest holds the Balcony key.
- Fortune-teller's Room: hint for Madame Clairvoya and Mario's five items.
- Balcony (Boolossus): hint added 2026-11-18: pull him into a unicorn ice statue's horn, freeze the small Boos with ice from a statue, vacuum them. Boss speech and lock are built but unplayed.

## Area 4

- Blackout, Breaker Room, Grimmly, Clockwork Room, Armory, Cold Storage, Pipe Room, Artist's Studio, Ceramics Studio, Sealed Room, Secret Altar: hints exist.
- Open: Cellar dirt piles (type 378 `kdust`) in Go to; the roof chimney in Go to; Bowser and King Boo unplayed.
