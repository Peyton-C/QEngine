// Denon DJ MC6000MK2
// Channels 3 and 4 as RANE's main channels
//
// ---------------------------------------------------------------------------
// DELIBERATELY UNMAPPED (see README.md in this directory for the full table)
//
//   Jog wheels / platters — the RMZ2 is motorized and takes timecode, not MIDI
//       jog data. Use VINYL MODE (mapped to Slip) instead.
//   PITCH slider     — sent as Pitch Bend (0xEn); SpeedSlider only accepts a
//                      pair of CCs, so there is no way to express it here.
//   LOOP IN/OUT      — manual looping has no counterpart component; auto-loop
//                      and the pads cover looping.
//   VU meters, all LED feedback — Engine emits these in RMZ2's protocol, which
//       the MC6000MK2 does not understand. Expect dark buttons; the stray
//       messages are ignored by the hardware.

import airAssignments 1.0
import ControlSurfaceModules 0.1
import QtQuick 2.12
import Planck 1.0

MidiAssignment {
	objectName: 'Denon MC6000MK2 Controller Assignment'

	Utility {
		id: util
	}

	// ASSUMPTION (2): the non-deck sections all transmit on MIDI channel 1.
	GlobalAssignmentConfig {
		id: globalConfig
		midiChannel: 0x00
	}

	GlobalAction {
		id: globalAction
	}

	// Browse-section buttons. The MC6000MK2 has no LEDs on these, so hasLed is
	// false — nothing to light, and it keeps useless feedback off the wire.
	Navigation {
		navigationButtonsModel: ListModel {
			ListElement {
				name: 'Source'
				shiftName: ''
				note: 0x4D // AREA
				hasLed: false
			}
			ListElement {
				name: 'Browse'
				shiftName: ''
				note: 0x65 // LIST
				hasLed: false
			}
			ListElement {
				name: 'Menu'
				shiftName: 'CycleView'
				note: 0x64 // PANEL
				hasLed: false
			}
		}
	}

	// cueMixCC is the monitor PAN knob and cueGainCC the PHONES knob — the
	// MC6000MK2's headphone section has no separate cue/master blend control.
	// No cue-split or crossfader-reverse button exists on this unit.
	Mixer {
		crossfaderCC: 0x16        // CROSS FADER (AUDIO)
		crossfaderContourCC: 0x45 // X FADER CONTOUR
		masterCC: 0x19            // MASTER LEVEL VR
		boothCC: 0x1B             // BOOTH LEVEL VR
		cueMixCC: 0x43            // PAN VR
		cueGainCC: 0x44           // PHONES VR
	}

	MicOnToggles {
		mic1Note: 0x26 // MIC ON 1
		mic2Note: 0x27 // MIC ON 2
	}

	// Only high and low are assignable; the MC6000MK2's MIC MID knobs
	// (CC 0x23 / 0x33) have no counterpart property.
	MicEq {
		mic1HighCC: 0x21
		mic1LowCC: 0x22
		mic2HighCC: 0x31
		mic2LowCC: 0x32
	}

	MicFx {
		onNote: 0x44   // ECHO ON 1
		depthCC: 0x1C  // ECHO VR
	}

	///////////////////////////////////////////////////////////////////////////
	// FX
	//
	// Engine has one assignable DJ FX unit, the MC6000MK2 has two. FX1 drives
	// it; FX2's EFX buttons are folded in as three more FX selects so all six
	// select slots are reachable, but FX2's knobs and BEATS control are left
	// unmapped — there is no second unit for them to drive.

	FxAssignmentConfig {
		id: fxConfig
		midiChannel: 0x00
		channelNames: ['Channel1', 'Channel2']
	}

	DJFxSelect {
		selectType: DJFxSelect.AssignableButton
		buttonsModel: ListModel {
			Component.onCompleted: {
				const types = [
					{'note': 0x15}, // EFX1 SW (FX1)
					{'note': 0x12}, // EFX2 SW (FX1)
					{'note': 0x13}, // EFX3 SW (FX1)
					{'note': 0x55}, // EFX1 SW (FX2)
					{'note': 0x52}, // EFX2 SW (FX2)
					{'note': 0x53}, // EFX3 SW (FX2)
				]
				for(const type of types) {
					append({'note': type.note})
				}
			}
		}
	}

	// FX1's DECK ASSIGN 3 / 4 buttons route the FX unit onto Engine's two
	// mixer channels. Unlike the RMZ2's paddles these are plain buttons on one
	// MIDI channel, so the two entries are distinguished by note, and the model
	// order is what pairs them with channelNames above.
	SwitchableFxActivate {
		hasLeds: false
		activateControlsModel: ListModel {
			ListElement {
				midiChannel: 0x00
				note: 0x5A // DECK ASSIGN 3 (FX1)
			}
			ListElement {
				midiChannel: 0x00
				note: 0x5B // DECK ASSIGN 4 (FX1)
			}
		}
	}

	DJFxWetDry {
		cc: 0x55 // EFX1 KNOB (FX1) — absolute 0x00-0x7F
	}

	// BEATS KNOB is an endless encoder sending 0x00 to increment and 0x7F to
	// decrement. If FX time ends up scrolling the wrong way or not at all, this
	// encoding is the first thing to check against the RMZ2's convention.
	DJFxTime {
		pushNote: 0x40 // BEATS (FX1)
		turnCC: 0x58   // BEATS KNOB (FX1)
	}

	///////////////////////////////////////////////////////////////////////////
	// Decks
	//
	// ASSUMPTION (1): deck-section controls arrive on the selected deck's
	// channel. Change deckMidiChannel here to move to another DECK CHG. pair.

	Repeater {
		model: ListModel {
			ListElement {
				deckName: 'Left'
				deckMidiChannel: 0x02 // DECK CHG. 3
				shiftNote: 0x60       // SHIFT (DECK LEFT)
			}

			ListElement {
				deckName: 'Right'
				deckMidiChannel: 0x03 // DECK CHG. 4
				shiftNote: 0x61       // SHIFT (DECK RIGHT)
			}
		}

		Item {
			objectName: 'Deck %1'.arg(model.deckName)

			DeckAssignmentConfig {
				id: deckConfig
				name: model.deckName
				midiChannel: model.deckMidiChannel
			}

			DeckAction {
				id: deckAction
			}

			PlayCue {
				playNote: 0x43 // PLAY
				cueNote: 0x42  // CUE
				cueShiftAction: Action.Backtrack
			}

			Sync {
				syncNote: 0x6B // SYNC
				syncHoldAction: Action.InstantDouble
			}

			KeyLock {
				note: 0x06 // KEY LOCK
			}

			// With no jog data reaching Engine, VINYL MODE is most useful as
			// the slip toggle rather than as a scratch-mode switch.
			Slip {
				note: 0x04 // VINYL MODE
			}

			Censor {
				note: 0x50 // CENSOR
			}

			PitchBend {
				plusNote: 0x0C  // BEND +
				minusNote: 0x0D // BEND -
			}

			// LOOP CUT -/+ halve and double the running loop.
			AutoLoop {
				onOffNote: 0x1D // AUTO LOOP
				halveNote: 0x69 // LOOP CUT -
				doubleNote: 0x6A // LOOP CUT +
			}

			Shift {
				note: model.shiftNote
			}

			// The pad section is relabelled: the four HOT CUE buttons pick the
			// pad *mode* and the four SAMP buttons are the pads themselves.
			// ActionPads needs eight consecutive notes and the MC6000MK2's hot
			// cue notes are not consecutive (0x17 0x18 0x19 0x20), so this is
			// the only arrangement that reaches every pad mode. Printed labels
			// will not match; the README has the table.
			PadModeSelect {
				buttonsModel: ListModel {
					ListElement { note: 0x17 } // HOT CUE1
					ListElement { note: 0x18 } // HOT CUE2
					ListElement { note: 0x19 } // HOT CUE3
					ListElement { note: 0x20 } // HOT CUE4
				}
			}

			// Pads 1-4 are SAMP.1-4 (0x21-0x24). Pads 5-8 fall on 0x25-0x28,
			// which the deck sections do not use, so they are simply dead —
			// except 0x28, the TRACK SELECT push below. That only collides if
			// the browse knob turns out to transmit per-deck rather than on
			// 0x00; if it does, one of the two has to move.
			ActionPads {
				firstPadNote: 0x21
				ledType: LedType.Simple
			}

			// The TRACK SELECT knob is physically in the centre browse section
			// and may well transmit on channel 0x00 rather than per-deck — the
			// one control most likely to need a capture. If it lands on 0x00,
			// browsing will not respond to either deck.
			BrowseEncoder {
				pushNote: 0x28 // TRACK SELECT KNOB SW
				turnCC: 0x54   // TRACK SELECT KNOB
				doubleTapAction: Action.InstantDouble
				unloadTrackViaShiftBrowseEncoder: true
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Mixer channels
	//
	// Engine's channels 1 and 2 are wired to the MC6000MK2's CH3 and CH4
	// strips. All four strips share one MIDI channel and are told apart by CC
	// number, so mixerChannelMidiChannel is 0x00 for both.

	Repeater {
		model: ListModel {
			ListElement {
				mixerChannelName: '1'
				mixerChannelMidiChannel: 0x00
				deckName: 'Left'
				pflNote: 0x05    // CUE MIXER CH3
				trimCC: 0x0C     // INPUT LEVEL (CH3)
				trebleCC: 0x0D   // EQ HIGH VR (CH3)
				midCC: 0x0E      // EQ MID VR (CH3)
				bassCC: 0x0F     // EQ LOW VR (CH3)
				faderCC: 0x10    // FADER (CH3)
				filterCC: 0x66   // FILTER (L) KNOB
				filterNote: 0x16 // FILTER ON (L)
			}
			ListElement {
				mixerChannelName: '2'
				mixerChannelMidiChannel: 0x00
				deckName: 'Right'
				pflNote: 0x07    // CUE MIXER CH4
				trimCC: 0x11     // INPUT LEVEL (CH4)
				trebleCC: 0x12   // EQ HIGH VR (CH4)
				midCC: 0x13      // EQ MID VR (CH4)
				bassCC: 0x14     // EQ LOW VR (CH4)
				faderCC: 0x15    // FADER (CH4)
				filterCC: 0x67   // FILTER (R) KNOB
				filterNote: 0x1E // FILTER ON (R)
			}
		}

		Item {
			objectName: 'Mixer Channel %1'.arg(model.mixerChannelName)

			MixerChannelAssignmentConfig {
				id: mixerChannelConfig
				name: model.mixerChannelName
				midiChannel: model.mixerChannelMidiChannel
			}

			MixerChannelCore {
				pflNote: model.pflNote
				trimCC: model.trimCC
				trebleCC: model.trebleCC
				midCC: model.midCC
				bassCC: model.bassCC
				faderCC: model.faderCC
			}

			// The FILTER knob and its ON button drive Engine's sweep FX. L is
			// the left deck's strip, R the right one.
			SweepFxKnob {
				cc: model.filterCC
			}

			SweepFxActivate {
				note: model.filterNote
			}
		}
	}

	// Kept from the vendor mapping: this is the Computer/Hybrid Mode relay to
	// the f_midi-0 USB gadget, unrelated to which controller is attached.
	HighSpeedForwarder {
		master: device
		targetDevice: "f_midi-0"
		deviceCollection: MidiDevices
	}
}