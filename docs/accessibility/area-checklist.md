# Area checklist

What each area asks of the player, from IGN's walkthrough (Area 1 to 4 pages, read 2026-11-06), against what the mod gives a blind player. The user cleared Areas 1 and 2 blind on 2026-11-06. Areas 3 and 4 are built from the guides and the game's tables and are not yet played. Mark items done here as they are built, and tested when the user has played them.

Type names come from the character type table on the disc (`0x8033B590`, name at +0x18). Map tables (`jmp/enemyinfo`, `generatorinfo`, `objinfo`, `furnitureinfo`) were read from `Map/map2.szp` with `tools/jmp_dump.py`.

## Everywhere

- Search view targets: done 2026-11-06. With the search view up (X), "Search view aims for you" (on) turns it to the cheese, mouse hole, telescope, crystal ball, mirror or dog house in Luigi's room and says to press A. Tested on the Foyer mirror. The game does not open the search view while a ghost is near, so the others are untested.
- Mirrors: every mirror but the Mirror Room's warps to the Foyer with A in the search view (Super Mario Wiki). Said when the search view lands on one.
- Mini bosses (portrait ghosts): boss tone while far, Bosses in Go to. Done 2026-11-06. Melody too.
- Boo-sealed doors: element door actors `eldoor07` to `eldoor10` (arg0 the room behind: 42 Washroom, 59 Balcony). Not spoken as such yet: say how many Boos the door wants, and how many are caught. Open.
- Emblems (Tea Room, Cellar) that move Luigi between floor and ceiling: not found in the map tables read so far. Open: research in the Tea Room.

## Area 1

Cleared blind by the user.

## Area 2

Cleared blind by the user. Added 2026-11-06: Bogmire hit speech, shadow pull, Mr. Luggs' food (Go to, lock, quarters), Kitchen burning door in Go to, Kitchen flying dishes and pots under the thrown damage setting.

## Area 3

- Courtyard, well, Bottom of the Well, Rec Room (punching bags, treadmill key): room hints exist. Treadmill notice untested.
- Second floor burning door to the Tea Room: `eldoor02`, at the top of the stairwell from the Rec Room's hallway door. In Go to under Elements; the stairwell says it is on fire and where water is (Courtyard spirit). Stairs route tested 2026-11-18.
- Biff Atlas: "Hit 1 of 3" from the game's counter, tested 2026-11-18.
- Tea Room: plates fly (type 147 `ifly` is under the thrown damage setting), emblems to the ceiling and onto the table for the ice medal chest: emblems open (above). Cheese at the back: search view aim.
- Sparks (explode when vacuumed) and bowling balls: probably types 70/71 `poo1`/`poo2` and 72/73 `ball1`/`ball2`, both with the ghost flag, so they get the ghost tone and the word "Ghost". Open: confirm in play, then name them and warn not to vacuum Sparks.
- Gold mouse in the second floor hallway: ghost flag, so ghost tone and "Gold Mouse".
- Astral Hall: Candles in Go to.
- Observatory: telescope (search view aim), meteor (type 228 `star`, generator star_41_1): open: add to the thrown list and the lock, so it can be thrown at the moon. Then the star on the winding path.
- Washroom 2: water above the toilet, Elements.
- Nana: yarn in the thrown list.
- Bathroom (Petunia): curtain, ice. Hint exists.
- Twins' Room: mobile on the ceiling, then hide and seek: the twins are in two of five boxes (types 124 to 128 `ibox`, generator `gemini_box`), found by which ones shake under the vacuum. Open: say which boxes hold them, or a tone.
- Billiards Room: billiard balls in the thrown list.
- Projection Room: seven invisible ghosts that show by smoke puffs. Open: check whether the ghost tone follows them while invisible.
- Fortune-teller's Room: flash the crystal ball, search view on her (crystal ball by search view aim), then A five times. Hint exists.
- Safari Room: cheese (search view aim), animal heads, water ghosts, Garbage Can Ghosts (banana warnings).
- Boolossus: phases and lock built, unplayed.

## Area 4

- Blackout: Foyer and Wardrobe hints. Grimmly: mirror back turned, hint exists.
- Breaker Room: red switch, hint exists.
- Cellar: dirt piles (type 378 `kdust`): open: list them in Go to so they can be vacuumed. Emblem: open.
- Attic trunks, Clockwork Room clocks and soldiers' keys, Armory boxes and maces: hints exist.
- Roof: elevator door, chimney to climb with A. Open: chimney in Go to.
- Ceramics Studio: Jarvis in vases, ice from the frozen vase (Elements).
- Sealed Room: vacuum the mirror's cloth, then search view aim on the mirror (warps to the Foyer).
- Guest Room, Twins' sister Sue Pea, Pipe Room sewage, Cold Storage fires, Artist's Studio: hints exist.
- Secret Altar: 40 Boos. Bowser and King Boo: phases and lock built, unplayed.
