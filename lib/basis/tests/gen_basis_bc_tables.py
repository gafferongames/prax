#!/usr/bin/env python3
"""Prints the tables of basis_etc1s_bc.h: the ends of the blocks of BC1 and BC4 that hold an ETC1S block best.

A channel of an ETC1S block is a base value of five bits, one of ETC1's eight tables of modifiers, and the selectors
that its texels use. BC1 is searched as Basis Universal's transcoder made its own tables: for each base, table, run of
selectors from a lowest to a highest and each of ten ways of placing the run's selectors among BC1's four colors, the
two ends of 5 or 6 bits whose colors leave the least squared error for the run's values, with the colors a third of
the way rounded down, and the first of the best with the high end before the low. A block then takes, for its run, the
placing whose three channels' errors sum to the least, so the blocks are the ones that the reference transcoder
writes. The colors of one selector alone are the ends that make it best, a third of the way or at one end.

BC4 is searched for each set of three or four selectors that a block uses, over every two ends of 8 bits and both of
BC4's orders of them: the ends whose nearest of the eight values leave the least squared error for the set's values,
with the values between the ends taken exactly, as a GPU interpolates them.

An entry of BC1's tables is one word: the low end in its low byte, the high end in the next, and the error above them.
An entry of BC4's is the first end in its low byte, the second in the next, then from bit 16 the index of BC4 that
each selector takes, three bits each. An entry of the tables of one color is the end of the high color in its low byte
and the end of the low in the next.

Run with no argument. It needs numpy.
"""

import numpy

ETC1_MODIFIERS = ((2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183))

# The runs of selectors that BC1's tables have an entry for, from the lowest selector to the highest.
BC1_RUNS = ((0, 3), (1, 3), (0, 2), (1, 2), (2, 3), (0, 1))

# Where each placing puts the four selectors, of BC1's four colors from the low end to the high.
BC1_PLACINGS = ((0, 0, 1, 1), (0, 0, 1, 2), (0, 0, 1, 3), (0, 0, 2, 3), (0, 1, 1, 1), (0, 1, 2, 2), (0, 1, 2, 3),
                (0, 2, 3, 3), (1, 2, 2, 2), (1, 2, 3, 3))

# The sets of selectors that BC4's table has an entry for, as masks: those of three selectors or four.
BC4_SETS = (0b0111, 0b1011, 0b1101, 0b1110, 0b1111)


# The column that a line of the header reaches at most.
LIMIT = 120


def colors(table, base):
    """Returns the four values of a channel of an ETC1S block, by selector."""
    small, large = ETC1_MODIFIERS[table]
    expanded = base << 3 | base >> 2
    return [min(255, max(0, expanded + delta)) for delta in (-large, -small, small, large)]


def bc1_table(bits):
    """Returns the words of BC1's table of ends of `bits` bits, by table, base, run and placing."""
    levels = 1 << bits
    ends = numpy.arange(levels, dtype=numpy.int64)
    expanded = ends << (8 - bits) | ends >> (2 * bits - 8)
    # By high end, then low end: the four colors from the low end to the high.
    low = expanded.reshape(1, levels)
    high = expanded.reshape(levels, 1)
    palette = numpy.stack([numpy.broadcast_to(low, (levels, levels)), (low * 2 + high) // 3, (high * 2 + low) // 3,
                           numpy.broadcast_to(high, (levels, levels))])
    words = []
    for table in range(8):
        for base in range(32):
            values = colors(table, base)
            for lowest, highest in BC1_RUNS:
                for placing in BC1_PLACINGS:
                    total = numpy.zeros((levels, levels), dtype=numpy.int64)
                    for selector in range(lowest, highest + 1):
                        total += (values[selector] - palette[placing[selector]]) ** 2
                    best = int(total.argmin())
                    error = int(total.flat[best])
                    assert error <= 0xFFFF
                    words.append(best % levels | (best // levels) << 8 | error << 16)
    return words


def bc1_one_color(bits, third):
    """Returns the words of the table of one color of `bits` bits: for each value, the high end and the low that make
    it a third of the way from the high end, with a little against ends far apart, or that make it at the high end."""
    levels = 1 << bits
    ends = numpy.arange(levels, dtype=numpy.int64)
    expanded = ends << (8 - bits) | ends >> (2 * bits - 8)
    low = expanded.reshape(levels, 1)
    high = expanded.reshape(1, levels)
    words = []
    for value in range(256):
        if third:
            error = numpy.abs((high * 2 + low) // 3 - value) + numpy.abs(high - low) * 3 // 100
        else:
            error = numpy.broadcast_to(numpy.abs(high - value), (1, levels))
        best = int(error.argmin())
        words.append(best % levels | (best // levels if third else 0) << 8)
    return words


def bc4_table():
    """Returns the words of BC4's table, by table, base and set of selectors."""
    first = numpy.arange(256, dtype=numpy.float64).reshape(256, 1, 1)
    second = numpy.arange(256, dtype=numpy.float64).reshape(1, 256, 1)
    eight = numpy.arange(8, dtype=numpy.float64).reshape(1, 1, 8)
    six = numpy.minimum(eight, 5)
    # BC4's eight values, by index: the two ends, then six between them where the first is the greater, and otherwise
    # four between them, zero and 255.
    between8 = (first * (7 - (eight - 1)) + second * (eight - 1)) / 7
    between6 = (first * (5 - (six - 1)) + second * (six - 1)) / 5
    order = numpy.array([0, 1, 2, 3, 4, 5, 6, 7]).reshape(1, 1, 8)
    values8 = numpy.where(order == 0, first, numpy.where(order == 1, second, between8))
    values6 = numpy.where(order == 0, first, numpy.where(order == 1, second,
                                                         numpy.where(order == 6, 0.0, numpy.where(order == 7, 255.0,
                                                                                                  between6))))
    values = numpy.where(first > second, values8, values6)
    words = []
    for table in range(8):
        for base in range(32):
            wanted = colors(table, base)
            errors = []
            picks = []
            for value in wanted:
                distance = (values - value) ** 2
                picks.append(distance.argmin(axis=2))
                errors.append(distance.min(axis=2))
            for mask in BC4_SETS:
                total = sum(errors[s] for s in range(4) if mask >> s & 1)
                best = numpy.unravel_index(total.argmin(), total.shape)
                word = int(best[0]) | int(best[1]) << 8
                for s in range(4):
                    if mask >> s & 1:
                        word |= int(picks[s][best]) << (16 + 3 * s)
                words.append(word)
    return words


def emit(comment, name, shape, words):
    """Prints `words` as a constexpr array of `shape` under `comment`, a list that fits on a line on one line, and the
    entries of a longer one as many to a line as fit."""
    dims = "".join("[%d]" % d for d in shape)
    print("// %s" % comment)
    print("inline constexpr uint%d_t %s%s = {" % (32 if max(words) > 0xFFFF else 16, name, dims))

    def nested(at, depth, indent):
        size = 1
        for d in shape[depth + 1:]:
            size *= d
        if depth == len(shape) - 1:
            entries = ["0x%x" % word for word in words[at:at + shape[depth]]]
            one = "%s{%s}," % (indent, ", ".join(entries))
            if len(one) <= LIMIT:
                return [one]
            lines = []
            line = indent + "    "
            for entry in entries:
                piece = entry + ","
                if len(line) + 1 + len(piece) > LIMIT and line.strip():
                    lines.append(line)
                    line = indent + "    "
                line += (" " if line.strip() else "") + piece
            lines.append(line)
            return ["%s{" % indent] + lines + ["%s}," % indent]
        out = ["%s{" % indent]
        for i in range(shape[depth]):
            out += nested(at + i * size, depth + 1, indent + "    ")
        return out + ["%s}," % indent]

    lines = nested(0, 0, "")
    print("\n".join(lines[1:-1]))
    print("};\n")


HEADER = """\
// The ends of the blocks of BC1 and of BC4 that hold an ETC1S block best, for the transcoders of ETC1S to BC1, BC3, BC4
// and BC5, by ETC1's table of modifiers and the channel's base value of five bits. tests/gen_basis_bc_tables.py prints
// this file. BC1's tables are searched as Basis Universal's transcoder made its own, so the blocks are the ones that it
// writes.
//
// kBasisBc1Five and kBasisBc1Six hold, for each run of selectors of kBasisBc1Runs and each placing of its selectors of
// kBasisBc1Placings, the low end of a channel in the low byte, the high end in the next, and the squared error that
// they leave above them. kBasisBc1OneThird holds, for a value of 8 bits, the high end of 5 or 6 bits in its low byte
// and the low end in the next, which make the value a third of the way from the high end. kBasisBc1OneEnd holds the end
// that makes the value alone. kBasisBc4 holds, for each set of three or four selectors of kBasisBc4Sets, the first end
// in the low byte, the second in the next, and from bit 16 the index of BC4 that each selector takes, three bits each.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The runs of selectors that BC1's tables have an entry for, from the lowest selector to the highest.
inline constexpr uint8_t kBasisBc1Runs[6][2] = {{0, 3}, {1, 3}, {0, 2}, {1, 2}, {2, 3}, {0, 1}};

// Where each placing puts the four selectors, of BC1's four colors from the low end to the high.
inline constexpr uint8_t kBasisBc1Placings[10][4] = {
    {0, 0, 1, 1}, {0, 0, 1, 2}, {0, 0, 1, 3}, {0, 0, 2, 3}, {0, 1, 1, 1},
    {0, 1, 2, 2}, {0, 1, 2, 3}, {0, 2, 3, 3}, {1, 2, 2, 2}, {1, 2, 3, 3},
};

// The sets of selectors that BC4's table has an entry for, as masks of the selectors that a block uses.
inline constexpr uint8_t kBasisBc4Sets[5] = {0x7, 0xB, 0xD, 0xE, 0xF};
"""

FOOTER = """\
} // namespace pr_impl

PR_ASSUME_NONNULL_END"""


def main():
    print(HEADER)
    emit("The ends of red and blue, of 5 bits, by table, base, run and placing.", "kBasisBc1Five", (8, 32, 6, 10),
         bc1_table(5))
    emit("The ends of green, of 6 bits.", "kBasisBc1Six", (8, 32, 6, 10), bc1_table(6))
    emit("The ends that make a value of 8 bits a third of the way from the high end, of 5 bits and of 6.",
         "kBasisBc1OneThird", (2, 256), bc1_one_color(5, True) + bc1_one_color(6, True))
    emit("The end that makes a value of 8 bits, of 5 bits and of 6.", "kBasisBc1OneEnd", (2, 256),
         bc1_one_color(5, False) + bc1_one_color(6, False))
    emit("The ends of BC4 and the index of each selector, by table, base and set of selectors.", "kBasisBc4",
         (8, 32, 5), bc4_table())
    print(FOOTER)


if __name__ == "__main__":
    main()
