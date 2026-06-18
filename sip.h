#ifndef SIP_H
#define SIP_H

#include <stdint.h>
#include <netinet/in.h>

/*
 * A deliberately primitive SIP/RTP stack, just enough to carry one fax call
 * over a real SIP leg instead of the raw TCP audio pipe. It is modelled on
 * ../sip_modem/sip_interface but pared down hard for this tool's needs:
 *
 *   - exactly ONE call per process run (no threads, no call list);
 *   - G.711 A-law (PCMA, RTP payload type 8) at 8 kHz only - the sample
 *     format spandsp's fax engine speaks, with no resampling;
 *   - two roles: dial out (UAC) or answer one inbound INVITE (UAS), the
 *     latter optionally preceded by a single REGISTER to a registrar;
 *   - blocking, best-effort signalling: one 401/407 digest retry, simple
 *     fixed retransmits - no full RFC 3261 transaction timer machinery.
 *
 * Once a call is established the caller drives the media itself, pacing
 * sip_media_tx() at 20 ms and draining sip_media_rx()/sip_media_poll_sip()
 * in between - exactly the cadence spandsp's fax_tx()/fax_rx() expect.
 */

typedef struct {
    int   dial;                 /* 1 = place a call (UAC); 0 = answer (UAS)   */
    char  target[256];          /* dial: sip:user@host | user@host | user     */
    int   do_register;          /* answer: REGISTER upstream before answering */

    char  local_user[128];      /* user part of our AoR                       */
    char  registrar_host[256];  /* host part of our AoR; proxy for dial-out   */
    char  password[128];        /* digest password (may be empty)             */
    int   local_sip_port;       /* local UDP SIP port (default 5060)          */
    int   reg_expires;          /* REGISTER lifetime, seconds                 */
    int   verbose;              /* log SIP traffic to stderr                  */

    /* Daemon mode: run forever, fork a child per inbound call, spool each
     * received fax. See sip_daemon_* below and run_daemon() in sip_fax.c. */
    int   daemon_mode;          /* 1 = long-lived multi-call daemon           */
    char  spool_dir[256];       /* directory for <rand>.tiff / <rand>.invite  */
    int   reg_interval;         /* re-REGISTER cadence, seconds               */

    int   enable_t38;           /* offer (dial) / accept (answer) T.38 re-INVITE */
} sip_config_t;

typedef struct {
    int               sip_sock;
    int               rtp_sock;
    int               verbose;

    char              local_ip[64];
    char              local_user[128]; /* user part of our AoR (for Contact)  */
    int               local_sip_port;
    struct sockaddr_in sip_peer;       /* where in-dialog requests are sent */
    struct sockaddr_in remote_rtp;     /* negotiated media destination      */

    /* RTP TX state (PCMA, PT 8, 8 kHz). */
    uint16_t          rtp_seq;
    uint32_t          rtp_ts;
    uint32_t          rtp_ssrc;
    int               have_rtp_dest;

    int               log_callid;     /* prefix SIP log lines with call_id (daemon) */

    /* Dialog identifiers, captured at establish time so we can emit (and
     * answer) an in-dialog BYE without re-deriving them. */
    char              call_id[256];
    char              bye_ruri[600];   /* Request-URI for our BYE           */
    char              bye_from[512];   /* From: value for our requests      */
    char              bye_to[512];     /* To: value for our requests        */
    int               cseq;            /* next CSeq number for our requests */

    /* For UAS: the 200 OK we sent, replayed if the INVITE is retransmitted. */
    char              ok_buf[2048];
    int               ok_buf_len;

    int               peer_hung_up;    /* set once a BYE has been exchanged */

    /* T.38 (UDPTL) media, negotiated via re-INVITE. */
    int               enable_t38;      /* offer/accept T.38 for this call    */
    int               t38_sock;        /* UDPTL socket, or -1                */
    int               t38_active;      /* media has switched to T.38         */
    struct sockaddr_in t38_peer;       /* peer's UDPTL address               */
    int               t38_far_datagram;/* peer's T38FaxMaxDatagram           */
} sip_media_t;

/* Run the signalling to bring up one call, blocking until it is established
 * (media negotiated, ACK exchanged). Returns 0 on success, -1 on failure. */
int  sip_media_establish(const sip_config_t *cfg, sip_media_t *m);

/* Encode n linear samples (<=160) as one A-law RTP packet and send it. */
void sip_media_tx(sip_media_t *m, const int16_t *pcm, int n);

/* Receive at most one pending RTP packet (non-blocking), decode A-law into
 * pcm (capacity max samples), and return the sample count (0 if none). */
int  sip_media_rx(sip_media_t *m, int16_t *pcm, int max);

/* Service one pending SIP datagram (non-blocking): answer a peer BYE, replay
 * our 200 OK / ACK on retransmits, and (if m->enable_t38) accept an inbound
 * T.38 re-INVITE — switching the media to T.38. Returns 1 if the peer ended
 * the call. */
int  sip_media_poll_sip(sip_media_t *m);

/* ── T.38 (UDPTL) media ─────────────────────────────────────────────────── */
/* True once the media has switched to T.38 (after a re-INVITE was accepted or
 * offered+accepted). */
int  sip_media_is_t38(const sip_media_t *m);
/* Send / receive a UDPTL datagram on the T.38 media socket (non-blocking rx:
 * returns datagram length, 0 if none, <0 on error). */
void sip_t38_tx(sip_media_t *m, const uint8_t *dgram, int len);
int  sip_t38_rx(sip_media_t *m, uint8_t *buf, int max);
/* UAC: after the audio call is up, offer T.38 via an in-dialog re-INVITE.
 * Returns 1 if the peer accepted (media now T.38), 0 otherwise. Blocking. */
int  sip_offer_t38(sip_media_t *m, const sip_config_t *cfg);

/* Daemon UAS: accept an in-dialog T.38 re-INVITE on `dlg` (answers 200 with a
 * T.38 SDP, opens a UDPTL socket, records peer/far-datagram). Returns the UDPTL
 * fd to hand to the media child, or -1 if not an acceptable T.38 offer. */
int  sip_t38_accept(sip_media_t *dlg, const char *req, struct sockaddr_in *from,
                    int *far_datagram, int *local_port);
/* Re-answer a retransmitted (already-accepted) T.38 re-INVITE from the saved port. */
void sip_t38_reanswer(sip_media_t *dlg, const char *req, struct sockaddr_in *from,
                      int local_port);

/* Tear the call down: send BYE (or 200 to a pending BYE) and close sockets. */
void sip_media_hangup(sip_media_t *m);

/* ── Daemon-mode building blocks (concurrent, fork-per-call) ──────────────
 *
 * The daemon owns ONE listening socket (the parent's SIP control plane); a
 * forked child per call owns only the media (its own rtp_sock + fax engine).
 * In-dialog requests for every call return to the single listening port
 * (our Contact stays :local_sip_port), so the parent demultiplexes them by
 * Call-ID and answers them itself. See run_daemon() in sip_fax.c.
 */

/* Bind the listening SIP socket, discover the local IP, and (if cfg->do_register)
 * perform the initial REGISTER. Returns 0 on success, -1 on failure. */
int  sip_daemon_listen(const sip_config_t *cfg, sip_media_t *listen);

/* (Re-)REGISTER with the registrar. Returns 0 on success, -1 on failure. */
int  sip_daemon_register(sip_media_t *listen, const sip_config_t *cfg);

/* Accept an inbound INVITE on the listening socket: open a fresh RTP socket,
 * send 100/180/200 (Contact at the listen port, SDP at the new RTP port), and
 * populate *call (its own rtp_sock + remote_rtp + dialog state; call->sip_sock
 * is the listen socket so the parent can answer in-dialog requests and BYE).
 * Returns 0 on success, -1 if the INVITE carries no usable PCMA SDP. */
int  sip_uas_accept(const sip_config_t *cfg, sip_media_t *listen,
                    const char *invite, struct sockaddr_in *from,
                    sip_media_t *call);

/* Send a bodyless final response (e.g. "486 Busy Here") to an INVITE we refuse,
 * echoing its dialog headers (adds a To-tag). */
void sip_uas_decline(sip_media_t *listen, const char *invite,
                     struct sockaddr_in *from, const char *status);

/* Send a bodyless response (e.g. "200 OK" to a BYE, "488 Not Acceptable Here"
 * to a re-INVITE) echoing an in-dialog request's headers, to *to. */
void sip_dialog_respond(sip_media_t *m, const char *req, const char *status,
                        struct sockaddr_in *to);

/* Send an in-dialog BYE for *call from call->sip_sock to call->sip_peer using
 * its captured dialog state. Does not wait or close (the daemon reads the 200
 * on its main loop). */
void sip_call_send_bye(sip_media_t *call);

/* Resend the stored 200 OK (answers a retransmitted initial INVITE whose ACK
 * hasn't arrived) to *to. */
void sip_call_resend_ok(sip_media_t *call, struct sockaddr_in *to);

#endif /* SIP_H */
