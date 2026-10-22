// Deck state for the DDJ-FLX10 jog screen bridge, as JSON datagrams on the
// MONTERO channel "flx10" (abstract socket @montero/flx10).
//
// Exploratory for now: every property is sent with its translator's four
// readings (unnormalized, value, string, state), because which one carries the
// number -- and in what unit -- differs from property to property and is
// easiest read off a playing deck. Trim to what the bridge uses once known.
import QtQuick
import StateMap 1.0
import Montero 1.0

Item {
	id: feed

	readonly property var paths: [
		"PlayState", "Play", "CurrentBPM", "Speed", "SpeedRange", "SpeedState",
		"DeckIsMaster", "Sync", "BeatNumber",
		"Track/SongLoaded", "Track/SongAnalyzed", "Track/SongName", "Track/ArtistName",
		"Track/TrackNetworkPath", "Track/TrackLength", "Track/SampleRate",
		"Track/SongPosition", "Track/PlayPosition", "Track/TrackPosition",
		"Track/CurrentKey", "Track/CurrentKeyCents", "Track/KeyLock", "Track/KeyOffset",
		"Track/OriginalTempo", "Track/MetaDataBPM",
		"Track/LoopEnableState", "Track/CurrentLoopInPosition",
		"Track/CurrentLoopOutPosition", "Track/CurrentLoopSizeInBeats",
		"Track/QuickCue1Exists", "Track/QuickCue1Position",
		"Track/QuickCue2Exists", "Track/QuickCue2Position"
	]
	property var decks: []

	Component.onCompleted: {
		const all = []
		for (let d = 1; d <= 4; d++) {
			const deck = {}
			for (const p of paths)
				deck[p] = Planck.getProperty("/Engine/Deck" + d + "/" + p)
			all.push(deck)
		}
		decks = all
		Montero.log("flx10: feed up, " + paths.length + " properties per deck")
	}

	function read(p) {
		if (!p || !p.translator)
			return null
		const t = p.translator
		return { u: t.unnormalized, v: t.value, s: t.string, st: t.state }
	}

	Timer {
		interval: 100
		running: true
		repeat: true
		onTriggered: {
			const out = { t: Date.now(), decks: [] }
			for (const deck of feed.decks) {
				const o = {}
				for (const p in deck)
					o[p] = feed.read(deck[p])
				out.decks.push(o)
			}
			Montero.send("flx10", JSON.stringify(out))
		}
	}
}
