# itlwm

**An Intel Wi-Fi Adapter Kernel Extension for macOS, based on the OpenBSD Project.**

## Documentation

We highly recommend exploring our documentation before using this Kernel Extension:

- [Intro](https://OpenIntelWireless.github.io/itlwm)
- [Compatibility](https://openintelwireless.github.io/itlwm/Compat)
- [FAQ](https://openintelwireless.github.io/itlwm/FAQ)

## AirportItlwm on macOS 26

The `AirportItlwm-Tahoe` target integrates the native IO80211 WCL interface.
It uses the existing PCI device table and the iwn, iwm, and iwx backends; it does
not add support for devices or radio features absent from those backends.
The implementation includes scan results, association and link events, private
MAC synchronization, firmware lifecycle notifications, and beacon-based link
quality updates. Native ABI declarations are isolated in `include/Airport/macOS26`;
the new behavior is compiled only with `AIRPORT_WCL`.

Runtime validation so far covers Intel Wireless-AC 7260. Other devices require
hardware validation; compilation is not evidence of working Wi-Fi on every card.
HE/MLO extended BSS reporting, WPA3-only authentication, AWDL, and wake-on-wireless
are not advertised as implemented by this integration.

WPA2/WPA3-Personal transition networks with optional PMF are presented to WCL
using their advertised WPA2-PSK option. The original received RSN IE is retained
by net80211 for handshake validation. This does not implement SAE or enable
connections to WPA3-only networks or networks requiring PMF.

Build with Xcode and MacKernelSDK at commit
`3f750085caa17ec3a7880f11c11bf4f48cd6a164` checked out in `MacKernelSDK`:

```sh
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build
```

The `AirportItlwm (all)` scheme also builds this target. CI compiles itlwm and all
nine AirportItlwm targets in Debug and Release, with separate artifacts for each
system. Every push to `main` publishes an alpha prerelease after both configurations
pass. The rolling `v<version>-alpha` tag points to the published commit; obsolete
assets and older prereleases are removed after upload, while stable releases are
retained. Packages use the upstream naming format, for example
`AirportItlwm-Tahoe-v2.4.0-RELEASE-alpha-<commit>.zip`.
Pull requests only build and upload CI artifacts.

## Download

[![Download from https://github.com/OpenIntelWireless/itlwm/releases](https://img.shields.io/github/v/release/OpenIntelWireless/itlwm?label=Download)](https://github.com/OpenIntelWireless/itlwm/releases)

## Questions and Issues

Check out our [FAQ Page](https://openintelwireless.github.io/itlwm/FAQ) for more info.

If you have other questions or feedback, feel free to [![Join the chat at https://gitter.im/OpenIntelWireless/itlwm](https://badges.gitter.im/OpenIntelWireless/itlwm.svg)](https://gitter.im/OpenIntelWireless/itlwm?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge).

We only accept bug reports in GitHub Issues, before opening an issue, you're recommended to reconfirm it with us on [Gitter](https://gitter.im/OpenIntelWireless/itlwm); once it's confirmed, please use the provided issue template.

## Credits

- [Acidanthera](https://github.com/acidanthera) for [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)
- [Apple](https://www.apple.com) for [macOS](https://www.apple.com/macos)
- [AppleIntelWiFi](https://github.com/AppleIntelWiFi) for [Black80211-Catalina](https://github.com/AppleIntelWiFi/Black80211-Catalina)
- [ErrorErrorError](https://github.com/ErrorErrorError) for UserClient bug fixes
- [Intel](https://www.intel.com) for [Wireless Adapter Firmwares](https://www.intel.com/content/www/us/en/support/articles/000005511/network-and-io/wireless.html) and [iwlwifi](https://wireless.wiki.kernel.org/en/users/drivers/iwlwifi)
- [Linux](https://www.kernel.org) for [iwlwifi](https://wireless.wiki.kernel.org/en/users/drivers/iwlwifi)
- [mercurysquad](https://github.com/mercurysquad) for [Voodoo80211](https://github.com/mercurysquad/Voodoo80211)
- [OpenBSD](https://openbsd.org) for [net80211, iwn, iwm, and iwx](https://github.com/openbsd/src)
- [pigworlds](https://github.com/OpenIntelWireless/itlwm/commits?author=pigworlds) for DVM devices support, MIRA bug fixes, and Tx aggregation for MVM Gen 1 devices
- [rpeshkov](https://github.com/rpeshkov) for [black80211](https://github.com/rpeshkov/black80211)
- [usr-sse2](https://github.com/usr-sse2) for implementing the usage of Apple RSN Supplicant and bug fixes
- [zxystd](https://github.com/zxystd) for developing [itlwm](https://github.com/OpenIntelWireless/itlwm)

## Acknowledgements

- [@penghubingzhou](https://github.com/startpenghubingzhou)
- [@Bat.bat](https://github.com/williambj1)
- [@iStarForever](https://github.com/XStar-Dev)
- [@stevezhengshiqi](https://github.com/stevezhengshiqi)
- [@DogAndPot](https://github.com/DogAndPot) for providing resources and help for system configuration
- [@Daliansky](https://github.com/Daliansky) for providing Wi-Fi cards
