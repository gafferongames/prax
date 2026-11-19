# exr scoreboard

The cases that a plain run passes, by part of the suite, and the rate. The run writes this table, and
the diff of a change to it is what the change did.

| Part               |   Cases | Passing |
|--------------------|--------:|--------:|
| openexr-images     | 283/283 |  100.0% |
| StandardShaderBall |   12/12 |  100.0% |
| recompressed       | 100/100 |  100.0% |
| **total**          | 395/395 |  100.0% |

Of the 395 files, 184 are refused by both decoders, and 14 hold deep data alone, which lib/exr does not
read. 921719683 samples are compared, and 18502928 of data windows outside their display windows are not. The parts
that decode are, by compression: none 10, rle 10, zips 103, zip 41, piz 46, pxr24 18, b44 13, b44a 11, dwaa 10, dwab 10.
