#include "core/guidance_scan.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace ambient::guidance {
namespace {

// Four unit vectors in three dimensions
const std::vector<float> kMatrix = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0.6f, 0.8f, 0};

TEST(Scan, ReturnsTheNearestRowsInOrder) {
    const float query[] = {0.6f, 0.8f, 0};
    const auto hits = Scan(kMatrix.data(), 4, 3, query, 3);
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].ord, 3u);
    EXPECT_NEAR(hits[0].cosine, 1.0f, 1e-6f);
    EXPECT_EQ(hits[1].ord, 1u);
    EXPECT_NEAR(hits[1].cosine, 0.8f, 1e-6f);
    EXPECT_EQ(hits[2].ord, 0u);
    EXPECT_NEAR(hits[2].cosine, 0.6f, 1e-6f);
}

TEST(Scan, KIsClampedAndTiesBreakOnOrdinal) {
    const float query[] = {0, 0, 1};
    EXPECT_EQ(Scan(kMatrix.data(), 4, 3, query, 10).size(), 4u);
    EXPECT_TRUE(Scan(kMatrix.data(), 4, 3, query, 0).empty());
    const auto hits = Scan(kMatrix.data(), 4, 3, query, 4);
    EXPECT_EQ(hits[0].ord, 2u);
    EXPECT_EQ(hits[1].ord, 0u) << "rows 0, 1 and 3 all score 0; lowest ordinal first";
    EXPECT_EQ(hits[2].ord, 1u);
    EXPECT_EQ(hits[3].ord, 3u);
}

TEST(Scan, AnEmptyMatrixYieldsNothing) {
    const float query[] = {1, 0, 0};
    EXPECT_TRUE(Scan(kMatrix.data(), 0, 3, query, 5).empty());
}

}  // namespace
}  // namespace ambient::guidance
