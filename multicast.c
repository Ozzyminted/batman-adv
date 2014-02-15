/* Copyright (C) 2014 B.A.T.M.A.N. contributors:
 *
 * Linus Lüssing
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include "main.h"
#include "multicast.h"
#include "originator.h"
#include "hard-interface.h"
#include "translation-table.h"
#include "multicast.h"

/**
 * batadv_mcast_mla_softif_get - get softif multicast listeners
 * @dev: the device to collect multicast addresses from
 * @mcast_list: a list to put found addresses into
 *
 * Collect multicast addresses of the local multicast listeners
 * on the given soft interface, dev, in the given mcast_list.
 *
 * Return -ENOMEM on memory allocation error or the number of
 * items added to the mcast_list otherwise.
 */
static int batadv_mcast_mla_softif_get(struct net_device *dev,
				       struct hlist_head *mcast_list)
{
	struct netdev_hw_addr *mc_list_entry;
	struct batadv_hw_addr *new;
	int ret = 0;

	netif_addr_lock_bh(dev);
	netdev_for_each_mc_addr(mc_list_entry, dev) {
		new = kmalloc(sizeof(*new), GFP_ATOMIC);
		if (!new) {
			ret = -ENOMEM;
			break;
		}

		memcpy(&new->addr, &mc_list_entry->addr, ETH_ALEN);
		hlist_add_head(&new->list, mcast_list);
		ret++;
	}
	netif_addr_unlock_bh(dev);

	return ret;
}

/**
 * batadv_mcast_mla_is_duplicate - check whether an address is in a list
 * @mcast_addr: the multicast address to check
 * @mcast_list: the list with multicast addresses to search in
 *
 * Return true if the given address is already in the given list.
 * Otherwise returns false.
 */
static bool batadv_mcast_mla_is_duplicate(uint8_t *mcast_addr,
					  struct hlist_head *mcast_list)
{
	struct batadv_hw_addr *mcast_entry;

	hlist_for_each_entry(mcast_entry, mcast_list, list)
		if (batadv_compare_eth(mcast_entry->addr, mcast_addr))
			return true;

	return false;
}

/**
 * batadv_mcast_mla_list_free - free a list of multicast addresses
 * @mcast_list: the list to free
 *
 * Remove and free all items in the given mcast_list.
 */
static void batadv_mcast_mla_list_free(struct hlist_head *mcast_list)
{
	struct batadv_hw_addr *mcast_entry;
	struct hlist_node *tmp;

	hlist_for_each_entry_safe(mcast_entry, tmp, mcast_list, list) {
		hlist_del(&mcast_entry->list);
		kfree(mcast_entry);
	}
}

/**
 * batadv_mcast_mla_tt_retract - clean up multicast listener announcements
 * @bat_priv: the bat priv with all the soft interface information
 * @mcast_list: a list of addresses which should _not_ be removed
 *
 * Retract the announcement of any multicast listener from the
 * translation table except the ones listed in the given mcast_list.
 *
 * If mcast_list is NULL then all are retracted.
 */
static void batadv_mcast_mla_tt_retract(struct batadv_priv *bat_priv,
					struct hlist_head *mcast_list)
{
	struct batadv_hw_addr *mcast_entry;
	struct hlist_node *tmp;

	hlist_for_each_entry_safe(mcast_entry, tmp, &bat_priv->mcast.mla_list,
				  list) {
		if (mcast_list &&
		    batadv_mcast_mla_is_duplicate(mcast_entry->addr,
						  mcast_list))
			continue;

		batadv_tt_local_remove(bat_priv, mcast_entry->addr,
				       BATADV_NO_FLAGS,
				       "mcast TT outdated", false);

		hlist_del(&mcast_entry->list);
		kfree(mcast_entry);
	}
}

/**
 * batadv_mcast_mla_tt_add - add multicast listener announcements
 * @bat_priv: the bat priv with all the soft interface information
 * @mcast_list: a list of addresses which are going to get added
 *
 * Add multicast listener announcements from the given mcast_list to the
 * translation table if they have not been added yet.
 */
static void batadv_mcast_mla_tt_add(struct batadv_priv *bat_priv,
				    struct hlist_head *mcast_list)
{
	struct batadv_hw_addr *mcast_entry;
	struct hlist_node *tmp;

	if (!mcast_list)
		return;

	hlist_for_each_entry_safe(mcast_entry, tmp, mcast_list, list) {
		if (batadv_mcast_mla_is_duplicate(mcast_entry->addr,
						  &bat_priv->mcast.mla_list))
			continue;

		if (!batadv_tt_local_add(bat_priv->soft_iface,
					 mcast_entry->addr, BATADV_NO_FLAGS,
					 BATADV_NULL_IFINDEX, BATADV_NO_MARK))
			continue;

		hlist_del(&mcast_entry->list);
		hlist_add_head(&mcast_entry->list, &bat_priv->mcast.mla_list);
	}
}

/**
 * batadv_mcast_get_bridge - get the bridge interface on our soft interface
 * @bat_priv: the bat priv with all the soft interface information
 *
 * Return the next bridge interface on top of our soft interface and increase
 * its refcount. If no such bridge interface exists, then return NULL.
 */
static struct net_device *
batadv_mcast_get_bridge(struct batadv_priv *bat_priv)
{
	struct net_device *upper = bat_priv->soft_iface;

	rcu_read_lock();

	do {
		upper = netdev_master_upper_dev_get_rcu(upper);
	} while (upper && !(upper->priv_flags & IFF_EBRIDGE));

	if (upper)
		dev_hold(upper);

	rcu_read_unlock();

	return upper;
}

/**
 * batadv_mcast_has_bridge - check whether the soft-iface is bridged
 * @bat_priv: the bat priv with all the soft interface information
 *
 * Check whether there is a bridge on top of our soft interface. Return
 * true if so, false otherwise.
 */
static bool batadv_mcast_has_bridge(struct batadv_priv *bat_priv)
{
	struct net_device *bridge;

	bridge = batadv_mcast_get_bridge(bat_priv);
	if (!bridge)
		goto out;

	dev_put(bridge);
	return true;
out:
	return false;
}

/**
 * batadv_mcast_mla_tvlv_update - update multicast tvlv
 * @bat_priv: the bat priv with all the soft interface information
 *
 * Update the own multicast tvlv with our current multicast related settings,
 * capabilities and inabilities.
 *
 * Return true if the tvlv container is registered afterwards. Otherwise return
 * false.
 */
static bool batadv_mcast_mla_tvlv_update(struct batadv_priv *bat_priv)
{
	struct batadv_tvlv_mcast_data mcast_data;

	mcast_data.flags = BATADV_NO_FLAGS;
	memset(mcast_data.reserved, 0, sizeof(mcast_data.reserved));

	/* Avoid attaching MLAs, if there is a bridge on top of our soft
	 * interface, we don't support that yet (TODO)
	 */
	if (batadv_mcast_has_bridge(bat_priv)) {
		if (bat_priv->mcast.enabled) {
			batadv_tvlv_container_unregister(bat_priv,
							 BATADV_TVLV_MCAST, 1);
			bat_priv->mcast.enabled = false;
		}

		return false;
	}

	if (!bat_priv->mcast.enabled ||
	    mcast_data.flags != bat_priv->mcast.flags) {
		batadv_tvlv_container_register(bat_priv, BATADV_TVLV_MCAST, 1,
					       &mcast_data, sizeof(mcast_data));
		bat_priv->mcast.flags = mcast_data.flags;
		bat_priv->mcast.enabled = true;
	}

	return true;
}

/**
 * batadv_mcast_mla_update - update the own MLAs
 * @bat_priv: the bat priv with all the soft interface information
 *
 * Update the own multicast listener announcements in the translation
 * table as well as the own, announced multicast tvlv container.
 */
void batadv_mcast_mla_update(struct batadv_priv *bat_priv)
{
	struct net_device *soft_iface = bat_priv->soft_iface;
	struct hlist_head mcast_list = HLIST_HEAD_INIT;
	int ret;

	if (!batadv_mcast_mla_tvlv_update(bat_priv))
		goto update;

	ret = batadv_mcast_mla_softif_get(soft_iface, &mcast_list);
	if (ret < 0)
		goto out;

update:
	batadv_mcast_mla_tt_retract(bat_priv, &mcast_list);
	batadv_mcast_mla_tt_add(bat_priv, &mcast_list);

out:
	batadv_mcast_mla_list_free(&mcast_list);
}

/**
 * batadv_mcast_forw_mode_check_ipv6 - check for optimized forwarding potential
 * @bat_priv: the bat priv with all the soft interface information
 * @skb: the IPv6 packet to check
 *
 * Check whether the given IPv6 packet has the potential to
 * be forwarded with a mode more optimal than classic flooding.
 *
 * If so then return 0. Otherwise -EINVAL is returned or -ENOMEM if we are
 * out of memory.
 */
static int batadv_mcast_forw_mode_check_ipv6(struct batadv_priv *bat_priv,
					     struct sk_buff *skb)
{
	struct ipv6hdr *ip6hdr;

	/* We might fail due to out-of-memory -> drop it */
	if (!pskb_may_pull(skb, sizeof(struct ethhdr) + sizeof(*ip6hdr)))
		return -ENOMEM;

	ip6hdr = ipv6_hdr(skb);

	/* TODO: Implement Multicast Router Discovery (RFC4286),
	 * then allow scope > link local, too
	 */
	if (IPV6_ADDR_MC_SCOPE(&ip6hdr->daddr) !=
	    IPV6_ADDR_SCOPE_LINKLOCAL)
		return -EINVAL;

	/* link-local-all-nodes multicast listeners behind a bridge are
	 * not snoopable (see RFC4541, section 3, paragraph 3)
	 */
	if (ipv6_addr_is_ll_all_nodes(&ip6hdr->daddr))
		return -EINVAL;

	return 0;
}

/**
 * batadv_mcast_forw_mode_check - check for optimized forwarding potential
 * @bat_priv: the bat priv with all the soft interface information
 * @skb: the multicast frame to check
 *
 * Check whether the given multicast ethernet frame has the potential to
 * be forwarded with a mode more optimal than classic flooding.
 *
 * If so then return 0. Otherwise -EINVAL is returned or -ENOMEM if we are
 * out of memory.
 */
static int batadv_mcast_forw_mode_check(struct batadv_priv *bat_priv,
					struct sk_buff *skb)
{
	struct ethhdr *ethhdr = eth_hdr(skb);

	if (!atomic_read(&bat_priv->multicast_mode))
		return -EINVAL;

	if (atomic_read(&bat_priv->mcast.num_disabled))
		return -EINVAL;

	switch (ntohs(ethhdr->h_proto)) {
	case ETH_P_IPV6:
		return batadv_mcast_forw_mode_check_ipv6(bat_priv, skb);
	default:
		return -EINVAL;
	}
}

/**
 * batadv_mcast_forw_tt_node_get - get a multicast tt node
 * @bat_priv: the bat priv with all the soft interface information
 * @ethhdr: the ether header containing the multicast destination
 *
 * Return an orig_node matching the multicast address provided by ethhdr
 * via a translation table lookup. This increases the returned nodes refcount.
 */
static struct batadv_orig_node *
batadv_mcast_forw_tt_node_get(struct batadv_priv *bat_priv,
			      struct ethhdr *ethhdr)
{
	return batadv_transtable_search(bat_priv, ethhdr->h_source,
					ethhdr->h_dest, BATADV_NO_FLAGS);
}

/**
 * batadv_mcast_forw_mode - check on how to forward a multicast packet
 * @bat_priv: the bat priv with all the soft interface information
 * @skb: The multicast packet to check
 * @orig: an originator to be set to forward the skb to
 *
 * Return the forwarding mode as enum batadv_forw_mode and in case of
 * BATADV_FORW_SINGLE set the orig to the single originator the skb
 * should be forwarded to.
 */
enum batadv_forw_mode
batadv_mcast_forw_mode(struct batadv_priv *bat_priv, struct sk_buff *skb,
		       struct batadv_orig_node **orig)
{
	struct ethhdr *ethhdr;
	int ret, tt_count;

	ret = batadv_mcast_forw_mode_check(bat_priv, skb);
	if (ret == -ENOMEM)
		return BATADV_FORW_NONE;
	else if (ret < 0)
		return BATADV_FORW_ALL;

	ethhdr = eth_hdr(skb);

	tt_count = batadv_tt_global_hash_count(bat_priv, ethhdr->h_dest,
					       BATADV_NO_FLAGS);

	switch (tt_count) {
	case 1:
		*orig = batadv_mcast_forw_tt_node_get(bat_priv, ethhdr);
		if (*orig)
			return BATADV_FORW_SINGLE;

		/* fall through */
	case 0:
		return BATADV_FORW_NONE;
	default:
		return BATADV_FORW_ALL;
	}
}

/**
 * batadv_mcast_tvlv_ogm_handler_v1 - process incoming multicast tvlv container
 * @bat_priv: the bat priv with all the soft interface information
 * @orig: the orig_node of the ogm
 * @flags: flags indicating the tvlv state (see batadv_tvlv_handler_flags)
 * @tvlv_value: tvlv buffer containing the multicast data
 * @tvlv_value_len: tvlv buffer length
 */
static void batadv_mcast_tvlv_ogm_handler_v1(struct batadv_priv *bat_priv,
					     struct batadv_orig_node *orig,
					     uint8_t flags,
					     void *tvlv_value,
					     uint16_t tvlv_value_len)
{
	bool orig_mcast_enabled = !(flags & BATADV_TVLV_HANDLER_OGM_CIFNOTFND);
	uint8_t mcast_flags = BATADV_NO_FLAGS;
	bool orig_initialized;

	orig_initialized = orig->capa_initialized & BATADV_ORIG_CAPA_HAS_MCAST;

	/* If mcast support is turned on decrease the disabled mcast node
	 * counter only if we had increased it for this node before. If this
	 * is a completely new orig_node no need to decrease the counter.
	 */
	if (orig_mcast_enabled &&
	    !(orig->capabilities & BATADV_ORIG_CAPA_HAS_MCAST)) {
		if (orig_initialized)
			atomic_dec(&bat_priv->mcast.num_disabled);
		orig->capabilities |= BATADV_ORIG_CAPA_HAS_MCAST;
	/* If mcast support is being switched off increase the disabled
	 * mcast node counter.
	 */
	} else if (!orig_mcast_enabled &&
		   orig->capabilities & BATADV_ORIG_CAPA_HAS_MCAST) {
		atomic_inc(&bat_priv->mcast.num_disabled);
		orig->capabilities &= ~BATADV_ORIG_CAPA_HAS_MCAST;
	}

	orig->capa_initialized |= BATADV_ORIG_CAPA_HAS_MCAST;

	if (orig_mcast_enabled && tvlv_value &&
	    (tvlv_value_len >= sizeof(mcast_flags)))
		mcast_flags = *(uint8_t *)tvlv_value;

	orig->mcast_flags = mcast_flags;
}

/**
 * batadv_mcast_init - initialize the multicast optimizations structures
 * @bat_priv: the bat priv with all the soft interface information
 */
void batadv_mcast_init(struct batadv_priv *bat_priv)
{
	batadv_tvlv_handler_register(bat_priv, batadv_mcast_tvlv_ogm_handler_v1,
				     NULL, BATADV_TVLV_MCAST, 1,
				     BATADV_TVLV_HANDLER_OGM_CIFNOTFND);
}

/**
 * batadv_mcast_free - free the multicast optimizations structures
 * @bat_priv: the bat priv with all the soft interface information
 */
void batadv_mcast_free(struct batadv_priv *bat_priv)
{
	batadv_tvlv_container_unregister(bat_priv, BATADV_TVLV_MCAST, 1);
	batadv_tvlv_handler_unregister(bat_priv, BATADV_TVLV_MCAST, 1);

	batadv_mcast_mla_tt_retract(bat_priv, NULL);
}

/**
 * batadv_mcast_purge_orig - reset originator global mcast state modifications
 * @orig: the originator which is going to get purged
 */
void batadv_mcast_purge_orig(struct batadv_orig_node *orig)
{
	struct batadv_priv *bat_priv = orig->bat_priv;

	if (!(orig->capabilities & BATADV_ORIG_CAPA_HAS_MCAST))
		atomic_dec(&bat_priv->mcast.num_disabled);
}
