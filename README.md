# FHL: firmware Huffman LZ

An LZ77 codec with semi-static Huffman coding, built for decompressing
firmware stages from SPI flash, with a 3.5 KB decoder and no adaptive state.
On coreboot stages it is 4% denser than LZSA2 and, on the two Intel cores
measured (N100, i5-8350U), decodes faster than LZSA2 and coreboot's zstd.
That lead is CPU-dependent: on a Ryzen 9 8945HS both decode faster than
FHL, and outside firmware zstd -19 is smaller and faster (see
[Other data and cores](#other-data-and-cores)).

## Numbers

Corpus: 28 coreboot stages (romstage, ramstage and postcar from 10 boards),
3,646,560 bytes. Decode time is the sum of per-stage medians over 7 passes
on one core, built with gcc `-O2 -march=x86-64-v2`. Decoder size is
`.text + .rodata` at x86-64 `-Os`. Two runs on the i5-8350U agreed within
0.3%.

| codec | bytes | vs LZSA2 | N100 | i5-8350U | decoder | RAM |
|---|---:|---:|---:|---:|---:|---:|
| LZMA (coreboot, lc3) | 1,565,241 | −12.8% | 101.1 ms | 102.4 ms | 3.6 KB | 16 KB |
| [ANZ](https://github.com/ArthurHeymans/anz) | 1,581,767 | −11.9% | 59.9 ms | 69.1 ms | 2.9 KB | 83 KB |
| zstd -19 (coreboot decoder) | 1,665,519 | −7.2% | 6.7 ms | 6.7 ms | 41 KB | 96 KB |
| **FHL `-c`** | 1,716,520 | −4.4% | 5.8 ms | 6.5 ms | 3.5 KB | 45 KB |
| **FHL `-c -K 1`** | 1,722,404 | −4.1% | **5.6 ms** | **6.3 ms** | 3.5 KB | 45 KB (10 KB used) |
| LZSA2, [lzsa2-fast](https://github.com/ArthurHeymans/lzsa2-fast) | 1,795,384 | 0 | 6.4 ms | 6.9 ms | 1.9 KB | 0 |
| LZSA2, upstream lzsa | 1,795,384 | 0 | 7.1 ms | 7.8 ms | 1.1 KB | 0 |

These FHL streams are from `-c`, the earlier default encoder. The current
default writes 1,714,883 B (−0.1%); its streams decoded 0.4-1.6% faster than
the `-c` ones on all three hosts (`gbench`, below), so the times above
should hold for them too.

I also modelled boot-time load cost as flash read time plus decode time,
with the decoder's own code read from flash once per board. In that model FHL
has the shortest stage load time on an N100-class core from 30 MHz
single-I/O SPI up to 100 MB/s. This is a host model, not a measurement on
real boot hardware.

## How it decodes fast

The decoder runs in two passes.

1. **Literals first.** All literals are decoded into the tail of the output
   buffer. They alternate between two bitstreams, and each literal's
   Huffman table is chosen by the literal two before it, so the two
   lookup chains are independent and run in parallel. Each table entry
   carries the index of the next table, so selecting it costs no extra load.
2. **Commands.** These come from their own bitstream, and distances from a
   third. Each is read with a single 64-bit peek. Literal runs are copied
   from the output tail. For the leading `nfast` commands (counted by the
   encoder) the 16-byte over-copies need no bounds checks. The rep distance
   is picked by indexing an array, and matches use 8-byte moves.

## Format

All multi-byte fields are little-endian, and bitstreams are LSB-first.

```
 0  u32 size       decompressed size
 4  u8  K          literal tables: 1, 2, 4, 8 or 16
 5  u32 nlit       number of literals
 9  u32 nfast      leading commands whose wide copies stay in bounds
13  u32 offset of literal stream A (literals 0, 2, 4, ...)
17  u32 offset of literal stream B (literals 1, 3, 5, ...)
21  u32 offset of the distance stream
25  main bitstream: context map, code lengths, then the commands
```

- **Context map:** 256 entries of log2(K) bits each. The literal two before
  (0 at the start) selects the table for the next literal.
- **Code lengths:** 15 meta-code lengths of 3 bits each, then the lengths
  for K×256 literal symbols, 256 command symbols and 43 distance symbols.
  These are coded with the meta code: 0-11 is a literal length, 12 repeats
  the previous length 3-6 times (2 extra bits), 13 is a run of 3-10 zeros
  (3 extra bits) and 14 is a run of 11-138 zeros (7 extra bits).
- **Huffman codes:** canonical, at most 10 bits, sent bit-reversed.
- **Commands:** a Huffman symbol `16*llc + mlc` encodes the literal-run and
  match-length codes. It is followed by the run's extra bits and then the
  length's extra bits (see `ll_base`/`ll_bits`/`ml_base`/`ml_bits` in
  `fhl.c`; the minimum match is 2). The last command has literals only.
- **Distances:** symbol 0-2 is a rep distance (1 and 2 move to the front).
  Symbol 3+s is slot s: s < 4 gives distance s+1, otherwise
  `((2 | s&1) << nb) + extra + 1` with `nb = s/2 - 1` raw extra bits.
- Every stream is followed by 8 zero bytes, the last one by 16.

The encoder runs a forward optimal parse twice: first with flat prices, then
with Huffman prices from the first parse's statistics (more iterations made
the streams larger). It k-means-clusters the 256 contexts into K tables. The
flat parse is the same for every K, so it runs once; by default each K then
gets its own priced parse on its own thread, and the smallest stream wins;
`-K` picks one. Matches come from a binary tree per 3-byte hash (as LZMA's
bt), which is serial. Options:

- `-c` uses hash chains instead, searched on `-j` threads with the same
  matches for any thread count. This was the default before; it writes
  the same streams as then, slightly larger than the tree's.
- `-g` seeds the prices with a lazy greedy parse instead of the flat one.
- `-f` prices every K from one parse (priced with PARSE_K = 4 tables) and
  writes only the K whose literals estimate smallest.

## Use

```c
#include "fhl.h"
static uint64_t work[FHL_WORK_SIZE / 8];
size_t n = fhl_decode(src, src_len, dst, dst_cap, work);   /* 0 on a bad header */
```

Build with `-DFHL_DECODER_ONLY` to get just `fhl_decode()`.

On 32-bit targets and on targets without fast misaligned loads (RISC-V
unless GCC reports `__riscv_misaligned_fast`) the decoder reads its
bitstreams as aligned native words: 30% faster than 8-byte unaligned reads
for i386 on a Ryzen 9 and 10% on an Atom D410, but 15-20% slower on x86-64.
`-DFHL_WORD_READS=0` or `=1` overrides the choice.

```
make            # fhl c [-K 1|2|4|8|16] [-p passes] [-d depth] [-c] [-g] [-f] [-j threads] in out
make test       # round trips under ASan/UBSan
```

Compile the decoder at `-O2` for the speed figures above; `-Os` trades speed
for a smaller decoder. Explicit copy builtins preserve the fixed-size loads
and copies even with `-ffreestanding -fno-builtin`. Variable-size copies may
still call the firmware's `memmove` implementation.

Encoding the corpus takes the following time in seconds. The encoders verify
every stream they write. FHL uses about 70 bytes of RAM per input byte per
thread.

| encoder | N100 | i5-8350U | Ryzen 9 8945HS |
|---|---:|---:|---:|
| zstd -19 | 0.9 | 1.0 | 0.6 |
| xz `--format=lzma -9e` | 2.0 | 2.4 | 1.5 |
| upstream `lzsa -f 2` | 15.3 | 16.9 | 9.1 |
| **fhl `-K 1`** | 1.9 | 2.0 | 1.2 |
| **fhl**, 1 thread | 6.4 | 6.9 | 3.6 |
| **fhl**, 5 threads | 3.0 | 3.1 | 1.5 |

That table predates the current encoder. Rerun on the Ryzen 9 8945HS, gcc
-O2, one thread unless noted:

| encoder | time | bytes |
|---|---:|---:|
| zstd -19 | 0.56 s | 1,665,631 |
| xz `--format=lzma -9e` | 1.56 s | 1,565,605 |
| `fhl -c` (the earlier default), before this round's speedups | 3.81 s | 1,716,520 |
| `fhl -c` | 1.38 s | 1,716,520 |
| `fhl -c`, 5 threads | 0.73 s | 1,716,520 |
| `fhl` | 1.13 s | 1,714,883 |
| `fhl`, 5 threads | 0.78 s | 1,714,883 |
| `fhl -f` | 0.65 s | 1,715,903 |
| `fhl -g -f` | **0.57 s** | 1,717,830 |
| `fhl -g -f -d 16` | 0.52 s | 1,719,673 |

The tree match finder is serial, so threads help it less than they help
`-c`. On the N100 and i5-8350U, `fhl -g -f` takes 0.8-0.9 s and zstd -19
0.8 s.

## Other data and cores

A second harness, `gbench`, times one-core decode of FHL against stock zstd
and upstream lzsa, all built with gcc `-O2 -march=x86-64-v2`, in MB/s of
output (best of 3 runs of at least 20 ms per block). It ranks them as the
table above does on the N100 and i5-8350U.

Decode MB/s on the coreboot corpus:

| codec | bytes | N100 | i5-8350U | Ryzen 9 8945HS |
|---|---:|---:|---:|---:|
| zstd -19 | 1,664,949 | 582 | 525 | **1221** |
| zstd -19, coreboot's build | 1,664,949 | 577 | 511 | 1193 |
| `fhl` | 1,714,883 | **653** | **570** | 1103 |
| upstream LZSA2 | 1,795,384 | 535 | 478 | 1176 |

Decode MB/s on 1 MiB blocks of the [Silesia corpus](https://sun.aei.polsl.pl/~sdeor/corpus/)
(the first 4 MiB of each of its 12 files, compressed block by block):

| codec | bytes | N100 | i5-8350U | Ryzen 9 8945HS |
|---|---:|---:|---:|---:|
| zstd -19 | 18,025,972 | **989** | **907** | **2022** |
| `fhl` | 18,114,930 | 916 | 817 | 1584 |
| `fhl -c` | 18,286,558 | 903 | 809 | 1599 |
| upstream LZSA2 | 20,988,344 | 744 | 691 | 1490 |

So FHL's speed lead over zstd is specific to firmware on these Intel cores.
On general data zstd -19 is 0.5% smaller and 8-28% faster to decode, and
encodes at a similar speed (`fhl -g -f` takes 1.1-1.3x zstd -19's time). What
FHL keeps everywhere is its size: a 3.5 KB decoder against zstd's 41 KB.

## Limitations

- The decoder trusts the stream beyond its header. Use it on streams you
  produced or have verified, for example by hash.
- It needs the whole output buffer, because literals are staged in its
  tail, so in-place decompression is not possible.
- There is no stored mode: incompressible data grows by about 0.05% and
  decodes far slower than a stored block would.
- The window is 1 MiB.

## License

MIT, see [LICENSE](LICENSE).
