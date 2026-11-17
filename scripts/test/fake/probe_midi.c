/* midi: how Engine finds its control surface.
 *
 * Engine enumerates ALSA sequencer clients, drops any without a card number,
 * and binds a control map only to one that answers a MIDI Device Inquiry as a
 * device it knows. midisurface is a userspace client, so it has no card number
 * until alsashim supplies one, and it answers the inquiry from the product code.
 *
 * FAKE_EXPECT_SURFACE=1, which check.sh sets when the rootfs has midisurface,
 * makes a missing surface a failure and not a skip. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fake.h"

static snd_seq_t *seq;
static int port;

/* Waits up to ms for an event of the given type. For sysex, collects it. */
static int wait_for(int type, int ms, unsigned char *out, size_t *out_len, snd_seq_event_t *copy) {
    size_t got = 0;
    for (int waited = 0; waited < ms;) {
        snd_seq_event_t *ev;
        if (snd_seq_event_input(seq, &ev) < 0) {
            usleep(10000);
            waited += 10;
            continue;
        }
        if (ev->type != type) continue;
        if (copy) *copy = *ev;
        if (type != SND_SEQ_EVENT_SYSEX) return 1;
        /* A long sysex arrives in pieces; the last ends in F7. */
        size_t n = ev->data.ext.len;
        if (n > *out_len - got) n = *out_len - got;
        memcpy(out + got, ev->data.ext.ptr, n);
        got += n;
        if (got && out[got - 1] == 0xF7) {
            *out_len = got;
            return 1;
        }
    }
    return 0;
}

void probe_midi(void) {
    const char *expect = getenv("FAKE_EXPECT_SURFACE");
    int required = expect && *expect == '1';
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        if (required) t_fail("midi", "no ALSA sequencer");
        else t_skip("midi", "no ALSA sequencer");
        return;
    }
    snd_seq_set_client_name(seq, "Engine");
    port = snd_seq_create_simple_port(seq, "fake",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    /* The surface: a userspace client named for a product's controller. */
    snd_seq_client_info_t *info;
    snd_seq_client_info_alloca(&info);
    snd_seq_client_info_set_client(info, -1);
    int surface = -1, surface_card = -1;
    char surface_name[64] = "";
    while (snd_seq_query_next_client(seq, info) >= 0) {
        int id = snd_seq_client_info_get_client(info);
        if (id == snd_seq_client_id(seq) ||
            snd_seq_client_info_get_type(info) != SND_SEQ_USER_CLIENT ||
            !strstr(snd_seq_client_info_get_name(info), "Controller"))
            continue;
        surface = id;
        /* Newer than the alsa-lib this is built against, so found at run time,
         * where alsashim's definition comes first. */
        int (*get_card)(const snd_seq_client_info_t *) = entry("snd_seq_client_info_get_card");
        surface_card = get_card ? get_card(info) : -1;
        snprintf(surface_name, sizeof(surface_name), "%s", snd_seq_client_info_get_name(info));
        break;
    }
    if (surface < 0) {
        if (required) t_fail("midi.surface", "no control surface client");
        else t_skip("midi.surface", "no control surface running here");
        snd_seq_close(seq);
        return;
    }
    t_ok("midi.surface", "client %d, %s", surface, surface_name);
    if (surface_card < 0)
        t_fail("midi.card-number", "none, so Engine drops the client");
    else t_ok("midi.card-number", "card %d", surface_card);

    if (snd_seq_connect_from(seq, port, surface, 0) < 0 ||
        snd_seq_connect_to(seq, port, surface, 0) < 0) {
        t_fail("midi.inquiry", "could not connect to the surface's port");
        snd_seq_close(seq);
        return;
    }
    unsigned char request[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}, reply[64];
    size_t reply_len = sizeof(reply);
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_sysex(&ev, sizeof(request), request);
    snd_seq_event_output_direct(seq, &ev);
    if (!wait_for(SND_SEQ_EVENT_SYSEX, 3000, reply, &reply_len, NULL))
        t_fail("midi.inquiry", "no reply to the device inquiry");
    else if (reply_len < 10 || reply[1] != 0x7E || reply[3] != 0x06 || reply[4] != 0x02)
        t_fail("midi.inquiry", "%zu bytes, not an identity reply", reply_len);
    else
        t_ok("midi.inquiry", "%zu bytes, manufacturer %02X %02X %02X, family %02X", reply_len,
             reply[5], reply[6], reply[7], reply[8]);

    /* And a button, through the fifo the service reads commands from. */
    int fifo = open("/run/midisurface.fifo", O_WRONLY | O_NONBLOCK);
    if (fifo < 0) {
        t_skip("midi.control", "no /run/midisurface.fifo");
    } else {
        const char *cmd = "on 1 2 127\n";
        snd_seq_event_t note;
        if (write(fifo, cmd, strlen(cmd)) < 0 ||
            !wait_for(SND_SEQ_EVENT_NOTEON, 3000, NULL, NULL, &note))
            t_fail("midi.control", "no note from the surface");
        else if (note.data.note.channel != 1 || note.data.note.note != 2)
            t_fail("midi.control", "channel %d note %d, sent 1 and 2", note.data.note.channel,
                   note.data.note.note);
        else t_ok("midi.control", "note on, channel 1 note 2");
        const char *off = "off 1 2\n";
        if (write(fifo, off, strlen(off)) < 0) { /* the note is only a test */ }
        close(fifo);
    }
    snd_seq_close(seq);
}
