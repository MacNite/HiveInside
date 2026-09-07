/* throughput.h — TEMPORARY BLE notification-throughput measurement (issue #71,
 * phase 0e). Not part of the shipping feature set; see docs/ble-throughput-spike.md.
 *
 * The audio feature in issue #71 has to move a recording from the node to the
 * relay, i.e. peripheral -> central *notifications* from RAM. Nothing in this
 * firmware does that today, so nothing measures it either. The only number we
 * have is the OTA relay's ~1-1.5 kB/s, and that path is a poor proxy for three
 * reasons: it is central -> peripheral, every chunk is a write *with response*
 * (two connection intervals minimum), and ota.c writes RRAM synchronously
 * inside the ATT callback, where each 512-byte flush runs in a radio timeslot
 * that pre-empts connection events. An audio stream does none of those things.
 *
 * So this module answers one question and then gets deleted: with this SoC,
 * this Zephyr configuration and a given central, how many bytes per second can
 * we notify out of RAM? It exposes a GATT service that blasts a counter pattern
 * for a requested number of seconds and reports what it managed to send, and it
 * logs the negotiated link parameters (interval, PHY, data length) that decide
 * the answer.
 *
 * The whole module compiles to nothing unless ENABLE_THROUGHPUT_SPIKE is set,
 * which throughput-spike.conf does through CONFIG_COMPILER_OPT. A normal build
 * — bring-up or low-power — is byte-identical with this file present.
 */
#pragma once
