# Tahoe V2 AWDL creation reverse-engineering notes

This note is based on the x86_64 `IO80211Family` loaded on Tahoe 25G313
(`1200.13.1`). No EFI or system binary is modified by this note.

## Confirmed native flow

`IO80211Controller::attachVirtualInterface(vif**, mac, role, enable)` at
`0x1130aa` does the following:

1. Calls the controller vtable's `createVirtualInterface(mac, role)` slot.
2. If the returned object is null, returns false immediately.
3. Calls a virtual object check at vtable offset `0xdd8` with the controller
   as its argument. This is the attach/readiness gate; it is not merely an
   `IOService::attach` call.
4. If the check fails, releases the object and returns false.
5. Stores the object into the caller's slot.
6. If the final boolean is true, calls the object's vtable slot `0x5b0`
   with argument zero. This is the native enable/start step.

Therefore a successful `new + init` is not sufficient. A Tahoe AWDL object
must pass the V2 Skywalk readiness gate and then be enabled by the native
attach helper.

## Confirmed Tahoe object initialization

`IO80211VirtualInterface::init(controller, mac, role, name)` at `0x1ad5f8`:

- calls `IO80211SkywalkInterface::init(controller, mac)`;
- allocates its V2 expansion data;
- calls `setInterfaceRole(role)`;
- allocates its lock and per-interface expansion objects;
- registers the interested driver with the controller;
- creates the interface property dictionary;
- initializes the V2 flow-queue database.

For role `6`, `IO80211VirtualInterface::createPeerManager()` selects the
native `IO80211AWDLPeerManager::withInterface(vif, mac)` factory. This means
the generic `IO80211VirtualInterface` class is capable of selecting AWDL's
peer manager, but only after its Skywalk initialization and attach gate have
completed.

## Parent interface finding

`IO80211SkywalkInterface::setParentInterface(vif)` at `0x1767a2` only stores
the pointer at expansion-data offset `0x68`. It does not attach, register, or
allocate the AWDL object. It must be treated as a post-creation relationship,
not as a replacement for `attachVirtualInterface`.

## Current driver mismatch to resolve

The driver currently creates a generic `IO80211VirtualInterface`, calls its
`init`, and passes it to `attachVirtualInterface`. The observed
`kIOReturnNoMemory` (`0xe00002bd`) after the IO80211Family fallback is
therefore most likely the V2 readiness gate or the subsequent native enable
slot failing, not actual heap exhaustion.

The next safe experiment must instrument or reproduce the native readiness
gate without pre-creating an interface during driver startup. In particular,
we need to identify the vtable method at `0xdd8` and the method at `0x5b0`
from the Tahoe class layout before changing the driver.

## Safety boundary

Do not deploy another AWDL kext until the native gate and enable slots are
identified. The stable EFI state is the original Tahoe V2 kext with no AWDL
OpenCore patch; its Tahoe binary hash is recorded in the task history.

## Vtable calibration warning

The offsets used by `attachVirtualInterface` are raw vtable offsets, not
source-level method numbers. A first symbol-table comparison shows that
blindly labeling `vtable + 0xdd8` as a named readiness method is unsafe: the
current `IO80211VirtualInterface` vtable layout contains inherited Skywalk
slots, reserved slots, and ABI-specific entries. The address observed at the
corresponding raw slot resolves near
`IO80211SkywalkInterface::destroyEventPipe`, which has an incompatible
source-level signature if interpreted directly. This means the vtable base,
subobject adjustment, or slot mapping still needs calibration.

The next reverse-engineering step is therefore to recover the complete
`IO80211VirtualInterface` vtable start and compare the raw slot sequence with
the class declarations before assigning names to `0xdd8` or `0x5b0`.
