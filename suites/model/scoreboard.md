# model scoreboard

The cases that a plain run passes, by part of the suite, and the rate. The run writes this table, and
the diff of a change to it is what the change did.

| Part       |     Cases | Passing |
|------------|----------:|--------:|
| loads      |   303/303 |  100.0% |
| validation |   303/303 |  100.0% |
| slots      |   303/303 |  100.0% |
| techniques |   303/303 |  100.0% |
| drawn      |   303/303 |  100.0% |
| textures   |       7/7 |  100.0% |
| **total**  | 1522/1522 |  100.0% |

Sampled otherwise than their files state, as the materials ask: the images whose files state sRGB values
that a material reads as data.

| File states        | Images | Assets |
|--------------------|-------:|-------:|
| sRGB, read as data |      7 |      3 |

Drawn without what lib/model does not draw yet, and counted above as they are drawn:

| Not drawn                                                           | Assets |
|---------------------------------------------------------------------|-------:|
| lines                                                               |      2 |
| points                                                              |      2 |
| textures past the 5 that a device with slots samples for a material |      5 |
