// Pioneer DDJ-FLX10, as the Prime 4 G2's (JP24) control surface.
// Numbers are from the DDJ-FLX10 MIDI message list (AlphaTheta, 2023); those
// marked "captured" were read off a real unit with aseqdump.
//
// Needs midisurface started with
//
//   --forward DDJ-FLX10 --echo-lamps
//   --relative-cc 0x21=0x37,0x4D --relative-cc 0x22=0x37,0x4D --relative-cc 0x23=0x37,0x4D
//   --note-map 0:0x3C=6:0x50 --note-map 1:0x3C=6:0x51
//   --note-map 2:0x3C=6:0x52 --note-map 3:0x3C=6:0x53
//
// --echo-lamps because the FLX10 is lit by echoing its own notes, which is
// what Engine does; JP24_Controller_Device.qml turns Engine's values into the
// FLX10's. --relative-cc for the jog wheels, which count ticks on a CC per
// surface (side 0x21, platter top 0x22, and 0x23 for the top with vinyl mode
// off) where Engine wants one absolute position. --note-map for deck select:
// see the deck section.
//
// Three things about the FLX10 shape this file.
//
//   Shift is done in the hardware. Held, the FLX10 sends a different note for
//   most buttons (CUE 0x0C becomes 0x48), and the pads move to their own MIDI
//   channel. Engine wants the same note with its own shift state, which it
//   still gets from the SHIFT button (0x3F). So a control whose shifted action
//   matters is assigned twice, once on each note, and Engine applies shift
//   to whichever arrives.
//
//   So are pad modes. Each mode and page sends its own run of notes (hot cue
//   0x00 and 0x08, pad FX 1 0x10 and 0x18, ...), and lights by the same notes.
//   Engine picks the pad mode itself, so every run is assigned to the same
//   eight pads: whatever mode the FLX10 shows, its pads drive Engine's current
//   one, and the lamps go out on every run so the one on screen is right.
//
//   And deck layers. Decks 3 and 4 send on MIDI channels 3 and 4, where the
//   G2's Left and Right never change channel. So each side is assigned twice,
//   on both of its channels, and both drive whichever deck Engine has on that
//   side. The FLX10 says which deck a side is on with note 0x3C on the deck's
//   channel; --note-map moves those onto the global channel as DeckSelect's
//   notes, so Engine follows the hardware's DECK buttons.
//
// ---------------------------------------------------------------------------
// NOT MAPPED
//
//   Quantize, key sync, tempo reset, mix point -- no component to hang them
//       on, or not tried yet.
//   Beat FX select and channel select, mic, sampler -- not yet.
//   Jog screens -- the bridge in scripts/pi/flx10 draws them.

import airAssignments 1.0
import InputAssignment 0.1
import OutputAssignment 0.1
import ControlSurfaceModules 0.1
import QtQuick 2.12
import Planck 1.0

MidiAssignment {
	objectName: 'Pioneer DDJ-FLX10 as PRIME 4 MKII Controller Assignment'
	id: assignment

	Utility {
		id: util
	}

	// The browser section's channel (MIDI channel 7).
	GlobalAssignmentConfig {
		id: globalConfig
		midiChannel: 0x06
	}

	GlobalAction {
		id: globalAction
	}

	// Captured: turning sends CC 0x40, 1 clockwise and 127 the other way.
	// Pressing loads (see Load below), so the push here is SHIFT + press.
	// The FLX10 has no joystick. Its directions get notes nothing sends:
	// left at -1, every button on this channel opened the track info view.
	BrowseEncoderJoystick {
		pushNote: 0x5D  // BROWSE + SHIFT (captured)
		turnCC: 0x40    // BROWSE rotate (captured)
		leftNote: 0x54
		rightNote: 0x55
		upNote: 0x56
		downNote: 0x57
	}

	Back {
		note: 0x65 // BACK
	}

	View {
		note: 0x7A // VIEW
		shiftAction: Action.SwitchMainViewLayout
	}

	// 14-bit pairs; the MSB alone is plenty for these.
	Mixer {
		midiChannel: 0x06
		crossfaderCC: 0x1F // CROSSFADER (captured)
		masterCC: 0x08     // MASTER LEVEL
		boothCC: 0x09      // BOOTH LEVEL
		cueMixCC: 0x0C     // HEADPHONES MIX
		cueGainCC: 0x0D    // HEADPHONES LEVEL
	}

	// The SOUND COLOR FX buttons, as the G2's four sweep effects.
	SweepFxSelect {
		midiChannel: 0x06
		channelNames: ['1', '2', '3', '4']
		flashOnActive: true
		ledType: LedType.Simple
		buttonsModel: ListModel {
			Component.onCompleted: {
				const types = [
					{'note': 0x05, 'fxIndex': SweepEffect.DualFilter}, // FILTER
					{'note': 0x04, 'fxIndex': SweepEffect.NoiseSweep}, // NOISE
					{'note': 0x01, 'fxIndex': SweepEffect.Wash},       // D.ECHO
					{'note': 0x00, 'fxIndex': SweepEffect.Reverb},     // SPACE
				]
				for(const type of types) {
					append({'note': type.note, 'fxIndex': type.fxIndex})
				}
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Decks
	//
	// Each side twice, once per layer channel; see the header. DeckSelect and
	// its notes belong to the side, so only the first of each pair has one.

	Repeater {
		model: ListModel {
			ListElement {
				deckName: 'Left'
				deckMidiChannel: 0x00 // DECK 1
				loadNote: 0x46        // BROWSE press, deck 1 on the left (captured)
				selectNote: 0x50      // 0:0x3C, moved by --note-map
				deckIndex: 1
			}
			ListElement {
				deckName: 'Right'
				deckMidiChannel: 0x01 // DECK 2
				loadNote: 0x47        // BROWSE press, deck 2 on the right
				selectNote: 0x51      // 1:0x3C, moved by --note-map
				deckIndex: 2
			}
			ListElement {
				deckName: 'Left'
				deckMidiChannel: 0x02 // DECK 3
				loadNote: 0x48        // BROWSE press, deck 3 on the left
				selectNote: -1
				deckIndex: 3
			}
			ListElement {
				deckName: 'Right'
				deckMidiChannel: 0x03 // DECK 4
				loadNote: 0x49        // BROWSE press, deck 4 on the right
				selectNote: -1
				deckIndex: 4
			}
		}

		Item {
			objectName: 'Deck %1 (channel %2)'.arg(model.deckName).arg(model.deckMidiChannel + 1)

			DeckAssignmentConfig {
				id: deckConfig
				name: model.deckName
				midiChannel: model.deckMidiChannel
			}

			DeckAction {
				id: deckAction
			}

			// Engine's Load listens on the global channel, telling sides apart
			// by note, which is how the FLX10 sends it.
			Load {
				note: model.loadNote
			}

			PlayCue {
				playNote: 0x0B // PLAY/PAUSE
				cueNote: 0x0C  // CUE (captured)
				cueShiftAction: Action.SetCuePoint
				playLedType: LedType.Simple
			}

			PlayCue {
				playNote: 0x47 // PLAY/PAUSE + SHIFT
				cueNote: 0x48  // CUE + SHIFT (captured)
				cueShiftAction: Action.SetCuePoint
			}

			Sync {
				syncNote: 0x58   // BEAT SYNC
				masterNote: 0x5C // BEAT SYNC + SHIFT
				syncHoldAction: Action.InstantDouble
			}

			// Rekordbox puts master tempo on SHIFT + TEMPO RESET.
			KeyLock {
				note: 0x60 // TEMPO RESET + SHIFT
				ledType: LedType.Simple
			}

			// KEY SYNC as key lock, like the MC6000MK2's KEY LOCK: the module
			// holds for Engine's key sync and flashes the lamp while the key is
			// shifted. The long press (0x1A) is left alone; a press sends 0x65
			// either way. SHIFT is hardware here, so key reset is its own note.
			KeyLock {
				note: 0x65 // KEY SYNC
				ledType: LedType.Simple
			}

			ValueNoteAssignment {
				objectName: 'Key Reset Deck ' + deckConfig.name
				note: 0x64 // KEY SYNC + SHIFT
				channel: deckConfig.midiChannel
				enabled: deckConfig.inDJMode
				output: ActionOutput {
					target: PropertyTarget {
						path: deckConfig.activeDeckTrackPath + 'KeyReset'
					}
				}
			}

			Slip {
				note: 0x40 // SLIP
			}

			Censor {
				note: 0x15 // SLIP REVERSE
			}

			Shift {
				note: 0x3F // SHIFT (captured)
			}

			ManualLoop {
				inNote: 0x10  // LOOP IN
				outNote: 0x11 // LOOP OUT
			}

			// The G2's auto loop is an encoder; the FLX10 has buttons, which
			// the component also takes. Halve and double are on SHIFT + LOOP
			// IN/OUT, labelled 1/2X and 2X.
			AutoLoop {
				onOffNote: 0x14  // 4 BEAT/EXIT
				halveNote: 0x4C  // LOOP IN + SHIFT
				doubleNote: 0x4D // LOOP OUT + SHIFT
			}

			BeatJump {
				prevNote: 0x5E // 4 BEAT JUMP <
				nextNote: 0x5F // 4 BEAT JUMP >
			}

			TrackSkip {
				prevNote: 0x61 // 4 BEAT JUMP < + SHIFT
				nextNote: 0x62 // 4 BEAT JUMP > + SHIFT
			}

			// The G2's pad parameter arrows.
			Parameter {
				leftNote: 0x51  // CUE/LOOP CALL <
				rightNote: 0x53 // CUE/LOOP CALL >
			}

			// The ACTIVE PART buttons toggle their stems, as in rekordbox: the
			// stem pad mode's per-part toggles, and with SHIFT the part's FX.
			// INST is Melody and Bass together.
			//
			// Written out rather than StemsOverrides for the lamps, which the
			// FLX10 also shows as its jog ring colour. StemsOverrides sends 127
			// for on and 1 for off straight to MIDI, past the device file, and
			// the FLX10 lights at any non-zero value, so the lamps stayed lit.
			//
			// Plain concatenation and loops below: Engine pre-parses assignment
			// files, and a template string ending in a substitution made it reject
			// this whole file, leaving the controller dead.
			Repeater {
				model: ListModel {
					ListElement { note: 0x0D; parts: "Drums" }       // ACTIVE PART DRUMS
					ListElement { note: 0x0E; parts: "Vocal" }       // ACTIVE PART VOCAL
					ListElement { note: 0x0F; parts: "Melody,Bass" } // ACTIVE PART INST
				}

				Item {
					id: partButton
					readonly property int note: model.note
					readonly property var partList: model.parts.split(",")
					readonly property QObjProperty pStemmed: Planck.getProperty(deckConfig.activeDeckTrackPath + 'Stemmed')
					readonly property bool stemmed: pStemmed && pStemmed.translator && pStemmed.translator.state
					// BackgroundActive is what follows a part being muted (true while
					// it plays). StemsOverrides lights from <part>State, which stayed
					// true through every toggle.
					readonly property var pStates: {
						var list = []
						for (var i = 0; i < partList.length; i++)
							list.push(Planck.getProperty(deckConfig.activeDeckPath + 'Stems/' + partList[i] + 'BackgroundActive'))
						return list
					}
					readonly property bool active: {
						for (var i = 0; i < pStates.length; i++) {
							var p = pStates[i]
							if (p && p.translator && p.translator.state)
								return true
						}
						return false
					}

					Repeater {
						model: partButton.partList

						Item {
							ValueNoteAssignment {
								objectName: 'Stem %1 Deck %2'.arg(modelData).arg(deckConfig.name)
								note: partButton.note
								channel: deckConfig.midiChannel
								enabled: !globalConfig.shift && deckConfig.inDJMode && partButton.stemmed
								output: ActionOutput {
									target: PropertyTarget {
										path: deckConfig.activeDeckPath + 'Stems/' + modelData
									}
								}
							}

							ValueNoteAssignment {
								objectName: 'Stem %1 Fx Deck %2'.arg(modelData).arg(deckConfig.name)
								note: partButton.note
								channel: deckConfig.midiChannel
								enabled: globalConfig.shift && deckConfig.inDJMode && partButton.stemmed
								output: ActionOutput {
									target: PropertyTarget {
										path: deckConfig.activeDeckPath + 'Stems/' + modelData + 'Fx'
									}
								}
							}
						}
					}

					OutputAssignment {
						objectName: 'Stem LED %1 Deck %2'.arg(model.parts).arg(deckConfig.name)
						properties: { 'ledValue': '' }
						readonly property int ledValue: partButton.stemmed && partButton.active ? 127 : 0
						function send() {
							device.sendSimpleColor(deckConfig.midiChannel, partButton.note, ledValue)
						}
						Component.onCompleted: send()
					}
				}
			}

			// Captured: touch is note 0x36; turning sends CC 0x22 touched and
			// 0x21 by the side, ticks around 64. --relative-cc sums them into
			// the pair below. Smaller is slower: at the original Prime 4's
			// 1638.7 * 3.6 one flick travelled a third of a two-minute track,
			// 16 times that was worse, and a 32nd of it far too slow.
			JogWheel {
				touchNote: 0x36
				ccUpper: 0x37
				ccLower: 0x4D
				jogSensitivity: 1638.7 * 3.6 / 5
			}

			// Captured: a 14-bit pair, 0x00 then 0x20.
			SpeedSlider {
				ccUpper: 0x00
				ccLower: 0x20
			}

			PadModeSelect {
				ledType: LedType.Simple
				buttonsModel: ListModel {
					ListElement { note: 0x1B } // HOT CUE (captured)
					ListElement { note: 0x1E } // PAD FX 1 (captured)
					ListElement { note: 0x20 } // BEAT JUMP
					ListElement { note: 0x22 } // SAMPLER
				}
			}

			PadModeSelect {
				buttonsModel: ListModel {
					ListElement { note: 0x69 } // HOT CUE + SHIFT
					ListElement { note: 0x6B } // PAD FX 1 + SHIFT
					ListElement { note: 0x6D } // BEAT JUMP + SHIFT
					ListElement { note: 0x6F } // SAMPLER + SHIFT
				}
			}

			Loader {
				active: model.selectNote >= 0
				sourceComponent: DeckSelect {
					primaryDeckIndex: model.deckIndex
					primaryDeckNote: model.selectNote
				}
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Pads
	//
	// One block per pad channel: each deck's own, then each deck's shifted
	// one. Each block assigns both pages of all eight FLX10 pad modes to
	// Engine's eight pads; see the header. Only the unshifted blocks light.

	Repeater {
		model: ListModel {
			ListElement { deckName: 'Left';  padMidiChannel: 0x07; lit: true }  // DECK 1
			ListElement { deckName: 'Right'; padMidiChannel: 0x09; lit: true }  // DECK 2
			ListElement { deckName: 'Left';  padMidiChannel: 0x0B; lit: true }  // DECK 3
			ListElement { deckName: 'Right'; padMidiChannel: 0x0D; lit: true }  // DECK 4
			ListElement { deckName: 'Left';  padMidiChannel: 0x08; lit: false } // DECK 1 + SHIFT
			ListElement { deckName: 'Right'; padMidiChannel: 0x0A; lit: false } // DECK 2 + SHIFT
			ListElement { deckName: 'Left';  padMidiChannel: 0x0C; lit: false } // DECK 3 + SHIFT
			ListElement { deckName: 'Right'; padMidiChannel: 0x0E; lit: false } // DECK 4 + SHIFT
		}

		Item {
			id: padBlock
			objectName: 'Deck %1 Pads (channel %2)'.arg(model.deckName).arg(model.padMidiChannel + 1)
			readonly property bool lit: model.lit

			DeckAssignmentConfig {
				id: deckConfig
				name: model.deckName
				midiChannel: model.padMidiChannel
			}

			DeckAction {
				id: deckAction
			}

			// HOT CUE, PAD FX 1, BEAT JUMP, SAMPLER, KEYBOARD, PAD FX 2,
			// BEAT LOOP, KEY SHIFT: pages 1 and 2 of each. The FLX10 keeps a
			// page per mode per deck, and with page 1 alone a deck left on
			// page 2 had dead pads.
			Repeater {
				model: [0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
				        0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78]

				ActionPads {
					firstPadNote: modelData
					ledType: padBlock.lit ? LedType.RGB : LedType.None
				}
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Mixer channels
	//
	// Strip N is on MIDI channel N, as on the G2. Its COLOR knob is on the
	// effect section's channel instead, so it gets a second block there.

	Repeater {
		model: ListModel {
			ListElement { mixerChannelName: '1'; mixerChannelMidiChannel: 0x00 }
			ListElement { mixerChannelName: '2'; mixerChannelMidiChannel: 0x01 }
			ListElement { mixerChannelName: '3'; mixerChannelMidiChannel: 0x02 }
			ListElement { mixerChannelName: '4'; mixerChannelMidiChannel: 0x03 }
		}

		Item {
			objectName: 'Mixer Channel %1'.arg(model.mixerChannelName)

			MixerChannelAssignmentConfig {
				id: mixerChannelConfig
				name: model.mixerChannelName
				midiChannel: model.mixerChannelMidiChannel
			}

			// 14-bit pairs; the MSB alone is plenty.
			MixerChannelCore {
				pflNote: 0x54  // CH CUE
				trimCC: 0x04   // TRIM
				trebleCC: 0x07 // EQ HI
				midCC: 0x0B    // EQ MID
				bassCC: 0x0F   // EQ LOW
				faderCC: 0x13  // CH FADER (captured)
			}

			// SHIFT + CH CUE switches the strip's EQ knobs to stem levels, as
			// on the G2; the FLX10 sends it as its own note.
			ValueNoteAssignment {
				objectName: 'Stems Level Mixer Channel %1'.arg(mixerChannelConfig.name)
				channel: mixerChannelConfig.midiChannel
				note: 0x39 // CH CUE + SHIFT
				enabled: !mixerChannelConfig.inComputerMode
				output: ActionOutput {
					target: PropertyTarget {
						path: mixerChannelConfig.channelStripModePath
					}
				}
			}

			// The channel meter, which the FLX10 shows when the strip's input
			// selector is on PC. The CC values are a guess at its scale.
			VUMeter {
				readonly property real headroomAdjustDb: 10.0
				property var specThresholds: [-40, -20, -8, -4, -2, 0, 2, 4, 6, 8, 50]
				dBThresholds: specThresholds.map((threshold) => threshold - headroomAdjustDb)
				ledCCValues: [0, 12, 25, 38, 51, 64, 76, 89, 102, 115, 127]
				peakHoldCCValues: [0, 12, 25, 38, 51, 64, 76, 89, 102, 115, 127]
				vuMeterType: VUMeterType.Channel
				vuLedCCIndexing: VULedCCIndexing.Incremental
				model: ListModel {
					ListElement {
						cc: 0x02 // CH LEVEL METER
					}
				}
			}
		}
	}

	Repeater {
		model: ListModel {
			ListElement { mixerChannelName: '1'; colorCC: 0x17 } // COLOR (CH 1)
			ListElement { mixerChannelName: '2'; colorCC: 0x18 } // COLOR (CH 2)
			ListElement { mixerChannelName: '3'; colorCC: 0x19 } // COLOR (CH 3)
			ListElement { mixerChannelName: '4'; colorCC: 0x1A } // COLOR (CH 4)
		}

		Item {
			objectName: 'Mixer Channel %1 Color'.arg(model.mixerChannelName)

			MixerChannelAssignmentConfig {
				id: mixerChannelConfig
				name: model.mixerChannelName
				midiChannel: 0x06
			}

			SweepFxKnob {
				cc: model.colorCC
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Beat FX, as the G2's first DJ FX unit (MIDI channel 5)

	Item {
		objectName: 'DJ Fx Unit 1'

		FxAssignmentConfig {
			id: fxConfig
			midiChannel: 0x04
			channelNames: [1]
		}

		DJFxActivate {
			fxActivateType: FxActivateType.Button
			activateControlsModel: ListModel {
				ListElement {
					note: 0x47 // BEAT FX ON/OFF
				}
			}
			flashOnActive: true
		}

		DJFxWetDry {
			cc: 0x02 // LEVEL/DEPTH
		}

		DJFxRateButton {
			note: 0x4A // BEAT <
			targetProperty: "DecrementBeats"
		}

		DJFxRateButton {
			note: 0x4B // BEAT >
			targetProperty: "IncrementBeats"
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
