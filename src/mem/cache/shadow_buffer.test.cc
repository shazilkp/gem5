/*
 * Unit tests for the SpaceSpec shadow buffer (Layer 0).
 *
 * Every test here corresponds to a way the previous implementation actually
 * failed.  They are grouped by which of those failures they pin down, and the
 * comments say which, because the failure modes are the valuable part -- a
 * reader who knows why "OwnerSurvivesSquashedOwner" exists will understand
 * the design far better than one reading the design alone.
 *
 * No simulator is involved: this exercises ownership and lifetime only.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "mem/cache/shadow_buffer.hh"

using namespace gem5;

namespace
{

constexpr std::size_t kLine = 64;
constexpr std::uint64_t kLineMask = ~std::uint64_t(63);

/** A line filled with a recognisable byte, so identity is checkable. */
std::vector<std::uint8_t>
makeLine(std::uint8_t fill)
{
    return std::vector<std::uint8_t>(kLine, fill);
}

std::uint64_t
lineAddr(std::uint64_t n)
{
    return (n * kLine) & kLineMask;
}

} // namespace

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

TEST(ShadowBufferTest, StartsEmpty)
{
    ShadowBuffer buf(4, kLine);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.capacity(), 4u);
    EXPECT_EQ(buf.lineSize(), kLine);
    EXPECT_EQ(buf.oldestOwner(), std::nullopt);
    EXPECT_TRUE(buf.checkInvariants());
}

TEST(ShadowBufferTest, CaptureThenLookupReturnsSameData)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0xAB);

    EXPECT_EQ(buf.capture(1, lineAddr(0), data.data()),
              CaptureResult::Accepted);
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_TRUE(buf.holds(lineAddr(0)));

    std::vector<std::uint8_t> out(kLine, 0);
    EXPECT_TRUE(buf.lookup(lineAddr(0), 0, out.data(), kLine));
    EXPECT_EQ(out, data);
}

TEST(ShadowBufferTest, LookupMissReturnsNothing)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x11);
    buf.capture(1, lineAddr(0), data.data());

    std::vector<std::uint8_t> out(kLine, 0);
    EXPECT_FALSE(buf.lookup(lineAddr(9), 0, out.data(), kLine));
}

TEST(ShadowBufferTest, LookupHonoursOffsetAndLength)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0);
    for (std::size_t i = 0; i < kLine; i++)
        data[i] = static_cast<std::uint8_t>(i);

    buf.capture(1, lineAddr(0), data.data());

    // A partial read at a non-zero offset must land on the right bytes.
    std::vector<std::uint8_t> out(8, 0);
    EXPECT_TRUE(buf.lookup(lineAddr(0), 8, out.data(), 8));
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(out[i], static_cast<std::uint8_t>(i + 8)) << "at " << i;

    // Reading past the end of the line must be refused, not truncated.
    EXPECT_FALSE(buf.lookup(lineAddr(0), 60, out.data(), 8));
}

// ---------------------------------------------------------------------------
// Ownership: the first implementation's livelock
// ---------------------------------------------------------------------------

// A second load touching the same line must NOT become the owner.  If it
// did, and it were then squashed, the entry would be stranded: nothing would
// ever release it, the line would never reach the real cache, and every later
// access to it would re-capture and re-miss, forever.  This is precisely the
// livelock that killed the first implementation.
TEST(ShadowBufferTest, RefreshKeepsOriginalOwner)
{
    ShadowBuffer buf(4, kLine);
    auto first = makeLine(0x01);
    auto second = makeLine(0x02);

    EXPECT_EQ(buf.capture(10, lineAddr(0), first.data()),
              CaptureResult::Accepted);
    EXPECT_EQ(buf.capture(20, lineAddr(0), second.data()),
              CaptureResult::Accepted);

    // One entry, not two.
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf.stats().captures, 2u);
    EXPECT_EQ(buf.stats().refreshes, 1u);

    // The owner is still the *oldest* load that captured the line.
    EXPECT_EQ(buf.oldestOwner().value(), 10u);

    // Contents reflect the most recent capture.
    std::vector<std::uint8_t> out(kLine, 0);
    EXPECT_TRUE(buf.lookup(lineAddr(0), 0, out.data(), kLine));
    EXPECT_EQ(out, second);

    EXPECT_TRUE(buf.checkInvariants());
}

// The original defect, stated as a regression test.  With the newest load as
// owner, retiring through seqNum 20 removes nothing (20 is not < 20), the
// entry survives, and occupancy never returns to zero.
TEST(ShadowBufferTest, RetirementReleasesRegardlessOfOwnerOutcome)
{
    ShadowBuffer buf(4, kLine);
    auto first = makeLine(0x01);
    auto second = makeLine(0x02);

    buf.capture(10, lineAddr(0), first.data());
    buf.capture(20, lineAddr(0), second.data());

    // Owner is 10.  Once load 10 has retired -- committed *or* squashed --
    // the entry must go, whether or not load 20 ever retires.
    EXPECT_EQ(buf.retireThrough(11), 1u);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_TRUE(buf.empty());
}

// Directly models the livelock scenario: repeated access to one line across
// many retirement boundaries must not accumulate entries, and the buffer must
// end up empty once everything has retired.
TEST(ShadowBufferTest, RepeatedAccessToOneLineDoesNotAccumulate)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x33);

    std::uint64_t seq = 1;
    for (int round = 0; round < 100; round++) {
        // Two loads touch the line, then both retire.
        buf.capture(seq, lineAddr(0), data.data());
        buf.capture(seq + 1, lineAddr(0), data.data());
        EXPECT_LE(buf.size(), 1u) << "round " << round;
        buf.retireThrough(seq + 2);
        EXPECT_EQ(buf.size(), 0u)
            << "round " << round << ": entry stranded across retirement";
        seq += 2;
    }
    EXPECT_TRUE(buf.checkInvariants());
}

// Retire must stop at the first live entry, because the queue is ordered.
TEST(ShadowBufferTest, RetireStopsAtFirstLiveEntry)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x44);

    buf.capture(10, lineAddr(0), data.data());
    buf.capture(20, lineAddr(1), data.data());
    buf.capture(30, lineAddr(2), data.data());
    EXPECT_EQ(buf.size(), 3u);

    // Watermark past the first entry only.
    EXPECT_EQ(buf.retireThrough(15), 1u);
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf.oldestOwner().value(), 20u);

    EXPECT_EQ(buf.retireThrough(25), 1u);
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf.oldestOwner().value(), 30u);

    EXPECT_EQ(buf.retireThrough(35), 1u);
    EXPECT_TRUE(buf.empty());

    // Releasing from empty is a no-op, not an error.
    EXPECT_EQ(buf.retireThrough(100), 0u);
    EXPECT_TRUE(buf.checkInvariants());
}

// Retirement must release in owner order, so promotion always takes from the
// front and the structure stays sorted.
TEST(ShadowBufferTest, RetirementPreservesOrdering)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x55);

    // Captured out of order; the buffer is responsible for staying sorted.
    buf.capture(30, lineAddr(2), data.data());
    buf.capture(10, lineAddr(0), data.data());
    buf.capture(20, lineAddr(1), data.data());
    EXPECT_TRUE(buf.checkInvariants()) << "out-of-order capture broke ordering";
    EXPECT_EQ(buf.oldestOwner().value(), 10u);
    EXPECT_EQ(buf.newestOwner().value(), 30u);

    EXPECT_EQ(buf.retireThrough(25), 2u);
    EXPECT_TRUE(buf.checkInvariants());
    EXPECT_EQ(buf.oldestOwner().value(), 30u);
}

// The watermark is an exclusive bound: an entry owned by a load that is NOT
// yet retired must survive, even when that load's sequence number equals the
// watermark.  Using <= here would release a still-speculative line early,
// which is a security regression rather than a correctness one -- so this
// pins the boundary down from both sides.
TEST(ShadowBufferTest, WatermarkIsExclusive)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x6E);

    buf.capture(100, lineAddr(0), data.data());

    // A watermark below the owner leaves it alone.
    EXPECT_EQ(buf.retireThrough(99), 0u);
    EXPECT_TRUE(buf.holds(lineAddr(0)));

    // A watermark equal to the owner must also leave it alone: that load has
    // not retired yet, only everything before it has.
    EXPECT_EQ(buf.retireThrough(100), 0u)
        << "watermark is exclusive; an owner equal to it is still live";
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_TRUE(buf.holds(lineAddr(0)));

    // One past the owner releases it.
    EXPECT_EQ(buf.retireThrough(101), 1u);
    EXPECT_FALSE(buf.holds(lineAddr(0)));
}

// Retirement must never release an entry whose owner is still speculative.
// This is the safety property behind the exclusive watermark: an early
// release would put a possibly-wrong line into the real cache.
TEST(ShadowBufferTest, NeverReleasesLiveOwner)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x6F);

    buf.capture(7, lineAddr(0), data.data());
    buf.capture(9, lineAddr(1), data.data());

    // A watermark of 8 retires the load at 7 but not the load at 9.
    EXPECT_EQ(buf.retireThrough(8), 1u);
    EXPECT_FALSE(buf.holds(lineAddr(0)));
    EXPECT_TRUE(buf.holds(lineAddr(1)))
        << "the load at seq 9 has not retired; its line must stay shadowed";

    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf.oldestOwner().value(), 9u);
}

// ---------------------------------------------------------------------------
// Squash: discardFrom(), the exact complement of retireThrough()
// ---------------------------------------------------------------------------

// A squash must throw away the squashed loads' lines and keep those of loads
// that survive.  Getting this backwards is silent and expensive: the lines
// belonging to surviving loads vanish, so they are never promoted, are never
// in L1, and are re-fetched by every later access.  Nothing crashes; the
// cache just quietly stops working.
TEST(ShadowBufferTest, DiscardKeepsSurvivors)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x6A);

    buf.capture(10, lineAddr(0), data.data());   // survives a squash at 30
    buf.capture(20, lineAddr(1), data.data());   // survives
    buf.capture(30, lineAddr(2), data.data());   // squashed
    buf.capture(40, lineAddr(3), data.data());   // squashed
    EXPECT_EQ(buf.size(), 4u);

    EXPECT_EQ(buf.discardFrom(30), 2u);
    EXPECT_EQ(buf.size(), 2u);

    EXPECT_TRUE(buf.holds(lineAddr(0))) << "surviving load lost its line";
    EXPECT_TRUE(buf.holds(lineAddr(1))) << "surviving load lost its line";
    EXPECT_FALSE(buf.holds(lineAddr(2)));
    EXPECT_FALSE(buf.holds(lineAddr(3)));
    EXPECT_TRUE(buf.checkInvariants());
}

// retireThrough() releases the oldest; discardFrom() removes the newest.
// Running both must leave exactly the middle band.  This is the property that
// was actually wrong in the first attempt.
TEST(ShadowBufferTest, RetireAndDiscardAreComplementary)
{
    ShadowBuffer buf(16, kLine);
    auto data = makeLine(0x6B);

    for (int i = 0; i < 5; i++)
        buf.capture(10 * (i + 1), lineAddr(i), data.data());
    EXPECT_EQ(buf.size(), 5u);

    // Loads 10 and 20 have retired; 30, 40 and 50 are live.
    EXPECT_EQ(buf.retireThrough(30), 2u);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf.oldestOwner().value(), 30u);

    // A squash at 50 removes 50 only.
    EXPECT_EQ(buf.discardFrom(50), 1u);
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf.oldestOwner().value(), 30u);
    EXPECT_EQ(buf.newestOwner().value(), 40u);

    // A squash at 40 removes only 40. Load 30 is older than the squash point,
    // so it survives -- that is the whole point of the exclusive bound, and
    // it is why the two operations cannot be substituted for one another.
    EXPECT_EQ(buf.discardFrom(40), 1u);
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf.oldestOwner().value(), 30u);

    // Only a squash at 30 removes the last survivor.
    EXPECT_EQ(buf.discardFrom(30), 1u);
    EXPECT_TRUE(buf.empty());
    EXPECT_TRUE(buf.checkInvariants());
}

// A squashed owner's line must never be promotable. If it were, a discarded
// speculative line could still reach the real cache, which is precisely the
// property the mechanism exists to guarantee.
TEST(ShadowBufferTest, DiscardedOwnerIsNotPromotable)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x6C);

    buf.capture(10, lineAddr(0), data.data());
    buf.capture(20, lineAddr(1), data.data());
    EXPECT_EQ(buf.discardFrom(20), 1u);

    std::uint64_t addr = 0;
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(buf.promote(20, addr, out))
        << "a squashed load's line must never be promoted into the cache";
    EXPECT_EQ(buf.size(), 1u);
}

// An LVP mismatch squashes the violating load itself, so its own entry must be
// among the discarded ones. squash_num here is already the decremented,
// exclusive bound that ROB::squash() receives.
TEST(ShadowBufferTest, DiscardIncludesTheViolatingLoad)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x6D);

    buf.capture(20, lineAddr(0), data.data());
    buf.capture(21, lineAddr(1), data.data());

    // ROB::squash() receives 20 (one below the violating load at 21) when the
    // violating instruction is included in the squash.
    EXPECT_EQ(buf.discardFrom(20), 2u);
    EXPECT_TRUE(buf.empty());
}

// ---------------------------------------------------------------------------
// The late-fill scenario: capture() arriving after its owner was resolved
// ---------------------------------------------------------------------------

// This is the failure that killed the first implementation, reproduced at the
// buffer level.  A load misses and its request is tagged.  Before the fill
// returns, a branch squashes that load.  The fill arrives anyway -- gem5's
// MSHRs service squashed targets, because they have no notion of squashed
// instructions -- so capture() runs against a sequence number that will never
// commit and that the already-executed discard could not have covered.
//
// If nothing else removes it, the entry is stranded: no promote() will ever
// match a dead owner, while a later access to the same line refreshes it and
// keeps that dead owner.  The line then never reaches the cache and every
// access re-fetches it, without bound.  That is a livelock, and it presents as
// a hang rather than a wrong answer.
//
// The buffer's answer is that the retirement watermark removes it, so the
// recovery does not depend on catching the late capture at the moment it
// happens.
TEST(ShadowBufferTest, LateCaptureOfSquashedLoadIsSwept)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x70);

    // Load 90 is older than the branch, so it survives the squash.
    buf.capture(90, lineAddr(0), data.data());

    // A branch squashes at 95. The discard runs now, while load 100's line has
    // not been fetched yet, so it cannot remove an entry that does not exist.
    EXPECT_EQ(buf.discardFrom(95), 0u);
    EXPECT_EQ(buf.size(), 1u);

    // Now the fill for the squashed load lands.
    EXPECT_EQ(buf.capture(100, lineAddr(1), data.data()),
              CaptureResult::Accepted);
    EXPECT_EQ(buf.size(), 2u);

    // Load 90 reaches the commit head and its line is installed.
    std::uint64_t addr = 0;
    std::vector<std::uint8_t> out;
    EXPECT_TRUE(buf.promote(90, addr, out));

    // Commit then advances past 100 -- the squashed instructions are re-fetched
    // under fresh, higher sequence numbers, so the watermark does climb past
    // the dead owner -- and the sweep reclaims the stranded entry.
    EXPECT_EQ(buf.retireThrough(101), 1u);
    EXPECT_TRUE(buf.empty());
    EXPECT_FALSE(buf.holds(lineAddr(1)))
        << "a line owned by a squashed load must not be retained";
    EXPECT_TRUE(buf.checkInvariants());
}

// The subtlety that makes recovery possible at all: the watermark sweep only
// reaches entries *below* the watermark, so it cannot clean up a stranded entry
// immediately.  Recovery is deferred until commit advances past the dead owner.
//
// That is safe precisely because sequence numbers are monotonic and never
// reused, so the watermark is guaranteed to pass 100 eventually.  It does mean
// the line is unavailable -- and will re-miss -- for that window, which is a
// bounded cost rather than the unbounded re-fetch of the original bug.
TEST(ShadowBufferTest, StrandedEntryWaitsForTheWatermarkToClimb)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x74);

    buf.capture(100, lineAddr(1), data.data());   // late fill, owner is dead

    // The watermark is below the dead owner, so the sweep cannot reach it.
    EXPECT_EQ(buf.retireThrough(95), 0u);
    EXPECT_EQ(buf.size(), 1u) << "the sweep must not reach above the watermark";

    // Only once commit passes the dead owner does it go.
    EXPECT_EQ(buf.retireThrough(101), 1u);
    EXPECT_TRUE(buf.empty());
}

// The stranded entry must be gone, not merely unreachable.  If it survives,
// the next access to that line refreshes it under the dead owner and the
// livelock is permanent rather than merely delayed.
TEST(ShadowBufferTest, StrandedLineIsNotRevivedByReaccess)
{
    ShadowBuffer buf(8, kLine);
    auto a = makeLine(0x71);
    auto b = makeLine(0x72);

    buf.capture(100, lineAddr(1), a.data());   // squashed load's late fill
    EXPECT_EQ(buf.retireThrough(101), 1u);

    // Later, a different live load touches the same line.
    EXPECT_EQ(buf.capture(300, lineAddr(1), b.data()), CaptureResult::Accepted);
    EXPECT_EQ(buf.oldestOwner().value(), 300u)
        << "the new capture must own the line, not the dead one";

    std::uint64_t addr = 0;
    std::vector<std::uint8_t> out;
    EXPECT_TRUE(buf.promote(300, addr, out))
        << "the line must be promotable by its live owner";
    EXPECT_EQ(addr, lineAddr(1));
    EXPECT_EQ(out[0], 0x72) << "and must carry the refreshed contents";
}

// The sweep must not touch anything still live, even when the live owner is
// older than a dead one that arrived later -- which is the normal state, since
// a younger load can miss and fill before an older one does.
TEST(ShadowBufferTest, SweepKeepsLiveEntriesRegardlessOfArrivalOrder)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x73);

    buf.capture(300, lineAddr(0), data.data());  // younger load fills first
    buf.capture(200, lineAddr(1), data.data());  // older load fills later
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf.oldestOwner().value(), 200u)
        << "entries must be ordered by owner, not by arrival";

    EXPECT_EQ(buf.retireThrough(250), 1u);
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_TRUE(buf.holds(lineAddr(0)))
        << "load 300 is still live and must survive the sweep";
    EXPECT_TRUE(buf.checkInvariants());
}

// ---------------------------------------------------------------------------
// Promotion
// ---------------------------------------------------------------------------

TEST(ShadowBufferTest, PromoteByOwnerReturnsDataAndFrees)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x77);
    buf.capture(42, lineAddr(3), data.data());

    std::vector<std::uint8_t> out;
    std::uint64_t addr = 0;
    EXPECT_TRUE(buf.promote(42, addr, out));
    EXPECT_EQ(out, data);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_FALSE(buf.holds(lineAddr(3)));

    // Promoting again finds nothing, and that is not an error.
    std::vector<std::uint8_t> out2;
    std::uint64_t addr2 = 0;
    EXPECT_FALSE(buf.promote(42, addr2, out2));
}

TEST(ShadowBufferTest, PromoteWithWrongOwnerFindsNothing)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x88);
    buf.capture(10, lineAddr(0), data.data());

    std::vector<std::uint8_t> out;
    std::uint64_t addr = 0;
    EXPECT_FALSE(buf.promote(99, addr, out));
    EXPECT_EQ(buf.size(), 1u) << "a failed promote must not consume the entry";
}

// A line captured by two loads is promoted once, and only by the owner.
TEST(ShadowBufferTest, PromoteConsumesSharedLineExactlyOnce)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0x99);
    buf.capture(10, lineAddr(0), data.data());
    buf.capture(20, lineAddr(0), data.data());

    std::vector<std::uint8_t> out;
    std::uint64_t addr = 0;
    EXPECT_TRUE(buf.promote(10, addr, out));
    EXPECT_EQ(buf.size(), 0u);

    EXPECT_FALSE(buf.promote(20, addr, out))
        << "promoting a non-owner must not succeed";
}

// ---------------------------------------------------------------------------
// Store interaction
// ---------------------------------------------------------------------------

TEST(ShadowBufferTest, DropLineRemovesEntry)
{
    ShadowBuffer buf(4, kLine);
    auto data = makeLine(0xAA);
    buf.capture(10, lineAddr(0), data.data());

    EXPECT_TRUE(buf.dropLine(lineAddr(0)));
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_FALSE(buf.holds(lineAddr(0)));
    EXPECT_FALSE(buf.dropLine(lineAddr(0)));
}

// Dropping one line must not disturb its neighbours.
TEST(ShadowBufferTest, DropLineAffectsOnlyTheNamedLine)
{
    ShadowBuffer buf(8, kLine);
    auto a = makeLine(0x01);
    auto b = makeLine(0x02);
    auto c = makeLine(0x03);

    buf.capture(10, lineAddr(0), a.data());
    buf.capture(20, lineAddr(1), b.data());
    buf.capture(30, lineAddr(2), c.data());

    EXPECT_TRUE(buf.dropLine(lineAddr(1)));
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_TRUE(buf.holds(lineAddr(0)));
    EXPECT_FALSE(buf.holds(lineAddr(1)));
    EXPECT_TRUE(buf.holds(lineAddr(2)));
    EXPECT_TRUE(buf.checkInvariants());
}

// ---------------------------------------------------------------------------
// Overflow
// ---------------------------------------------------------------------------

// SafeSpec forbids evicting one speculative line to admit another, so an
// overflowing capture must be refused rather than displacing an existing
// entry.
TEST(ShadowBufferTest, OverflowRefusesRatherThanEvicts)
{
    ShadowBuffer buf(2, kLine);
    auto a = makeLine(0x01);
    auto b = makeLine(0x02);
    auto c = makeLine(0x03);

    EXPECT_EQ(buf.capture(10, lineAddr(0), a.data()),
              CaptureResult::Accepted);
    EXPECT_EQ(buf.capture(20, lineAddr(1), b.data()),
              CaptureResult::Accepted);
    EXPECT_EQ(buf.capture(30, lineAddr(2), c.data()), CaptureResult::Overflow);

    // Nothing was displaced, and the overflow was not stored.
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_TRUE(buf.holds(lineAddr(0)));
    EXPECT_TRUE(buf.holds(lineAddr(1)));
    EXPECT_FALSE(buf.holds(lineAddr(2)));
    EXPECT_EQ(buf.stats().overflows, 1u);

    std::vector<std::uint8_t> out(kLine, 0);
    EXPECT_FALSE(buf.lookup(lineAddr(2), 0, out.data(), kLine));
    EXPECT_TRUE(buf.checkInvariants());
}

// Refreshing a line that is already held must still succeed when the buffer
// is full: it needs no new slot, so it must not be mistaken for an overflow.
TEST(ShadowBufferTest, RefreshSucceedsWhenFull)
{
    ShadowBuffer buf(1, kLine);
    auto a = makeLine(0x01);
    auto b = makeLine(0x02);

    EXPECT_EQ(buf.capture(10, lineAddr(0), a.data()),
              CaptureResult::Accepted);
    EXPECT_EQ(buf.capture(20, lineAddr(0), b.data()), CaptureResult::Accepted)
        << "refresh of a held line needs no slot and must not overflow";
    EXPECT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf.oldestOwner().value(), 10u);

    EXPECT_EQ(buf.capture(30, lineAddr(1), b.data()), CaptureResult::Overflow);
}

// An overflowed capture must not leave the structure in a state where the
// caller can later "promote" something that was never stored.
TEST(ShadowBufferTest, OverflowedCaptureIsNotPromotable)
{
    ShadowBuffer buf(1, kLine);
    auto a = makeLine(0x01);
    auto b = makeLine(0x02);

    buf.capture(10, lineAddr(0), a.data());
    EXPECT_EQ(buf.capture(20, lineAddr(1), b.data()), CaptureResult::Overflow);

    std::vector<std::uint8_t> out;
    std::uint64_t addr = 0;
    EXPECT_FALSE(buf.promote(20, addr, out))
        << "an overflowed capture must never be promotable";
    EXPECT_EQ(buf.size(), 1u);
}

// Releasing entries must make room again.
TEST(ShadowBufferTest, RetireRestoresCapacity)
{
    ShadowBuffer buf(2, kLine);
    auto a = makeLine(0x01);
    auto b = makeLine(0x02);
    auto c = makeLine(0x03);

    buf.capture(10, lineAddr(0), a.data());
    buf.capture(20, lineAddr(1), b.data());
    EXPECT_EQ(buf.capture(30, lineAddr(2), c.data()), CaptureResult::Overflow);

    buf.retireThrough(15);
    EXPECT_EQ(buf.size(), 1u);

    EXPECT_EQ(buf.capture(30, lineAddr(2), c.data()),
              CaptureResult::Accepted)
        << "retirement must restore the ability to capture";
    EXPECT_TRUE(buf.checkInvariants());
}

// ---------------------------------------------------------------------------
// Mixed workload: the shape that actually broke
// ---------------------------------------------------------------------------

// Many lines, repeated, with a retirement watermark that advances the way a
// commit pointer would.  This is the generalisation of the livelock case:
// the buffer must track the watermark and never grow without bound, and must
// never exceed capacity.
TEST(ShadowBufferTest, MixedWorkloadTracksWatermark)
{
    ShadowBuffer buf(8, kLine);
    auto data = makeLine(0x5A);

    std::uint64_t seq = 1;
    std::uint64_t committed = 0;

    for (int round = 0; round < 500; round++) {
        // Capture a scatter of lines, some repeated.
        for (int i = 0; i < 6; i++) {
            std::uint64_t which = (round * 6 + i) % 11;
            buf.capture(seq, lineAddr(which), data.data());
            seq++;
        }

        EXPECT_LE(buf.size(), buf.capacity()) << "round " << round;
        EXPECT_TRUE(buf.checkInvariants()) << "round " << round;

        // Commit roughly half of what was issued.
        committed += 3;
        buf.retireThrough(committed);
    }

    EXPECT_TRUE(buf.checkInvariants());

    // Every entry still held must be owned by a load that has not retired.
    // Anything older would mean retirement had skipped it.
    if (!buf.empty()) {
        EXPECT_GE(buf.oldestOwner().value(), committed)
            << "an entry older than the watermark survived retirement";
    }
}