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
