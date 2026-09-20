# Editor API requests

Things the Command Bar was asked for that the editor cannot do. Each entry is
written when the local model answers `needs_api`: it records the phrase, the
actions the model weighed and rejected, and what it thought implementing this
would take.

**These are suggestions, not specifications.** A model wrote them from one
designer's sentence; it has no view of the codebase beyond the action registry it
was handed. Read an entry as evidence that somebody wanted something -- the
valuable part -- and judge the proposed design yourself. Repeats matter more than
any single entry: the same request three times is a feature, once is a mood.

Appended by `sources/editor/intent/IntentNotes.cpp`. Safe to prune.

## Already built

**Count the WANTS, not the entries.** Eleven notes here are seven distinct phrases, because a
note is written per model turn and not per ask: "покрась пальмы в ярко-красный" is two notes
three minutes apart, one wanting. Of those seven, one asked for nothing and five have been
built, which leaves one open.

That is also where the rule at the top of this file earns its keep, but only for the right
kind of repeat. Two notes in the same minute are one person asking once. `rename` at 04:59,
05:03 and 05:11 is a person coming at it three ways because it kept not working -- and that
one did become a feature.

The built ones are kept as a line each instead of a page each: the wanting was the valuable
part, and it has been answered.

| asked for | answered by |
|---|---|
| `respond_to_chat` (x2) -- "как дела твои" | the chat turn: the command bar answers in prose |
| `rename` / `renameSelection` (x3) -- "переименуй шарики в spheres" | `rename`, with `perAsset` for "назови объекты нормальными именами" |
| `setColor` / `setBaseColor` (x2) -- "покрась пальмы в ярко-красный" | `setColor`, which tints without touching the mesh or the material |
| `createSplineZone` -- "создай сплайн зону по контуру острова" | `traceZone`, which finds the waterline itself |
| `spawnEven` -- "20 пальм разного типа равномерно по острову" | `spawn`: several assets in `target.assets`, even by construction, `zone` for the area |
| a `query` for "че почем?" | nothing, and rightly: the phrase asked for nothing |

---

## arrangeInShape(objects, shape='X') — place each rock along the two diagonal strokes of an X

- **asked**: выложи камни в зоне взгляда в виде буквы Х
- **read as**: arrange the rocks in view into the shape of the letter X
- **rejected existing because**: align only sets one shared coordinate on a single axis (a straight line), distribute only spaces objects evenly along one axis, and move shifts them by a fixed offset — none of them can place objects along two crossing diagonals, so no combination of existing actions forms an X
- **level**: data/levels/wind_test.json
- **when**: 2026-09-20 22:02

The designer wanted to arrange rocks in the camera view into the shape of the letter X. The smallest missing action is `arrangeInShape(objects, shape='X')`, which places objects along two crossing diagonal strokes. Existing actions like `align` or `distribute` only operate on single axes or fixed offsets, making it impossible to form intersecting diagonals. Implementation requires the editor to calculate a 2D path for the specific glyph and map object indices to points along that path. The main difficulty is defining the coordinate space: should the X be centered on the camera’s look-at point, or relative to the current selection’s bounding box? Additionally, handling varying object sizes to prevent overlap on the diagonals adds complexity to the placement logic.
