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

    std::printf("shadow buffer: all checks passed\n");
    return 0;
}
