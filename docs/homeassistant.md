# Home Assistant integration

HiveInside broadcasts measurements in its compact HiveHub manufacturer-data
beacon. It is not a BTHome device, so Home Assistant does not auto-discover it.
The connectable custom GATT service is reserved for firmware updates and does
not expose measurement characteristics.

## Recommended path: via HiveHub

HiveHub passively scans the node, decodes its climate, vibration, acoustic, and
battery fields, and forwards them to the backend. Surface those values in Home
Assistant through the ecosystem's API or MQTT integration. This preserves the
sensor's connectionless, low-power measurement path and requires no additional
BLE connection.

## Direct BLE experimentation

A local scanner can receive the manufacturer record, but must implement the
HiveInside binary frame described in the firmware README. Generic BTHome and
GATT sensor integrations will not decode it. Prefer the HiveHub path for normal
deployments so there is one authoritative decoder and device identity mapping.

## Range and placement

BLE from inside a wooden hive attenuates somewhat. Keep HiveHub within roughly
10–15 m, or co-locate it on the same stand.
