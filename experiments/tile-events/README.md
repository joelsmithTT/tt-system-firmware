# Tile events: Tensix → ARC → host prototype

Pathfinding for a generic way for a Tensix tile to tell the host that something
happened, built on the device event ring (`joelsmith/device-events`). Not for upstream.

## Protocol

1. **Tile publishes.** It writes a 24-byte record into its own L1, anywhere 16-byte aligned:
   magic `0x54455654` ("TEVT"), `type` (u16) and a reserved u16, `cause`, `data[3]`.
2. **Tile pings ARC.** It writes a 32-bit token to ARC's MSI catcher FIFO, NOC (8,0)
   address `0x800B0000`, with a non-posted NOC write from L1:
   bit 31 set, bits 30:12 the record address >> 4, bits 11:0 its translated coordinates
   `x | y << 6`. Value 0 stays the host's message queue doorbell.
3. **ARC forwards.** It reads the record over the NOC, appends it to the event ring with the
   tile as `source`, and clears the magic as the acknowledgement. The KMD drains the ring as
   usual, and readers get the record from `READ_EVENTS`.

ARC firmware and tile firmware agree on the record format and token encoding only, not on a
fixed L1 address.

## ARC's MSI catcher FIFO (measured on P100A)

- 64 entries of 32 bits. A write to a full FIFO is dropped, and the sender's non-posted
  write still completes, so the sender can't tell.
- Status at `+0x8`: bit 0 ready (not full), bit 8 nonempty, bit 9 overflow.
- Overflow asserts at `64 - hwm` entries. `hwm` is at `+0xC`, resets to 1 and is writable.
  Stock firmware flushes the whole FIFO on overflow.
- Same design as the L2CPU tile's 16-entry MSI catcher
  (tt-isa-documentation `BlackholeA0/L2CPUTile/MSICatcher.md`), scaled to 64.

## Results (P100A, BRISC on Tensix (2,2))

| BRISC sends | Result |
| --- | --- |
| 1, then 10 tokens | All delivered and acknowledged, in order |
| 100 tokens, waiting for each write | ARC forwarded all 100. The 64-entry event ring filled faster than the KMD drained it, so the host got 64 to 86 records plus a `FW_EVENTS_LOST` record counting the rest |
| 100 tokens back to back | ARC received 67 to 68; the rest were dropped at the FIFO. Every record that arrived was intact and in order. ARC's overflow handler never fired |

The goal is to stop a workload quickly, so if many tiles fail at once, catching some of them
is enough.

## Running it

**Only use Tensix tiles that exist on every SKU**: translated x 1–7 and 10–14, y 2–11.
x 15–16 are fused off on p100, and a NOC access to a missing tile can hang the host. Both
the tools and the firmware refuse other coordinates. (2,2) is a good default.

Needs this branch's firmware on the card and the KMD from tt-kmd `joelsmith/events-ring`.

```sh
SFPI=~/sfpi KMD_DIR=~/tt-kmd ./build.sh
./tiledrive 2 2 10      # 10 tokens back to back
./tiledrive 2 2 100 1   # 100 tokens, waiting for each write to complete
./stats                 # ARC's forwarding counters
```

`tiledrive` holds the tile's RISC-V cores in reset, copies `brisc.bin` to L1 address 0, sets
its parameters, releases BRISC, then reports ARC's counters, how many records ARC
acknowledged, and what arrived through `READ_EVENTS`. It puts the tile back in reset when
done.

The BRISC program clears its NOC transaction counters before sending; they are stale after
reset, and waiting on them otherwise never finishes.
