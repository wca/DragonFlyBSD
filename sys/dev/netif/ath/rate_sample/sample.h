/*-
 * Copyright (c) 2005 John Bicket
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    similar to the "NO WARRANTY" disclaimer below ("Disclaimer") and any
 *    redistribution must be conditioned upon including a substantially
 *    similar Disclaimer requirement for further binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF NONINFRINGEMENT, MERCHANTIBILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGES.
 *
 * $FreeBSD: head/sys/dev/ath/ath_rate/sample/sample.h 185482 2008-11-30 19:06:35Z sam $
 */

/*
 * Defintions for the Atheros Wireless LAN controller driver.
 */
#ifndef _DEV_ATH_RATE_SAMPLE_H
#define _DEV_ATH_RATE_SAMPLE_H

/* per-device state */
struct sample_softc {
	struct ath_ratectrl arc;	/* base class */
	int	smoothing_rate;		/* ewma percentage [0..99] */
	int	smoothing_minpackets;
	int	sample_rate;		/* %time to try different tx rates */
	int	max_successive_failures;
	int	stale_failure_timeout;	/* how long to honor max_successive_failures */
	int	min_switch;		/* min time between rate changes */
};
#define	ATH_SOFTC_SAMPLE(sc)	((struct sample_softc *)sc->sc_rc)

struct rate_stats {	
	unsigned average_tx_time;
	int successive_failures;
	int tries;
	int total_packets;
	int packets_acked;
	unsigned perfect_tx_time; /* transmit time for 0 retries */
	int last_tx;
};

struct txschedule {
	uint8_t	t0, r0;		/* series 0: tries, rate code */
	uint8_t	t1, r1;		/* series 1: tries, rate code */
	uint8_t	t2, r2;		/* series 2: tries, rate code */
	uint8_t	t3, r3;		/* series 3: tries, rate code */
};

/*
 * for now, we track performance for three different packet
 * size buckets
 */
#define NUM_PACKET_SIZE_BINS 2

/* per-node state */
struct sample_node {
	int static_rix;			/* rate index of fixed tx rate */
#define	SAMPLE_MAXRATES	32		/* NB: corresponds to hal info[32] */
	uint32_t ratemask;		/* bit mask of valid rate indices */
	const struct txschedule *sched;	/* tx schedule table */

	struct rate_stats stats[NUM_PACKET_SIZE_BINS][SAMPLE_MAXRATES];
	int last_sample_rix[NUM_PACKET_SIZE_BINS];

	int current_sample_rix[NUM_PACKET_SIZE_BINS];       
	int packets_sent[NUM_PACKET_SIZE_BINS];

	int current_rix[NUM_PACKET_SIZE_BINS];
	int packets_since_switch[NUM_PACKET_SIZE_BINS];
	unsigned ticks_since_switch[NUM_PACKET_SIZE_BINS];

	int packets_since_sample[NUM_PACKET_SIZE_BINS];
	unsigned sample_tt[NUM_PACKET_SIZE_BINS];
};
#define	ATH_NODE_SAMPLE(an)	((struct sample_node *)&(an)[1])
#define	IS_RATE_DEFINED(sn, rix)	(((sn)->ratemask & (1<<(rix))) != 0)

#ifndef MIN
#define	MIN(a,b)	((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define	MAX(a,b)	((a) > (b) ? (a) : (b))
#endif

#define WIFI_CW_MIN 31
#define WIFI_CW_MAX 1023

/*
 * Definitions for pulling the rate and trie counts from
 * a 5212 h/w descriptor.  These Don't belong here; the
 * driver should record this information so the rate control
 * code doesn't go groveling around in the descriptor bits.
 */
#define	ds_ctl2		ds_hw[0]
#define	ds_ctl3		ds_hw[1]

/* TX ds_ctl2 */
#define	AR_XmitDataTries0	0x000f0000	/* series 0 max attempts */
#define	AR_XmitDataTries0_S	16
#define	AR_XmitDataTries1	0x00f00000	/* series 1 max attempts */
#define	AR_XmitDataTries1_S	20
#define	AR_XmitDataTries2	0x0f000000	/* series 2 max attempts */
#define	AR_XmitDataTries2_S	24
#define	AR_XmitDataTries3	0xf0000000	/* series 3 max attempts */
#define	AR_XmitDataTries3_S	28

/* TX ds_ctl3 */
#define	AR_XmitRate0		0x0000001f	/* series 0 tx rate */
#define	AR_XmitRate0_S		0
#define	AR_XmitRate1		0x000003e0	/* series 1 tx rate */
#define	AR_XmitRate1_S		5
#define	AR_XmitRate2		0x00007c00	/* series 2 tx rate */
#define	AR_XmitRate2_S		10
#define	AR_XmitRate3		0x000f8000	/* series 3 tx rate */
#define	AR_XmitRate3_S		15

/* TX ds_ctl3 for 5416 */
#define	AR5416_XmitRate0	0x000000ff	/* series 0 tx rate */
#define	AR5416_XmitRate0_S	0
#define	AR5416_XmitRate1	0x0000ff00	/* series 1 tx rate */
#define	AR5416_XmitRate1_S	8
#define	AR5416_XmitRate2	0x00ff0000	/* series 2 tx rate */
#define	AR5416_XmitRate2_S	16
#define	AR5416_XmitRate3	0xff000000	/* series 3 tx rate */
#define	AR5416_XmitRate3_S	24

#define MS(_v, _f)	(((_v) & (_f)) >> _f##_S)

/*
 * Calculate the transmit duration of a frame.
 */
static unsigned calc_usecs_unicast_packet(struct ath_softc *sc,
				int length, 
				int rix, int short_retries, int long_retries) {
	const HAL_RATE_TABLE *rt = sc->sc_currates;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	int rts, cts;
	
	unsigned t_slot = 20;
	unsigned t_difs = 50; 
	unsigned t_sifs = 10; 
	int tt = 0;
	int x = 0;
	int cw = WIFI_CW_MIN;
	int cix;
	
	KASSERT(rt != NULL, ("no rate table, mode %u", sc->sc_curmode));

	if (rix >= rt->rateCount) {
		kprintf("bogus rix %d, max %u, mode %u\n",
		       rix, rt->rateCount, sc->sc_curmode);
		return 0;
	}
	cix = rt->info[rix].controlRate;
	/* 
	 * XXX getting mac/phy level timings should be fixed for turbo
	 * rates, and there is probably a way to get this from the
	 * hal...
	 */
	switch (rt->info[rix].phy) {
	case IEEE80211_T_OFDM:
		t_slot = 9;
		t_sifs = 16;
		t_difs = 28;
		/* fall through */
	case IEEE80211_T_TURBO:
		t_slot = 9;
		t_sifs = 8;
		t_difs = 28;
		break;
	case IEEE80211_T_DS:
		/* fall through to default */
	default:
		/* pg 205 ieee.802.11.pdf */
		t_slot = 20;
		t_difs = 50;
		t_sifs = 10;
	}

	rts = cts = 0;

	if ((ic->ic_flags & IEEE80211_F_USEPROT) &&
	    rt->info[rix].phy == IEEE80211_T_OFDM) {
		if (ic->ic_protmode == IEEE80211_PROT_RTSCTS)
			rts = 1;
		else if (ic->ic_protmode == IEEE80211_PROT_CTSONLY)
			cts = 1;

		cix = rt->info[sc->sc_protrix].controlRate;

	}

	if (0 /*length > ic->ic_rtsthreshold */) {
		rts = 1;
	}

	if (rts || cts) {
		int ctsduration = 0;

		/* NB: this is intentionally not a runtime check */
		KASSERT(cix < rt->rateCount,
		    ("bogus cix %d, max %u, mode %u", cix, rt->rateCount,
		     sc->sc_curmode));

		if (rts)		/* SIFS + CTS */
			ctsduration += rt->info[cix].spAckDuration;

		ctsduration += ath_hal_computetxtime(sc->sc_ah,
						     rt, length, rix, AH_TRUE);

		if (cts)	/* SIFS + ACK */
			ctsduration += rt->info[cix].spAckDuration;

		tt += (short_retries + 1) * ctsduration;
	}
	tt += t_difs;
	tt += (long_retries+1)*(t_sifs + rt->info[rix].spAckDuration);
	tt += (long_retries+1)*ath_hal_computetxtime(sc->sc_ah, rt, length, 
						rix, AH_TRUE);
	for (x = 0; x <= short_retries + long_retries; x++) {
		cw = MIN(WIFI_CW_MAX, (cw + 1) * 2);
		tt += (t_slot * cw/2);
	}
	return tt;
}
#endif /* _DEV_ATH_RATE_SAMPLE_H */