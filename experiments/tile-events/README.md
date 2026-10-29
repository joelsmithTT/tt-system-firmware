# Tile events: Tensix → ARC → host prototype

Pathfinding for a generic way for a Tensix tile to tell the host that something
happened, built on the device event ring (`joelsmith/device-events`). Not for upstream.

## Protocol

1. **Tile publishes.** It writes a 24-byte record into its own L1:
   magic `0x54455654` ("TEVT"), `type` (u16) and a reserved u16, `cause`, `data[3]`.
2. **Tile announces it to ARC**, through either of two ARC tile blocks (below), with a
   non-posted NOC write from L1 to (8,0).
3. **ARC forwards.** It reads the record over the NOC, appends it to the event ring with the
   tile as `source`, and clears the magic as the acknowledgement. The KMD drains the ring as
   usual, and readers get the record from `READ_EVENTS`.

ARC firmware and tile firmware agree on the record format and announcement encoding only,
not on a fixed L1 address.

### Announcing through the PMM mailbox (preferred)

The Power Management Messaging block, `0x80400000`, keeps the latest 16-bit message from each
tile in its own slot, so a burst from many tiles loses nothing. The tile writes to `+0x0`:

| Bits | Field |
| --- | --- |
| 31:26, 25:20 | its translated x, y |
| 12 | flag0: a record is pending |
| 11:0 | record L1 address >> 9 (records 512-byte aligned) |

ARC sets flag0's total-count threshold to 1 and routes it to PMM's urgent interrupt (ARC
line 77; regular is 76). The handler finds flagged tiles from `PMM_FLG0_COL_STAT` and the
per-column bitmaps, forwards each record, clears the tile's flag by writing a message on its
behalf, and only then clears the magic. A tile waits for that ack before announcing its next
record, since a second message would overwrite the first.

Register map: syseng `blackhole-bringup`, `t6py/.../data/blackhole/a0/arc.json`, `pmm_block`.
Production firmware doesn't otherwise use PMM.

### Announcing through the MSI catcher

The tile writes a token to the MSI catcher FIFO, `0x800B0000`, the same FIFO the host uses as
the message queue doorbell: bit 31 set, bits 30:12 the record address >> 4, bits 11:0 its
coordinates `x | y << 6`. Value 0 stays the host's doorbell.

- 64 entries of 32 bits. A write to a full FIFO is dropped, and the sender's non-posted
  write still completes, so the sender can't tell.
- Status at `+0x8`: bit 0 ready (not full), bit 8 nonempty, bit 9 overflow.
- Overflow asserts at `64 - hwm` entries. `hwm` is at `+0xC`, resets to 1 and is writable.
  Stock firmware flushes the whole FIFO on overflow.
- Same design as the L2CPU tile's 16-entry MSI catcher
  (tt-isa-documentation `BlackholeA0/L2CPUTile/MSICatcher.md`), scaled to 64.

## Results (P100A)

| Test | PMM mailbox | MSI catcher |
| --- | --- | --- |
| (2,2) sends 100 | ARC forwarded 100/100 | ARC forwarded 100/100 waiting for each write; about 68 back to back, the rest dropped at the FIFO |
| 120 tiles send 4 each, back to back | ARC forwarded 480/480, in one interrupt | ARC received about 240 tokens and forwarded 128 |

Every record that reached the host was intact, from the right tile and in order per tile.
In the 120-tile runs the host got about 220 of the 480. ARC appends faster than the KMD drains
the 64-entry event ring, and the rest arrive as `FW_EVENTS_LOST` counts.

The goal is to stop a workload quickly, so if many tiles fail at once, catching some of them
is enough.

## Running it

**Only use Tensix tiles that exist on every SKU**: translated x 1–7 and 10–14, y 2–11.
x 15–16 are fused off on p100, and a NOC access to a missing tile can hang the host. Both
the tools and the firmware refuse other coordinates. (2,2) is a good default.

Needs this branch's firmware on the card and the KMD from tt-kmd `joelsmith/events-ring`.

```sh
SFPI=~/sfpi KMD_DIR=~/tt-kmd ./build.sh
./tiledrive -p 10 2,2        # 10 records through PMM
./tiledrive 10 2,2           # through the MSI catcher, back to back
./tiledrive -s 100 2,2       # MSI catcher, waiting for each write to complete
./tiledrive -p 4 2,2 3,2 10,5  # several tiles at once
./stats                      # ARC's forwarding counters
```

`tiledrive` holds each tile's RISC-V cores in reset, copies `brisc.bin` to L1 address 0, sets
its parameters, releases the BRISCs one after another, then reports ARC's counters, how many
records ARC acknowledged, and what arrived through `READ_EVENTS`. It puts the tiles back in
reset when done.

The BRISC program clears its NOC transaction counters before sending; they are stale after
reset, and waiting on them otherwise never finishes.
