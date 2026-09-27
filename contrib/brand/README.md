# perfcached brand - parked designs (2026-09-21)

Decided in conversation on 2026-09-21 and parked: nothing here is wired
into the daemon, the page or the docs.  These files are the decisions,
drawn, so they are not lost.

## The family

| file | what | where it goes |
|---|---|---|
| `perfcached-mark.svg` | the anchor - four cells, upright, still | the daemon: README, package, the status page |
| `perfd-mark.svg` | the compass - four cells, lit north, red needle | the client side: libperfd, perfcli, client docs |
| `favicon.svg` | the anchor's small cut, 16-32 px | the status page favicon |
| `perfcached-lockup.svg` | mark + leaning wordmark + tagline | README header, release notes |

**One lit cell, three grey.**  The blue cell is the record *found*; the
three grey cells are where it is *kept* - which is also, literally, a
three-node fleet with one owner.  Both marks share the four cells and
the lit north, so they read as one family: the anchor holds, the
compass finds.

**The anchor is upright and still, by decision.**  Speed lives in the
wordmark (a leaning cut, tracking pulled in) and the tagline ("found
fast, kept safe"), never in the mark - motion lines on an anchor
contradict the one promise the daemon most needs to project.

**Flukes "as before".**  The arms rise straight out of the crown with
the fluke cells at mid-height.  A version with the flukes one gap
lower was drawn and rejected: the arms then bow below the crown and the
whole thing reads as a fishing hook.  If the arms ever feel short, they
lengthen OUTWARD (wider fluke cells), never downward.

**Colour.**  Two inks and one accent: ink `#1b1f24` / `#e6e9ee` (dark),
kept `#9aa4b2` / `#6b7482`, found `#2f6df6`.  The compass alone carries
a second colour: its north needle point is red `#d6453d`, the way
magnetic north is red on every real compass.  It is a device only a
compass can carry - the needle POINTS, the blue cell is what it FOUND
- so it does not leak into the anchor, and the family keeps one
shared accent.  At favicon size the red may be dropped and the blue
cell left to carry north alone.

## Considered and not taken

- A 3x2 bucket grid under a tag bar (perfcached's own structure; too
  abstract as a mark, kept as the CELL motif inside the anchor).
- An arena grid with a diagonal run; a three-node triangle (generic); a
  `pc` monogram.
- A compass rose alone (Safari-adjacent; says "find", not "keep").
- A six-cell anchor (a nail without the stock and arms).
- A barracuda - "fast, dangerous".  Not for the mark: Barracuda
  Networks owns the association in exactly this audience, and
  "dangerous" is the wrong promise for a thing operators put sessions
  in.  Kept as the project ANIMAL if one is wanted: docs, benchmarks,
  pcbench, release codenames.

## Production notes

- Set the word in a true oblique of a geometric sans, not a mechanical
  `skewX`; the `f` and `d` ascenders take the lean better drawn.
- Cut the raster set from these files: 16/32/64/512, light and dark.
- The tagline reads second: small, wide, in the "kept" grey.
