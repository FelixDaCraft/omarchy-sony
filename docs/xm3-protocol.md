# WH-1000XM3 — MDR v1 protocol map

Everything below was captured from a real **Sony WH-1000XM3, firmware 4.5.2**
over RFCOMM. It is the reference for `daemon/src/MDRProtocol.cpp`.

The XM3 speaks MDR protocol **v1**. The XM4/XM5 speak **v2**: same framing,
different opcodes and inquired types. Payloads for the wrong version are simply
ACKed and then ignored — the headset never answers and never errors, which is
why a v2 client looks like it connects and then does nothing.

## Transport

| | |
|---|---|
| Service UUID | `96cc203e-5068-46ad-b32d-e316f5e069ba` (legacy Sony MDR) |
| v2 UUID | `956c7b26-d49a-4ba8-b03f-b17d393cb6e2` — **not advertised by the XM3** |
| RFCOMM channel | `15` on this unit; resolve over SDP, do not hardcode |

Framing is unchanged from v2: `0x3E <type> <seq> <len:4 BE> <payload> <csum> 0x3C`,
with `0x3E/0x3D/0x3C` escaped as `0x3D 0x2E / 0x3D 0x2D / 0x3D 0x2C`, and an
8-bit additive checksum over the unescaped header + payload.

## Handshake — mandatory

The XM3 answers **nothing** until `CONNECT_GET_PROTOCOL_INFO` opens the session.
Send it first on every RFCOMM connection.

```
>  00 00
<  01 00 40 10
```

Each inbound DATA frame must be ACKed promptly (type `0x01`, seq `1 - rx_seq`,
empty payload) or the headset retransmits three times and then drops the session.

## Verified commands

| Feature | GET | Response | Notes |
|---|---|---|---|
| Protocol info | `00 00` | `01 00 40 10` | mandatory handshake |
| Model name | `04 01` | `05 01 0a "WH-1000XM3"` | `05 01 <len> <ascii>` |
| Firmware | `04 02` | `05 02 05 "4.5.2"` | |
| Battery | `10 00` | `11\|13 00 <level> <charging>` | v2's `22/23` is ignored |
| Noise control | `66 02` | `67\|69 02 …` | see below; v2's `66 17` is ignored |
| Equalizer | `56 01` | `57\|59 01 <preset> 06 <cb> <b0..b4>` | inquired type **`01`**, not `00` |
| DSEE HX | `e6 01` | `e7\|e9 01 00 <value>` | value is at payload\[3], not \[2] |

### Noise control (NCASM, inquired type `0x02`)

```
SET  68 02 <effect> 02 <ncDualSingle> 01 <asmId> <asmLevel>
RET  67 02 <effect> 02 <ncDualSingle> 01 <asmId> <asmLevel>
NTFY 69 02 …
```

* `effect` — **`0x11` (ADJUSTMENT_COMPLETION) on every "on" SET**, `0x00` for off.
  Sending `0x01` is accepted and silently does nothing: that is the single
  easiest way to get a client that appears to work but never changes anything.
  The headset *reports* `0x01`/`0x00`.
* `ncDualSingle` — `0x02` DUAL (noise cancelling), `0x00` OFF (ambient sound).
* `asmId` — `0x00` normal, `0x01` focus on voice.
* `asmLevel` — `0..20`. Reported in every mode, so the last ambient level
  survives a trip through ANC or Off.

Mode mapping used by the daemon:

| Mode | effect | ncDualSingle | asmLevel |
|---|---|---|---|
| `anc` | `0x11` | `0x02` | `0x00` |
| `ambient` | `0x11` | `0x00` | `1..20` |
| `off` | `0x00` | `0x00` | `0x00` |

There is **no wind-noise-reduction mode** on the XM3 (that is XM4/XM5). Ambient
level 0 is just the quietest ambient setting.

### Equalizer (EQEBB, inquired type `0x01`)

```
SET preset  58 01 <preset> 00
SET custom  58 01 a0 06 <clearBass> <b0> <b1> <b2> <b3> <b4>
RET         57 01 <preset> 06 <clearBass> <b0..b4>
```

Band bytes are offset by +10, so `0x0a` is 0 dB and the range `0x00..0x14` maps
to −10..+10. Selecting a preset makes the headset report that preset's bands,
e.g. Bright (`0x10`) returns `09 0a 0f 11 11 13`.

## Not supported by the XM3

These inquired types are ACKed and never answered, and the daemon does not send
them:

| Feature | Command | Why |
|---|---|---|
| Speak-to-Chat | `f6 0c` | XM4/XM5 feature |
| Wearing detection | `f6 01` | not exposed on this model |
| Multipoint | `d6 d1` | answers `d7 d1 02 00`, but this is not multipoint on the XM3 — the XM3 has none |
| Battery (v2) | `22 00` | v2 opcode |
| NC/ASM (v2) | `66 17` | v2 inquired type |

Other types that do answer but are not used by the plugin: `e6 02`
(`e7 02 00 01`), `f6 04` (`f7 04 01 00 00`), `d6 d2` (`d7 d2 01 01`),
`46 01` (`47 01 00`), `86 01` (`87 01 01 01 01 0a`).
