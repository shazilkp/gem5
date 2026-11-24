/*
 * Layer 6 / experiment A4 — Monocypher crypto_verify32 target.
 *
 * Ownership split (the point of this translation unit):
 *
 *   - The 32-byte key is a `static const` object owned by this file.  No other
 *     translation unit can name it, so the attacker in poc_verify32.c has no
 *     architectural reference to the value it is supposed to obtain.
 *   - `target_verify32(guess)` is the only attacker-callable entry point.  It
 *     calls the *real* Monocypher crypto_verify32() with the attacker's guess
 *     as the first operand and the key as the second operand.
 *   - `target_verify32_selfcheck()` is the experimenter's architectural
 *     consistency check (crypto_verify32(key, key) == 0).  It is not on the
 *     attacker's measurement path and exposes only a boolean.
 *   - `oracle_report_verify32()` is the post-hoc oracle: it compares the
 *     attacker's recovered word with the real key word and prints it.  It runs
 *     after the measurement, playing the role of the experimenter checking the
 *     answer.  It is the only code (besides the target itself) that reads the
 *     key bytes.
 *
 * Why the guess is not the key:
 *   The task wording suggested passing guess == key "so the compare does not
 *   early-exit".  crypto_verify32 is Monocypher's constant-time comparison:
 *   `x32()` loads all 32 bytes of *both* operands unconditionally and the exit
 *   path is branchless (see monocypher.c: x16/x32/neq0).  There is therefore no
 *   early exit, and the guess *content* cannot move the key's load PCs or their
 *   distances.  The attacker cannot name the key anyway (ownership split), so
 *   the harness passes a fixed attacker-chosen guess and the gate re-measures d
 *   with two different guess buffers to prove invariance.  See RESULTS_A4.md.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "monocypher.h"

/* 32-byte key, all bytes distinct.  Last word (offsets 24..31) is
 * 6a db 4c bd 2e 8f 10 a1.  `static` => local symbol, unnameable elsewhere. */
static const uint8_t v32_key_storage[32]
    __attribute__((aligned(8), used)) = {
        0x5a, 0x1b, 0x9c, 0x2d, 0x7e, 0x3f, 0xa0, 0x41,
        0x13, 0xe2, 0x84, 0x65, 0xc6, 0x27, 0xd8, 0x09,
        0xf1, 0x32, 0xb3, 0x74, 0x05, 0x96, 0x57, 0xe8,
        0x6a, 0xdb, 0x4c, 0xbd, 0x2e, 0x8f, 0x10, 0xa1
    };

/* Real Monocypher comparison: guess is the first operand, the key is the
 * second.  Tail-calls crypto_verify32 in the current build, so the callee's
 * `ret` returns directly to the attacker's caller. */
__attribute__((noinline, noclone, noipa))
int
target_verify32(const uint8_t guess[32])
{
    return crypto_verify32(guess, v32_key_storage);
}

/* Architectural consistency check: the key compares equal to itself.  Returns
 * 0 on success (the same convention as crypto_verify32). */
__attribute__((noinline, noclone, noipa))
int
target_verify32_selfcheck(void)
{
    return crypto_verify32(v32_key_storage, v32_key_storage);
}

/* Post-hoc oracle: `lane_base` is the byte offset (in the key) of the 8-byte
 * word the attacker claims to have measured.  `known_mask` bit i means the
 * attacker measured key byte (lane_base + i). */
__attribute__((noinline, noclone, noipa))
int
oracle_report_verify32(uint64_t recovered, uint64_t known_mask, int lane_base,
                       int arch_ok, uint64_t sink)
{
    uint64_t expect = 0;
    int known = 0;

    for (int i = 0; i < 8; i++)
        known += (int)((known_mask >> i) & 1ULL);
    memcpy(&expect, v32_key_storage + lane_base, 8);

    int match = arch_ok && (known_mask == 0xffULL) && (recovered == expect);

    printf("verify32_attack key_word=%016llx recovered=%016llx known=%02x "
           "bytes=%d/8 lane_base=%d arch=%s sink=%llu %s\n",
           (unsigned long long)expect, (unsigned long long)recovered,
           (unsigned)(known_mask & 0xff), known, lane_base,
           arch_ok ? "OK" : "BAD", (unsigned long long)sink,
           match ? "MATCH" : "NO-MATCH");
    return match ? 0 : 1;
}
