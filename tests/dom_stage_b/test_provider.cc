/*
 * Host-side unit test for the committed-value provider.
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cpu/o3/dom_provider.hh"

using gem5::o3::DomValueProvider;

int
main()
{
    DomValueProvider provider(64);

    const uint64_t word = 0x1122334455667788ull;
    provider.fill(0x1000, reinterpret_cast<const uint8_t *>(&word), 8);

    uint64_t out = 0;
    assert(provider.lookup(0x1000, 8, reinterpret_cast<uint8_t *>(&out)));
    assert(out == word);

    // A narrower load reads the low half of the same word.
    uint32_t half = 0;
    assert(provider.lookup(0x1004, 4, reinterpret_cast<uint8_t *>(&half)));
    assert(half == 0x11223344u);

    // An untouched word misses.
    assert(!provider.lookup(0x2000, 8, reinterpret_cast<uint8_t *>(&out)));

    // A partial fill only knows the filled bytes, so a wider load misses.
    const uint32_t low = 0xaabbccdd;
    provider.fill(0x3000, reinterpret_cast<const uint8_t *>(&low), 4);
    assert(provider.lookup(0x3000, 4, reinterpret_cast<uint8_t *>(&half)));
    assert(half == low);
    assert(!provider.lookup(0x3000, 8, reinterpret_cast<uint8_t *>(&out)));

    // Filling the other half completes the word.
    const uint32_t high = 0x01020304;
    provider.fill(0x3004, reinterpret_cast<const uint8_t *>(&high), 4);
    assert(provider.lookup(0x3000, 8, reinterpret_cast<uint8_t *>(&out)));
    assert(out == 0x01020304aabbccddull);

    // Updating a single byte is visible to both narrow and wide loads.
    const uint8_t byte = 0xee;
    provider.fill(0x3001, &byte, 1);
    assert(provider.lookup(0x3000, 4, reinterpret_cast<uint8_t *>(&half)));
    assert(half == 0xaabbeeDDu);
    assert(provider.lookup(0x3000, 8, reinterpret_cast<uint8_t *>(&out)));
    assert(out == 0x01020304aabbeeDDull);

    // Accesses that straddle a word are bypassed.
    uint8_t straddle[4] = {};
    assert(!provider.lookup(0x4007, 2, straddle));

    // Invalidating committed bytes removes them from the known set.
    provider.fill(0x5000, reinterpret_cast<const uint8_t *>(&word), 8);
    provider.invalidate(0x5000, 2);
    assert(!provider.lookup(0x5000, 8, reinterpret_cast<uint8_t *>(&out)));
    uint32_t tail = 0;
    assert(provider.lookup(0x5002, 4, reinterpret_cast<uint8_t *>(&tail)));
    provider.invalidate(0x5002, 6);
    assert(!provider.lookup(0x5002, 4, reinterpret_cast<uint8_t *>(&tail)));

    // A syscall/DMA flush drops every committed entry.
    provider.fill(0x7000, reinterpret_cast<const uint8_t *>(&word), 8);
    assert(provider.lookup(0x7000, 8, reinterpret_cast<uint8_t *>(&out)));
    provider.flush();
    assert(!provider.lookup(0x7000, 8, reinterpret_cast<uint8_t *>(&out)));

    // A store that crosses a word boundary invalidates every touched word.
    provider.fill(0x6000, reinterpret_cast<const uint8_t *>(&word), 8);
    provider.fill(0x6008, reinterpret_cast<const uint8_t *>(&word), 8);
    provider.invalidateRange(0x6004, 8);
    assert(!provider.lookup(0x6000, 8, reinterpret_cast<uint8_t *>(&out)));
    assert(provider.lookup(0x6000, 4, reinterpret_cast<uint8_t *>(&half)));
    assert(!provider.lookup(0x6008, 8, reinterpret_cast<uint8_t *>(&out)));
    assert(provider.lookup(0x600c, 4, reinterpret_cast<uint8_t *>(&half)));

    // A reused way must not leak the evicted word's byte mask.  Fill four
    // words in one set, then reuse a way for a fifth word with a partial
    // write: a wider lookup of the fifth word must miss.
    DomValueProvider ev(16);   // 4 sets x 4 ways
    for (int i = 0; i < 4; i++) {
        ev.fill(static_cast<gem5::Addr>(i) * 4 * 8,
                reinterpret_cast<const uint8_t *>(&word), 8);
    }
    const uint32_t partial = 0xdeadbeef;
    ev.fill(16 * 8, reinterpret_cast<const uint8_t *>(&partial), 4);
    uint64_t wide = 0;
    assert(!ev.lookup(16 * 8, 8, reinterpret_cast<uint8_t *>(&wide)));
    uint32_t narrow = 0;
    assert(ev.lookup(16 * 8, 4, reinterpret_cast<uint8_t *>(&narrow)));
    assert(narrow == partial);

    // Round-robin eviction remains functional.
    for (int i = 0; i < 1000; i++) {
        provider.fill(0x8000 + i * 8,
                      reinterpret_cast<const uint8_t *>(&word), 8);
    }
    assert(provider.lookup(0x8000 + 999 * 8, 8,
                           reinterpret_cast<uint8_t *>(&out)));
    assert(out == word);

    std::printf("provider: all checks passed\n");
    return 0;
}
