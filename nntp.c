/*
 * Copyright (C) 1998 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999 Andrej Gritsenko <andrej@lucky.net>
 * Copyright (C) 2000-2012 Vsevolod Volkov <vvv@mutt.org.ua>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "mutt.h"
#include "mutt_curses.h"
#include "sort.h"
#include "mx.h"
#include "mime.h"
#include "rfc1524.h"
#include "rfc2047.h"
#include "mailbox.h"
#include "mutt_crypt.h"
#include "nntp.h"

#if defined(USE_SSL)
#include "mutt_ssl.h"
#endif

#ifdef HAVE_PGP
#include "pgp.h"
#endif

#ifdef HAVE_SMIME
#include "smime.h"
#endif

#if USE_HCACHE
#include "hcache.h"
#endif

#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#ifdef USE_SASL
#include <sasl/sasl.h>
#include <sasl/saslutil.h>

#include "mutt_sasl.h"
#endif

static int nntp_connect_error (NNTP_SERVER *nserv)
{
  nserv->status = NNTP_NONE;
  mutt_error _("Server closed connection!");
  mutt_sleep (2);
  return -1;
}

/* Get capabilities:
 * -1 - error, connection is closed
 *  0 - mode is reader, capabilities setted up
 *  1 - need to switch to reader mode */
static int nntp_capabilities (NNTP_SERVER *nserv)
{
  CONNECTION *conn = nserv->conn;
  unsigned int mode_reader = 0;
  char buf[LONG_STRING];
  char authinfo[LONG_STRING] = "";

  nserv->hasCAPABILITIES = 0;
  nserv->hasSTARTTLS = 0;
  nserv->hasDATE = 0;
  nserv->hasLIST_NEWSGROUPS = 0;
  nserv->hasLISTGROUP = 0;
  nserv->hasLISTGROUPrange = 0;
  nserv->hasOVER = 0;
  FREE (&nserv->authenticators);

  if (mutt_socket_write (conn, "CAPABILITIES\r\n") < 0 ||
      mutt_socket_readln (buf, sizeof (buf), conn) < 0)
    return nntp_connect_error (nserv);

  /* no capabilities */
  if (mutt_strncmp ("101", buf, 3))
    return 1;
  nserv->hasCAPABILITIES = 1;

  /* parse capabilities */
  do
  {
    if (mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (!mutt_strcmp ("STARTTLS", buf))
      nserv->hasSTARTTLS = 1;
    else if (!mutt_strcmp ("MODE-READER", buf))
      mode_reader = 1;
    else if (!mutt_strcmp ("READER", buf))
    {
      nserv->hasDATE = 1;
      nserv->hasLISTGROUP = 1;
      nserv->hasLISTGROUPrange = 1;
    }
    else if (!mutt_strncmp ("AUTHINFO ", buf, 9))
    {
      safe_strcat (buf, sizeof (buf), " ");
      strfcpy (authinfo, buf + 8, sizeof (authinfo));
    }
#ifdef USE_SASL
    else if (!mutt_strncmp ("SASL ", buf, 5))
    {
      char *p = buf + 5;
      while (*p == ' ')
	p++;
      nserv->authenticators = safe_strdup (p);
    }
#endif
    else if (!mutt_strcmp ("OVER", buf))
      nserv->hasOVER = 1;
    else if (!mutt_strncmp ("LIST ", buf, 5))
    {
      char *p = strstr (buf, " NEWSGROUPS");
      if (p)
      {
	p += 11;
	if (*p == '\0' || *p == ' ')
	  nserv->hasLIST_NEWSGROUPS = 1;
      }
    }
  } while (mutt_strcmp (".", buf));
  *buf = '\0';
#ifdef USE_SASL
  if (nserv->authenticators && strcasestr (authinfo, " SASL "))
    strfcpy (buf, nserv->authenticators, sizeof (buf));
#endif
  if (strcasestr (authinfo, " USER "))
  {
    if (*buf)
      safe_strcat (buf, sizeof (buf), " ");
    safe_strcat (buf, sizeof (buf), "USER");
  }
  mutt_str_replace (&nserv->authenticators, buf);

  /* current mode is reader */
  if (nserv->hasDATE)
    return 0;

  /* server is mode-switching, need to switch to reader mode */
  if (mode_reader)
    return 1;

  mutt_socket_close (conn);
  nserv->status = NNTP_BYE;
  mutt_error _("Server doesn't support reader mode.");
  mutt_sleep (2);
  return -1;
}

char *OverviewFmt =
	"Subject:\0"
	"From:\0"
	"Date:\0"
	"Message-ID:\0"
	"References:\0"
	"Content-Length:\0"
	"Lines:\0"
	"\0";

/* Detect supported commands */
static int nntp_attempt_features (NNTP_SERVER *nserv)
{
  CONNECTION *conn = nserv->conn;
  char buf[LONG_STRING];

  /* no CAPABILITIES, trying DATE, LISTGROUP, LIST NEWSGROUPS */
  if (!nserv->hasCAPABILITIES)
  {
    if (mutt_socket_write (conn, "DATE\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (mutt_strncmp ("500", buf, 3))
      nserv->hasDATE = 1;

    if (mutt_socket_write (conn, "LISTGROUP\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (mutt_strncmp ("500", buf, 3))
      nserv->hasLISTGROUP = 1;

    if (mutt_socket_write (conn, "LIST NEWSGROUPS +\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (mutt_strncmp ("500", buf, 3))
      nserv->hasLIST_NEWSGROUPS = 1;
    if (!mutt_strncmp ("215", buf, 3))
    {
      do
      {
	if (mutt_socket_readln (buf, sizeof (buf), conn) < 0)
	  return nntp_connect_error (nserv);
      } while (mutt_strcmp (".", buf));
    }
  }

  /* no LIST NEWSGROUPS, trying XGTITLE */
  if (!nserv->hasLIST_NEWSGROUPS)
  {
    if (mutt_socket_write (conn, "XGTITLE\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (mutt_strncmp ("500", buf, 3))
      nserv->hasXGTITLE = 1;
  }

  /* no OVER, trying XOVER */
  if (!nserv->hasOVER)
  {
    if (mutt_socket_write (conn, "XOVER\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (mutt_strncmp ("500", buf, 3))
      nserv->hasXOVER = 1;
  }

  /* trying LIST OVERVIEW.FMT */
  if (nserv->hasOVER || nserv->hasXOVER)
  {
    if (mutt_socket_write (conn, "LIST OVERVIEW.FMT\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (mutt_strncmp ("215", buf, 3))
      nserv->overview_fmt = OverviewFmt;
    else
    {
      int chunk, cont = 0;
      size_t buflen = 2 * LONG_STRING, off = 0, b = 0;

      if (nserv->overview_fmt)
	FREE (&nserv->overview_fmt);
      nserv->overview_fmt = safe_malloc (buflen);

      while (1)
      {
	if (buflen - off < LONG_STRING)
	{
	  buflen *= 2;
	  safe_realloc (&nserv->overview_fmt, buflen);
	}

	chunk = mutt_socket_readln (nserv->overview_fmt + off,
				    buflen - off, conn);
	if (chunk < 0)
	{
	  FREE (&nserv->overview_fmt);
	  return nntp_connect_error (nserv);
	}

	if (!cont && !mutt_strcmp (".", nserv->overview_fmt + off))
	  break;

	cont = chunk >= buflen - off ? 1 : 0;
	off += strlen (nserv->overview_fmt + off);
	if (!cont)
	{
	  char *colon;

	  if (nserv->overview_fmt[b] == ':')
	  {
	    memmove (nserv->overview_fmt + b,
		     nserv->overview_fmt + b + 1, off - b - 1);
	    nserv->overview_fmt[off - 1] = ':';
	  }
	  colon = strchr (nserv->overview_fmt + b, ':');
	  if (!colon)
	    nserv->overview_fmt[off++] = ':';
	  else if (strcmp (colon + 1, "full"))
	    off = colon + 1 - nserv->overview_fmt;
	  if (!strcasecmp (nserv->overview_fmt + b, "Bytes:"))
	  {
	    strcpy (nserv->overview_fmt + b, "Content-Length:");
	    off = b + strlen (nserv->overview_fmt + b);
	  }
	  nserv->overview_fmt[off++] = '\0';
	  b = off;
	}
      }
      nserv->overview_fmt[off++] = '\0';
      safe_realloc (&nserv->overview_fmt, off);
    }
  }
  return 0;
}

/* Get login, password and authenticate */
static int nntp_auth (NNTP_SERVER *nserv)
{
  CONNECTION *conn = nserv->conn;
  char buf[LONG_STRING];
  char authenticators[LONG_STRING] = "USER";
  char *method, *a, *p;
  unsigned char flags = conn->account.flags;

  while (1)
  {
    /* get login and password */
    if (mutt_account_getuser (&conn->account) || !conn->account.user[0] ||
	mutt_account_getpass (&conn->account) || !conn->account.pass[0])
      break;

    /* get list of authenticators */
    if (NntpAuthenticators && *NntpAuthenticators)
      strfcpy (authenticators, NntpAuthenticators, sizeof (authenticators));
    else if (nserv->hasCAPABILITIES)
    {
      strfcpy (authenticators, NONULL (nserv->authenticators),
	       sizeof (authenticators));
      p = authenticators;
      while (*p)
      {
	if (*p == ' ')
	  *p = ':';
	p++;
      }
    }
    p = authenticators;
    while (*p)
    {
      *p = ascii_toupper (*p);
      p++;
    }

    dprint (1, (debugfile,
		"nntp_auth: available methods: %s\n", nserv->authenticators));
    a = authenticators;
    while (1)
    {
      if (!a)
      {
	mutt_error _("No authenticators available");
	mutt_sleep (2);
	break;
      }

      method = a;
      a = strchr (a, ':');
      if (a)
	*a++ = '\0';

      /* check authenticator */
      if (nserv->hasCAPABILITIES)
      {
	char *m;

	if (!nserv->authenticators)
	  continue;
	m = strcasestr (nserv->authenticators, method);
	if (!m)
	  continue;
	if (m > nserv->authenticators && *(m - 1) != ' ')
	  continue;
	m += strlen (method);
	if (*m != '\0' && *m != ' ')
	  continue;
      }
      dprint (1, (debugfile, "nntp_auth: trying method %s\n", method));

      /* AUTHINFO USER authentication */
      if (!strcmp (method, "USER"))
      {
	mutt_message (_("Authenticating (%s)..."), method);
	snprintf (buf, sizeof (buf), "AUTHINFO USER %s\r\n", conn->account.user);
	if (mutt_socket_write (conn, buf) < 0 ||
	    mutt_socket_readln (buf, sizeof (buf), conn) < 0)
	  break;

	/* authenticated, password is not required */
	if (!mutt_strncmp ("281", buf, 3))
	  return 0;

	/* username accepted, sending password */
	if (!mutt_strncmp ("381", buf, 3))
	{
#ifdef DEBUG
	  if (debuglevel < M_SOCK_LOG_FULL)
	    dprint (M_SOCK_LOG_CMD, (debugfile,
		    "%d> AUTHINFO PASS *\n", conn->fd));
#endif
	  snprintf (buf, sizeof (buf), "AUTHINFO PASS %s\r\n",
		    conn->account.pass);
	  if (mutt_socket_write_d (conn, buf, -1, M_SOCK_LOG_FULL) < 0 ||
	      mutt_socket_readln (buf, sizeof (buf), conn) < 0)
	  break;

	  /* authenticated */
	  if (!mutt_strncmp ("281", buf, 3))
	    return 0;
	}

	/* server doesn't support AUTHINFO USER, trying next method */
	if (*buf == '5')
	  continue;
      }

      else
      {
#ifdef USE_SASL
	sasl_conn_t *saslconn;
	sasl_interact_t *interaction = NULL;
	int rc;
	char inbuf[LONG_STRING] = "";
	const char *mech;
	const char *client_out = NULL;
	unsigned int client_len, len;

	if (mutt_sasl_client_new (conn, &saslconn) < 0)
	{
	  dprint (1, (debugfile,
		  "nntp_auth: error allocating SASL connection.\n"));
	  continue;
	}

	while (1)
	{
	  rc = sasl_client_start (saslconn, method, &interaction,
				  &client_out, &client_len, &mech);
	  if (rc != SASL_INTERACT)
	    break;
	  mutt_sasl_interact (interaction);
	}
	if (rc != SASL_OK && rc != SASL_CONTINUE)
	{
	  sasl_dispose (&saslconn);
	  dprint (1, (debugfile,
		  "nntp_auth: error starting SASL authentication exchange.\n"));
	  continue;
	}

	mutt_message (_("Authenticating (%s)..."), method);
	snprintf (buf, sizeof (buf), "AUTHINFO SASL %s", method);

	/* looping protocol */
	while (rc == SASL_CONTINUE || (rc == SASL_OK && client_len))
	{
	  /* send out client response */
	  if (client_len)
	  {
#ifdef DEBUG
	    if (debuglevel >= M_SOCK_LOG_FULL)
	    {
	      char tmp[LONG_STRING];
	      memcpy (tmp, client_out, client_len);
	      for (p = tmp; p < tmp + client_len; p++)
	      {
		if (*p == '\0')
		  *p = '.';
	      }
	      *p = '\0';
	      dprint (1, (debugfile, "SASL> %s\n", tmp));
	    }
#endif

	    if (*buf)
	      safe_strcat (buf, sizeof (buf), " ");
	    len = strlen (buf);
	    if (sasl_encode64 (client_out, client_len,
		buf + len, sizeof (buf) - len, &len) != SASL_OK)
	    {
	      dprint (1, (debugfile,
		      "nntp_auth: error base64-encoding client response.\n"));
	      break;
	    }
	  }

	  safe_strcat (buf, sizeof (buf), "\r\n");
#ifdef DEBUG
	  if (debuglevel < M_SOCK_LOG_FULL)
	  {
	    if (strchr (buf, ' '))
	      dprint (M_SOCK_LOG_CMD, (debugfile, "%d> AUTHINFO SASL %s%s\n",
		      conn->fd, method, client_len ? " sasl_data" : ""));
	    else
	      dprint (M_SOCK_LOG_CMD, (debugfile, "%d> sasl_data\n", conn->fd));
	  }
#endif
	  client_len = 0;
	  if (mutt_socket_write_d (conn, buf, -1, M_SOCK_LOG_FULL) < 0 ||
	      mutt_socket_readln_d (inbuf, sizeof (inbuf), conn, M_SOCK_LOG_FULL) < 0)
	    break;
	  if (mutt_strncmp (inbuf, "283 ", 4) &&
	      mutt_strncmp (inbuf, "383 ", 4))
	  {
#ifdef DEBUG
	    if (debuglevel < M_SOCK_LOG_FULL)
	      dprint (M_SOCK_LOG_CMD, (debugfile, "%d< %s\n", conn->fd, inbuf));
#endif
	    break;
	  }
#ifdef DEBUG
	  if (debuglevel < M_SOCK_LOG_FULL)
	  {
	    inbuf[3] = '\0';
	    dprint (M_SOCK_LOG_CMD, (debugfile,
		    "%d< %s sasl_data\n", conn->fd, inbuf));
	  }
#endif

	  if (!strcmp ("=", inbuf + 4))
	    len = 0;
	  else if (sasl_decode64 (inbuf + 4, strlen (inbuf + 4),
		   buf, sizeof (buf) - 1, &len) != SASL_OK)
	  {
	    dprint (1, (debugfile,
		    "nntp_auth: error base64-decoding server response.\n"));
	    break;
	  }
#ifdef DEBUG
	  else if (debuglevel >= M_SOCK_LOG_FULL)
	  {
	    char tmp[LONG_STRING];
	    memcpy (tmp, buf, len);
	    for (p = tmp; p < tmp + len; p++)
	    {
	      if (*p == '\0')
		*p = '.';
	    }
	    *p = '\0';
	    dprint (1, (debugfile, "SASL< %s\n", tmp));
	  }
#endif

	  while (1)
	  {
	    rc = sasl_client_step (saslconn, buf, len,
				   &interaction, &client_out, &client_len);
	    if (rc != SASL_INTERACT)
	      break;
	    mutt_sasl_interact (interaction);
	  }
	  if (*inbuf != '3')
	    break;

	  *buf = '\0';
	} /* looping protocol */

	if (rc == SASL_OK && client_len == 0 && *inbuf == '2')
	{
	  mutt_sasl_setup_conn (conn, saslconn);
	  return 0;
	}

	/* terminate SASL sessoin */
	sasl_dispose (&saslconn);
	if (conn->fd < 0)
	  break;
	if (!mutt_strncmp (inbuf, "383 ", 4))
	{
	  if (mutt_socket_write (conn, "*\r\n") < 0 ||
	      mutt_socket_readln (inbuf, sizeof (inbuf), conn) < 0)
	    break;
	}

	/* server doesn't support AUTHINFO SASL, trying next method */
	if (*inbuf == '5')
	  continue;
#else
	continue;
#endif /* USE_SASL */
      }

      mutt_error (_("%s authentication failed."), method);
      mutt_sleep (2);
      break;
    }
    break;
  }

  /* error */
  nserv->status = NNTP_BYE;
  conn->account.flags = flags;
  if (conn->fd < 0)
  {
    mutt_error _("Server closed connection!");
    mutt_sleep (2);
  }
  else
    mutt_socket_close (conn);
  return -1;
}

/* Connect to server, authenticate and get capabilities */
int nntp_open_connection (NNTP_SERVER *nserv)
{
  CONNECTION *conn = nserv->conn;
  char buf[STRING];
  int cap;
  unsigned int posting = 0, auth = 1;

  if (nserv->status == NNTP_OK)
    return 0;
  if (nserv->status == NNTP_BYE)
    return -1;
  nserv->status = NNTP_NONE;

  if (mutt_socket_open (conn) < 0)
    return -1;

  if (mutt_socket_readln (buf, sizeof (buf), conn) < 0)
    return nntp_connect_error (nserv);

  if (!mutt_strncmp ("200", buf, 3))
    posting = 1;
  else if (mutt_strncmp ("201", buf, 3))
  {
    mutt_socket_close (conn);
    mutt_remove_trailing_ws (buf);
    mutt_error ("%s", buf);
    mutt_sleep (2);
    return -1;
  }

  /* get initial capabilities */
  cap = nntp_capabilities (nserv);
  if (cap < 0)
    return -1;

  /* tell news server to switch to mode reader if it isn't so */
  if (cap > 0)
  {
    if (mutt_socket_write (conn, "MODE READER\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);

    if (!mutt_strncmp ("200", buf, 3))
      posting = 1;
    else if (!mutt_strncmp ("201", buf, 3))
      posting = 0;
    /* error if has capabilities, ignore result if no capabilities */
    else if (nserv->hasCAPABILITIES)
    {
      mutt_socket_close (conn);
      mutt_error _("Could not switch to reader mode.");
      mutt_sleep (2);
      return -1;
    }

    /* recheck capabilities after MODE READER */
    if (nserv->hasCAPABILITIES)
    {
      cap = nntp_capabilities (nserv);
      if (cap < 0)
	return -1;
    }
  }

  mutt_message (_("Connected to %s. %s"), conn->account.host,
		posting ? _("Posting is ok.") : _("Posting is NOT ok."));
  mutt_sleep (1);

#if defined(USE_SSL)
  /* Attempt STARTTLS if available and desired. */
  if (nserv->use_tls != 1 && (nserv->hasSTARTTLS || option (OPTSSLFORCETLS)))
  {
    if (nserv->use_tls == 0)
      nserv->use_tls = option (OPTSSLFORCETLS) ||
			query_quadoption (OPT_SSLSTARTTLS,
			_("Secure connection with TLS?")) == M_YES ? 2 : 1;
    if (nserv->use_tls == 2)
    {
      if (mutt_socket_write (conn, "STARTTLS\r\n") < 0 ||
	  mutt_socket_readln (buf, sizeof (buf), conn) < 0)
	return nntp_connect_error (nserv);
      if (mutt_strncmp ("382", buf, 3))
      {
	nserv->use_tls = 0;
	mutt_error ("STARTTLS: %s", buf);
	mutt_sleep (2);
      }
      else if (mutt_ssl_starttls (conn))
      {
	nserv->use_tls = 0;
	nserv->status = NNTP_NONE;
	mutt_socket_close (nserv->conn);
	mutt_error _("Could not negotiate TLS connection");
	mutt_sleep (2);
	return -1;
      }
      else
      {
	/* recheck capabilities after STARTTLS */
	cap = nntp_capabilities (nserv);
	if (cap < 0)
	  return -1;
      }
    }
  }
#endif

  /* authentication required? */
  if (conn->account.flags & M_ACCT_USER)
  {
    if (!conn->account.user[0])
      auth = 0;
  }
  else
  {
    if (mutt_socket_write (conn, "STAT\r\n") < 0 ||
	mutt_socket_readln (buf, sizeof (buf), conn) < 0)
      return nntp_connect_error (nserv);
    if (mutt_strncmp ("480", buf, 3))
      auth = 0;
  }

  /* authenticate */
  if (auth && nntp_auth (nserv) < 0)
      return -1;

  /* get final capabilities after authentication */
  if (nserv->hasCAPABILITIES && (auth || cap > 0))
  {
    cap = nntp_capabilities (nserv);
    if (cap < 0)
      return -1;
    if (cap > 0)
    {
      mutt_socket_close (conn);
      mutt_error _("Could not switch to reader mode.");
      mutt_sleep (2);
      return -1;
    }
  }

  /* attempt features */
  if (nntp_attempt_features (nserv) < 0)
    return -1;

  nserv->status = NNTP_OK;
  return 0;
}

/* Send data from buffer and receive answer to same buffer */
static int nntp_query (NNTP_DATA *nntp_data, char *line, size_t linelen)
{
  NNTP_SERVER *nserv = nntp_data->nserv;
  char buf[LONG_STRING];

  if (nserv->status == NNTP_BYE)
    return -1;

  while (1)
  {
    if (nserv->status == NNTP_OK)
    {
      int rc = 0;

      if (*line)
	rc = mutt_socket_write (nserv->conn, line);
      else if (nntp_data->group)
      {
	snprintf (buf, sizeof (buf), "GROUP %s\r\n", nntp_data->group);
	rc = mutt_socket_write (nserv->conn, buf);
      }
      if (rc >= 0)
	rc = mutt_socket_readln (buf, sizeof (buf), nserv->conn);
      if (rc >= 0)
	break;
    }

    /* reconnect */
    while (1)
    {
      nserv->status = NNTP_NONE;
      if (nntp_open_connection (nserv) == 0)
	break;

      snprintf (buf, sizeof (buf), _("Connection to %s lost. Reconnect?"),
		nserv->conn->account.host);
      if (mutt_yesorno (buf, M_YES) != M_YES)
      {
	nserv->status = NNTP_BYE;
	return -1;
      }
    }

    /* select newsgroup after reconnection */
    if (nntp_data->group)
    {
      snprintf (buf, sizeof (buf), "GROUP %s\r\n", nntp_data->group);
      if (mutt_socket_write (nserv->conn, buf) < 0 ||
	  mutt_socket_readln (buf, sizeof (buf), nserv->conn) < 0)
	return nntp_connect_error (nserv);
    }
    if (!*line)
      break;
  }

  strfcpy (line, buf, linelen);
  return 0;
}

/* This function calls funct(*line, *data) for each received line,
 * funct(NULL, *data) if rewind(*data) needs, exits when fail or done:
 *  0 - success
 *  1 - bad response (answer in query buffer)
 * -1 - conection lost
 * -2 - error in funct(*line, *data) */
static int nntp_fetch_lines (NNTP_DATA *nntp_data, char *query, size_t qlen,
			char *msg, int (*funct) (char *, void *), void *data)
{
  int done = FALSE;
  int rc;

  while (!done)
  {
    char buf[LONG_STRING];
    char *line;
    unsigned int lines = 0;
    size_t off = 0;
    progress_t progress;

    if (msg)
      mutt_progress_init (&progress, msg, M_PROGRESS_MSG, ReadInc, -1);

    strfcpy (buf, query, sizeof (buf));
    if (nntp_query (nntp_data, buf, sizeof (buf)) < 0)
      return -1;
    if (buf[0] != '2')
    {
      strfcpy (query, buf, qlen);
      return 1;
    }

    line = safe_malloc (sizeof (buf));
    rc = 0;

    while (1)
    {
      char *p;
      int chunk = mutt_socket_readln_d (buf, sizeof (buf),
		  nntp_data->nserv->conn, M_SOCK_LOG_HDR);
      if (chunk < 0)
      {
	nntp_data->nserv->status = NNTP_NONE;
	break;
      }

      p = buf;
      if (!off && buf[0] == '.')
      {
	if (buf[1] == '\0')
	{
	  done = TRUE;
	  break;
	}
	if (buf[1] == '.')
	  p++;
      }

      strfcpy (line + off, p, sizeof (buf));

      if (chunk >= sizeof (buf))
	off += strlen (p);
      else
      {
	if (msg)
	  mutt_progress_update (&progress, ++lines, -1);

	if (rc == 0 && funct (line, data) < 0)
	  rc = -2;
	off = 0;
      }

      safe_realloc (&line, off + sizeof (buf));
    }
    FREE (&line);
    funct (NULL, data);
  }
  return rc;
}

/* Parse newsgroup description */
static int fetch_description (char *line, void *data)
{
  NNTP_SERVER *nserv = data;
  NNTP_DATA *nntp_data;
  char *desc;

  if (!line)
    return 0;

  desc = strpbrk (line, " \t");
  if (desc)
  {
    *desc++ = '\0';
    desc += strspn (desc, " \t");
  }
  else
    desc = strchr (line, '\0');

  nntp_data = hash_find (nserv->groups_hash, line);
  if (nntp_data && mutt_strcmp (desc, nntp_data->desc))
  {
    mutt_str_replace (&nntp_data->desc, desc);
    dprint (2, (debugfile, "group: %s, desc: %s\n", line, desc));
  }
  return 0;
}

/* Fetch newsgroups descriptions.
 * Returns the same code as nntp_fetch_lines() */
static int get_description (NNTP_DATA *nntp_data, char *wildmat, char *msg)
{
  NNTP_SERVER *nserv;
  char buf[STRING];
  char *cmd;
  int rc;

  /* get newsgroup description, if possible */
  nserv = nntp_data->nserv;
  if (!wildmat)
    wildmat = nntp_data->group;
  if (nserv->hasLIST_NEWSGROUPS)
    cmd = "LIST NEWSGROUPS";
  else if (nserv->hasXGTITLE)
    cmd = "XGTITLE";
  else
    return 0;

  snprintf (buf, sizeof (buf), "%s %s\r\n", cmd, wildmat);
  rc = nntp_fetch_lines (nntp_data, buf, sizeof (buf), msg,
			 fetch_description, nserv);
  if (rc > 0)
  {
    mutt_error ("%s: %s", cmd, buf);
    mutt_sleep (2);
  }
  return rc;
}

/* Update read flag and set article number if empty */
static void nntp_parse_xref (CONTEXT *ctx, HEADER *hdr)
{
  NNTP_DATA *nntp_data = ctx->data;
  char *buf, *p;

  buf = p = safe_strdup (hdr->env->xref);
  while (p)
  {
    char *grp, *colon;
    anum_t anum;

    /* skip to next word */
    p += strspn (p, " \t");
    grp = p;

    /* skip to end of word */
    p = strpbrk (p, " \t");
    if (p)
      *p++ = '\0';

    /* find colon */
    colon = strchr (grp, ':');
    if (!colon)
      continue;
    *colon++ = '\0';
    if (sscanf (colon, ANUM, &anum) != 1)
      continue;

    nntp_article_status (ctx, hdr, grp, anum);
    if (hdr && !NHDR (hdr)->article_num && !mutt_strcmp (nntp_data->group, grp))
      NHDR (hdr)->article_num = anum;
  }
  FREE (&buf);
}

/* Write line to temporarily file */
static int fetch_tempfile (char *line, void *data)
{
  FILE *fp = data;

  if (!line)
    rewind (fp);
  else if (fputs (line, fp) == EOF || fputc ('\n', fp) == EOF)
    return -1;
  return 0;
}

typedef struct
{
  CONTEXT *ctx;
  anum_t first;
  anum_t last;
  int restore;
  unsigned char *messages;
  progress_t progress;
#ifdef USE_HCACHE
  header_cache_t *hc;
#endif
} FETCH_CTX;

/* Parse article number */
static int fetch_numbers (char *line, void *data)
{
  FETCH_CTX *fc = data;
  anum_t anum;

  if (!line)
    return 0;
  if (sscanf (line, ANUM, &anum) != 1)
    return 0;
  if (anum < fc->first || anum > fc->last)
    return 0;
  fc->messages[anum - fc->first] = 1;
  return 0;
}

/* Parse overview line */
static int parse_overview_line (char *line, void *data)
{
  FETCH_CTX *fc = data;
  CONTEXT *ctx = fc->ctx;
  NNTP_DATA *nntp_data = ctx->data;
  HEADER *hdr;
  FILE *fp;
  char tempfile[_POSIX_PATH_MAX];
  char *header, *field;
  int save = 1;
  anum_t anum;

  if (!line)
    return 0;

  /* parse article number */
  field = strchr (line, '\t');
  if (field)
    *field++ = '\0';
  if (sscanf (line, ANUM, &anum) != 1)
    return 0;
  dprint (2, (debugfile, "parse_overview_line: " ANUM "\n", anum));

  /* out of bounds */
  if (anum < fc->first || anum > fc->last)
    return 0;

  /* not in LISTGROUP */
  if (!fc->messages[anum - fc->first])
  {
    /* progress */
    if (!ctx->quiet)
      mutt_progress_update (&fc->progress, anum - fc->first + 1, -1);
    return 0;
  }

  /* convert overview line to header */
  mutt_mktemp (tempfile, sizeof (tempfile));
  fp = safe_fopen (tempfile, "w+");
  if (!fp)
    return -1;

  header = nntp_data->nserv->overview_fmt;
  while (field)
  {
    char *b = field;

    if (*header)
    {
      if (strstr (header, ":full") == NULL && fputs (header, fp) == EOF)
      {
	fclose (fp);
	unlink (tempfile);
	return -1;
      }
      header = strchr (header, '\0') + 1;
    }

    field = strchr (field, '\t');
    if (field)
      *field++ = '\0';
    if (fputs (b, fp) == EOF || fputc ('\n', fp) == EOF)
    {
      fclose (fp);
      unlink (tempfile);
      return -1;
    }
  }
  rewind (fp);

  /* allocate memory for headers */
  if (ctx->msgcount >= ctx->hdrmax)
    mx_alloc_memory (ctx);

  /* parse header */
  hdr = ctx->hdrs[ctx->msgcount] = mutt_new_header ();
  hdr->env = mutt_read_rfc822_header (fp, hdr, 0, 0);
  hdr->env->newsgroups = safe_strdup (nntp_data->group);
  hdr->received = hdr->date_sent;
  fclose (fp);
  unlink (tempfile);

#ifdef USE_HCACHE
  if (fc->hc)
  {
    void *hdata;
    char buf[16];

    /* try to replace with header from cache */
    snprintf (buf, sizeof (buf), "%d", anum);
    hdata = mutt_hcache_fetch (fc->hc, buf, strlen);
    if (hdata)
    {
      dprint (2, (debugfile,
		  "parse_overview_line: mutt_hcache_fetch %s\n", buf));
      mutt_free_header (&hdr);
      ctx->hdrs[ctx->msgcount] =
      hdr = mutt_hcache_restore (hdata, NULL);
      FREE (&hdata);
      hdr->data = 0;
      hdr->read = 0;
      hdr->old = 0;

      /* skip header marked as deleted in cache */
      if (hdr->deleted && !fc->restore)
      {
	if (nntp_data->bcache)
	{
	  dprint (2, (debugfile,
		      "parse_overview_line: mutt_bcache_del %s\n", buf));
	  mutt_bcache_del (nntp_data->bcache, buf);
	}
	save = 0;
      }
    }

    /* not chached yet, store header */
    else
    {
      dprint (2, (debugfile,
		  "parse_overview_line: mutt_hcache_store %s\n", buf));
      mutt_hcache_store (fc->hc, buf, hdr, 0, strlen, M_GENERATE_UIDVALIDITY);
    }
  }
#endif

  if (save)
  {
    hdr->index = ctx->msgcount++;
    hdr->read = 0;
    hdr->old = 0;
    hdr->deleted = 0;
    hdr->data = safe_calloc (1, sizeof (NNTP_HEADER_DATA));
    NHDR (hdr)->article_num = anum;
    if (fc->restore)
      hdr->changed = 1;
    else
    {
      nntp_article_status (ctx, hdr, NULL, anum);
      if (!hdr->read)
	nntp_parse_xref (ctx, hdr);
    }
    if (anum > nntp_data->lastLoaded)
      nntp_data->lastLoaded = anum;
  }
  else
    mutt_free_header (&hdr);

  /* progress */
  if (!ctx->quiet)
    mutt_progress_update (&fc->progress, anum - fc->first + 1, -1);
  return 0;
}

/* Fetch headers */
static int nntp_fetch_headers (CONTEXT *ctx, void *hc,
			       anum_t first, anum_t last, int restore)
{
  NNTP_DATA *nntp_data = ctx->data;
  FETCH_CTX fc;
  HEADER *hdr;
  char buf[HUGE_STRING];
  int rc = 0;
  int oldmsgcount = ctx->msgcount;
  anum_t current;
  anum_t first_over = first;
#ifdef USE_HCACHE
  void *hdata;
#endif

  /* if empty group or nothing to do */
  if (!last || first > last)
    return 0;

  /* init fetch context */
  fc.ctx = ctx;
  fc.first = first;
  fc.last = last;
  fc.restore = restore;
  fc.messages = safe_calloc (last - first + 1, sizeof (unsigned char));
#ifdef USE_HCACHE
  fc.hc = hc;
#endif

  /* fetch list of articles */
  if (option (OPTLISTGROUP) && nntp_data->nserv->hasLISTGROUP &&
      !nntp_data->deleted)
  {
    if (!ctx->quiet)
      mutt_message _("Fetching list of articles...");
    if (nntp_data->nserv->hasLISTGROUPrange)
      snprintf (buf, sizeof (buf), "LISTGROUP %s %d-%d\r\n", nntp_data->group,
		first, last);
    else
      snprintf (buf, sizeof (buf), "LISTGROUP %s\r\n", nntp_data->group);
    rc = nntp_fetch_lines (nntp_data, buf, sizeof (buf), NULL,
			   fetch_numbers, &fc);
    if (rc > 0)
    {
      mutt_error ("LISTGROUP: %s", buf);
      mutt_sleep (2);
    }
    if (rc == 0)
    {
      for (current = first; current <= last && rc == 0; current++)
      {
	if (fc.messages[current - first])
	  continue;

	snprintf (buf, sizeof (buf), "%d", current);
	if (nntp_data->bcache)
	{
	  dprint (2, (debugfile,
		      "nntp_fetch_headers: mutt_bcache_del %s\n", buf));
	  mutt_bcache_del (nntp_data->bcache, buf);
	}

#ifdef USE_HCACHE
	if (fc.hc)
	{
	  dprint (2, (debugfile,
		      "nntp_fetch_headers: mutt_hcache_delete %s\n", buf));
	  mutt_hcache_delete (fc.hc, buf, strlen);
	}
#endif
      }
    }
  }
  else
    for (current = first; current <= last; current++)
      fc.messages[current - first] = 1;

  /* fetching header from cache or server, or fallback to fetch overview */
  if (!ctx->quiet)
    mutt_progress_init (&fc.progress, _("Fetching message headers..."),
			M_PROGRESS_MSG, ReadInc, last - first + 1);
  for (current = first; current <= last && rc == 0; current++)
  {
    if (!ctx->quiet)
      mutt_progress_update (&fc.progress, current - first + 1, -1);

#ifdef USE_HCACHE
    snprintf (buf, sizeof (buf), "%d", current);
#endif

    /* delete header from cache that does not exist on server */
    if (!fc.messages[current - first])
      continue;

    /* allocate memory for headers */
    if (ctx->msgcount >= ctx->hdrmax)
      mx_alloc_memory (ctx);

#ifdef USE_HCACHE
    /* try to fetch header from cache */
    hdata = mutt_hcache_fetch (fc.hc, buf, strlen);
    if (hdata)
    {
      dprint (2, (debugfile,
		  "nntp_fetch_headers: mutt_hcache_fetch %s\n", buf));
      ctx->hdrs[ctx->msgcount] =
      hdr = mutt_hcache_restore (hdata, NULL);
      FREE (&hdata);
      hdr->data = 0;

      /* skip header marked as deleted in cache */
      if (hdr->deleted && !restore)
      {
	mutt_free_header (&hdr);
	if (nntp_data->bcache)
	{
	  dprint (2, (debugfile,
		      "nntp_fetch_headers: mutt_bcache_del %s\n", buf));
	  mutt_bcache_del (nntp_data->bcache, buf);
	}
	continue;
      }

      hdr->read = 0;
      hdr->old = 0;
    }
    else
#endif

    /* don't try to fetch header from removed newsgroup */
    if (nntp_data->deleted)
      continue;

    /* fallback to fetch overview */
    else if (nntp_data->nserv->hasOVER || nntp_data->nserv->hasXOVER)
      if (option (OPTLISTGROUP) && nntp_data->nserv->hasLISTGROUP)
	break;
      else
	continue;

    /* fetch header from server */
    else
    {
      FILE *fp;
      char tempfile[_POSIX_PATH_MAX];

      mutt_mktemp (tempfile, sizeof (tempfile));
      fp = safe_fopen (tempfile, "w+");
      if (!fp)
      {
	mutt_perror (tempfile);
	mutt_sleep (2);
	unlink (tempfile);
	rc = -1;
	break;
      }

      snprintf (buf, sizeof (buf), "HEAD %d\r\n", current);
      rc = nntp_fetch_lines (nntp_data, buf, sizeof (buf), NULL,
			     fetch_tempfile, fp);
      if (rc)
      {
	fclose (fp);
	unlink (tempfile);
	if (rc < 0)
	  break;

	/* invalid response */
	if (mutt_strncmp ("423", buf, 3))
	{
	  mutt_error ("HEAD: %s", buf);
	  mutt_sleep (2);
	  break;
	}

	/* no such article */
	if (nntp_data->bcache)
	{
	  snprintf (buf, sizeof (buf), "%d", current);
	  dprint (2, (debugfile,
		      "nntp_fetch_headers: mutt_bcache_del %s\n", buf));
	  mutt_bcache_del (nntp_data->bcache, buf);
	}
	rc = 0;
	continue;
      }

      /* parse header */
      hdr = ctx->hdrs[ctx->msgcount] = mutt_new_header ();
      hdr->env = mutt_read_rfc822_header (fp, hdr, 0, 0);
      hdr->received = hdr->date_sent;
      fclose (fp);
      unlink (tempfile);
    }

    /* save header in context */
    hdr->index = ctx->msgcount++;
    hdr->read = 0;
    hdr->old = 0;
    hdr->deleted = 0;
    hdr->data = safe_calloc (1, sizeof (NNTP_HEADER_DATA));
    NHDR (hdr)->article_num = current;
    if (restore)
      hdr->changed = 1;
    else
    {
      nntp_article_status (ctx, hdr, NULL, NHDR (hdr)->article_num);
      if (!hdr->read)
	nntp_parse_xref (ctx, hdr);
    }
    if (current > nntp_data->lastLoaded)
      nntp_data->lastLoaded = current;
    first_over = current + 1;
  }

  if (!option (OPTLISTGROUP) || !nntp_data->nserv->hasLISTGROUP)
    current = first_over;

  /* fetch overview information */
  if (current <= last && rc == 0 && !nntp_data->deleted) {
    char *cmd = nntp_data->nserv->hasOVER ? "OVER" : "XOVER";
    snprintf (buf, sizeof (buf), "%s %d-%d\r\n", cmd, current, last);
    rc = nntp_fetch_lines (nntp_data, buf, sizeof (buf), NULL,
	 parse_overview_line, &fc);
    if (rc > 0)
    {
      mutt_error ("%s: %s", cmd, buf);
      mutt_sleep (2);
    }
  }

  if (ctx->msgcount > oldmsgcount)
    mx_update_context (ctx, ctx->msgcount - oldmsgcount);

  FREE (&fc.messages);
  if (rc != 0)
    return -1;
  mutt_clear_error ();
  return 0;
}

/* Open newsgroup */
int nntp_open_mailbox (CONTEXT *ctx)
{
  NNTP_SERVER *nserv;
  NNTP_DATA *nntp_data;
  char buf[HUGE_STRING];
  char server[LONG_STRING];
  char *group;
  int rc;
  void *hc = NULL;
  anum_t first, last, count = 0;
  ciss_url_t url;

  strfcpy (buf, ctx->path, sizeof (buf));
  if (url_parse_ciss (&url, buf) < 0 || !url.path ||
     !(url.scheme == U_NNTP || url.scheme == U_NNTPS))
  {
    mutt_error (_("%s is an invalid newsgroup specification!"), ctx->path);
    mutt_sleep (2);
    return -1;
  }

  group = url.path;
  url.path = strchr (url.path, '\0');
  url_ciss_tostring (&url, server, sizeof (server), 0);
  nserv = nntp_select_server (server, 1);
  if (!nserv)
    return -1;
  CurrentNewsSrv = nserv;

  /* find news group data structure */
  nntp_data = hash_find (nserv->groups_hash, group);
  if (!nntp_data)
  {
    nntp_newsrc_close (nserv);
    mutt_error (_("Newsgroup %s not found on the server."), group);
    mutt_sleep (2);
    return -1;
  }

  mutt_bit_unset (ctx->rights, M_ACL_INSERT);
  if (!nntp_data->newsrc_ent && !nntp_data->subscribed &&
      !option (OPTSAVEUNSUB))
    ctx->readonly = 1;

  /* select newsgroup */
  mutt_message (_("Selecting %s..."), group);
  buf[0] = '\0';
  if (nntp_query (nntp_data, buf, sizeof (buf)) < 0)
  {
    nntp_newsrc_close (nserv);
    return -1;
  }

  /* newsgroup not found, remove it */
  if (!mutt_strncmp ("411", buf, 3))
  {
    mutt_error (_("Newsgroup %s has been removed from the server."),
		nntp_data->group);
    if (!nntp_data->deleted)
    {
      nntp_data->deleted = 1;
      nntp_active_save_cache (nserv);
    }
    if (nntp_data->newsrc_ent && !nntp_data->subscribed &&
	!option (OPTSAVEUNSUB))
    {
      FREE (&nntp_data->newsrc_ent);
      nntp_data->newsrc_len = 0;
      nntp_delete_group_cache (nntp_data);
      nntp_newsrc_update (nserv);
    }
    mutt_sleep (2);
  }

  /* parse newsgroup info */
  else {
    if (sscanf (buf, "211 " ANUM " " ANUM " " ANUM, &count, &first, &last) != 3)
    {
      nntp_newsrc_close (nserv);
      mutt_error ("GROUP: %s", buf);
      mutt_sleep (2);
      return -1;
    }
    nntp_data->firstMessage = first;
    nntp_data->lastMessage = last;
    nntp_data->deleted = 0;

    /* get description if empty */
    if (option (OPTLOADDESC) && !nntp_data->desc)
    {
      if (get_description (nntp_data, NULL, NULL) < 0)
      {
	nntp_newsrc_close (nserv);
	return -1;
      }
      if (nntp_data->desc)
	nntp_active_save_cache (nserv);
    }
  }

  time (&nserv->check_time);
  ctx->data = nntp_data;
  ctx->mx_close = nntp_fastclose_mailbox;
  if (!nntp_data->bcache && (nntp_data->newsrc_ent ||
      nntp_data->subscribed || option (OPTSAVEUNSUB)))
    nntp_data->bcache = mutt_bcache_open (&nserv->conn->account,
			nntp_data->group);

  /* strip off extra articles if adding context is greater than $nntp_context */
  first = nntp_data->firstMessage;
  if (NntpContext && nntp_data->lastMessage - first + 1 > NntpContext)
    first = nntp_data->lastMessage - NntpContext + 1;
  nntp_data->lastLoaded = first ? first - 1 : 0;
  count = nntp_data->firstMessage;
  nntp_data->firstMessage = first;
  nntp_bcache_update (nntp_data);
  nntp_data->firstMessage = count;
#ifdef USE_HCACHE
  hc = nntp_hcache_open (nntp_data);
  nntp_hcache_update (nntp_data, hc);
#endif
  if (!hc)
  {
    mutt_bit_unset (ctx->rights, M_ACL_WRITE);
    mutt_bit_unset (ctx->rights, M_ACL_DELETE);
  }
  nntp_newsrc_close (nserv);
  rc = nntp_fetch_headers (ctx, hc, first, nntp_data->lastMessage, 0);
#ifdef USE_HCACHE
  mutt_hcache_close (hc);
#endif
  if (rc < 0)
    return -1;
  nntp_data->lastLoaded = nntp_data->lastMessage;
  nserv->newsrc_modified = 0;
  return 0;
}

/* Fetch message */
int nntp_fetch_message (MESSAGE *msg, CONTEXT *ctx, int msgno)
{
  NNTP_DATA *nntp_data = ctx->data;
  NNTP_ACACHE *acache;
  HEADER *hdr = ctx->hdrs[msgno];
  char buf[_POSIX_PATH_MAX];
  char article[16];
  char *fetch_msg = _("Fetching message...");
  int rc;

  /* try to get article from cache */
  acache = &nntp_data->acache[hdr->index % NNTP_ACACHE_LEN];
  if (acache->path)
  {
    if (acache->index == hdr->index)
    {
      msg->fp = fopen (acache->path, "r");
      if (msg->fp)
	return 0;
    }
    /* clear previous entry */
    else
    {
      unlink (acache->path);
      FREE (&acache->path);
    }
  }
  snprintf (article, sizeof (article), "%d", NHDR (hdr)->article_num);
  msg->fp = mutt_bcache_get (nntp_data->bcache, article);
  if (msg->fp)
  {
    if (NHDR (hdr)->parsed)
      return 0;
  }
  else
  {
    /* don't try to fetch article from removed newsgroup */
    if (nntp_data->deleted)
      return -1;

    /* create new cache file */
    mutt_message (fetch_msg);
    msg->fp = mutt_bcache_put (nntp_data->bcache, article, 1);
    if (!msg->fp)
    {
      mutt_mktemp (buf, sizeof (buf));
      acache->path = safe_strdup (buf);
      acache->index = hdr->index;
      msg->fp = safe_fopen (acache->path, "w+");
      if (!msg->fp)
      {
	mutt_perror (acache->path);
	unlink (acache->path);
	FREE (&acache->path);
	return -1;
      }
    }

    /* fetch message to cache file */
    snprintf (buf, sizeof (buf), "ARTICLE %s\r\n",
	      NHDR (hdr)->article_num ? article : hdr->env->message_id);
    rc = nntp_fetch_lines (nntp_data, buf, sizeof (buf), fetch_msg,
			   fetch_tempfile, msg->fp);
    if (rc)
    {
      safe_fclose (&msg->fp);
      if (acache->path)
      {
	unlink (acache->path);
	FREE (&acache->path);
      }
      if (rc > 0)
      {
	if (!mutt_strncmp (NHDR (hdr)->article_num ? "423" : "430", buf, 3))
	  mutt_error (_("Article %d not found on the server."),
		      NHDR (hdr)->article_num ? article : hdr->env->message_id);
	else
	  mutt_error ("ARTICLE: %s", buf);
      }
      return -1;
    }

    if (!acache->path)
      mutt_bcache_commit (nntp_data->bcache, article);
  }

  /* replace envelope with new one
   * hash elements must be updated because pointers will be changed */
  if (ctx->id_hash && hdr->env->message_id)
    hash_delete (ctx->id_hash, hdr->env->message_id, hdr, NULL);
  if (ctx->subj_hash && hdr->env->real_subj)
    hash_delete (ctx->subj_hash, hdr->env->real_subj, hdr, NULL);

  mutt_free_envelope (&hdr->env);
  hdr->env = mutt_read_rfc822_header (msg->fp, hdr, 0, 0);

  if (ctx->id_hash && hdr->env->message_id)
    hash_insert (ctx->id_hash, hdr->env->message_id, hdr, 0);
  if (ctx->subj_hash && hdr->env->real_subj)
    hash_insert (ctx->subj_hash, hdr->env->real_subj, hdr, 1);

  /* fix content length */
  fseek (msg->fp, 0, SEEK_END);
  hdr->content->length = ftell (msg->fp) - hdr->content->offset;

  /* this is called in mutt before the open which fetches the message,
   * which is probably wrong, but we just call it again here to handle
   * the problem instead of fixing it */
  NHDR (hdr)->parsed = 1;
  mutt_parse_mime_message (ctx, hdr);

  /* these would normally be updated in mx_update_context(), but the
   * full headers aren't parsed with overview, so the information wasn't
   * available then */
  if (WithCrypto)
    hdr->security = crypt_query (hdr->content);

  rewind (msg->fp);
  mutt_clear_error();
  return 0;
}

/* Post article */
int nntp_post (const char *msg) {
  NNTP_DATA *nntp_data, nntp_tmp;
  FILE *fp;
  char buf[LONG_STRING];
  size_t len;

  if (Context && Context->magic == M_NNTP)
    nntp_data = Context->data;
  else
  {
    CurrentNewsSrv = nntp_select_server (NewsServer, 0);
    if (!CurrentNewsSrv)
      return -1;

    nntp_data = &nntp_tmp;
    nntp_data->nserv = CurrentNewsSrv;
    nntp_data->group = NULL;
  }

  fp = safe_fopen (msg, "r");
  if (!fp)
  {
    mutt_perror (msg);
    return -1;
  }

  strfcpy (buf, "POST\r\n", sizeof (buf));
  if (nntp_query (nntp_data, buf, sizeof (buf)) < 0)
    return -1;
  if (buf[0] != '3')
  {
    mutt_error (_("Can't post article: %s"), buf);
    return -1;
  }

  buf[0] = '.';
  buf[1] = '\0';
  while (fgets (buf + 1, sizeof (buf) - 2, fp))
  {
    len = strlen (buf);
    if (buf[len - 1] == '\n')
    {
      buf[len - 1] = '\r';
      buf[len] = '\n';
      len++;
      buf[len] = '\0';
    }
    if (mutt_socket_write_d (nntp_data->nserv->conn,
	buf[1] == '.' ? buf : buf + 1, -1, M_SOCK_LOG_HDR) < 0)
      return nntp_connect_error (nntp_data->nserv);
  }
  fclose (fp);

  if ((buf[strlen (buf) - 1] != '\n' &&
      mutt_socket_write_d (nntp_data->nserv->conn, "\r\n", -1, M_SOCK_LOG_HDR) < 0) ||
      mutt_socket_write_d (nntp_data->nserv->conn, ".\r\n", -1, M_SOCK_LOG_HDR) < 0 ||
      mutt_socket_readln (buf, sizeof (buf), nntp_data->nserv->conn) < 0)
    return nntp_connect_error (nntp_data->nserv);
  if (buf[0] != '2')
  {
    mutt_error (_("Can't post article: %s"), buf);
    return -1;
  }
  return 0;
}

/* Save changes to .newsrc and cache */
int nntp_sync_mailbox (CONTEXT *ctx)
{
  NNTP_DATA *nntp_data = ctx->data;
  int rc, i;
#ifdef USE_HCACHE
  header_cache_t *hc;
#endif

  /* check for new articles */
  nntp_data->nserv->check_time = 0;
  rc = nntp_check_mailbox (ctx, 1);
  if (rc)
    return rc;

#ifdef USE_HCACHE
  nntp_data->lastCached = 0;
  hc = nntp_hcache_open (nntp_data);
#endif

  nntp_data->unread = ctx->unread;
  for (i = 0; i < ctx->msgcount; i++)
  {
    HEADER *hdr = ctx->hdrs[i];
    char buf[16];

    snprintf (buf, sizeof (buf), "%d", NHDR (hdr)->article_num);
    if (nntp_data->bcache && hdr->deleted)
    {
      dprint (2, (debugfile, "nntp_sync_mailbox: mutt_bcache_del %s\n", buf));
      mutt_bcache_del (nntp_data->bcache, buf);
    }

#ifdef USE_HCACHE
    if (hc && (hdr->changed || hdr->deleted))
    {
      if (hdr->deleted && !hdr->read)
	nntp_data->unread--;
      dprint (2, (debugfile, "nntp_sync_mailbox: mutt_hcache_store %s\n", buf));
      mutt_hcache_store (hc, buf, hdr, 0, strlen, M_GENERATE_UIDVALIDITY);
    }
#endif
  }

#ifdef USE_HCACHE
  if (hc)
  {
    mutt_hcache_close (hc);
    nntp_data->lastCached = nntp_data->lastLoaded;
  }
#endif

  /* save .newsrc entries */
  nntp_newsrc_gen_entries (ctx);
  nntp_newsrc_update (nntp_data->nserv);
  nntp_newsrc_close (nntp_data->nserv);
  return 0;
}

/* Free up memory associated with the newsgroup context */
int nntp_fastclose_mailbox (CONTEXT *ctx)
{
  NNTP_DATA *nntp_data = ctx->data, *nntp_tmp;

  if (!nntp_data)
    return 0;

  nntp_acache_free (nntp_data);
  if (!nntp_data->nserv || !nntp_data->nserv->groups_hash || !nntp_data->group)
    return 0;

  nntp_tmp = hash_find (nntp_data->nserv->groups_hash, nntp_data->group);
  if (nntp_tmp == NULL || nntp_tmp != nntp_data)
    nntp_data_free (nntp_data);
  return 0;
}

/* Get date and time from server */
int nntp_date (NNTP_SERVER *nserv, time_t *now)
{
  if (nserv->hasDATE)
  {
    NNTP_DATA nntp_data;
    char buf[LONG_STRING];
    struct tm tm;

    nntp_data.nserv = nserv;
    nntp_data.group = NULL;
    strfcpy (buf, "DATE\r\n", sizeof (buf));
    if (nntp_query (&nntp_data, buf, sizeof (buf)) < 0)
      return -1;

    if (sscanf (buf, "111 %4d%2d%2d%2d%2d%2d%*s", &tm.tm_year, &tm.tm_mon,
		&tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6)
    {
      tm.tm_year -= 1900;
      tm.tm_mon--;
      *now = timegm (&tm);
      if (*now >= 0)
      {
	dprint (1, (debugfile, "nntp_date: server time is %d\n", *now));
	return 0;
      }
    }
  }
  time (now);
  return 0;
}

/* Fetch list of all newsgroups from server */
int nntp_active_fetch (NNTP_SERVER *nserv)
{
  NNTP_DATA nntp_data;
  char msg[SHORT_STRING];
  char buf[LONG_STRING];
  unsigned int i;
  int rc;

  snprintf (msg, sizeof (msg), _("Loading list of groups from server %s..."),
	    nserv->conn->account.host);
  mutt_message (msg);
  if (nntp_date (nserv, &nserv->newgroups_time) < 0)
    return -1;

  nntp_data.nserv = nserv;
  nntp_data.group = NULL;
  strfcpy (buf, "LIST\r\n", sizeof (buf));
  rc = nntp_fetch_lines (&nntp_data, buf, sizeof (buf), msg,
			 nntp_add_group, nserv);
  if (rc)
  {
    if (rc > 0)
    {
      mutt_error ("LIST: %s", buf);
      mutt_sleep (2);
    }
    return -1;
  }

  if (option (OPTLOADDESC) &&
      get_description (&nntp_data, "*", _("Loading descriptions...")) < 0)
    return -1;

  for (i = 0; i < nserv->groups_num; i++)
  {
    NNTP_DATA *nntp_data = nserv->groups_list[i];

    if (nntp_data && nntp_data->deleted && !nntp_data->newsrc_ent)
    {
      nntp_delete_group_cache (nntp_data);
      hash_delete (nserv->groups_hash, nntp_data->group, NULL, nntp_data_free);
      nserv->groups_list[i] = NULL;
    }
  }
  nntp_active_save_cache (nserv);
  mutt_clear_error ();
  return 0;
}

/* Check newsgroup for new articles:
 *  1 - new articles found
 *  0 - no change
 * -1 - lost connection */
static int nntp_group_poll (NNTP_DATA *nntp_data, int update_stat)
{
  char buf[LONG_STRING] = "";
  anum_t count, first, last;

  /* use GROUP command to poll newsgroup */
  if (nntp_query (nntp_data, buf, sizeof (buf)) < 0)
    return -1;
  if (sscanf (buf, "211 " ANUM " " ANUM " " ANUM, &count, &first, &last) != 3)
    return 0;
  if (first == nntp_data->firstMessage && last == nntp_data->lastMessage)
    return 0;

  /* articles have been renumbered */
  if (last < nntp_data->lastMessage)
  {
    nntp_data->lastCached = 0;
    if (nntp_data->newsrc_len)
    {
      safe_realloc (&nntp_data->newsrc_ent, sizeof (NEWSRC_ENTRY));
      nntp_data->newsrc_len = 1;
      nntp_data->newsrc_ent[0].first = 1;
      nntp_data->newsrc_ent[0].last = 0;
    }
  }
  nntp_data->firstMessage = first;
  nntp_data->lastMessage = last;
  if (!update_stat)
    return 1;

  /* update counters */
  else if (!last || (!nntp_data->newsrc_ent && !nntp_data->lastCached))
    nntp_data->unread = count;
  else
    nntp_group_unread_stat (nntp_data);
  return 1;
}

/* Check current newsgroup for new articles:
 *  M_REOPENED	- articles have been renumbered or removed from server
 *  M_NEW_MAIL	- new articles found
 *  0		- no change
 * -1		- lost connection */
int nntp_check_mailbox (CONTEXT *ctx, int leave_lock)
{
  NNTP_DATA *nntp_data = ctx->data;
  NNTP_SERVER *nserv = nntp_data->nserv;
  time_t now = time (NULL);
  int i, j;
  int rc, ret = 0;
  void *hc = NULL;

  if (nserv->check_time + NewsPollTimeout > now)
    return 0;

  mutt_message _("Checking for new messages...");
  if (nntp_newsrc_parse (nserv) < 0)
    return -1;

  nserv->check_time = now;
  rc = nntp_group_poll (nntp_data, 0);
  if (rc < 0)
  {
    nntp_newsrc_close (nserv);
    return -1;
  }
  if (rc)
    nntp_active_save_cache (nserv);

  /* articles have been renumbered, remove all headers */
  if (nntp_data->lastMessage < nntp_data->lastLoaded)
  {
    for (i = 0; i < ctx->msgcount; i++)
      mutt_free_header (&ctx->hdrs[i]);
    ctx->msgcount = 0;
    ctx->tagged = 0;

    if (nntp_data->lastMessage < nntp_data->lastLoaded)
    {
      nntp_data->lastLoaded = nntp_data->firstMessage - 1;
      if (NntpContext && nntp_data->lastMessage - nntp_data->lastLoaded >
	  NntpContext)
	nntp_data->lastLoaded = nntp_data->lastMessage - NntpContext;
    }
    ret = M_REOPENED;
  }

  /* .newsrc has been externally modified */
  if (nserv->newsrc_modified)
  {
    anum_t anum;
#ifdef USE_HCACHE
    unsigned char *messages;
    char buf[16];
    void *hdata;
    HEADER *hdr;
    anum_t first = nntp_data->firstMessage;

    if (NntpContext && nntp_data->lastMessage - first + 1 > NntpContext)
      first = nntp_data->lastMessage - NntpContext + 1;
    messages = safe_calloc (nntp_data->lastLoaded - first + 1,
			    sizeof (unsigned char));
    hc = nntp_hcache_open (nntp_data);
    nntp_hcache_update (nntp_data, hc);
#endif

    /* update flags according to .newsrc */
    for (i = j = 0; i < ctx->msgcount; i++)
    {
      int flagged = 0;
      anum = NHDR (ctx->hdrs[i])->article_num;

#ifdef USE_HCACHE
      /* check hcache for flagged and deleted flags */
      if (hc)
      {
	if (anum >= first && anum <= nntp_data->lastLoaded)
	  messages[anum - first] = 1;

	snprintf (buf, sizeof (buf), "%d", anum);
	hdata = mutt_hcache_fetch (hc, buf, strlen);
	if (hdata)
	{
	  int deleted;

	  dprint (2, (debugfile,
		      "nntp_check_mailbox: mutt_hcache_fetch %s\n", buf));
	  hdr = mutt_hcache_restore (hdata, NULL);
	  FREE (&hdata);
	  hdr->data = 0;
	  deleted = hdr->deleted;
	  flagged = hdr->flagged;
	  mutt_free_header (&hdr);

	  /* header marked as deleted, removing from context */
	  if (deleted)
	  {
	    mutt_set_flag (ctx, ctx->hdrs[i], M_TAG, 0);
	    mutt_free_header (&ctx->hdrs[i]);
	    continue;
	  }
	}
      }
#endif

      if (!ctx->hdrs[i]->changed)
      {
	ctx->hdrs[i]->flagged = flagged;
	ctx->hdrs[i]->read = 0;
	ctx->hdrs[i]->old = 0;
	nntp_article_status (ctx, ctx->hdrs[i], NULL, anum);
	if (!ctx->hdrs[i]->read)
	  nntp_parse_xref (ctx, ctx->hdrs[i]);
      }
      ctx->hdrs[j++] = ctx->hdrs[i];
    }

#ifdef USE_HCACHE
    ctx->msgcount = j;

    /* restore headers without "deleted" flag */
    for (anum = first; anum <= nntp_data->lastLoaded; anum++)
    {
      if (messages[anum - first])
	continue;

      snprintf (buf, sizeof (buf), "%d", anum);
      hdata = mutt_hcache_fetch (hc, buf, strlen);
      if (hdata)
      {
	dprint (2, (debugfile,
		    "nntp_check_mailbox: mutt_hcache_fetch %s\n", buf));
	if (ctx->msgcount >= ctx->hdrmax)
	  mx_alloc_memory (ctx);

	ctx->hdrs[ctx->msgcount] =
	hdr = mutt_hcache_restore (hdata, NULL);
	FREE (&hdata);
	hdr->data = 0;
	if (hdr->deleted)
	{
	  mutt_free_header (&hdr);
	  if (nntp_data->bcache)
	  {
	    dprint (2, (debugfile,
			"nntp_check_mailbox: mutt_bcache_del %s\n", buf));
	    mutt_bcache_del (nntp_data->bcache, buf);
	  }
	  continue;
	}

	ctx->msgcount++;
	hdr->read = 0;
	hdr->old = 0;
	hdr->data = safe_calloc (1, sizeof (NNTP_HEADER_DATA));
	NHDR (hdr)->article_num = anum;
	nntp_article_status (ctx, hdr, NULL, anum);
	if (!hdr->read)
	  nntp_parse_xref (ctx, hdr);
      }
    }
    FREE (&messages);
#endif

    nserv->newsrc_modified = 0;
    ret = M_REOPENED;
  }

  /* some headers were removed, context must be updated */
  if (ret == M_REOPENED)
  {
    if (ctx->subj_hash)
      hash_destroy (&ctx->subj_hash, NULL);
    if (ctx->id_hash)
      hash_destroy (&ctx->id_hash, NULL);
    mutt_clear_threads (ctx);

    ctx->vcount = 0;
    ctx->deleted = 0;
    ctx->new = 0;
    ctx->unread = 0;
    ctx->flagged = 0;
    ctx->changed = 0;
    ctx->id_hash = NULL;
    ctx->subj_hash = NULL;
    mx_update_context (ctx, ctx->msgcount);
  }

  /* fetch headers of new articles */
  if (nntp_data->lastMessage > nntp_data->lastLoaded)
  {
    int oldmsgcount = ctx->msgcount;
    int quiet = ctx->quiet;
    ctx->quiet = 1;
#ifdef USE_HCACHE
    if (!hc)
    {
      hc = nntp_hcache_open (nntp_data);
      nntp_hcache_update (nntp_data, hc);
    }
#endif
    rc = nntp_fetch_headers (ctx, hc, nntp_data->lastLoaded + 1,
			     nntp_data->lastMessage, 0);
    ctx->quiet = quiet;
    if (rc >= 0)
      nntp_data->lastLoaded = nntp_data->lastMessage;
    if (ret == 0 && ctx->msgcount > oldmsgcount)
      ret = M_NEW_MAIL;
  }

#ifdef USE_HCACHE
  mutt_hcache_close (hc);
#endif
  if (ret || !leave_lock)
    nntp_newsrc_close (nserv);
  mutt_clear_error ();
  return ret;
}

/* Check for new groups and new articles in subscribed groups:
 *  1 - new groups found
 *  0 - no new groups
 * -1 - error */
int nntp_check_new_groups (NNTP_SERVER *nserv)
{
  NNTP_DATA nntp_data;
  time_t now;
  struct tm *tm;
  char buf[LONG_STRING];
  char *msg = _("Checking for new newsgroups...");
  unsigned int i;
  int rc, update_active = FALSE;

  if (!nserv || !nserv->newgroups_time)
    return -1;

  /* check subscribed newsgroups for new articles */
  if (option (OPTSHOWNEWNEWS))
  {
    mutt_message _("Checking for new messages...");
    for (i = 0; i < nserv->groups_num; i++)
    {
      NNTP_DATA *nntp_data = nserv->groups_list[i];

      if (nntp_data && nntp_data->subscribed)
      {
	rc = nntp_group_poll (nntp_data, 1);
	if (rc < 0)
	  return -1;
	if (rc > 0)
	  update_active = TRUE;
      }
    }
    /* select current newsgroup */
    if (Context && Context->magic == M_NNTP)
    {
      buf[0] = '\0';
      if (nntp_query ((NNTP_DATA *)Context->data, buf, sizeof (buf)) < 0)
	return -1;
    }
  }
  else if (nserv->newgroups_time)
    return 0;

  /* get list of new groups */
  mutt_message (msg);
  if (nntp_date (nserv, &now) < 0)
    return -1;
  nntp_data.nserv = nserv;
  if (Context && Context->magic == M_NNTP)
    nntp_data.group = ((NNTP_DATA *)Context->data)->group;
  else
    nntp_data.group = NULL;
  i = nserv->groups_num;
  tm = gmtime (&nserv->newgroups_time);
  snprintf (buf, sizeof (buf), "NEWGROUPS %02d%02d%02d %02d%02d%02d GMT\r\n",
	    tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday,
	    tm->tm_hour, tm->tm_min, tm->tm_sec);
  rc = nntp_fetch_lines (&nntp_data, buf, sizeof (buf), msg,
			 nntp_add_group, nserv);
  if (rc)
  {
    if (rc > 0)
    {
      mutt_error ("NEWGROUPS: %s", buf);
      mutt_sleep (2);
    }
    return -1;
  }

  /* new groups found */
  rc = 0;
  if (nserv->groups_num != i)
  {
    nserv->newgroups_time = now;

    /* loading descriptions */
    if (option (OPTLOADDESC))
    {
      unsigned int count = 0;
      progress_t progress;

      mutt_progress_init (&progress, _("Loading descriptions..."),
			  M_PROGRESS_MSG, ReadInc, nserv->groups_num - i);
      for (; i < nserv->groups_num; i++)
      {
	NNTP_DATA *nntp_data = nserv->groups_list[i];

	if (get_description (nntp_data, NULL, NULL) < 0)
	  return -1;
	mutt_progress_update (&progress, ++count, -1);
      }
    }
    update_active = TRUE;
    rc = 1;
  }
  if (update_active)
    nntp_active_save_cache (nserv);
  mutt_clear_error ();
  return rc;
}

/* Fetch article by Message-ID:
 *  0 - success
 *  1 - no such article
 * -1 - error */
int nntp_check_msgid (CONTEXT *ctx, const char *msgid)
{
  NNTP_DATA *nntp_data = ctx->data;
  HEADER *hdr;
  FILE *fp;
  char tempfile[_POSIX_PATH_MAX];
  char buf[LONG_STRING];
  int rc;

  mutt_mktemp (tempfile, sizeof (tempfile));
  fp = safe_fopen (tempfile, "w+");
  if (!fp)
  {
    mutt_perror (tempfile);
    unlink (tempfile);
    return -1;
  }

  snprintf (buf, sizeof (buf), "HEAD %s\r\n", msgid);
  rc = nntp_fetch_lines (nntp_data, buf, sizeof (buf), NULL,
			 fetch_tempfile, fp);
  if (rc)
  {
    fclose (fp);
    unlink (tempfile);
    if (rc < 0)
      return -1;
    if (!mutt_strncmp ("430", buf, 3))
      return 1;
    mutt_error ("HEAD: %s", buf);
    return -1;
  }

  /* parse header */
  if (ctx->msgcount == ctx->hdrmax)
    mx_alloc_memory (ctx);
  hdr = ctx->hdrs[ctx->msgcount] = mutt_new_header ();
  hdr->data = safe_calloc (1, sizeof (NNTP_HEADER_DATA));
  hdr->env = mutt_read_rfc822_header (fp, hdr, 0, 0);
  fclose (fp);
  unlink (tempfile);

  /* get article number */
  if (hdr->env->xref)
    nntp_parse_xref (ctx, hdr);
  else
  {
    snprintf (buf, sizeof (buf), "STAT %s\r\n", msgid);
    if (nntp_query (nntp_data, buf, sizeof (buf)) < 0)
    {
      mutt_free_header (&hdr);
      return -1;
    }
    sscanf (buf + 4, ANUM, &NHDR (hdr)->article_num);
  }

  /* reset flags */
  hdr->read = 0;
  hdr->old = 0;
  hdr->deleted = 0;
  hdr->changed = 1;
  hdr->received = hdr->date_sent;
  hdr->index = ctx->msgcount++;
  mx_update_context (ctx, 1);
  return 0;
}

typedef struct
{
  CONTEXT *ctx;
  unsigned int num;
  unsigned int max;
  anum_t *child;
} CHILD_CTX;

/* Parse XPAT line */
static int fetch_children (char *line, void *data)
{
  CHILD_CTX *cc = data;
  anum_t anum;
  unsigned int i;

  if (!line || sscanf (line, ANUM, &anum) != 1)
    return 0;
  for (i = 0; i < cc->ctx->msgcount; i++)
    if (NHDR (cc->ctx->hdrs[i])->article_num == anum)
      return 0;
  if (cc->num >= cc->max)
  {
    cc->max *= 2;
    safe_realloc (&cc->child, sizeof (anum_t) * cc->max);
  }
  cc->child[cc->num++] = anum;
  return 0;
}

/* Fetch children of article with the Message-ID */
int nntp_check_children (CONTEXT *ctx, const char *msgid)
{
  NNTP_DATA *nntp_data = ctx->data;
  CHILD_CTX cc;
  char buf[STRING];
  int i, rc, quiet;
  void *hc = NULL;

  if (!nntp_data || !nntp_data->nserv)
    return -1;
  if (nntp_data->firstMessage > nntp_data->lastLoaded)
    return 0;

  /* init context */
  cc.ctx = ctx;
  cc.num = 0;
  cc.max = 10;
  cc.child = safe_malloc (sizeof (anum_t) * cc.max);

  /* fetch numbers of child messages */
  snprintf (buf, sizeof (buf), "XPAT References %d-%d *%s*\r\n",
	    nntp_data->firstMessage, nntp_data->lastLoaded, msgid);
  rc = nntp_fetch_lines (nntp_data, buf, sizeof (buf), NULL,
			 fetch_children, &cc);
  if (rc)
  {
    FREE (&cc.child);
    if (rc > 0) {
      if (mutt_strncmp ("500", buf, 3))
	mutt_error ("XPAT: %s", buf);
      else
	mutt_error _("Unable to find child articles because server does not support XPAT command.");
    }
    return -1;
  }

  /* fetch all found messages */
  quiet = ctx->quiet;
  ctx->quiet = 1;
#ifdef USE_HCACHE
  hc = nntp_hcache_open (nntp_data);
#endif
  for (i = 0; i < cc.num; i++)
  {
    rc = nntp_fetch_headers (ctx, hc, cc.child[i], cc.child[i], 1);
    if (rc < 0)
      break;
  }
#ifdef USE_HCACHE
  mutt_hcache_close (hc);
#endif
  ctx->quiet = quiet;
  FREE (&cc.child);
  return rc < 0 ? -1 : 0;
}