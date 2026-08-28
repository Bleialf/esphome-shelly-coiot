# esphome-shelly-coiot

**Push-basierte ESPHome-Komponente für Shelly Gen1 Energiezähler — ohne Polling, ohne konfigurierte IP.**

Ein ESPHome-`external_component`, das einem **Shelly 3EM / Shelly EM (Gen1)** zuhört,
statt ihn abzufragen. Die Werte kommen über **CoIoT** — das CoAP-basierte
Push-Protokoll, das Gen1-Shellys von sich aus sprechen.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Bleialf/esphome-shelly-coiot
      ref: main
    components: [shelly_coiot]

shelly_coiot:
  id: shelly3em
  mac: "A4CF12345678"

sensor:
  - platform: shelly_coiot
    shelly_coiot_id: shelly3em
    phase_a:
      power:
        name: "L1 Leistung"
    total_power:
      name: "Gesamtleistung"
```

Das ist die vollständige Konfiguration. Es gibt kein `update_interval`,
keine `ip_address:` und keinen HTTP-Request.

---

## Warum

Die üblichen Lösungen fragen den Shelly per HTTP in einem festen Intervall ab.
Das kostet Netzwerkverkehr, liefert Werte immer zu spät, und bricht sobald sich
die IP des Shelly ändert.

CoIoT dreht das um. Der Shelly schickt seinen Status unaufgefordert an die
Multicast-Gruppe `224.0.1.187:5683` — per Default alle 15 s und zusätzlich bei
jeder Änderung. Diese Komponente hört einfach zu.

Und weil jedes Paket seine Herkunft mitbringt, fällt die **Discovery als
Nebenprodukt ab**: die IP steht in der Absenderadresse, die Gerätezuordnung in
CoAP-Option 3332 (`SHEM-3#<MAC>#2`). Kein mDNS-Client, kein Subnetz-Scan, keine
DHCP-Reservierung. Du konfigurierst die MAC — einmal — und nie wieder eine
Adresse.

Nebeneffekt: das funktioniert auch dann noch, wenn der ESP selbst gar keine
Route zum Shelly hat. Zuhören reicht.

---

## Features

* **Kein Polling.** Reiner Multicast-Listener, kein HTTP.
* **Selbstfindend.** IP und Device-ID werden aus dem Paket gelesen und als
  Text-Sensoren in Home Assistant veröffentlicht.
* **Alle Messwerte des 3EM**, die CoIoT hergibt: pro Phase Leistung, Spannung,
  Strom, Leistungsfaktor, Energie bezogen und eingespeist.
* **Summen über alle Phasen** werden im ESP gerechnet — `total_power`,
  `total_energy`, `total_energy_returned`, letztere mit
  `state_class: total_increasing` für das HA-Energie-Dashboard.
* **Erreichbarkeitsprüfung.** Bleiben die Pakete aus, wird das Gerät als
  offline gemeldet und die Sensoren auf `unavailable` gesetzt statt still den
  letzten Wert einzufrieren.
* **Robuster Parser.** Handgeschriebener CoAP-Decoder und ein
  JSON-Teilparser für die `G`-Tupel, ohne ArduinoJson — spart Heap und
  Abhängigkeiten. `null`-Werte werden übersprungen statt als `0` gemeldet.

## Unterstützte Geräte

| Gerät | Status |
|-------|--------|
| Shelly 3EM (SHEM-3, Gen1) | vollständig |
| Shelly EM (SHEM, Gen1) | sollte funktionieren — dem EM fehlen Strom und Leistungsfaktor, `model: "SHEM"` setzen |
| andere Gen1-Shellys | der CoAP-Decoder passt, die Sensor-IDs in `sensor.py` nicht |
| Shelly Pro 3EM, EM Gen3, alles Gen2+ | **nein** — Gen2 hat kein CoIoT, dort geht RPC über WebSocket oder UDP |

Getestete Plattformen: ESP32 (esp-idf und Arduino) sowie ESP8266 (Arduino).
Auf ESP-IDF läuft ein lwip-BSD-Socket, unter Arduino `WiFiUDP`.

---

## Konfiguration

### Hub: `shelly_coiot`

| Option | Typ | Default | Bedeutung |
|--------|-----|---------|-----------|
| `mac` | MAC | — | Auf genau dieses Gerät filtern. Schreibweise egal (`AA:BB:...`, `aa-bb-...`, `AABB...`). Weglassen = erstes passendes Gerät gewinnt. |
| `model` | string | `SHEM-3` | Modell-Präfix der CoIoT-Device-ID. `""` akzeptiert jeden Gen1-Shelly. |
| `timeout` | Zeit | `60s` | Ohne Paket in dieser Zeit gilt das Gerät als offline. |
| `mark_unavailable_on_timeout` | bool | `true` | Sensoren beim Timeout auf `NaN` setzen, statt den letzten Wert stehen zu lassen. |

### Plattformen

`sensor` kennt `phase_a`, `phase_b`, `phase_c` mit je `power`, `voltage`,
`current`, `power_factor`, `energy`, `energy_returned`, dazu auf oberster Ebene
`total_power`, `total_energy`, `total_energy_returned`.

`binary_sensor` kennt `relay`, `overpower` und `online`.

`text_sensor` kennt `ip_address` und `device_id`.

Eine vollständige Gerätekonfiguration inklusive WireGuard-Anbindung liegt unter
[`example/shelly-3em-bridge.yaml`](example/shelly-3em-bridge.yaml).

---

## Am Shelly

Unter **Settings → Advanced - Developer Settings** muss **CoIoT enabled**
angehakt sein, und **CoIoT peer** muss leer sein oder `mcast` enthalten. Steht
dort eine IP, sendet der Shelly nur noch Unicast dorthin.

Filtert dein Netz Multicast weg (manche APs, VLAN-Grenzen sowieso), kannst du
umgekehrt die feste IP des ESP als CoIoT-Peer eintragen — Unicast-Pakete nimmt
die Komponente genauso entgegen. Dann ist allerdings die IP des ESP fix zu
vergeben, und der Discovery-Vorteil gilt nur noch für den Shelly.

Wenn du die MAC nicht kennst: `mac:` weglassen, Logger auf `DEBUG`. Die
Komponente loggt jede gesehene Device-ID.

---

## Sensor-IDs (SHEM-3)

Verifiziert gegen die `/cit/d`-Beschreibung, wie sie der ioBroker-Shelly-Adapter
und das openHAB-Shelly-Binding unabhängig voneinander hinterlegt haben.

| Kanal | power | energy | energyReturned | voltage | current | powerFactor |
|-------|-------|--------|----------------|---------|---------|-------------|
| L1 (`emeter_0`) | 4105 | 4106 | 4107 | 4108 | 4109 | 4110 |
| L2 (`emeter_1`) | 4205 | 4206 | 4207 | 4208 | 4209 | 4210 |
| L3 (`emeter_2`) | 4305 | 4306 | 4307 | 4308 | 4309 | 4310 |

Dazu `1101` (Relais) und `6102` (Überlast).

Blindleistung und Neutralleiterstrom liefert der 3EM über CoIoT **nicht** —
die gibt es nur per HTTP `/status`, und dafür müsste man wieder pollen.

Energiewerte kommen in Wh und werden auf kWh umgerechnet.

## Paketformat

```
CoAP-Header    ver=1, type=NON, code=0.30   ("publish status")
Option 3332    "SHEM-3#A4CF12345678#2"      Modell # MAC # CoIoT-Version
Option 3412    Gültigkeitsdauer (uint16)
Option 3420    Report-Seriennummer (uint16) — ändert sich nur bei Änderung
0xFF
Payload        {"G":[[0,4105,123.4],[0,4106,1000],[0,4108,231.5], ...]}
```

Jedes Tupel ist `[Kanal, Sensor-ID, Wert]`. Wiederholte Seriennummern werden
übersprungen — das spart auf einem ESP8266 spürbar Rechenzeit.

---

## Verifikation

* `esphome config` gegen ESPHome 2026.6.5 — valide.
* CoAP-Decoder und Payload-Parser laufen als Host-Build gegen ein
  handgebautes 3EM-Statuspaket (324 Byte, 21 Tupel). Alle Options-Nummern,
  Device-ID, Serial und Messwerte werden korrekt dekodiert.
* Der Parser wurde mit jeder Präfix-Länge des Pakets (0..324 Byte) und einer
  Reihe kaputter Payloads unter AddressSanitizer und UndefinedBehaviorSanitizer
  durchgejagt — kein Overrun, keine Endlosschleife.

## Quellen

* [CoIoT for Shelly devices (rev 1.0)](https://shelly-api-docs.shelly.cloud/gen1/docs/coiot/v1/CoIoT%20for%20Shelly%20devices%20(rev%201.0)%20.pdf) — offizielle Spezifikation
* [Shelly Gen1 API Reference](https://shelly-api-docs.shelly.cloud/gen1/)
* [ioBroker.shelly – shellyem3.js](https://github.com/iobroker-community-adapters/ioBroker.shelly/blob/v11.0.0/lib/devices/gen1/shellyem3.js)
* [openHAB Shelly Binding – Shelly1CoIoTVersion2.java](https://github.com/openhab/openhab-addons/blob/main/bundles/org.openhab.binding.shelly/src/main/java/org/openhab/binding/shelly/internal/api1/Shelly1CoIoTVersion2.java)

## Lizenz

MIT — siehe [LICENSE](LICENSE).
