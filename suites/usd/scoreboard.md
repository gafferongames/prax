# usd scoreboard

The cases that a plain run passes, by part of the suite, and the rate. The run writes this table, and
the diff of a change to it is what the change did.

| Part                          |     Cases | Passing |
|-------------------------------|----------:|--------:|
| assets, crate                 |       7/7 |  100.0% |
| assets, geometry              |   256/256 |  100.0% |
| assets, in a package          |     15/15 |  100.0% |
| assets, packages              |     15/15 |  100.0% |
| assets, refused               |       2/2 |  100.0% |
| assets, reified               |   282/282 |  100.0% |
| assets, stages                |   254/254 |  100.0% |
| assets, stages refused        |       2/2 |  100.0% |
| assets, text                  |   260/260 |  100.0% |
| specification, composition    |   138/138 |  100.0% |
| specification, crate          |   655/655 |  100.0% |
| specification, geometry       | 1280/1280 |  100.0% |
| specification, refused        |       8/8 |  100.0% |
| specification, reified        | 1274/1274 |  100.0% |
| specification, stages         | 1273/1273 |  100.0% |
| specification, stages refused |       8/8 |  100.0% |
| specification, text           |   618/618 |  100.0% |
| **total**                     | 6347/6347 |  100.0% |

Reified at the context's start, as each diagnostic names what varies over time and no track holds: the stages that have it.

| Diagnostic | Stages |
|------------|-------:|
| time-range |      5 |

Skipped, and in no part's count: the layers that hold what lib/usd does not read, and the stages that reach a layer that lib/usd or the reference does not read.

| Holds                                                            | Cases |
|------------------------------------------------------------------|------:|
| a layer in MaterialX's format, which the reference does not read |    56 |
| a root layer that lib/usd does not read                          |     5 |
| spline                                                           |     2 |
