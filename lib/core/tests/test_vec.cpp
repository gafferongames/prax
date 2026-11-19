#include "pr/vec.h"

#include "tools/harness/test.h"

TEST_CASE("the dot product of vectors in space sums the products of their coordinates") {
    CHECK(prVec3Dot(PrVec3{1, 2, 3}, PrVec3{4, -5, 6}) == 12);
    CHECK(prVec3Dot(PrVec3{1, 0, 0}, PrVec3{0, 1, 0}) == 0);
}

TEST_CASE("the cross product is perpendicular to both vectors, and right-handed") {
    CHECK(prVec3Cross(PrVec3{1, 0, 0}, PrVec3{0, 1, 0}) == PrVec3{0, 0, 1});
    CHECK(prVec3Cross(PrVec3{0, 1, 0}, PrVec3{0, 0, 1}) == PrVec3{1, 0, 0});
    CHECK(prVec3Cross(PrVec3{0, 1, 0}, PrVec3{1, 0, 0}) == PrVec3{0, 0, -1});
    PrVec3 a{1, 2, 3};
    PrVec3 b{-4, 5, 0.5f};
    PrVec3 c = prVec3Cross(a, b);
    CHECK(c == PrVec3{-14, -12.5f, 13});
    CHECK(prVec3Dot(c, a) == 0);
    CHECK(prVec3Dot(c, b) == 0);
    CHECK(prVec3Cross(a, a) == PrVec3{0, 0, 0});
}

TEST_CASE("a vector in space has a length, and normalizes to a length of 1 unless it has none") {
    CHECK(prVec3Length(PrVec3{2, 3, 6}) == 7);
    CHECK(prVec3Length(PrVec3{0, 0, 0}) == 0);
    CHECK(prVec3Normalize(PrVec3{0, 0, -4}) == PrVec3{0, 0, -1});
    CHECK(prVec3Normalize(PrVec3{2, 3, 6}) == PrVec3{2 / 7.0f, 3 / 7.0f, 6 / 7.0f});
    CHECK(prVec3Normalize(PrVec3{0, 0, 0}) == PrVec3{0, 0, 0});
}

TEST_CASE("a vector of doubles widens a vector of floats, and has a dot product, cross product and length") {
    CHECK(prDVec3FromVec3(PrVec3{1, -2, 0.5f}) == PrDVec3{1, -2, 0.5});
    CHECK(prDVec3Dot(PrDVec3{1, 2, 3}, PrDVec3{4, -5, 6}) == 12);
    CHECK(prDVec3Cross(PrDVec3{1, 0, 0}, PrDVec3{0, 1, 0}) == PrDVec3{0, 0, 1});
    CHECK(prDVec3Cross(PrDVec3{0, 1, 0}, PrDVec3{1, 0, 0}) == PrDVec3{0, 0, -1});
    CHECK(prDVec3Cross(PrDVec3{1, 2, 3}, PrDVec3{-4, 5, 0.5}) == PrDVec3{-14, -12.5, 13});
    CHECK(prDVec3Length(PrDVec3{2, 3, 6}) == 7);
    CHECK(prDVec3Length(PrDVec3{0, 0, 0}) == 0);
    CHECK(-PrDVec3{1, 2, 3} == PrDVec3{-1, -2, -3});
    CHECK(PrDVec3{1, 2, 3} * 2.0 == PrDVec3{2, 4, 6});
}
