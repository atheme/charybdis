/************************************************************************
 *   IRC - Internet Relay Chat, extensions/spamfilter_nicks.c
 *   Copyright (C) 2016 Jason Volk
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 */

#include "stdinc.h"
#include "modules.h"
#include "hook.h"
#include "client.h"
#include "send.h"
#include "hash.h"
#include "newconf.h"
#include "irc_radixtree.h"
#include "spamfilter.h"


/* Conf items & defaults */
size_t conf_limit            = 5;
size_t conf_nicklen_min      = 4;
size_t conf_bloom_size       = 1024 * 64;
size_t conf_bloom_bits       = 16;
time_t conf_bloom_refresh    = 86400;


/* Bloom filter hashes */
static
uint64_t bloom_hash_fnv(const char *const str)
{
	return fnv_hash_upper((const unsigned char *)str,conf_bloom_bits);
}

static
uint64_t bloom_hash_bernstein(const char *const str)
{
	uint64_t ret = 7681;
	for(size_t i = 0; str[i]; i++)
		ret = ret * 33ULL + str[i];

	return ret;
}

#define NUM_HASHES 2
uint64_t (*bloom_hashes[NUM_HASHES])(const char *const str) =
{
	bloom_hash_fnv,
	bloom_hash_bernstein,
};


/* Bloom filter state */
uint8_t *bloom[NUM_HASHES];
uint64_t bloom_salt;
size_t bloom_size;
size_t bloom_members;
time_t bloom_flushed;
struct irc_radixtree *chans;  // Channels with MODE_SPAMFILTER that participate in the bloom filter


static
void bloom_flush(void)
{
	for(size_t i = 0; i < NUM_HASHES; i++)
		memset(bloom[i],0x0,bloom_size);

	bloom_flushed = rb_current_time();
	bloom_members = 0;
}

static
void bloom_destroy(void)
{
	for(size_t i = 0; i < NUM_HASHES; i++)
	{
		rb_free(bloom[i]);
		bloom[i] = NULL;
	}

	bloom_members = 0;
	bloom_size = 0;
}

static
void bloom_create(const size_t size)
{
	if(!size)
		return;

	for(size_t i = 0; i < NUM_HASHES; i++)
		bloom[i] = rb_malloc(size);

	bloom_size = size;
	bloom_flush();
}

static
void bloom_add(const size_t filter,
               uint64_t hash)
{
	hash += bloom_salt;
	hash %= bloom_size * 8UL;
	bloom[filter][hash / 8UL] |= (1U << (hash % 8UL));
}

static
int bloom_test(const size_t filter,
               uint64_t hash)
{
	hash += bloom_salt;
	hash %= bloom_size * 8UL;
	const int bit = hash % 8UL;
	return (bloom[filter][hash / 8UL] & (1U << bit)) >> bit;
}

static
void bloom_add_str(const char *const str)
{
	for(size_t i = 0; i < NUM_HASHES; i++)
		bloom_add(i,bloom_hashes[i](str));

	bloom_members++;
}

static
int bloom_test_str(const char *const str)
{
	uint count = 0;
	for(size_t i = 0; i < NUM_HASHES; i++)
		count += bloom_test(i,bloom_hashes[i](str));

	return count >= NUM_HASHES;
}


static
int chans_has(const struct Channel *const chptr)
{
	return irc_radixtree_retrieve(chans,chptr->chname) != NULL;
}

static
int chans_add(struct Channel *const chptr)
{
	if(!irc_radixtree_add(chans,chptr->chname,chptr))
		return 0;

	rb_dlink_node *ptr;
	RB_DLINK_FOREACH(ptr,chptr->members.head)
	{
		const struct membership *const msptr = ptr->data;
		bloom_add_str(msptr->client_p->name);
	}

	return 1;
}


static
int expired(void)
{
	return bloom_flushed + conf_bloom_refresh < rb_current_time();
}

static
void reset(void)
{
	if(bloom[0])
		bloom_flush();

	if(chans)
		irc_radixtree_destroy(chans,NULL,NULL);

	chans = irc_radixtree_create("chans",irc_radixtree_irccasecanon);
}

static
void resize(const size_t size)
{
	bloom_destroy();
	reset();
	bloom_create(size);
}


static
int prob_test_token(const char *const token)
{
	return bloom_test_str(token);
}


static
int real_test_token(const char *const token,
                    struct Channel *const chptr)
{
	struct Client *const client = find_named_client(token);
	return client && IsMember(client,chptr);
}


static
void false_positive_message(void)
{
	sendto_realops_snomask(SNO_GENERAL,L_ALL,
	                       "spamfilter: Nickname bloom filter false positive (size: %zu members: %zu channels: %zu flushed: %zu ago)",
	                       bloom_size,
	                       bloom_members,
	                       irc_radixtree_size(chans),
	                       rb_current_time() - bloom_flushed);
}


/*
 * Always find the length of any multibyte character to advance past.
 * The unicode space characters of concern are only of length 3.
 */
static
int is_delim(const uint8_t *const ptr,
             int *const adv)
{
	/* Some ascii ranges */
	if((ptr[0] >= 0x20 && ptr[0] <= 0x2F) ||
	   (ptr[0] >= 0x3A && ptr[0] <= 0x40) ||
	   (ptr[0] >= 0x5C && ptr[0] <= 0x60) ||
	   (ptr[0] >= 0x7B && ptr[0] <= 0x7F))
		return 1;

	/* Unicode below here */
	const int len = ((ptr[0] & 0x80) == 0x80)+
	                ((ptr[0] & 0xC0) == 0xC0)+
	                ((ptr[0] & 0xE0) == 0xE0)+
	                ((ptr[0] & 0xF0) == 0xF0)+
	                ((ptr[0] & 0xF8) == 0xF8)+
	                ((ptr[0] & 0xFC) == 0xFC);

	if(len)
		*adv += len - 1;

	if(len != 3)
		return 0;

	switch((htonl(*(const uint32_t *)ptr) & 0x1F7F7F00U) >> 8)
	{
		case 0x20000:
		case 0x20001:
		case 0x20002:
		case 0x20003:
		case 0x20004:
		case 0x20005:
		case 0x20006:
		case 0x20007:
		case 0x20008:
		case 0x20009:
		case 0x2000A:
		case 0x2000B:
		case 0x2002F:
		case 0x2005F:
		case 0x30000:
		case 0xf3b3f:
			return 1;

		default:
			return 0;
	}
}


static
uint count_nicks(const unsigned char *const text,
                 struct Channel *const chptr)
{
	uint ret = 0;
	const uint len = strlen(text);
	for(uint i = 0, j = 0, k = 0; i + 6 < len; i++)
	{
		if(!is_delim(text+i,&k))
		{
			j++;
			continue;
		}

		if(j >= conf_nicklen_min && j <= NICKLEN)
		{
			char token[NICKLEN+1];
			rb_strlcpy(token,text+i-j,j+1);
			if(prob_test_token(token))
			{
				if(rb_likely(real_test_token(token,chptr)))
					ret++;
				else
					false_positive_message();
			}
		}

		i += k;
		j = 0;
		k = 0;
	}

	return ret;
}


static
void hook_spamfilter_query(hook_data_privmsg_channel *const hook)
{
	if(hook->approved != 0)
		return;

	if(!bloom[0])
		return;

	const uint counted = count_nicks(hook->text,hook->chptr);
	if(counted < conf_limit)
		return;

	static char reason[64];
	snprintf(reason,sizeof(reason),"nicks: counted at least %u names",counted);
	hook->reason = reason;
	hook->approved = -1;
}


static
void hook_channel_join(hook_data_channel_approval *const data)
{
	if(~data->chptr->mode.mode & chmode_table[(uint8_t)MODE_SPAMFILTER].mode_type)
		return;

	if(!bloom[0])
		return;

	if(expired())
		reset();

	if(chans_has(data->chptr))
		bloom_add_str(data->client->name);
	else
		chans_add(data->chptr);
}


static
int conf_spamfilter_nicks_end(struct TopConf *const tc)
{
	if(conf_bloom_size != bloom_size)
		resize(conf_bloom_size);

	return 0;
}


static void set_conf_limit(void *const val)          { conf_limit = *(int *)val;                 }
static void set_conf_nicklen_min(void *const val)    { conf_nicklen_min = *(int *)val;           }
static void set_conf_bloom_size(void *const val)     { conf_bloom_size = *(int *)val;            }
static void set_conf_bloom_bits(void *const val)     { conf_bloom_bits = *(int *)val;            }
static void set_conf_bloom_refresh(void *const val)  { conf_bloom_refresh = *(time_t *)val;      }


struct ConfEntry conf_spamfilter_nicks[] =
{
	{ "limit",           CF_INT,   set_conf_limit,              0, NULL },
	{ "nicklen_min",     CF_INT,   set_conf_nicklen_min,        0, NULL },
	{ "bloom_size",      CF_INT,   set_conf_bloom_size,         0, NULL },
	{ "bloom_bits",      CF_INT,   set_conf_bloom_bits,         0, NULL },
	{ "bloom_refresh",   CF_TIME,  set_conf_bloom_refresh,      0, NULL },
	{ "\0", 0, NULL, 0, NULL }
};


static
int modinit(void)
{
	add_top_conf("spamfilter_nicks",NULL,conf_spamfilter_nicks_end,conf_spamfilter_nicks);
	rb_get_random(&bloom_salt,sizeof(bloom_salt));
	resize(conf_bloom_size);
	return 0;
}


static
void modfini(void)
{
	bloom_destroy();
	irc_radixtree_destroy(chans,NULL,NULL);
	remove_top_conf("spamfilter_nicks");
}


mapi_hfn_list_av1 hfnlist[] =
{
	{ "spamfilter_query", (hookfn)hook_spamfilter_query        },
	{ "channel_join", (hookfn)hook_channel_join                },
	{ NULL, NULL                                               }
};

DECLARE_MODULE_AV1
(
	spamfilter_nicks,
	modinit,
	modfini,
	NULL,
	NULL,
	hfnlist,
	"$Revision: 0 $"
);
