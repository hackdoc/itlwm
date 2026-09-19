#!/bin/bash
set -u
OUT="${1:-$PWD/awdl-logs-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
exec > >(tee "$OUT/collector.log") 2>&1
echo "# AWDL/Intel diagnostic bundle $(date -u +%FT%TZ)"
{
  echo '## host'; sw_vers; uname -a
  echo '## interfaces'; ifconfig awdl0 2>&1 || true; ifconfig 2>&1
  echo '## IO80211/Skywalk'; ioreg -l -p IO80211Plane 2>&1 || true
  ioreg -l -p IOService -n AirportItlwm 2>&1 || true
  echo '## wireless'; system_profiler SPAirPortDataType 2>&1 || true
  echo '## loaded kexts'; kmutil showloaded 2>&1 | grep -Ei 'itlwm|airport|bluetooth|80211' || true
} > "$OUT/system.txt"
# Keep only the relevant processes and a bounded interval to avoid collecting
# unrelated user data. Run this while reproducing AWDL activity.
log show --last 10m --style compact --info --debug \
  --predicate '(process == "airportd" OR process == "sharingd" OR process == "bluetoothd" OR process == "kernel") AND (eventMessage CONTAINS[c] "AWDL" OR eventMessage CONTAINS[c] "AirDrop" OR eventMessage CONTAINS[c] "AirPlay" OR eventMessage CONTAINS[c] "IO80211" OR eventMessage CONTAINS[c] "Itlwm" OR eventMessage CONTAINS[c] "Bluetooth" OR eventMessage CONTAINS[c] "coex")' \
  > "$OUT/awdl.log" 2>&1 || true
printf 'Diagnostics saved to %s\n' "$OUT"
