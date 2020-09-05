/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_quit.c: Makes a user quit from IRC.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 */

#include "stdinc.h"
#include "client.h"
#include "ircd.h"
#include "numeric.h"
#include "s_serv.h"
#include "send.h"
#include "msg.h"
#include "parse.h"
#include "modules.h"
#include "s_conf.h"
#include "inline/stringops.h"
#include "s_newconf.h"

static const char quit_desc[] = "Provides the QUIT command to allow a user to leave the network";

static void m_quit(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_quit(struct MsgBuf *, struct Client *, struct Client *, int, const char **);

static int h_client_quit;

struct Message quit_msgtab = {
	"QUIT", 0, 0, 0, 0,
	{{m_quit, 0}, {m_quit, 0}, {ms_quit, 0}, mg_ignore, mg_ignore, {m_quit, 0}}
};

mapi_clist_av1 quit_clist[] = { &quit_msgtab, NULL };

mapi_hlist_av1 quit_hlist[] = {
	{ "client_quit", &h_client_quit },
	{ NULL, NULL }
};

DECLARE_MODULE_AV2(quit, NULL, NULL, quit_clist, quit_hlist, NULL, NULL, NULL, quit_desc);

/*
** m_quit
**      parv[1] = comment
*/
static void
m_quit(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	char *comment_copy = LOCAL_COPY((parc > 1 && parv[1]) ? parv[1] : client_p->name);
	const char *comment = comment_copy;
	char reason[REASONLEN + 1];
	hook_data_client_quit hdata;

	source_p->flags |= FLAGS_NORMALEX;

	if (strlen(comment_copy) > (size_t) REASONLEN)
		comment_copy[REASONLEN] = '\0';

	strip_colour(comment_copy);

	hdata.client = client_p;
	hdata.reason = hdata.orig_reason = comment;
	call_hook(h_client_quit, &hdata);
	comment = hdata.reason;

	/* don't add Quit: if the reason comes from a module */
	if (ConfigFileEntry.client_exit && hdata.reason == hdata.orig_reason && comment[0])
	{
		snprintf(reason, sizeof(reason), "Quit: %s", comment);
		comment = reason;
	}

	if (comment == NULL || (!IsOperGeneral(source_p) && hdata.reason == hdata.orig_reason &&
	   (source_p->localClient->firsttime + ConfigFileEntry.anti_spam_exit_message_time) >
	   rb_current_time()))
	{
		exit_client(client_p, source_p, source_p, "Client Quit");
		return;
	}

	exit_client(client_p, source_p, source_p, comment);
}

/*
** ms_quit
**      parv[1] = comment
*/
static void
ms_quit(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	char *comment = LOCAL_COPY((parc > 1 && parv[1]) ? parv[1] : client_p->name);

	source_p->flags |= FLAGS_NORMALEX;
	if(strlen(comment) > (size_t) REASONLEN)
		comment[REASONLEN] = '\0';

	exit_client(client_p, source_p, source_p, comment);
}
