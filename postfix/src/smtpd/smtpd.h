/*++
/* NAME
/*	smtpd 3h
/* SUMMARY
/*	smtp server
/* SYNOPSIS
/*	include "smtpd.h"
/* DESCRIPTION
/* .nf

 /*
  * System library.
  */
#include <unistd.h>

 /*
  * SASL library.
  */
#ifdef USE_SASL_AUTH
#include <sasl.h>
#include <saslutil.h>
#endif

 /*
  * Utility library.
  */
#include <vstream.h>
#include <vstring.h>
#include <argv.h>

 /*
  * Global library.
  */
#include <mail_stream.h>

 /*
  * Variables that keep track of conversation state. There is only one SMTP
  * conversation at a time, so the state variables can be made global. And
  * some of this has to be global anyway, so that the run-time error handler
  * can clean up in case of a fatal error deep down in some library routine.
  */
typedef struct SMTPD_DEFER {
    int     active;			/* is this active */
    VSTRING *reason;			/* reason for deferral */
    int     class;			/* error notification class */
} SMTPD_DEFER;

typedef struct SMTPD_XFORWARD_ATTR {
    int     flags;			/* see below */
    char   *name;			/* name for access control */
    char   *addr;			/* address for access control */
    char   *namaddr;			/* name[address] */
    char   *protocol;			/* email protocol */
    char   *helo_name;			/* helo/ehlo parameter */
    char   *ident;			/* message identifier */
} SMTPD_XFORWARD_ATTR;

#define SMTPD_XFORWARD_FLAG_INIT (1<<0)	/* preset done */
#define SMTPD_XFORWARD_FLAG_NAME (1<<1)	/* client name received */
#define SMTPD_XFORWARD_FLAG_ADDR (1<<2)	/* client address received */
#define SMTPD_XFORWARD_FLAG_PROTO (1<<3)/* protocol received */
#define SMTPD_XFORWARD_FLAG_HELO (1<<4)	/* client helo received */
#define SMTPD_XFORWARD_FLAG_IDENT (1<<5)/* message identifier received */

#define SMTPD_XFORWARD_FLAG_CLIENT_MASK \
	(SMTPD_XFORWARD_FLAG_NAME | SMTPD_XFORWARD_FLAG_ADDR \
	| SMTPD_XFORWARD_FLAG_PROTO | SMTPD_XFORWARD_FLAG_HELO)

typedef struct SMTPD_STATE {
    int     err;
    VSTREAM *client;
    VSTRING *buffer;
    time_t  time;
    char   *name;
    char   *addr;
    char   *namaddr;
    int     peer_code;			/* 2=ok, 4=soft, 5=hard */
    int     error_count;
    int     error_mask;
    int     notify_mask;
    char   *helo_name;
    char   *queue_id;
    VSTREAM *cleanup;
    MAIL_STREAM *dest;
    int     rcpt_count;
    char   *access_denied;
    ARGV   *history;
    char   *reason;
    char   *sender;
    char   *encoding;			/* owned by mail_cmd() */
    char   *verp_delims;		/* owned by mail_cmd() */
    char   *recipient;
    char   *etrn_name;
    char   *protocol;
    char   *where;
    int     recursion;
    off_t   msg_size;
    int     junk_cmds;
#ifdef USE_SASL_AUTH
#if SASL_VERSION_MAJOR >= 2
    const char *sasl_mechanism_list;
#else
    char   *sasl_mechanism_list;
#endif
    char   *sasl_method;
    char   *sasl_username;
    char   *sasl_sender;
    sasl_conn_t *sasl_conn;
    VSTRING *sasl_encoded;
    VSTRING *sasl_decoded;
#endif
    int     rcptmap_checked;
    int     warn_if_reject;		/* force reject into warning */
    SMTPD_DEFER defer_if_reject;	/* force reject into deferral */
    SMTPD_DEFER defer_if_permit;	/* force permit into deferral */
    int     defer_if_permit_client;	/* force permit into warning */
    int     defer_if_permit_helo;	/* force permit into warning */
    int     defer_if_permit_sender;	/* force permit into warning */
    int     discard;			/* discard message */
    char   *saved_filter;		/* postponed filter action */
    char   *saved_redirect;		/* postponed redirect action */
    int     saved_flags;		/* postponed hold/discard */
    VSTRING *expand_buf;		/* scratch space for $name expansion */
    VSTREAM *proxy;			/* proxy handle */
    VSTRING *proxy_buffer;		/* proxy query/reply buffer */
    char   *proxy_mail;			/* owned by mail_cmd() */
    int     proxy_xforward_features;	/* proxy XFORWARD features */
    SMTPD_XFORWARD_ATTR xforward;	/* override access control */
} SMTPD_STATE;

extern void smtpd_state_init(SMTPD_STATE *, VSTREAM *);
extern void smtpd_state_reset(SMTPD_STATE *);

 /*
  * Conversation stages.  This is used for "lost connection after XXX"
  * diagnostics.
  */
#define SMTPD_AFTER_CONNECT	"CONNECT"
#define SMTPD_AFTER_DOT		"END-OF-MESSAGE"

 /*
  * Postfix representation of unknown client information within smtpd
  * processes. This is not the representation that Postfix uses in queue
  * files, in queue manager delivery requests, nor is it the representation
  * of information in XCLIENT/XFORWARD commands!
  */
#define CLIENT_ATTR_UNKNOWN	"unknown"

#define CLIENT_NAME_UNKNOWN	CLIENT_ATTR_UNKNOWN
#define CLIENT_ADDR_UNKNOWN	CLIENT_ATTR_UNKNOWN
#define CLIENT_NAMADDR_UNKNOWN	CLIENT_ATTR_UNKNOWN
#define CLIENT_HELO_UNKNOWN	0
#define CLIENT_PROTO_UNKNOWN	CLIENT_ATTR_UNKNOWN
#define CLIENT_IDENT_UNKNOWN	0

#define IS_AVAIL_CLIENT_ATTR(v)	((v) && strcmp((v), CLIENT_ATTR_UNKNOWN))

#define IS_AVAIL_CLIENT_NAME(v)	IS_AVAIL_CLIENT_ATTR(v)
#define IS_AVAIL_CLIENT_ADDR(v)	IS_AVAIL_CLIENT_ATTR(v)
#define IS_AVAIL_CLIENT_NAMADDR(v) IS_AVAIL_CLIENT_ATTR(v)
#define IS_AVAIL_CLIENT_HELO(v)	((v) != 0)
#define IS_AVAIL_CLIENT_PROTO(v) IS_AVAIL_CLIENT_ATTR(v)
#define IS_AVAIL_CLIENT_IDENT(v) ((v) != 0)

 /*
  * If running in stand-alone mode, do not try to talk to Postfix daemons but
  * write to queue file instead.
  */
#define SMTPD_STAND_ALONE(state) \
	(state->client == VSTREAM_IN && getuid() != var_owner_uid)

 /*
  * If running as proxy front-end, disable actions that require communication
  * with the cleanup server.
  */
#define USE_SMTPD_PROXY(state) \
	(SMTPD_STAND_ALONE(state) == 0 && *var_smtpd_proxy_filt)

 /*
  * SMTPD peer information lookup.
  */
extern void smtpd_peer_init(SMTPD_STATE *state);
extern void smtpd_peer_reset(SMTPD_STATE *state);

#define	SMTPD_PEER_CODE_OK	2
#define SMTPD_PEER_CODE_TEMP	4
#define SMTPD_PEER_CODE_PERM	5

 /*
  * Choose between normal or forwarded attributes.
  * 
  * Note 1: inside the SMTP server, forwarded attributes must have the exact
  * same representation as normal attributes: unknown string values are
  * "unknown", except for HELO which defaults to null. This is better than
  * having to change every piece of code that accesses a possibly forwarded
  * attribute.
  * 
  * Note 2: outside the SMTP server, the representation of unknown/known
  * attribute values is different in queue files, in queue manager delivery
  * requests, and in over-the-network XFORWARD commands.
  */
#define SMTPD_PROXY_XFORWARD_NAME (1<<0)	/* client name */
#define SMTPD_PROXY_XFORWARD_ADDR (1<<1)	/* client address */
#define SMTPD_PROXY_XFORWARD_PROTO (1<<2)	/* protocol */
#define SMTPD_PROXY_XFORWARD_HELO (1<<3)	/* client helo */
#define SMTPD_PROXY_XFORWARD_IDENT (1<<4)	/* message identifier */

 /*
  * If forwarding client information, don't mix information from the current
  * SMTP session with forwarded information from an up-stream session.
  */
#define FORWARD_CLIENT_ATTR(s, a) \
	(((s)->xforward.flags & SMTPD_XFORWARD_FLAG_CLIENT_MASK) ? \
	    (s)->xforward.a : (s)->a)

#define FORWARD_IDENT_ATTR(s) \
	(((s)->xforward.flags & SMTPD_XFORWARD_FLAG_IDENT) ? \
	    (s)->queue_id : (s)->ident)

#define FORWARD_ADDR(s)		FORWARD_CLIENT_ATTR((s), addr)
#define FORWARD_NAME(s)		FORWARD_CLIENT_ATTR((s), name)
#define FORWARD_NAMADDR(s)	FORWARD_CLIENT_ATTR((s), namaddr)
#define FORWARD_PROTO(s)	FORWARD_CLIENT_ATTR((s), protocol)
#define FORWARD_HELO(s)		FORWARD_CLIENT_ATTR((s), helo_name)
#define FORWARD_IDENT(s)	FORWARD_IDENT_ATTR(s)

extern void smtpd_xforward_init(SMTPD_STATE *state);
extern void smtpd_xforward_preset(SMTPD_STATE *state);
extern void smtpd_xforward_reset(SMTPD_STATE *state);

 /*
  * Transparency: before mail is queued, do we check for unknown recipients,
  * do we allow address mapping, automatic bcc, header/body checks?
  */
extern int smtpd_input_transp_mask;

/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/
