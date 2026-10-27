.. _ttfw_device_events:

Device Events
=============

This page describes how firmware tells the host kernel module (KMD) that something
happened on the device, such as an uncorrectable error. Firmware appends a record to an
event ring in CSM, then sends a doorbell interrupt.

The implementation lives in ``lib/tenstorrent/bh_arc/device_events.c``.

Doorbell
--------

After it publishes an event, firmware sends PCIe MSI vector 0 on the host-facing PCIe
tile, the same interrupt that ``tt_pcie_log`` uses for log batches. The MSI carries no
payload. It only means "look for something new": one MSI may cover zero, one or many
events, and the host must tolerate finding nothing.

Event ring
----------

Telemetry tag ``TAG_EVENT_RING_ADDR`` (86) holds the CSM address of the ring: a 32-byte
header followed by ``num_entries`` records. A missing tag, or a value of 0, means the
firmware does not publish events.

.. list-table:: Header
   :header-rows: 1
   :widths: 20 15 65

   * - Field
     - Type
     - Meaning
   * - ``magic``
     - ``uint32_t``
     - ``0x54455652`` ("TEVR")
   * - ``version``
     - ``uint32_t``
     - 1
   * - ``entry_size``
     - ``uint32_t``
     - 32
   * - ``num_entries``
     - ``uint32_t``
     - 64 (a power of 2)
   * - ``wptr``
     - ``uint32_t``
     - Sequence number the next record will get. Written by firmware; 0 at boot.
   * - ``rptr``
     - ``uint32_t``
     - Sequence number of the next record the host will read. Written by the host.
   * - ``overflow``
     - ``uint32_t``
     - Records dropped because the ring was full. Written by firmware; wraps at 2^32.
   * - ``reserved``
     - ``uint32_t``
     - 0

.. list-table:: Record
   :header-rows: 1
   :widths: 20 15 65

   * - Field
     - Type
     - Meaning
   * - ``seq``
     - ``uint32_t``
     - Sequence number of this record
   * - ``type``
     - ``uint16_t``
     - Event type. 0x0000 to 0x7FFF are firmware types; 0x8000 and up belong to the KMD.
   * - ``source``
     - ``uint16_t``
     - For an event raised by a tile, its translated NOC coordinates packed as in
       ``NOC_NODE_ID``: ``x | y << 6``. Otherwise type-specific.
   * - ``cause``
     - ``uint32_t``
     - Type-specific subcode
   * - ``data[3]``
     - ``uint32_t``
     - Type-specific data
   * - ``timestamp``
     - ``uint64_t``
     - ARC reference clock since boot: 50 MHz, so one tick is 20 ns

Firmware types: ``0x0000`` is TEST. ``0x0001`` (EVENTS_SUPPRESSED, with data[0] the
suppressed type and data[1] the count) is reserved for future rate limiting.
Firmware types are defined in ``device_events.h``, which is their only registry: the KMD
passes them through without interpreting them, and host software keeps its own copy.

This is a producer/consumer ring. ``wptr`` and ``rptr`` are free-running, and record
``s`` is stored in entry ``s % num_entries``. Records ``[rptr, wptr)`` are unread.

Firmware appends a record by writing every field including ``seq``, then, after a
barrier, ``wptr = s + 1``. Firmware never waits for the host: if ``wptr - rptr`` is
already ``num_entries``, the ring is full and firmware drops the new record and
increments ``overflow`` instead. An ``rptr`` that is not within ``num_entries`` of
``wptr`` is treated the same way. Firmware code appends with ``event_ring_append()``,
or ``event_ring_append_no_doorbell()`` followed by one ``event_doorbell()`` for a batch.
All three may be called from any context.

The host reads entries ``[rptr, wptr)``, then writes ``rptr = wptr`` to release them.
Because firmware never writes an entry before the host has released it, the host's
reads cannot be torn. An increase in ``overflow`` tells the host how many records were
dropped. A chip reset clears CSM, so the ring always starts empty.

Test message
------------

This message exists so that the host path can be tested without a real error source.
It accepts only the test type and returns ``EINVAL`` (22) for anything else, without
changing state.

``TT_SMC_MSG_TEST_EVENT_RECORD`` (``0x91``), ``struct test_event_record_rqst``:

* The request carries ``type`` (bytes 2-3), ``source`` (bytes 4-5), ``repeat``
  (bytes 6-7), ``cause`` (word 2) and ``data[3]`` (words 3 to 5).
* ``repeat`` must be 0 to 1024. That many records are appended, followed by one doorbell.
  With 0, only the doorbell is sent, so the host can test an MSI that brings nothing new.
* Record ``i`` (0 to ``repeat - 1``) has ``data[2]`` set to the requested ``data[2] + i``,
  so a single record round-trips every field and a batch shows order and loss.
* Records that do not fit are dropped and counted in ``overflow``, so a ``repeat`` above
  ``num_entries`` keeps the first ``num_entries`` records of the batch when nobody reads
  in between.
* Response word 1 is ``wptr`` after the append.
