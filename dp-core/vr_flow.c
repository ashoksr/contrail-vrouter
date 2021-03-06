/*
 * vr_flow.c -- flow handling
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */
#include <vr_os.h>
#include "vr_sandesh.h"
#include "vr_message.h"
#include "vr_mcast.h"
#include "vr_btable.h"
#include "vr_fragment.h"

#define VR_NUM_FLOW_TABLES          1
#define VR_DEF_FLOW_ENTRIES         (512 * 1024)
#define VR_FLOW_TABLE_SIZE          (vr_flow_entries * \
        sizeof(struct vr_flow_entry))

#define VR_NUM_OFLOW_TABLES         1
#define VR_DEF_OFLOW_ENTRIES        (8 * 1024)
#define VR_OFLOW_TABLE_SIZE         (vr_oflow_entries *\
        sizeof(struct vr_flow_entry))

#define VR_FLOW_ENTRIES_PER_BUCKET  4U

#define VR_MAX_FLOW_QUEUE_ENTRIES   3U

#define VR_MAX_FLOW_TABLE_HOLD_COUNT \
                                    4096

unsigned int vr_flow_entries = VR_DEF_FLOW_ENTRIES;
unsigned int vr_oflow_entries = VR_DEF_OFLOW_ENTRIES;

#ifdef __KERNEL__
extern unsigned short vr_flow_major;
#endif

extern int vr_ip_input(struct vrouter *, unsigned short,
        struct vr_packet *, struct vr_forwarding_md *);
extern void vr_ip_update_csum(struct vr_packet *, unsigned int,
        unsigned int);

static void vr_flush_entry(struct vrouter *, struct vr_flow_entry *,
        struct vr_flow_md *, struct vr_forwarding_md *);

static void
vr_flow_reset_mirror(struct vrouter *router, struct vr_flow_entry *fe, 
                                                            unsigned int index)
{
    if (fe->fe_flags & VR_FLOW_FLAG_MIRROR) {
        vrouter_put_mirror(router, fe->fe_mirror_id);
        fe->fe_mirror_id = VR_MAX_MIRROR_INDICES;
        vrouter_put_mirror(router, fe->fe_sec_mirror_id);
        fe->fe_sec_mirror_id = VR_MAX_MIRROR_INDICES;
        vr_mirror_meta_entry_del(router, index);
    }
    fe->fe_flags &= ~VR_FLOW_FLAG_MIRROR;
    fe->fe_mirror_id = VR_MAX_MIRROR_INDICES;
    fe->fe_sec_mirror_id = VR_MAX_MIRROR_INDICES;

    return;
}

static void
vr_init_flow_entry(struct vr_flow_entry *fe)
{
    fe->fe_rflow = -1;
    fe->fe_mirror_id = VR_MAX_MIRROR_INDICES;
    fe->fe_sec_mirror_id = VR_MAX_MIRROR_INDICES;
    fe->fe_ecmp_nh_index = -1;

    return;
}


static void
vr_reset_flow_entry(struct vrouter *router, struct vr_flow_entry *fe,
        unsigned int index)
{
    memset(&fe->fe_stats, 0, sizeof(fe->fe_stats));
    memset(&fe->fe_hold_list, 0, sizeof(fe->fe_hold_list));;
    memset(&fe->fe_key, 0, sizeof(fe->fe_key));

    vr_flow_reset_mirror(router, fe, index);
    fe->fe_ecmp_nh_index = -1;
    fe->fe_src_nh_index = NH_DISCARD_ID;
    fe->fe_rflow = -1;
    fe->fe_action = VR_FLOW_ACTION_DROP;
    fe->fe_flags = 0;

    return;
}


static inline bool
vr_set_flow_active(struct vr_flow_entry *fe)
{
    return __sync_bool_compare_and_swap(&fe->fe_flags,
            fe->fe_flags & ~VR_FLOW_FLAG_ACTIVE, VR_FLOW_FLAG_ACTIVE);
}

static inline struct vr_flow_entry *
vr_flow_table_entry_get(struct vrouter *router, unsigned int i)
{
    return (struct vr_flow_entry *)vr_btable_get(router->vr_flow_table, i);
}

static inline struct vr_flow_entry *
vr_oflow_table_entry_get(struct vrouter *router, unsigned int i)
{
    return (struct vr_flow_entry *)vr_btable_get(router->vr_oflow_table, i);
}

unsigned int
vr_flow_table_size(struct vrouter *router)
{
    return vr_btable_size(router->vr_flow_table);
}

unsigned int
vr_oflow_table_size(struct vrouter *router)
{
    return vr_btable_size(router->vr_oflow_table);
}

/*
 * this is used by the mmap code. mmap sees the whole flow table
 * (including the overflow table) as one large table. so, given
 * an offset into that large memory, we should return the correct
 * virtual address
 */
void *
vr_flow_get_va(struct vrouter *router, uint64_t offset)
{
    struct vr_btable *table = router->vr_flow_table;
    unsigned int size = vr_flow_table_size(router);

    if (offset >= vr_flow_table_size(router)) {
        table = router->vr_oflow_table;
        offset -= size;
    }

    return vr_btable_get_address(table, offset);
}

static struct vr_flow_entry *
vr_get_flow_entry(struct vrouter *router, int index)
{
    struct vr_btable *table;

    if (index < 0)
        return NULL;

    if ((unsigned int)index < vr_flow_entries)
        table = router->vr_flow_table;
    else {
        table = router->vr_oflow_table;
        index -= vr_flow_entries;
        if ((unsigned int)index >= vr_oflow_entries)
            return NULL;
    }

    return (struct vr_flow_entry *)vr_btable_get(table, index);
}

static inline void
vr_get_flow_key(struct vr_flow_key *key, unsigned short vrf, struct vr_ip *ip,
        unsigned short sport, unsigned short dport)
{
    unsigned short *t_hdr;
    struct vr_icmp *icmph;

    /* copy both source and destinations */
    memcpy(&key->key_src_ip, &ip->ip_saddr, 2 * sizeof(ip->ip_saddr));
    key->key_proto = ip->ip_proto;
    key->key_zero = 0;
    key->key_vrf_id = vrf;

    /* extract port information */
    t_hdr = (unsigned short *)((char *)ip + (ip->ip_hl * 4));

    switch (ip->ip_proto) {
    case VR_IP_PROTO_TCP:
    case VR_IP_PROTO_UDP:
        key->key_src_port = sport;
        key->key_dst_port = dport;
        break;

    case VR_IP_PROTO_ICMP:
        icmph = (struct vr_icmp *)t_hdr;
        if (icmph->icmp_type == VR_ICMP_TYPE_ECHO ||
                icmph->icmp_type == VR_ICMP_TYPE_ECHO_REPLY) {
            key->key_src_port = icmph->icmp_eid;
            key->key_dst_port = VR_ICMP_TYPE_ECHO_REPLY;
        } else {
            key->key_src_port = 0;
            key->key_dst_port = icmph->icmp_type;
        }

        break;

    default:
        key->key_src_port = key->key_dst_port = 0;
        break;
    }

    return;
}

static struct vr_flow_entry *
vr_find_free_entry(struct vrouter *router, struct vr_flow_key *key,
        unsigned int *fe_index)
{
    unsigned int i, index, hash;
    struct vr_flow_entry *tmp_fe, *fe = NULL;

    *fe_index = 0;

    hash = vr_hash(key, sizeof(*key), 0);

    index = (hash % vr_flow_entries) & ~(VR_FLOW_ENTRIES_PER_BUCKET - 1);
    for (i = 0; i < VR_FLOW_ENTRIES_PER_BUCKET; i++) {
        tmp_fe = vr_flow_table_entry_get(router, index);
        if (tmp_fe && !(tmp_fe->fe_flags & VR_FLOW_FLAG_ACTIVE)) {
            if (vr_set_flow_active(tmp_fe)) {
                vr_init_flow_entry(tmp_fe);
                fe = tmp_fe;
                break;
            }
        }
        index++;
    }
        
    if (!fe) {
        index = hash % vr_oflow_entries;
        for (i = 0; i < vr_oflow_entries; i++) {
            tmp_fe = vr_oflow_table_entry_get(router, index);
            if (tmp_fe && !(tmp_fe->fe_flags & VR_FLOW_FLAG_ACTIVE)) {
                if (vr_set_flow_active(tmp_fe)) {
                    vr_init_flow_entry(tmp_fe);
                    fe = tmp_fe;
                    break;
                }
            }
            index = (index + 1) % vr_oflow_entries;
        }

        if (fe)
            *fe_index += vr_flow_entries;
    }

    if (fe) {
        *fe_index += index;
        memcpy(&fe->fe_key, key, sizeof(*key));
    }

    return fe;
}

static inline struct vr_flow_entry *
vr_flow_table_lookup(struct vr_flow_key *key, struct vr_btable *table,
                unsigned int table_size, unsigned int bucket_size,
                unsigned int hash, unsigned int *fe_index)
{
    unsigned int i;
    struct vr_flow_entry *flow_e;

    hash %= table_size;

    if (!bucket_size) {
        bucket_size = table_size;
    } else {
        hash &= ~(bucket_size - 1);
    }

    for (i = 0; i < bucket_size; i++) {
        flow_e = (struct vr_flow_entry *)vr_btable_get(table,
                (hash + i) % table_size);
        if (flow_e && flow_e->fe_flags & VR_FLOW_FLAG_ACTIVE) {
            if (!memcmp(&flow_e->fe_key, key, sizeof(*key))) {
                *fe_index = (hash + i) % table_size;
                return flow_e;
            }
        }
    }

    return NULL;
}


struct vr_flow_entry *
vr_find_flow(struct vrouter *router, struct vr_flow_key *key,
        unsigned int *fe_index)
{
    unsigned int hash;
    struct vr_flow_entry *flow_e;

    hash = vr_hash(key, sizeof(*key), 0);

    /* first look in the regular flow table */
    flow_e = vr_flow_table_lookup(key, router->vr_flow_table, vr_flow_entries,
                    VR_FLOW_ENTRIES_PER_BUCKET, hash, fe_index);
    /* if not in the regular flow table, lookup in the overflow flow table */
    if (!flow_e) {
        flow_e = vr_flow_table_lookup(key, router->vr_oflow_table, vr_oflow_entries,
                        0, hash, fe_index);
        *fe_index += vr_flow_entries;
    }

    return flow_e;
}

static inline bool
vr_flow_queue_is_empty(struct vrouter *router, struct vr_flow_entry *fe)
{
    if (fe->fe_hold_list.node_p)
        return false;
    return true;
}


static int
vr_enqueue_flow(struct vr_flow_entry *fe, struct vr_packet *pkt,
        unsigned short proto, struct vr_forwarding_md *fmd)
{
    unsigned int i = 0;
    unsigned short drop_reason = 0;
    struct vr_list_node **head = &fe->fe_hold_list.node_p;
    struct vr_packet_node *pnode;

    while (*head && ++i) {
        head = &(*head)->node_n;
    }

    if (i >= VR_MAX_FLOW_QUEUE_ENTRIES) {
        drop_reason = VP_DROP_FLOW_QUEUE_LIMIT_EXCEEDED;
        goto drop;
    }

    pnode = (struct vr_packet_node *)vr_zalloc(sizeof(struct vr_packet_node));
    if (!pnode) {
        drop_reason = VP_DROP_FLOW_NO_MEMORY;
        goto drop;
    }

    pnode->pl_packet = pkt;
    pnode->pl_proto = proto;
    if (fmd)
        pnode->pl_outer_src_ip = fmd->fmd_outer_src_ip;
    *head = &pnode->pl_node;

    return 0;

drop:
    vr_pfree(pkt, drop_reason);
    return 0;
}

static int
vr_flow_forward(unsigned short vrf, struct vr_packet *pkt,
        unsigned short proto, struct vr_forwarding_md *fmd)
{
    struct vr_interface *vif = pkt->vp_if;
    struct vrouter *router = vif->vif_router;

    if (proto != VR_ETH_PROTO_IP) {
        vr_pfree(pkt, VP_DROP_FLOW_INVALID_PROTOCOL);
        return 0;
    }

    if (pkt->vp_nh)
        return nh_output(vrf, pkt, pkt->vp_nh, fmd);

    pkt_set_data(pkt, pkt->vp_network_h);
    return vr_ip_input(router, vrf, pkt, fmd);
}

static int
vr_flow_nat(unsigned short vrf, struct vr_flow_entry *fe, struct vr_packet *pkt,
        unsigned short proto, struct vr_forwarding_md *fmd)
{
    unsigned int ip_inc, inc = 0; 
    unsigned short *t_sport, *t_dport;
    struct vrouter *router = pkt->vp_if->vif_router;
    struct vr_flow_entry *rfe;
    struct vr_ip *ip;

    if (fe->fe_rflow < 0)
        goto drop;

    rfe = vr_get_flow_entry(router, fe->fe_rflow);
    if (!rfe)
        goto drop;

    ip = (struct vr_ip *)pkt_data(pkt);

    if (fe->fe_flags & VR_FLOW_FLAG_SNAT) {
        vr_incremental_diff(ip->ip_saddr, rfe->fe_key.key_dest_ip, &inc);
        ip->ip_saddr = rfe->fe_key.key_dest_ip;
    }

    if (fe->fe_flags & VR_FLOW_FLAG_DNAT) {
        vr_incremental_diff(ip->ip_daddr, rfe->fe_key.key_src_ip, &inc);
        ip->ip_daddr = rfe->fe_key.key_src_ip;
    }

    ip_inc = inc;

    if (vr_ip_transport_header_valid(ip)) {
        t_sport = (unsigned short *)((unsigned char *)ip +
                (ip->ip_hl * 4));
        t_dport = t_sport + 1;

        if (fe->fe_flags & VR_FLOW_FLAG_SPAT) {
            vr_incremental_diff(*t_sport, rfe->fe_key.key_dst_port, &inc);
            *t_sport = rfe->fe_key.key_dst_port;
        }

        if (fe->fe_flags & VR_FLOW_FLAG_DPAT) {
            vr_incremental_diff(*t_dport, rfe->fe_key.key_src_port, &inc);
            *t_dport = rfe->fe_key.key_src_port;
        }
    }

    if (ip->ip_csum != VR_DIAG_IP_CSUM)
        vr_ip_update_csum(pkt, ip_inc, inc);

    return vr_flow_forward(vrf, pkt, proto, fmd);

drop:
    vr_pfree(pkt, VP_DROP_FLOW_NAT_NO_RFLOW);
    return 0;
}

static void
vr_flow_set_forwarding_md(struct vrouter *router, struct vr_flow_entry *fe,
        unsigned int index, struct vr_forwarding_md *md)
{
    struct vr_flow_entry *rfe;

    md->fmd_flow_index = index;
    md->fmd_ecmp_nh_index = fe->fe_ecmp_nh_index;
    if (fe->fe_flags & VR_RFLOW_VALID) {
        rfe = vr_get_flow_entry(router, fe->fe_rflow);
        if (rfe)
            md->fmd_ecmp_src_nh_index = rfe->fe_ecmp_nh_index;
    }

    return;
}

static int
vr_flow_action(struct vrouter *router, struct vr_flow_entry *fe, 
        unsigned int index, struct vr_packet *pkt,
        unsigned short proto, struct vr_forwarding_md *fmd)
{
    int ret = 0, valid_src;
    unsigned short vrf;
    struct vr_forwarding_md mirror_fmd;
    struct vr_nexthop *src_nh;

    vrf = fe->fe_key.key_vrf_id;
    if (fe->fe_flags & VR_FLOW_FLAG_VRFT)
        vrf = fe->fe_dvrf;

    vr_flow_set_forwarding_md(router, fe, index, fmd);
    src_nh = __vrouter_get_nexthop(router, fe->fe_src_nh_index);
    if (!src_nh) {
        vr_pfree(pkt, VP_DROP_INVALID_NH);
        return 0;
    }

    if (src_nh->nh_validate_src) {
        valid_src = src_nh->nh_validate_src(vrf, pkt, src_nh, fmd);
        if (valid_src == NH_SOURCE_INVALID) {
            vr_pfree(pkt, VP_DROP_INVALID_SOURCE);
            return 0;
        }

#if 0
        if (valid_src == NH_SOURCE_MISMATCH)
            return vr_trap(pkt, vrf,
                    AGENT_TRAP_SOURCE_MISMATCH, &fmd->fmd_flow_index);
#else
        if (valid_src == NH_SOURCE_MISMATCH)
            return vr_trap(pkt, vrf,
                    AGENT_TRAP_ECMP_RESOLVE, &fmd->fmd_flow_index);
#endif
    }


    if (fe->fe_flags & VR_FLOW_FLAG_MIRROR) {
        if (fe->fe_mirror_id < VR_MAX_MIRROR_INDICES) {
            mirror_fmd = *fmd;
            mirror_fmd.fmd_ecmp_nh_index = -1;
            vr_mirror(router, fe->fe_mirror_id, pkt, &mirror_fmd);
        }
        if (fe->fe_sec_mirror_id < VR_MAX_MIRROR_INDICES) {
            mirror_fmd = *fmd;
            mirror_fmd.fmd_ecmp_nh_index = -1;
            vr_mirror(router, fe->fe_sec_mirror_id, pkt, &mirror_fmd);
        }
    }

    switch (fe->fe_action) {
    case VR_FLOW_ACTION_DROP:
        vr_pfree(pkt, VP_DROP_FLOW_ACTION_DROP);
        break;

    case VR_FLOW_ACTION_FORWARD:
        ret = vr_flow_forward(vrf, pkt, proto, fmd);
        break;

    case VR_FLOW_ACTION_NAT:
        ret = vr_flow_nat(vrf, fe, pkt, proto, fmd);
        break;

    default:
        vr_pfree(pkt, VP_DROP_FLOW_ACTION_INVALID);
        break;
    }

    return ret;
}


unsigned int
vr_trap_flow(struct vrouter *router, struct vr_flow_entry *fe,
        struct vr_packet *pkt, unsigned int index)
{
    unsigned int trap_reason;
    struct vr_packet *npkt;

    npkt = vr_pclone(pkt);
    if (!npkt)
        return -ENOMEM;

    vr_preset(npkt);

    switch (fe->fe_flags & VR_FLOW_FLAG_TRAP_MASK) {
    case VR_FLOW_FLAG_TRAP_ECMP:
        trap_reason = AGENT_TRAP_ECMP_RESOLVE;
        break;

    default:
        trap_reason = AGENT_TRAP_FLOW_MISS;
        break;
    }


    return vr_trap(npkt, fe->fe_key.key_vrf_id, trap_reason, &index);
}

static int
vr_do_flow_action(struct vrouter *router, struct vr_flow_entry *fe,
        unsigned int index, struct vr_packet *pkt,
        unsigned short proto, struct vr_forwarding_md *fmd)
{
    uint32_t new_stats;

    new_stats = __sync_add_and_fetch(&fe->fe_stats.flow_bytes, pkt_len(pkt));
    if (new_stats < pkt_len(pkt))
        fe->fe_stats.flow_bytes_oflow++;

    new_stats = __sync_add_and_fetch(&fe->fe_stats.flow_packets, 1);
    if (!new_stats) 
        fe->fe_stats.flow_packets_oflow++;

    if (fe->fe_action == VR_FLOW_ACTION_HOLD) {
        if (vr_flow_queue_is_empty(router, fe)) {
            vr_trap_flow(router, fe, pkt, index);
            return vr_enqueue_flow(fe, pkt, proto, fmd);
        } else {
            vr_pfree(pkt, VP_DROP_FLOW_UNUSABLE);
            return 0;
        }
    }

    return vr_flow_action(router, fe, index, pkt, proto, fmd);
}

static unsigned int
vr_flow_table_hold_count(struct vrouter *router)
{
    unsigned int i, num_cpus;
    uint64_t hcount = 0, act_count;
    struct vr_flow_table_info *infop = router->vr_flow_table_info;

    num_cpus = vr_num_cpus;
    for (i = 0; i < num_cpus; i++)
        hcount += infop->vfti_hold_count[i];

    act_count = infop->vfti_action_count;
    if (hcount >= act_count)
        return hcount - act_count;

    return 0;
}

static void
vr_flow_entry_set_hold(struct vrouter *router, struct vr_flow_entry *flow_e)
{
    unsigned int cpu;
    uint64_t act_count;
    struct vr_flow_table_info *infop = router->vr_flow_table_info;

    cpu = vr_get_cpu();
    flow_e->fe_action = VR_FLOW_ACTION_HOLD;

    if (infop->vfti_hold_count[cpu] + 1 < infop->vfti_hold_count[cpu]) {
        act_count = infop->vfti_action_count;
        if (act_count > infop->vfti_hold_count[cpu]) {
           (void)__sync_sub_and_fetch(&infop->vfti_action_count,
                    infop->vfti_hold_count[cpu]);
            infop->vfti_hold_count[cpu] = 0;
        } else {
            infop->vfti_hold_count[cpu] -= act_count;
            (void)__sync_sub_and_fetch(&infop->vfti_action_count,
                    act_count);
        }
    }

    infop->vfti_hold_count[cpu]++;

    return;
}

static int
vr_flow_lookup(struct vrouter *router, struct vr_flow_key *key,
        struct vr_packet *pkt, unsigned short proto,
        struct vr_forwarding_md *fmd)
{
    unsigned int fe_index;
    struct vr_flow_entry *flow_e;

    pkt->vp_flags |= VP_FLAG_FLOW_SET;

    flow_e = vr_find_flow(router, key, &fe_index);
    if (!flow_e) {
        if (vr_flow_table_hold_count(router) > VR_MAX_FLOW_TABLE_HOLD_COUNT) {
            vr_pfree(pkt, VP_DROP_FLOW_UNUSABLE);
            return 0;
        }

        flow_e = vr_find_free_entry(router, key, &fe_index);
        if (!flow_e) {
            vr_pfree(pkt, VP_DROP_FLOW_TABLE_FULL);
            return 0;
        }

        /* mark as hold */
        vr_flow_entry_set_hold(router, flow_e);
        vr_do_flow_action(router, flow_e, fe_index, pkt, proto, fmd);
        return 0;
    } 
    

    return vr_do_flow_action(router, flow_e, fe_index, pkt, proto, fmd);
}

/*
 * This inline function decides whether to trap the packet, or bypass 
 * flow table or not. 
 */
inline unsigned int
vr_flow_parse(struct vrouter *router, struct vr_flow_key *key,
        struct vr_packet *pkt, unsigned int *trap_res)
{
   unsigned int proto_port;
   /* without any data, the result has to be BYPASS, right? */
   unsigned int res = VR_FLOW_BYPASS;

    /* 
     * if the packet has already done one round of flow lookup, there
     * is no point in doing it again, eh?
     */
    if (pkt->vp_flags & VP_FLAG_FLOW_SET)
        return res;

    /*
     * if the interface is policy enabled, or if somebody else (eg:nexthop)
     * has requested for a policy lookup, packet has to go through a lookup
     */
    if (pkt->vp_if->vif_flags & VIF_FLAG_POLICY_ENABLED ||
            pkt->vp_flags & VP_FLAG_FLOW_GET)
        res = VR_FLOW_LOOKUP;

    /*
     * ..., but then there are some exceptions, as checked below.
     * please note that these conditions also need to work when policy is
     * really not enabled
     */
    if (key) {
        if (IS_BMCAST_IP(key->key_dest_ip)) {
           /* no flow lookup for multicast or broadcast ip */
           res = VR_FLOW_BYPASS;
           pkt->vp_flags |= VP_FLAG_MULTICAST | VP_FLAG_FLOW_SET;
        }

        proto_port = (key->key_proto << VR_FLOW_PROTO_SHIFT) |
                                                key->key_dst_port;
        if (proto_port == VR_UDP_DHCP_SPORT ||
                proto_port == VR_UDP_DHCP_CPORT) {
            res = VR_FLOW_TRAP;
            pkt->vp_flags |= VP_FLAG_FLOW_SET;
            if (trap_res)
                *trap_res = AGENT_TRAP_L3_PROTOCOLS;
        }
    }

    return res;
}

unsigned int
vr_flow_inet_input(struct vrouter *router, unsigned short vrf,
        struct vr_packet *pkt, unsigned short proto,
        struct vr_forwarding_md *fmd)
{
    struct vr_flow_key key, *key_p = &key;
    struct vr_ip *ip;
    struct vr_fragment *frag;
    unsigned int flow_parse_res;
    unsigned int trap_res  = 0;
    unsigned short *t_hdr, sport, dport;

    /*
     * interface is in a mode where it wants all packets to be received
     * without doing lookups to figure out whether packets were destined
     * to me or not
     */
    if (pkt->vp_flags & VP_FLAG_TO_ME)
        return vr_ip_rcv(router, pkt, fmd);

    ip = (struct vr_ip *)pkt_network_header(pkt);
    /* if the packet is not a fragment, we easily know the sport, and dport */
    if (vr_ip_transport_header_valid(ip)) {
        t_hdr = (unsigned short *)((char *)ip + (ip->ip_hl * 4));
        sport = *t_hdr;
        dport = *(t_hdr + 1);
    } else {
        /* ...else, we need to get it from somewhere */
        flow_parse_res = vr_flow_parse(router, NULL, pkt, &trap_res);
        /* ...and it really matters only if we need to do a flow lookup */
        if (flow_parse_res == VR_FLOW_LOOKUP) {
            frag = vr_fragment_get(router, vrf, ip);
            if (!frag) {
                vr_pfree(pkt, VP_DROP_FRAGMENTS);
                return 0;
            }
            sport = frag->f_sport;
            dport = frag->f_dport;
            if (vr_ip_fragment_tail(ip))
                vr_fragment_del(frag);
        } else {
            /* 
             * since there is no other way of deriving a key, set the
             * key_p to NULL, indicating to code below that there is
             * indeed no need for flow lookup
             */
            key_p = NULL;
        }
    }

    if (key_p) {
        /* we have everything to make a key */
        vr_get_flow_key(key_p, vrf, ip, sport, dport);
        flow_parse_res = vr_flow_parse(router, key_p, pkt, &trap_res);
        if (flow_parse_res == VR_FLOW_LOOKUP && vr_ip_fragment_head(ip))
            vr_fragment_add(router, vrf, ip, key_p->key_src_port,
                    key_p->key_dst_port);

        if (flow_parse_res == VR_FLOW_BYPASS) {
            return vr_flow_forward(vrf, pkt, proto, fmd);
        } else if (flow_parse_res == VR_FLOW_TRAP) {
            return vr_trap(pkt, vrf, trap_res, NULL);
        }

        return vr_flow_lookup(router, key_p, pkt, proto, fmd);
    }

    /* 
     * ...come here, when there is not enough information to do a
     * flow lookup
     */
    return vr_flow_forward(vrf, pkt, proto, fmd);
}

static void
vr_flush_entry(struct vrouter *router, struct vr_flow_entry *fe,
        struct vr_flow_md *flmd, struct vr_forwarding_md *fmd)
{
    struct vr_list_node *head;
    struct vr_packet_node *pnode;

    head = fe->fe_hold_list.node_p;
    fe->fe_hold_list.node_p = NULL;

    while (head) {
        pnode = (struct vr_packet_node *)head;
        if (fmd)
            fmd->fmd_outer_src_ip = pnode->pl_outer_src_ip;

        vr_flow_action(router, fe, flmd->flmd_index, pnode->pl_packet,
                pnode->pl_proto, fmd);

        head = pnode->pl_node.node_n;
        vr_free(pnode);
    }

    return;
}

static void
vr_flow_flush(void *arg)
{
    struct vrouter *router;
    struct vr_flow_entry *fe;
    struct vr_forwarding_md fmd;
    struct vr_flow_md *flmd = 
                (struct vr_flow_md *)arg;

    router = flmd->flmd_router;
    if (!router)
        return;

    fe = vr_get_flow_entry(router, flmd->flmd_index);
    if (!fe)
        return;

    vr_init_forwarding_md(&fmd);
    vr_flow_set_forwarding_md(router, fe, flmd->flmd_index, &fmd);

    vr_flush_entry(router, fe, flmd, &fmd);

    if (!(flmd->flmd_flags & VR_FLOW_FLAG_ACTIVE)) {
        vr_reset_flow_entry(router, fe, flmd->flmd_index);
    } 

    return;
}

static void
vr_flow_set_mirror(struct vrouter *router, vr_flow_req *req,
        struct vr_flow_entry *fe)
{
    struct vr_mirror_entry *mirror = NULL, *sec_mirror = NULL;

    if (!(req->fr_flags & VR_FLOW_FLAG_MIRROR) &&
            (fe->fe_flags & VR_FLOW_FLAG_MIRROR)) {
    	vr_flow_reset_mirror(router, fe, req->fr_index);
        return;
    }

    if (!(req->fr_flags & VR_FLOW_FLAG_MIRROR))
        return;

    if (fe->fe_mirror_id != req->fr_mir_id) {
        if (fe->fe_mirror_id < router->vr_max_mirror_indices) {
            vrouter_put_mirror(router, fe->fe_mirror_id);
            fe->fe_mirror_id = router->vr_max_mirror_indices;
        }

        if ((unsigned int)req->fr_mir_id < router->vr_max_mirror_indices) {
            mirror = vrouter_get_mirror(req->fr_rid, req->fr_mir_id);
            if (mirror)
                fe->fe_mirror_id = req->fr_mir_id;

            /* when we reached this point, we had already done all the
             * sanity checks we could do. failing here will add only
             * complexity to code here. so !mirror case, we will not
             * handle
             */
        }
    }

    if (fe->fe_sec_mirror_id != req->fr_sec_mir_id) {
        if (fe->fe_sec_mirror_id < router->vr_max_mirror_indices) {
            vrouter_put_mirror(router, fe->fe_sec_mirror_id);
            fe->fe_sec_mirror_id = router->vr_max_mirror_indices;
        }

        if ((unsigned int)req->fr_sec_mir_id < router->vr_max_mirror_indices) {
            sec_mirror = vrouter_get_mirror(req->fr_rid, req->fr_sec_mir_id);
            if (sec_mirror)
                fe->fe_sec_mirror_id = req->fr_sec_mir_id;
        }
    }

    if (req->fr_pcap_meta_data_size && req->fr_pcap_meta_data)
        vr_mirror_meta_entry_set(router, req->fr_index,
                req->fr_mir_sip, req->fr_mir_sport,
                req->fr_pcap_meta_data, req->fr_pcap_meta_data_size,
                req->fr_mir_vrf);

    return;
}

static struct vr_flow_entry *
vr_add_flow(unsigned int rid, struct vr_flow_key *key,
        unsigned int *fe_index)
{
    struct vr_flow_entry *flow_e;
    struct vrouter *router = vrouter_get(rid);

    flow_e = vr_find_flow(router, key, fe_index);
    if (!flow_e)
        flow_e = vr_find_free_entry(router, key, fe_index);

    return flow_e;
}

static struct vr_flow_entry *
vr_add_flow_req(vr_flow_req *req, unsigned int *fe_index)
{
    struct vr_flow_key key;
    struct vr_flow_entry *fe;

    key.key_src_port = req->fr_flow_sport;
    key.key_dst_port = req->fr_flow_dport;
    key.key_src_ip = req->fr_flow_sip;
    key.key_dest_ip = req->fr_flow_dip;
    key.key_vrf_id = req->fr_flow_vrf;
    key.key_proto = req->fr_flow_proto;
    key.key_zero = 0;

    fe = vr_add_flow(req->fr_rid, &key, fe_index);
    if (fe)
        req->fr_index = *fe_index;

    return fe;
}

/*
 * can be called with 'fe' as null (specifically when flow is added from
 * agent), in which case we should be checking only the request
 */
static int
vr_flow_req_is_invalid(struct vrouter *router, vr_flow_req *req,
        struct vr_flow_entry *fe)
{
    struct vr_flow_entry *rfe;

    if (fe) {
        if ((unsigned int)req->fr_flow_sip != fe->fe_key.key_src_ip ||
                (unsigned int)req->fr_flow_dip != fe->fe_key.key_dest_ip ||
                (unsigned short)req->fr_flow_sport != fe->fe_key.key_src_port ||
                (unsigned short)req->fr_flow_dport != fe->fe_key.key_dst_port||
                (unsigned short)req->fr_flow_vrf != fe->fe_key.key_vrf_id ||
                (unsigned char)req->fr_flow_proto != fe->fe_key.key_proto) {
            return -EBADF;
        }
    }

    if (req->fr_flags & VR_FLOW_FLAG_VRFT) {
        if ((unsigned short)req->fr_flow_dvrf >= VR_MAX_VRFS)
            return -EINVAL;
    }

    if (req->fr_flags & VR_FLOW_FLAG_MIRROR) {
        if (((unsigned int)req->fr_mir_id >= router->vr_max_mirror_indices) &&
                (unsigned int)req->fr_sec_mir_id >= router->vr_max_mirror_indices)
            return -EINVAL;
    }

    if (req->fr_flags & VR_RFLOW_VALID) {
        rfe = vr_get_flow_entry(router, req->fr_rindex);
        if (!rfe)
            return -EINVAL;
    }

    /* 
     * for delete, we need not validate nh_index from incoming request
     */
    if (req->fr_flags & VR_FLOW_FLAG_ACTIVE) {
        if (!__vrouter_get_nexthop(router, req->fr_src_nh_index))
            return -EINVAL;
    }

    return 0;
}

static int
vr_flow_schedule_transition(struct vrouter *router, vr_flow_req *req,
        struct vr_flow_entry *fe)
{
    struct vr_flow_md *flmd = NULL;

    flmd = (struct vr_flow_md *)vr_malloc(sizeof(*flmd));
    if (!flmd)
        return -ENOMEM;

    flmd->flmd_router = router;
    flmd->flmd_index = req->fr_index;
    flmd->flmd_action = req->fr_action;
    flmd->flmd_flags = req->fr_flags;

    vr_schedule_work(vr_get_cpu(), vr_flow_flush, (void *)flmd);
    return 0;
}

static int
vr_flow_delete(struct vrouter *router, vr_flow_req *req,
        struct vr_flow_entry *fe)
{
    fe->fe_action = VR_FLOW_ACTION_DROP;
    vr_flow_reset_mirror(router, fe, req->fr_index);

    return vr_flow_schedule_transition(router, req, fe);
}


/* command from agent */
static int
vr_flow_set(struct vrouter *router, vr_flow_req *req)
{
    int ret;
    unsigned int fe_index;
    struct vr_flow_entry *fe = NULL;
    struct vr_flow_table_info *infop = router->vr_flow_table_info;

    router = vrouter_get(req->fr_rid);
    if (!router)
        return -EINVAL;

    fe = vr_get_flow_entry(router, req->fr_index);

    if ((ret = vr_flow_req_is_invalid(router, req, fe)))
        return ret;

    if (fe && (fe->fe_action == VR_FLOW_ACTION_HOLD) &&
            ((req->fr_action != fe->fe_action) ||
             !(req->fr_flags & VR_FLOW_FLAG_ACTIVE)))
        __sync_fetch_and_add(&infop->vfti_action_count, 1);
    /* 
     * for delete, absence of the requested flow entry is caustic. so
     * handle that case first
     */
    if (!(req->fr_flags & VR_FLOW_FLAG_ACTIVE)) {
        if (!fe)
            return -EINVAL;
        return vr_flow_delete(router, req, fe);
    }


    /*
     * for non-delete cases, absence of flow entry means addition of a
     * new flow entry with the key specified in the request
     */
    if (!fe) {
        fe = vr_add_flow_req(req, &fe_index);
        if (!fe)
            return -ENOSPC;
    }

    vr_flow_set_mirror(router, req, fe);

    if (req->fr_flags & VR_RFLOW_VALID) {
        fe->fe_rflow = req->fr_rindex;
    } else {
        if (fe->fe_rflow >= 0)
            fe->fe_rflow = -1;
    }

    if (req->fr_flags & VR_FLOW_FLAG_VRFT) 
        fe->fe_dvrf = req->fr_flow_dvrf;

    fe->fe_ecmp_nh_index = req->fr_ecmp_nh_index;
    fe->fe_src_nh_index = req->fr_src_nh_index;
    fe->fe_action = req->fr_action;
    fe->fe_flags = req->fr_flags; 


    return vr_flow_schedule_transition(router, req, fe);
}

/*
 * sandesh handler for vr_flow_req
 */
void
vr_flow_req_process(void *s_req)
{
    int ret = 0;
    struct vrouter *router;
    vr_flow_req *req = (vr_flow_req *)s_req;

    router = vrouter_get(req->fr_rid);
    switch (req->fr_op) {
    case FLOW_OP_FLOW_TABLE_GET:
        req->fr_ftable_size = vr_flow_table_size(router) +
            vr_oflow_table_size(router);
#ifdef __KERNEL__
        req->fr_ftable_dev = vr_flow_major;
#endif
        break;

    case FLOW_OP_FLOW_SET:
        ret = vr_flow_set(router, req);
        break;

    default:
        ret = -EINVAL;
    }

    vr_message_response(VR_FLOW_OBJECT_ID, req, ret);
    return;
}

static void
vr_flow_table_info_destroy(struct vrouter *router)
{
    if (!router->vr_flow_table_info)
        return;

    vr_free(router->vr_flow_table_info);
    router->vr_flow_table_info = NULL;
    router->vr_flow_table_info_size = 0;

    return;
}

static void
vr_flow_table_info_reset(struct vrouter *router)
{
    if (!router->vr_flow_table_info)
        return;

    memset(router->vr_flow_table_info, 0, router->vr_flow_table_info_size);
    return;
}

static int
vr_flow_table_info_init(struct vrouter *router)
{
    unsigned int size;
    struct vr_flow_table_info *infop;

    if (router->vr_flow_table_info)
        return 0;

    size = sizeof(struct vr_flow_table_info) + sizeof(uint32_t) * vr_num_cpus;
    infop = (struct vr_flow_table_info *)vr_zalloc(size);
    if (!infop)
        return vr_module_error(-ENOMEM, __FUNCTION__, __LINE__, size);

    router->vr_flow_table_info = infop;
    router->vr_flow_table_info_size = size;

    return 0;
}

static void
vr_flow_table_destroy(struct vrouter *router)
{
    if (router->vr_flow_table) {
        vr_btable_free(router->vr_flow_table);
        router->vr_flow_table = NULL;
    }

    if (router->vr_oflow_table) {
        vr_btable_free(router->vr_oflow_table);
        router->vr_oflow_table = NULL;
    }

    vr_flow_table_info_destroy(router);

    return;
}

static void
vr_flow_table_reset(struct vrouter *router)
{
    unsigned int start, end, i;
    struct vr_flow_entry *fe;
    struct vr_forwarding_md fmd;
    struct vr_flow_md flmd;

    start = end = 0;
    if (router->vr_flow_table)
        end = vr_btable_entries(router->vr_flow_table);

    if (router->vr_oflow_table) {
        if (!end)
            start = vr_flow_entries;
        end += vr_btable_entries(router->vr_oflow_table);
    }

    if (end) {
        vr_init_forwarding_md(&fmd);
        flmd.flmd_action = VR_FLOW_ACTION_DROP;
        for (i = start; i < end; i++) {
            fe = vr_get_flow_entry(router, i);
            if (fe) {
                flmd.flmd_index = i;
                flmd.flmd_flags = fe->fe_flags;
                fe->fe_action = VR_FLOW_ACTION_DROP;
                vr_flush_entry(router, fe, &flmd, &fmd);
                vr_reset_flow_entry(router, fe, i);
            }
        }
    }

    vr_flow_table_info_reset(router);

    return;
}


static int
vr_flow_table_init(struct vrouter *router)
{
    if (!router->vr_flow_table) {
        if (vr_flow_entries % VR_FLOW_ENTRIES_PER_BUCKET)
            return vr_module_error(-EINVAL, __FUNCTION__,
                    __LINE__, vr_flow_entries);

        router->vr_flow_table = vr_btable_alloc(vr_flow_entries,
                sizeof(struct vr_flow_entry));
        if (!router->vr_flow_table) {
            return vr_module_error(-ENOMEM, __FUNCTION__,
                    __LINE__, VR_DEF_FLOW_ENTRIES);
        }
    }

    if (!router->vr_oflow_table) {
        router->vr_oflow_table = vr_btable_alloc(vr_oflow_entries,
                sizeof(struct vr_flow_entry));
        if (!router->vr_oflow_table) {
            return vr_module_error(-ENOMEM, __FUNCTION__,
                    __LINE__, VR_DEF_OFLOW_ENTRIES);
        }
    }

    return vr_flow_table_info_init(router);
}


/* flow module exit and init */
void
vr_flow_exit(struct vrouter *router, bool soft_reset)
{
    vr_flow_table_reset(router);
    if (!soft_reset) {
        vr_flow_table_destroy(router);
        vr_fragment_table_exit(router);
    }

    return;
}

int
vr_flow_init(struct vrouter *router)
{
    int ret;

    if ((ret = vr_fragment_table_init(router) < 0))
        return ret;

    if ((ret = vr_flow_table_init(router)))
        return ret;

    return 0;
}
