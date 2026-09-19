# AWDL data-plane next step

## Current boundary

The Skywalk/IO80211 control path is partially wired, but AWDL management/data
TX is intentionally not connected. `requestPacketTx()` must not call the
ordinary Ethernet `outputPacket()` path: that path queues an mbuf on the STA
ifnet and cannot report per-packet AWDL completion.

`TxPacketRequest` is an ABI structure containing up to eight mbufs. The
ownership rule is not documented by the local headers, so the driver currently
leaves the virtual-interface queue untouched. `outputActionFrame()` now
explicitly drops and frees only valid, owned mbufs and returns
`kIOReturnOutputDropped`; it never forwards an action frame to the STA queue.

## Next implementation step (planned, not implemented here)

1. Instrument a development build around `dequeueTxPackets()` to record
   `pkt_cnt`, mbuf addresses, lengths, and return values without submitting to
   firmware.
2. Confirm whether dequeue transfers ownership and whether
   `reportTransmitStatus()` or `dropTxPacket()` performs the release.
3. Add a dedicated AWDL management-TX adapter for iwm/iwx. It must validate
   the AWDL role, packet count (maximum eight), packet headers, and frame type,
   then submit through the firmware management queue and complete each mbuf
   exactly once.
4. Add a short off-channel/time-event reservation only after the adapter works
   on the current channel. Do not reuse the normal STA Ethernet queue.

## Bluetooth coexistence follow-up

Before changing `BT_CONFIG`, collect iwm/iwx capability and runtime state:
MPLUT support, shared-antenna availability, BT priority, SCO/BLE activity,
and management-TX delay/drop counters. Use these observations to choose AWDL
protected-period/priority policy; do not claim AWDL Bluetooth scheduling from
ordinary Wi-Fi coexistence alone.
