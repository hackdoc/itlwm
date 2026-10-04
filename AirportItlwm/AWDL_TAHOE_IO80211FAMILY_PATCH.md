# Tahoe AWDL V2 `IO80211Family` patch point

This note records the statically verified patch point for the x86_64 Tahoe
`IO80211Family` currently present on the test machine (`25G313`,
`IO80211Family 1200.13.1`). It is intentionally documentation only; it does
not modify the system kext or the EFI.

## Function and control flow

The relevant function is:

```text
__ZL20setVIRTUAL_IF_CREATEP17IO80211ControllerP23IO80211SkywalkInterfaceP20IO80211APIUserClientP13apple80211req
```

At x86_64 file/VM offset `0x1056a8`, Apple sends IOC `0x5e` to WCL. The
fallback decision is:

```asm
testb  $0x1,(%r15)             ; WCL handled flag
je     fallback
cmpl   $0xe082280f,%eax        ; WCL pass-through sentinel
je     fallback
return WCL result
```

On the current machine the observed result is `0xe00002c2`
(`kIOReturnBadArgument`) with the handled flag set. That is why the call never
reaches the driver's `setVIRTUAL_IF_CREATE` implementation.

## Candidate minimal patch

The immediate operand of the `cmpl` at VM/file offset `0x105717` is:

```text
old: 0f 28 82 e0       ; 0xe082280f
new: c2 02 00 e0       ; 0xe00002c2
```

This preserves the existing branch structure and makes the observed WCL
failure fall through to Apple's already-implemented call to:

```text
apple80211setVIRTUAL_IF_CREATE(interface, request)
```

The patch is x86_64- and build-specific. It must not be applied to arm64e or
to another Tahoe build without re-disassembling and re-validating the same
function and bytes.

## Required validation after applying through OpenCore

1. The system must still boot with the unmodified kext available as rollback.
2. `wifip2pd` must stop reporting `-536870206` for VIF creation.
3. The driver must log `setVIRTUAL_IF_CREATE` and then `attachVirtualInterface`.
4. Only after that should `awdl0` and the AWDL data path be debugged.

This patch only changes dispatch. It does not implement AWDL synchronization,
peer management, channel scheduling, or the Bluetooth handshake.
