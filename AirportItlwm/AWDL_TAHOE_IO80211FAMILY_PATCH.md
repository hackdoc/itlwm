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

The four-byte sequence is not unique in the binary, so an OpenCore patch must
use a larger function-local anchor. The 115-byte prefix from the function
start through the compare immediate is unique in the current x86_64 slice
(`count = 1`). The generated OpenCore entry is:

```text
Identifier: com.apple.iokit.IO80211Family
Arch: x86_64
MinKernel: 25.0.0
MaxKernel: ""
Count: 1
Find:    554889e541574156534883ec18488b057c7a1c00488b00488945e0b81600000083791820754f4989ce488b49204885c974434889f3488b8620010000488bb8d80000004885ff744c4c8d7ddf41c6070041b820000000bec8692880ba5e0000004d89f9e868bbf0ff41f6070174223d0f2882e0
Replace: 554889e541574156534883ec18488b057c7a1c00488b00488945e0b81600000083791820754f4989ce488b49204885c974434889f3488b86200100004885ff744c4c8d7ddf41c6070041b820000000bec8692880ba5e0000004d89f9e868bbf0ff41f6070174223dc20200e0
Mask:    fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
```

The `Mask` above is all `ff` and has the same length as `Find` (115 bytes).
Do not shorten the `Find` to the four-byte immediate: that sequence occurs
437 times in this binary.

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
