# FHL: firmware Huffman LZ

An LZ77 codec with semi-static Huffman coding, built for decompressing
firmware stages from SPI flash. It is 4% denser than LZSA2 and decodes faster
than LZSA2, with a 3.5 KB decoder and no adaptive state.

## Numbers

Corpus: 28 coreboot stages (romstage, ramstage and postcar from 10 boards),
3,646,560 bytes. Decode time is the sum of per-stage medians on an Intel N100
core, built with gcc `-O2 -march=x86-64-v2`. Decoder size is `.text + .rodata`
at x86-64 `-Os`.

| codec | bytes | vs LZSA2 | decode | decoder | RAM |
|---|---:|---:|---:|---:|---:|
| LZMA (coreboot, lc3) | 1,565,241 | −12.8% | 101.0 ms | 3.6 KB | 16 KB |
| [ANZ](https://github.com/ArthurHeymans/anz) | 1,581,693 | −11.9% | 60.0 ms | 2.9 KB | 83 KB |
| zstd -19 (coreboot decoder) | 1,665,519 | −7.2% | 6.6 ms | 41 KB | 96 KB |
| **FHL** | 1,718,463 | −4.3% | 5.8 ms | 3.5 KB | 45 KB |
| **FHL `-K 1`** | 1,723,647 | −4.0% | **5.5 ms** | 3.5 KB | 45 KB (10 KB used) |
| LZSA2, [lzsa2-fast](https://github.com/ArthurHeymans/lzsa2-fast) | 1,795,384 | 0 | 6.4 ms | 1.9 KB | 0 |
| LZSA2, upstream lzsa | 1,795,384 | 0 | 7.1 ms | 1.1 KB | 0 |

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
- Every stream is followed by 8 zero bytes.

The encoder uses forward optimal parsing with Huffman prices, iterated four
times. It k-means-clusters the 256 contexts into K tables and keeps the
smallest K (or uses `-K`).

## Use

```c
#include "fhl.h"
static uint64_t work[FHL_WORK_SIZE / 8];
size_t n = fhl_decode(src, src_len, dst, dst_cap, work);   /* 0 on a bad header */
```

Build with `-DFHL_DECODER_ONLY` to get just `fhl_decode()`.

```
make            # fhl c [-K 1|2|4|8|16] [-p passes] in out ; fhl d in out
make test       # round trips under ASan/UBSan
```

The encoder verifies every stream it writes. It runs at about 0.5 MB/s
(2 MB/s with `-K 1`) and uses about 70 bytes of RAM per input byte.

## Limitations

- The decoder trusts the stream beyond its header. Use it on streams you
  produced or have verified, for example by hash.
- It needs the whole output buffer, because literals are staged in its
  tail, so in-place decompression is not possible.
- There is no stored mode: incompressible data grows by about 0.05%.
- The window is 1 MiB.

## License

MIT, see [LICENSE](LICENSE).
