// A renderer's environments. The calling thread fills the first level of an environment's cube, from an image or from
// the sky's model, and computes the spherical harmonics of its diffuse light. The device fills the other levels, in a
// command buffer that the call submits. The frame thread takes the result from the queue when a frame begins.

#include "pr/render_environment.h"
#include "render/src/render_environment.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"

#include "render/src/render.h"
#include "render/src/render_resource.h"

namespace pr_impl {

namespace {

constexpr PrGpuFormat kCubeFormat = PrGpuFormat_Rgba16F;
constexpr uint32_t kFaces = 6;
constexpr double kPi = 3.14159265358979323846;

// The largest radiance that a cube holds, which is the largest half float.
constexpr float kMaxRadiance = 65504;

// The width of a face of a cube's last level, which holds the radiance filtered for a roughness of 1. A smaller cube
// has one level.
constexpr uint32_t kLastLevelSize = 8;

// The number of directions that the device samples for each texel of a filtered level.
constexpr uint32_t kFilterSamples = 128;

// The width of the faces that the spherical harmonics are summed over. A larger cube's texels are averaged down to it
// first: the harmonics of three bands change little across a texel of this size.
constexpr uint32_t kHarmonicsSize = 32;

// ---------------------------------------------------------------------------------------------------------------------
// Cubes in memory

// Returns the direction, not of unit length, through a point of a face of a cube. `u` and `v` run from -1 to 1 along
// the face's columns and rows, as lib/gpu lays out a cube's faces.
PrVec3 faceDirection(uint32_t face, float u, float v) {
    switch (face) {
        case 0:
            return {1, -v, -u};
        case 1:
            return {-1, -v, u};
        case 2:
            return {u, 1, v};
        case 3:
            return {u, -1, -v};
        case 4:
            return {u, -v, 1};
        default:
            return {-u, -v, -1};
    }
}

// Returns where the center of a texel lies across a face of `size` texels, from -1 to 1.
float texelCenter(uint32_t index, uint32_t size) {
    return (float(index) + 0.5f) / float(size) * 2 - 1;
}

// Returns a value that a cube holds for a radiance: zero for one that is negative or not a number.
float storedRadiance(float value) {
    if (!(value > 0))
        return 0;
    return value < kMaxRadiance ? value : kMaxRadiance;
}

// Allocates the first level of a cube of `size`: six faces of rows of texels of red, green and blue.
float* allocCube(uint32_t size) {
    return static_cast<float*>(prMemoryAlloc(size_t(kFaces) * size * size * 3 * sizeof(float)));
}

// Reads the red, green and blue of the pixel at (x, y) of an image, as a cube holds them.
void readPixel(const PrRenderRadianceImage& image, uint32_t x, uint32_t y, float* out) {
    float pixel[3];
    const uint8_t* at =
        image.pixels.data + size_t(y) * image.pixels.stride + size_t(x) * image.channels * sizeof(float);
    prMemCopy(pixel, at, sizeof(pixel));
    for (uint32_t channel = 0; channel < 3; ++channel)
        out[channel] = storedRadiance(pixel[channel]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Equirectangular images

// An equirectangular image in memory: rows of texels of red, green and blue.
struct Equirect {
    uint32_t width;
    uint32_t height;
    float* texels;
};

// The number of points along each side of a cube's texel at which an equirectangular image is read. Their mean is the
// texel's radiance, which keeps the light of a source that is smaller than the texel wherever it lies in it.
constexpr uint32_t kEquirectTaps = 3;

// Copies an image into memory with its width and height divided by `factor`: each texel is the mean of the block of
// pixels that it covers.
Equirect equirectReduce(const PrRenderRadianceImage& image, uint32_t factor) {
    Equirect e{.width = image.pixels.extent.x / factor, .height = image.pixels.extent.y / factor};
    e.texels = static_cast<float*>(prMemoryAlloc(size_t(e.width) * e.height * 3 * sizeof(float)));
    float scale = 1.0f / float(factor * factor);
    for (uint32_t y = 0; y < e.height; ++y) {
        for (uint32_t x = 0; x < e.width; ++x) {
            float sum[3] = {0, 0, 0};
            for (uint32_t dy = 0; dy < factor; ++dy) {
                for (uint32_t dx = 0; dx < factor; ++dx) {
                    float pixel[3];
                    readPixel(image, x * factor + dx, y * factor + dy, pixel);
                    for (uint32_t channel = 0; channel < 3; ++channel)
                        sum[channel] += pixel[channel];
                }
            }
            float* to = e.texels + (size_t(y) * e.width + x) * 3;
            for (uint32_t channel = 0; channel < 3; ++channel)
                to[channel] = sum[channel] * scale;
        }
    }
    return e;
}

// Returns where an image holds a direction: a column and a row, with a texel's center at whole numbers.
PrVec2 equirectPlace(const Equirect& e, PrVec3 direction) {
    float length = prVec3Length(direction);
    float u = 0.5f + prF32Atan2(direction.z, direction.x) / float(2 * kPi);
    float v = prF32Acos(prF32Clamp(direction.y / length, -1, 1)) / float(kPi);
    return {u * float(e.width) - 0.5f, v * float(e.height) - 0.5f};
}

// Adds to `sum` the image's radiance at a column and a row, filtered between the four nearest texels. The image wraps
// around its columns, and ends at its top and bottom rows.
void equirectAdd(const Equirect& e, PrVec2 place, float* sum) {
    float y = prF32Clamp(place.y, 0, float(e.height - 1));
    float x0 = prF32Floor(place.x);
    float y0 = prF32Floor(y);
    float fx = place.x - x0;
    float fy = y - y0;
    int32_t width = int32_t(e.width);
    uint32_t left = uint32_t((int32_t(x0) % width + width) % width);
    uint32_t right = (left + 1) % e.width;
    uint32_t top = uint32_t(y0);
    uint32_t bottom = top + 1 < e.height ? top + 1 : top;
    const float* a = e.texels + (size_t(top) * e.width + left) * 3;
    const float* b = e.texels + (size_t(top) * e.width + right) * 3;
    const float* c = e.texels + (size_t(bottom) * e.width + left) * 3;
    const float* d = e.texels + (size_t(bottom) * e.width + right) * 3;
    for (uint32_t channel = 0; channel < 3; ++channel) {
        float upper = a[channel] + (b[channel] - a[channel]) * fx;
        float lower = c[channel] + (d[channel] - c[channel]) * fx;
        sum[channel] += upper + (lower - upper) * fy;
    }
}

// Fills the first level of a cube of `size` from an equirectangular image.
//
// The image is first reduced, by a power of two, until it is less than eight faces wide: its texels are then no larger
// than a cube's, which a quarter turn of four faces' width would match at the middle of a face. A cube's texel is the
// mean of the image at kEquirectTaps by kEquirectTaps points across it. The points' places in the image are
// interpolated from the places of the texel's four corners, which costs one direction's trigonometry for each texel.
void cubeFromEquirect(const PrRenderRadianceImage& image, uint32_t size, float* cube) {
    uint32_t factor = 1;
    while (image.pixels.extent.x / (factor * 2) >= size * 4 && image.pixels.extent.y / (factor * 2) >= 1)
        factor *= 2;
    Equirect e = equirectReduce(image, factor);
    uint32_t corners = size + 1;
    PrVec2* places = static_cast<PrVec2*>(prMemoryAlloc(size_t(corners) * corners * sizeof(PrVec2)));
    float halfWidth = float(e.width) * 0.5f;
    for (uint32_t face = 0; face < kFaces; ++face) {
        for (uint32_t y = 0; y < corners; ++y) {
            for (uint32_t x = 0; x < corners; ++x) {
                PrVec3 d = faceDirection(face, float(x) / float(size) * 2 - 1, float(y) / float(size) * 2 - 1);
                places[y * corners + x] = equirectPlace(e, d);
            }
        }
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                PrVec2 corner[4] = {
                    places[y * corners + x],
                    places[y * corners + x + 1],
                    places[(y + 1) * corners + x],
                    places[(y + 1) * corners + x + 1],
                };
                // The corners lie on one side of the seam where the image's columns wrap.
                for (uint32_t i = 1; i < 4; ++i) {
                    if (corner[i].x - corner[0].x > halfWidth)
                        corner[i].x -= float(e.width);
                    else if (corner[i].x - corner[0].x < -halfWidth)
                        corner[i].x += float(e.width);
                }
                float sum[3] = {0, 0, 0};
                for (uint32_t tap = 0; tap < kEquirectTaps * kEquirectTaps; ++tap) {
                    float fx = (float(tap % kEquirectTaps) + 0.5f) / kEquirectTaps;
                    float fy = (float(tap / kEquirectTaps) + 0.5f) / kEquirectTaps;
                    PrVec2 upper = corner[0] + (corner[1] - corner[0]) * fx;
                    PrVec2 lower = corner[2] + (corner[3] - corner[2]) * fx;
                    equirectAdd(e, upper + (lower - upper) * fy, sum);
                }
                float* to = cube + ((size_t(face) * size + y) * size + x) * 3;
                for (uint32_t channel = 0; channel < 3; ++channel)
                    to[channel] = sum[channel] / float(kEquirectTaps * kEquirectTaps);
            }
        }
    }
    prMemoryFree(places);
    prMemoryFree(e.texels);
}

// ---------------------------------------------------------------------------------------------------------------------
// The sky
//
// The sky's model follows Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR
// 2020). Three tables are made for a sky, each from the one before: the amounts of air, haze and ozone from any point
// to space, the light that reaches a point after two scatterings or more, and the light that a viewer on the ground
// sees along each direction.

// The atmosphere, in meters and per meter: a planet's radius and the radius of its atmosphere's top, the heights over
// which the air and the haze thin by a factor of e, the height of the middle of the ozone layer and the distance from
// it at which the layer ends, the scattering of red, green and blue light by the air at the ground, and the absorption
// of each by the ozone at the layer's middle. These are Hillaire's, as his reference implementation sets up the Earth's
// atmosphere (SetupEarthAtmosphere in UnrealEngineSkyAtmosphere's SkyAtmosphereCommon.cpp). Its haze is not this one.
constexpr double kPlanetRadius = 6360e3;
constexpr double kAtmosphereRadius = 6460e3;
constexpr double kAirHeight = 8000;
constexpr double kHazeHeight = 1200;
constexpr double kOzoneMiddle = 25e3;
constexpr double kOzoneHalfWidth = 15e3;
constexpr double kAirScattering[3] = {5.802e-6, 13.558e-6, 33.1e-6};
constexpr double kOzoneAbsorption[3] = {0.650e-6, 1.881e-6, 0.085e-6};

// A clear day's haze at the ground: its extinction, the part of it that is scattering, and how far forward it scatters.
// The extinction is what makes the model's direct sunlight that of a clear day in the Illuminating Engineering
// Society's daylight availability (IES RP-21, the IESNA Lighting Handbook): the illuminance above the atmosphere times
// exp(-0.21 m) at a relative air mass of m. lib/render's tests cite where the formula's constants are published. The
// model's air and ozone alone give an exponent of 0.128, and the haze the rest: an optical depth of 0.08 straight up.
constexpr double kHazeExtinction = 6.7e-5;
constexpr double kHazeAlbedo = 0.9;
constexpr double kHazeAnisotropy = 0.76;

// The height of the viewer above the ground.
constexpr double kViewerHeight = 1;

// The number of steps of the integral along a view ray, and of the integral from a point to space.
constexpr uint32_t kViewSteps = 32;
constexpr uint32_t kDepthSteps = 24;

// A ray is the distance `radius` of its start from the planet's center, and the cosine `cosine` of the angle between
// its direction and the zenith at its start.

// Returns the distance along a ray to the atmosphere's top.
double distanceToSpace(double radius, double cosine) {
    double b = radius * cosine;
    return -b + prF64Sqrt(prF64Max(b * b - radius * radius + kAtmosphereRadius * kAtmosphereRadius, 0));
}

// Returns true if a ray meets the planet.
bool meetsPlanet(double radius, double cosine) {
    return cosine < 0 && radius * radius * (cosine * cosine - 1) + kPlanetRadius * kPlanetRadius > 0;
}

// Returns the distance along a ray that meets the planet to the ground.
double distanceToGround(double radius, double cosine) {
    double b = radius * cosine;
    return -b - prF64Sqrt(prF64Max(b * b - radius * radius + kPlanetRadius * kPlanetRadius, 0));
}

// Returns the distance from the planet's center of the point at `distance` along a ray.
double radiusAlong(double radius, double cosine, double distance) {
    return prF64Sqrt(radius * radius + distance * distance + 2 * radius * cosine * distance);
}

// The amounts of air, haze and ozone at a point, relative to the air's and the haze's at the ground and to the ozone's
// at the middle of its layer. Along a path, their integrals, in meters.
struct Depth {
    double air;
    double haze;
    double ozone;
};

// Returns the densities at a distance from the planet's center.
Depth densityAt(double radius) {
    double height = prF64Max(radius - kPlanetRadius, 0);
    return {
        prF64Exp(-height / kAirHeight),
        prF64Exp(-height / kHazeHeight),
        prF64Max(1 - prF64Abs(height - kOzoneMiddle) / kOzoneHalfWidth, 0),
    };
}

// Returns the mean of a density that thins by a factor of e over `scale`, along a path that climbs evenly from one
// height to another.
double meanDensity(double from, double to, double scale) {
    double climb = (to - from) / scale;
    if (prF64Abs(climb) < 1e-4)
        return prF64Exp(-from / scale);
    return (prF64Exp(-from / scale) - prF64Exp(-to / scale)) / climb;
}

// Returns the mean densities along a step of a ray, from the distances from the planet's center of its start, its
// middle and its end. A step may be long against the heights over which the air and the haze thin, where the densities
// at its middle would stand for it badly.
Depth densityBetween(double from, double middle, double to) {
    double low = prF64Max(from - kPlanetRadius, 0);
    double half = prF64Max(middle - kPlanetRadius, 0);
    double high = prF64Max(to - kPlanetRadius, 0);
    return {
        (meanDensity(low, half, kAirHeight) + meanDensity(half, high, kAirHeight)) * 0.5,
        (meanDensity(low, half, kHazeHeight) + meanDensity(half, high, kHazeHeight)) * 0.5,
        densityAt(middle).ozone,
    };
}

// What a channel's light loses along a meter of densities `density`, or along a path of amounts `density`, for an
// amount of haze `haze` of a clear day's.
double extinction(Depth density, uint32_t channel, double haze) {
    return kAirScattering[channel] * density.air + kHazeExtinction * haze * density.haze +
        kOzoneAbsorption[channel] * density.ozone;
}

// What a meter of densities `density` scatters of a channel's light.
double scattering(Depth density, uint32_t channel, double haze) {
    return kAirScattering[channel] * density.air + kHazeExtinction * kHazeAlbedo * haze * density.haze;
}

// The step `index` of `steps` along a path of `length`: its middle and its length. The steps lengthen with the square
// of the distance, since the air is densest at the path's start.
struct Step {
    double at;
    double length;
};

Step stepOf(double length, uint32_t index, uint32_t steps) {
    double from = double(index) / steps;
    double to = double(index + 1) / steps;
    return {length * (from * from + to * to) * 0.5, length * (to * to - from * from)};
}

// Returns the amounts of air, haze and ozone along a ray to the atmosphere's top.
Depth depthToSpace(double radius, double cosine, uint32_t steps) {
    double length = distanceToSpace(radius, cosine);
    Depth depth{};
    for (uint32_t i = 0; i < steps; ++i) {
        Step step = stepOf(length, i, steps);
        Depth density = densityAt(radiusAlong(radius, cosine, step.at));
        depth.air += density.air * step.length;
        depth.haze += density.haze * step.length;
        depth.ozone += density.ozone * step.length;
    }
    return depth;
}

// The amounts along the rays to space that do not meet the planet, by the ray's start and its direction, as Bruneton
// and Neyret lay out their table of transmittance ("Precomputed Atmospheric Scattering", 2008). A row is a distance
// from the start to the horizon, which grows with the square root of the height, and a column is a distance along the
// ray to the atmosphere's top, from the least at that height, straight up, to the greatest, toward the horizon. The
// amounts are close to linear in that distance, which is why a table this small holds them.
constexpr uint32_t kDepthRows = 16;
constexpr uint32_t kDepthColumns = 48;

struct DepthTable {
    Depth entries[kDepthRows][kDepthColumns];
};

// The distance from the horizon of a point on the ground to the atmosphere's top.
constexpr double kHorizonToSpace = 1132254.3883774;

void depthTableFill(DepthTable* table) {
    for (uint32_t row = 0; row < kDepthRows; ++row) {
        double toHorizon = kHorizonToSpace * row / (kDepthRows - 1);
        double radius = prF64Sqrt(toHorizon * toHorizon + kPlanetRadius * kPlanetRadius);
        double least = kAtmosphereRadius - radius;
        double most = toHorizon + kHorizonToSpace;
        for (uint32_t column = 0; column < kDepthColumns; ++column) {
            double distance = least + (most - least) * column / (kDepthColumns - 1);
            double cosine = 1;
            if (distance > 0) {
                cosine = (kHorizonToSpace * kHorizonToSpace - toHorizon * toHorizon - distance * distance) /
                    (2 * radius * distance);
            }
            table->entries[row][column] = depthToSpace(radius, prF64Max(-1, prF64Min(1, cosine)), kDepthSteps);
        }
    }
}

// Returns the amounts along a ray to space that does not meet the planet, between the table's nearest entries.
Depth depthLookup(const DepthTable& table, double radius, double cosine) {
    double toHorizon = prF64Sqrt(prF64Max(radius * radius - kPlanetRadius * kPlanetRadius, 0));
    double least = kAtmosphereRadius - radius;
    double most = toHorizon + kHorizonToSpace;
    double distance = distanceToSpace(radius, cosine);
    double row = prF64Min(toHorizon / kHorizonToSpace, 1) * (kDepthRows - 1);
    double column = most > least ? prF64Max(0, prF64Min(1, (distance - least) / (most - least))) : 0;
    column *= kDepthColumns - 1;
    uint32_t row0 = uint32_t(row) < kDepthRows - 1 ? uint32_t(row) : kDepthRows - 2;
    uint32_t column0 = uint32_t(column) < kDepthColumns - 1 ? uint32_t(column) : kDepthColumns - 2;
    double fr = row - row0;
    double fc = column - column0;
    Depth depth{};
    for (uint32_t i = 0; i < 4; ++i) {
        const Depth& corner = table.entries[row0 + (i >> 1)][column0 + (i & 1)];
        double weight = ((i >> 1) != 0 ? fr : 1 - fr) * ((i & 1) != 0 ? fc : 1 - fc);
        depth.air += corner.air * weight;
        depth.haze += corner.haze * weight;
        depth.ozone += corner.ozone * weight;
    }
    return depth;
}

// What is fixed for a sky while its tables are made.
struct SkyAir {
    double haze;                // the amount of haze, of a clear day's
    double albedo[3];           // the part of the light that the ground reflects
    DepthTable depths;
};

// Writes the part of each channel's sunlight that reaches a point through the atmosphere, for a sun at the cosine
// `cosine` from the point's zenith: none in the planet's shadow.
void sunlightAt(const SkyAir& air, double radius, double cosine, double* out) {
    if (meetsPlanet(radius, cosine)) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    Depth depth = depthLookup(air.depths, radius, cosine);
    for (uint32_t channel = 0; channel < 3; ++channel)
        out[channel] = prF64Exp(-extinction(depth, channel, air.haze));
}

// Takes a step of the integral of scattered light along a ray, of `length` through densities `density`. `through` is
// the part of each channel's light that crosses from the ray's start to the step's start, and becomes the part that
// crosses to its end. `weight` is the integral over the step of the part that crosses from the ray's start to each
// point of it. Within a step, the loss is taken as even, which holds the integral together where a step is long against
// the distance that light travels in the air: the energy-conserving form of Hillaire's "Physically Based and Unified
// Volumetric Rendering in Frostbite" (SIGGRAPH 2015), which his sky's reference implementation integrates with.
void rayStep(const SkyAir& air, Depth density, double length, double* through, double* weight) {
    for (uint32_t channel = 0; channel < 3; ++channel) {
        double loss = extinction(density, channel, air.haze);
        double kept = prF64Exp(-loss * length);
        weight[channel] = through[channel] * (loss * length > 1e-9 ? (1 - kept) / loss : length);
        through[channel] *= kept;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Light scattered more than once

// The light that reaches a point after two scatterings or more and is scattered there, by the point's height and by the
// cosine of the sun's angle to its zenith, for a unit of the sun's irradiance and of the point's scattering: Hillaire's
// approximation. The light scattered twice is integrated over the directions about the point, with the ground's
// reflection, as if every scattering were even in every direction. The orders after it are taken to be the same light
// times a constant part each, the part of the light scattered about the point that the air scatters again, so that they
// sum as a geometric series.
//
// The rows are heights that grow with the square of the row. The columns span the cosines that the points along a
// viewer's rays have: the sun's angle to the viewer's zenith, and the angle to either side of it that the planet's
// curve turns a point's zenith by before the ray leaves the atmosphere.
constexpr uint32_t kMultiRows = 12;
constexpr uint32_t kMultiColumns = 6;

// The directions about a point that the light is integrated over: cosines of the angle to the zenith, by turns about it
// over the half circle on one side of the sun, since the other side mirrors it. And the steps along each.
constexpr uint32_t kMultiCosines = 6;
constexpr uint32_t kMultiTurns = 4;
constexpr uint32_t kMultiSteps = 12;

// The angle that the planet's curve turns the zenith by along a ray from the ground to the atmosphere's top, at most,
// in radians.
constexpr double kZenithTurn = 0.18;

struct MultiTable {
    double lowCosine;
    double highCosine;
    double entries[kMultiRows][kMultiColumns][3];
};

double multiRowHeight(uint32_t row) {
    double t = double(row) / (kMultiRows - 1);
    return (kAtmosphereRadius - kPlanetRadius) * t * t;
}

// Computes the table's entry for a point at `radius` under a sun at `sunCosine` from its zenith.
void multiIntegrate(const SkyAir& air, double radius, double sunCosine, double* out) {
    PrDVec3 point{0, radius, 0};
    PrDVec3 sun{prF64Sqrt(prF64Max(1 - sunCosine * sunCosine, 0)), sunCosine, 0};
    double second[3] = {0, 0, 0};       // the light scattered twice, summed over the directions
    double again[3] = {0, 0, 0};        // the part of the point's light that is scattered again, summed likewise
    for (uint32_t i = 0; i < kMultiCosines; ++i) {
        double cosine = (i + 0.5) / kMultiCosines * 2 - 1;
        double sine = prF64Sqrt(1 - cosine * cosine);
        bool grounded = meetsPlanet(radius, cosine);
        double length = grounded ? distanceToGround(radius, cosine) : distanceToSpace(radius, cosine);
        for (uint32_t k = 0; k < kMultiTurns; ++k) {
            double turn = kPi * (k + 0.5) / kMultiTurns;
            PrDVec3 direction{sine * prF64Cos(turn), cosine, sine * prF64Sin(turn)};
            double through[3] = {1, 1, 1};
            double fromRadius = radius;
            for (uint32_t step = 0; step < kMultiSteps; ++step) {
                PrDVec3 at = point + direction * (length * (step + 0.5) / kMultiSteps);
                double atRadius = prDVec3Length(at);
                double toRadius = radiusAlong(radius, cosine, length * (step + 1) / kMultiSteps);
                Depth density = densityBetween(fromRadius, atRadius, toRadius);
                fromRadius = toRadius;
                double weight[3];
                double sunlight[3];
                rayStep(air, density, length / kMultiSteps, through, weight);
                sunlightAt(air, atRadius, prDVec3Dot(at, sun) / atRadius, sunlight);
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    double scattered = scattering(density, channel, air.haze) * weight[channel];
                    second[channel] += scattered * sunlight[channel] / (4 * kPi);
                    again[channel] += scattered;
                }
            }
            if (!grounded)
                continue;
            // The ground at the ray's end reflects the sunlight that it receives.
            PrDVec3 ground = point + direction * length;
            double groundCosine = prDVec3Dot(ground, sun) / kPlanetRadius;
            if (groundCosine <= 0)
                continue;
            double sunlight[3];
            sunlightAt(air, kPlanetRadius, groundCosine, sunlight);
            for (uint32_t channel = 0; channel < 3; ++channel)
                second[channel] += through[channel] * sunlight[channel] * groundCosine * air.albedo[channel] / kPi;
        }
    }
    double directions = kMultiCosines * kMultiTurns;
    for (uint32_t channel = 0; channel < 3; ++channel)
        out[channel] = second[channel] / directions / (1 - prF64Min(again[channel] / directions, 0.99));
}

void multiTableFill(const SkyAir& air, double sunCosine, MultiTable* table) {
    double angle = prF64Acos(prF64Max(-1, prF64Min(1, sunCosine)));
    table->lowCosine = prF64Cos(prF64Min(angle + kZenithTurn, kPi));
    table->highCosine = prF64Cos(prF64Max(angle - kZenithTurn, 0));
    for (uint32_t row = 0; row < kMultiRows; ++row) {
        for (uint32_t column = 0; column < kMultiColumns; ++column) {
            double cosine = table->lowCosine + (table->highCosine - table->lowCosine) * column / (kMultiColumns - 1);
            multiIntegrate(air, kPlanetRadius + multiRowHeight(row), cosine, table->entries[row][column]);
        }
    }
}

// Writes the table's light at a point, between its nearest entries.
void multiLookup(const MultiTable& table, double radius, double sunCosine, double* out) {
    double height = prF64Max(radius - kPlanetRadius, 0) / (kAtmosphereRadius - kPlanetRadius);
    double row = prF64Sqrt(prF64Min(height, 1)) * (kMultiRows - 1);
    double span = table.highCosine - table.lowCosine;
    double column = span > 0 ? prF64Max(0, prF64Min(1, (sunCosine - table.lowCosine) / span)) : 0;
    column *= kMultiColumns - 1;
    uint32_t row0 = uint32_t(row) < kMultiRows - 1 ? uint32_t(row) : kMultiRows - 2;
    uint32_t column0 = uint32_t(column) < kMultiColumns - 1 ? uint32_t(column) : kMultiColumns - 2;
    double fr = row - row0;
    double fc = column - column0;
    out[0] = out[1] = out[2] = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const double* corner = table.entries[row0 + (i >> 1)][column0 + (i & 1)];
        double weight = ((i >> 1) != 0 ? fr : 1 - fr) * ((i & 1) != 0 ? fc : 1 - fc);
        for (uint32_t channel = 0; channel < 3; ++channel)
            out[channel] += corner[channel] * weight;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The sky that a viewer sees

// The sky's integrals are tabulated over the view's height above the horizon and its turn away from the sun, and a
// texel reads them between the table's entries. The integrals vary slowly: the sharp peak around the sun is in the
// phase functions, which each texel computes for itself.
constexpr uint32_t kSkyHeights = 48;
constexpr uint32_t kSkyTurns = 24;

// The light that the atmosphere along a view ray sends toward the viewer, by channel, for unit irradiance above the
// atmosphere: the sunlight that the air and the haze scatter once, before each one's scattering coefficient and phase
// function, and the light that was scattered before, which leaves evenly.
struct SkyEntry {
    double air[3];
    double haze[3];
    double multi[3];
};

// Returns the viewer's distance from the planet's center. The viewer is on the axis through it, just above the ground.
double viewerRadius() {
    return kPlanetRadius + kViewerHeight;
}

// Integrates the light along a view ray that does not meet the ground. Each step adds the sunlight that reaches it and
// is scattered there, and the light that reaches it after other scatterings, dimmed by the air, the haze and the ozone
// between the step and the viewer. A step in the planet's shadow adds no sunlight.
SkyEntry skyIntegrate(const SkyAir& air, const MultiTable& multi, PrDVec3 view, PrDVec3 sun) {
    PrDVec3 origin{0, viewerRadius(), 0};
    double length = distanceToSpace(viewerRadius(), view.y);
    SkyEntry entry{};
    double through[3] = {1, 1, 1};
    for (uint32_t i = 0; i < kViewSteps; ++i) {
        Step step = stepOf(length, i, kViewSteps);
        PrDVec3 point = origin + view * step.at;
        double radius = prDVec3Length(point);
        double sunCosine = prDVec3Dot(point, sun) / radius;
        Depth density = densityAt(radius);
        double weight[3];
        double sunlight[3];
        double scattered[3];
        rayStep(air, density, step.length, through, weight);
        sunlightAt(air, radius, sunCosine, sunlight);
        multiLookup(multi, radius, sunCosine, scattered);
        for (uint32_t channel = 0; channel < 3; ++channel) {
            entry.air[channel] += density.air * sunlight[channel] * weight[channel];
            entry.haze[channel] += density.haze * sunlight[channel] * weight[channel];
            entry.multi[channel] += scattering(density, channel, air.haze) * scattered[channel] * weight[channel];
        }
    }
    return entry;
}

// The phase functions of the air (Rayleigh's) and of the haze (Cornette and Shanks's), at the cosine of the angle
// between the sunlight and the scattered light.
double airPhase(double cosine) {
    return 3 / (16 * kPi) * (1 + cosine * cosine);
}

double hazePhase(double cosine) {
    double g = kHazeAnisotropy;
    double base = 1 + g * g - 2 * g * cosine;
    return 3 / (8 * kPi) * (1 - g * g) * (1 + cosine * cosine) / ((2 + g * g) * base * prF64Sqrt(base));
}

// A sky that is ready to be read along any direction.
struct Sky {
    PrDVec3 sun;                // toward the sun, of unit length
    double irradiance;          // of the sun above the atmosphere
    SkyAir air;
    MultiTable multi;
    double ground[3];                         // the radiance of the ground
    SkyEntry entries[kSkyHeights][kSkyTurns]; // by the view's height, then its turn from the sun
};

// The view's height above the horizon at a row of the table, as the sine of its angle. The rows crowd toward the
// horizon, where the sky changes fastest.
double skyRowHeight(uint32_t row) {
    double t = double(row) / (kSkyHeights - 1);
    return t * t;
}

// Returns the radiance of a table entry for a view at the cosine `cosine` from the sun.
void entryRadiance(const Sky& sky, const SkyEntry& entry, double cosine, double* out) {
    double air = airPhase(cosine);
    double haze = hazePhase(cosine) * kHazeExtinction * kHazeAlbedo * sky.air.haze;
    for (uint32_t channel = 0; channel < 3; ++channel) {
        out[channel] = sky.irradiance *
            (kAirScattering[channel] * air * entry.air[channel] + haze * entry.haze[channel] + entry.multi[channel]);
    }
}

// Returns the part of the sun's light, by channel, that reaches the viewer through the atmosphere.
void sunTransmittance(PrDVec3 sun, double haze, double* out) {
    bool hidden = sun.y <= 0 || meetsPlanet(viewerRadius(), sun.y);
    Depth depth = depthToSpace(viewerRadius(), sun.y, kViewSteps);
    for (uint32_t channel = 0; channel < 3; ++channel)
        out[channel] = hidden ? 0 : prF64Exp(-extinction(depth, channel, haze));
}

PrDVec3 sunDirection(const PrRenderSkyDesc& desc) {
    PrDVec3 sun = prDVec3FromVec3(desc.sunDirection);
    double length = prDVec3Length(sun);
    if (!(length > 0))
        return {0, 1, 0};
    return sun / length;
}

// Fills a sky's tables, and computes its ground: a surface that reflects evenly what the sun and the sky above give it.
void skyPrepare(const PrRenderSkyDesc& desc, Sky* sky) {
    sky->sun = sunDirection(desc);
    sky->irradiance = double(prF32Max(desc.sunIrradiance, 0));
    sky->air.haze = double(prF32Max(desc.haze, 0));
    const float albedo[3] = {desc.groundAlbedo.x, desc.groundAlbedo.y, desc.groundAlbedo.z};
    for (uint32_t channel = 0; channel < 3; ++channel)
        sky->air.albedo[channel] = double(prF32Clamp(albedo[channel], 0, 1));
    depthTableFill(&sky->air.depths);
    multiTableFill(sky->air, sky->sun.y, &sky->multi);
    // The table's frame has the sun in the plane of x and y.
    PrDVec3 sun{prF64Sqrt(prF64Max(1 - sky->sun.y * sky->sun.y, 0)), sky->sun.y, 0};
    double fromSky[3] = {0, 0, 0};
    for (uint32_t row = 0; row < kSkyHeights; ++row) {
        double height = skyRowHeight(row);
        double flat = prF64Sqrt(prF64Max(1 - height * height, 0));
        // The solid angle that the entries of a row stand for, on both sides of the sun: the row's share of the heights
        // times a turn's share of the half circle.
        double below = row > 0 ? skyRowHeight(row - 1) : height;
        double above = row + 1 < kSkyHeights ? skyRowHeight(row + 1) : height;
        double band = (above - below) * 0.5;
        for (uint32_t turn = 0; turn < kSkyTurns; ++turn) {
            double angle = kPi * turn / (kSkyTurns - 1);
            PrDVec3 view{flat * prF64Cos(angle), height, flat * prF64Sin(angle)};
            sky->entries[row][turn] = skyIntegrate(sky->air, sky->multi, view, sun);
            double radiance[3];
            entryRadiance(*sky, sky->entries[row][turn], prDVec3Dot(view, sun), radiance);
            double turns = (turn == 0 || turn + 1 == kSkyTurns ? 0.5 : 1.0) * kPi / (kSkyTurns - 1);
            for (uint32_t channel = 0; channel < 3; ++channel)
                fromSky[channel] += radiance[channel] * height * band * turns * 2;
        }
    }
    double through[3];
    sunTransmittance(sky->sun, sky->air.haze, through);
    for (uint32_t channel = 0; channel < 3; ++channel) {
        double fromSun = sky->irradiance * through[channel] * prF64Max(sky->sun.y, 0);
        sky->ground[channel] = sky->air.albedo[channel] * (fromSun + fromSky[channel]) / kPi;
    }
}

// Returns the sky's radiance along a unit direction: the ground's below the horizon, and above it the table's integrals
// between their nearest entries, with the phase functions of the direction itself.
PrVec3 skyRadiance(const Sky& sky, PrDVec3 direction) {
    if (direction.y < 0)
        return {float(sky.ground[0]), float(sky.ground[1]), float(sky.ground[2])};
    double row = prF64Sqrt(direction.y) * (kSkyHeights - 1);
    double flat = prF64Sqrt((direction.x * direction.x + direction.z * direction.z) *
        (sky.sun.x * sky.sun.x + sky.sun.z * sky.sun.z));
    double turnCosine = flat > 1e-12 ? (direction.x * sky.sun.x + direction.z * sky.sun.z) / flat : 1;
    double turn = prF64Acos(prF64Max(-1, prF64Min(1, turnCosine))) / kPi * (kSkyTurns - 1);
    uint32_t row0 = uint32_t(row) < kSkyHeights - 1 ? uint32_t(row) : kSkyHeights - 2;
    uint32_t turn0 = uint32_t(turn) < kSkyTurns - 1 ? uint32_t(turn) : kSkyTurns - 2;
    double fr = row - row0;
    double ft = turn - turn0;
    SkyEntry entry{};
    for (uint32_t i = 0; i < 4; ++i) {
        const SkyEntry& corner = sky.entries[row0 + (i >> 1)][turn0 + (i & 1)];
        double weight = ((i >> 1) != 0 ? fr : 1 - fr) * ((i & 1) != 0 ? ft : 1 - ft);
        for (uint32_t channel = 0; channel < 3; ++channel) {
            entry.air[channel] += corner.air[channel] * weight;
            entry.haze[channel] += corner.haze[channel] * weight;
            entry.multi[channel] += corner.multi[channel] * weight;
        }
    }
    double radiance[3];
    entryRadiance(sky, entry, prDVec3Dot(direction, sky.sun), radiance);
    return {storedRadiance(float(radiance[0])), storedRadiance(float(radiance[1])), storedRadiance(float(radiance[2]))};
}

// Fills the first level of a cube of `size` with a sky.
void cubeFromSky(const PrRenderSkyDesc& desc, uint32_t size, float* cube) {
    Sky* sky = allocZeroed<Sky>();
    skyPrepare(desc, sky);
    for (uint32_t face = 0; face < kFaces; ++face) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                PrVec3 through = faceDirection(face, texelCenter(x, size), texelCenter(y, size));
                PrDVec3 d = prDVec3FromVec3(through);
                double length = prDVec3Length(d);
                PrVec3 radiance = skyRadiance(*sky, d / length);
                float* to = cube + ((size_t(face) * size + y) * size + x) * 3;
                to[0] = radiance.x;
                to[1] = radiance.y;
                to[2] = radiance.z;
            }
        }
    }
    prMemoryFree(sky);
}

// ---------------------------------------------------------------------------------------------------------------------
// Diffuse light

// The factors that turn the nine sums of radiance times a harmonic's term into the coefficients that the shaders read:
// each harmonic's normalization, squared since it weights the sum and the term alike, times what is left of its band
// once the radiance is gathered over a hemisphere by the cosine of the angle to the normal, over pi. Ramamoorthi and
// Hanrahan give the normalizations ("An Efficient Representation for Irradiance Environment Maps", 2001, equation 3)
// and the bands' factors, which are pi, 2 pi / 3 and pi / 4 before the division by pi (equations 8 and 9).
constexpr double kHarmonicFactors[kPrRenderEnvironmentHarmonics] = {
    0.282095 * 0.282095,
    0.488603 * 0.488603 * 2 / 3,
    0.488603 * 0.488603 * 2 / 3,
    0.488603 * 0.488603 * 2 / 3,
    1.092548 * 1.092548 / 4,
    1.092548 * 1.092548 / 4,
    0.315392 * 0.315392 / 4,
    1.092548 * 1.092548 / 4,
    0.546274 * 0.546274 / 4,
};

// Writes the terms of the harmonics at a unit direction, in the order of PrRenderFrameUniforms::environmentIrradiance.
void harmonicTerms(double x, double y, double z, double* terms) {
    terms[0] = 1;
    terms[1] = y;
    terms[2] = z;
    terms[3] = x;
    terms[4] = x * y;
    terms[5] = y * z;
    terms[6] = 3 * z * z - 1;
    terms[7] = x * z;
    terms[8] = x * x - y * y;
}

// Computes the diffuse light of the first level of a cube of `size`, as the uniforms hold it.
void cubeHarmonics(const float* cube, uint32_t size, PrRenderVec4* out) {
    // Blocks of texels are averaged if the faces divide into them evenly.
    uint32_t block = size > kHarmonicsSize && size % kHarmonicsSize == 0 ? size / kHarmonicsSize : 1;
    uint32_t coarse = size / block;
    double sums[kPrRenderEnvironmentHarmonics][3] = {};
    double solidAngle = 0;
    for (uint32_t face = 0; face < kFaces; ++face) {
        for (uint32_t y = 0; y < coarse; ++y) {
            for (uint32_t x = 0; x < coarse; ++x) {
                double radiance[3] = {0, 0, 0};
                for (uint32_t dy = 0; dy < block; ++dy) {
                    const float* row = cube + ((size_t(face) * size + y * block + dy) * size + x * block) * 3;
                    for (uint32_t i = 0; i < block * 3; ++i)
                        radiance[i % 3] += double(row[i]);
                }
                double u = double(texelCenter(x, coarse));
                double v = double(texelCenter(y, coarse));
                PrVec3 d = faceDirection(face, float(u), float(v));
                double length2 = 1 + u * u + v * v;
                double length = prF64Sqrt(length2);
                // A texel's solid angle falls off from the middle of its face with the cube of the distance to it.
                double weight = 1 / (length2 * length);
                double terms[kPrRenderEnvironmentHarmonics];
                harmonicTerms(double(d.x) / length, double(d.y) / length, double(d.z) / length, terms);
                for (uint32_t k = 0; k < kPrRenderEnvironmentHarmonics; ++k) {
                    for (uint32_t channel = 0; channel < 3; ++channel)
                        sums[k][channel] += radiance[channel] * terms[k] * weight;
                }
                solidAngle += weight;
            }
        }
    }
    // The weights are scaled so that they sum to the sphere, which makes an even environment's light exact.
    double scale = 4 * kPi / (solidAngle * block * block);
    for (uint32_t k = 0; k < kPrRenderEnvironmentHarmonics; ++k) {
        double factor = kHarmonicFactors[k] * scale;
        out[k] = {float(sums[k][0] * factor), float(sums[k][1] * factor), float(sums[k][2] * factor), 0};
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Cubes on the device

uint32_t levelSize(uint32_t size, uint32_t level) {
    uint32_t reduced = size >> level;
    return reduced != 0 ? reduced : 1;
}

// Returns the number of levels of a cube of `size` down to one texel.
uint32_t fullLevels(uint32_t size) {
    uint32_t levels = 1;
    while ((size >> levels) != 0)
        ++levels;
    return levels;
}

// Returns the number of levels of an environment's cube of `size`: down to kLastLevelSize, and at least one.
uint32_t filteredLevels(uint32_t size) {
    uint32_t levels = 1;
    while (levelSize(size, levels) >= kLastLevelSize && (size >> levels) != 0)
        ++levels;
    return levels;
}

PrStatus createCube(Render* r, uint32_t size, uint32_t levels, PrStringView name, PrGpuTexture* out) {
    PrGpuTextureDesc desc{
        .kind = PrGpuTextureKind_Cube,
        .format = kCubeFormat,
        .usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget,
        .levels = uint8_t(levels),
        .samples = 1,
        .extent = {size, size, kFaces},
        .name = name,
    };
    return prGpuTextureCreate(r->device, &desc, out);
}

// Uploads the first level of a cube from texels of red, green and blue floats.
void uploadCube(Render* r, PrGpuTexture cube, const float* texels, uint32_t size) {
    size_t count = size_t(kFaces) * size * size;
    PrFloat16* halves = static_cast<PrFloat16*>(prMemoryAlloc(count * 4 * sizeof(PrFloat16)));
    PrFloat16 one = prFloat16FromFloat(1);
    for (size_t i = 0; i < count; ++i) {
        for (uint32_t channel = 0; channel < 3; ++channel)
            halves[i * 4 + channel] = prFloat16FromFloat(texels[i * 3 + channel]);
        halves[i * 4 + 3] = one;
    }
    PrGpuTextureRegion region{.extent = {size, size, kFaces}};
    uint32_t rowBytes = size * 4 * uint32_t(sizeof(PrFloat16));
    prGpuTextureUpload(r->device, cube, &region, halves, rowBytes, rowBytes * size);
    prMemoryFree(halves);
}

// Records a draw that covers one face of one level of `target`, with `source` and the environments' sampler for the
// shader to sample.
void drawFace(Render* r, PrGpuCommands* commands, PrGpuPipeline pipeline, PrGpuTexture target, uint32_t level,
        PrGpuTexture source, PrRenderEnvironmentPush push) {
    PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Discard, PrColor{});
    desc.color[0].level = uint8_t(level);
    desc.color[0].layer = uint16_t(push.face);
    desc.name = "render environment";
    push.source = source.id;
    push.sampler = r->environmentSampler.id;
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    if (!r->bindless)
        prGpuCommandsSetTexture(commands, r->bindings[RenderLibrary_EnvironmentFilter].source, source,
            r->environmentSampler);
    prGpuPassDraw(pass, 3, 1, 0, 0);
    prGpuPassEnd(pass);
}

// Records the work that fills the levels of an environment's cube. `work` is a cube whose first level holds the
// environment's radiance, and `cube` is the environment's own.
//
// Each level of `work` after the first is the mean of the level before it. The first level of `cube` is a copy of that
// of `work`, and each level after it is `work` filtered for the level's roughness. The filter reads the levels of
// `work` whose texels match the solid angle of each of its samples, which is why `work` has them.
void recordFilter(Render* r, PrGpuCommands* commands, PrGpuTexture work, PrGpuTexture cube, uint32_t size, uint32_t levels) {
    uint32_t workLevels = fullLevels(size);
    PrGpuAccessFlags read = PrGpuAccessFlag_ShaderRead;
    PrGpuAccessFlags undefined = PrGpuAccessFlag_ShaderRead | PrGpuAccessFlag_Discard;
    PrGpuSubresources first{.firstLevel = 0, .levels = 1};
    PrGpuSubresources workRest{.firstLevel = 1};
    PrGpuSubresources cubeRest{.firstLevel = 1};

    if (workLevels > 1)
        prGpuCommandsTextureBarrier(commands, work, workRest, undefined, PrGpuAccessFlag_ColorAttachment);
    for (uint32_t level = 1; level < workLevels; ++level) {
        // A view of the level before alone, since the cube's other levels are being drawn to.
        PrGpuTexture before{};
        PrGpuSubresources one{.firstLevel = uint8_t(level - 1), .levels = 1};
        if (prGpuTextureCreateView(r->device, work, PrGpuTextureKind_Cube, one, &before) != PrStatus_Ok)
            continue;
        for (uint32_t face = 0; face < kFaces; ++face) {
            PrRenderEnvironmentPush push{.face = face, .targetSize = float(levelSize(size, level))};
            drawFace(r, commands, r->environmentReduce, work, level, before, push);
        }
        PrGpuSubresources drawn{.firstLevel = uint8_t(level), .levels = 1};
        prGpuCommandsTextureBarrier(commands, work, drawn, PrGpuAccessFlag_ColorAttachment, read);
        prGpuTextureRelease(r->device, before);
    }

    prGpuCommandsTextureBarrier(commands, work, first, read, PrGpuAccessFlag_CopyRead);
    prGpuCommandsTextureBarrier(commands, cube, first, undefined, PrGpuAccessFlag_CopyWrite);
    PrGpuTextureRegion faces{.extent = {size, size, kFaces}};
    prGpuCommandsCopyTexture(commands, work, &faces, cube, &faces);
    prGpuCommandsTextureBarrier(commands, work, first, PrGpuAccessFlag_CopyRead, read);
    prGpuCommandsTextureBarrier(commands, cube, first, PrGpuAccessFlag_CopyWrite, read);

    if (levels > 1)
        prGpuCommandsTextureBarrier(commands, cube, cubeRest, undefined, PrGpuAccessFlag_ColorAttachment);
    for (uint32_t level = 1; level < levels; ++level) {
        for (uint32_t face = 0; face < kFaces; ++face) {
            PrRenderEnvironmentPush push{
                .face = face,
                .targetSize = float(levelSize(size, level)),
                .roughness = float(level) / float(levels - 1),
                .sourceSize = float(size),
                .sampleCount = kFilterSamples,
            };
            drawFace(r, commands, r->environmentSpecular, cube, level, work, push);
        }
    }
    if (levels > 1)
        prGpuCommandsTextureBarrier(commands, cube, cubeRest, PrGpuAccessFlag_ColorAttachment, read);
    if (!r->bindless)
        prGpuCommandsSetTexture(commands, r->bindings[RenderLibrary_EnvironmentFilter].source, PrGpuTexture{},
            PrGpuSampler{});
}

// The sun's disc of an environment, as RenderEnvironmentState holds it.
struct SunDisc {
    PrRenderVec4 sun;
    PrRenderVec4 radiance;
};

// Returns the disc of a sky's sun. Its mean radiance is the irradiance that the sun gives the ground, over the solid
// angle of the disc, so that the disc and the directional light of prRenderSkySunIrradiance are one light.
SunDisc sunDisc(const PrRenderSkyDesc& sky) {
    float sine = prF32Sin(prF32Clamp(sky.sunAngularRadius, 0, 1));
    if (!(sine > 0))
        return SunDisc{};
    PrDVec3 sun = sunDirection(sky);
    PrVec3 irradiance = prRenderSkySunIrradiance(&sky);
    float solidAngle = float(kPi) * sine * sine;
    return SunDisc{
        .sun = {float(sun.x), float(sun.y), float(sun.z), sine},
        .radiance = {irradiance.x / solidAngle, irradiance.y / solidAngle, irradiance.z / solidAngle, 0},
    };
}

// Makes an environment's cube from the first level in `texels`, which the call frees, and queues it with its diffuse
// light and its sun's disc for the next frame.
PrStatus setCube(Render* r, PrRenderEnvironment environment, float* texels, uint32_t size, SunDisc disc) {
    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    PrStatus status = PrStatus_Ok;
    if (prGpuDeviceIsLost(r->device))
        status = PrStatus_Lost;
    else if (!prGpuDeviceSupportsFormat(r->device, kCubeFormat, usage))
        status = PrStatus_NotSupported;
    uint32_t levels = filteredLevels(size);
    PrGpuTexture work{};
    PrGpuTexture cube{};
    if (status == PrStatus_Ok)
        status = createCube(r, size, fullLevels(size), "render environment work", &work);
    if (status == PrStatus_Ok)
        status = createCube(r, size, levels, "render environment", &cube);
    PrGpuCommands* commands = nullptr;
    if (status == PrStatus_Ok && !prGpuCommandsBegin(r->device, nullptr, &commands))
        status = PrStatus_Lost;
    if (status != PrStatus_Ok) {
        prGpuTextureRelease(r->device, work);
        prGpuTextureRelease(r->device, cube);
        prMemoryFree(texels);
        return status;
    }
    uploadCube(r, work, texels, size);
    recordFilter(r, commands, work, cube, size, levels);
    prGpuCommandsSubmit(commands);
    prGpuTextureRelease(r->device, work);

    RenderOp op{.kind = RenderOpKind_EnvironmentData, .id = environment.id};
    op.environment = allocZeroed<RenderEnvironmentState>();
    op.environment->ready = true;
    op.environment->cube = cube;
    op.environment->faceSize = size;
    op.environment->levels = levels;
    op.environment->sun = disc.sun;
    op.environment->sunRadiance = disc.radiance;
    cubeHarmonics(texels, size, op.environment->irradiance);
    prMemoryFree(texels);
    renderOpQueue(r, op);
    return PrStatus_Ok;
}

const RenderEnvironmentState* PR_NULLABLE readyEnvironment(const Render* r, uint32_t id) {
    if (id == 0 || id >= r->environments.count || !r->environments.data[id].ready)
        return nullptr;
    return &r->environments.data[id];
}

PrGpuGraphicsPipelineDesc filterPipeline(const Render* r, PrStringView fragment) {
    PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(r->libraries[RenderLibrary_EnvironmentFilter]);
    desc.vertex = "filterVertex";
    desc.fragment = fragment;
    desc.formats.colorCount = 1;
    desc.formats.color[0] = kCubeFormat;
    desc.name = "render environment";
    return desc;
}

} // namespace

PrStatus renderEnvironmentObjectsCreate(Render* render) {
    Render* r = render;
    PrGpuTextureDesc black{
        .kind = PrGpuTextureKind_Cube,
        .format = PrGpuFormat_Rgba8,
        .usage = PrGpuTextureUsageFlag_Sampled,
        .levels = 1,
        .samples = 1,
        .extent = {1, 1, kFaces},
        .name = "render black cube",
    };
    PrStatus status = prGpuTextureCreate(r->device, &black, &r->blackCube);
    if (status != PrStatus_Ok)
        return status;
    const uint8_t texels[kFaces * 4] = {};
    PrGpuTextureRegion region{.extent = {1, 1, kFaces}};
    prGpuTextureUpload(r->device, r->blackCube, &region, texels, 4, 4);
    PrGpuSamplerDesc sampler = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Clamp);
    status = prGpuSamplerIntern(r->device, &sampler, &r->environmentSampler);
    if (status != PrStatus_Ok)
        return status;
    // A device that does not render to half floats makes no environment, and needs no pipeline to fill one.
    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    if (!prGpuDeviceSupportsFormat(r->device, kCubeFormat, usage))
        return PrStatus_Ok;
    PrGpuGraphicsPipelineDesc reduce = filterPipeline(r, "reducePixel");
    status = prGpuPipelineCreateGraphics(r->device, &reduce, &r->environmentReduce);
    if (status != PrStatus_Ok)
        return status;
    PrGpuGraphicsPipelineDesc specular = filterPipeline(r, "specularPixel");
    return prGpuPipelineCreateGraphics(r->device, &specular, &r->environmentSpecular);
}

void renderEnvironmentObjectsDestroy(Render* render) {
    prGpuPipelineRelease(render->device, render->environmentReduce);
    prGpuPipelineRelease(render->device, render->environmentSpecular);
    prGpuTextureRelease(render->device, render->blackCube);
}

void renderEnvironmentUniforms(const Render* render, const PrRenderSceneEnvironment& environment,
        PrRenderFrameUniforms* uniforms) {
    PrRenderFrameUniforms& u = *uniforms;
    // The uniforms turn the world's directions into the environment's, which is the turn's transpose.
    const float (*m)[4] = environment.rotation.m;
    for (uint32_t row = 0; row < 3; ++row)
        u.environmentRotation[row] = {m[0][row], m[1][row], m[2][row], 0};
    u.environment = render->blackCube.id;
    u.environmentSampler = render->environmentSampler.id;
    u.environmentIntensity = environment.intensity;
    u.environmentLastLevel = 0;
    const RenderEnvironmentState* state = readyEnvironment(render, environment.environment.id);
    for (uint32_t k = 0; k < kPrRenderEnvironmentHarmonics; ++k) {
        PrRenderVec4 c{};
        if (state != nullptr)
            c = state->irradiance[k];
        u.environmentIrradiance[k] =
            {c.x * environment.intensity, c.y * environment.intensity, c.z * environment.intensity, 0};
    }
    u.environmentSun = {};
    u.environmentSunRadiance = {};
    if (state == nullptr)
        return;
    u.environmentSun = state->sun;
    u.environmentSunRadiance = {
        state->sunRadiance.x * environment.intensity,
        state->sunRadiance.y * environment.intensity,
        state->sunRadiance.z * environment.intensity,
        0,
    };
    u.environment = state->cube.id;
    u.environmentLastLevel = float(state->levels - 1);
}

} // namespace pr_impl

using namespace pr_impl;

PrRenderSkyDesc prRenderSkyDescDefault(void) {
    return PrRenderSkyDesc{
        .sunDirection = {0.5f, 0.70710678f, 0.5f},
        .sunIrradiance = kPrRenderSunIlluminance,
        .haze = 1,
        .groundAlbedo = {0.2f, 0.2f, 0.2f},
        .sunAngularRadius = kPrRenderSunAngularRadius,
    };
}

PrVec3 prRenderSkySunIrradiance(const PrRenderSkyDesc* sky) {
    double through[3];
    sunTransmittance(sunDirection(*sky), double(prF32Max(sky->haze, 0)), through);
    float irradiance = prF32Max(sky->sunIrradiance, 0);
    return PrVec3{float(through[0]) * irradiance, float(through[1]) * irradiance, float(through[2]) * irradiance};
}

PrRenderEnvironment prRenderEnvironmentCreate(PrRender* render) {
    return PrRenderEnvironment{renderIdTake(render, &render->environmentIds)};
}

PrStatus prRenderEnvironmentSetEquirect(PrRender* render, PrRenderEnvironment environment, const PrRenderRadianceImage* image) {
    PR_ASSERT(environment.id != 0 && (image->channels == 3 || image->channels == 4));
    PR_ASSERT(image->pixels.extent.x != 0 && image->pixels.extent.y != 0);
    uint32_t size = render->options.environmentSize;
    float* texels = allocCube(size);
    cubeFromEquirect(*image, size, texels);
    return setCube(render, environment, texels, size, SunDisc{});
}

PrStatus prRenderEnvironmentSetCube(PrRender* render, PrRenderEnvironment environment, const PrRenderRadianceImage* faces) {
    uint32_t size = faces[0].pixels.extent.x;
    PR_ASSERT(environment.id != 0 && size != 0 && size <= kPrRenderEnvironmentMaxSize);
    float* texels = allocCube(size);
    for (uint32_t face = 0; face < kFaces; ++face) {
        const PrRenderRadianceImage& image = faces[face];
        PR_ASSERT(image.pixels.extent.x == size && image.pixels.extent.y == size);
        PR_ASSERT(image.channels == 3 || image.channels == 4);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x)
                readPixel(image, x, y, texels + ((size_t(face) * size + y) * size + x) * 3);
        }
    }
    return setCube(render, environment, texels, size, SunDisc{});
}

PrStatus prRenderEnvironmentSetSky(PrRender* render, PrRenderEnvironment environment, const PrRenderSkyDesc* sky) {
    PR_ASSERT(environment.id != 0);
    uint32_t size = render->options.environmentSize;
    float* texels = allocCube(size);
    cubeFromSky(*sky, size, texels);
    return setCube(render, environment, texels, size, sunDisc(*sky));
}

void prRenderEnvironmentDestroy(PrRender* render, PrRenderEnvironment environment) {
    if (environment.id != 0)
        renderOpQueue(render, RenderOp{.kind = RenderOpKind_EnvironmentDestroy, .id = environment.id});
}

bool prRenderEnvironmentIsReady(const PrRender* render, PrRenderEnvironment environment) {
    return readyEnvironment(render, environment.id) != nullptr;
}

PrRenderEnvironmentInfo prRenderEnvironmentInfo(const PrRender* render, PrRenderEnvironment environment) {
    const RenderEnvironmentState* state = readyEnvironment(render, environment.id);
    if (state == nullptr)
        return PrRenderEnvironmentInfo{};
    return PrRenderEnvironmentInfo{.texture = state->cube, .faceSize = state->faceSize, .levels = state->levels};
}

PrVec3 prRenderEnvironmentIrradiance(const PrRender* render, PrRenderEnvironment environment, PrVec3 normal) {
    const RenderEnvironmentState* state = readyEnvironment(render, environment.id);
    if (state == nullptr)
        return PrVec3{};
    double terms[kPrRenderEnvironmentHarmonics];
    harmonicTerms(double(normal.x), double(normal.y), double(normal.z), terms);
    float sum[3] = {0, 0, 0};
    for (uint32_t k = 0; k < kPrRenderEnvironmentHarmonics; ++k) {
        sum[0] += state->irradiance[k].x * float(terms[k]);
        sum[1] += state->irradiance[k].y * float(terms[k]);
        sum[2] += state->irradiance[k].z * float(terms[k]);
    }
    return PrVec3{prF32Max(sum[0], 0), prF32Max(sum[1], 0), prF32Max(sum[2], 0)};
}
