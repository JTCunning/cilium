/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright Authors of Cilium */

#ifndef __LIB_L3_H_
#define __LIB_L3_H_

#include "common.h"
#include "ipv6.h"
#include "ipv4.h"
#include "eps.h"
#include "eth.h"
#include "dbg.h"
#include "l4.h"
#include "icmp6.h"
#include "csum.h"

#ifdef ENABLE_IPV6
static __always_inline int ipv6_l3(struct __ctx_buff *ctx, int l3_off,
				   const __u8 *smac, const __u8 *dmac,
				   __u8 direction)
{
	int ret;

	ret = ipv6_dec_hoplimit(ctx, l3_off);
	if (IS_ERR(ret))
		return ret;
	if (ret > 0) {
		/* Hoplimit was reached */
		return icmp6_send_time_exceeded(ctx, l3_off, direction);
	}

	if (smac && eth_store_saddr(ctx, smac, 0) < 0)
		return DROP_WRITE_ERROR;
	if (dmac && eth_store_daddr(ctx, dmac, 0) < 0)
		return DROP_WRITE_ERROR;

	return CTX_ACT_OK;
}
#endif /* ENABLE_IPV6 */

static __always_inline int ipv4_l3(struct __ctx_buff *ctx, int l3_off,
				   const __u8 *smac, const __u8 *dmac,
				   struct iphdr *ip4)
{
	if (ipv4_dec_ttl(ctx, l3_off, ip4)) {
		/* FIXME: Send ICMP TTL */
		return DROP_INVALID;
	}

	if (smac && eth_store_saddr(ctx, smac, 0) < 0)
		return DROP_WRITE_ERROR;
	if (dmac && eth_store_daddr(ctx, dmac, 0) < 0)
		return DROP_WRITE_ERROR;

	return CTX_ACT_OK;
}

#ifndef SKIP_POLICY_MAP
#ifdef ENABLE_IPV6
/* Performs IPv6 L2/L3 handling and delivers the packet to the destination pod
 * on the same node, either via the stack or via a redirect call.
 * Depending on the configuration, it may also enforce ingress policies for the
 * destination pod via a tail call.
 */
static __always_inline int ipv6_local_delivery(struct __ctx_buff *ctx, int l3_off,
					       __u32 seclabel,
					       const struct endpoint_info *ep,
					       __u8 direction,
					       bool from_host __maybe_unused)
{
	mac_t router_mac = ep->node_mac;
	mac_t lxc_mac = ep->mac;
	int ret;

	cilium_dbg(ctx, DBG_LOCAL_DELIVERY, ep->lxc_id, seclabel);

	/* This will invalidate the size check */
	ret = ipv6_l3(ctx, l3_off, (__u8 *) &router_mac, (__u8 *) &lxc_mac, direction);
	if (ret != CTX_ACT_OK)
		return ret;

#ifdef LOCAL_DELIVERY_METRICS
	/*
	 * Special LXC case for updating egress forwarding metrics.
	 * Note that the packet could still be dropped but it would show up
	 * as an ingress drop counter in metrics.
	 */
	update_metrics(ctx_full_len(ctx), direction, REASON_FORWARDED);
#endif

#if defined(USE_BPF_PROG_FOR_INGRESS_POLICY) && \
	!defined(FORCE_LOCAL_POLICY_EVAL_AT_SOURCE)
	ctx->mark |= MARK_MAGIC_IDENTITY;
	set_identity_mark(ctx, seclabel);

	return redirect_ep(ctx, ep->ifindex, from_host);
#else
	/* Jumps to destination pod's BPF program to enforce ingress policies. */
	ctx_store_meta(ctx, CB_SRC_LABEL, seclabel);
	ctx_store_meta(ctx, CB_IFINDEX, ep->ifindex);
	ctx_store_meta(ctx, CB_FROM_HOST, from_host ? 1 : 0);

	tail_call_dynamic(ctx, &POLICY_CALL_MAP, ep->lxc_id);
	return DROP_MISSED_TAIL_CALL;
#endif
}
#endif /* ENABLE_IPV6 */

/* Performs IPv4 L2/L3 handling and delivers the packet to the destination pod
 * on the same node, either via the stack or via a redirect call.
 * Depending on the configuration, it may also enforce ingress policies for the
 * destination pod via a tail call.
 */
static __always_inline int ipv4_local_delivery(struct __ctx_buff *ctx, int l3_off,
					       __u32 seclabel, struct iphdr *ip4,
					       const struct endpoint_info *ep,
					       __u8 direction __maybe_unused,
					       bool from_host __maybe_unused)
{
	mac_t router_mac = ep->node_mac;
	mac_t lxc_mac = ep->mac;
	int ret;

	cilium_dbg(ctx, DBG_LOCAL_DELIVERY, ep->lxc_id, seclabel);

	ret = ipv4_l3(ctx, l3_off, (__u8 *) &router_mac, (__u8 *) &lxc_mac, ip4);
	if (ret != CTX_ACT_OK)
		return ret;

#ifdef LOCAL_DELIVERY_METRICS
	/*
	 * Special LXC case for updating egress forwarding metrics.
	 * Note that the packet could still be dropped but it would show up
	 * as an ingress drop counter in metrics.
	 */
	update_metrics(ctx_full_len(ctx), direction, REASON_FORWARDED);
#endif

#if defined(USE_BPF_PROG_FOR_INGRESS_POLICY) && \
	!defined(FORCE_LOCAL_POLICY_EVAL_AT_SOURCE)
	ctx->mark |= MARK_MAGIC_IDENTITY;
	set_identity_mark(ctx, seclabel);

	return redirect_ep(ctx, ep->ifindex, from_host);
#else
	/* Jumps to destination pod's BPF program to enforce ingress policies. */
	ctx_store_meta(ctx, CB_SRC_LABEL, seclabel);
	ctx_store_meta(ctx, CB_IFINDEX, ep->ifindex);
	ctx_store_meta(ctx, CB_FROM_HOST, from_host ? 1 : 0);

	tail_call_dynamic(ctx, &POLICY_CALL_MAP, ep->lxc_id);
	return DROP_MISSED_TAIL_CALL;
#endif
}
#endif /* SKIP_POLICY_MAP */

static __always_inline __u8 get_min_encrypt_key(__u8 peer_key __maybe_unused)
{
#ifdef ENABLE_IPSEC
	__u8 local_key = 0;
	__u32 encrypt_key = 0;
	struct encrypt_config *cfg;

	cfg = map_lookup_elem(&ENCRYPT_MAP, &encrypt_key);
	/* Having no key info for a context is the same as no encryption */
	if (cfg)
		local_key = cfg->encrypt_key;

	/* If both ends can encrypt/decrypt use smaller of the two this
	 * way both ends will have keys installed assuming key IDs are
	 * always increasing. However, we have to handle roll-over case
	 * and to do this safely we assume keys are no more than one ahead.
	 * We expect user/control-place to accomplish this. Notice zero
	 * will always be returned if either local or peer have the zero
	 * key indicating no encryption.
	 */
	if (peer_key == MAX_KEY_INDEX)
		return local_key == 1 ? peer_key : local_key;
	if (local_key == MAX_KEY_INDEX)
		return peer_key == 1 ? local_key : peer_key;
	return local_key < peer_key ? local_key : peer_key;
#else
	return 0;
#endif /* ENABLE_IPSEC */
}

#endif
