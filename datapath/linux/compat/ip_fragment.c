/*
 * IP fragmentation backport, heavily based on linux/net/ipv4/ip_fragment.c,
 * copied from Linux 192132b9a034 net: Add support for VRFs to inetpeer cache
 *
 * INET        An implementation of the TCP/IP protocol suite for the LINUX
 *        operating system.  INET is implemented using the  BSD Socket
 *        interface as the means of communication with the user level.
 *
 *        The IP fragmentation functionality.
 *
 * Authors:    Fred N. van Kempen <waltje@uWalt.NL.Mugnet.ORG>
 *        Alan Cox <alan@lxorguk.ukuu.org.uk>
 *
 * Fixes:
 *        Alan Cox    :    Split from ip.c , see ip_input.c for history.
 *        David S. Miller :    Begin massive cleanup...
 *        Andi Kleen    :    Add sysctls.
 *        xxxx        :    Overlapfrag bug.
 *        Ultima          :       ip_expire() kernel panic.
 *        Bill Hawes    :    Frag accounting and evictor fixes.
 *        John McDonald    :    0 length frag bug.
 *        Alexey Kuznetsov:    SMP races, threading, cleanup.
 *        Patrick McHardy :    LRU queue of frag heads for evictor.
 */

#include <linux/version.h>

#ifndef HAVE_CORRECT_MRU_HANDLING

#define pr_fmt(fmt) "IPv4: " fmt

#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/jiffies.h>
#include <linux/skbuff.h>
#include <linux/list.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/netdevice.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <net/route.h>
#include <net/dst.h>
#include <net/sock.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/checksum.h>
#include <net/inetpeer.h>
#include <net/inet_frag.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/inet.h>
#include <linux/netfilter_ipv4.h>
#include <net/inet_ecn.h>
#include <net/vrf.h>
#include <net/netfilter/ipv4/nf_defrag_ipv4.h>
#include <net/netns/generic.h>
#include "datapath.h"

/* NOTE. Logic of IP defragmentation is parallel to corresponding IPv6
 * code now. If you change something here, _PLEASE_ update ipv6/reassembly.c
 * as well. Or notify me, at least. --ANK
 */

static int sysctl_ipfrag_max_dist __read_mostly = 64;
static const char ip_frag_cache_name[] = "ovs-frag4";

struct ipfrag_skb_cb
{
    struct inet_skb_parm    h;
    int            offset;
};

#define FRAG_CB(skb)    ((struct ipfrag_skb_cb *)((skb)->cb))

/* Describe an entry in the "incomplete datagrams" queue. */
struct ipq {
    struct inet_frag_queue q;

    u32        user;
    __be32        saddr;
    __be32        daddr;
    __be16        id;
    u8        protocol;
    u8        ecn; /* RFC3168 support */
    u16        max_df_size; /* largest frag with DF set seen */
    int             iif;
    int             vif;   /* VRF device index */
    unsigned int    rid;
    struct inet_peer *peer;
};

static u8 ip4_frag_ecn(u8 tos)
{
    return 1 << (tos & INET_ECN_MASK);
}

static struct inet_frags ip4_frags;

static int ip_frag_reasm(struct ipq *qp, struct sk_buff *prev,
             struct net_device *dev);

struct ip4_create_arg {
    struct iphdr *iph;
    u32 user;
    int vif;
};

static struct netns_frags *get_netns_frags_from_net(struct net *net)
{
#ifdef HAVE_INET_FRAG_LRU_MOVE
    struct ovs_net *ovs_net = net_generic(net, ovs_net_id);
    return &(ovs_net->ipv4_frags);
#else
    return &(net->ipv4.frags);
#endif
}

static struct net *get_net_from_netns_frags(struct netns_frags *frags)
{
    struct net *net;
#ifdef HAVE_INET_FRAG_LRU_MOVE
    struct ovs_net *ovs_net;

    ovs_net = container_of(frags, struct ovs_net, ipv4_frags);
    net = ovs_net->net;
#else
    net = container_of(frags, struct net, ipv4.frags);
#endif
    return net;
}

void ovs_netns_frags_init(struct net *net)
{
#ifdef HAVE_INET_FRAG_LRU_MOVE
    struct ovs_net *ovs_net = net_generic(net, ovs_net_id);

    ovs_net->ipv4_frags.high_thresh = 4 * 1024 * 1024;
    ovs_net->ipv4_frags.low_thresh = 3 * 1024 * 1024;
    ovs_net->ipv4_frags.timeout = IP_FRAG_TIME;
    inet_frags_init_net(&(ovs_net->ipv4_frags));
    ovs_net->net = net;
#endif
}

void ovs_netns_frags_exit(struct net *net)
{
    struct netns_frags *frags;

    frags = get_netns_frags_from_net(net);
    inet_frags_exit_net(frags, &ip4_frags);
}

static unsigned int ipqhashfn(__be16 id, __be32 saddr, __be32 daddr, u8 prot)
{
    net_get_random_once(&ip4_frags.rnd, sizeof(ip4_frags.rnd));
    return jhash_3words((__force u32)id << 16 | prot,
                (__force u32)saddr, (__force u32)daddr,
                ip4_frags.rnd);
}
/* fb3cfe6e75b9 ("inet: frag: remove hash size assumptions from callers")
 * shifted this logic into inet_fragment, but prior kernels still need this.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,17,0)
#define ipqhashfn(a, b, c, d) (ipqhashfn(a, b, c, d) & (INETFRAGS_HASHSZ - 1))
#endif

#ifdef HAVE_INET_FRAGS_CONST
static unsigned int ip4_hashfn(const struct inet_frag_queue *q)
#else
static unsigned int ip4_hashfn(struct inet_frag_queue *q)
#endif
{
    const struct ipq *ipq;

    ipq = container_of(q, struct ipq, q);
    return ipqhashfn(ipq->id, ipq->saddr, ipq->daddr, ipq->protocol);
}

#ifdef HAVE_INET_FRAGS_CONST
static bool ip4_frag_match(const struct inet_frag_queue *q, const void *a)
#else
static bool ip4_frag_match(struct inet_frag_queue *q, void *a)
#endif
{
    const struct ipq *qp;
    const struct ip4_create_arg *arg = a;

    qp = container_of(q, struct ipq, q);
    return    qp->id == arg->iph->id &&
        qp->saddr == arg->iph->saddr &&
        qp->daddr == arg->iph->daddr &&
        qp->protocol == arg->iph->protocol &&
        qp->user == arg->user &&
        qp->vif == arg->vif;
}

#ifdef HAVE_INET_FRAGS_CONST
static void ip4_frag_init(struct inet_frag_queue *q, const void *a)
#else
static void ip4_frag_init(struct inet_frag_queue *q, void *a)
#endif
{
    struct ipq *qp = container_of(q, struct ipq, q);
    struct net *net = get_net_from_netns_frags(q->net);

    const struct ip4_create_arg *arg = a;

    qp->protocol = arg->iph->protocol;
    qp->id = arg->iph->id;
    qp->ecn = ip4_frag_ecn(arg->iph->tos);
    qp->saddr = arg->iph->saddr;
    qp->daddr = arg->iph->daddr;
    qp->vif = arg->vif;
    qp->user = arg->user;
    qp->peer = sysctl_ipfrag_max_dist ?
        inet_getpeer_v4(net->ipv4.peers, arg->iph->saddr, arg->vif, 1) :
        NULL;
}

static void ip4_frag_free(struct inet_frag_queue *q)
{
    struct ipq *qp;

    qp = container_of(q, struct ipq, q);
    if (qp->peer)
        inet_putpeer(qp->peer);
}


/* Destruction primitives. */

static void ipq_put(struct ipq *ipq)
{
    inet_frag_put(&ipq->q, &ip4_frags);
}

/* Kill ipq entry. It is not destroyed immediately,
 * because caller (and someone more) holds reference count.
 */
static void ipq_kill(struct ipq *ipq)
{
    inet_frag_kill(&ipq->q, &ip4_frags);
}

static bool frag_expire_skip_icmp(u32 user)
{
    return user == IP_DEFRAG_AF_PACKET ||
           ip_defrag_user_in_between(user, IP_DEFRAG_CONNTRACK_IN,
                     __IP_DEFRAG_CONNTRACK_IN_END) ||
           ip_defrag_user_in_between(user, IP_DEFRAG_CONNTRACK_BRIDGE_IN,
                     __IP_DEFRAG_CONNTRACK_BRIDGE_IN);
}

/*
 * Oops, a fragment queue timed out.  Kill it and send an ICMP reply.
 */
static void ip_expire(unsigned long arg)
{
    struct ipq *qp;
    struct net *net;

    qp = container_of((struct inet_frag_queue *) arg, struct ipq, q);
    net = get_net_from_netns_frags(qp->q.net);

    spin_lock(&qp->q.lock);

    if (qp_flags(qp) & INET_FRAG_COMPLETE)
        goto out;

    ipq_kill(qp);
    IP_INC_STATS_BH(net, IPSTATS_MIB_REASMFAILS);

    if (!inet_frag_evicting(&qp->q)) {
        struct sk_buff *head = qp->q.fragments;
        const struct iphdr *iph;
        int err;

        IP_INC_STATS_BH(net, IPSTATS_MIB_REASMTIMEOUT);

        if (!(qp_flags(qp) & INET_FRAG_FIRST_IN) || !qp->q.fragments)
            goto out;

        rcu_read_lock();
        head->dev = dev_get_by_index_rcu(net, qp->iif);
        if (!head->dev)
            goto out_rcu_unlock;

        /* skb has no dst, perform route lookup again */
        iph = ip_hdr(head);
        err = ip_route_input_noref(head, iph->daddr, iph->saddr,
                       iph->tos, head->dev);
        if (err)
            goto out_rcu_unlock;

        /* Only an end host needs to send an ICMP
         * "Fragment Reassembly Timeout" message, per RFC792.
         */
        if (frag_expire_skip_icmp(qp->user) &&
            (skb_rtable(head)->rt_type != RTN_LOCAL))
            goto out_rcu_unlock;

        /* Send an ICMP "Fragment Reassembly Timeout" message. */
        icmp_send(head, ICMP_TIME_EXCEEDED, ICMP_EXC_FRAGTIME, 0);
out_rcu_unlock:
        rcu_read_unlock();
    }
out:
    spin_unlock(&qp->q.lock);
    ipq_put(qp);
}

#ifdef HAVE_INET_FRAG_EVICTOR
/* Memory limiting on fragments.  Evictor trashes the oldest
 * fragment queue until we are back under the threshold.
 *
 * Necessary for kernels earlier than v3.17. Replaced in commit
 * b13d3cbfb8e8 ("inet: frag: move eviction of queues to work queue").
 */
static void ip_evictor(struct net *net)
{
    int evicted;
    struct netns_frags *frags;

    frags = get_netns_frags_from_net(net);
    evicted = inet_frag_evictor(frags, &ip4_frags, false);
    if (evicted)
        IP_ADD_STATS_BH(net, IPSTATS_MIB_REASMFAILS, evicted);
}
#endif

/* Find the correct entry in the "incomplete datagrams" queue for
 * this IP datagram, and create new one, if nothing is found.
 */
static struct ipq *ip_find(struct net *net, struct iphdr *iph,
               u32 user, int vif)
{
    struct inet_frag_queue *q;
    struct ip4_create_arg arg;
    unsigned int hash;
    struct netns_frags *frags;

    arg.iph = iph;
    arg.user = user;
    arg.vif = vif;

#ifdef HAVE_INET_FRAGS_WITH_RWLOCK
    read_lock(&ip4_frags.lock);
#endif
    hash = ipqhashfn(iph->id, iph->saddr, iph->daddr, iph->protocol);

    frags = get_netns_frags_from_net(net);
    q = inet_frag_find(frags, &ip4_frags, &arg, hash);
    if (IS_ERR_OR_NULL(q)) {
        inet_frag_maybe_warn_overflow(q, pr_fmt());
        return NULL;
    }
    return container_of(q, struct ipq, q);
}

/* Is the fragment too far ahead to be part of ipq? */
static int ip_frag_too_far(struct ipq *qp)
{
    struct inet_peer *peer = qp->peer;
    unsigned int max = sysctl_ipfrag_max_dist;
    unsigned int start, end;

    int rc;

    if (!peer || !max)
        return 0;

    start = qp->rid;
    end = atomic_inc_return(&peer->rid);
    qp->rid = end;

    rc = qp->q.fragments && (end - start) > max;

    if (rc) {
        struct net *net;

        net = get_net_from_netns_frags(qp->q.net);
        IP_INC_STATS_BH(net, IPSTATS_MIB_REASMFAILS);
    }

    return rc;
}

static int ip_frag_reinit(struct ipq *qp)
{
    struct sk_buff *fp;
    unsigned int sum_truesize = 0;

    if (!mod_timer(&qp->q.timer, jiffies + qp->q.net->timeout)) {
        atomic_inc(&qp->q.refcnt);
        return -ETIMEDOUT;
    }

    fp = qp->q.fragments;
    do {
        struct sk_buff *xp = fp->next;

        sum_truesize += fp->truesize;
        kfree_skb(fp);
        fp = xp;
    } while (fp);
    sub_frag_mem_limit(qp->q.net, sum_truesize);

    qp_flags(qp) = 0;
    qp->q.len = 0;
    qp->q.meat = 0;
    qp->q.fragments = NULL;
    qp->q.fragments_tail = NULL;
    qp->iif = 0;
    qp->ecn = 0;

    return 0;
}

/* Add new segment to existing queue. */
static int ip_frag_queue(struct ipq *qp, struct sk_buff *skb)
{
    struct sk_buff *prev, *next;
    struct net_device *dev;
    unsigned int fragsize;
    int flags, offset;
    int ihl, end;
    int err = -ENOENT;
    u8 ecn;

    if (qp_flags(qp) & INET_FRAG_COMPLETE)
        goto err;

    if (!(IPCB(skb)->flags & IPSKB_FRAG_COMPLETE) &&
        unlikely(ip_frag_too_far(qp)) &&
        unlikely(err = ip_frag_reinit(qp))) {
        ipq_kill(qp);
        goto err;
    }

    ecn = ip4_frag_ecn(ip_hdr(skb)->tos);
    offset = ntohs(ip_hdr(skb)->frag_off);
    flags = offset & ~IP_OFFSET;
    offset &= IP_OFFSET;
    offset <<= 3;        /* offset is in 8-byte chunks */
    ihl = ip_hdrlen(skb);

    /* Determine the position of this fragment. */
    end = offset + skb->len - skb_network_offset(skb) - ihl;
    err = -EINVAL;

    /* Is this the final fragment? */
    if ((flags & IP_MF) == 0) {
        /* If we already have some bits beyond end
         * or have different end, the segment is corrupted.
         */
        if (end < qp->q.len ||
            ((qp_flags(qp) & INET_FRAG_LAST_IN) && end != qp->q.len))
            goto err;
        qp_flags(qp) |= INET_FRAG_LAST_IN;
        qp->q.len = end;
    } else {
        if (end&7) {
            end &= ~7;
            if (skb->ip_summed != CHECKSUM_UNNECESSARY)
                skb->ip_summed = CHECKSUM_NONE;
        }
        if (end > qp->q.len) {
            /* Some bits beyond end -> corruption. */
            if (qp_flags(qp) & INET_FRAG_LAST_IN)
                goto err;
            qp->q.len = end;
        }
    }
    if (end == offset)
        goto err;

    err = -ENOMEM;
    if (!pskb_pull(skb, skb_network_offset(skb) + ihl))
        goto err;

    err = pskb_trim_rcsum(skb, end - offset);
    if (err)
        goto err;

    /* Find out which fragments are in front and at the back of us
     * in the chain of fragments so far.  We must know where to put
     * this fragment, right?
     */
    prev = qp->q.fragments_tail;
    if (!prev || FRAG_CB(prev)->offset < offset) {
        next = NULL;
        goto found;
    }
    prev = NULL;
    for (next = qp->q.fragments; next != NULL; next = next->next) {
        if (FRAG_CB(next)->offset >= offset)
            break;    /* bingo! */
        prev = next;
    }

found:
    /* We found where to put this one.  Check for overlap with
     * preceding fragment, and, if needed, align things so that
     * any overlaps are eliminated.
     */
    if (prev) {
        int i = (FRAG_CB(prev)->offset + prev->len) - offset;

        if (i > 0) {
            offset += i;
            err = -EINVAL;
            if (end <= offset)
                goto err;
            err = -ENOMEM;
            if (!pskb_pull(skb, i))
                goto err;
            if (skb->ip_summed != CHECKSUM_UNNECESSARY)
                skb->ip_summed = CHECKSUM_NONE;
        }
    }

    err = -ENOMEM;

    while (next && FRAG_CB(next)->offset < end) {
        int i = end - FRAG_CB(next)->offset; /* overlap is 'i' bytes */

        if (i < next->len) {
            /* Eat head of the next overlapped fragment
             * and leave the loop. The next ones cannot overlap.
             */
            if (!pskb_pull(next, i))
                goto err;
            FRAG_CB(next)->offset += i;
            qp->q.meat -= i;
            if (next->ip_summed != CHECKSUM_UNNECESSARY)
                next->ip_summed = CHECKSUM_NONE;
            break;
        } else {
            struct sk_buff *free_it = next;

            /* Old fragment is completely overridden with
             * new one drop it.
             */
            next = next->next;

            if (prev)
                prev->next = next;
            else
                qp->q.fragments = next;

            qp->q.meat -= free_it->len;
            sub_frag_mem_limit(qp->q.net, free_it->truesize);
            kfree_skb(free_it);
        }
    }

    FRAG_CB(skb)->offset = offset;

    /* Insert this fragment in the chain of fragments. */
    skb->next = next;
    if (!next)
        qp->q.fragments_tail = skb;
    if (prev)
        prev->next = skb;
    else
        qp->q.fragments = skb;

    dev = skb->dev;
    if (dev) {
        qp->iif = dev->ifindex;
        skb->dev = NULL;
    }
    qp->q.stamp = skb->tstamp;
    qp->q.meat += skb->len;
    qp->ecn |= ecn;
    add_frag_mem_limit(qp->q.net, skb->truesize);
    if (offset == 0)
        qp_flags(qp) |= INET_FRAG_FIRST_IN;

    fragsize = skb->len + ihl;

    if (fragsize > qp->q.max_size)
        qp->q.max_size = fragsize;

    if (ip_hdr(skb)->frag_off & htons(IP_DF) &&
        fragsize > qp->max_df_size)
        qp->max_df_size = fragsize;

    if (qp_flags(qp) == (INET_FRAG_FIRST_IN | INET_FRAG_LAST_IN) &&
        qp->q.meat == qp->q.len) {
        unsigned long orefdst = skb->_skb_refdst;

        skb->_skb_refdst = 0UL;
        err = ip_frag_reasm(qp, prev, dev);
        skb->_skb_refdst = orefdst;
        return err;
    }

    skb_dst_drop(skb);
    inet_frag_lru_move(&qp->q);
    return -EINPROGRESS;

err:
    kfree_skb(skb);
    return err;
}


/* Build a new IP datagram from all its fragments. */

static int ip_frag_reasm(struct ipq *qp, struct sk_buff *prev,
             struct net_device *dev)
{
    struct net *net = get_net_from_netns_frags(qp->q.net);
    struct iphdr *iph;
    struct sk_buff *fp, *head = qp->q.fragments;
    int len;
    int ihlen;
    int err;
    u8 ecn;

    ipq_kill(qp);

    ecn = ip_frag_ecn_table[qp->ecn];
    if (unlikely(ecn == 0xff)) {
        err = -EINVAL;
        goto out_fail;
    }
    /* Make the one we just received the head. */
    if (prev) {
        head = prev->next;
        fp = skb_clone(head, GFP_ATOMIC);
        if (!fp)
            goto out_nomem;

        fp->next = head->next;
        if (!fp->next)
            qp->q.fragments_tail = fp;
        prev->next = fp;

        skb_morph(head, qp->q.fragments);
        head->next = qp->q.fragments->next;

        consume_skb(qp->q.fragments);
        qp->q.fragments = head;
    }

    WARN_ON(!head);
    WARN_ON(FRAG_CB(head)->offset != 0);

    /* Allocate a new buffer for the datagram. */
    ihlen = ip_hdrlen(head);
    len = ihlen + qp->q.len;

    err = -E2BIG;
    if (len > 65535)
        goto out_oversize;

    /* Head of list must not be cloned. */
    if (skb_unclone(head, GFP_ATOMIC))
        goto out_nomem;

    /* If the first fragment is fragmented itself, we split
     * it to two chunks: the first with data and paged part
     * and the second, holding only fragments. */
    if (skb_has_frag_list(head)) {
        struct sk_buff *clone;
        int i, plen = 0;

        clone = alloc_skb(0, GFP_ATOMIC);
        if (!clone)
            goto out_nomem;
        clone->next = head->next;
        head->next = clone;
        skb_shinfo(clone)->frag_list = skb_shinfo(head)->frag_list;
        skb_frag_list_init(head);
        for (i = 0; i < skb_shinfo(head)->nr_frags; i++)
            plen += skb_frag_size(&skb_shinfo(head)->frags[i]);
        clone->len = clone->data_len = head->data_len - plen;
        head->data_len -= clone->len;
        head->len -= clone->len;
        clone->csum = 0;
        clone->ip_summed = head->ip_summed;
        add_frag_mem_limit(qp->q.net, clone->truesize);
    }

    skb_shinfo(head)->frag_list = head->next;
    skb_push(head, head->data - skb_network_header(head));

    for (fp=head->next; fp; fp = fp->next) {
        head->data_len += fp->len;
        head->len += fp->len;
        if (head->ip_summed != fp->ip_summed)
            head->ip_summed = CHECKSUM_NONE;
        else if (head->ip_summed == CHECKSUM_COMPLETE)
            head->csum = csum_add(head->csum, fp->csum);
        head->truesize += fp->truesize;
    }
    sub_frag_mem_limit(qp->q.net, head->truesize);

    head->next = NULL;
    head->dev = dev;
    head->tstamp = qp->q.stamp;
    IPCB(head)->frag_max_size = max(qp->max_df_size, qp->q.max_size);

    iph = ip_hdr(head);
    iph->tot_len = htons(len);
    iph->tos |= ecn;

    /* When we set IP_DF on a refragmented skb we must also force a
     * call to ip_fragment to avoid forwarding a DF-skb of size s while
     * original sender only sent fragments of size f (where f < s).
     *
     * We only set DF/IPSKB_FRAG_PMTU if such DF fragment was the largest
     * frag seen to avoid sending tiny DF-fragments in case skb was built
     * from one very small df-fragment and one large non-df frag.
     */
    if (qp->max_df_size == qp->q.max_size) {
        IPCB(head)->flags |= IPSKB_FRAG_PMTU;
        iph->frag_off = htons(IP_DF);
    } else {
        iph->frag_off = 0;
    }

    ip_send_check(iph);

    IP_INC_STATS_BH(net, IPSTATS_MIB_REASMOKS);
    qp->q.fragments = NULL;
    qp->q.fragments_tail = NULL;
    return 0;

out_nomem:
    net_dbg_ratelimited("queue_glue: no memory for gluing queue %p\n", qp);
    err = -ENOMEM;
    goto out_fail;
out_oversize:
    net_info_ratelimited("Oversized IP packet from %pI4\n", &qp->saddr);
out_fail:
    IP_INC_STATS_BH(net, IPSTATS_MIB_REASMFAILS);
    return err;
}

/* Process an incoming IP datagram fragment. */
int rpl_ip_defrag(struct net *net, struct sk_buff *skb, u32 user)
{
    struct net_device *dev = skb->dev ? : skb_dst(skb)->dev;
    int vif = vrf_master_ifindex_rcu(dev);
    struct ipq *qp;

    IP_INC_STATS_BH(net, IPSTATS_MIB_REASMREQDS);
    skb_orphan(skb);

#ifdef HAVE_INET_FRAG_EVICTOR
    /* Start by cleaning up the memory. */
    ip_evictor(net);
#endif

    /* Lookup (or create) queue header */
    qp = ip_find(net, ip_hdr(skb), user, vif);
    if (qp) {
        int ret;

        spin_lock(&qp->q.lock);

        ret = ip_frag_queue(qp, skb);

        spin_unlock(&qp->q.lock);
        ipq_put(qp);
        return ret;
    }

    IP_INC_STATS_BH(net, IPSTATS_MIB_REASMFAILS);
    kfree_skb(skb);
    return -ENOMEM;
}

#ifdef HAVE_DEFRAG_ENABLE_TAKES_NET
static int __net_init ipv4_frags_init_net(struct net *net)
{
    return nf_defrag_ipv4_enable(net);
}
#endif

static void __net_exit ipv4_frags_exit_net(struct net *net)
{
}

static struct pernet_operations ip4_frags_ops = {
#ifdef HAVE_DEFRAG_ENABLE_TAKES_NET
    .init = ipv4_frags_init_net,
#endif
    .exit = ipv4_frags_exit_net,
};

int __init rpl_ipfrag_init(void)
{
#ifndef HAVE_DEFRAG_ENABLE_TAKES_NET
    nf_defrag_ipv4_enable();
#endif
    register_pernet_subsys(&ip4_frags_ops);
    ip4_frags.hashfn = ip4_hashfn;
    ip4_frags.constructor = ip4_frag_init;
    ip4_frags.destructor = ip4_frag_free;
    ip4_frags.skb_free = NULL;
    ip4_frags.qsize = sizeof(struct ipq);
    ip4_frags.match = ip4_frag_match;
    ip4_frags.frag_expire = ip_expire;
#ifdef HAVE_INET_FRAGS_WITH_FRAGS_WORK
    ip4_frags.frags_cache_name = ip_frag_cache_name;
#endif
#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,0)
    ip4_frags.secret_interval = 10 * 60 * HZ;
#endif
    if (inet_frags_init(&ip4_frags)) {
        pr_warn("IP: failed to allocate ip4_frags cache\n");
        return -ENOMEM;
    }
    return 0;
}

void rpl_ipfrag_fini(void)
{
    inet_frags_fini(&ip4_frags);
    unregister_pernet_subsys(&ip4_frags_ops);
}

#endif /* !HAVE_CORRECT_MRU_HANDLING */
