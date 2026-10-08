// Denon DJ MC6000MK2, as the Prime 4 G2's (JP24) control surface.
// Decks 3 and 4, and the CH3/CH4 strips, as Engine's Left/Right decks and mixer
// channels 1 and 2 -- the same choice, for the same reason, as this directory's
// RMZ2 mapping. See README.md.
//
// Mixer and deck-button numbers are from that RMZ2 mapping, which was written
// from the MC6000MK2 MIDI command list. Everything marked "captured" was read
// off a real unit with aseqdump, and corrects two things the list did not say:
//
//   - which MIDI channel each deck sends on. Left is 0x00 (deck 1) or 0x01
//     (deck 3); right is 0x02 (deck 2) or 0x03 (deck 4). Not 1-2-3-4 in order.
//   - the SAMP. buttons do not follow DECK CHG. They always send on the side's
//     first channel (0x00 left, 0x02 right), whichever deck is selected.
//
// Needs midisurface started with
//
//   --forward MC6000MK2 --pitchbend-cc 0x05,0x06 --relative-cc 0x51=0x37,0x4D
//   --note-map 0:0x28=0x70
//   --note-map 1:0x62=0:0x62 --note-map 2:0x63=0:0x63 --note-map 3:0x63=0:0x63
//
// for the controls an assignment file cannot describe as the unit sends them:
// the pitch faders (Pitch Bend), the jog wheels (relative ticks), the
// browse-encoder push (the same note as the left deck's eighth pad) and the LOAD
// buttons (sent on the deck's channel, wanted on the global one). Each is noted
// where it is used below; midisurface.c explains the translations.
//
// ---------------------------------------------------------------------------
// DELIBERATELY UNMAPPED
//
//   LOOP IN/OUT, AUTO LOOP, the CH1/CH2 strips, FX -- the Prime 4 G2 has
//       components for these, but with different shapes or numbers that have
//       not been captured.
//   Deck layers -- DECK CHG. changes the MIDI channel the deck section sends on,
//       which Engine's DeckSelect cannot follow. Stay on decks 3 and 4.
//   Microphones -- the G2 mixes them in software and the MC6000MK2 in hardware.
//   Stems, playlist banks, media slots -- no matching controls.
//   VU meters, all LED feedback -- Engine emits these in the G2's protocol, and
//       midisurface forwards one way only.

import airAssignments 1.0
import InputAssignment 0.1
import OutputAssignment 0.1
import ControlSurfaceModules 0.1
import QtQuick 2.12
import Planck 1.0

MidiAssignment {
	objectName: 'Denon MC6000MK2 as PRIME 4 MKII Controller Assignment'
	id: assignment

	Utility {
		id: util
	}

	// The MC6000MK2's non-deck sections all transmit on MIDI channel 1.
	GlobalAssignmentConfig {
		id: globalConfig
		midiChannel: 0x00
	}

	GlobalAction {
		id: globalAction
	}

	// Captured: the browse section sends on channel 0x00, not per deck. The
	// encoder sends 0x00 per click one way and 0x7F the other. BCK and FWD sit
	// below it and take the joystick's left and right.
	BrowseEncoderJoystick {
		pushNote: 0x70  // TRACK SELECT KNOB SW: sends 0x28, moved by --note-map
		leftNote: 0x30  // BCK
		rightNote: 0x29 // FWD
		turnCC: 0x54    // TRACK SELECT KNOB
	}

	Mixer {
		midiChannel: 0x00
		crossfaderCC: 0x16 // CROSS FADER (AUDIO)
		masterCC: 0x19     // MASTER LEVEL VR
		boothCC: 0x1B      // BOOTH LEVEL VR
		cueMixCC: 0x43     // PAN VR
		cueGainCC: 0x44    // PHONES VR
	}

	// The MC6000MK2 has no LEDs on these.
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
				shiftName: ''
				note: 0x64 // PANEL
				hasLed: false
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Decks
	//
	// Deck-section controls arrive on the selected deck's channel. Change
	// deckMidiChannel here to move to another DECK CHG. pair -- and the pad
	// block below with it, if the pair is not on these two sides' decks.

	Repeater {
		model: ListModel {
			ListElement {
				deckName: 'Left'
				deckMidiChannel: 0x01 // DECK CHG. 3 (captured)
				shiftNote: 0x60       // SHIFT (DECK LEFT)
				loadNote: 0x62        // LOAD, left (captured)
			}
			ListElement {
				deckName: 'Right'
				deckMidiChannel: 0x03 // DECK CHG. 4 (captured)
				shiftNote: 0x61       // SHIFT (DECK RIGHT)
				loadNote: 0x63        // LOAD, right (captured)
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
				cueShiftAction: Action.SetCuePoint
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

			Shift {
				note: model.shiftNote
			}

			// The four HOT CUE buttons pick the pad mode and the four SAMP
			// buttons are pads 1-4; see README.md for why they are relabelled.
			PadModeSelect {
				buttonsModel: ListModel {
					ListElement { note: 0x17 } // HOT CUE1
					ListElement { note: 0x18 } // HOT CUE2
					ListElement { note: 0x19 } // HOT CUE3
					ListElement { note: 0x20 } // HOT CUE4
				}
			}

			// Engine listens for Load on the global channel, whichever deck it
			// belongs to, and the MC6000MK2 sends it on the deck's own. The
			// --note-map lines in the header move it across.
			Load {
				note: model.loadNote
			}

			// Captured: the platter top sends note 0x51 on touch, and turning
			// sends CC 0x51 as a relative count around 64 (65 forward, 63
			// back). Engine wants an absolute position as a 14-bit pair, which
			// is what --relative-cc turns it into; the pair and the starting
			// jogSensitivity are the original Prime 4's. Tune jogSensitivity
			// here if the platter feels too quick or too slow.
			JogWheel {
				touchNote: 0x51
				ccUpper: 0x37
				ccLower: 0x4D
				jogSensitivity: 1638.7 * 3.6
			}

			// The fader sends Pitch Bend; midisurface re-sends it as this pair.
			SpeedSlider {
				ccUpper: 0x05
				ccLower: 0x06
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Pads
	//
	// A second block per deck, because the SAMP. buttons send on a different
	// channel from the rest of the deck section: always the side's first
	// channel, whatever DECK CHG. says. Pads 5-8 fall on 0x25-0x28, which the
	// deck sections do not send, so they are simply dead.
	//
	// The G2's pads are quad pads -- each can be split into four -- and report
	// where they were struck through a pair of CCs. A plain button has no
	// position to report, so every press lands wherever Engine puts a pad with
	// none.

	Repeater {
		model: ListModel {
			ListElement {
				deckName: 'Left'
				padMidiChannel: 0x00 // captured
			}
			ListElement {
				deckName: 'Right'
				padMidiChannel: 0x02 // captured
			}
		}

		Item {
			objectName: 'Deck %1 Pads'.arg(model.deckName)

			DeckAssignmentConfig {
				id: deckConfig
				name: model.deckName
				midiChannel: model.padMidiChannel
			}

			DeckAction {
				id: deckAction
			}

			QuadPads {
				firstPadNote: 0x21 // SAMP.1
				ledType: LedType.Simple
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Mixer channels
	//
	// All four MC6000MK2 strips share one MIDI channel and are told apart by CC
	// number, so mixerChannelMidiChannel is 0x00 for both.

	Repeater {
		model: ListModel {
			ListElement {
				mixerChannelName: '1'
				mixerChannelMidiChannel: 0x00
				pflNote: 0x05  // CUE MIXER CH3
				trimCC: 0x0C   // INPUT LEVEL (CH3)
				trebleCC: 0x0D // EQ HIGH VR (CH3)
				midCC: 0x0E    // EQ MID VR (CH3)
				bassCC: 0x0F   // EQ LOW VR (CH3)
				faderCC: 0x10  // FADER (CH3)
				filterCC: 0x66 // FILTER (L) KNOB
			}
			ListElement {
				mixerChannelName: '2'
				mixerChannelMidiChannel: 0x00
				pflNote: 0x07  // CUE MIXER CH4
				trimCC: 0x11   // INPUT LEVEL (CH4)
				trebleCC: 0x12 // EQ HIGH VR (CH4)
				midCC: 0x13    // EQ MID VR (CH4)
				bassCC: 0x14   // EQ LOW VR (CH4)
				faderCC: 0x15  // FADER (CH4)
				filterCC: 0x67 // FILTER (R) KNOB
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

			SweepFxKnob {
				cc: model.filterCC
			}
		}
	}

	// Kept from the vendor mapping: the Computer/Hybrid Mode relay to the
	// f_midi-0 USB gadget, unrelated to which controller is attached.
	HighSpeedForwarder {
		master: device
		targetDevice: "f_midi-0"
		deviceCollection: MidiDevices
	}
}
