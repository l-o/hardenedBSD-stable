/*-
 * Copyright (c) 2002-2009 Luigi Rizzo, Universita` di Pisa
 *
 * Supported by: Valeria Paoli
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Sockopt support for ipfw. The routines here implement
 * the upper half of the ipfw code.
 */

#include "opt_ipfw.h"
#include "opt_inet.h"
#ifndef INET
#error IPFIREWALL requires INET.
#endif /* INET */
#include "opt_inet6.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>	/* struct m_tag used by nested headers */
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/fnv_hash.h>
#include <net/if.h>
#include <net/route.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip_var.h> /* hooks */
#include <netinet/ip_fw.h>

#include <netpfil/ipfw/ip_fw_private.h>
#include <netpfil/ipfw/ip_fw_table.h>

#ifdef MAC
#include <security/mac/mac_framework.h>
#endif

static int ipfw_ctl(struct sockopt *sopt);
static int check_ipfw_rule_body(ipfw_insn *cmd, int cmd_len,
    struct rule_check_info *ci);
static int check_ipfw_rule1(struct ip_fw_rule *rule, int size,
    struct rule_check_info *ci);
static int check_ipfw_rule0(struct ip_fw_rule0 *rule, int size,
    struct rule_check_info *ci);

#define	NAMEDOBJ_HASH_SIZE	32

struct namedobj_instance {
	struct namedobjects_head	*names;
	struct namedobjects_head	*values;
	uint32_t nn_size;		/* names hash size */
	uint32_t nv_size;		/* number hash size */
	u_long *idx_mask;		/* used items bitmask */
	uint32_t max_blocks;		/* number of "long" blocks in bitmask */
	uint32_t count;			/* number of items */
	uint16_t free_off[IPFW_MAX_SETS];	/* first possible free offset */
	objhash_hash_f	*hash_f;
	objhash_cmp_f	*cmp_f;
};
#define	BLOCK_ITEMS	(8 * sizeof(u_long))	/* Number of items for ffsl() */

static uint32_t objhash_hash_name(struct namedobj_instance *ni, void *key,
    uint32_t kopt);
static uint32_t objhash_hash_idx(struct namedobj_instance *ni, uint32_t val);
static int objhash_cmp_name(struct named_object *no, void *name, uint32_t set);

static int ipfw_flush_sopt_data(struct sockopt_data *sd);

MALLOC_DEFINE(M_IPFW, "IpFw/IpAcct", "IpFw/IpAcct chain's");

/*
 * static variables followed by global ones
 */

#ifndef USERSPACE

static VNET_DEFINE(uma_zone_t, ipfw_cntr_zone);
#define	V_ipfw_cntr_zone		VNET(ipfw_cntr_zone)

void
ipfw_init_counters()
{

	V_ipfw_cntr_zone = uma_zcreate("IPFW counters",
	    sizeof(ip_fw_cntr), NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, UMA_ZONE_PCPU);
}

void
ipfw_destroy_counters()
{
	
	uma_zdestroy(V_ipfw_cntr_zone);
}

struct ip_fw *
ipfw_alloc_rule(struct ip_fw_chain *chain, size_t rulesize)
{
	struct ip_fw *rule;

	rule = malloc(rulesize, M_IPFW, M_WAITOK | M_ZERO);
	rule->cntr = uma_zalloc(V_ipfw_cntr_zone, M_WAITOK | M_ZERO);

	return (rule);
}

static void
free_rule(struct ip_fw *rule)
{

	uma_zfree(V_ipfw_cntr_zone, rule->cntr);
	free(rule, M_IPFW);
}
#else
void
ipfw_init_counters()
{
}

void
ipfw_destroy_counters()
{
}

struct ip_fw *
ipfw_alloc_rule(struct ip_fw_chain *chain, size_t rulesize)
{
	struct ip_fw *rule;

	rule = malloc(rulesize, M_IPFW, M_WAITOK | M_ZERO);

	return (rule);
}

static void
free_rule(struct ip_fw *rule)
{

	free(rule, M_IPFW);
}

#endif


/*
 * Find the smallest rule >= key, id.
 * We could use bsearch but it is so simple that we code it directly
 */
int
ipfw_find_rule(struct ip_fw_chain *chain, uint32_t key, uint32_t id)
{
	int i, lo, hi;
	struct ip_fw *r;

  	for (lo = 0, hi = chain->n_rules - 1; lo < hi;) {
		i = (lo + hi) / 2;
		r = chain->map[i];
		if (r->rulenum < key)
			lo = i + 1;	/* continue from the next one */
		else if (r->rulenum > key)
			hi = i;		/* this might be good */
		else if (r->id < id)
			lo = i + 1;	/* continue from the next one */
		else /* r->id >= id */
			hi = i;		/* this might be good */
	};
	return hi;
}

/*
 * Builds skipto cache on rule set @map.
 */
static void
update_skipto_cache(struct ip_fw_chain *chain, struct ip_fw **map)
{
	int *smap, rulenum;
	int i, mi;

	IPFW_UH_WLOCK_ASSERT(chain);

	mi = 0;
	rulenum = map[mi]->rulenum;
	smap = chain->idxmap_back;

	if (smap == NULL)
		return;

	for (i = 0; i < 65536; i++) {
		smap[i] = mi;
		/* Use the same rule index until i < rulenum */
		if (i != rulenum || i == 65535)
			continue;
		/* Find next rule with num > i */
		rulenum = map[++mi]->rulenum;
		while (rulenum == i)
			rulenum = map[++mi]->rulenum;
	}
}

/*
 * Swaps prepared (backup) index with current one.
 */
static void
swap_skipto_cache(struct ip_fw_chain *chain)
{
	int *map;

	IPFW_UH_WLOCK_ASSERT(chain);
	IPFW_WLOCK_ASSERT(chain);

	map = chain->idxmap;
	chain->idxmap = chain->idxmap_back;
	chain->idxmap_back = map;
}

/*
 * Allocate and initialize skipto cache.
 */
void
ipfw_init_skipto_cache(struct ip_fw_chain *chain)
{
	int *idxmap, *idxmap_back;

	idxmap = malloc(65536 * sizeof(uint32_t *), M_IPFW,
	    M_WAITOK | M_ZERO);
	idxmap_back = malloc(65536 * sizeof(uint32_t *), M_IPFW,
	    M_WAITOK | M_ZERO);

	/*
	 * Note we may be called at any time after initialization,
	 * for example, on first skipto rule, so we need to
	 * provide valid chain->idxmap on return
	 */

	IPFW_UH_WLOCK(chain);
	if (chain->idxmap != NULL) {
		IPFW_UH_WUNLOCK(chain);
		free(idxmap, M_IPFW);
		free(idxmap_back, M_IPFW);
		return;
	}

	/* Set backup pointer first to permit building cache */
	chain->idxmap_back = idxmap_back;
	update_skipto_cache(chain, chain->map);
	IPFW_WLOCK(chain);
	/* It is now safe to set chain->idxmap ptr */
	chain->idxmap = idxmap;
	swap_skipto_cache(chain);
	IPFW_WUNLOCK(chain);
	IPFW_UH_WUNLOCK(chain);
}

/*
 * Destroys skipto cache.
 */
void
ipfw_destroy_skipto_cache(struct ip_fw_chain *chain)
{

	if (chain->idxmap != NULL)
		free(chain->idxmap, M_IPFW);
	if (chain->idxmap != NULL)
		free(chain->idxmap_back, M_IPFW);
}


/*
 * allocate a new map, returns the chain locked. extra is the number
 * of entries to add or delete.
 */
static struct ip_fw **
get_map(struct ip_fw_chain *chain, int extra, int locked)
{

	for (;;) {
		struct ip_fw **map;
		int i, mflags;

		mflags = M_ZERO | ((locked != 0) ? M_NOWAIT : M_WAITOK);

		i = chain->n_rules + extra;
		map = malloc(i * sizeof(struct ip_fw *), M_IPFW, mflags);
		if (map == NULL) {
			printf("%s: cannot allocate map\n", __FUNCTION__);
			return NULL;
		}
		if (!locked)
			IPFW_UH_WLOCK(chain);
		if (i >= chain->n_rules + extra) /* good */
			return map;
		/* otherwise we lost the race, free and retry */
		if (!locked)
			IPFW_UH_WUNLOCK(chain);
		free(map, M_IPFW);
	}
}

/*
 * swap the maps. It is supposed to be called with IPFW_UH_WLOCK
 */
static struct ip_fw **
swap_map(struct ip_fw_chain *chain, struct ip_fw **new_map, int new_len)
{
	struct ip_fw **old_map;

	IPFW_WLOCK(chain);
	chain->id++;
	chain->n_rules = new_len;
	old_map = chain->map;
	chain->map = new_map;
	swap_skipto_cache(chain);
	IPFW_WUNLOCK(chain);
	return old_map;
}


static void
export_cntr1_base(struct ip_fw *krule, struct ip_fw_bcounter *cntr)
{

	cntr->size = sizeof(*cntr);

	if (krule->cntr != NULL) {
		cntr->pcnt = counter_u64_fetch(krule->cntr);
		cntr->bcnt = counter_u64_fetch(krule->cntr + 1);
		cntr->timestamp = krule->timestamp;
	}
	if (cntr->timestamp > 0)
		cntr->timestamp += boottime.tv_sec;
}

static void
export_cntr0_base(struct ip_fw *krule, struct ip_fw_bcounter0 *cntr)
{

	if (krule->cntr != NULL) {
		cntr->pcnt = counter_u64_fetch(krule->cntr);
		cntr->bcnt = counter_u64_fetch(krule->cntr + 1);
		cntr->timestamp = krule->timestamp;
	}
	if (cntr->timestamp > 0)
		cntr->timestamp += boottime.tv_sec;
}

/*
 * Copies rule @urule from v1 userland format (current).
 * to kernel @krule.
 * Assume @krule is zeroed.
 */
static void
import_rule1(struct rule_check_info *ci)
{
	struct ip_fw_rule *urule;
	struct ip_fw *krule;

	urule = (struct ip_fw_rule *)ci->urule;
	krule = (struct ip_fw *)ci->krule;

	/* copy header */
	krule->act_ofs = urule->act_ofs;
	krule->cmd_len = urule->cmd_len;
	krule->rulenum = urule->rulenum;
	krule->set = urule->set;
	krule->flags = urule->flags;

	/* Save rulenum offset */
	ci->urule_numoff = offsetof(struct ip_fw_rule, rulenum);

	/* Copy opcodes */
	memcpy(krule->cmd, urule->cmd, krule->cmd_len * sizeof(uint32_t));
}

/*
 * Export rule into v1 format (Current).
 * Layout:
 * [ ipfw_obj_tlv(IPFW_TLV_RULE_ENT)
 *     [ ip_fw_rule ] OR
 *     [ ip_fw_bcounter ip_fw_rule] (depends on rcntrs).
 * ]
 * Assume @data is zeroed.
 */
static void
export_rule1(struct ip_fw *krule, caddr_t data, int len, int rcntrs)
{
	struct ip_fw_bcounter *cntr;
	struct ip_fw_rule *urule;
	ipfw_obj_tlv *tlv;

	/* Fill in TLV header */
	tlv = (ipfw_obj_tlv *)data;
	tlv->type = IPFW_TLV_RULE_ENT;
	tlv->length = len;

	if (rcntrs != 0) {
		/* Copy counters */
		cntr = (struct ip_fw_bcounter *)(tlv + 1);
		urule = (struct ip_fw_rule *)(cntr + 1);
		export_cntr1_base(krule, cntr);
	} else
		urule = (struct ip_fw_rule *)(tlv + 1);

	/* copy header */
	urule->act_ofs = krule->act_ofs;
	urule->cmd_len = krule->cmd_len;
	urule->rulenum = krule->rulenum;
	urule->set = krule->set;
	urule->flags = krule->flags;
	urule->id = krule->id;

	/* Copy opcodes */
	memcpy(urule->cmd, krule->cmd, krule->cmd_len * sizeof(uint32_t));
}


/*
 * Copies rule @urule from FreeBSD8 userland format (v0)
 * to kernel @krule.
 * Assume @krule is zeroed.
 */
static void
import_rule0(struct rule_check_info *ci)
{
	struct ip_fw_rule0 *urule;
	struct ip_fw *krule;
	int cmdlen, l;
	ipfw_insn *cmd;
	ipfw_insn_limit *lcmd;
	ipfw_insn_if *cmdif;

	urule = (struct ip_fw_rule0 *)ci->urule;
	krule = (struct ip_fw *)ci->krule;

	/* copy header */
	krule->act_ofs = urule->act_ofs;
	krule->cmd_len = urule->cmd_len;
	krule->rulenum = urule->rulenum;
	krule->set = urule->set;
	if ((urule->_pad & 1) != 0)
		krule->flags |= IPFW_RULE_NOOPT;

	/* Save rulenum offset */
	ci->urule_numoff = offsetof(struct ip_fw_rule0, rulenum);

	/* Copy opcodes */
	memcpy(krule->cmd, urule->cmd, krule->cmd_len * sizeof(uint32_t));

	/*
	 * Alter opcodes:
	 * 1) convert tablearg value from 65335 to 0
	 * 2) Add high bit to O_SETFIB/O_SETDSCP values (to make room for targ).
	 * 3) convert table number in iface opcodes to u16
	 */
	l = krule->cmd_len;
	cmd = krule->cmd;
	cmdlen = 0;

	for ( ;	l > 0 ; l -= cmdlen, cmd += cmdlen) {
		cmdlen = F_LEN(cmd);

		switch (cmd->opcode) {
		/* Opcodes supporting tablearg */
		case O_TAG:
		case O_TAGGED:
		case O_PIPE:
		case O_QUEUE:
		case O_DIVERT:
		case O_TEE:
		case O_SKIPTO:
		case O_CALLRETURN:
		case O_NETGRAPH:
		case O_NGTEE:
		case O_NAT:
			if (cmd->arg1 == 65535)
				cmd->arg1 = IP_FW_TARG;
			break;
		case O_SETFIB:
		case O_SETDSCP:
			if (cmd->arg1 == 65535)
				cmd->arg1 = IP_FW_TARG;
			else
				cmd->arg1 |= 0x8000;
			break;
		case O_LIMIT:
			lcmd = (ipfw_insn_limit *)cmd;
			if (lcmd->conn_limit == 65535)
				lcmd->conn_limit = IP_FW_TARG;
			break;
		/* Interface tables */
		case O_XMIT:
		case O_RECV:
		case O_VIA:
			/* Interface table, possibly */
			cmdif = (ipfw_insn_if *)cmd;
			if (cmdif->name[0] != '\1')
				break;

			cmdif->p.kidx = (uint16_t)cmdif->p.glob;
			break;
		}
	}
}

/*
 * Copies rule @krule from kernel to FreeBSD8 userland format (v0)
 */
static void
export_rule0(struct ip_fw *krule, struct ip_fw_rule0 *urule, int len)
{
	int cmdlen, l;
	ipfw_insn *cmd;
	ipfw_insn_limit *lcmd;
	ipfw_insn_if *cmdif;

	/* copy header */
	memset(urule, 0, len);
	urule->act_ofs = krule->act_ofs;
	urule->cmd_len = krule->cmd_len;
	urule->rulenum = krule->rulenum;
	urule->set = krule->set;
	if ((krule->flags & IPFW_RULE_NOOPT) != 0)
		urule->_pad |= 1;

	/* Copy opcodes */
	memcpy(urule->cmd, krule->cmd, krule->cmd_len * sizeof(uint32_t));

	/* Export counters */
	export_cntr0_base(krule, (struct ip_fw_bcounter0 *)&urule->pcnt);

	/*
	 * Alter opcodes:
	 * 1) convert tablearg value from 0 to 65335
	 * 2) Remove highest bit from O_SETFIB/O_SETDSCP values.
	 * 3) convert table number in iface opcodes to int
	 */
	l = urule->cmd_len;
	cmd = urule->cmd;
	cmdlen = 0;

	for ( ;	l > 0 ; l -= cmdlen, cmd += cmdlen) {
		cmdlen = F_LEN(cmd);

		switch (cmd->opcode) {
		/* Opcodes supporting tablearg */
		case O_TAG:
		case O_TAGGED:
		case O_PIPE:
		case O_QUEUE:
		case O_DIVERT:
		case O_TEE:
		case O_SKIPTO:
		case O_CALLRETURN:
		case O_NETGRAPH:
		case O_NGTEE:
		case O_NAT:
			if (cmd->arg1 == IP_FW_TARG)
				cmd->arg1 = 65535;
			break;
		case O_SETFIB:
		case O_SETDSCP:
			if (cmd->arg1 == IP_FW_TARG)
				cmd->arg1 = 65535;
			else
				cmd->arg1 &= ~0x8000;
			break;
		case O_LIMIT:
			lcmd = (ipfw_insn_limit *)cmd;
			if (lcmd->conn_limit == IP_FW_TARG)
				lcmd->conn_limit = 65535;
			break;
		/* Interface tables */
		case O_XMIT:
		case O_RECV:
		case O_VIA:
			/* Interface table, possibly */
			cmdif = (ipfw_insn_if *)cmd;
			if (cmdif->name[0] != '\1')
				break;

			cmdif->p.glob = cmdif->p.kidx;
			break;
		}
	}
}

/*
 * Add new rule(s) to the list possibly creating rule number for each.
 * Update the rule_number in the input struct so the caller knows it as well.
 * Must be called without IPFW_UH held
 */
static int
commit_rules(struct ip_fw_chain *chain, struct rule_check_info *rci, int count)
{
	int error, i, insert_before, tcount;
	uint16_t rulenum, *pnum;
	struct rule_check_info *ci;
	struct ip_fw *krule;
	struct ip_fw **map;	/* the new array of pointers */

	/* Check if we need to do table remap */
	tcount = 0;
	for (ci = rci, i = 0; i < count; ci++, i++) {
		if (ci->table_opcodes == 0)
			continue;

		/*
		 * Rule has some table opcodes.
		 * Reference & allocate needed tables/
		 */
		error = ipfw_rewrite_table_uidx(chain, ci);
		if (error != 0) {

			/*
			 * rewrite failed, state for current rule
			 * has been reverted. Check if we need to
			 * revert more.
			 */
			if (tcount > 0) {

				/*
				 * We have some more table rules
				 * we need to rollback.
				 */

				IPFW_UH_WLOCK(chain);
				while (ci != rci) {
					ci--;
					if (ci->table_opcodes == 0)
						continue;
					ipfw_unref_rule_tables(chain,ci->krule);

				}
				IPFW_UH_WUNLOCK(chain);

			}

			return (error);
		}

		tcount++;
	}

	/* get_map returns with IPFW_UH_WLOCK if successful */
	map = get_map(chain, count, 0 /* not locked */);
	if (map == NULL) {
		if (tcount > 0) {
			/* Unbind tables */
			IPFW_UH_WLOCK(chain);
			for (ci = rci, i = 0; i < count; ci++, i++) {
				if (ci->table_opcodes == 0)
					continue;

				ipfw_unref_rule_tables(chain, ci->krule);
			}
			IPFW_UH_WUNLOCK(chain);
		}

		return (ENOSPC);
	}

	if (V_autoinc_step < 1)
		V_autoinc_step = 1;
	else if (V_autoinc_step > 1000)
		V_autoinc_step = 1000;

	/* FIXME: Handle count > 1 */
	ci = rci;
	krule = ci->krule;
	rulenum = krule->rulenum;

	/* find the insertion point, we will insert before */
	insert_before = rulenum ? rulenum + 1 : IPFW_DEFAULT_RULE;
	i = ipfw_find_rule(chain, insert_before, 0);
	/* duplicate first part */
	if (i > 0)
		bcopy(chain->map, map, i * sizeof(struct ip_fw *));
	map[i] = krule;
	/* duplicate remaining part, we always have the default rule */
	bcopy(chain->map + i, map + i + 1,
		sizeof(struct ip_fw *) *(chain->n_rules - i));
	if (rulenum == 0) {
		/* Compute rule number and write it back */
		rulenum = i > 0 ? map[i-1]->rulenum : 0;
		if (rulenum < IPFW_DEFAULT_RULE - V_autoinc_step)
			rulenum += V_autoinc_step;
		krule->rulenum = rulenum;
		/* Save number to userland rule */
		pnum = (uint16_t *)((caddr_t)ci->urule + ci->urule_numoff);
		*pnum = rulenum;
	}

	krule->id = chain->id + 1;
	update_skipto_cache(chain, map);
	map = swap_map(chain, map, chain->n_rules + 1);
	chain->static_len += RULEUSIZE0(krule);
	IPFW_UH_WUNLOCK(chain);
	if (map)
		free(map, M_IPFW);
	return (0);
}

/*
 * Adds @rule to the list of rules to reap
 */
void
ipfw_reap_add(struct ip_fw_chain *chain, struct ip_fw **head,
    struct ip_fw *rule)
{

	IPFW_UH_WLOCK_ASSERT(chain);

	/* Unlink rule from everywhere */
	ipfw_unref_rule_tables(chain, rule);

	*((struct ip_fw **)rule) = *head;
	*head = rule;
}

/*
 * Reclaim storage associated with a list of rules.  This is
 * typically the list created using remove_rule.
 * A NULL pointer on input is handled correctly.
 */
void
ipfw_reap_rules(struct ip_fw *head)
{
	struct ip_fw *rule;

	while ((rule = head) != NULL) {
		head = *((struct ip_fw **)head);
		free_rule(rule);
	}
}

/*
 * Rules to keep are
 *	(default || reserved || !match_set || !match_number)
 * where
 *   default ::= (rule->rulenum == IPFW_DEFAULT_RULE)
 *	// the default rule is always protected
 *
 *   reserved ::= (cmd == 0 && n == 0 && rule->set == RESVD_SET)
 *	// RESVD_SET is protected only if cmd == 0 and n == 0 ("ipfw flush")
 *
 *   match_set ::= (cmd == 0 || rule->set == set)
 *	// set number is ignored for cmd == 0
 *
 *   match_number ::= (cmd == 1 || n == 0 || n == rule->rulenum)
 *	// number is ignored for cmd == 1 or n == 0
 *
 */
int
ipfw_match_range(struct ip_fw *rule, ipfw_range_tlv *rt)
{

	/* Don't match default rule regardless of query */
	if (rule->rulenum == IPFW_DEFAULT_RULE)
		return (0);

	/* Don't match rules in reserved set for flush requests */
	if ((rt->flags & IPFW_RCFLAG_ALL) != 0 && rule->set == RESVD_SET)
		return (0);

	/* If we're filtering by set, don't match other sets */
	if ((rt->flags & IPFW_RCFLAG_SET) != 0 && rule->set != rt->set)
		return (0);

	if ((rt->flags & IPFW_RCFLAG_RANGE) != 0 &&
	    (rule->rulenum < rt->start_rule || rule->rulenum > rt->end_rule))
		return (0);

	return (1);
}

/*
 * Delete rules matching range @rt.
 * Saves number of deleted rules in @ndel.
 *
 * Returns 0 on success.
 */
static int
delete_range(struct ip_fw_chain *chain, ipfw_range_tlv *rt, int *ndel)
{
	struct ip_fw *reap, *rule, **map;
	int end, start;
	int i, n, ndyn, ofs;

	reap = NULL;
	IPFW_UH_WLOCK(chain);	/* arbitrate writers */

	/*
	 * Stage 1: Determine range to inspect.
	 * Range is half-inclusive, e.g [start, end).
	 */
	start = 0;
	end = chain->n_rules - 1;

	if ((rt->flags & IPFW_RCFLAG_RANGE) != 0) {
		start = ipfw_find_rule(chain, rt->start_rule, 0);

		end = ipfw_find_rule(chain, rt->end_rule, 0);
		if (rt->end_rule != IPFW_DEFAULT_RULE)
			while (chain->map[end]->rulenum == rt->end_rule)
				end++;
	}

	/* Allocate new map of the same size */
	map = get_map(chain, 0, 1 /* locked */);
	if (map == NULL) {
		IPFW_UH_WUNLOCK(chain);
		return (ENOMEM);
	}

	n = 0;
	ndyn = 0;
	ofs = start;
	/* 1. bcopy the initial part of the map */
	if (start > 0)
		bcopy(chain->map, map, start * sizeof(struct ip_fw *));
	/* 2. copy active rules between start and end */
	for (i = start; i < end; i++) {
		rule = chain->map[i];
		if (ipfw_match_range(rule, rt) == 0) {
			map[ofs++] = rule;
			continue;
		}

		n++;
		if (ipfw_is_dyn_rule(rule) != 0)
			ndyn++;
	}
	/* 3. copy the final part of the map */
	bcopy(chain->map + end, map + ofs,
		(chain->n_rules - end) * sizeof(struct ip_fw *));
	/* 4. recalculate skipto cache */
	update_skipto_cache(chain, map);
	/* 5. swap the maps (under UH_WLOCK + WHLOCK) */
	map = swap_map(chain, map, chain->n_rules - n);
	/* 6. Remove all dynamic states originated by deleted rules */
	if (ndyn > 0)
		ipfw_expire_dyn_rules(chain, rt);
	/* 7. now remove the rules deleted from the old map */
	for (i = start; i < end; i++) {
		rule = map[i];
		if (ipfw_match_range(rule, rt) == 0)
			continue;
		chain->static_len -= RULEUSIZE0(rule);
		ipfw_reap_add(chain, &reap, rule);
	}
	IPFW_UH_WUNLOCK(chain);

	ipfw_reap_rules(reap);
	if (map != NULL)
		free(map, M_IPFW);
	*ndel = n;
	return (0);
}

/*
 * Changes set of given rule rannge @rt
 * with each other.
 *
 * Returns 0 on success.
 */
static int
move_range(struct ip_fw_chain *chain, ipfw_range_tlv *rt)
{
	struct ip_fw *rule;
	int i;

	IPFW_UH_WLOCK(chain);

	/*
	 * Move rules with matching paramenerts to a new set.
	 * This one is much more complex. We have to ensure
	 * that all referenced tables (if any) are referenced
	 * by given rule subset only. Otherwise, we can't move
	 * them to new set and have to return error.
	 */
	if (V_fw_tables_sets != 0) {
		if (ipfw_move_tables_sets(chain, rt, rt->new_set) != 0) {
			IPFW_UH_WUNLOCK(chain);
			return (EBUSY);
		}
	}

	/* XXX: We have to do swap holding WLOCK */
	for (i = 0; i < chain->n_rules - 1; i++) {
		rule = chain->map[i];
		if (ipfw_match_range(rule, rt) == 0)
			continue;
		rule->set = rt->new_set;
	}

	IPFW_UH_WUNLOCK(chain);

	return (0);
}

/*
 * Clear counters for a specific rule.
 * Normally run under IPFW_UH_RLOCK, but these are idempotent ops
 * so we only care that rules do not disappear.
 */
static void
clear_counters(struct ip_fw *rule, int log_only)
{
	ipfw_insn_log *l = (ipfw_insn_log *)ACTION_PTR(rule);

	if (log_only == 0)
		IPFW_ZERO_RULE_COUNTER(rule);
	if (l->o.opcode == O_LOG)
		l->log_left = l->max_log;
}

/*
 * Flushes rules counters and/or log values on matching range.
 *
 * Returns number of items cleared.
 */
static int
clear_range(struct ip_fw_chain *chain, ipfw_range_tlv *rt, int log_only)
{
	struct ip_fw *rule;
	int num;
	int i;

	num = 0;

	IPFW_UH_WLOCK(chain);	/* arbitrate writers */
	for (i = 0; i < chain->n_rules - 1; i++) {
		rule = chain->map[i];
		if (ipfw_match_range(rule, rt) == 0)
			continue;
		clear_counters(rule, log_only);
		num++;
	}
	IPFW_UH_WUNLOCK(chain);

	return (num);
}

static int
check_range_tlv(ipfw_range_tlv *rt)
{

	if (rt->head.length != sizeof(*rt))
		return (1);
	if (rt->start_rule > rt->end_rule)
		return (1);
	if (rt->set >= IPFW_MAX_SETS || rt->new_set >= IPFW_MAX_SETS)
		return (1);

	return (0);
}

/*
 * Delete rules matching specified parameters
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_header ipfw_range_tlv ]
 * Reply: [ ipfw_obj_header ipfw_range_tlv ]
 *
 * Saves number of deleted rules in ipfw_range_tlv->new_set.
 *
 * Returns 0 on success.
 */
static int
del_rules(struct ip_fw_chain *chain, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_range_header *rh;
	int error, ndel;

	if (sd->valsize != sizeof(*rh))
		return (EINVAL);

	rh = (ipfw_range_header *)ipfw_get_sopt_space(sd, sd->valsize);

	if (check_range_tlv(&rh->range) != 0)
		return (EINVAL);

	ndel = 0;
	if ((error = delete_range(chain, &rh->range, &ndel)) != 0)
		return (error);

	/* Save number of rules deleted */
	rh->range.new_set = ndel;
	return (0);
}

/*
 * Move rules/sets matching specified parameters
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_header ipfw_range_tlv ]
 *
 * Returns 0 on success.
 */
static int
move_rules(struct ip_fw_chain *chain, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_range_header *rh;

	if (sd->valsize != sizeof(*rh))
		return (EINVAL);

	rh = (ipfw_range_header *)ipfw_get_sopt_space(sd, sd->valsize);

	if (check_range_tlv(&rh->range) != 0)
		return (EINVAL);

	return (move_range(chain, &rh->range));
}

/*
 * Clear rule accounting data matching specified parameters
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_header ipfw_range_tlv ]
 * Reply: [ ipfw_obj_header ipfw_range_tlv ]
 *
 * Saves number of cleared rules in ipfw_range_tlv->new_set.
 *
 * Returns 0 on success.
 */
static int
clear_rules(struct ip_fw_chain *chain, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_range_header *rh;
	int log_only, num;
	char *msg;

	if (sd->valsize != sizeof(*rh))
		return (EINVAL);

	rh = (ipfw_range_header *)ipfw_get_sopt_space(sd, sd->valsize);

	if (check_range_tlv(&rh->range) != 0)
		return (EINVAL);

	log_only = (op3->opcode == IP_FW_XRESETLOG);

	num = clear_range(chain, &rh->range, log_only);

	if (rh->range.flags & IPFW_RCFLAG_ALL)
		msg = log_only ? "All logging counts reset" :
		    "Accounting cleared";
	else
		msg = log_only ? "logging count reset" : "cleared";

	if (V_fw_verbose) {
		int lev = LOG_SECURITY | LOG_NOTICE;
		log(lev, "ipfw: %s.\n", msg);
	}

	/* Save number of rules cleared */
	rh->range.new_set = num;
	return (0);
}

static void
enable_sets(struct ip_fw_chain *chain, ipfw_range_tlv *rt)
{
	uint32_t v_set;

	IPFW_UH_WLOCK_ASSERT(chain);

	/* Change enabled/disabled sets mask */
	v_set = (V_set_disable | rt->set) & ~rt->new_set;
	v_set &= ~(1 << RESVD_SET); /* set RESVD_SET always enabled */
	IPFW_WLOCK(chain);
	V_set_disable = v_set;
	IPFW_WUNLOCK(chain);
}

static void
swap_sets(struct ip_fw_chain *chain, ipfw_range_tlv *rt, int mv)
{
	struct ip_fw *rule;
	int i;

	IPFW_UH_WLOCK_ASSERT(chain);

	/* Swap or move two sets */
	for (i = 0; i < chain->n_rules - 1; i++) {
		rule = chain->map[i];
		if (rule->set == rt->set)
			rule->set = rt->new_set;
		else if (rule->set == rt->new_set && mv == 0)
			rule->set = rt->set;
	}
	if (V_fw_tables_sets != 0)
		ipfw_swap_tables_sets(chain, rt->set, rt->new_set, mv);
}

/*
 * Swaps or moves set
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_header ipfw_range_tlv ]
 *
 * Returns 0 on success.
 */
static int
manage_sets(struct ip_fw_chain *chain, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_range_header *rh;

	if (sd->valsize != sizeof(*rh))
		return (EINVAL);

	rh = (ipfw_range_header *)ipfw_get_sopt_space(sd, sd->valsize);

	if (rh->range.head.length != sizeof(ipfw_range_tlv))
		return (1);

	IPFW_UH_WLOCK(chain);
	switch (op3->opcode) {
	case IP_FW_SET_SWAP:
	case IP_FW_SET_MOVE:
		swap_sets(chain, &rh->range, op3->opcode == IP_FW_SET_MOVE);
		break;
	case IP_FW_SET_ENABLE:
		enable_sets(chain, &rh->range);
		break;
	}
	IPFW_UH_WUNLOCK(chain);

	return (0);
}

/**
 * Remove all rules with given number, or do set manipulation.
 * Assumes chain != NULL && *chain != NULL.
 *
 * The argument is an uint32_t. The low 16 bit are the rule or set number;
 * the next 8 bits are the new set; the top 8 bits indicate the command:
 *
 *	0	delete rules numbered "rulenum"
 *	1	delete rules in set "rulenum"
 *	2	move rules "rulenum" to set "new_set"
 *	3	move rules from set "rulenum" to set "new_set"
 *	4	swap sets "rulenum" and "new_set"
 *	5	delete rules "rulenum" and set "new_set"
 */
static int
del_entry(struct ip_fw_chain *chain, uint32_t arg)
{
	uint32_t num;	/* rule number or old_set */
	uint8_t cmd, new_set;
	int do_del, ndel;
	int error = 0;
	ipfw_range_tlv rt;

	num = arg & 0xffff;
	cmd = (arg >> 24) & 0xff;
	new_set = (arg >> 16) & 0xff;

	if (cmd > 5 || new_set > RESVD_SET)
		return EINVAL;
	if (cmd == 0 || cmd == 2 || cmd == 5) {
		if (num >= IPFW_DEFAULT_RULE)
			return EINVAL;
	} else {
		if (num > RESVD_SET)	/* old_set */
			return EINVAL;
	}

	/* Convert old requests into new representation */
	memset(&rt, 0, sizeof(rt));
	rt.start_rule = num;
	rt.end_rule = num;
	rt.set = num;
	rt.new_set = new_set;
	do_del = 0;

	switch (cmd) {
	case 0: /* delete rules numbered "rulenum" */
		if (num == 0)
			rt.flags |= IPFW_RCFLAG_ALL;
		else
			rt.flags |= IPFW_RCFLAG_RANGE;
		do_del = 1;
		break;
	case 1: /* delete rules in set "rulenum" */
		rt.flags |= IPFW_RCFLAG_SET;
		do_del = 1;
		break;
	case 5: /* delete rules "rulenum" and set "new_set" */
		rt.flags |= IPFW_RCFLAG_RANGE | IPFW_RCFLAG_SET;
		rt.set = new_set;
		rt.new_set = 0;
		do_del = 1;
		break;
	case 2: /* move rules "rulenum" to set "new_set" */
		rt.flags |= IPFW_RCFLAG_RANGE;
		break;
	case 3: /* move rules from set "rulenum" to set "new_set" */
		IPFW_UH_WLOCK(chain);
		swap_sets(chain, &rt, 1);
		IPFW_UH_WUNLOCK(chain);
		return (0);
	case 4: /* swap sets "rulenum" and "new_set" */
		IPFW_UH_WLOCK(chain);
		swap_sets(chain, &rt, 0);
		IPFW_UH_WUNLOCK(chain);
		return (0);
	default:
		return (ENOTSUP);
	}

	if (do_del != 0) {
		if ((error = delete_range(chain, &rt, &ndel)) != 0)
			return (error);

		if (ndel == 0 && (cmd != 1 && num != 0))
			return (EINVAL);

		return (0);
	}

	return (move_range(chain, &rt));
}

/**
 * Reset some or all counters on firewall rules.
 * The argument `arg' is an u_int32_t. The low 16 bit are the rule number,
 * the next 8 bits are the set number, the top 8 bits are the command:
 *	0	work with rules from all set's;
 *	1	work with rules only from specified set.
 * Specified rule number is zero if we want to clear all entries.
 * log_only is 1 if we only want to reset logs, zero otherwise.
 */
static int
zero_entry(struct ip_fw_chain *chain, u_int32_t arg, int log_only)
{
	struct ip_fw *rule;
	char *msg;
	int i;

	uint16_t rulenum = arg & 0xffff;
	uint8_t set = (arg >> 16) & 0xff;
	uint8_t cmd = (arg >> 24) & 0xff;

	if (cmd > 1)
		return (EINVAL);
	if (cmd == 1 && set > RESVD_SET)
		return (EINVAL);

	IPFW_UH_RLOCK(chain);
	if (rulenum == 0) {
		V_norule_counter = 0;
		for (i = 0; i < chain->n_rules; i++) {
			rule = chain->map[i];
			/* Skip rules not in our set. */
			if (cmd == 1 && rule->set != set)
				continue;
			clear_counters(rule, log_only);
		}
		msg = log_only ? "All logging counts reset" :
		    "Accounting cleared";
	} else {
		int cleared = 0;
		for (i = 0; i < chain->n_rules; i++) {
			rule = chain->map[i];
			if (rule->rulenum == rulenum) {
				if (cmd == 0 || rule->set == set)
					clear_counters(rule, log_only);
				cleared = 1;
			}
			if (rule->rulenum > rulenum)
				break;
		}
		if (!cleared) {	/* we did not find any matching rules */
			IPFW_UH_RUNLOCK(chain);
			return (EINVAL);
		}
		msg = log_only ? "logging count reset" : "cleared";
	}
	IPFW_UH_RUNLOCK(chain);

	if (V_fw_verbose) {
		int lev = LOG_SECURITY | LOG_NOTICE;

		if (rulenum)
			log(lev, "ipfw: Entry %d %s.\n", rulenum, msg);
		else
			log(lev, "ipfw: %s.\n", msg);
	}
	return (0);
}


/*
 * Check rule head in FreeBSD11 format
 *
 */
static int
check_ipfw_rule1(struct ip_fw_rule *rule, int size,
    struct rule_check_info *ci)
{
	int l;

	if (size < sizeof(*rule)) {
		printf("ipfw: rule too short\n");
		return (EINVAL);
	}

	/* Check for valid cmd_len */
	l = roundup2(RULESIZE(rule), sizeof(uint64_t));
	if (l != size) {
		printf("ipfw: size mismatch (have %d want %d)\n", size, l);
		return (EINVAL);
	}
	if (rule->act_ofs >= rule->cmd_len) {
		printf("ipfw: bogus action offset (%u > %u)\n",
		    rule->act_ofs, rule->cmd_len - 1);
		return (EINVAL);
	}

	if (rule->rulenum > IPFW_DEFAULT_RULE - 1)
		return (EINVAL);

	return (check_ipfw_rule_body(rule->cmd, rule->cmd_len, ci));
}

/*
 * Check rule head in FreeBSD8 format
 *
 */
static int
check_ipfw_rule0(struct ip_fw_rule0 *rule, int size,
    struct rule_check_info *ci)
{
	int l;

	if (size < sizeof(*rule)) {
		printf("ipfw: rule too short\n");
		return (EINVAL);
	}

	/* Check for valid cmd_len */
	l = sizeof(*rule) + rule->cmd_len * 4 - 4;
	if (l != size) {
		printf("ipfw: size mismatch (have %d want %d)\n", size, l);
		return (EINVAL);
	}
	if (rule->act_ofs >= rule->cmd_len) {
		printf("ipfw: bogus action offset (%u > %u)\n",
		    rule->act_ofs, rule->cmd_len - 1);
		return (EINVAL);
	}

	if (rule->rulenum > IPFW_DEFAULT_RULE - 1)
		return (EINVAL);

	return (check_ipfw_rule_body(rule->cmd, rule->cmd_len, ci));
}

static int
check_ipfw_rule_body(ipfw_insn *cmd, int cmd_len, struct rule_check_info *ci)
{
	int cmdlen, l;
	int have_action;

	have_action = 0;

	/*
	 * Now go for the individual checks. Very simple ones, basically only
	 * instruction sizes.
	 */
	for (l = cmd_len; l > 0 ; l -= cmdlen, cmd += cmdlen) {
		cmdlen = F_LEN(cmd);
		if (cmdlen > l) {
			printf("ipfw: opcode %d size truncated\n",
			    cmd->opcode);
			return EINVAL;
		}
		switch (cmd->opcode) {
		case O_PROBE_STATE:
		case O_KEEP_STATE:
		case O_PROTO:
		case O_IP_SRC_ME:
		case O_IP_DST_ME:
		case O_LAYER2:
		case O_IN:
		case O_FRAG:
		case O_DIVERTED:
		case O_IPOPT:
		case O_IPTOS:
		case O_IPPRECEDENCE:
		case O_IPVER:
		case O_SOCKARG:
		case O_TCPFLAGS:
		case O_TCPOPTS:
		case O_ESTAB:
		case O_VERREVPATH:
		case O_VERSRCREACH:
		case O_ANTISPOOF:
		case O_IPSEC:
#ifdef INET6
		case O_IP6_SRC_ME:
		case O_IP6_DST_ME:
		case O_EXT_HDR:
		case O_IP6:
#endif
		case O_IP4:
		case O_TAG:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
			break;

		case O_FIB:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
			if (cmd->arg1 >= rt_numfibs) {
				printf("ipfw: invalid fib number %d\n",
					cmd->arg1);
				return EINVAL;
			}
			break;

		case O_SETFIB:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
			if ((cmd->arg1 != IP_FW_TARG) &&
			    ((cmd->arg1 & 0x7FFFF) >= rt_numfibs)) {
				printf("ipfw: invalid fib number %d\n",
					cmd->arg1 & 0x7FFFF);
				return EINVAL;
			}
			goto check_action;

		case O_UID:
		case O_GID:
		case O_JAIL:
		case O_IP_SRC:
		case O_IP_DST:
		case O_TCPSEQ:
		case O_TCPACK:
		case O_PROB:
		case O_ICMPTYPE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32))
				goto bad_size;
			break;

		case O_LIMIT:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_limit))
				goto bad_size;
			break;

		case O_LOG:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_log))
				goto bad_size;

			((ipfw_insn_log *)cmd)->log_left =
			    ((ipfw_insn_log *)cmd)->max_log;

			break;

		case O_IP_SRC_MASK:
		case O_IP_DST_MASK:
			/* only odd command lengths */
			if ( !(cmdlen & 1) || cmdlen > 31)
				goto bad_size;
			break;

		case O_IP_SRC_SET:
		case O_IP_DST_SET:
			if (cmd->arg1 == 0 || cmd->arg1 > 256) {
				printf("ipfw: invalid set size %d\n",
					cmd->arg1);
				return EINVAL;
			}
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32) +
			    (cmd->arg1+31)/32 )
				goto bad_size;
			break;

		case O_IP_SRC_LOOKUP:
		case O_IP_DST_LOOKUP:
			if (cmd->arg1 >= V_fw_tables_max) {
				printf("ipfw: invalid table number %d\n",
				    cmd->arg1);
				return (EINVAL);
			}
			if (cmdlen != F_INSN_SIZE(ipfw_insn) &&
			    cmdlen != F_INSN_SIZE(ipfw_insn_u32) + 1 &&
			    cmdlen != F_INSN_SIZE(ipfw_insn_u32))
				goto bad_size;
			ci->table_opcodes++;
			break;
		case O_IP_FLOW_LOOKUP:
			if (cmd->arg1 >= V_fw_tables_max) {
				printf("ipfw: invalid table number %d\n",
				    cmd->arg1);
				return (EINVAL);
			}
			if (cmdlen != F_INSN_SIZE(ipfw_insn) &&
			    cmdlen != F_INSN_SIZE(ipfw_insn_u32))
				goto bad_size;
			ci->table_opcodes++;
			break;
		case O_MACADDR2:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_mac))
				goto bad_size;
			break;

		case O_NOP:
		case O_IPID:
		case O_IPTTL:
		case O_IPLEN:
		case O_TCPDATALEN:
		case O_TCPWIN:
		case O_TAGGED:
			if (cmdlen < 1 || cmdlen > 31)
				goto bad_size;
			break;

		case O_DSCP:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32) + 1)
				goto bad_size;
			break;

		case O_MAC_TYPE:
		case O_IP_SRCPORT:
		case O_IP_DSTPORT: /* XXX artificial limit, 30 port pairs */
			if (cmdlen < 2 || cmdlen > 31)
				goto bad_size;
			break;

		case O_RECV:
		case O_XMIT:
		case O_VIA:
			if (((ipfw_insn_if *)cmd)->name[0] == '\1')
				ci->table_opcodes++;
			if (cmdlen != F_INSN_SIZE(ipfw_insn_if))
				goto bad_size;
			break;

		case O_ALTQ:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_altq))
				goto bad_size;
			break;

		case O_PIPE:
		case O_QUEUE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
			goto check_action;

		case O_FORWARD_IP:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_sa))
				goto bad_size;
			goto check_action;
#ifdef INET6
		case O_FORWARD_IP6:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_sa6))
				goto bad_size;
			goto check_action;
#endif /* INET6 */

		case O_DIVERT:
		case O_TEE:
			if (ip_divert_ptr == NULL)
				return EINVAL;
			else
				goto check_size;
		case O_NETGRAPH:
		case O_NGTEE:
			if (ng_ipfw_input_p == NULL)
				return EINVAL;
			else
				goto check_size;
		case O_NAT:
			if (!IPFW_NAT_LOADED)
				return EINVAL;
			if (cmdlen != F_INSN_SIZE(ipfw_insn_nat))
 				goto bad_size;		
 			goto check_action;
		case O_FORWARD_MAC: /* XXX not implemented yet */
		case O_CHECK_STATE:
		case O_COUNT:
		case O_ACCEPT:
		case O_DENY:
		case O_REJECT:
		case O_SETDSCP:
#ifdef INET6
		case O_UNREACH6:
#endif
		case O_SKIPTO:
		case O_REASS:
		case O_CALLRETURN:
check_size:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
check_action:
			if (have_action) {
				printf("ipfw: opcode %d, multiple actions"
					" not allowed\n",
					cmd->opcode);
				return (EINVAL);
			}
			have_action = 1;
			if (l != cmdlen) {
				printf("ipfw: opcode %d, action must be"
					" last opcode\n",
					cmd->opcode);
				return (EINVAL);
			}
			break;
#ifdef INET6
		case O_IP6_SRC:
		case O_IP6_DST:
			if (cmdlen != F_INSN_SIZE(struct in6_addr) +
			    F_INSN_SIZE(ipfw_insn))
				goto bad_size;
			break;

		case O_FLOW6ID:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32) +
			    ((ipfw_insn_u32 *)cmd)->o.arg1)
				goto bad_size;
			break;

		case O_IP6_SRC_MASK:
		case O_IP6_DST_MASK:
			if ( !(cmdlen & 1) || cmdlen > 127)
				goto bad_size;
			break;
		case O_ICMP6TYPE:
			if( cmdlen != F_INSN_SIZE( ipfw_insn_icmp6 ) )
				goto bad_size;
			break;
#endif

		default:
			switch (cmd->opcode) {
#ifndef INET6
			case O_IP6_SRC_ME:
			case O_IP6_DST_ME:
			case O_EXT_HDR:
			case O_IP6:
			case O_UNREACH6:
			case O_IP6_SRC:
			case O_IP6_DST:
			case O_FLOW6ID:
			case O_IP6_SRC_MASK:
			case O_IP6_DST_MASK:
			case O_ICMP6TYPE:
				printf("ipfw: no IPv6 support in kernel\n");
				return (EPROTONOSUPPORT);
#endif
			default:
				printf("ipfw: opcode %d, unknown opcode\n",
					cmd->opcode);
				return (EINVAL);
			}
		}
	}
	if (have_action == 0) {
		printf("ipfw: missing action\n");
		return (EINVAL);
	}
	return 0;

bad_size:
	printf("ipfw: opcode %d size %d wrong\n",
		cmd->opcode, cmdlen);
	return (EINVAL);
}


/*
 * Translation of requests for compatibility with FreeBSD 7.2/8.
 * a static variable tells us if we have an old client from userland,
 * and if necessary we translate requests and responses between the
 * two formats.
 */
static int is7 = 0;

struct ip_fw7 {
	struct ip_fw7	*next;		/* linked list of rules     */
	struct ip_fw7	*next_rule;	/* ptr to next [skipto] rule    */
	/* 'next_rule' is used to pass up 'set_disable' status      */

	uint16_t	act_ofs;	/* offset of action in 32-bit units */
	uint16_t	cmd_len;	/* # of 32-bit words in cmd */
	uint16_t	rulenum;	/* rule number          */
	uint8_t		set;		/* rule set (0..31)     */
	// #define RESVD_SET   31  /* set for default and persistent rules */
	uint8_t		_pad;		/* padding          */
	// uint32_t        id;             /* rule id, only in v.8 */
	/* These fields are present in all rules.           */
	uint64_t	pcnt;		/* Packet counter       */
	uint64_t	bcnt;		/* Byte counter         */
	uint32_t	timestamp;	/* tv_sec of last match     */

	ipfw_insn	cmd[1];		/* storage for commands     */
};

static int convert_rule_to_7(struct ip_fw_rule0 *rule);
static int convert_rule_to_8(struct ip_fw_rule0 *rule);

#ifndef RULESIZE7
#define RULESIZE7(rule)  (sizeof(struct ip_fw7) + \
	((struct ip_fw7 *)(rule))->cmd_len * 4 - 4)
#endif


/*
 * Copy the static and dynamic rules to the supplied buffer
 * and return the amount of space actually used.
 * Must be run under IPFW_UH_RLOCK
 */
static size_t
ipfw_getrules(struct ip_fw_chain *chain, void *buf, size_t space)
{
	char *bp = buf;
	char *ep = bp + space;
	struct ip_fw *rule;
	struct ip_fw_rule0 *dst;
	int error, i, l, warnflag;
	time_t	boot_seconds;

	warnflag = 0;

        boot_seconds = boottime.tv_sec;
	for (i = 0; i < chain->n_rules; i++) {
		rule = chain->map[i];

		if (is7) {
		    /* Convert rule to FreeBSd 7.2 format */
		    l = RULESIZE7(rule);
		    if (bp + l + sizeof(uint32_t) <= ep) {
			bcopy(rule, bp, l + sizeof(uint32_t));
			error = ipfw_rewrite_table_kidx(chain,
			    (struct ip_fw_rule0 *)bp);
			if (error != 0)
				return (0);
			error = convert_rule_to_7((struct ip_fw_rule0 *) bp);
			if (error)
				return 0; /*XXX correct? */
			/*
			 * XXX HACK. Store the disable mask in the "next"
			 * pointer in a wild attempt to keep the ABI the same.
			 * Why do we do this on EVERY rule?
			 */
			bcopy(&V_set_disable,
				&(((struct ip_fw7 *)bp)->next_rule),
				sizeof(V_set_disable));
			if (((struct ip_fw7 *)bp)->timestamp)
			    ((struct ip_fw7 *)bp)->timestamp += boot_seconds;
			bp += l;
		    }
		    continue; /* go to next rule */
		}

		l = RULEUSIZE0(rule);
		if (bp + l > ep) { /* should not happen */
			printf("overflow dumping static rules\n");
			break;
		}
		dst = (struct ip_fw_rule0 *)bp;
		export_rule0(rule, dst, l);
		error = ipfw_rewrite_table_kidx(chain, dst);

		/*
		 * XXX HACK. Store the disable mask in the "next"
		 * pointer in a wild attempt to keep the ABI the same.
		 * Why do we do this on EVERY rule?
		 *
		 * XXX: "ipfw set show" (ab)uses IP_FW_GET to read disabled mask
		 * so we need to fail _after_ saving at least one mask.
		 */
		bcopy(&V_set_disable, &dst->next_rule, sizeof(V_set_disable));
		if (dst->timestamp)
			dst->timestamp += boot_seconds;
		bp += l;

		if (error != 0) {
			if (error == 2) {
				/* Non-fatal table rewrite error. */
				warnflag = 1;
				continue;
			}
			printf("Stop on rule %d. Fail to convert table\n",
			    rule->rulenum);
			break;
		}
	}
	if (warnflag != 0)
		printf("ipfw: process %s is using legacy interfaces,"
		    " consider rebuilding\n", "");
	ipfw_get_dynamic(chain, &bp, ep); /* protected by the dynamic lock */
	return (bp - (char *)buf);
}


struct dump_args {
	uint32_t	b;	/* start rule */
	uint32_t	e;	/* end rule */
	uint32_t	rcount;	/* number of rules */
	uint32_t	rsize;	/* rules size */
	uint32_t	tcount;	/* number of tables */
	int		rcounters;	/* counters */
};

/*
 * Dumps static rules with table TLVs in buffer @sd.
 *
 * Returns 0 on success.
 */
static int
dump_static_rules(struct ip_fw_chain *chain, struct dump_args *da,
    uint32_t *bmask, struct sockopt_data *sd)
{
	int error;
	int i, l;
	uint32_t tcount;
	ipfw_obj_ctlv *ctlv;
	struct ip_fw *krule;
	caddr_t dst;

	/* Dump table names first (if any) */
	if (da->tcount > 0) {
		/* Header first */
		ctlv = (ipfw_obj_ctlv *)ipfw_get_sopt_space(sd, sizeof(*ctlv));
		if (ctlv == NULL)
			return (ENOMEM);
		ctlv->head.type = IPFW_TLV_TBLNAME_LIST;
		ctlv->head.length = da->tcount * sizeof(ipfw_obj_ntlv) + 
		    sizeof(*ctlv);
		ctlv->count = da->tcount;
		ctlv->objsize = sizeof(ipfw_obj_ntlv);
	}

	i = 0;
	tcount = da->tcount;
	while (tcount > 0) {
		if ((bmask[i / 32] & (1 << (i % 32))) == 0) {
			i++;
			continue;
		}

		if ((error = ipfw_export_table_ntlv(chain, i, sd)) != 0)
			return (error);

		i++;
		tcount--;
	}

	/* Dump rules */
	ctlv = (ipfw_obj_ctlv *)ipfw_get_sopt_space(sd, sizeof(*ctlv));
	if (ctlv == NULL)
		return (ENOMEM);
	ctlv->head.type = IPFW_TLV_RULE_LIST;
	ctlv->head.length = da->rsize + sizeof(*ctlv);
	ctlv->count = da->rcount;

	for (i = da->b; i < da->e; i++) {
		krule = chain->map[i];

		l = RULEUSIZE1(krule) + sizeof(ipfw_obj_tlv);
		if (da->rcounters != 0)
			l += sizeof(struct ip_fw_bcounter);
		dst = (caddr_t)ipfw_get_sopt_space(sd, l);
		if (dst == NULL)
			return (ENOMEM);

		export_rule1(krule, dst, l, da->rcounters);
	}

	return (0);
}

/*
 * Dumps requested objects data
 * Data layout (version 0)(current):
 * Request: [ ipfw_cfg_lheader ] + IPFW_CFG_GET_* flags
 *   size = ipfw_cfg_lheader.size
 * Reply: [ ipfw_rules_lheader 
 *   [ ipfw_obj_ctlv(IPFW_TLV_TBL_LIST) ipfw_obj_ntlv x N ] (optional)
 *   [ ipfw_obj_ctlv(IPFW_TLV_RULE_LIST)
 *     ipfw_obj_tlv(IPFW_TLV_RULE_ENT) [ ip_fw_bcounter (optional) ip_fw_rule ]
 *   ] (optional)
 *   [ ipfw_obj_ctlv(IPFW_TLV_STATE_LIST) ipfw_obj_dyntlv x N ] (optional)
 * ]
 * * NOTE IPFW_TLV_STATE_LIST has the single valid field: objsize.
 * The rest (size, count) are set to zero and needs to be ignored.
 *
 * Returns 0 on success.
 */
static int
dump_config(struct ip_fw_chain *chain, struct sockopt_data *sd)
{
	ipfw_cfg_lheader *hdr;
	struct ip_fw *rule;
	size_t sz, rnum;
	uint32_t hdr_flags;
	int error, i;
	struct dump_args da;
	uint32_t *bmask;

	hdr = (ipfw_cfg_lheader *)ipfw_get_sopt_header(sd, sizeof(*hdr));
	if (hdr == NULL)
		return (EINVAL);

	error = 0;
	bmask = NULL;
	/* Allocate needed state */
	if (hdr->flags & IPFW_CFG_GET_STATIC)
		bmask = malloc(IPFW_TABLES_MAX / 8, M_TEMP, M_WAITOK | M_ZERO);

	IPFW_UH_RLOCK(chain);

	/*
	 * STAGE 1: Determine size/count for objects in range.
	 * Prepare used tables bitmask.
	 */
	sz = 0;
	memset(&da, 0, sizeof(da));

	da.b = 0;
	da.e = chain->n_rules;

	if (hdr->end_rule != 0) {
		/* Handle custom range */
		if ((rnum = hdr->start_rule) > IPFW_DEFAULT_RULE)
			rnum = IPFW_DEFAULT_RULE;
		da.b = ipfw_find_rule(chain, rnum, 0);
		rnum = hdr->end_rule;
		rnum = (rnum < IPFW_DEFAULT_RULE) ? rnum+1 : IPFW_DEFAULT_RULE;
		da.e = ipfw_find_rule(chain, rnum, 0);
	}

	if (hdr->flags & IPFW_CFG_GET_STATIC) {
		for (i = da.b; i < da.e; i++) {
			rule = chain->map[i];
			da.rsize += RULEUSIZE1(rule) + sizeof(ipfw_obj_tlv);
			da.rcount++;
			da.tcount += ipfw_mark_table_kidx(chain, rule, bmask);
		}
		/* Add counters if requested */
		if (hdr->flags & IPFW_CFG_GET_COUNTERS) {
			da.rsize += sizeof(struct ip_fw_bcounter) * da.rcount;
			da.rcounters = 1;
		}

		if (da.tcount > 0)
			sz += da.tcount * sizeof(ipfw_obj_ntlv) +
			    sizeof(ipfw_obj_ctlv);
		sz += da.rsize + sizeof(ipfw_obj_ctlv);
	}

	if (hdr->flags & IPFW_CFG_GET_STATES)
		sz += ipfw_dyn_get_count() * sizeof(ipfw_obj_dyntlv) +
		     sizeof(ipfw_obj_ctlv);


	/*
	 * Fill header anyway.
	 * Note we have to save header fields to stable storage
	 * buffer inside @sd can be flushed after dumping rules
	 */
	hdr->size = sz;
	hdr->set_mask = ~V_set_disable;
	hdr_flags = hdr->flags;
	hdr = NULL;

	if (sd->valsize < sz) {
		error = ENOMEM;
		goto cleanup;
	}

	/* STAGE2: Store actual data */
	if (hdr_flags & IPFW_CFG_GET_STATIC) {
		error = dump_static_rules(chain, &da, bmask, sd);
		if (error != 0)
			goto cleanup;
	}

	if (hdr_flags & IPFW_CFG_GET_STATES)
		error = ipfw_dump_states(chain, sd);

cleanup:
	IPFW_UH_RUNLOCK(chain);

	if (bmask != NULL)
		free(bmask, M_TEMP);

	return (error);
}

#define IP_FW3_OPLENGTH(x)	((x)->sopt_valsize - sizeof(ip_fw3_opheader))
#define	IP_FW3_WRITEBUF	4096			/* small page-size write buffer */
#define	IP_FW3_READBUF	16 * 1024 * 1024	/* handle large rulesets */


static int
check_object_name(ipfw_obj_ntlv *ntlv)
{
	int error;

	switch (ntlv->head.type) {
	case IPFW_TLV_TBL_NAME:
		error = ipfw_check_table_name(ntlv->name);
		break;
	default:
		error = ENOTSUP;
	}

	return (0);
}

/*
 * Adds one or more rules to ipfw @chain.
 * Data layout (version 0)(current):
 * Request:
 * [
 *   ip_fw3_opheader
 *   [ ipfw_obj_ctlv(IPFW_TLV_TBL_LIST) ipfw_obj_ntlv x N ] (optional *1)
 *   [ ipfw_obj_ctlv(IPFW_TLV_RULE_LIST) ip_fw x N ] (*2) (*3)
 * ]
 * Reply:
 * [
 *   ip_fw3_opheader
 *   [ ipfw_obj_ctlv(IPFW_TLV_TBL_LIST) ipfw_obj_ntlv x N ] (optional)
 *   [ ipfw_obj_ctlv(IPFW_TLV_RULE_LIST) ip_fw x N ]
 * ]
 *
 * Rules in reply are modified to store their actual ruleset number.
 *
 * (*1) TLVs inside IPFW_TLV_TBL_LIST needs to be sorted ascending
 * accoring to their idx field and there has to be no duplicates.
 * (*2) Numbered rules inside IPFW_TLV_RULE_LIST needs to be sorted ascending.
 * (*3) Each ip_fw structure needs to be aligned to u64 boundary.
 *
 * Returns 0 on success.
 */
static int
add_rules(struct ip_fw_chain *chain, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_obj_ctlv *ctlv, *rtlv, *tstate;
	ipfw_obj_ntlv *ntlv;
	int clen, error, idx;
	uint32_t count, read;
	struct ip_fw_rule *r;
	struct rule_check_info rci, *ci, *cbuf;
	int i, rsize;

	if (sd->valsize > IP_FW3_READBUF)
		return (EINVAL);

	op3 = (ip_fw3_opheader *)ipfw_get_sopt_space(sd, sd->valsize);
	ctlv = (ipfw_obj_ctlv *)(op3 + 1);

	read = sizeof(ip_fw3_opheader);
	rtlv = NULL;
	tstate = NULL;
	cbuf = NULL;
	memset(&rci, 0, sizeof(struct rule_check_info));

	if (read + sizeof(*ctlv) > sd->valsize)
		return (EINVAL);

	if (ctlv->head.type == IPFW_TLV_TBLNAME_LIST) {
		clen = ctlv->head.length;
		/* Check size and alignment */
		if (clen > sd->valsize || clen < sizeof(*ctlv))
			return (EINVAL);
		if ((clen % sizeof(uint64_t)) != 0)
			return (EINVAL);

		/*
		 * Some table names or other named objects.
		 * Check for validness.
		 */
		count = (ctlv->head.length - sizeof(*ctlv)) / sizeof(*ntlv);
		if (ctlv->count != count || ctlv->objsize != sizeof(*ntlv))
			return (EINVAL);

		/*
		 * Check each TLV.
		 * Ensure TLVs are sorted ascending and
		 * there are no duplicates.
		 */
		idx = -1;
		ntlv = (ipfw_obj_ntlv *)(ctlv + 1);
		while (count > 0) {
			if (ntlv->head.length != sizeof(ipfw_obj_ntlv))
				return (EINVAL);

			error = check_object_name(ntlv);
			if (error != 0)
				return (error);

			if (ntlv->idx <= idx)
				return (EINVAL);

			idx = ntlv->idx;
			count--;
			ntlv++;
		}

		tstate = ctlv;
		read += ctlv->head.length;
		ctlv = (ipfw_obj_ctlv *)((caddr_t)ctlv + ctlv->head.length);
	}

	if (read + sizeof(*ctlv) > sd->valsize)
		return (EINVAL);

	if (ctlv->head.type == IPFW_TLV_RULE_LIST) {
		clen = ctlv->head.length;
		if (clen + read > sd->valsize || clen < sizeof(*ctlv))
			return (EINVAL);
		if ((clen % sizeof(uint64_t)) != 0)
			return (EINVAL);

		/*
		 * TODO: Permit adding multiple rules at once
		 */
		if (ctlv->count != 1)
			return (ENOTSUP);

		clen -= sizeof(*ctlv);

		if (ctlv->count > clen / sizeof(struct ip_fw_rule))
			return (EINVAL);

		/* Allocate state for each rule or use stack */
		if (ctlv->count == 1) {
			memset(&rci, 0, sizeof(struct rule_check_info));
			cbuf = &rci;
		} else
			cbuf = malloc(ctlv->count * sizeof(*ci), M_TEMP,
			    M_WAITOK | M_ZERO);
		ci = cbuf;

		/*
		 * Check each rule for validness.
		 * Ensure numbered rules are sorted ascending
		 * and properly aligned
		 */
		idx = 0;
		r = (struct ip_fw_rule *)(ctlv + 1);
		count = 0;
		error = 0;
		while (clen > 0) {
			rsize = roundup2(RULESIZE(r), sizeof(uint64_t));
			if (rsize > clen || ctlv->count <= count) {
				error = EINVAL;
				break;
			}

			ci->ctlv = tstate;
			error = check_ipfw_rule1(r, rsize, ci);
			if (error != 0)
				break;

			/* Check sorting */
			if (r->rulenum != 0 && r->rulenum < idx) {
				printf("rulenum %d idx %d\n", r->rulenum, idx);
				error = EINVAL;
				break;
			}
			idx = r->rulenum;

			ci->urule = (caddr_t)r;

			rsize = roundup2(rsize, sizeof(uint64_t));
			clen -= rsize;
			r = (struct ip_fw_rule *)((caddr_t)r + rsize);
			count++;
			ci++;
		}

		if (ctlv->count != count || error != 0) {
			if (cbuf != &rci)
				free(cbuf, M_TEMP);
			return (EINVAL);
		}

		rtlv = ctlv;
		read += ctlv->head.length;
		ctlv = (ipfw_obj_ctlv *)((caddr_t)ctlv + ctlv->head.length);
	}

	if (read != sd->valsize || rtlv == NULL || rtlv->count == 0) {
		if (cbuf != NULL && cbuf != &rci)
			free(cbuf, M_TEMP);
		return (EINVAL);
	}

	/*
	 * Passed rules seems to be valid.
	 * Allocate storage and try to add them to chain.
	 */
	for (i = 0, ci = cbuf; i < rtlv->count; i++, ci++) {
		clen = RULEKSIZE1((struct ip_fw_rule *)ci->urule);
		ci->krule = ipfw_alloc_rule(chain, clen);
		import_rule1(ci);
	}

	if ((error = commit_rules(chain, cbuf, rtlv->count)) != 0) {
		/* Free allocate krules */
		for (i = 0, ci = cbuf; i < rtlv->count; i++, ci++)
			free(ci->krule, M_IPFW);
	}

	if (cbuf != NULL && cbuf != &rci)
		free(cbuf, M_TEMP);

	return (error);
}

/*
 * Writes data accumulated in @sd to sockopt buffer.
 * Zeroes internal @sd buffer.
 */
static int
ipfw_flush_sopt_data(struct sockopt_data *sd)
{
	int error;
	size_t sz;

	if ((sz = sd->koff) == 0)
		return (0);

	if (sd->sopt->sopt_dir == SOPT_GET) {
		error = sooptcopyout(sd->sopt, sd->kbuf, sz);
		if (error != 0)
			return (error);
	}

	memset(sd->kbuf, 0, sd->ksize);
	sd->ktotal += sd->koff;
	sd->koff = 0;
	if (sd->ktotal + sd->ksize < sd->valsize)
		sd->kavail = sd->ksize;
	else
		sd->kavail = sd->valsize - sd->ktotal;

	/* Update sopt buffer */
	sd->sopt->sopt_valsize = sd->kavail;
	sd->sopt->sopt_val = sd->sopt_val + sd->ktotal;

	return (0);
}

/*
 * Ensures that @sd buffer has contigious @neeeded number of
 * bytes.
 *
 * Returns pointer to requested space or NULL.
 */
caddr_t
ipfw_get_sopt_space(struct sockopt_data *sd, size_t needed)
{
	int error;
	caddr_t addr;

	if (sd->kavail < needed) {
		/*
		 * Flush data and try another time.
		 */
		error = ipfw_flush_sopt_data(sd);

		if (sd->kavail < needed || error != 0)
			return (NULL);
	}

	addr = sd->kbuf + sd->koff;
	sd->koff += needed;
	sd->kavail -= needed;
	return (addr);
}

/*
 * Requests @needed contigious bytes from @sd buffer.
 * Function is used to notify subsystem that we are
 * interesed in first @needed bytes (request header)
 * and the rest buffer can be safely zeroed.
 *
 * Returns pointer to requested space or NULL.
 */
caddr_t
ipfw_get_sopt_header(struct sockopt_data *sd, size_t needed)
{
	caddr_t addr;

	if ((addr = ipfw_get_sopt_space(sd, needed)) == NULL)
		return (NULL);

	if (sd->kavail > 0)
		memset(sd->kbuf + sd->koff, 0, sd->kavail);
	
	return (addr);
}

/*
 * New sockopt handler.
 */
int
ipfw_ctl3(struct sockopt *sopt)
{
	int error, ctype;
	size_t bsize_max, size, valsize;
	struct ip_fw_chain *chain;
	uint32_t opt;
	char xbuf[256];
	struct sockopt_data sdata;
	ip_fw3_opheader *op3 = NULL;

	error = priv_check(sopt->sopt_td, PRIV_NETINET_IPFW);
	if (error != 0)
		return (error);

	if (sopt->sopt_name != IP_FW3)
		return (ipfw_ctl(sopt));

	chain = &V_layer3_chain;
	error = 0;

	/* Save original valsize before it is altered via sooptcopyin() */
	valsize = sopt->sopt_valsize;
	memset(&sdata, 0, sizeof(sdata));
	/* Read op3 header first to determine actual operation */
	op3 = (ip_fw3_opheader *)xbuf;
	error = sooptcopyin(sopt, op3, sizeof(*op3), sizeof(*op3));
	if (error != 0)
		return (error);
	opt = op3->opcode;
	sopt->sopt_valsize = valsize;

	/*
	 * Determine opcode type/buffer size:
	 * use on-stack xbuf for short request,
	 * allocate sliding-window buf for data export or
	 * contigious buffer for special ops.
	 */
	ctype = (sopt->sopt_dir == SOPT_GET) ? SOPT_GET : SOPT_SET;
	switch (opt) {
	case IP_FW_XADD:
	case IP_FW_XDEL:
	case IP_FW_TABLE_XADD:
	case IP_FW_TABLE_XDEL:
		ctype = SOPT_SET;
		bsize_max = IP_FW3_READBUF;
		break;
	default:
		bsize_max = IP_FW3_WRITEBUF;
	}

	/*
	 * Disallow modifications in really-really secure mode, but still allow
	 * the logging counters to be reset.
	 */
	if (ctype == SOPT_SET && opt != IP_FW_XRESETLOG) {
		error = securelevel_ge(sopt->sopt_td->td_ucred, 3);
		if (error != 0)
			return (error);
	}

	/*
	 * Fill in sockopt_data structure that may be useful for
	 * IP_FW3 get requests.
	 */

	if (valsize <= sizeof(xbuf)) {
		sdata.kbuf = xbuf;
		sdata.ksize = sizeof(xbuf);
		sdata.kavail = valsize;
	} else {
		if (valsize < bsize_max)
			size = valsize;
		else
			size = bsize_max;

		sdata.kbuf = malloc(size, M_TEMP, M_WAITOK | M_ZERO);
		sdata.ksize = size;
		sdata.kavail = size;
	}

	sdata.sopt = sopt;
	sdata.sopt_val = sopt->sopt_val;
	sdata.valsize = valsize;

	/*
	 * Copy either all request (if valsize < bsize_max)
	 * or first bsize_max bytes to guarantee most consumers
	 * that all necessary data has been copied).
	 * Anyway, copy not less than sizeof(ip_fw3_opheader).
	 */
	if ((error = sooptcopyin(sopt, sdata.kbuf, sdata.ksize,
	    sizeof(ip_fw3_opheader))) != 0)
		return (error);
	op3 = (ip_fw3_opheader *)sdata.kbuf;
	opt = op3->opcode;

	switch (opt) {
	case IP_FW_XGET:
		error = dump_config(chain, &sdata);
		break;

	case IP_FW_XADD:
		error = add_rules(chain, op3, &sdata);
		break;

	case IP_FW_XDEL:
		error = del_rules(chain, op3, &sdata);
		break;

	case IP_FW_XZERO:
	case IP_FW_XRESETLOG:
		error = clear_rules(chain, op3, &sdata);
		break;

	case IP_FW_XMOVE:
		error = move_rules(chain, op3, &sdata);
		break;

	case IP_FW_SET_SWAP:
	case IP_FW_SET_MOVE:
	case IP_FW_SET_ENABLE:
		error = manage_sets(chain, op3, &sdata);
		break;

	case IP_FW_XIFLIST:
		error = ipfw_list_ifaces(chain, &sdata);
		break;

	/*--- TABLE opcodes ---*/
	case IP_FW_TABLE_XCREATE:
		error = ipfw_create_table(chain, op3, &sdata);
		break;

	case IP_FW_TABLE_XDESTROY:
	case IP_FW_TABLE_XFLUSH:
		error = ipfw_flush_table(chain, op3, &sdata);
		break;

	case IP_FW_TABLE_XMODIFY:
		error = ipfw_modify_table(chain, op3, &sdata);
		break;

	case IP_FW_TABLE_XINFO:
		error = ipfw_describe_table(chain, &sdata);
		break;

	case IP_FW_TABLES_XLIST:
		error = ipfw_list_tables(chain, &sdata);
		break;

	case IP_FW_TABLE_XLIST:
		error = ipfw_dump_table(chain, op3, &sdata);
		break;

	case IP_FW_TABLE_XADD:
	case IP_FW_TABLE_XDEL:
		error = ipfw_manage_table_ent(chain, op3, &sdata);
		break;

	case IP_FW_TABLE_XFIND:
		error = ipfw_find_table_entry(chain, op3, &sdata);
		break;

	case IP_FW_TABLE_XSWAP:
		error = ipfw_swap_table(chain, op3, &sdata);
		break;

	case IP_FW_TABLES_ALIST:
		error = ipfw_list_table_algo(chain, &sdata);
		break;

	case IP_FW_TABLE_VLIST:
		error = ipfw_list_table_values(chain, op3, &sdata);
		break;

	case IP_FW_TABLE_XGETSIZE:
		{
			uint32_t *tbl;
			struct tid_info ti;

			if (IP_FW3_OPLENGTH(sopt) < sizeof(uint32_t)) {
				error = EINVAL;
				break;
			}

			tbl = (uint32_t *)(op3 + 1);

			memset(&ti, 0, sizeof(ti));
			ti.uidx = *tbl;
			IPFW_UH_RLOCK(chain);
			error = ipfw_count_xtable(chain, &ti, tbl);
			IPFW_UH_RUNLOCK(chain);
			if (error)
				break;
			error = sooptcopyout(sopt, op3, sopt->sopt_valsize);
		}
		break;

	default:
		printf("ipfw: ipfw_ctl3 invalid option %d\n", opt);
		error = EINVAL;
	}

	/* Flush state and free buffers */
	if (error == 0)
		error = ipfw_flush_sopt_data(&sdata);
	else
		ipfw_flush_sopt_data(&sdata);

	/* Restore original pointer and set number of bytes written */
	sopt->sopt_val = sdata.sopt_val;
	sopt->sopt_valsize = sdata.ktotal;
	if (sdata.kbuf != xbuf)
		free(sdata.kbuf, M_TEMP);

	return (error);
}

/**
 * {set|get}sockopt parser.
 */
int
ipfw_ctl(struct sockopt *sopt)
{
#define	RULE_MAXSIZE	(256*sizeof(u_int32_t))
	int error;
	size_t size, valsize;
	struct ip_fw *buf;
	struct ip_fw_rule0 *rule;
	struct ip_fw_chain *chain;
	u_int32_t rulenum[2];
	uint32_t opt;
	struct rule_check_info ci;

	chain = &V_layer3_chain;
	error = 0;

	/* Save original valsize before it is altered via sooptcopyin() */
	valsize = sopt->sopt_valsize;
	opt = sopt->sopt_name;

	/*
	 * Disallow modifications in really-really secure mode, but still allow
	 * the logging counters to be reset.
	 */
	if (opt == IP_FW_ADD ||
	    (sopt->sopt_dir == SOPT_SET && opt != IP_FW_RESETLOG)) {
		error = securelevel_ge(sopt->sopt_td->td_ucred, 3);
		if (error != 0)
			return (error);
	}

	switch (opt) {
	case IP_FW_GET:
		/*
		 * pass up a copy of the current rules. Static rules
		 * come first (the last of which has number IPFW_DEFAULT_RULE),
		 * followed by a possibly empty list of dynamic rule.
		 * The last dynamic rule has NULL in the "next" field.
		 *
		 * Note that the calculated size is used to bound the
		 * amount of data returned to the user.  The rule set may
		 * change between calculating the size and returning the
		 * data in which case we'll just return what fits.
		 */
		for (;;) {
			int len = 0, want;

			size = chain->static_len;
			size += ipfw_dyn_len();
			if (size >= sopt->sopt_valsize)
				break;
			buf = malloc(size, M_TEMP, M_WAITOK | M_ZERO);
			IPFW_UH_RLOCK(chain);
			/* check again how much space we need */
			want = chain->static_len + ipfw_dyn_len();
			if (size >= want)
				len = ipfw_getrules(chain, buf, size);
			IPFW_UH_RUNLOCK(chain);
			if (size >= want)
				error = sooptcopyout(sopt, buf, len);
			free(buf, M_TEMP);
			if (size >= want)
				break;
		}
		break;

	case IP_FW_FLUSH:
		/* locking is done within del_entry() */
		error = del_entry(chain, 0); /* special case, rule=0, cmd=0 means all */
		break;

	case IP_FW_ADD:
		rule = malloc(RULE_MAXSIZE, M_TEMP, M_WAITOK);
		error = sooptcopyin(sopt, rule, RULE_MAXSIZE,
			sizeof(struct ip_fw7) );

		memset(&ci, 0, sizeof(struct rule_check_info));

		/*
		 * If the size of commands equals RULESIZE7 then we assume
		 * a FreeBSD7.2 binary is talking to us (set is7=1).
		 * is7 is persistent so the next 'ipfw list' command
		 * will use this format.
		 * NOTE: If wrong version is guessed (this can happen if
		 *       the first ipfw command is 'ipfw [pipe] list')
		 *       the ipfw binary may crash or loop infinitly...
		 */
		size = sopt->sopt_valsize;
		if (size == RULESIZE7(rule)) {
		    is7 = 1;
		    error = convert_rule_to_8(rule);
		    if (error) {
			free(rule, M_TEMP);
			return error;
		    }
		    size = RULESIZE(rule);
		} else
		    is7 = 0;
		if (error == 0)
			error = check_ipfw_rule0(rule, size, &ci);
		if (error == 0) {
			/* locking is done within add_rule() */
			struct ip_fw *krule;
			krule = ipfw_alloc_rule(chain, RULEKSIZE0(rule));
			ci.urule = (caddr_t)rule;
			ci.krule = krule;
			import_rule0(&ci);
			error = commit_rules(chain, &ci, 1);
			if (!error && sopt->sopt_dir == SOPT_GET) {
				if (is7) {
					error = convert_rule_to_7(rule);
					size = RULESIZE7(rule);
					if (error) {
						free(rule, M_TEMP);
						return error;
					}
				}
				error = sooptcopyout(sopt, rule, size);
			}
		}
		free(rule, M_TEMP);
		break;

	case IP_FW_DEL:
		/*
		 * IP_FW_DEL is used for deleting single rules or sets,
		 * and (ab)used to atomically manipulate sets. Argument size
		 * is used to distinguish between the two:
		 *    sizeof(u_int32_t)
		 *	delete single rule or set of rules,
		 *	or reassign rules (or sets) to a different set.
		 *    2*sizeof(u_int32_t)
		 *	atomic disable/enable sets.
		 *	first u_int32_t contains sets to be disabled,
		 *	second u_int32_t contains sets to be enabled.
		 */
		error = sooptcopyin(sopt, rulenum,
			2*sizeof(u_int32_t), sizeof(u_int32_t));
		if (error)
			break;
		size = sopt->sopt_valsize;
		if (size == sizeof(u_int32_t) && rulenum[0] != 0) {
			/* delete or reassign, locking done in del_entry() */
			error = del_entry(chain, rulenum[0]);
		} else if (size == 2*sizeof(u_int32_t)) { /* set enable/disable */
			IPFW_UH_WLOCK(chain);
			V_set_disable =
			    (V_set_disable | rulenum[0]) & ~rulenum[1] &
			    ~(1<<RESVD_SET); /* set RESVD_SET always enabled */
			IPFW_UH_WUNLOCK(chain);
		} else
			error = EINVAL;
		break;

	case IP_FW_ZERO:
	case IP_FW_RESETLOG: /* argument is an u_int_32, the rule number */
		rulenum[0] = 0;
		if (sopt->sopt_val != 0) {
		    error = sooptcopyin(sopt, rulenum,
			    sizeof(u_int32_t), sizeof(u_int32_t));
		    if (error)
			break;
		}
		error = zero_entry(chain, rulenum[0],
			sopt->sopt_name == IP_FW_RESETLOG);
		break;

	/*--- TABLE opcodes ---*/
	case IP_FW_TABLE_ADD:
	case IP_FW_TABLE_DEL:
		{
			ipfw_table_entry ent;
			struct tentry_info tei;
			struct tid_info ti;
			struct table_value v;

			error = sooptcopyin(sopt, &ent,
			    sizeof(ent), sizeof(ent));
			if (error)
				break;

			memset(&tei, 0, sizeof(tei));
			tei.paddr = &ent.addr;
			tei.subtype = AF_INET;
			tei.masklen = ent.masklen;
			ipfw_import_table_value_legacy(ent.value, &v);
			tei.pvalue = &v;
			memset(&ti, 0, sizeof(ti));
			ti.uidx = ent.tbl;
			ti.type = IPFW_TABLE_CIDR;

			error = (opt == IP_FW_TABLE_ADD) ?
			    add_table_entry(chain, &ti, &tei, 0, 1) :
			    del_table_entry(chain, &ti, &tei, 0, 1);
		}
		break;


	case IP_FW_TABLE_FLUSH:
		{
			u_int16_t tbl;
			struct tid_info ti;

			error = sooptcopyin(sopt, &tbl,
			    sizeof(tbl), sizeof(tbl));
			if (error)
				break;
			memset(&ti, 0, sizeof(ti));
			ti.uidx = tbl;
			error = flush_table(chain, &ti);
		}
		break;

	case IP_FW_TABLE_GETSIZE:
		{
			u_int32_t tbl, cnt;
			struct tid_info ti;

			if ((error = sooptcopyin(sopt, &tbl, sizeof(tbl),
			    sizeof(tbl))))
				break;
			memset(&ti, 0, sizeof(ti));
			ti.uidx = tbl;
			IPFW_RLOCK(chain);
			error = ipfw_count_table(chain, &ti, &cnt);
			IPFW_RUNLOCK(chain);
			if (error)
				break;
			error = sooptcopyout(sopt, &cnt, sizeof(cnt));
		}
		break;

	case IP_FW_TABLE_LIST:
		{
			ipfw_table *tbl;
			struct tid_info ti;

			if (sopt->sopt_valsize < sizeof(*tbl)) {
				error = EINVAL;
				break;
			}
			size = sopt->sopt_valsize;
			tbl = malloc(size, M_TEMP, M_WAITOK);
			error = sooptcopyin(sopt, tbl, size, sizeof(*tbl));
			if (error) {
				free(tbl, M_TEMP);
				break;
			}
			tbl->size = (size - sizeof(*tbl)) /
			    sizeof(ipfw_table_entry);
			memset(&ti, 0, sizeof(ti));
			ti.uidx = tbl->tbl;
			IPFW_RLOCK(chain);
			error = ipfw_dump_table_legacy(chain, &ti, tbl);
			IPFW_RUNLOCK(chain);
			if (error) {
				free(tbl, M_TEMP);
				break;
			}
			error = sooptcopyout(sopt, tbl, size);
			free(tbl, M_TEMP);
		}
		break;

	/*--- NAT operations are protected by the IPFW_LOCK ---*/
	case IP_FW_NAT_CFG:
		if (IPFW_NAT_LOADED)
			error = ipfw_nat_cfg_ptr(sopt);
		else {
			printf("IP_FW_NAT_CFG: %s\n",
			    "ipfw_nat not present, please load it");
			error = EINVAL;
		}
		break;

	case IP_FW_NAT_DEL:
		if (IPFW_NAT_LOADED)
			error = ipfw_nat_del_ptr(sopt);
		else {
			printf("IP_FW_NAT_DEL: %s\n",
			    "ipfw_nat not present, please load it");
			error = EINVAL;
		}
		break;

	case IP_FW_NAT_GET_CONFIG:
		if (IPFW_NAT_LOADED)
			error = ipfw_nat_get_cfg_ptr(sopt);
		else {
			printf("IP_FW_NAT_GET_CFG: %s\n",
			    "ipfw_nat not present, please load it");
			error = EINVAL;
		}
		break;

	case IP_FW_NAT_GET_LOG:
		if (IPFW_NAT_LOADED)
			error = ipfw_nat_get_log_ptr(sopt);
		else {
			printf("IP_FW_NAT_GET_LOG: %s\n",
			    "ipfw_nat not present, please load it");
			error = EINVAL;
		}
		break;

	default:
		printf("ipfw: ipfw_ctl invalid option %d\n", sopt->sopt_name);
		error = EINVAL;
	}

	return (error);
#undef RULE_MAXSIZE
}
#define	RULE_MAXSIZE	(256*sizeof(u_int32_t))

/* Functions to convert rules 7.2 <==> 8.0 */
static int
convert_rule_to_7(struct ip_fw_rule0 *rule)
{
	/* Used to modify original rule */
	struct ip_fw7 *rule7 = (struct ip_fw7 *)rule;
	/* copy of original rule, version 8 */
	struct ip_fw_rule0 *tmp;

	/* Used to copy commands */
	ipfw_insn *ccmd, *dst;
	int ll = 0, ccmdlen = 0;

	tmp = malloc(RULE_MAXSIZE, M_TEMP, M_NOWAIT | M_ZERO);
	if (tmp == NULL) {
		return 1; //XXX error
	}
	bcopy(rule, tmp, RULE_MAXSIZE);

	/* Copy fields */
	//rule7->_pad = tmp->_pad;
	rule7->set = tmp->set;
	rule7->rulenum = tmp->rulenum;
	rule7->cmd_len = tmp->cmd_len;
	rule7->act_ofs = tmp->act_ofs;
	rule7->next_rule = (struct ip_fw7 *)tmp->next_rule;
	rule7->cmd_len = tmp->cmd_len;
	rule7->pcnt = tmp->pcnt;
	rule7->bcnt = tmp->bcnt;
	rule7->timestamp = tmp->timestamp;

	/* Copy commands */
	for (ll = tmp->cmd_len, ccmd = tmp->cmd, dst = rule7->cmd ;
			ll > 0 ; ll -= ccmdlen, ccmd += ccmdlen, dst += ccmdlen) {
		ccmdlen = F_LEN(ccmd);

		bcopy(ccmd, dst, F_LEN(ccmd)*sizeof(uint32_t));

		if (dst->opcode > O_NAT)
			/* O_REASS doesn't exists in 7.2 version, so
			 * decrement opcode if it is after O_REASS
			 */
			dst->opcode--;

		if (ccmdlen > ll) {
			printf("ipfw: opcode %d size truncated\n",
				ccmd->opcode);
			return EINVAL;
		}
	}
	free(tmp, M_TEMP);

	return 0;
}

static int
convert_rule_to_8(struct ip_fw_rule0 *rule)
{
	/* Used to modify original rule */
	struct ip_fw7 *rule7 = (struct ip_fw7 *) rule;

	/* Used to copy commands */
	ipfw_insn *ccmd, *dst;
	int ll = 0, ccmdlen = 0;

	/* Copy of original rule */
	struct ip_fw7 *tmp = malloc(RULE_MAXSIZE, M_TEMP, M_NOWAIT | M_ZERO);
	if (tmp == NULL) {
		return 1; //XXX error
	}

	bcopy(rule7, tmp, RULE_MAXSIZE);

	for (ll = tmp->cmd_len, ccmd = tmp->cmd, dst = rule->cmd ;
			ll > 0 ; ll -= ccmdlen, ccmd += ccmdlen, dst += ccmdlen) {
		ccmdlen = F_LEN(ccmd);
		
		bcopy(ccmd, dst, F_LEN(ccmd)*sizeof(uint32_t));

		if (dst->opcode > O_NAT)
			/* O_REASS doesn't exists in 7.2 version, so
			 * increment opcode if it is after O_REASS
			 */
			dst->opcode++;

		if (ccmdlen > ll) {
			printf("ipfw: opcode %d size truncated\n",
			    ccmd->opcode);
			return EINVAL;
		}
	}

	rule->_pad = tmp->_pad;
	rule->set = tmp->set;
	rule->rulenum = tmp->rulenum;
	rule->cmd_len = tmp->cmd_len;
	rule->act_ofs = tmp->act_ofs;
	rule->next_rule = (struct ip_fw *)tmp->next_rule;
	rule->cmd_len = tmp->cmd_len;
	rule->id = 0; /* XXX see if is ok = 0 */
	rule->pcnt = tmp->pcnt;
	rule->bcnt = tmp->bcnt;
	rule->timestamp = tmp->timestamp;

	free (tmp, M_TEMP);
	return 0;
}

/*
 * Named object api
 *
 */

/*
 * Allocate new bitmask which can be used to enlarge/shrink
 * named instance index.
 */
void
ipfw_objhash_bitmap_alloc(uint32_t items, void **idx, int *pblocks)
{
	size_t size;
	int max_blocks;
	u_long *idx_mask;

	KASSERT((items % BLOCK_ITEMS) == 0,
	   ("bitmask size needs to power of 2 and greater or equal to %d",
	    BLOCK_ITEMS));

	max_blocks = items / BLOCK_ITEMS;
	size = items / 8;
	idx_mask = malloc(size * IPFW_MAX_SETS, M_IPFW, M_WAITOK);
	/* Mark all as free */
	memset(idx_mask, 0xFF, size * IPFW_MAX_SETS);
	*idx_mask &= ~(u_long)1; /* Skip index 0 */

	*idx = idx_mask;
	*pblocks = max_blocks;
}

/*
 * Copy current bitmask index to new one.
 */
void
ipfw_objhash_bitmap_merge(struct namedobj_instance *ni, void **idx, int *blocks)
{
	int old_blocks, new_blocks;
	u_long *old_idx, *new_idx;
	int i;

	old_idx = ni->idx_mask;
	old_blocks = ni->max_blocks;
	new_idx = *idx;
	new_blocks = *blocks;

	for (i = 0; i < IPFW_MAX_SETS; i++) {
		memcpy(&new_idx[new_blocks * i], &old_idx[old_blocks * i],
		    old_blocks * sizeof(u_long));
	}
}

/*
 * Swaps current @ni index with new one.
 */
void
ipfw_objhash_bitmap_swap(struct namedobj_instance *ni, void **idx, int *blocks)
{
	int old_blocks;
	u_long *old_idx;

	old_idx = ni->idx_mask;
	old_blocks = ni->max_blocks;

	ni->idx_mask = *idx;
	ni->max_blocks = *blocks;

	/* Save old values */
	*idx = old_idx;
	*blocks = old_blocks;
}

void
ipfw_objhash_bitmap_free(void *idx, int blocks)
{

	free(idx, M_IPFW);
}

/*
 * Creates named hash instance.
 * Must be called without holding any locks.
 * Return pointer to new instance.
 */
struct namedobj_instance *
ipfw_objhash_create(uint32_t items)
{
	struct namedobj_instance *ni;
	int i;
	size_t size;

	size = sizeof(struct namedobj_instance) +
	    sizeof(struct namedobjects_head) * NAMEDOBJ_HASH_SIZE +
	    sizeof(struct namedobjects_head) * NAMEDOBJ_HASH_SIZE;

	ni = malloc(size, M_IPFW, M_WAITOK | M_ZERO);
	ni->nn_size = NAMEDOBJ_HASH_SIZE;
	ni->nv_size = NAMEDOBJ_HASH_SIZE;

	ni->names = (struct namedobjects_head *)(ni +1);
	ni->values = &ni->names[ni->nn_size];

	for (i = 0; i < ni->nn_size; i++)
		TAILQ_INIT(&ni->names[i]);

	for (i = 0; i < ni->nv_size; i++)
		TAILQ_INIT(&ni->values[i]);

	/* Set default hashing/comparison functions */
	ni->hash_f = objhash_hash_name;
	ni->cmp_f = objhash_cmp_name;

	/* Allocate bitmask separately due to possible resize */
	ipfw_objhash_bitmap_alloc(items, (void*)&ni->idx_mask, &ni->max_blocks);

	return (ni);
}

void
ipfw_objhash_destroy(struct namedobj_instance *ni)
{

	free(ni->idx_mask, M_IPFW);
	free(ni, M_IPFW);
}

void
ipfw_objhash_set_funcs(struct namedobj_instance *ni, objhash_hash_f *hash_f,
    objhash_cmp_f *cmp_f)
{

	ni->hash_f = hash_f;
	ni->cmp_f = cmp_f;
}

static uint32_t
objhash_hash_name(struct namedobj_instance *ni, void *name, uint32_t set)
{

	return (fnv_32_str((char *)name, FNV1_32_INIT));
}

static int
objhash_cmp_name(struct named_object *no, void *name, uint32_t set)
{

	if ((strcmp(no->name, (char *)name) == 0) && (no->set == set))
		return (0);

	return (1);
}

static uint32_t
objhash_hash_idx(struct namedobj_instance *ni, uint32_t val)
{
	uint32_t v;

	v = val % (ni->nv_size - 1);

	return (v);
}

struct named_object *
ipfw_objhash_lookup_name(struct namedobj_instance *ni, uint32_t set, char *name)
{
	struct named_object *no;
	uint32_t hash;

	hash = ni->hash_f(ni, name, set) % ni->nn_size;
	
	TAILQ_FOREACH(no, &ni->names[hash], nn_next) {
		if (ni->cmp_f(no, name, set) == 0)
			return (no);
	}

	return (NULL);
}

struct named_object *
ipfw_objhash_lookup_kidx(struct namedobj_instance *ni, uint16_t kidx)
{
	struct named_object *no;
	uint32_t hash;

	hash = objhash_hash_idx(ni, kidx);
	
	TAILQ_FOREACH(no, &ni->values[hash], nv_next) {
		if (no->kidx == kidx)
			return (no);
	}

	return (NULL);
}

int
ipfw_objhash_same_name(struct namedobj_instance *ni, struct named_object *a,
    struct named_object *b)
{

	if ((strcmp(a->name, b->name) == 0) && a->set == b->set)
		return (1);

	return (0);
}

void
ipfw_objhash_add(struct namedobj_instance *ni, struct named_object *no)
{
	uint32_t hash;

	hash = ni->hash_f(ni, no->name, no->set) % ni->nn_size;
	TAILQ_INSERT_HEAD(&ni->names[hash], no, nn_next);

	hash = objhash_hash_idx(ni, no->kidx);
	TAILQ_INSERT_HEAD(&ni->values[hash], no, nv_next);

	ni->count++;
}

void
ipfw_objhash_del(struct namedobj_instance *ni, struct named_object *no)
{
	uint32_t hash;

	hash = ni->hash_f(ni, no->name, no->set) % ni->nn_size;
	TAILQ_REMOVE(&ni->names[hash], no, nn_next);

	hash = objhash_hash_idx(ni, no->kidx);
	TAILQ_REMOVE(&ni->values[hash], no, nv_next);

	ni->count--;
}

uint32_t
ipfw_objhash_count(struct namedobj_instance *ni)
{

	return (ni->count);
}

/*
 * Runs @func for each found named object.
 * It is safe to delete objects from callback
 */
void
ipfw_objhash_foreach(struct namedobj_instance *ni, objhash_cb_t *f, void *arg)
{
	struct named_object *no, *no_tmp;
	int i;

	for (i = 0; i < ni->nn_size; i++) {
		TAILQ_FOREACH_SAFE(no, &ni->names[i], nn_next, no_tmp)
			f(ni, no, arg);
	}
}

/*
 * Removes index from given set.
 * Returns 0 on success.
 */
int
ipfw_objhash_free_idx(struct namedobj_instance *ni, uint16_t idx)
{
	u_long *mask;
	int i, v;

	i = idx / BLOCK_ITEMS;
	v = idx % BLOCK_ITEMS;

	if (i >= ni->max_blocks)
		return (1);

	mask = &ni->idx_mask[i];

	if ((*mask & ((u_long)1 << v)) != 0)
		return (1);

	/* Mark as free */
	*mask |= (u_long)1 << v;

	/* Update free offset */
	if (ni->free_off[0] > i)
		ni->free_off[0] = i;
	
	return (0);
}

/*
 * Allocate new index in given instance and stores in in @pidx.
 * Returns 0 on success.
 */
int
ipfw_objhash_alloc_idx(void *n, uint16_t *pidx)
{
	struct namedobj_instance *ni;
	u_long *mask;
	int i, off, v;

	ni = (struct namedobj_instance *)n;

	off = ni->free_off[0];
	mask = &ni->idx_mask[off];

	for (i = off; i < ni->max_blocks; i++, mask++) {
		if ((v = ffsl(*mask)) == 0)
			continue;

		/* Mark as busy */
		*mask &= ~ ((u_long)1 << (v - 1));

		ni->free_off[0] = i;
		
		v = BLOCK_ITEMS * i + v - 1;

		*pidx = v;
		return (0);
	}

	return (1);
}

/* end of file */
