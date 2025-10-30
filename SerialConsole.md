# Serial Console Command Reference

This document defines the SCPI-style serial command interface for configuring and monitoring the PiratESP32 FM RDS Stereo Encoder.

Protocol
- Set: `GROUP:ITEM <value>`
- Get: `GROUP:ITEM?` → `OK ITEM=<value>`
- Status: `GROUP:STATUS?` → grouped `key=value` fields
- Replies: `OK` on success; `ERR <code> <message>` on failure
- Values
  - Strings in quotes; supports escapes `\"` and `\n`
  - Hex numbers `0x####`; decimals by default
  - Booleans accept `0|1` (and `ON|OFF` for set); replies use `0|1`
- JSON Mode (optional): `SYST:COMM:JSON ON|OFF`, `SYST:COMM:JSON?` (see JSON Mode Details)

Notes
- Case-insensitive commands and items: `rds:pi`, `RDS:PI`, and `RDS:Pi` are equivalent.
- Logging can interleave with replies; reduce log level while scripting (see System & Configuration).

Examples
- `RDS:PI 0x52A1` → `OK`
- `RDS:PI?` → `OK PI=0x52A1`
- `RDS:PS "PiratESP"` → `OK`
- `RDS:STATUS?` → `OK PI=0x52A1,PTY=10,TP=1,TA=0,MS=1,PS="PiratESP",RT="...",RTAB=A,ENABLE=1`

---

## RDS (Core)

- `RDS:PI <hex|dec>` / `RDS:PI?`
  - Example: `RDS:PI 0x52A1`

- `RDS:PTY <0-31|NAME>` / `RDS:PTY?`
  - Accepts numeric code or standard PTY name (e.g., `POP_MUSIC`).
  - Example: `RDS:PTY 10`

- `RDS:PTY:LIST?`
  - Returns the mapping list of PTY names and codes. Also accepts `RDS:PTY LIST?`.
  - Example:
    - `OK 0=NONE,1=NEWS,2=CURRENT_AFFAIRS,3=INFORMATION,4=SPORT,5=EDUCATION,6=DRAMA,7=CULTURE,8=SCIENCE,9=VARIED,10=POP_MUSIC,11=ROCK_MUSIC,12=EASY_LISTENING,13=LIGHT_CLASSICAL,14=SERIOUS_CLASSICAL,15=OTHER_MUSIC,16=WEATHER,17=FINANCE,18=CHILDREN,19=SOCIAL_AFFAIRS,20=RELIGION,21=PHONE_IN,22=TRAVEL,23=LEISURE,24=JAZZ_MUSIC,25=COUNTRY_MUSIC,26=NATIONAL_MUSIC,27=OLDIES_MUSIC,28=FOLK_MUSIC,29=DOCUMENTARY,30=ALARM_TEST,31=ALARM`

- `RDS:TP <0|1>` / `RDS:TP?`
- `RDS:TA <0|1>` / `RDS:TA?`
- `RDS:MS <0|1>` / `RDS:MS?`

- `RDS:PS "<text>"` / `RDS:PS?`
  - Station name; padded/truncated to 8 chars.
  - Example: `RDS:PS "PiratESP"`

- `RDS:RT "<text>"` / `RDS:RT?`
  - RadioText broadcast (up to 64 characters).
  - Example: `RDS:RT "Artist • Title"`

- `RDS:ENABLE <0|1>` / `RDS:ENABLE?`
  - Enables/disables the 57 kHz RDS subcarrier injection.

- `RDS:STATUS?`
  - `OK PI=...,PTY=...,TP=...,TA=...,MS=...,PS="...",RT="...",RTAB=...,ENABLE=...`

---

## RadioText Rotation

Behavior: if more than one item in the RTLIST, the items are rotated following RTPERIOD.

- `RDS:RTLIST:ADD "<text>"`
- `RDS:RTLIST:DEL <idx>`
- `RDS:RTLIST:CLEAR`
- `RDS:RTLIST?`
  - `OK 0="...",1="...",2="..."`
- `RDS:RTPERIOD <sec>` / `RDS:RTPERIOD?`
  - Rotation period in seconds.

Notes
- On rotation advance, broadcast RT is set (64c, A/B toggled) and the display marquee updates at wrap (no mid‑scroll change).

---

## Pilot and Audio

- `AUDIO:STEREO <0|1>` / `AUDIO:STEREO?`
  - Controls the 38 kHz L−R subcarrier. Pilot (19 kHz) is controlled separately by `PILOT:ENABLE`.

- `PILOT:ENABLE <0|1>` / `PILOT:ENABLE?`
  - Enables/disables the 19 kHz stereo pilot tone.

- `PILOT:AUTO <0|1>` / `PILOT:AUTO?`
  - Auto‑mute pilot after sustained silence. Affects pilot only; L−R remains enabled.

- `PILOT:THRESH <float>` / `PILOT:THRESH?`
  - Silence RMS threshold (e.g., `0.001` ≈ −60 dBFS).

- `PILOT:HOLD <ms>` / `PILOT:HOLD?`
  - Silence hold time before auto‑mute. (e.g., 2000 ms)

- `AUDIO:PREEMPH <0|1>` / `AUDIO:PREEMPH?`
  - Enables/disables 50 µs pre‑emphasis per configuration.

- `AUDIO:STATUS?`
  - `OK STEREO=1,PREEMPH=1`


---

## System & Configuration


- `SYST:CONF:SAVE [name]`
- `SYST:CONF:LOAD [name]`
- `SYST:CONF:LIST?`
- `SYST:CONF:ACTIVE?`
- `SYST:CONF:DELETE "<name>"`
- `SYST:CONF:DEFAULT`
  - Reset all settings to factory defaults.

Profiles Notes
- `SYST:CONF:LIST?` reply keys:
  - Text mode: `OK RTLIST="name1,name2"` (historical key; treat as the list of profile names)
  - JSON mode: `{ "ok": true, "data": { "LIST": ["name1", ...] } }`

- `SYST:DEFAULTS`
  - Alias for `SYST:CONF:DEFAULT`. Reset all settings to factory defaults.

- `SYST:VERSION?`
  - `OK VERSION=...,BUILD=YYYYMMDD,BUILDTIME=YYYY-MM-DDThh:mm:ssZ`

- `SYST:STATUS?`
  - `OK UPTIME=...,CPU=...,CORE0=...,CORE1=...,HEAP_FREE=...,HEAP_MIN=...,STEREO=1,AUDIO_CLIPPING=0`

- `SYST:HEAP?`
  - `OK CURRENT_FREE=...,MIN_FREE=...`

- `SYST:REBOOT`

- `SYST:HELP?` | `SYST:HELP RDS` | `SYST:HELP AUDIO` | `SYST:HELP PILOT` | `SYST:HELP SYST`
  - Returns a concise list of available items for each group.

- `SYST:PIPELINE:RESET`
  - Soft reset of the audio pipeline (flushes states and DMA, restores pilot amplitude).

- `SYST:HELP?` | `SYST:HELP RDS` | `SYST:HELP AUDIO` | `SYST:HELP SYST`

- `SYST:LOG:LEVEL OFF|ERROR|WARN|INFO|DEBUG` / `SYST:LOG:LEVEL?`
  - `OFF` mutes background logs (deferred until end of startup); other levels unmute and set threshold

- `SYST:COMM:JSON ON|OFF` / `SYST:COMM:JSON?`

---

Response Formats
- Text mode
  - Success: `OK` or `OK KEY=VALUE[,KEY=VALUE...]`
  - Error: `ERR <code>` (message optional)
- JSON mode
  - Success: `{"ok":true,"data":{...}}`
  - Error: `{"ok":false,"error":{"code":"...","message":"..."}}`

Implementation Notes
 - For booleans, the parser accepts `ON|OFF` and `0|1`; replies use `0|1`.
 - Length limits: PS=8 chars; broadcast RT=64 chars; rotation items and UI marquee can be long.

---

## JSON Mode Details

Enable / Disable
- `SYST:COMM:JSON ON` → subsequent replies are single-line JSON
- `SYST:COMM:JSON OFF` → switch back to text mode
- `SYST:COMM:JSON?` → `OK JSON=1|0`

Envelope
- Success: `{ "ok": true, "data": { ... } }`
- Error: `{ "ok": false, "error": { "code": "...", "message": "..." } }`

Keys and Types
- Keys mirror text-mode names (e.g., `PI`, `PTY`, `PS`, `RT`, `ENABLE`).
- Numbers use JSON numeric types; strings as JSON strings; booleans returned as `0|1` numbers unless otherwise noted.
- Lists are arrays where applicable (e.g., `RTLIST`).

Examples
```
> RDS:PI 0x52A1
< {"ok":true}

> RDS:PI?
< {"ok":true,"data":{"PI":"0x52A1"}}

> RDS:STATUS?
< {"ok":true,"data":{"PI":"0x52A1","PTY":10,"TP":1,"TA":0,"MS":1,
                       "PS":"PiratESP","RT":"Artist • Title …","RTAB":"A","ENABLE":1}}

> RDS:RTLIST?
< {"ok":true,"data":{"RTLIST":["Item A","Item B","Item C"]}}

> AUDIO:STATUS?
< {"ok":true,"data":{"STEREO":1,"PREEMPH":1}}

> SYST:VERSION?
< {"ok":true,"data":{"VERSION":"1.2.3","BUILD":"20251021",
                      "BUILDTIME":"2025-10-21T12:34:56Z"}}

> SYST:STATUS?
< {"ok":true,"data":{"UPTIME":3600,"CPU":45.2,"CORE0":92,"CORE1":22,
                      "HEAP_FREE":65536,"HEAP_MIN":32768,
                      "STEREO":1,"AUDIO_CLIPPING":0}}

> RDS:PI
< {"ok":false,"error":{"code":"MISSING_ARG","message":""}}
```

---

## PTY Codes (EU RDS)

The firmware uses the European RDS PTY list. Use `RDS:PTY <code|NAME>` to set and `RDS:PTY?` to query. The `RDS:PTY:LIST?` command returns the same mapping in a compact form.

```
0=NONE,1=NEWS,2=CURRENT_AFFAIRS,3=INFORMATION,4=SPORT,5=EDUCATION,6=DRAMA,7=CULTURE,8=SCIENCE,9=VARIED,
10=POP_MUSIC,11=ROCK_MUSIC,12=EASY_LISTENING,13=LIGHT_CLASSICAL,14=SERIOUS_CLASSICAL,15=OTHER_MUSIC,
16=WEATHER,17=FINANCE,18=CHILDREN,19=SOCIAL_AFFAIRS,20=RELIGION,21=PHONE_IN,22=TRAVEL,23=LEISURE,
24=JAZZ_MUSIC,25=COUNTRY_MUSIC,26=NATIONAL_MUSIC,27=OLDIES_MUSIC,28=FOLK_MUSIC,29=DOCUMENTARY,30=ALARM_TEST,31=ALARM
```

Logging Interleaving
- JSON mode affects command replies only; background logs remain plain text.
- To avoid mixing logs with JSON replies, reduce log level while scripting: `SYST:LOG:LEVEL WARN`.

Schema (inline)
- The following JSON Schema defines the reply envelope and common payloads. Clients can
  validate by checking `ok` and, if true, matching `data` to the expected shape.

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://piratesp32.local/schemas/serial-console-response.json",
  "title": "Serial Console Response",
  "type": "object",
  "properties": {
    "ok": { "type": "boolean" },
    "data": { "type": "object" },
    "error": {
      "type": "object",
      "properties": {
        "code": { "type": "string" },
        "message": { "type": "string" }
      },
      "required": ["code", "message"],
      "additionalProperties": false
    }
  },
  "required": ["ok"],
  "oneOf": [
    { "required": ["ok", "data"], "properties": { "ok": { "const": true } } },
    { "required": ["ok", "error"], "properties": { "ok": { "const": false } } }
  ],
  "additionalProperties": false,
  "$defs": {
    "RdsStatus": {
      "type": "object",
      "properties": {
        "PI": { "type": ["string", "integer"] },
        "PTY": { "type": "integer", "minimum": 0, "maximum": 31 },
        "TP": { "type": "integer", "enum": [0,1] },
        "TA": { "type": "integer", "enum": [0,1] },
        "MS": { "type": "integer", "enum": [0,1] },
        "PS": { "type": "string" },
        "RT": { "type": "string" },
        "RTAB": { "type": "string", "enum": ["A","B"] },
        "ENABLE": { "type": "integer", "enum": [0,1] }
      },
      "additionalProperties": true
    },
    "RtList": {
      "type": "object",
      "properties": {
        "RTLIST": {
          "type": "array",
          "items": { "type": "string" }
        }
      },
      "required": ["RTLIST"],
      "additionalProperties": false
    },
    "AudioStatus": {
      "type": "object",
      "properties": {
        "STEREO": { "type": "integer", "enum": [0,1] },
        "PREEMPH": { "type": "integer", "enum": [0,1] }
      },
      "additionalProperties": true
    },
    "SystemVersion": {
      "type": "object",
      "properties": {
        "VERSION": { "type": "string" },
        "BUILD": { "type": "string" },
        "BUILDTIME": { "type": "string" }
      },
      "additionalProperties": false
    },
    "SystemStatus": {
      "type": "object",
      "properties": {
        "UPTIME": { "type": "integer", "minimum": 0 },
        "CPU": { "type": "number" },
        "CORE0": { "type": "number" },
        "CORE1": { "type": "number" },
        "HEAP_FREE": { "type": "integer", "minimum": 0 },
        "HEAP_MIN": { "type": "integer", "minimum": 0 },
        "STEREO": { "type": "integer", "enum": [0,1] },
        "AUDIO_CLIPPING": { "type": "integer", "enum": [0,1] }
      },
      "additionalProperties": true
    },
    "HeapStatus": {
      "type": "object",
      "properties": {
        "CURRENT_FREE": { "type": "integer", "minimum": 0 },
        "MIN_FREE": { "type": "integer", "minimum": 0 }
      },
      "required": ["CURRENT_FREE","MIN_FREE"],
      "additionalProperties": false
    }
  }
}
```
