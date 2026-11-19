#!/usr/bin/env python3
"""Generates the table of the split sum that lib/render's shading models read (lib/render/src/render_shaders.cpp): what
a GGX specular lobe reflects of an even environment, as a scale and a bias of the lobe's reflection at normal
incidence. Each entry is the integral of the lobe of pr/render_lobes.slang over the directions of light: GGX with the
height-correlated Smith visibility, with Schlick's weight between the scale and the bias. The table holds the
directional albedo of glTF's sheen lobe beside them: the Charlie distribution with its visibility, integrated alike.
lib/render's tests integrate the same lobes and compare the table with them."""

import math
import os

from tools.gen.gen_common import ROOT, banner, namespaced, table, write

OUT = os.path.join(ROOT, "lib", "render", "src", "render_split_sum_table.h")

# The table's width and height. Its columns are the roughness, from 0 to 1. Its rows are the square root of the cosine
# of the eye's angle to the normal, from 0 to 1: the split sum changes fastest where a surface is seen at a grazing
# angle, and the square root gives that end more rows.
SIDE = 32

# The number of half vectors that an entry sums, by the lobe's distribution.
SAMPLES = 4096


def split_sum(roughness, nv):
    """Returns the scale and the bias of the split sum at a roughness and a cosine of the eye's angle."""
    alpha = max(roughness * roughness, 0.002)
    alpha2 = alpha * alpha
    eye = (math.sqrt(max(1 - nv * nv, 0)), nv)
    eye_term = math.sqrt(nv * nv * (1 - alpha2) + alpha2)
    scale = 0.0
    bias = 0.0
    for i in range(SAMPLES):
        # The second coordinate is the first's radical inverse in base 2, so the pairs cover the square evenly.
        u2 = int(format(i, "032b")[::-1], 2) / 4294967296
        around = 2 * math.pi * (i + 0.5) / SAMPLES
        cosine = math.sqrt((1 - u2) / (1 + (alpha2 - 1) * u2))
        sine = math.sqrt(max(1 - cosine * cosine, 0))
        vh = eye[0] * sine * math.cos(around) + eye[1] * cosine
        nl = 2 * vh * cosine - eye[1]
        if nl <= 0 or vh <= 0:
            continue
        visibility = 0.5 / (nl * eye_term + nv * math.sqrt(nl * nl * (1 - alpha2) + alpha2))
        # The lobe times the light's cosine, over the density of the sampled direction.
        weight = visibility * nl * 4 * vh / cosine
        schlick = (1 - vh) ** 5
        scale += weight * (1 - schlick)
        bias += weight * schlick
    return scale / SAMPLES, bias / SAMPLES


def sheen_lambda_fit(x, alpha):
    t = (1 - alpha) * (1 - alpha)
    a = 21.5473 + (25.3245 - 21.5473) * t
    b = 3.82987 + (3.32435 - 3.82987) * t
    c = 0.19823 + (0.16801 - 0.19823) * t
    d = -1.97760 + (-1.27393 + 1.97760) * t
    e = -4.32054 + (-4.85967 + 4.32054) * t
    return a / (1 + b * x ** c) + d * x + e


def sheen_lambda(cosine, alpha):
    if abs(cosine) < 0.5:
        return math.exp(sheen_lambda_fit(cosine, alpha))
    return math.exp(2 * sheen_lambda_fit(0.5, alpha) - sheen_lambda_fit(1 - cosine, alpha))


# The number of directions of light that an entry of the sheen's albedo sums, along each of the hemisphere's two angles.
SHEEN_STEPS = 48


def sheen_albedo(roughness, nv):
    """Returns the directional albedo of the sheen lobe of renderSheenLobe (pr/render_lobes.slang) at a sheen roughness
    and a cosine of the eye's angle: the lobe times the light's cosine, summed over the hemisphere of light."""
    alpha = max(roughness, 0.000001) ** 2
    # The same least width as the tests' integral, below which the fit of the visibility has no meaning.
    alpha = max(alpha, 1e-3)
    nv = max(nv, 1e-3)
    eye = (math.sqrt(max(1 - nv * nv, 0)), 0.0, nv)
    lambda_eye = sheen_lambda(nv, alpha)
    total = 0.0
    for i in range(SHEEN_STEPS):
        # Even steps of the light's cosine, so that each direction stands for the same solid angle.
        nl = (i + 0.5) / SHEEN_STEPS
        sine = math.sqrt(1 - nl * nl)
        lambda_light = sheen_lambda(nl, alpha)
        visibility = min(max(1 / ((1 + lambda_eye + lambda_light) * (4 * nv * nl)), 0), 1)
        for k in range(SHEEN_STEPS):
            around = 2 * math.pi * (k + 0.5) / SHEEN_STEPS
            light = (sine * math.cos(around), sine * math.sin(around), nl)
            h = (light[0] + eye[0], light[1], light[2] + eye[2])
            length = math.sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2])
            nh = h[2] / length
            distribution = (2 + 1 / alpha) * max(1 - nh * nh, 0) ** (0.5 / alpha) / (2 * math.pi)
            total += distribution * visibility * nl
    return total * 2 * math.pi / (SHEEN_STEPS * SHEEN_STEPS)


def main():
    values = []
    for row in range(SIDE):
        nv = (row / (SIDE - 1)) ** 2
        for column in range(SIDE):
            scale, bias = split_sum(column / (SIDE - 1), nv)
            sheen = sheen_albedo(column / (SIDE - 1), nv)
            values += [round(min(max(scale, 0), 1) * 65535), round(min(max(bias, 0), 1) * 65535),
                       round(min(max(sheen, 0), 1) * 65535)]
    header = namespaced([banner(__file__), "\n// The table of the split sum and of the sheen's albedo, which render_shaders.cpp gives a renderer.\n",
                         '\n#pragma once\n\n#include "pr/base.h"\n'], [
        "\n// The table's width and height in entries.\n",
        "constexpr uint32_t kRenderSplitSumSide = %d;\n" % SIDE,
        "\n// The scale and the bias of the split sum, and the directional albedo of the sheen lobe, each as 16 bits in",
        " which 65535 is 1. An entry's column is the roughness, from 0 at the first to 1 at the last. Its row is the",
        " square root of the cosine of the eye's angle to the normal, from 0 to 1. The rows follow one another.",
        table("constexpr uint16_t kRenderSplitSum[%d]" % len(values), values, per_line=15),
    ])
    write(OUT, header)


if __name__ == "__main__":
    main()
