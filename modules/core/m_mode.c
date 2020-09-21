/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_mode.c: Sets a user or channel mode.
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
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "match.h"
#include "ircd.h"
#include "numeric.h"
#include "s_user.h"
#include "s_conf.h"
#include "s_serv.h"
#include "logger.h"
#include "send.h"
#include "msg.h"
#include "parse.h"
#include "modules.h"
#include "packet.h"
#include "s_newconf.h"

static const char mode_desc[] =
	"Provides the MODE and MLOCK client and server commands, and TS6 server-to-server TMODE and BMASK commands";

static void m_mode(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_mode(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_tmode(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_mlock(struct MsgBuf *, struct Client *, struct Client *, int, const char **);
static void ms_bmask(struct MsgBuf *, struct Client *, struct Client *, int, const char **);

struct Message mode_msgtab = {
	"MODE", 0, 0, 0, 0,
	{mg_unreg, {m_mode, 2}, {m_mode, 3}, {ms_mode, 3}, mg_ignore, {m_mode, 2}}
};
struct Message tmode_msgtab = {
	"TMODE", 0, 0, 0, 0,
	{mg_ignore, mg_ignore, {ms_tmode, 4}, {ms_tmode, 4}, mg_ignore, mg_ignore}
};
struct Message mlock_msgtab = {
	"MLOCK", 0, 0, 0, 0,
	{mg_ignore, mg_ignore, {ms_mlock, 3}, {ms_mlock, 3}, mg_ignore, mg_ignore}
};
struct Message bmask_msgtab = {
	"BMASK", 0, 0, 0, 0,
	{mg_ignore, mg_ignore, mg_ignore, {ms_bmask, 5}, mg_ignore, mg_ignore}
};

mapi_clist_av1 mode_clist[] = { &mode_msgtab, &tmode_msgtab, &mlock_msgtab, &bmask_msgtab, NULL };

DECLARE_MODULE_AV2(mode, NULL, NULL, mode_clist, NULL, NULL, NULL, NULL, mode_desc);

/*
 * m_mode - MODE command handler
 * parv[1] - channel
 */
static void
m_mode(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Channel *chptr = NULL;
	struct membership *msptr;
	int n = 2;
	const char *dest;
	int operspy = 0;

	dest = parv[1];

	if(IsOperSpy(source_p) && *dest == '!')
	{
		dest++;
		operspy = 1;

		if(EmptyString(dest))
		{
			sendto_one(source_p, form_str(ERR_NEEDMOREPARAMS),
				   me.name, source_p->name, "MODE");
			return;
		}
	}

	/* Now, try to find the channel in question */
	if(!IsChanPrefix(*dest))
	{
		/* if here, it has to be a non-channel name */
		user_mode(client_p, source_p, parc, parv);
		return;
	}

	if(!check_channel_name(dest))
	{
		sendto_one_numeric(source_p, ERR_BADCHANNAME, form_str(ERR_BADCHANNAME), parv[1]);
		return;
	}

	chptr = find_channel(dest);

	if(chptr == NULL)
	{
		sendto_one_numeric(source_p, ERR_NOSUCHCHANNEL,
				   form_str(ERR_NOSUCHCHANNEL), parv[1]);
		return;
	}

	/* Now know the channel exists */
	if(parc < n + 1)
	{
		if(operspy)
			report_operspy(source_p, "MODE", chptr->chname);

		sendto_one(source_p, form_str(RPL_CHANNELMODEIS),
			   me.name, source_p->name, parv[1],
			   operspy ? channel_modes(chptr, &me) : channel_modes(chptr, source_p));

		sendto_one(source_p, form_str(RPL_CREATIONTIME),
			   me.name, source_p->name, parv[1], chptr->channelts);
	}
	else
	{
		msptr = find_channel_membership(chptr, source_p);

		/* Finish the flood grace period... */
		if(MyClient(source_p) && !IsFloodDone(source_p))
		{
			if(!((parc == 3) && (parv[2][0] == 'b' || parv[2][0] == 'q') && (parv[2][1] == '\0')))
				flood_endgrace(source_p);
		}

		set_channel_mode(client_p, source_p, chptr, msptr, parc - n, parv + n);
	}
}

static void
ms_mode(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Channel *chptr;

	chptr = find_channel(parv[1]);

	if(chptr == NULL)
	{
		sendto_one_numeric(source_p, ERR_NOSUCHCHANNEL,
				   form_str(ERR_NOSUCHCHANNEL), parv[1]);
		return;
	}

	set_channel_mode(client_p, source_p, chptr, NULL, parc - 2, parv + 2);
}

static void
ms_tmode(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Channel *chptr = NULL;
	struct membership *msptr;

	/* Now, try to find the channel in question */
	if(!IsChanPrefix(parv[2][0]) || !check_channel_name(parv[2]))
	{
		sendto_one_numeric(source_p, ERR_BADCHANNAME, form_str(ERR_BADCHANNAME), parv[2]);
		return;
	}

	chptr = find_channel(parv[2]);

	if(chptr == NULL)
	{
		sendto_one_numeric(source_p, ERR_NOSUCHCHANNEL,
				   form_str(ERR_NOSUCHCHANNEL), parv[2]);
		return;
	}

	/* TS is higher, drop it. */
	if(atol(parv[1]) > chptr->channelts)
		return;

	if(IsServer(source_p))
	{
		set_channel_mode(client_p, source_p, chptr, NULL, parc - 3, parv + 3);
	}
	else
	{
		msptr = find_channel_membership(chptr, source_p);

		set_channel_mode(client_p, source_p, chptr, msptr, parc - 3, parv + 3);
	}
}

static void
ms_mlock(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Channel *chptr = NULL;

	/* Now, try to find the channel in question */
	if(!IsChanPrefix(parv[2][0]) || !check_channel_name(parv[2]))
	{
		sendto_one_numeric(source_p, ERR_BADCHANNAME, form_str(ERR_BADCHANNAME), parv[2]);
		return;
	}

	chptr = find_channel(parv[2]);

	if(chptr == NULL)
	{
		sendto_one_numeric(source_p, ERR_NOSUCHCHANNEL,
				   form_str(ERR_NOSUCHCHANNEL), parv[2]);
		return;
	}

	/* TS is higher, drop it. */
	if(atol(parv[1]) > chptr->channelts)
		return;

	if(IsServer(source_p))
		set_channel_mlock(client_p, source_p, chptr, parv[3], true);
}

static void
possibly_remove_lower_forward(struct Client *fakesource_p, int mems,
		struct Channel *chptr, rb_dlink_list *banlist, int mchar,
		const char *mask, const char *forward)
{
	struct Ban *actualBan;
	rb_dlink_node *ptr;

	RB_DLINK_FOREACH(ptr, banlist->head)
	{
		actualBan = ptr->data;
		if(!irccmp(actualBan->banstr, mask) &&
				(actualBan->forward == NULL ||
				 irccmp(actualBan->forward, forward) < 0))
		{
			sendto_channel_local(fakesource_p, mems, chptr, ":%s MODE %s -%c %s%s%s",
					fakesource_p->name,
					chptr->chname,
					mchar,
					actualBan->banstr,
					actualBan->forward ? "$" : "",
					actualBan->forward ? actualBan->forward : "");
			rb_dlinkDelete(&actualBan->node, banlist);
			free_ban(actualBan);
			return;
		}
	}
}

static void
ms_bmask(struct MsgBuf *msgbuf_p, struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	static char modebuf[BUFSIZE];
	static char parabuf[BUFSIZE];
	struct Channel *chptr;
	rb_dlink_list *banlist;
	char *s, *forward;
	char *t;
	char *mbuf;
	char *pbuf;
	long mode_type;
	int mlen;
	int plen = 0;
	int tlen;
	int arglen;
	int modecount = 0;
	int needcap = NOCAPS;
	int mems;
	struct Client *fakesource_p;

	if(!IsChanPrefix(parv[2][0]) || !check_channel_name(parv[2]))
		return;

	if((chptr = find_channel(parv[2])) == NULL)
		return;

	/* TS is higher, drop it. */
	if(atol(parv[1]) > chptr->channelts)
		return;

	switch (parv[3][0])
	{
	case 'b':
		banlist = &chptr->banlist;
		mode_type = CHFL_BAN;
		mems = ALL_MEMBERS;
		break;

	case 'e':
		banlist = &chptr->exceptlist;
		mode_type = CHFL_EXCEPTION;
		needcap = CAP_EX;
		mems = ONLY_CHANOPS;
		break;

	case 'I':
		banlist = &chptr->invexlist;
		mode_type = CHFL_INVEX;
		needcap = CAP_IE;
		mems = ONLY_CHANOPS;
		break;

		/* maybe we should just blindly propagate this? */
	default:
		return;
	}

	parabuf[0] = '\0';
	s = LOCAL_COPY(parv[4]);

	/* Hide connecting server on netburst -- jilles */
	if (ConfigServerHide.flatten_links && !HasSentEob(source_p))
		fakesource_p = &me;
	else
		fakesource_p = source_p;
	mlen = sprintf(modebuf, ":%s MODE %s +", fakesource_p->name, chptr->chname);
	mbuf = modebuf + mlen;
	pbuf = parabuf;

	while(*s == ' ')
		s++;

	/* next char isnt a space, point t to the next one */
	if((t = strchr(s, ' ')) != NULL)
	{
		*t++ = '\0';

		/* double spaces will break the parser */
		while(*t == ' ')
			t++;
	}

	/* couldve skipped spaces and got nothing.. */
	while(!EmptyString(s))
	{
		/* ban with a leading ':' -- this will break the protocol */
		if(*s == ':')
			goto nextban;

		tlen = strlen(s);

		/* I dont even want to begin parsing this.. */
		if(tlen > MODEBUFLEN)
			break;

		if((forward = strchr(s+1, '$')) != NULL)
		{
			*forward++ = '\0';
			if(*forward == '\0')
				tlen--, forward = NULL;
			else
				possibly_remove_lower_forward(fakesource_p,
						mems, chptr, banlist,
						parv[3][0], s, forward);
		}

		if(add_id(fakesource_p, chptr, s, forward, banlist, mode_type))
		{
			/* this new one wont fit.. */
			if(mlen + MAXMODEPARAMS + plen + tlen > BUFSIZE - 5 ||
			   modecount >= MAXMODEPARAMS)
			{
				*mbuf = '\0';
				*(pbuf - 1) = '\0';
				sendto_channel_local(fakesource_p, mems, chptr, "%s %s", modebuf, parabuf);

				mbuf = modebuf + mlen;
				pbuf = parabuf;
				plen = modecount = 0;
			}

			if (forward != NULL)
				forward[-1] = '$';

			*mbuf++ = parv[3][0];
			arglen = sprintf(pbuf, "%s ", s);
			pbuf += arglen;
			plen += arglen;
			modecount++;
		}

	      nextban:
		s = t;

		if(s != NULL)
		{
			if((t = strchr(s, ' ')) != NULL)
			{
				*t++ = '\0';

				while(*t == ' ')
					t++;
			}
		}
	}

	if(modecount)
	{
		*mbuf = '\0';
		*(pbuf - 1) = '\0';
		sendto_channel_local(fakesource_p, mems, chptr, "%s %s", modebuf, parabuf);
	}

	sendto_server(client_p, chptr, CAP_TS6 | needcap, NOCAPS, ":%s BMASK %ld %s %s :%s",
		      source_p->id, (long) chptr->channelts, chptr->chname, parv[3], parv[4]);
}
