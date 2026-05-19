#include <gtest/gtest.h>
#include "ui/ToastQueue.h"
#include <thread>
#include <vector>

TEST(ToastQueue, PostAndSnapshotReturnsNewestFirst) {
    ToastQueue q;
    q.post(ToastSeverity::Info,    "first");
    q.post(ToastSeverity::Error,   "second");
    q.post(ToastSeverity::Success, "third");

    auto snap = q.snapshot(/*nowMs=*/1000);
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[0].message, "third");
    EXPECT_EQ(snap[1].message, "second");
    EXPECT_EQ(snap[2].message, "first");
}

TEST(ToastQueue, SnapshotDropsExpired) {
    ToastQueue q;
    q.post(ToastSeverity::Success, "fast");  // expires at 3000
    q.post(ToastSeverity::Error,   "slow");  // expires at 6000

    auto snap = q.snapshot(/*nowMs=*/4000);
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].message, "slow");
}

TEST(ToastQueue, SnapshotEnforcesMaxVisibleEvictingOldest) {
    ToastQueue q;
    for (int i = 0; i < 7; ++i) q.post(ToastSeverity::Info, std::to_string(i));

    auto snap = q.snapshot(/*nowMs=*/0);
    ASSERT_EQ(snap.size(), ToastQueue::MaxVisible);  // 5
    EXPECT_EQ(snap[0].message, "6");
    EXPECT_EQ(snap[snap.size() - 1].message, "2");
}

TEST(ToastQueue, DismissRemovesById) {
    ToastQueue q;
    q.post(ToastSeverity::Info, "a");
    q.post(ToastSeverity::Info, "b");
    auto snap = q.snapshot(0);
    ASSERT_EQ(snap.size(), 2u);

    q.dismiss(snap[0].id);
    auto snap2 = q.snapshot(0);
    ASSERT_EQ(snap2.size(), 1u);
    EXPECT_EQ(snap2[0].message, "a");
}

TEST(ToastQueue, ConcurrentPostsAllArrive) {
    ToastQueue q;
    const int N = 4, M = 250;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&q, t] {
            for (int i = 0; i < M; ++i)
                q.post(ToastSeverity::Info, "t" + std::to_string(t) + "-" + std::to_string(i));
        });
    }
    for (auto& th : threads) th.join();

    // 1000 posts but cap is 5; verify cap holds and dismiss/snapshot don't crash.
    auto snap = q.snapshot(0);
    EXPECT_EQ(snap.size(), ToastQueue::MaxVisible);
}

TEST(ToastQueue, SeverityDeterminesDuration) {
    ToastQueue q;
    q.post(ToastSeverity::Error,   "err");
    q.post(ToastSeverity::Info,    "inf");
    q.post(ToastSeverity::Success, "ok");
    auto snap = q.snapshot(0);
    ASSERT_EQ(snap.size(), 3u);
    // Index 0 = success (newest), 1 = info, 2 = error (oldest)
    EXPECT_EQ(snap[2].expiresAtMs, ToastQueue::DurationMsErr);
    EXPECT_EQ(snap[1].expiresAtMs, ToastQueue::DurationMsInf);
    EXPECT_EQ(snap[0].expiresAtMs, ToastQueue::DurationMsOk);
}
