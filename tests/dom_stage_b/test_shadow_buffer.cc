/*
 * Host-side unit test for the Delay-on-Miss shadow buffer.  This is the
 * smallest testable unit of B1 and does not depend on the gem5 build.
 */

#include <cassert>
#include <cstdio>

#include "cpu/o3/dom.hh"

using gem5::InstSeqNum;
using gem5::o3::DomShadowBuffer;

int
main()
{
    DomShadowBuffer sb;
    assert(sb.empty());
    assert(sb.youngestCaster() == 0);

    sb.add(10);
    assert(sb.youngestCaster() == 10);
    sb.add(20);
    assert(sb.youngestCaster() == 20);
    assert(sb.isCasting(10));
    assert(sb.isCasting(20));

    // Resolving the youngest caster releases only loads that recorded it.
    assert(sb.resolve(20));
    assert(!sb.resolve(20));
    assert(!sb.isCasting(20));
    assert(sb.youngestCaster() == 10);

    // A resolved non-head caster cannot be returned as a shadow.
    assert(sb.resolve(10));
    assert(sb.youngestCaster() == 0);

    // Retiring up to and including 20 removes both entries.
    sb.retire(20);
    assert(sb.empty());

    // Squash drops younger casters and keeps the squash point.
    DomShadowBuffer sq;
    sq.add(30);
    sq.add(40);
    sq.add(50);
    sq.squash(40);
    assert(sq.size() == 2);
    assert(sq.isCasting(30));
    assert(sq.isCasting(40));
    assert(!sq.isCasting(50));
    assert(sq.youngestCaster() == 40);

    // Retire is in dispatch order and drops everything at or below.
    sq.retire(40);
    assert(sq.empty());

    // A load must record the youngest caster *before* its own E entry is
    // added.  If it is added first, youngestCaster() returns the load itself
    // and it deadlocks waiting for its own retirement.
    DomShadowBuffer self;
    self.add(100);
    const InstSeqNum loadSeq = 101;
    const InstSeqNum caster = self.youngestCaster();
    assert(caster == 100);
    assert(caster < loadSeq);
    self.add(loadSeq);
    assert(self.contains(loadSeq));
    assert(self.youngestCaster() == loadSeq);   // the trap the CPU must avoid
    auto popped = self.retire(loadSeq);
    assert(popped.size() == 2);
    assert(popped[0] == 100 && popped[1] == loadSeq);
    assert(self.empty());

    // resolve() stops new shadows, but a load that already recorded the
    // caster stays shadowed until the entry leaves the SB.
    DomShadowBuffer sc;
    sc.add(200);
    assert(sc.isCasting(200));
    assert(sc.contains(200));
    assert(sc.resolve(200));
    assert(!sc.isCasting(200));
    assert(sc.contains(200));
    auto retired = sc.retire(200);
    assert(retired.size() == 1 && retired[0] == 200);
    assert(!sc.contains(200));

    // Squash removes the entry from the buffer entirely.
    DomShadowBuffer ss;
    ss.add(300);
    ss.squash(299);
    assert(!ss.contains(300));
    assert(!ss.isCasting(300));

    std::printf("shadow buffer: all checks passed\n");
    return 0;
}
