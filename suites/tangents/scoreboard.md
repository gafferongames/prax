# tangents scoreboard

The cases that a plain run passes, by part of the suite, and the rate. The run writes this table, and
the diff of a change to it is what the change did.

| Part       |   Cases | Passing |
|------------|--------:|--------:|
| MikkTSpace | 249/249 |  100.0% |
| **total**  | 249/249 |  100.0% |

The corners of the cases counted: 29907030, of which lib/mesh writes its replacement tangent at 332186 where
MikkTSpace has none of its own. Of the others, how many each generator gives MikkTSpace's tangent, how many
it gives a tangent within 1e-6 and 1e-3 radians of it, how many differ in handedness, and the largest
angle from MikkTSpace's tangent, in radians:

| Generator                                |    Equal | Within 1e-6 | Within 1e-3 | Handedness | Largest |
|------------------------------------------|---------:|------------:|------------:|-----------:|--------:|
| lib/mesh                                 | 29574764 |    29574764 |    29574790 |          0 |    1.57 |
| meshoptimizer                            |   516605 |     3886461 |    17142349 |      41968 |    3.14 |
| meshoptimizer, meshopt_TangentCompatible |   676575 |    17487735 |    29851244 |      41968 |    3.14 |

In no count: the 54 assets without a primitive that has texture coordinates, and the 0 assets that
require an extension that lib/gltf does not read.
