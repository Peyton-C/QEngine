// Pioneer DDJ-FLX10, as the Prime 4 G2's (JP24) control surface: how Engine
// lights it.
//
// The vendor JP24 device, with its two lamp functions rewritten for the
// FLX10. midisurface --echo-lamps hands Engine's notes to the controller as
// they are, and the assignment file uses the FLX10's own notes, so all that is
// left to translate is the value:
//
//   buttons  the FLX10 takes 0x7F for lit and 0x00 for dark. Engine's dim
//            (velocity 1) would read as lit, and the FLX10 has no dim level,
//            so dim is dark.
//   pads     the pad channels (MIDI channels 8-15) take a colour number,
//            1-127, as the velocity. 0x00 is the pad's own dim backlight.
//
// Everything else -- the identity handshake, hybrid mode relay, power-on
// state -- is the vendor file unchanged, since midisurface answers for the
// G2 and drops the SysEx the G2 would have acted on.
import airAssignments 1.0
import InputAssignment 0.1
import OutputAssignment 0.1
import Device 0.1
import QtQuick 2.0
import Planck 1.0

Device {
	id: device

	property string hybridModeMidiDeviceName: "f_midi-0"

	controls: []
	useGlobalShift: true
	numberOfLayers: 0

	readonly property QObjProperty pRunningDark: Planck.getProperty("/Client/PowerHandler/RunningDark");
	readonly property bool isRunningDark: pRunningDark && pRunningDark.translator && pRunningDark.translator.state
	onIsRunningDarkChanged: {
		for(var channel in currentColors) {
			for(var key in currentColors[channel]) {
				sendColor(channel, key, isRunningDark ? Qt.rgba(0, 0, 0, 1) : currentColors[channel][key]);
			}
		}
		for(var channel in currentSimpleColors) {
			for(var simpleKey in currentSimpleColors[channel]) {
				sendSimpleColor(channel, simpleKey, isRunningDark ? 0 : currentSimpleColors[channel][simpleKey]);
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Setup

	function sendInitializationMessage() {
		Midi.sendSysEx("F0 00 02 0B 7f 14 60 00 04 04 01 01 03 F7")
	}

	property Timer initTimer: Timer {
		interval: 1000
		repeat: false
		onTriggered: {
				device.isInitializing = false
		}
	}

	property bool isInitializing: false

	Component.onCompleted: {
		currentColors = {}
		currentSimpleColors = {}

		Midi.sendSysEx("F0 7E 00 06 01 F7") // Device Inquiry

		isInitializing = true

		requestPowerOnButtonState();

		sendInitializationMessage();

		initTimer.start()
	}

	function setQuadPadsMode(mode, deckName) {
	}

	property var currentColors
	property var currentSimpleColors

	///////////////////////////////////////////////////////////////////////////
	// Lamps

	function isPadChannel(channel) {
		return channel >= 7 && channel <= 14
	}

	// The FLX10's pad colours, read off the unit by lighting every number:
	// 1-62 run once round the hue wheel backwards from blue, 63 is dark, 64
	// white, and 65-126 repeat 1-62 dimmed. Each pair is a colour number and
	// the hue it showed, unwrapped so the list only falls (violet as -105).
	readonly property var padHues: [
		[1, 240], [11, 180], [16, 165], [22, 120], [25, 95], [32, 52],
		[37, 38], [40, 20], [43, 0], [46, -15], [48, -35], [52, -50],
		[56, -62], [59, -80], [62, -105]
	]

	function padColorNumber(color) {
		var c = Qt.rgba(color.r, color.g, color.b, color.a)
		var hi = Math.max(c.r, c.g, c.b)
		var lo = Math.min(c.r, c.g, c.b)
		if (hi < 0.08)
			return 0                           // the pad's own dim backlight
		// Greys: white when bright. Engine's idle pad colour is a dark grey,
		// which as white lit every empty pad brightly; the FLX10's own dim
		// backlight (0) is what that state looks like on rekordbox.
		if (hi - lo < 0.15 * hi)
			return hi < 0.55 ? 0 : 64
		var h
		if (hi === c.r)
			h = ((c.g - c.b) / (hi - lo) + 6) % 6
		else if (hi === c.g)
			h = (c.b - c.r) / (hi - lo) + 2
		else
			h = (c.r - c.g) / (hi - lo) + 4
		h *= 60
		if (h > 240)
			h -= 360
		var n = padHues[padHues.length - 1][0]
		if (h >= padHues[0][1]) {
			n = 1
		} else {
			for (var i = 1; i < padHues.length; i++) {
				var a = padHues[i - 1], b = padHues[i]
				if (h >= b[1]) {
					n = Math.round(a[0] + (b[0] - a[0]) * (a[1] - h) / (a[1] - b[1]))
					break
				}
			}
			// Between violet and blue: whichever end is nearer.
			if (h < padHues[padHues.length - 1][1] && h < -112)
				n = 1
		}
		return hi < 0.55 ? n + 64 : n
	}

	function sendNoteOn(channel, index, value) {
		Midi.sendNoteOn(channel, index, value);
	}

	function sendSimpleColor(channel, index, value) {
		if(!currentSimpleColors[channel]) {
			currentSimpleColors[channel] = {};
		}
		currentSimpleColors[channel][index] = value

		if(isRunningDark)
			return;

		if(value >= 64) {
			Midi.sendNoteOn(channel, index, 0x7F)
		} else {
			Midi.sendNoteOff(channel, index)
		}
	}

	function sendColor(channel, index, color)
	{
		if(!currentColors[channel]) {
			currentColors[channel] = {};
		}
		currentColors[channel][index] = color;

		if(isRunningDark) {
			return;
		}

		var value
		if (isPadChannel(channel)) {
			value = padColorNumber(color)
		} else {
			var c = Qt.rgba(color.r, color.g, color.b, color.a)
			value = Math.max(c.r, c.g, c.b) > 0.5 ? 0x7F : 0
		}
		if (value > 0) {
			Midi.sendNoteOn(channel, index, value)
		} else {
			Midi.sendNoteOff(channel, index)
		}
	}

	function requestPowerOnButtonState() {
		Midi.sendSysEx("F0 00 02 0B 7F 14 42 00 00 F7");
	}

	function sysExToIntList(sysExString)
	{
		var valueList = sysExString.split(" ");
		var result = [];

		for(var i=0;i<valueList.length;i++) {
			result.push(parseInt(valueList[i], 16))
		}

		return result;
	}

	function sysEx(sysExString) {
		MidiDevices.sendSysEx(hybridModeMidiDeviceName, sysExString)
		return false;
	}

	function note(timeStamp, channel, noteIndex, noteVelocity) {
		if (noteVelocity === 0) {
			MidiDevices.sendNoteOff(hybridModeMidiDeviceName, channel, noteIndex)
		}
		else {
			MidiDevices.sendNoteOn(hybridModeMidiDeviceName, channel, noteIndex, noteVelocity)
		}
		return false;
	}

	function cc(timeStamp, channel, ccIndex, ccValue) {
		MidiDevices.sendControlChange(hybridModeMidiDeviceName, channel, ccIndex, ccValue)
		return false;
	}

	function pitchBend(timeStamp, channel, pbValue) {
		MidiDevices.sendPitch(hybridModeMidiDeviceName, channel, pbValue)
		return false;
	}
}
