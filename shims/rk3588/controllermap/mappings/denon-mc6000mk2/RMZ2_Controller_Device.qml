import airAssignments 1.0
import InputAssignment 0.1
import OutputAssignment 0.1
import Device 0.1
import QtQuick 2.0
import Planck 1.0

Device {
	id: device

	property real gamma: 3.5
	property real padGamma: 3.5

	controls: []
	useGlobalShift: false
	numberOfLayers: 0

	// identification
	readonly property string sysExStart: "F0"
	readonly property string manufacturerId: "00 00 17"
	readonly property string deviceId: "7F" // broadcast all
	readonly property string productId: "27"
	readonly property string sysExEnd: "F7"

	// requests
	readonly property string requestFaderCalibration: "0E"

	// responses
	readonly property string faderCalibrationResponse: "0D"
	readonly property string powerOnResponse: "42"

	// params
	readonly property string crossfaderId: "00"

	// results
	property int crossfaderCalibrationStatus: 0x00

	function createSysExMessage(command, bytes, param) {
		return [
			device.sysExStart,
			device.manufacturerId,
			device.deviceId,
			device.productId,
			command,
			bytes,
			param,
			device.sysExEnd
		].join(" ");
	}

	///////////////////////////////////////////////////////////////////////////
	// Setup

	function queryAbsoluteControls() {
		Midi.sendSysEx("F0 00 20 7F 03 01 F7")
	}

	function sendInitializationMessage() {
		Midi.sendSysEx("F0 00 00 17 7F 27 60 00 04 04 01 01 01 F7")
	}

	property Timer initPhaseEndTimer: Timer {
		interval: 1000
		repeat: false
		onTriggered: {
			device.isInitializing = false
			queryAbsoluteControls()
		}
	}

	property bool isInitializing: false

	Component.onCompleted: {
		Midi.sendSysEx("F0 7E 00 06 01 F7")

		isInitializing = true

		requestPowerOnButtonState()

		sendInitializationMessage();

		initPhaseEndTimer.start()
	}

	Component.onDestruction: {
		setMotorDirection(4, 0, true);
		setMotorDirection(5, 0, true);
		if(Planck.quitReason() === QuitReasons.UpdateFromFile
			|| Planck.quitReason() === QuitReasons.UpdateFromLoader
			|| Planck.quitReason() === QuitReasons.UpdateFromUrl
			|| Planck.quitReason() === QuitReasons.FactoryReset)
		{
			//Set LED Bloom in update mode
			Midi.sendNoteOn(15, 127, 2)
		}
	}

	function setMotorDirection( channel, direction, fast) {
		if(direction === 0) {
			Midi.sendControlChange(channel, fast? 23 : 25, 127); // Turn off
		}
		if(direction === 1) {
			Midi.sendControlChange(channel, 27, 0); // Not reversed
		}
		else if(direction === -1) {
			Midi.sendControlChange(channel, 27, 1); // Reverse
		}
		if(direction !== 0) {
			Midi.sendControlChange(channel, fast? 22 : 24, 127); // Turn on
		}
	}

	function setMotorSpeed( channel, speed, fortyFiveRpm) {
		var intSpeed = Math.max(0.0, Math.min(16383.0, speed*8191.0));

		var msb = Math.floor(intSpeed/128);
		var lsb = Math.floor(intSpeed%128);

		Midi.sendControlChange(channel, 26, fortyFiveRpm? 1 : 0);
		Midi.sendControlChange(channel, 33, msb);
		Midi.sendControlChange(channel, 32, lsb);
	}

	function setStopTime(channel, stopTime) {
		Midi.sendControlChange(channel, 29, Math.round(stopTime));
	}

	function setMotorTorque(torque) {
		Midi.sendNoteOn(15 , 113, torque ? 127 : 0); // Set torque
	}

	// Dec to Hex Conversion
	function d2h(d){
		return (+d).toString(16).toUpperCase()
	}

	function midiColorChannel(c, gamma){
		return d2h(Math.min(127, Math.max(0,Math.floor(Math.pow(c, gamma) * 127))))
	}

	function mapColor(color) {
		return Qt.rgba(color.r, color.g, color.b , color.a)
	}

	function midiColor(color, gamma) {
		var c = mapColor(color)
		return midiColorChannel(c.r, gamma)+ " " +  midiColorChannel(c.g, gamma) + " " +  midiColorChannel(c.b, gamma)
	}

	function sendNoteOn(channel, index, value) {
		Midi.sendNoteOn(channel, index, value)
	}

	function sendSimpleColor(channel, index, value) {
		if (value === 0) {
			Midi.sendNoteOff(channel, index)
		}
		else {
			Midi.sendNoteOn(channel, index, value)
		}
	}

	//Color Send Function
	function sendColor(channel, index, color)
	{
		var g = device.gamma
		if(index >= 15 && index <= 23) {
			g = device.padGamma
		}

		var sysEx = "F0 00 00 17 7F 27 03 00 05 " + d2h(channel) + " " + d2h(index) + " " + midiColor(color, g)+" F7"
		Midi.sendSysEx(sysEx)

	}

	function requestPowerOnButtonState() {
		Midi.sendSysEx("F0 00 00 17 7F 27 42 00 00 F7")
	}

	function sysExToIntList(sysExString)
	{
		var valueList = sysExString.split(" ")
		var result = []

		for(var i = 0; i < valueList.length; ++i) {
			result.push(parseInt(valueList[i], 16))
		}

		return result
	}

	function isManufacturer(valueList) {
		const manufacturerList = sysExToIntList(device.manufacturerId)
		return valueList[1] === manufacturerList[0]
			&& valueList[2] === manufacturerList[1]
			&& valueList[3] === manufacturerList[2]
	}

	function isProduct(valueList) {
		const productId = parseInt(device.productId, 16)
		return valueList[5] === productId
	}

	function isPowerOnResponse(valueList) {
		const responseCode = parseInt(device.powerOnResponse, 16)
		return isManufacturer(valueList) && isProduct(valueList) && valueList[6] === responseCode
	}

	function handlePowerOnResponse(request) {
		if(request === 0x00) {
			console.log("No special power on request")
		} else if(request === 0x01) {
			console.log("Request test-mode entry")
			quitToTestApp()
		} else {
			console.warn("Unknown power on request:", request)
		}
	}

	function sysEx(sysExString) {
		const valueList = sysExToIntList(sysExString)

		if(isPowerOnResponse(valueList))
		{
			handlePowerOnResponse(valueList[9])
		}
		else if(isFaderCalibrationResponse(valueList))
		{
			handleCrossfaderCalibrationResponse(valueList[9], valueList[10]);
		}
		else
		{
			return false;
		}
	}

	function note(timeStamp, channel, noteIndex, noteVelocity) {
		return false;
	}

	function cc(timeStamp, channel, ccIndex, ccValue) {
		return false;
	}

	function pitchBend(timeStamp, channel, pbValue) {
		return false;
	}

	property list<Item> oleds

	Repeater {
		model: 16
		Item {
			id: self

			property var invertDisplay: false

			function buildSysEx(converter) {
				var result = "00 00 17 7F 27 08 00"

				var stringAsHex = converter.stringToHex()
				//Length is + 2 to account for pad index and inverted state below
				var dataLength = converter.dataLength + 2

				// this should be Number of data bytes to follow (least significant) 0x02 -> 0x17
				result += " " + dataLength.toString(16)
				// pad index
				result += " " + index.toString(16)
				// inverted state
				result += invertDisplay ? " 01" : " 00"
				result += " " + stringAsHex

				return result;
			}

			function update(converter) {
				//called from timer
				var fullSysEx = buildSysEx(converter)
				Midi.sendSysEx(fullSysEx);
			}

			Component.onCompleted: {
				device.oleds.push(self)
			}
		}
	}

	function startCrossfaderCalibration() {
		device.crossfaderCalibrationStatus = 0x00
		const bytesToFollow = "00 01"
		Midi.sendSysEx(device.createSysExMessage(device.requestFaderCalibration, bytesToFollow, device.crossfaderId));
	}

	function isFaderCalibrationResponse(valueList) {
		const responseCode = parseInt(device.faderCalibrationResponse, 16)
		return isManufacturer(valueList) && isProduct(valueList) && valueList[6] === responseCode
	}

	function handleCrossfaderCalibrationResponse(faderId, status) {
		if(faderId === parseInt(device.crossfaderId, 16)) {
			console.log("Fader calibration finished with result:", status)
			device.crossfaderCalibrationStatus = status;
		} else {
			console.warn("Received fader calibration response for unknown fader id:", faderId)
		}
	}
}
