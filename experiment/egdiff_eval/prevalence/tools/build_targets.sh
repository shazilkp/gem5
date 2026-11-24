#!/usr/bin/env bash
# Build all prevalence targets as static, non-PIE riscv64 binaries so that the
# runtime PCs reported by qemu-riscv64 match `objdump -d` exactly.
#
# Extra flags (beyond the -static -O2 -fno-pic -no-pie required by the study):
#   -fno-ipa-icf                        keep the two marker functions distinct
#   -fno-builtin-{memcmp,strcmp,strlen} keep the control loops from being
#                                       rewritten into libc calls
#   -fno-tree-loop-distribute-patterns  same, for loop idiom recognition
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
build="$root/build"
mono="$root/third_party/Monocypher/src/monocypher.c"
CC="${CC:-riscv64-linux-gnu-gcc}"

mkdir -p "$build"

CFLAGS=(
  -std=gnu11
  -static -O2 -fno-pic -no-pie
  -fno-ipa-icf
  -fno-builtin-memcmp -fno-builtin-strcmp -fno-builtin-strlen
  -fno-ipa-pure-const
  -fno-tree-loop-distribute-patterns
  -Wall -Wno-unused-function -Wno-unused-variable
  -I"$root/targets"
  -I"$root/third_party/Monocypher/src"
)

echo "== compiler: $("$CC" --version | head -1)"
echo "== Monocypher: $(git -C "$root/third_party/Monocypher" rev-parse --short HEAD 2>/dev/null || echo unknown)"

for t in verify32 chacha20 blake2b aead memcmp strcmp hashlookup listwalk; do
  src="$root/targets/t_$t.c"
  out="$build/t_$t"
  echo "== building t_$t"
  case "$t" in
    verify32|chacha20|blake2b|aead)
      "$CC" "${CFLAGS[@]}" -o "$out" "$src" "$mono" ;;
    *)
      "$CC" "${CFLAGS[@]}" -o "$out" "$src" ;;
  esac
  riscv64-linux-gnu-objdump -d "$out" > "$build/t_$t.dis"
done

echo "== done; binaries in $build"
file "$build/t_verify32" | head -1
