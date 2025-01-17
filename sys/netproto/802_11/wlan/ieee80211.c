/*-
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002-2009 Sam Leffler, Errno Consulting
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * IEEE 802.11 generic handler
 */
#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/ethernet.h>

#include <netproto/802_11/ieee80211_var.h>
#include <netproto/802_11/ieee80211_regdomain.h>
#ifdef IEEE80211_SUPPORT_SUPERG
#include <netproto/802_11/ieee80211_superg.h>
#endif
#include <netproto/802_11/ieee80211_ratectl.h>

#include <net/bpf.h>

#define IEEE80211_NMBCLUSTERS_DEFMIN	32
#define IEEE80211_NMBCLUSTERS_DEFAULT	128

static int ieee80211_nmbclusters_default = IEEE80211_NMBCLUSTERS_DEFAULT;
TUNABLE_INT("net.link.ieee80211.nmbclusters", &ieee80211_nmbclusters_default);

const char *ieee80211_phymode_name[IEEE80211_MODE_MAX] = {
	[IEEE80211_MODE_AUTO]	  = "auto",
	[IEEE80211_MODE_11A]	  = "11a",
	[IEEE80211_MODE_11B]	  = "11b",
	[IEEE80211_MODE_11G]	  = "11g",
	[IEEE80211_MODE_FH]	  = "FH",
	[IEEE80211_MODE_TURBO_A]  = "turboA",
	[IEEE80211_MODE_TURBO_G]  = "turboG",
	[IEEE80211_MODE_STURBO_A] = "sturboA",
	[IEEE80211_MODE_HALF]	  = "half",
	[IEEE80211_MODE_QUARTER]  = "quarter",
	[IEEE80211_MODE_11NA]	  = "11na",
	[IEEE80211_MODE_11NG]	  = "11ng",
};
/* map ieee80211_opmode to the corresponding capability bit */
const int ieee80211_opcap[IEEE80211_OPMODE_MAX] = {
	[IEEE80211_M_IBSS]	= IEEE80211_C_IBSS,
	[IEEE80211_M_WDS]	= IEEE80211_C_WDS,
	[IEEE80211_M_STA]	= IEEE80211_C_STA,
	[IEEE80211_M_AHDEMO]	= IEEE80211_C_AHDEMO,
	[IEEE80211_M_HOSTAP]	= IEEE80211_C_HOSTAP,
	[IEEE80211_M_MONITOR]	= IEEE80211_C_MONITOR,
#ifdef IEEE80211_SUPPORT_MESH
	[IEEE80211_M_MBSS]	= IEEE80211_C_MBSS,
#endif
};

const uint8_t ieee80211broadcastaddr[IEEE80211_ADDR_LEN] =
	{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static	void ieee80211_syncflag_locked(struct ieee80211com *ic, int flag);
static	void ieee80211_syncflag_ht_locked(struct ieee80211com *ic, int flag);
static	void ieee80211_syncflag_ext_locked(struct ieee80211com *ic, int flag);
static	int ieee80211_media_setup(struct ieee80211com *ic,
		struct ifmedia *media, int caps, int addsta,
		ifm_change_cb_t media_change, ifm_stat_cb_t media_stat);
static	void ieee80211com_media_status(struct ifnet *, struct ifmediareq *);
static	int ieee80211com_media_change(struct ifnet *);
static	int media_status(enum ieee80211_opmode,
		const struct ieee80211_channel *);

MALLOC_DEFINE(M_80211_VAP, "80211vap", "802.11 vap state");

/*
 * Default supported rates for 802.11 operation (in IEEE .5Mb units).
 */
#define	B(r)	((r) | IEEE80211_RATE_BASIC)
static const struct ieee80211_rateset ieee80211_rateset_11a =
	{ 8, { B(12), 18, B(24), 36, B(48), 72, 96, 108 } };
static const struct ieee80211_rateset ieee80211_rateset_half =
	{ 8, { B(6), 9, B(12), 18, B(24), 36, 48, 54 } };
static const struct ieee80211_rateset ieee80211_rateset_quarter =
	{ 8, { B(3), 4, B(6), 9, B(12), 18, 24, 27 } };
static const struct ieee80211_rateset ieee80211_rateset_11b =
	{ 4, { B(2), B(4), B(11), B(22) } };
/* NB: OFDM rates are handled specially based on mode */
static const struct ieee80211_rateset ieee80211_rateset_11g =
	{ 12, { B(2), B(4), B(11), B(22), 12, 18, 24, 36, 48, 72, 96, 108 } };
#undef B

/*
 * Fill in 802.11 available channel set, mark
 * all available channels as active, and pick
 * a default channel if not already specified.
 */
static void
ieee80211_chan_init(struct ieee80211com *ic)
{
#define	DEFAULTRATES(m, def) do { \
	if (ic->ic_sup_rates[m].rs_nrates == 0) \
		ic->ic_sup_rates[m] = def; \
} while (0)
	struct ieee80211_channel *c;
	int i;

	KASSERT(0 < ic->ic_nchans && ic->ic_nchans <= IEEE80211_CHAN_MAX,
		("invalid number of channels specified: %u", ic->ic_nchans));
	memset(ic->ic_chan_avail, 0, sizeof(ic->ic_chan_avail));
	memset(ic->ic_modecaps, 0, sizeof(ic->ic_modecaps));
	setbit(ic->ic_modecaps, IEEE80211_MODE_AUTO);
	for (i = 0; i < ic->ic_nchans; i++) {
		c = &ic->ic_channels[i];
		KASSERT(c->ic_flags != 0, ("channel with no flags"));
		/*
		 * Help drivers that work only with frequencies by filling
		 * in IEEE channel #'s if not already calculated.  Note this
		 * mimics similar work done in ieee80211_setregdomain when
		 * changing regulatory state.
		 */
		if (c->ic_ieee == 0)
			c->ic_ieee = ieee80211_mhz2ieee(c->ic_freq,c->ic_flags);
		if (IEEE80211_IS_CHAN_HT40(c) && c->ic_extieee == 0)
			c->ic_extieee = ieee80211_mhz2ieee(c->ic_freq +
			    (IEEE80211_IS_CHAN_HT40U(c) ? 20 : -20),
			    c->ic_flags);
		/* default max tx power to max regulatory */
		if (c->ic_maxpower == 0)
			c->ic_maxpower = 2*c->ic_maxregpower;
		setbit(ic->ic_chan_avail, c->ic_ieee);
		/*
		 * Identify mode capabilities.
		 */
		if (IEEE80211_IS_CHAN_A(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_11A);
		if (IEEE80211_IS_CHAN_B(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_11B);
		if (IEEE80211_IS_CHAN_ANYG(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_11G);
		if (IEEE80211_IS_CHAN_FHSS(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_FH);
		if (IEEE80211_IS_CHAN_108A(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_TURBO_A);
		if (IEEE80211_IS_CHAN_108G(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_TURBO_G);
		if (IEEE80211_IS_CHAN_ST(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_STURBO_A);
		if (IEEE80211_IS_CHAN_HALF(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_HALF);
		if (IEEE80211_IS_CHAN_QUARTER(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_QUARTER);
		if (IEEE80211_IS_CHAN_HTA(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_11NA);
		if (IEEE80211_IS_CHAN_HTG(c))
			setbit(ic->ic_modecaps, IEEE80211_MODE_11NG);
	}
	/* initialize candidate channels to all available */
	memcpy(ic->ic_chan_active, ic->ic_chan_avail,
		sizeof(ic->ic_chan_avail));

	/* sort channel table to allow lookup optimizations */
	ieee80211_sort_channels(ic->ic_channels, ic->ic_nchans);

	/* invalidate any previous state */
	ic->ic_bsschan = IEEE80211_CHAN_ANYC;
	ic->ic_prevchan = NULL;
	ic->ic_csa_newchan = NULL;
	/* arbitrarily pick the first channel */
	ic->ic_curchan = &ic->ic_channels[0];
	ic->ic_rt = ieee80211_get_ratetable(ic->ic_curchan);

	/* fillin well-known rate sets if driver has not specified */
	DEFAULTRATES(IEEE80211_MODE_11B,	 ieee80211_rateset_11b);
	DEFAULTRATES(IEEE80211_MODE_11G,	 ieee80211_rateset_11g);
	DEFAULTRATES(IEEE80211_MODE_11A,	 ieee80211_rateset_11a);
	DEFAULTRATES(IEEE80211_MODE_TURBO_A,	 ieee80211_rateset_11a);
	DEFAULTRATES(IEEE80211_MODE_TURBO_G,	 ieee80211_rateset_11g);
	DEFAULTRATES(IEEE80211_MODE_STURBO_A,	 ieee80211_rateset_11a);
	DEFAULTRATES(IEEE80211_MODE_HALF,	 ieee80211_rateset_half);
	DEFAULTRATES(IEEE80211_MODE_QUARTER,	 ieee80211_rateset_quarter);
	DEFAULTRATES(IEEE80211_MODE_11NA,	 ieee80211_rateset_11a);
	DEFAULTRATES(IEEE80211_MODE_11NG,	 ieee80211_rateset_11g);

	/*
	 * Setup required information to fill the mcsset field, if driver did
	 * not. Assume a 2T2R setup for historic reasons.
	 */
	if (ic->ic_rxstream == 0)
		ic->ic_rxstream = 2;
	if (ic->ic_txstream == 0)
		ic->ic_txstream = 2;

	/*
	 * Set auto mode to reset active channel state and any desired channel.
	 */
	(void) ieee80211_setmode(ic, IEEE80211_MODE_AUTO);
#undef DEFAULTRATES
}

static void
null_update_mcast(struct ifnet *ifp)
{
	if_printf(ifp, "need multicast update callback\n");
}

static void
null_update_promisc(struct ifnet *ifp)
{
	if_printf(ifp, "need promiscuous mode update callback\n");
}

static int
null_transmit(struct ifnet *ifp, struct mbuf *m)
{
	m_freem(m);
	IFNET_STAT_INC(ifp, oerrors, 1);
	return EACCES;		/* XXX EIO/EPERM? */
}

#if defined(__DragonFly__)
static int
null_output(struct ifnet *ifp, struct mbuf *m,
	    struct sockaddr *dst, struct rtentry *ro)
#elif __FreeBSD_version >= 1000031
static int
null_output(struct ifnet *ifp, struct mbuf *m,
	const struct sockaddr *dst, struct route *ro)
#else
static int
null_output(struct ifnet *ifp, struct mbuf *m,
	struct sockaddr *dst, struct route *ro)
#endif
{
	if_printf(ifp, "discard raw packet\n");
	return null_transmit(ifp, m);
}

#if defined(__DragonFly__)

static void
null_input(struct ifnet *ifp, struct mbuf *m,
	   const struct pktinfo *pi, int cpuid)
{
	if_printf(ifp, "if_input should not be called\n");
	m_freem(m);
}

#else

static void
null_input(struct ifnet *ifp, struct mbuf *m)
{
	if_printf(ifp, "if_input should not be called\n");
	m_freem(m);
}

#endif

static void
null_update_chw(struct ieee80211com *ic)
{

	if_printf(ic->ic_ifp, "%s: need callback\n", __func__);
}

/*
 * Attach/setup the common net80211 state.  Called by
 * the driver on attach to prior to creating any vap's.
 */
void
ieee80211_ifattach(struct ieee80211com *ic,
	const uint8_t macaddr[IEEE80211_ADDR_LEN])
{
	struct ifnet *ifp = ic->ic_ifp;
	struct sockaddr_dl *sdl;
	struct ifaddr *ifa;

	KASSERT(ifp->if_type == IFT_IEEE80211, ("if_type %d", ifp->if_type));

	IEEE80211_LOCK_INIT(ic, ifp->if_xname);
	IEEE80211_TX_LOCK_INIT(ic, ifp->if_xname);
	TAILQ_INIT(&ic->ic_vaps);

	/* Create a taskqueue for all state changes */
	ic->ic_tq = taskqueue_create("ic_taskq", M_WAITOK | M_ZERO,
	    taskqueue_thread_enqueue, &ic->ic_tq);
#if defined(__DragonFly__)
	taskqueue_start_threads(&ic->ic_tq, 1, TDPRI_KERN_DAEMON, -1,
				"%s net80211 taskq", ifp->if_xname);
#else
	taskqueue_start_threads(&ic->ic_tq, 1, PI_NET, "%s net80211 taskq",
	    ifp->if_xname);
#endif
	/*
	 * Fill in 802.11 available channel set, mark all
	 * available channels as active, and pick a default
	 * channel if not already specified.
	 */
	ieee80211_media_init(ic);

	ic->ic_update_mcast = null_update_mcast;
	ic->ic_update_promisc = null_update_promisc;
	ic->ic_update_chw = null_update_chw;

	ic->ic_hash_key = arc4random();
	ic->ic_bintval = IEEE80211_BINTVAL_DEFAULT;
	ic->ic_lintval = ic->ic_bintval;
	ic->ic_txpowlimit = IEEE80211_TXPOWER_MAX;

	ieee80211_crypto_attach(ic);
	ieee80211_node_attach(ic);
	ieee80211_power_attach(ic);
	ieee80211_proto_attach(ic);
#ifdef IEEE80211_SUPPORT_SUPERG
	ieee80211_superg_attach(ic);
#endif
	ieee80211_ht_attach(ic);
	ieee80211_scan_attach(ic);
	ieee80211_regdomain_attach(ic);
	ieee80211_dfs_attach(ic);

	ieee80211_sysctl_attach(ic);

	ifp->if_addrlen = IEEE80211_ADDR_LEN;
	ifp->if_hdrlen = 0;

	/*
	 * If driver does not configure # of mbuf clusters/jclusters
	 * that could sit on the device queues for quite some time,
	 * we then assume:
	 * - The device queues only consume mbuf clusters.
	 * - No more than ieee80211_nmbclusters_default (by default
	 *   128) mbuf clusters will sit on the device queues for
	 *   quite some time.
	 */
	if (ifp->if_nmbclusters <= 0 && ifp->if_nmbjclusters <= 0) {
		if (ieee80211_nmbclusters_default <
		    IEEE80211_NMBCLUSTERS_DEFMIN) {
			kprintf("ieee80211 nmbclusters %d -> %d\n",
			    ieee80211_nmbclusters_default,
			    IEEE80211_NMBCLUSTERS_DEFAULT);
			ieee80211_nmbclusters_default =
			    IEEE80211_NMBCLUSTERS_DEFAULT;
		}
		ifp->if_nmbclusters = ieee80211_nmbclusters_default;
	}

	CURVNET_SET(vnet0);

	/*
	 * This function must _not_ be serialized by the WLAN serializer,
	 * since it could dead-lock the domsg to netisrs in if_attach().
	 */
	wlan_serialize_exit();
#if defined(__DragonFly__)
	if_attach(ifp, &wlan_global_serializer);
#else
	if_attach(ifp);
#endif
	wlan_serialize_enter();

	ifp->if_mtu = IEEE80211_MTU_MAX;
	ifp->if_broadcastaddr = ieee80211broadcastaddr;
	ifp->if_output = null_output;
	ifp->if_input = null_input;	/* just in case */
	ifp->if_resolvemulti = NULL;	/* NB: callers check */

	ifa = TAILQ_FIRST(&ifp->if_addrheads[mycpuid])->ifa;
	sdl = (struct sockaddr_dl *)ifa->ifa_addr;
	sdl->sdl_type = IFT_ETHER;		/* XXX IFT_IEEE80211? */
	sdl->sdl_alen = IEEE80211_ADDR_LEN;
	IEEE80211_ADDR_COPY(LLADDR(sdl), macaddr);

	CURVNET_RESTORE();
}

/*
 * Detach net80211 state on device detach.  Tear down
 * all vap's and reclaim all common state prior to the
 * device state going away.  Note we may call back into
 * driver; it must be prepared for this.
 */
void
ieee80211_ifdetach(struct ieee80211com *ic)
{
	struct ifnet *ifp = ic->ic_ifp;
	struct ieee80211vap *vap;

	/*
	 * The VAP is responsible for setting and clearing
	 * the VIMAGE context.
	 */
	while ((vap = TAILQ_FIRST(&ic->ic_vaps)) != NULL)
		ieee80211_vap_destroy(vap);

	/*
	 * WLAN serializer must _not_ be held for if_detach(),
	 * since it could dead-lock the domsg to netisrs.
	 *
	 * XXX
	 * This function actually should _not_ be serialized
	 * by the WLAN serializer, however, all 802.11 device
	 * drivers serialize it ...
	 */
	wlan_serialize_exit();

	/*
	 * This detaches the main interface, but not the vaps.
	 * Each VAP may be in a separate VIMAGE.
	 *
	 * Detach the main interface _after_ all vaps are
	 * destroyed, since the main interface is referenced
	 * on vaps' detach path.
	 */
	CURVNET_SET(ifp->if_vnet);
	if_detach(ifp);
	CURVNET_RESTORE();

	/* Re-hold WLAN serializer */
	wlan_serialize_enter();

	ieee80211_waitfor_parent(ic);

	ieee80211_sysctl_detach(ic);
	ieee80211_dfs_detach(ic);
	ieee80211_regdomain_detach(ic);
	ieee80211_scan_detach(ic);
#ifdef IEEE80211_SUPPORT_SUPERG
	ieee80211_superg_detach(ic);
#endif
	ieee80211_ht_detach(ic);
	/* NB: must be called before ieee80211_node_detach */
	ieee80211_proto_detach(ic);
	ieee80211_crypto_detach(ic);
	ieee80211_power_detach(ic);
	ieee80211_node_detach(ic);

	/* XXX VNET needed? */
	ifmedia_removeall(&ic->ic_media);

	taskqueue_free(ic->ic_tq);
	IEEE80211_TX_LOCK_DESTROY(ic);
	IEEE80211_LOCK_DESTROY(ic);
}

/*
 * Default reset method for use with the ioctl support.  This
 * method is invoked after any state change in the 802.11
 * layer that should be propagated to the hardware but not
 * require re-initialization of the 802.11 state machine (e.g
 * rescanning for an ap).  We always return ENETRESET which
 * should cause the driver to re-initialize the device. Drivers
 * can override this method to implement more optimized support.
 */
static int
default_reset(struct ieee80211vap *vap, u_long cmd)
{
	return ENETRESET;
}

/*
 * Prepare a vap for use.  Drivers use this call to
 * setup net80211 state in new vap's prior attaching
 * them with ieee80211_vap_attach (below).
 */
int
ieee80211_vap_setup(struct ieee80211com *ic, struct ieee80211vap *vap,
    const char name[IFNAMSIZ], int unit, enum ieee80211_opmode opmode,
    int flags, const uint8_t bssid[IEEE80211_ADDR_LEN],
    const uint8_t macaddr[IEEE80211_ADDR_LEN])
{
	struct ifnet *ifp;

	ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		if_printf(ic->ic_ifp, "%s: unable to allocate ifnet\n",
		    __func__);
		return ENOMEM;
	}
	if_initname(ifp, name, unit);
	ifp->if_softc = vap;			/* back pointer */
	ifp->if_flags = IFF_SIMPLEX | IFF_BROADCAST | IFF_MULTICAST;
	ifp->if_start = ieee80211_vap_start;
#if 0
	ifp->if_transmit = ieee80211_vap_transmit;
	ifp->if_qflush = ieee80211_vap_qflush;
#endif
	ifp->if_ioctl = ieee80211_ioctl;
	ifp->if_init = ieee80211_init;

	vap->iv_ifp = ifp;
	vap->iv_ic = ic;
	vap->iv_flags = ic->ic_flags;		/* propagate common flags */
	vap->iv_flags_ext = ic->ic_flags_ext;
	vap->iv_flags_ven = ic->ic_flags_ven;
	vap->iv_caps = ic->ic_caps &~ IEEE80211_C_OPMODE;
	vap->iv_htcaps = ic->ic_htcaps;
	vap->iv_htextcaps = ic->ic_htextcaps;
	vap->iv_opmode = opmode;
	vap->iv_caps |= ieee80211_opcap[opmode];
	switch (opmode) {
	case IEEE80211_M_WDS:
		/*
		 * WDS links must specify the bssid of the far end.
		 * For legacy operation this is a static relationship.
		 * For non-legacy operation the station must associate
		 * and be authorized to pass traffic.  Plumbing the
		 * vap to the proper node happens when the vap
		 * transitions to RUN state.
		 */
		IEEE80211_ADDR_COPY(vap->iv_des_bssid, bssid);
		vap->iv_flags |= IEEE80211_F_DESBSSID;
		if (flags & IEEE80211_CLONE_WDSLEGACY)
			vap->iv_flags_ext |= IEEE80211_FEXT_WDSLEGACY;
		break;
#ifdef IEEE80211_SUPPORT_TDMA
	case IEEE80211_M_AHDEMO:
		if (flags & IEEE80211_CLONE_TDMA) {
			/* NB: checked before clone operation allowed */
			KASSERT(ic->ic_caps & IEEE80211_C_TDMA,
			    ("not TDMA capable, ic_caps 0x%x", ic->ic_caps));
			/*
			 * Propagate TDMA capability to mark vap; this
			 * cannot be removed and is used to distinguish
			 * regular ahdemo operation from ahdemo+tdma.
			 */
			vap->iv_caps |= IEEE80211_C_TDMA;
		}
		break;
#endif
	default:
		break;
	}
	/* auto-enable s/w beacon miss support */
	if (flags & IEEE80211_CLONE_NOBEACONS)
		vap->iv_flags_ext |= IEEE80211_FEXT_SWBMISS;
	/* auto-generated or user supplied MAC address */
	if (flags & (IEEE80211_CLONE_BSSID|IEEE80211_CLONE_MACADDR))
		vap->iv_flags_ext |= IEEE80211_FEXT_UNIQMAC;
	/*
	 * Enable various functionality by default if we're
	 * capable; the driver can override us if it knows better.
	 */
	if (vap->iv_caps & IEEE80211_C_WME)
		vap->iv_flags |= IEEE80211_F_WME;
	if (vap->iv_caps & IEEE80211_C_BURST)
		vap->iv_flags |= IEEE80211_F_BURST;
	/* NB: bg scanning only makes sense for station mode right now */
#if 0
	/*
	 * DISABLE BGSCAN BY DEFAULT, many issues can crop up including
	 * the link going dead.
	 */
	if (vap->iv_opmode == IEEE80211_M_STA &&
	    (vap->iv_caps & IEEE80211_C_BGSCAN))
		vap->iv_flags |= IEEE80211_F_BGSCAN;
#endif
	vap->iv_flags |= IEEE80211_F_DOTH;	/* XXX no cap, just ena */
	/* NB: DFS support only makes sense for ap mode right now */
	if (vap->iv_opmode == IEEE80211_M_HOSTAP &&
	    (vap->iv_caps & IEEE80211_C_DFS))
		vap->iv_flags_ext |= IEEE80211_FEXT_DFS;

	vap->iv_des_chan = IEEE80211_CHAN_ANYC;		/* any channel is ok */
	vap->iv_bmissthreshold = IEEE80211_HWBMISS_DEFAULT;
	vap->iv_dtim_period = IEEE80211_DTIM_DEFAULT;
	/*
	 * Install a default reset method for the ioctl support;
	 * the driver can override this.
	 */
	vap->iv_reset = default_reset;

	IEEE80211_ADDR_COPY(vap->iv_myaddr, macaddr);

	ieee80211_sysctl_vattach(vap);
	ieee80211_crypto_vattach(vap);
	ieee80211_node_vattach(vap);
	ieee80211_power_vattach(vap);
	ieee80211_proto_vattach(vap);
#ifdef IEEE80211_SUPPORT_SUPERG
	ieee80211_superg_vattach(vap);
#endif
	ieee80211_ht_vattach(vap);
	ieee80211_scan_vattach(vap);
	ieee80211_regdomain_vattach(vap);
	ieee80211_radiotap_vattach(vap);
	ieee80211_ratectl_set(vap, IEEE80211_RATECTL_NONE);

	return 0;
}

/*
 * Activate a vap.  State should have been prepared with a
 * call to ieee80211_vap_setup and by the driver.  On return
 * from this call the vap is ready for use.
 */
int
ieee80211_vap_attach(struct ieee80211vap *vap,
	ifm_change_cb_t media_change, ifm_stat_cb_t media_stat)
{
	struct ifnet *ifp = vap->iv_ifp;
	struct ieee80211com *ic = vap->iv_ic;
	struct ifmediareq imr;
	int maxrate;

	/*
	 * This function must _not_ be serialized by the WLAN serializer,
	 * since it could dead-lock the domsg to netisrs in ether_ifattach().
	 */
	wlan_assert_notserialized();

	IEEE80211_DPRINTF(vap, IEEE80211_MSG_STATE,
	    "%s: %s parent %s flags 0x%x flags_ext 0x%x\n",
	    __func__, ieee80211_opmode_name[vap->iv_opmode],
	    ic->ic_ifp->if_xname, vap->iv_flags, vap->iv_flags_ext);

	/*
	 * Do late attach work that cannot happen until after
	 * the driver has had a chance to override defaults.
	 */
	ieee80211_node_latevattach(vap);
	ieee80211_power_latevattach(vap);

	maxrate = ieee80211_media_setup(ic, &vap->iv_media, vap->iv_caps,
	    vap->iv_opmode == IEEE80211_M_STA, media_change, media_stat);
	ieee80211_media_status(ifp, &imr);
	/* NB: strip explicit mode; we're actually in autoselect */
	ifmedia_set(&vap->iv_media,
	    imr.ifm_active &~ (IFM_MMASK | IFM_IEEE80211_TURBO));
	if (maxrate)
		ifp->if_baudrate = IF_Mbps(maxrate);

#if defined(__DragonFly__)
	ether_ifattach(ifp, vap->iv_myaddr, &wlan_global_serializer);
#else
	ether_ifattach(ifp, vap->iv_myaddr);
#endif
	/* hook output method setup by ether_ifattach */
	vap->iv_output = ifp->if_output;
	ifp->if_output = ieee80211_output;
	/* NB: if_mtu set by ether_ifattach to ETHERMTU */

	IEEE80211_LOCK(ic);
	TAILQ_INSERT_TAIL(&ic->ic_vaps, vap, iv_next);
	ieee80211_syncflag_locked(ic, IEEE80211_F_WME);
#ifdef IEEE80211_SUPPORT_SUPERG
	ieee80211_syncflag_locked(ic, IEEE80211_F_TURBOP);
#endif
	ieee80211_syncflag_locked(ic, IEEE80211_F_PCF);
	ieee80211_syncflag_locked(ic, IEEE80211_F_BURST);
	ieee80211_syncflag_ht_locked(ic, IEEE80211_FHT_HT);
	ieee80211_syncflag_ht_locked(ic, IEEE80211_FHT_USEHT40);
	ieee80211_syncifflag_locked(ic, IFF_PROMISC);
	ieee80211_syncifflag_locked(ic, IFF_ALLMULTI);
	IEEE80211_UNLOCK(ic);

	return 1;
}

/* 
 * Tear down vap state and reclaim the ifnet.
 * The driver is assumed to have prepared for
 * this; e.g. by turning off interrupts for the
 * underlying device.
 */
void
ieee80211_vap_detach(struct ieee80211vap *vap)
{
	struct ieee80211com *ic = vap->iv_ic;
	struct ifnet *ifp = vap->iv_ifp;

	/*
	 * This function must _not_ be serialized by the WLAN serializer,
	 * since it could dead-lock the domsg to netisrs in ether_ifdettach().
	 */
	wlan_assert_notserialized();

	CURVNET_SET(ifp->if_vnet);

	IEEE80211_DPRINTF(vap, IEEE80211_MSG_STATE, "%s: %s parent %s\n",
	    __func__, ieee80211_opmode_name[vap->iv_opmode],
	    ic->ic_ifp->if_xname);

	/* NB: bpfdetach is called by ether_ifdetach and claims all taps */
	ether_ifdetach(ifp);

	ieee80211_stop(vap);

	/*
	 * Flush any deferred vap tasks.
	 */
	ieee80211_draintask(ic, &vap->iv_nstate_task);
	ieee80211_draintask(ic, &vap->iv_swbmiss_task);

#if !defined(__DragonFly__)
	/* XXX band-aid until ifnet handles this for us */
	taskqueue_drain(taskqueue_swi, &ifp->if_linktask);
#endif

	IEEE80211_LOCK(ic);
	KASSERT(vap->iv_state == IEEE80211_S_INIT , ("vap still running"));
	TAILQ_REMOVE(&ic->ic_vaps, vap, iv_next);
	ieee80211_syncflag_locked(ic, IEEE80211_F_WME);
#ifdef IEEE80211_SUPPORT_SUPERG
	ieee80211_syncflag_locked(ic, IEEE80211_F_TURBOP);
#endif
	ieee80211_syncflag_locked(ic, IEEE80211_F_PCF);
	ieee80211_syncflag_locked(ic, IEEE80211_F_BURST);
	ieee80211_syncflag_ht_locked(ic, IEEE80211_FHT_HT);
	ieee80211_syncflag_ht_locked(ic, IEEE80211_FHT_USEHT40);
	/* NB: this handles the bpfdetach done below */
	ieee80211_syncflag_ext_locked(ic, IEEE80211_FEXT_BPF);
	ieee80211_syncifflag_locked(ic, IFF_PROMISC);
	ieee80211_syncifflag_locked(ic, IFF_ALLMULTI);
	IEEE80211_UNLOCK(ic);

	ifmedia_removeall(&vap->iv_media);

	ieee80211_radiotap_vdetach(vap);
	ieee80211_regdomain_vdetach(vap);
	ieee80211_scan_vdetach(vap);
#ifdef IEEE80211_SUPPORT_SUPERG
	ieee80211_superg_vdetach(vap);
#endif
	ieee80211_ht_vdetach(vap);
	/* NB: must be before ieee80211_node_vdetach */
	ieee80211_proto_vdetach(vap);
	ieee80211_crypto_vdetach(vap);
	ieee80211_power_vdetach(vap);
	ieee80211_node_vdetach(vap);
	ieee80211_sysctl_vdetach(vap);

	if_free(ifp);

	CURVNET_RESTORE();
}

/*
 * Synchronize flag bit state in the parent ifnet structure
 * according to the state of all vap ifnet's.  This is used,
 * for example, to handle IFF_PROMISC and IFF_ALLMULTI.
 */
void
ieee80211_syncifflag_locked(struct ieee80211com *ic, int flag)
{
	struct ifnet *ifp = ic->ic_ifp;
	struct ieee80211vap *vap;
	int bit, oflags;

	IEEE80211_LOCK_ASSERT(ic);

	bit = 0;
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next)
		if (vap->iv_ifp->if_flags & flag) {
			/*
			 * XXX the bridge sets PROMISC but we don't want to
			 * enable it on the device, discard here so all the
			 * drivers don't need to special-case it
			 */
			if (flag == IFF_PROMISC &&
			    !(vap->iv_opmode == IEEE80211_M_MONITOR ||
			      (vap->iv_opmode == IEEE80211_M_AHDEMO &&
			       (vap->iv_caps & IEEE80211_C_TDMA) == 0)))
				continue;
			bit = 1;
			break;
		}
	oflags = ifp->if_flags;
	if (bit)
		ifp->if_flags |= flag;
	else
		ifp->if_flags &= ~flag;
	if ((ifp->if_flags ^ oflags) & flag) {
		/* XXX should we return 1/0 and let caller do this? */
		if (ifp->if_drv_flags & IFF_DRV_RUNNING) {
			if (flag == IFF_PROMISC)
				ieee80211_runtask(ic, &ic->ic_promisc_task);
			else if (flag == IFF_ALLMULTI)
				ieee80211_runtask(ic, &ic->ic_mcast_task);
		}
	}
}

/*
 * Synchronize flag bit state in the com structure
 * according to the state of all vap's.  This is used,
 * for example, to handle state changes via ioctls.
 */
static void
ieee80211_syncflag_locked(struct ieee80211com *ic, int flag)
{
	struct ieee80211vap *vap;
	int bit;

	IEEE80211_LOCK_ASSERT(ic);

	bit = 0;
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next)
		if (vap->iv_flags & flag) {
			bit = 1;
			break;
		}
	if (bit)
		ic->ic_flags |= flag;
	else
		ic->ic_flags &= ~flag;
}

void
ieee80211_syncflag(struct ieee80211vap *vap, int flag)
{
	struct ieee80211com *ic = vap->iv_ic;

	IEEE80211_LOCK(ic);
	if (flag < 0) {
		flag = -flag;
		vap->iv_flags &= ~flag;
	} else
		vap->iv_flags |= flag;
	ieee80211_syncflag_locked(ic, flag);
	IEEE80211_UNLOCK(ic);
}

/*
 * Synchronize flags_ht bit state in the com structure
 * according to the state of all vap's.  This is used,
 * for example, to handle state changes via ioctls.
 */
static void
ieee80211_syncflag_ht_locked(struct ieee80211com *ic, int flag)
{
	struct ieee80211vap *vap;
	int bit;

	IEEE80211_LOCK_ASSERT(ic);

	bit = 0;
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next)
		if (vap->iv_flags_ht & flag) {
			bit = 1;
			break;
		}
	if (bit)
		ic->ic_flags_ht |= flag;
	else
		ic->ic_flags_ht &= ~flag;
}

void
ieee80211_syncflag_ht(struct ieee80211vap *vap, int flag)
{
	struct ieee80211com *ic = vap->iv_ic;

	IEEE80211_LOCK(ic);
	if (flag < 0) {
		flag = -flag;
		vap->iv_flags_ht &= ~flag;
	} else
		vap->iv_flags_ht |= flag;
	ieee80211_syncflag_ht_locked(ic, flag);
	IEEE80211_UNLOCK(ic);
}

/*
 * Synchronize flags_ext bit state in the com structure
 * according to the state of all vap's.  This is used,
 * for example, to handle state changes via ioctls.
 */
static void
ieee80211_syncflag_ext_locked(struct ieee80211com *ic, int flag)
{
	struct ieee80211vap *vap;
	int bit;

	IEEE80211_LOCK_ASSERT(ic);

	bit = 0;
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next)
		if (vap->iv_flags_ext & flag) {
			bit = 1;
			break;
		}
	if (bit)
		ic->ic_flags_ext |= flag;
	else
		ic->ic_flags_ext &= ~flag;
}

void
ieee80211_syncflag_ext(struct ieee80211vap *vap, int flag)
{
	struct ieee80211com *ic = vap->iv_ic;

	IEEE80211_LOCK(ic);
	if (flag < 0) {
		flag = -flag;
		vap->iv_flags_ext &= ~flag;
	} else
		vap->iv_flags_ext |= flag;
	ieee80211_syncflag_ext_locked(ic, flag);
	IEEE80211_UNLOCK(ic);
}

static __inline int
mapgsm(u_int freq, u_int flags)
{
	freq *= 10;
	if (flags & IEEE80211_CHAN_QUARTER)
		freq += 5;
	else if (flags & IEEE80211_CHAN_HALF)
		freq += 10;
	else
		freq += 20;
	/* NB: there is no 907/20 wide but leave room */
	return (freq - 906*10) / 5;
}

static __inline int
mappsb(u_int freq, u_int flags)
{
	return 37 + ((freq * 10) + ((freq % 5) == 2 ? 5 : 0) - 49400) / 5;
}

/*
 * Convert MHz frequency to IEEE channel number.
 */
int
ieee80211_mhz2ieee(u_int freq, u_int flags)
{
#define	IS_FREQ_IN_PSB(_freq) ((_freq) > 4940 && (_freq) < 4990)
	if (flags & IEEE80211_CHAN_GSM)
		return mapgsm(freq, flags);
	if (flags & IEEE80211_CHAN_2GHZ) {	/* 2GHz band */
		if (freq == 2484)
			return 14;
		if (freq < 2484)
			return ((int) freq - 2407) / 5;
		else
			return 15 + ((freq - 2512) / 20);
	} else if (flags & IEEE80211_CHAN_5GHZ) {	/* 5Ghz band */
		if (freq <= 5000) {
			/* XXX check regdomain? */
			if (IS_FREQ_IN_PSB(freq))
				return mappsb(freq, flags);
			return (freq - 4000) / 5;
		} else
			return (freq - 5000) / 5;
	} else {				/* either, guess */
		if (freq == 2484)
			return 14;
		if (freq < 2484) {
			if (907 <= freq && freq <= 922)
				return mapgsm(freq, flags);
			return ((int) freq - 2407) / 5;
		}
		if (freq < 5000) {
			if (IS_FREQ_IN_PSB(freq))
				return mappsb(freq, flags);
			else if (freq > 4900)
				return (freq - 4000) / 5;
			else
				return 15 + ((freq - 2512) / 20);
		}
		return (freq - 5000) / 5;
	}
#undef IS_FREQ_IN_PSB
}

/*
 * Convert channel to IEEE channel number.
 */
int
ieee80211_chan2ieee(struct ieee80211com *ic, const struct ieee80211_channel *c)
{
	if (c == NULL) {
		if_printf(ic->ic_ifp, "invalid channel (NULL)\n");
		return 0;		/* XXX */
	}
	return (c == IEEE80211_CHAN_ANYC ?  IEEE80211_CHAN_ANY : c->ic_ieee);
}

/*
 * Convert IEEE channel number to MHz frequency.
 */
u_int
ieee80211_ieee2mhz(u_int chan, u_int flags)
{
	if (flags & IEEE80211_CHAN_GSM)
		return 907 + 5 * (chan / 10);
	if (flags & IEEE80211_CHAN_2GHZ) {	/* 2GHz band */
		if (chan == 14)
			return 2484;
		if (chan < 14)
			return 2407 + chan*5;
		else
			return 2512 + ((chan-15)*20);
	} else if (flags & IEEE80211_CHAN_5GHZ) {/* 5Ghz band */
		if (flags & (IEEE80211_CHAN_HALF|IEEE80211_CHAN_QUARTER)) {
			chan -= 37;
			return 4940 + chan*5 + (chan % 5 ? 2 : 0);
		}
		return 5000 + (chan*5);
	} else {				/* either, guess */
		/* XXX can't distinguish PSB+GSM channels */
		if (chan == 14)
			return 2484;
		if (chan < 14)			/* 0-13 */
			return 2407 + chan*5;
		if (chan < 27)			/* 15-26 */
			return 2512 + ((chan-15)*20);
		return 5000 + (chan*5);
	}
}

/*
 * Locate a channel given a frequency+flags.  We cache
 * the previous lookup to optimize switching between two
 * channels--as happens with dynamic turbo.
 */
struct ieee80211_channel *
ieee80211_find_channel(struct ieee80211com *ic, int freq, int flags)
{
	struct ieee80211_channel *c;
	int i;

	flags &= IEEE80211_CHAN_ALLTURBO;
	c = ic->ic_prevchan;
	if (c != NULL && c->ic_freq == freq &&
	    (c->ic_flags & IEEE80211_CHAN_ALLTURBO) == flags)
		return c;
	/* brute force search */
	for (i = 0; i < ic->ic_nchans; i++) {
		c = &ic->ic_channels[i];
		if (c->ic_freq == freq &&
		    (c->ic_flags & IEEE80211_CHAN_ALLTURBO) == flags)
			return c;
	}
	return NULL;
}

/*
 * Locate a channel given a channel number+flags.  We cache
 * the previous lookup to optimize switching between two
 * channels--as happens with dynamic turbo.
 */
struct ieee80211_channel *
ieee80211_find_channel_byieee(struct ieee80211com *ic, int ieee, int flags)
{
	struct ieee80211_channel *c;
	int i;

	flags &= IEEE80211_CHAN_ALLTURBO;
	c = ic->ic_prevchan;
	if (c != NULL && c->ic_ieee == ieee &&
	    (c->ic_flags & IEEE80211_CHAN_ALLTURBO) == flags)
		return c;
	/* brute force search */
	for (i = 0; i < ic->ic_nchans; i++) {
		c = &ic->ic_channels[i];
		if (c->ic_ieee == ieee &&
		    (c->ic_flags & IEEE80211_CHAN_ALLTURBO) == flags)
			return c;
	}
	return NULL;
}

static void
addmedia(struct ifmedia *media, int caps, int addsta, int mode, int mword)
{
#define	ADD(_ic, _s, _o) \
	ifmedia_add(media, \
		IFM_MAKEWORD(IFM_IEEE80211, (_s), (_o), 0), 0, NULL)
	static const u_int mopts[IEEE80211_MODE_MAX] = { 
	    [IEEE80211_MODE_AUTO]	= IFM_AUTO,
	    [IEEE80211_MODE_11A]	= IFM_IEEE80211_11A,
	    [IEEE80211_MODE_11B]	= IFM_IEEE80211_11B,
	    [IEEE80211_MODE_11G]	= IFM_IEEE80211_11G,
	    [IEEE80211_MODE_FH]		= IFM_IEEE80211_FH,
	    [IEEE80211_MODE_TURBO_A]	= IFM_IEEE80211_11A|IFM_IEEE80211_TURBO,
	    [IEEE80211_MODE_TURBO_G]	= IFM_IEEE80211_11G|IFM_IEEE80211_TURBO,
	    [IEEE80211_MODE_STURBO_A]	= IFM_IEEE80211_11A|IFM_IEEE80211_TURBO,
	    [IEEE80211_MODE_HALF]	= IFM_IEEE80211_11A,	/* XXX */
	    [IEEE80211_MODE_QUARTER]	= IFM_IEEE80211_11A,	/* XXX */
	    [IEEE80211_MODE_11NA]	= IFM_IEEE80211_11NA,
	    [IEEE80211_MODE_11NG]	= IFM_IEEE80211_11NG,
	};
	u_int mopt;

	mopt = mopts[mode];
	if (addsta)
		ADD(ic, mword, mopt);	/* STA mode has no cap */
	if (caps & IEEE80211_C_IBSS)
		ADD(media, mword, mopt | IFM_IEEE80211_ADHOC);
	if (caps & IEEE80211_C_HOSTAP)
		ADD(media, mword, mopt | IFM_IEEE80211_HOSTAP);
	if (caps & IEEE80211_C_AHDEMO)
		ADD(media, mword, mopt | IFM_IEEE80211_ADHOC | IFM_FLAG0);
	if (caps & IEEE80211_C_MONITOR)
		ADD(media, mword, mopt | IFM_IEEE80211_MONITOR);
	if (caps & IEEE80211_C_WDS)
		ADD(media, mword, mopt | IFM_IEEE80211_WDS);
	if (caps & IEEE80211_C_MBSS)
		ADD(media, mword, mopt | IFM_IEEE80211_MBSS);
#undef ADD
}

/*
 * Setup the media data structures according to the channel and
 * rate tables.
 */
static int
ieee80211_media_setup(struct ieee80211com *ic,
	struct ifmedia *media, int caps, int addsta,
	ifm_change_cb_t media_change, ifm_stat_cb_t media_stat)
{
	int i, j, rate, maxrate, mword, r;
	enum ieee80211_phymode mode;
	const struct ieee80211_rateset *rs;
	struct ieee80211_rateset allrates;

	/*
	 * Fill in media characteristics.
	 */
	ifmedia_init(media, 0, media_change, media_stat);
	maxrate = 0;
	/*
	 * Add media for legacy operating modes.
	 */
	memset(&allrates, 0, sizeof(allrates));
	for (mode = IEEE80211_MODE_AUTO; mode < IEEE80211_MODE_11NA; mode++) {
		if (isclr(ic->ic_modecaps, mode))
			continue;
		addmedia(media, caps, addsta, mode, IFM_AUTO);
		if (mode == IEEE80211_MODE_AUTO)
			continue;
		rs = &ic->ic_sup_rates[mode];
		for (i = 0; i < rs->rs_nrates; i++) {
			rate = rs->rs_rates[i];
			mword = ieee80211_rate2media(ic, rate, mode);
			if (mword == 0)
				continue;
			addmedia(media, caps, addsta, mode, mword);
			/*
			 * Add legacy rate to the collection of all rates.
			 */
			r = rate & IEEE80211_RATE_VAL;
			for (j = 0; j < allrates.rs_nrates; j++)
				if (allrates.rs_rates[j] == r)
					break;
			if (j == allrates.rs_nrates) {
				/* unique, add to the set */
				allrates.rs_rates[j] = r;
				allrates.rs_nrates++;
			}
			rate = (rate & IEEE80211_RATE_VAL) / 2;
			if (rate > maxrate)
				maxrate = rate;
		}
	}
	for (i = 0; i < allrates.rs_nrates; i++) {
		mword = ieee80211_rate2media(ic, allrates.rs_rates[i],
				IEEE80211_MODE_AUTO);
		if (mword == 0)
			continue;
		/* NB: remove media options from mword */
		addmedia(media, caps, addsta,
		    IEEE80211_MODE_AUTO, IFM_SUBTYPE(mword));
	}
	/*
	 * Add HT/11n media.  Note that we do not have enough
	 * bits in the media subtype to express the MCS so we
	 * use a "placeholder" media subtype and any fixed MCS
	 * must be specified with a different mechanism.
	 */
	for (; mode <= IEEE80211_MODE_11NG; mode++) {
		if (isclr(ic->ic_modecaps, mode))
			continue;
		addmedia(media, caps, addsta, mode, IFM_AUTO);
		addmedia(media, caps, addsta, mode, IFM_IEEE80211_MCS);
	}
	if (isset(ic->ic_modecaps, IEEE80211_MODE_11NA) ||
	    isset(ic->ic_modecaps, IEEE80211_MODE_11NG)) {
		addmedia(media, caps, addsta,
		    IEEE80211_MODE_AUTO, IFM_IEEE80211_MCS);
		i = ic->ic_txstream * 8 - 1;
		if ((ic->ic_htcaps & IEEE80211_HTCAP_CHWIDTH40) &&
		    (ic->ic_htcaps & IEEE80211_HTCAP_SHORTGI40))
			rate = ieee80211_htrates[i].ht40_rate_400ns;
		else if ((ic->ic_htcaps & IEEE80211_HTCAP_CHWIDTH40))
			rate = ieee80211_htrates[i].ht40_rate_800ns;
		else if ((ic->ic_htcaps & IEEE80211_HTCAP_SHORTGI20))
			rate = ieee80211_htrates[i].ht20_rate_400ns;
		else
			rate = ieee80211_htrates[i].ht20_rate_800ns;
		if (rate > maxrate)
			maxrate = rate;
	}
	return maxrate;
}

void
ieee80211_media_init(struct ieee80211com *ic)
{
	struct ifnet *ifp = ic->ic_ifp;
	int maxrate;

	/* NB: this works because the structure is initialized to zero */
	if (!LIST_EMPTY(&ic->ic_media.ifm_list)) {
		/*
		 * We are re-initializing the channel list; clear
		 * the existing media state as the media routines
		 * don't suppress duplicates.
		 */
		ifmedia_removeall(&ic->ic_media);
	}
	ieee80211_chan_init(ic);

	/*
	 * Recalculate media settings in case new channel list changes
	 * the set of available modes.
	 */
	maxrate = ieee80211_media_setup(ic, &ic->ic_media, ic->ic_caps, 1,
		ieee80211com_media_change, ieee80211com_media_status);
	/* NB: strip explicit mode; we're actually in autoselect */
	ifmedia_set(&ic->ic_media,
	    media_status(ic->ic_opmode, ic->ic_curchan) &~
		(IFM_MMASK | IFM_IEEE80211_TURBO));
	if (maxrate)
		ifp->if_baudrate = IF_Mbps(maxrate);

	/* XXX need to propagate new media settings to vap's */
}

/* XXX inline or eliminate? */
const struct ieee80211_rateset *
ieee80211_get_suprates(struct ieee80211com *ic, const struct ieee80211_channel *c)
{
	/* XXX does this work for 11ng basic rates? */
	return &ic->ic_sup_rates[ieee80211_chan2mode(c)];
}

void
ieee80211_announce(struct ieee80211com *ic)
{
	struct ifnet *ifp = ic->ic_ifp;
	int i, rate, mword;
	enum ieee80211_phymode mode;
	const struct ieee80211_rateset *rs;

	/* NB: skip AUTO since it has no rates */
	for (mode = IEEE80211_MODE_AUTO+1; mode < IEEE80211_MODE_11NA; mode++) {
		if (isclr(ic->ic_modecaps, mode))
			continue;
		if_printf(ifp, "%s rates: ", ieee80211_phymode_name[mode]);
		rs = &ic->ic_sup_rates[mode];
		for (i = 0; i < rs->rs_nrates; i++) {
			mword = ieee80211_rate2media(ic, rs->rs_rates[i], mode);
			if (mword == 0)
				continue;
			rate = ieee80211_media2rate(mword);
			kprintf("%s%d%sMbps", (i != 0 ? " " : ""),
			    rate / 2, ((rate & 0x1) != 0 ? ".5" : ""));
		}
		kprintf("\n");
	}
	ieee80211_ht_announce(ic);
}

void
ieee80211_announce_channels(struct ieee80211com *ic)
{
	const struct ieee80211_channel *c;
	char type;
	int i, cw;

	kprintf("Chan  Freq  CW  RegPwr  MinPwr  MaxPwr\n");
	for (i = 0; i < ic->ic_nchans; i++) {
		c = &ic->ic_channels[i];
		if (IEEE80211_IS_CHAN_ST(c))
			type = 'S';
		else if (IEEE80211_IS_CHAN_108A(c))
			type = 'T';
		else if (IEEE80211_IS_CHAN_108G(c))
			type = 'G';
		else if (IEEE80211_IS_CHAN_HT(c))
			type = 'n';
		else if (IEEE80211_IS_CHAN_A(c))
			type = 'a';
		else if (IEEE80211_IS_CHAN_ANYG(c))
			type = 'g';
		else if (IEEE80211_IS_CHAN_B(c))
			type = 'b';
		else
			type = 'f';
		if (IEEE80211_IS_CHAN_HT40(c) || IEEE80211_IS_CHAN_TURBO(c))
			cw = 40;
		else if (IEEE80211_IS_CHAN_HALF(c))
			cw = 10;
		else if (IEEE80211_IS_CHAN_QUARTER(c))
			cw = 5;
		else
			cw = 20;
		kprintf("%4d  %4d%c %2d%c %6d  %4d.%d  %4d.%d\n"
			, c->ic_ieee, c->ic_freq, type
			, cw
			, IEEE80211_IS_CHAN_HT40U(c) ? '+' :
			  IEEE80211_IS_CHAN_HT40D(c) ? '-' : ' '
			, c->ic_maxregpower
			, c->ic_minpower / 2, c->ic_minpower & 1 ? 5 : 0
			, c->ic_maxpower / 2, c->ic_maxpower & 1 ? 5 : 0
		);
	}
}

static int
media2mode(const struct ifmedia_entry *ime, uint32_t flags, uint16_t *mode)
{
	switch (IFM_MODE(ime->ifm_media)) {
	case IFM_IEEE80211_11A:
		*mode = IEEE80211_MODE_11A;
		break;
	case IFM_IEEE80211_11B:
		*mode = IEEE80211_MODE_11B;
		break;
	case IFM_IEEE80211_11G:
		*mode = IEEE80211_MODE_11G;
		break;
	case IFM_IEEE80211_FH:
		*mode = IEEE80211_MODE_FH;
		break;
	case IFM_IEEE80211_11NA:
		*mode = IEEE80211_MODE_11NA;
		break;
	case IFM_IEEE80211_11NG:
		*mode = IEEE80211_MODE_11NG;
		break;
	case IFM_AUTO:
		*mode = IEEE80211_MODE_AUTO;
		break;
	default:
		return 0;
	}
	/*
	 * Turbo mode is an ``option''.
	 * XXX does not apply to AUTO
	 */
	if (ime->ifm_media & IFM_IEEE80211_TURBO) {
		if (*mode == IEEE80211_MODE_11A) {
			if (flags & IEEE80211_F_TURBOP)
				*mode = IEEE80211_MODE_TURBO_A;
			else
				*mode = IEEE80211_MODE_STURBO_A;
		} else if (*mode == IEEE80211_MODE_11G)
			*mode = IEEE80211_MODE_TURBO_G;
		else
			return 0;
	}
	/* XXX HT40 +/- */
	return 1;
}

/*
 * Handle a media change request on the underlying interface.
 */
int
ieee80211com_media_change(struct ifnet *ifp)
{
	return EINVAL;
}

/*
 * Handle a media change request on the vap interface.
 */
int
ieee80211_media_change(struct ifnet *ifp)
{
	struct ieee80211vap *vap = ifp->if_softc;
	struct ifmedia_entry *ime = vap->iv_media.ifm_cur;
	uint16_t newmode;

	if (!media2mode(ime, vap->iv_flags, &newmode))
		return EINVAL;
	if (vap->iv_des_mode != newmode) {
		vap->iv_des_mode = newmode;
		/* XXX kick state machine if up+running */
	}
	return 0;
}

/*
 * Common code to calculate the media status word
 * from the operating mode and channel state.
 */
static int
media_status(enum ieee80211_opmode opmode, const struct ieee80211_channel *chan)
{
	int status;

	status = IFM_IEEE80211;
	switch (opmode) {
	case IEEE80211_M_STA:
		break;
	case IEEE80211_M_IBSS:
		status |= IFM_IEEE80211_ADHOC;
		break;
	case IEEE80211_M_HOSTAP:
		status |= IFM_IEEE80211_HOSTAP;
		break;
	case IEEE80211_M_MONITOR:
		status |= IFM_IEEE80211_MONITOR;
		break;
	case IEEE80211_M_AHDEMO:
		status |= IFM_IEEE80211_ADHOC | IFM_FLAG0;
		break;
	case IEEE80211_M_WDS:
		status |= IFM_IEEE80211_WDS;
		break;
	case IEEE80211_M_MBSS:
		status |= IFM_IEEE80211_MBSS;
		break;
	}
	if (IEEE80211_IS_CHAN_HTA(chan)) {
		status |= IFM_IEEE80211_11NA;
	} else if (IEEE80211_IS_CHAN_HTG(chan)) {
		status |= IFM_IEEE80211_11NG;
	} else if (IEEE80211_IS_CHAN_A(chan)) {
		status |= IFM_IEEE80211_11A;
	} else if (IEEE80211_IS_CHAN_B(chan)) {
		status |= IFM_IEEE80211_11B;
	} else if (IEEE80211_IS_CHAN_ANYG(chan)) {
		status |= IFM_IEEE80211_11G;
	} else if (IEEE80211_IS_CHAN_FHSS(chan)) {
		status |= IFM_IEEE80211_FH;
	}
	/* XXX else complain? */

	if (IEEE80211_IS_CHAN_TURBO(chan))
		status |= IFM_IEEE80211_TURBO;
#if 0
	if (IEEE80211_IS_CHAN_HT20(chan))
		status |= IFM_IEEE80211_HT20;
	if (IEEE80211_IS_CHAN_HT40(chan))
		status |= IFM_IEEE80211_HT40;
#endif
	return status;
}

static void
ieee80211com_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap;

	imr->ifm_status = IFM_AVALID;
	TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next)
		if (vap->iv_ifp->if_flags & IFF_UP) {
			imr->ifm_status |= IFM_ACTIVE;
			break;
		}
	imr->ifm_active = media_status(ic->ic_opmode, ic->ic_curchan);
	if (imr->ifm_status & IFM_ACTIVE)
		imr->ifm_current = imr->ifm_active;
}

void
ieee80211_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	struct ieee80211vap *vap = ifp->if_softc;
	struct ieee80211com *ic = vap->iv_ic;
	enum ieee80211_phymode mode;

	imr->ifm_status = IFM_AVALID;
	/*
	 * NB: use the current channel's mode to lock down a xmit
	 * rate only when running; otherwise we may have a mismatch
	 * in which case the rate will not be convertible.
	 */
	if (vap->iv_state == IEEE80211_S_RUN ||
	    vap->iv_state == IEEE80211_S_SLEEP) {
		imr->ifm_status |= IFM_ACTIVE;
		mode = ieee80211_chan2mode(ic->ic_curchan);
	} else
		mode = IEEE80211_MODE_AUTO;
	imr->ifm_active = media_status(vap->iv_opmode, ic->ic_curchan);
	/*
	 * Calculate a current rate if possible.
	 */
	if (vap->iv_txparms[mode].ucastrate != IEEE80211_FIXED_RATE_NONE) {
		/*
		 * A fixed rate is set, report that.
		 */
		imr->ifm_active |= ieee80211_rate2media(ic,
			vap->iv_txparms[mode].ucastrate, mode);
	} else if (vap->iv_opmode == IEEE80211_M_STA) {
		/*
		 * In station mode report the current transmit rate.
		 */
		imr->ifm_active |= ieee80211_rate2media(ic,
			vap->iv_bss->ni_txrate, mode);
	} else
		imr->ifm_active |= IFM_AUTO;
	if (imr->ifm_status & IFM_ACTIVE)
		imr->ifm_current = imr->ifm_active;
}

/*
 * Set the current phy mode and recalculate the active channel
 * set based on the available channels for this mode.  Also
 * select a new default/current channel if the current one is
 * inappropriate for this mode.
 */
int
ieee80211_setmode(struct ieee80211com *ic, enum ieee80211_phymode mode)
{
	/*
	 * Adjust basic rates in 11b/11g supported rate set.
	 * Note that if operating on a hal/quarter rate channel
	 * this is a noop as those rates sets are different
	 * and used instead.
	 */
	if (mode == IEEE80211_MODE_11G || mode == IEEE80211_MODE_11B)
		ieee80211_setbasicrates(&ic->ic_sup_rates[mode], mode);

	ic->ic_curmode = mode;
	ieee80211_reset_erp(ic);	/* reset ERP state */

	return 0;
}

/*
 * Return the phy mode for with the specified channel.
 */
enum ieee80211_phymode
ieee80211_chan2mode(const struct ieee80211_channel *chan)
{

	if (IEEE80211_IS_CHAN_HTA(chan))
		return IEEE80211_MODE_11NA;
	else if (IEEE80211_IS_CHAN_HTG(chan))
		return IEEE80211_MODE_11NG;
	else if (IEEE80211_IS_CHAN_108G(chan))
		return IEEE80211_MODE_TURBO_G;
	else if (IEEE80211_IS_CHAN_ST(chan))
		return IEEE80211_MODE_STURBO_A;
	else if (IEEE80211_IS_CHAN_TURBO(chan))
		return IEEE80211_MODE_TURBO_A;
	else if (IEEE80211_IS_CHAN_HALF(chan))
		return IEEE80211_MODE_HALF;
	else if (IEEE80211_IS_CHAN_QUARTER(chan))
		return IEEE80211_MODE_QUARTER;
	else if (IEEE80211_IS_CHAN_A(chan))
		return IEEE80211_MODE_11A;
	else if (IEEE80211_IS_CHAN_ANYG(chan))
		return IEEE80211_MODE_11G;
	else if (IEEE80211_IS_CHAN_B(chan))
		return IEEE80211_MODE_11B;
	else if (IEEE80211_IS_CHAN_FHSS(chan))
		return IEEE80211_MODE_FH;

	/* NB: should not get here */
	kprintf("%s: cannot map channel to mode; freq %u flags 0x%x\n",
		__func__, chan->ic_freq, chan->ic_flags);
	return IEEE80211_MODE_11B;
}

struct ratemedia {
	u_int	match;	/* rate + mode */
	u_int	media;	/* if_media rate */
};

static int
findmedia(const struct ratemedia rates[], int n, u_int match)
{
	int i;

	for (i = 0; i < n; i++)
		if (rates[i].match == match)
			return rates[i].media;
	return IFM_AUTO;
}

/*
 * Convert IEEE80211 rate value to ifmedia subtype.
 * Rate is either a legacy rate in units of 0.5Mbps
 * or an MCS index.
 */
int
ieee80211_rate2media(struct ieee80211com *ic, int rate, enum ieee80211_phymode mode)
{
	static const struct ratemedia rates[] = {
		{   2 | IFM_IEEE80211_FH, IFM_IEEE80211_FH1 },
		{   4 | IFM_IEEE80211_FH, IFM_IEEE80211_FH2 },
		{   2 | IFM_IEEE80211_11B, IFM_IEEE80211_DS1 },
		{   4 | IFM_IEEE80211_11B, IFM_IEEE80211_DS2 },
		{  11 | IFM_IEEE80211_11B, IFM_IEEE80211_DS5 },
		{  22 | IFM_IEEE80211_11B, IFM_IEEE80211_DS11 },
		{  44 | IFM_IEEE80211_11B, IFM_IEEE80211_DS22 },
		{  12 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM6 },
		{  18 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM9 },
		{  24 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM12 },
		{  36 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM18 },
		{  48 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM24 },
		{  72 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM36 },
		{  96 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM48 },
		{ 108 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM54 },
		{   2 | IFM_IEEE80211_11G, IFM_IEEE80211_DS1 },
		{   4 | IFM_IEEE80211_11G, IFM_IEEE80211_DS2 },
		{  11 | IFM_IEEE80211_11G, IFM_IEEE80211_DS5 },
		{  22 | IFM_IEEE80211_11G, IFM_IEEE80211_DS11 },
		{  12 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM6 },
		{  18 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM9 },
		{  24 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM12 },
		{  36 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM18 },
		{  48 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM24 },
		{  72 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM36 },
		{  96 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM48 },
		{ 108 | IFM_IEEE80211_11G, IFM_IEEE80211_OFDM54 },
		{   6 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM3 },
		{   9 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM4 },
		{  54 | IFM_IEEE80211_11A, IFM_IEEE80211_OFDM27 },
		/* NB: OFDM72 doesn't realy exist so we don't handle it */
	};
	static const struct ratemedia htrates[] = {
		{   0, IFM_IEEE80211_MCS },
		{   1, IFM_IEEE80211_MCS },
		{   2, IFM_IEEE80211_MCS },
		{   3, IFM_IEEE80211_MCS },
		{   4, IFM_IEEE80211_MCS },
		{   5, IFM_IEEE80211_MCS },
		{   6, IFM_IEEE80211_MCS },
		{   7, IFM_IEEE80211_MCS },
		{   8, IFM_IEEE80211_MCS },
		{   9, IFM_IEEE80211_MCS },
		{  10, IFM_IEEE80211_MCS },
		{  11, IFM_IEEE80211_MCS },
		{  12, IFM_IEEE80211_MCS },
		{  13, IFM_IEEE80211_MCS },
		{  14, IFM_IEEE80211_MCS },
		{  15, IFM_IEEE80211_MCS },
		{  16, IFM_IEEE80211_MCS },
		{  17, IFM_IEEE80211_MCS },
		{  18, IFM_IEEE80211_MCS },
		{  19, IFM_IEEE80211_MCS },
		{  20, IFM_IEEE80211_MCS },
		{  21, IFM_IEEE80211_MCS },
		{  22, IFM_IEEE80211_MCS },
		{  23, IFM_IEEE80211_MCS },
		{  24, IFM_IEEE80211_MCS },
		{  25, IFM_IEEE80211_MCS },
		{  26, IFM_IEEE80211_MCS },
		{  27, IFM_IEEE80211_MCS },
		{  28, IFM_IEEE80211_MCS },
		{  29, IFM_IEEE80211_MCS },
		{  30, IFM_IEEE80211_MCS },
		{  31, IFM_IEEE80211_MCS },
		{  32, IFM_IEEE80211_MCS },
		{  33, IFM_IEEE80211_MCS },
		{  34, IFM_IEEE80211_MCS },
		{  35, IFM_IEEE80211_MCS },
		{  36, IFM_IEEE80211_MCS },
		{  37, IFM_IEEE80211_MCS },
		{  38, IFM_IEEE80211_MCS },
		{  39, IFM_IEEE80211_MCS },
		{  40, IFM_IEEE80211_MCS },
		{  41, IFM_IEEE80211_MCS },
		{  42, IFM_IEEE80211_MCS },
		{  43, IFM_IEEE80211_MCS },
		{  44, IFM_IEEE80211_MCS },
		{  45, IFM_IEEE80211_MCS },
		{  46, IFM_IEEE80211_MCS },
		{  47, IFM_IEEE80211_MCS },
		{  48, IFM_IEEE80211_MCS },
		{  49, IFM_IEEE80211_MCS },
		{  50, IFM_IEEE80211_MCS },
		{  51, IFM_IEEE80211_MCS },
		{  52, IFM_IEEE80211_MCS },
		{  53, IFM_IEEE80211_MCS },
		{  54, IFM_IEEE80211_MCS },
		{  55, IFM_IEEE80211_MCS },
		{  56, IFM_IEEE80211_MCS },
		{  57, IFM_IEEE80211_MCS },
		{  58, IFM_IEEE80211_MCS },
		{  59, IFM_IEEE80211_MCS },
		{  60, IFM_IEEE80211_MCS },
		{  61, IFM_IEEE80211_MCS },
		{  62, IFM_IEEE80211_MCS },
		{  63, IFM_IEEE80211_MCS },
		{  64, IFM_IEEE80211_MCS },
		{  65, IFM_IEEE80211_MCS },
		{  66, IFM_IEEE80211_MCS },
		{  67, IFM_IEEE80211_MCS },
		{  68, IFM_IEEE80211_MCS },
		{  69, IFM_IEEE80211_MCS },
		{  70, IFM_IEEE80211_MCS },
		{  71, IFM_IEEE80211_MCS },
		{  72, IFM_IEEE80211_MCS },
		{  73, IFM_IEEE80211_MCS },
		{  74, IFM_IEEE80211_MCS },
		{  75, IFM_IEEE80211_MCS },
		{  76, IFM_IEEE80211_MCS },
	};
	int m;

	/*
	 * Check 11n rates first for match as an MCS.
	 */
	if (mode == IEEE80211_MODE_11NA) {
		if (rate & IEEE80211_RATE_MCS) {
			rate &= ~IEEE80211_RATE_MCS;
			m = findmedia(htrates, nitems(htrates), rate);
			if (m != IFM_AUTO)
				return m | IFM_IEEE80211_11NA;
		}
	} else if (mode == IEEE80211_MODE_11NG) {
		/* NB: 12 is ambiguous, it will be treated as an MCS */
		if (rate & IEEE80211_RATE_MCS) {
			rate &= ~IEEE80211_RATE_MCS;
			m = findmedia(htrates, nitems(htrates), rate);
			if (m != IFM_AUTO)
				return m | IFM_IEEE80211_11NG;
		}
	}
	rate &= IEEE80211_RATE_VAL;
	switch (mode) {
	case IEEE80211_MODE_11A:
	case IEEE80211_MODE_HALF:		/* XXX good 'nuf */
	case IEEE80211_MODE_QUARTER:
	case IEEE80211_MODE_11NA:
	case IEEE80211_MODE_TURBO_A:
	case IEEE80211_MODE_STURBO_A:
		return findmedia(rates, nitems(rates),
		    rate | IFM_IEEE80211_11A);
	case IEEE80211_MODE_11B:
		return findmedia(rates, nitems(rates),
		    rate | IFM_IEEE80211_11B);
	case IEEE80211_MODE_FH:
		return findmedia(rates, nitems(rates),
		    rate | IFM_IEEE80211_FH);
	case IEEE80211_MODE_AUTO:
		/* NB: ic may be NULL for some drivers */
		if (ic != NULL && ic->ic_phytype == IEEE80211_T_FH)
			return findmedia(rates, nitems(rates),
			    rate | IFM_IEEE80211_FH);
		/* NB: hack, 11g matches both 11b+11a rates */
		/* fall thru... */
	case IEEE80211_MODE_11G:
	case IEEE80211_MODE_11NG:
	case IEEE80211_MODE_TURBO_G:
		return findmedia(rates, nitems(rates), rate | IFM_IEEE80211_11G);
	}
	return IFM_AUTO;
}

int
ieee80211_media2rate(int mword)
{
	static const int ieeerates[] = {
		-1,		/* IFM_AUTO */
		0,		/* IFM_MANUAL */
		0,		/* IFM_NONE */
		2,		/* IFM_IEEE80211_FH1 */
		4,		/* IFM_IEEE80211_FH2 */
		2,		/* IFM_IEEE80211_DS1 */
		4,		/* IFM_IEEE80211_DS2 */
		11,		/* IFM_IEEE80211_DS5 */
		22,		/* IFM_IEEE80211_DS11 */
		44,		/* IFM_IEEE80211_DS22 */
		12,		/* IFM_IEEE80211_OFDM6 */
		18,		/* IFM_IEEE80211_OFDM9 */
		24,		/* IFM_IEEE80211_OFDM12 */
		36,		/* IFM_IEEE80211_OFDM18 */
		48,		/* IFM_IEEE80211_OFDM24 */
		72,		/* IFM_IEEE80211_OFDM36 */
		96,		/* IFM_IEEE80211_OFDM48 */
		108,		/* IFM_IEEE80211_OFDM54 */
		144,		/* IFM_IEEE80211_OFDM72 */
		0,		/* IFM_IEEE80211_DS354k */
		0,		/* IFM_IEEE80211_DS512k */
		6,		/* IFM_IEEE80211_OFDM3 */
		9,		/* IFM_IEEE80211_OFDM4 */
		54,		/* IFM_IEEE80211_OFDM27 */
		-1,		/* IFM_IEEE80211_MCS */
	};
	return IFM_SUBTYPE(mword) < nitems(ieeerates) ?
		ieeerates[IFM_SUBTYPE(mword)] : 0;
}

/*
 * The following hash function is adapted from "Hash Functions" by Bob Jenkins
 * ("Algorithm Alley", Dr. Dobbs Journal, September 1997).
 */
#define	mix(a, b, c)							\
do {									\
	a -= b; a -= c; a ^= (c >> 13);					\
	b -= c; b -= a; b ^= (a << 8);					\
	c -= a; c -= b; c ^= (b >> 13);					\
	a -= b; a -= c; a ^= (c >> 12);					\
	b -= c; b -= a; b ^= (a << 16);					\
	c -= a; c -= b; c ^= (b >> 5);					\
	a -= b; a -= c; a ^= (c >> 3);					\
	b -= c; b -= a; b ^= (a << 10);					\
	c -= a; c -= b; c ^= (b >> 15);					\
} while (/*CONSTCOND*/0)

uint32_t
ieee80211_mac_hash(const struct ieee80211com *ic,
	const uint8_t addr[IEEE80211_ADDR_LEN])
{
	uint32_t a = 0x9e3779b9, b = 0x9e3779b9, c = ic->ic_hash_key;

	b += addr[5] << 8;
	b += addr[4];
	a += addr[3] << 24;
	a += addr[2] << 16;
	a += addr[1] << 8;
	a += addr[0];

	mix(a, b, c);

	return c;
}
#undef mix
