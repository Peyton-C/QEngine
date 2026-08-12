# Denon DJ MC6000MK2

Engine mapping for the MC6000MK2, installed into the RMZ2 control-surface slot
by [`controllermap.sh`](../../controllermap.sh). Source for every number:
*MC6000MK2 MIDI COMMAND LIST* (inMusic, rev. 2015-04-27).

## Decks 3 and 4, not 1 and 2

This mapping listens on the MC6000MK2's **decks 3 and 4** (MIDI channels 3 and
4, `0x02`/`0x03` zero-based) and its **CH3/CH4 mixer strips** — the outer two
channel faders. Channel 1's input-source selector on this unit is dead and
never reports its position, so channel 1 is avoided entirely; mixer channel N
carries deck N's audio, which is why the deck choice drags the strip choice
with it.

**Press DECK CHG. 3 on the left deck and DECK CHG. 4 on the right before
using it.** Nothing in the file can force that switch — on decks 1/2 the
surface is silent.

To move to another pair, change `deckMidiChannel` in the deck `ListModel` and
the CC/note numbers in the mixer `ListModel`; both are single blocks near the
end of the file.

## What each control does

Engine's own two decks are named Left/Right and its two mixer channels 1/2
throughout the file — those are Engine identities, not MC6000MK2 labels.

| MC6000MK2 control | Engine function |
| --- | --- |
| PLAY / CUE | play, cue (shift+cue = backtrack) |
| SYNC | sync (hold = instant double) |
| KEY LOCK | key lock |
| VINYL MODE | **slip** |
| CENSOR | censor |
| BEND + / − | pitch bend |
| AUTO LOOP | auto loop on/off |
| LOOP CUT − / + | halve / double the loop |
| HOT CUE 1–4 | **pad mode select** (not hot cues) |
| SAMP. 1–4 | **pads 1–4** in the selected pad mode |
| SHIFT | shift |
| TRACK SELECT knob + push | browse / load (shift+push unloads) |
| LIST / AREA / PANEL | Browse / Source / Menu views |
| CH3, CH4 strips: trim, 3-band EQ, fader, CUE | mixer channels 1 and 2 |
| FILTER ON (L/R) + FILTER knob | sweep FX per channel |
| CROSS FADER, X FADER CONTOUR | crossfader and contour |
| MASTER, BOOTH, PAN, PHONES | master, booth, cue mix, cue gain |
| MIC ON 1 / 2, MIC EQ HIGH/LOW | mic on, mic EQ |
| ECHO ON 1 + ECHO knob | mic FX on, mic FX depth |
| EFX 1–3 (FX1 **and** FX2) | the six DJ FX select slots |
| EFX1 knob (FX1) | FX wet/dry |
| BEATS button + knob (FX1) | FX time |
| DECK ASSIGN 3 / 4 (FX1) | route FX onto mixer channel 1 / 2 |

The pad row is relabelled because `ActionPads` needs eight *consecutive* notes
and the hot cue buttons are not consecutive (`0x17 0x18 0x19 0x20`). Driving
the pads from SAMP. 1–4 and the mode from HOT CUE 1–4 is the only arrangement
that reaches every pad mode. Hot cues are still available — as pads, in the
hot cue pad mode.

## Not mapped

| Control | Why |
| --- | --- |
| Jog wheels / platter | The RMZ2 is motorized and takes timecode, not MIDI jog data. Use VINYL MODE (slip) instead. |
| PITCH slider | Sent as Pitch Bend (`0xEn`); `SpeedSlider` only accepts a pair of CCs, so it cannot be expressed. |
| LOOP IN / OUT | No manual-loop component in Engine's vocabulary. |
| MIC MID knobs | `MicEq` exposes high and low only. |
| FX2 knobs / BEATS / TAP, DECK ASSIGN 1–2 | Engine has one FX unit; FX2's buttons are folded into FX select. |
| VIDEO on/knobs, X-F LINK, DUCKING, INPUT SOURCE, LOAD A/B, FWD/BCK, VIEW, SAMP. SELECT | No counterpart, or no free component to hang them on. |
| All LED and VU feedback | Engine emits RMZ2's protocol (note/SysEx), the MC6000MK2 expects its own CC triggers. Expect dark buttons. The stray messages are ignored by the hardware. |

## Assumptions worth verifying

The command list documents only "n = MIDI CH = 0 to 3" — it never states which
section transmits on which channel. Two things are inferred:

1. **Deck-section controls arrive on the selected deck's channel** (`0x02` /
   `0x03`). The alternative reading is that the firmware allocates two channels
   per side (left = `0x00`/`0x01`, right = `0x02`/`0x03`), which is what the
   sheet's meter table hints at — its meters are CH1→`0xB0`, CH2→`0xB2`,
   CH3→`0xB1`, CH4→`0xB3`. If the whole left deck is silent while the right
   works, try `0x01` for the left deck.
2. **Everything else arrives on MIDI channel 1** (`0x00`): mixer strips,
   master/booth/cue, mic, FX, browse. The mixer CCs are unique per strip, so
   all four strips can share one channel.

The most likely single casualty is the TRACK SELECT knob: it sits in the centre
browse section, so it may well transmit on `0x00` while `BrowseEncoder` — a
per-deck component — listens on the deck channels.

Confirm any of this with a capture on the unit rather than guessing:

```sh
aseqdump -l                  # find the controller's port
aseqdump -p <client>:<port>  # turn the control, read the channel
```

`aseqdump` prints channels 1-based; the numbers in the QML are zero-based.

## Applying it

`controllermap.sh` matches on USB vid:pid from [`../../manifest`](../../manifest),
which has no entry for this controller yet — the MC6000MK2's product id has to
be read off the unit first:

```sh
controllermap.sh --list      # with the controller plugged in
```

then add `<vid:pid> denon-mc6000mk2 Denon DJ MC6000MK2` to the manifest and run
`controllermap.sh`. Engine reads assignments when it binds the surface, so
restart `engine.service` afterwards.