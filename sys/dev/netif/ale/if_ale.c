/*-
 * Copyright (c) 2008, Pyun YongHyeon <yongari@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
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
 *
 * $FreeBSD: src/sys/dev/ale/if_ale.c,v 1.3 2008/12/03 09:01:12 yongari Exp $
 */

/* Driver for Atheros AR8121/AR8113/AR8114 PCIe Ethernet. */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/interrupt.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/serialize.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/bpf.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_llc.h>
#include <net/if_media.h>
#include <net/ifq_var.h>
#include <net/vlan/if_vlan_var.h>
#include <net/vlan/if_vlan_ether.h>

#include <netinet/ip.h>

#include <dev/netif/mii_layer/mii.h>
#include <dev/netif/mii_layer/miivar.h>

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>
#include "pcidevs.h"

#include <dev/netif/ale/if_alereg.h>
#include <dev/netif/ale/if_alevar.h>

/* "device miibus" required.  See GENERIC if you get errors here. */
#include "miibus_if.h"

/* For more information about Tx checksum offload issues see ale_encap(). */
#define	ALE_CSUM_FEATURES	(CSUM_TCP | CSUM_UDP)

struct ale_dmamap_ctx {
	int			nsegs;
	bus_dma_segment_t	*segs;
};

static int	ale_probe(device_t);
static int	ale_attach(device_t);
static int	ale_detach(device_t);
static int	ale_shutdown(device_t);
static int	ale_suspend(device_t);
static int	ale_resume(device_t);

static int	ale_miibus_readreg(device_t, int, int);
static int	ale_miibus_writereg(device_t, int, int, int);
static void	ale_miibus_statchg(device_t);

static void	ale_init(void *);
static void	ale_start(struct ifnet *, struct ifaltq_subque *);
static int	ale_ioctl(struct ifnet *, u_long, caddr_t, struct ucred *);
static void	ale_watchdog(struct ifnet *);
static int	ale_mediachange(struct ifnet *);
static void	ale_mediastatus(struct ifnet *, struct ifmediareq *);

static void	ale_intr(void *);
static int	ale_rxeof(struct ale_softc *sc);
static void	ale_rx_update_page(struct ale_softc *, struct ale_rx_page **,
		    uint32_t, uint32_t *);
static void	ale_rxcsum(struct ale_softc *, struct mbuf *, uint32_t);
static void	ale_txeof(struct ale_softc *);

static int	ale_dma_alloc(struct ale_softc *);
static void	ale_dma_free(struct ale_softc *);
static int	ale_check_boundary(struct ale_softc *);
static void	ale_dmamap_cb(void *, bus_dma_segment_t *, int, int);
static void	ale_dmamap_buf_cb(void *, bus_dma_segment_t *, int,
		    bus_size_t, int);
static int	ale_encap(struct ale_softc *, struct mbuf **);
static void	ale_init_rx_pages(struct ale_softc *);
static void	ale_init_tx_ring(struct ale_softc *);

static void	ale_stop(struct ale_softc *);
static void	ale_tick(void *);
static void	ale_get_macaddr(struct ale_softc *);
static void	ale_mac_config(struct ale_softc *);
static void	ale_phy_reset(struct ale_softc *);
static void	ale_reset(struct ale_softc *);
static void	ale_rxfilter(struct ale_softc *);
static void	ale_rxvlan(struct ale_softc *);
static void	ale_stats_clear(struct ale_softc *);
static void	ale_stats_update(struct ale_softc *);
static void	ale_stop_mac(struct ale_softc *);
#ifdef notyet
static void	ale_setlinkspeed(struct ale_softc *);
static void	ale_setwol(struct ale_softc *);
#endif

static void	ale_sysctl_node(struct ale_softc *);
static int	sysctl_hw_ale_int_mod(SYSCTL_HANDLER_ARGS);

/*
 * Devices supported by this driver.
 */
static struct ale_dev {
	uint16_t	ale_vendorid;
	uint16_t	ale_deviceid;
	const char	*ale_name;
} ale_devs[] = {
    { VENDORID_ATHEROS, DEVICEID_ATHEROS_AR81XX,
    "Atheros AR8121/AR8113/AR8114 PCIe Ethernet" },
};

static device_method_t ale_methods[] = {
	/* Device interface. */
	DEVMETHOD(device_probe,		ale_probe),
	DEVMETHOD(device_attach,	ale_attach),
	DEVMETHOD(device_detach,	ale_detach),
	DEVMETHOD(device_shutdown,	ale_shutdown),
	DEVMETHOD(device_suspend,	ale_suspend),
	DEVMETHOD(device_resume,	ale_resume),

	/* Bus interface. */
	DEVMETHOD(bus_print_child,	bus_generic_print_child),
	DEVMETHOD(bus_driver_added,	bus_generic_driver_added),

	/* MII interface. */
	DEVMETHOD(miibus_readreg,	ale_miibus_readreg),
	DEVMETHOD(miibus_writereg,	ale_miibus_writereg),
	DEVMETHOD(miibus_statchg,	ale_miibus_statchg),

	{ NULL, NULL }
};

static driver_t ale_driver = {
	"ale",
	ale_methods,
	sizeof(struct ale_softc)
};

static devclass_t ale_devclass;

DECLARE_DUMMY_MODULE(if_ale);
MODULE_VERSION(if_ale, 1);
MODULE_DEPEND(if_ale, miibus, 1, 1, 1);
DRIVER_MODULE(if_ale, pci, ale_driver, ale_devclass, NULL, NULL);
DRIVER_MODULE(miibus, ale, miibus_driver, miibus_devclass, NULL, NULL);

static int
ale_miibus_readreg(device_t dev, int phy, int reg)
{
	struct ale_softc *sc;
	uint32_t v;
	int i;

	sc = device_get_softc(dev);

	if (phy != sc->ale_phyaddr)
		return (0);

	if (sc->ale_flags & ALE_FLAG_FASTETHER) {
		if (reg == MII_100T2CR || reg == MII_100T2SR ||
		    reg == MII_EXTSR)
			return (0);
	}

	CSR_WRITE_4(sc, ALE_MDIO, MDIO_OP_EXECUTE | MDIO_OP_READ |
	    MDIO_SUP_PREAMBLE | MDIO_CLK_25_4 | MDIO_REG_ADDR(reg));
	for (i = ALE_PHY_TIMEOUT; i > 0; i--) {
		DELAY(5);
		v = CSR_READ_4(sc, ALE_MDIO);
		if ((v & (MDIO_OP_EXECUTE | MDIO_OP_BUSY)) == 0)
			break;
	}

	if (i == 0) {
		device_printf(sc->ale_dev, "phy read timeout : %d\n", reg);
		return (0);
	}

	return ((v & MDIO_DATA_MASK) >> MDIO_DATA_SHIFT);
}

static int
ale_miibus_writereg(device_t dev, int phy, int reg, int val)
{
	struct ale_softc *sc;
	uint32_t v;
	int i;

	sc = device_get_softc(dev);

	if (phy != sc->ale_phyaddr)
		return (0);

	if (sc->ale_flags & ALE_FLAG_FASTETHER) {
		if (reg == MII_100T2CR || reg == MII_100T2SR ||
		    reg == MII_EXTSR)
			return (0);
	}

	CSR_WRITE_4(sc, ALE_MDIO, MDIO_OP_EXECUTE | MDIO_OP_WRITE |
	    (val & MDIO_DATA_MASK) << MDIO_DATA_SHIFT |
	    MDIO_SUP_PREAMBLE | MDIO_CLK_25_4 | MDIO_REG_ADDR(reg));
	for (i = ALE_PHY_TIMEOUT; i > 0; i--) {
		DELAY(5);
		v = CSR_READ_4(sc, ALE_MDIO);
		if ((v & (MDIO_OP_EXECUTE | MDIO_OP_BUSY)) == 0)
			break;
	}

	if (i == 0)
		device_printf(sc->ale_dev, "phy write timeout : %d\n", reg);

	return (0);
}

static void
ale_miibus_statchg(device_t dev)
{
	struct ale_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mii_data *mii;
	uint32_t reg;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if ((ifp->if_flags & IFF_RUNNING) == 0)
		return;

	mii = device_get_softc(sc->ale_miibus);

	sc->ale_flags &= ~ALE_FLAG_LINK;
	if ((mii->mii_media_status & (IFM_ACTIVE | IFM_AVALID)) ==
	    (IFM_ACTIVE | IFM_AVALID)) {
		switch (IFM_SUBTYPE(mii->mii_media_active)) {
		case IFM_10_T:
		case IFM_100_TX:
			sc->ale_flags |= ALE_FLAG_LINK;
			break;

		case IFM_1000_T:
			if ((sc->ale_flags & ALE_FLAG_FASTETHER) == 0)
				sc->ale_flags |= ALE_FLAG_LINK;
			break;

		default:
			break;
		}
	}

	/* Stop Rx/Tx MACs. */
	ale_stop_mac(sc);

	/* Program MACs with resolved speed/duplex/flow-control. */
	if ((sc->ale_flags & ALE_FLAG_LINK) != 0) {
		ale_mac_config(sc);
		/* Reenable Tx/Rx MACs. */
		reg = CSR_READ_4(sc, ALE_MAC_CFG);
		reg |= MAC_CFG_TX_ENB | MAC_CFG_RX_ENB;
		CSR_WRITE_4(sc, ALE_MAC_CFG, reg);
	}
}

static void
ale_mediastatus(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct ale_softc *sc = ifp->if_softc;
	struct mii_data *mii = device_get_softc(sc->ale_miibus);

	ASSERT_SERIALIZED(ifp->if_serializer);

	mii_pollstat(mii);
	ifmr->ifm_status = mii->mii_media_status;
	ifmr->ifm_active = mii->mii_media_active;
}

static int
ale_mediachange(struct ifnet *ifp)
{
	struct ale_softc *sc = ifp->if_softc;
	struct mii_data *mii = device_get_softc(sc->ale_miibus);
	int error;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if (mii->mii_instance != 0) {
		struct mii_softc *miisc;

		LIST_FOREACH(miisc, &mii->mii_phys, mii_list)
			mii_phy_reset(miisc);
	}
	error = mii_mediachg(mii);

	return (error);
}

static int
ale_probe(device_t dev)
{
	struct ale_dev *sp;
	int i;
	uint16_t vendor, devid;

	vendor = pci_get_vendor(dev);
	devid = pci_get_device(dev);
	sp = ale_devs;
	for (i = 0; i < NELEM(ale_devs); i++) {
		if (vendor == sp->ale_vendorid &&
		    devid == sp->ale_deviceid) {
			device_set_desc(dev, sp->ale_name);
			return (0);
		}
		sp++;
	}

	return (ENXIO);
}

static void
ale_get_macaddr(struct ale_softc *sc)
{
	uint32_t ea[2], reg;
	int i, vpdc;

	reg = CSR_READ_4(sc, ALE_SPI_CTRL);
	if ((reg & SPI_VPD_ENB) != 0) {
		reg &= ~SPI_VPD_ENB;
		CSR_WRITE_4(sc, ALE_SPI_CTRL, reg);
	}

	vpdc = pci_get_vpdcap_ptr(sc->ale_dev);
	if (vpdc) {
		/*
		 * PCI VPD capability found, let TWSI reload EEPROM.
		 * This will set ethernet address of controller.
		 */
		CSR_WRITE_4(sc, ALE_TWSI_CTRL, CSR_READ_4(sc, ALE_TWSI_CTRL) |
		    TWSI_CTRL_SW_LD_START);
		for (i = 100; i > 0; i--) {
			DELAY(1000);
			reg = CSR_READ_4(sc, ALE_TWSI_CTRL);
			if ((reg & TWSI_CTRL_SW_LD_START) == 0)
				break;
		}
		if (i == 0)
			device_printf(sc->ale_dev,
			    "reloading EEPROM timeout!\n");
	} else {
		if (bootverbose)
			device_printf(sc->ale_dev,
			    "PCI VPD capability not found!\n");
	}

	ea[0] = CSR_READ_4(sc, ALE_PAR0);
	ea[1] = CSR_READ_4(sc, ALE_PAR1);
	sc->ale_eaddr[0] = (ea[1] >> 8) & 0xFF;
	sc->ale_eaddr[1] = (ea[1] >> 0) & 0xFF;
	sc->ale_eaddr[2] = (ea[0] >> 24) & 0xFF;
	sc->ale_eaddr[3] = (ea[0] >> 16) & 0xFF;
	sc->ale_eaddr[4] = (ea[0] >> 8) & 0xFF;
	sc->ale_eaddr[5] = (ea[0] >> 0) & 0xFF;
}

static void
ale_phy_reset(struct ale_softc *sc)
{
	/* Reset magic from Linux. */
	CSR_WRITE_2(sc, ALE_GPHY_CTRL,
	    GPHY_CTRL_HIB_EN | GPHY_CTRL_HIB_PULSE | GPHY_CTRL_SEL_ANA_RESET |
	    GPHY_CTRL_PHY_PLL_ON);
	DELAY(1000);
	CSR_WRITE_2(sc, ALE_GPHY_CTRL,
	    GPHY_CTRL_EXT_RESET | GPHY_CTRL_HIB_EN | GPHY_CTRL_HIB_PULSE |
	    GPHY_CTRL_SEL_ANA_RESET | GPHY_CTRL_PHY_PLL_ON);
	DELAY(1000);

#define	ATPHY_DBG_ADDR		0x1D
#define	ATPHY_DBG_DATA		0x1E

	/* Enable hibernation mode. */
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_ADDR, 0x0B);
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_DATA, 0xBC00);
	/* Set Class A/B for all modes. */
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_ADDR, 0x00);
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_DATA, 0x02EF);
	/* Enable 10BT power saving. */
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_ADDR, 0x12);
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_DATA, 0x4C04);
	/* Adjust 1000T power. */
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_ADDR, 0x04);
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_ADDR, 0x8BBB);
	/* 10BT center tap voltage. */
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_ADDR, 0x05);
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    ATPHY_DBG_ADDR, 0x2C46);

#undef	ATPHY_DBG_ADDR
#undef	ATPHY_DBG_DATA
	DELAY(1000);
}

static int
ale_attach(device_t dev)
{
	struct ale_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int error = 0;
	uint32_t rxf_len, txf_len;
	uint8_t pcie_ptr;

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	sc->ale_dev = dev;

	callout_init(&sc->ale_tick_ch);

#ifndef BURN_BRIDGES
	if (pci_get_powerstate(dev) != PCI_POWERSTATE_D0) {
		uint32_t irq, mem;

		irq = pci_read_config(dev, PCIR_INTLINE, 4);
		mem = pci_read_config(dev, ALE_PCIR_BAR, 4);

		device_printf(dev, "chip is in D%d power mode "
		    "-- setting to D0\n", pci_get_powerstate(dev));

		pci_set_powerstate(dev, PCI_POWERSTATE_D0);

		pci_write_config(dev, PCIR_INTLINE, irq, 4);
		pci_write_config(dev, ALE_PCIR_BAR, mem, 4);
	}
#endif	/* !BURN_BRIDGE */

	/* Enable bus mastering */
	pci_enable_busmaster(dev);

	/*
	 * Allocate memory mapped IO
	 */
	sc->ale_mem_rid = ALE_PCIR_BAR;
	sc->ale_mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
						 &sc->ale_mem_rid, RF_ACTIVE);
	if (sc->ale_mem_res == NULL) {
		device_printf(dev, "can't allocate IO memory\n");
		return ENXIO;
	}
	sc->ale_mem_bt = rman_get_bustag(sc->ale_mem_res);
	sc->ale_mem_bh = rman_get_bushandle(sc->ale_mem_res);

	/*
	 * Allocate IRQ
	 */
	sc->ale_irq_rid = 0;
	sc->ale_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ,
						 &sc->ale_irq_rid,
						 RF_SHAREABLE | RF_ACTIVE);
	if (sc->ale_irq_res == NULL) {
		device_printf(dev, "can't allocate irq\n");
		error = ENXIO;
		goto fail;
	}

	/* Set PHY address. */
	sc->ale_phyaddr = ALE_PHY_ADDR;

	/* Reset PHY. */
	ale_phy_reset(sc);

	/* Reset the ethernet controller. */
	ale_reset(sc);

	/* Get PCI and chip id/revision. */
	sc->ale_rev = pci_get_revid(dev);
	if (sc->ale_rev >= 0xF0) {
		/* L2E Rev. B. AR8114 */
		sc->ale_flags |= ALE_FLAG_FASTETHER;
	} else {
		if ((CSR_READ_4(sc, ALE_PHY_STATUS) & PHY_STATUS_100M) != 0) {
			/* L1E AR8121 */
			sc->ale_flags |= ALE_FLAG_JUMBO;
		} else {
			/* L2E Rev. A. AR8113 */
			sc->ale_flags |= ALE_FLAG_FASTETHER;
		}
	}

	/*
	 * All known controllers seems to require 4 bytes alignment
	 * of Tx buffers to make Tx checksum offload with custom
	 * checksum generation method work.
	 */
	sc->ale_flags |= ALE_FLAG_TXCSUM_BUG;

	/*
	 * All known controllers seems to have issues on Rx checksum
	 * offload for fragmented IP datagrams.
	 */
	sc->ale_flags |= ALE_FLAG_RXCSUM_BUG;

	/*
	 * Don't use Tx CMB. It is known to cause RRS update failure
	 * under certain circumstances. Typical phenomenon of the
	 * issue would be unexpected sequence number encountered in
	 * Rx handler.
	 */
	sc->ale_flags |= ALE_FLAG_TXCMB_BUG;
	sc->ale_chip_rev = CSR_READ_4(sc, ALE_MASTER_CFG) >>
	    MASTER_CHIP_REV_SHIFT;
	if (bootverbose) {
		device_printf(dev, "PCI device revision : 0x%04x\n",
		    sc->ale_rev);
		device_printf(dev, "Chip id/revision : 0x%04x\n",
		    sc->ale_chip_rev);
	}

	/*
	 * Uninitialized hardware returns an invalid chip id/revision
	 * as well as 0xFFFFFFFF for Tx/Rx fifo length.
	 */
	txf_len = CSR_READ_4(sc, ALE_SRAM_TX_FIFO_LEN);
	rxf_len = CSR_READ_4(sc, ALE_SRAM_RX_FIFO_LEN);
	if (sc->ale_chip_rev == 0xFFFF || txf_len == 0xFFFFFFFF ||
	    rxf_len == 0xFFFFFFF) {
		device_printf(dev,"chip revision : 0x%04x, %u Tx FIFO "
		    "%u Rx FIFO -- not initialized?\n", sc->ale_chip_rev,
		    txf_len, rxf_len);
		error = ENXIO;
		goto fail;
	}
	device_printf(dev, "%u Tx FIFO, %u Rx FIFO\n", txf_len, rxf_len);

	/* Get DMA parameters from PCIe device control register. */
	pcie_ptr = pci_get_pciecap_ptr(dev);
	if (pcie_ptr) {
		uint16_t devctl;

		sc->ale_flags |= ALE_FLAG_PCIE;
		devctl = pci_read_config(dev, pcie_ptr + PCIER_DEVCTRL, 2);
		/* Max read request size. */
		sc->ale_dma_rd_burst = ((devctl >> 12) & 0x07) <<
		    DMA_CFG_RD_BURST_SHIFT;
		/* Max payload size. */
		sc->ale_dma_wr_burst = ((devctl >> 5) & 0x07) <<
		    DMA_CFG_WR_BURST_SHIFT;
		if (bootverbose) {
			device_printf(dev, "Read request size : %d bytes.\n",
			    128 << ((devctl >> 12) & 0x07));
			device_printf(dev, "TLP payload size : %d bytes.\n",
			    128 << ((devctl >> 5) & 0x07));
		}
	} else {
		sc->ale_dma_rd_burst = DMA_CFG_RD_BURST_128;
		sc->ale_dma_wr_burst = DMA_CFG_WR_BURST_128;
	}

	/* Create device sysctl node. */
	ale_sysctl_node(sc);

	if ((error = ale_dma_alloc(sc) != 0))
		goto fail;

	/* Load station address. */
	ale_get_macaddr(sc);

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = ale_ioctl;
	ifp->if_start = ale_start;
	ifp->if_init = ale_init;
	ifp->if_watchdog = ale_watchdog;
	ifq_set_maxlen(&ifp->if_snd, ALE_TX_RING_CNT - 1);
	ifq_set_ready(&ifp->if_snd);

	ifp->if_capabilities = IFCAP_RXCSUM |
			       IFCAP_VLAN_MTU |
			       IFCAP_VLAN_HWTAGGING;
#ifdef notyet
	ifp->if_capabilities |= IFCAP_TXCSUM;
	ifp->if_hwassist = ALE_CSUM_FEATURES;
#endif
	ifp->if_capenable = ifp->if_capabilities;

	/* Set up MII bus. */
	if ((error = mii_phy_probe(dev, &sc->ale_miibus, ale_mediachange,
	    ale_mediastatus)) != 0) {
		device_printf(dev, "no PHY found!\n");
		goto fail;
	}

	ether_ifattach(ifp, sc->ale_eaddr, NULL);

	/* Tell the upper layer(s) we support long frames. */
	ifp->if_data.ifi_hdrlen = sizeof(struct ether_vlan_header);

	ifq_set_cpuid(&ifp->if_snd, rman_get_cpuid(sc->ale_irq_res));

	error = bus_setup_intr(dev, sc->ale_irq_res, INTR_MPSAFE, ale_intr, sc,
			       &sc->ale_irq_handle, ifp->if_serializer);
	if (error) {
		device_printf(dev, "could not set up interrupt handler.\n");
		ether_ifdetach(ifp);
		goto fail;
	}

	return 0;
fail:
	ale_detach(dev);
	return (error);
}

static int
ale_detach(device_t dev)
{
	struct ale_softc *sc = device_get_softc(dev);

	if (device_is_attached(dev)) {
		struct ifnet *ifp = &sc->arpcom.ac_if;

		lwkt_serialize_enter(ifp->if_serializer);
		sc->ale_flags |= ALE_FLAG_DETACH;
		ale_stop(sc);
		bus_teardown_intr(dev, sc->ale_irq_res, sc->ale_irq_handle);
		lwkt_serialize_exit(ifp->if_serializer);

		ether_ifdetach(ifp);
	}

	if (sc->ale_miibus != NULL)
		device_delete_child(dev, sc->ale_miibus);
	bus_generic_detach(dev);

	if (sc->ale_irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->ale_irq_rid,
				     sc->ale_irq_res);
	}
	if (sc->ale_mem_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->ale_mem_rid,
				     sc->ale_mem_res);
	}

	ale_dma_free(sc);

	return (0);
}

#define	ALE_SYSCTL_STAT_ADD32(c, h, n, p, d)	\
	    SYSCTL_ADD_UINT(c, h, OID_AUTO, n, CTLFLAG_RD, p, 0, d)
#define	ALE_SYSCTL_STAT_ADD64(c, h, n, p, d)	\
	    SYSCTL_ADD_QUAD(c, h, OID_AUTO, n, CTLFLAG_RD, p, 0, d)

static void
ale_sysctl_node(struct ale_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *child, *parent;
	struct sysctl_oid *tree;
	struct ale_hw_stats *stats;
	int error;

	stats = &sc->ale_stats;
	ctx = device_get_sysctl_ctx(sc->ale_dev);
	child = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->ale_dev));

	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "int_rx_mod",
	    CTLTYPE_INT | CTLFLAG_RW, &sc->ale_int_rx_mod, 0,
	    sysctl_hw_ale_int_mod, "I", "ale Rx interrupt moderation");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "int_tx_mod",
	    CTLTYPE_INT | CTLFLAG_RW, &sc->ale_int_tx_mod, 0,
	    sysctl_hw_ale_int_mod, "I", "ale Tx interrupt moderation");

	/*
	 * Pull in device tunables.
	 */
	sc->ale_int_rx_mod = ALE_IM_RX_TIMER_DEFAULT;
	error = resource_int_value(device_get_name(sc->ale_dev),
	    device_get_unit(sc->ale_dev), "int_rx_mod", &sc->ale_int_rx_mod);
	if (error == 0) {
		if (sc->ale_int_rx_mod < ALE_IM_TIMER_MIN ||
		    sc->ale_int_rx_mod > ALE_IM_TIMER_MAX) {
			device_printf(sc->ale_dev, "int_rx_mod value out of "
			    "range; using default: %d\n",
			    ALE_IM_RX_TIMER_DEFAULT);
			sc->ale_int_rx_mod = ALE_IM_RX_TIMER_DEFAULT;
		}
	}

	sc->ale_int_tx_mod = ALE_IM_TX_TIMER_DEFAULT;
	error = resource_int_value(device_get_name(sc->ale_dev),
	    device_get_unit(sc->ale_dev), "int_tx_mod", &sc->ale_int_tx_mod);
	if (error == 0) {
		if (sc->ale_int_tx_mod < ALE_IM_TIMER_MIN ||
		    sc->ale_int_tx_mod > ALE_IM_TIMER_MAX) {
			device_printf(sc->ale_dev, "int_tx_mod value out of "
			    "range; using default: %d\n",
			    ALE_IM_TX_TIMER_DEFAULT);
			sc->ale_int_tx_mod = ALE_IM_TX_TIMER_DEFAULT;
		}
	}

	/* Misc statistics. */
	ALE_SYSCTL_STAT_ADD32(ctx, child, "reset_brk_seq",
	    &stats->reset_brk_seq,
	    "Controller resets due to broken Rx sequnce number");

	tree = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, "stats", CTLFLAG_RD,
	    NULL, "ATE statistics");
	parent = SYSCTL_CHILDREN(tree);

	/* Rx statistics. */
	tree = SYSCTL_ADD_NODE(ctx, parent, OID_AUTO, "rx", CTLFLAG_RD,
	    NULL, "Rx MAC statistics");
	child = SYSCTL_CHILDREN(tree);
	ALE_SYSCTL_STAT_ADD32(ctx, child, "good_frames",
	    &stats->rx_frames, "Good frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "good_bcast_frames",
	    &stats->rx_bcast_frames, "Good broadcast frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "good_mcast_frames",
	    &stats->rx_mcast_frames, "Good multicast frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "pause_frames",
	    &stats->rx_pause_frames, "Pause control frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "control_frames",
	    &stats->rx_control_frames, "Control frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "crc_errs",
	    &stats->rx_crcerrs, "CRC errors");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "len_errs",
	    &stats->rx_lenerrs, "Frames with length mismatched");
	ALE_SYSCTL_STAT_ADD64(ctx, child, "good_octets",
	    &stats->rx_bytes, "Good octets");
	ALE_SYSCTL_STAT_ADD64(ctx, child, "good_bcast_octets",
	    &stats->rx_bcast_bytes, "Good broadcast octets");
	ALE_SYSCTL_STAT_ADD64(ctx, child, "good_mcast_octets",
	    &stats->rx_mcast_bytes, "Good multicast octets");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "runts",
	    &stats->rx_runts, "Too short frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "fragments",
	    &stats->rx_fragments, "Fragmented frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_64",
	    &stats->rx_pkts_64, "64 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_65_127",
	    &stats->rx_pkts_65_127, "65 to 127 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_128_255",
	    &stats->rx_pkts_128_255, "128 to 255 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_256_511",
	    &stats->rx_pkts_256_511, "256 to 511 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_512_1023",
	    &stats->rx_pkts_512_1023, "512 to 1023 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_1024_1518",
	    &stats->rx_pkts_1024_1518, "1024 to 1518 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_1519_max",
	    &stats->rx_pkts_1519_max, "1519 to max frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "trunc_errs",
	    &stats->rx_pkts_truncated, "Truncated frames due to MTU size");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "fifo_oflows",
	    &stats->rx_fifo_oflows, "FIFO overflows");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "rrs_errs",
	    &stats->rx_rrs_errs, "Return status write-back errors");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "align_errs",
	    &stats->rx_alignerrs, "Alignment errors");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "filtered",
	    &stats->rx_pkts_filtered,
	    "Frames dropped due to address filtering");

	/* Tx statistics. */
	tree = SYSCTL_ADD_NODE(ctx, parent, OID_AUTO, "tx", CTLFLAG_RD,
	    NULL, "Tx MAC statistics");
	child = SYSCTL_CHILDREN(tree);
	ALE_SYSCTL_STAT_ADD32(ctx, child, "good_frames",
	    &stats->tx_frames, "Good frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "good_bcast_frames",
	    &stats->tx_bcast_frames, "Good broadcast frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "good_mcast_frames",
	    &stats->tx_mcast_frames, "Good multicast frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "pause_frames",
	    &stats->tx_pause_frames, "Pause control frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "control_frames",
	    &stats->tx_control_frames, "Control frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "excess_defers",
	    &stats->tx_excess_defer, "Frames with excessive derferrals");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "defers",
	    &stats->tx_excess_defer, "Frames with derferrals");
	ALE_SYSCTL_STAT_ADD64(ctx, child, "good_octets",
	    &stats->tx_bytes, "Good octets");
	ALE_SYSCTL_STAT_ADD64(ctx, child, "good_bcast_octets",
	    &stats->tx_bcast_bytes, "Good broadcast octets");
	ALE_SYSCTL_STAT_ADD64(ctx, child, "good_mcast_octets",
	    &stats->tx_mcast_bytes, "Good multicast octets");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_64",
	    &stats->tx_pkts_64, "64 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_65_127",
	    &stats->tx_pkts_65_127, "65 to 127 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_128_255",
	    &stats->tx_pkts_128_255, "128 to 255 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_256_511",
	    &stats->tx_pkts_256_511, "256 to 511 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_512_1023",
	    &stats->tx_pkts_512_1023, "512 to 1023 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_1024_1518",
	    &stats->tx_pkts_1024_1518, "1024 to 1518 bytes frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "frames_1519_max",
	    &stats->tx_pkts_1519_max, "1519 to max frames");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "single_colls",
	    &stats->tx_single_colls, "Single collisions");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "multi_colls",
	    &stats->tx_multi_colls, "Multiple collisions");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "late_colls",
	    &stats->tx_late_colls, "Late collisions");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "excess_colls",
	    &stats->tx_excess_colls, "Excessive collisions");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "abort",
	    &stats->tx_abort, "Aborted frames due to Excessive collisions");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "underruns",
	    &stats->tx_underrun, "FIFO underruns");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "desc_underruns",
	    &stats->tx_desc_underrun, "Descriptor write-back errors");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "len_errs",
	    &stats->tx_lenerrs, "Frames with length mismatched");
	ALE_SYSCTL_STAT_ADD32(ctx, child, "trunc_errs",
	    &stats->tx_pkts_truncated, "Truncated frames due to MTU size");
}

#undef ALE_SYSCTL_STAT_ADD32
#undef ALE_SYSCTL_STAT_ADD64

struct ale_dmamap_arg {
	bus_addr_t	ale_busaddr;
};

static void
ale_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct ale_dmamap_arg *ctx;

	if (error != 0)
		return;

	KASSERT(nsegs == 1, ("%s: %d segments returned!", __func__, nsegs));

	ctx = (struct ale_dmamap_arg *)arg;
	ctx->ale_busaddr = segs[0].ds_addr;
}

/*
 * Tx descriptors/RXF0/CMB DMA blocks share ALE_DESC_ADDR_HI register
 * which specifies high address region of DMA blocks. Therefore these
 * blocks should have the same high address of given 4GB address
 * space(i.e. crossing 4GB boundary is not allowed).
 */
static int
ale_check_boundary(struct ale_softc *sc)
{
	bus_addr_t rx_cmb_end[ALE_RX_PAGES], tx_cmb_end;
	bus_addr_t rx_page_end[ALE_RX_PAGES], tx_ring_end;

	rx_page_end[0] = sc->ale_cdata.ale_rx_page[0].page_paddr +
	    sc->ale_pagesize;
	rx_page_end[1] = sc->ale_cdata.ale_rx_page[1].page_paddr +
	    sc->ale_pagesize;
	tx_ring_end = sc->ale_cdata.ale_tx_ring_paddr + ALE_TX_RING_SZ;
	tx_cmb_end = sc->ale_cdata.ale_tx_cmb_paddr + ALE_TX_CMB_SZ;
	rx_cmb_end[0] = sc->ale_cdata.ale_rx_page[0].cmb_paddr + ALE_RX_CMB_SZ;
	rx_cmb_end[1] = sc->ale_cdata.ale_rx_page[1].cmb_paddr + ALE_RX_CMB_SZ;

	if ((ALE_ADDR_HI(tx_ring_end) !=
	    ALE_ADDR_HI(sc->ale_cdata.ale_tx_ring_paddr)) ||
	    (ALE_ADDR_HI(rx_page_end[0]) !=
	    ALE_ADDR_HI(sc->ale_cdata.ale_rx_page[0].page_paddr)) ||
	    (ALE_ADDR_HI(rx_page_end[1]) !=
	    ALE_ADDR_HI(sc->ale_cdata.ale_rx_page[1].page_paddr)) ||
	    (ALE_ADDR_HI(tx_cmb_end) !=
	    ALE_ADDR_HI(sc->ale_cdata.ale_tx_cmb_paddr)) ||
	    (ALE_ADDR_HI(rx_cmb_end[0]) !=
	    ALE_ADDR_HI(sc->ale_cdata.ale_rx_page[0].cmb_paddr)) ||
	    (ALE_ADDR_HI(rx_cmb_end[1]) !=
	    ALE_ADDR_HI(sc->ale_cdata.ale_rx_page[1].cmb_paddr)))
		return (EFBIG);

	if ((ALE_ADDR_HI(tx_ring_end) != ALE_ADDR_HI(rx_page_end[0])) ||
	    (ALE_ADDR_HI(tx_ring_end) != ALE_ADDR_HI(rx_page_end[1])) ||
	    (ALE_ADDR_HI(tx_ring_end) != ALE_ADDR_HI(rx_cmb_end[0])) ||
	    (ALE_ADDR_HI(tx_ring_end) != ALE_ADDR_HI(rx_cmb_end[1])) ||
	    (ALE_ADDR_HI(tx_ring_end) != ALE_ADDR_HI(tx_cmb_end)))
		return (EFBIG);

	return (0);
}

static int
ale_dma_alloc(struct ale_softc *sc)
{
	struct ale_txdesc *txd;
	bus_addr_t lowaddr;
	struct ale_dmamap_arg ctx;
	int error, guard_size, i;

	if ((sc->ale_flags & ALE_FLAG_JUMBO) != 0)
		guard_size = ALE_JUMBO_FRAMELEN;
	else
		guard_size = ALE_MAX_FRAMELEN;
	sc->ale_pagesize = roundup(guard_size + ALE_RX_PAGE_SZ,
	    ALE_RX_PAGE_ALIGN);
	lowaddr = BUS_SPACE_MAXADDR;
again:
	/* Create parent DMA tag. */
	error = bus_dma_tag_create(
	    NULL,			/* parent */
	    1, 0,			/* alignment, boundary */
	    lowaddr,			/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    BUS_SPACE_MAXSIZE_32BIT,	/* maxsize */
	    0,				/* nsegments */
	    BUS_SPACE_MAXSIZE_32BIT,	/* maxsegsize */
	    0,				/* flags */
	    &sc->ale_cdata.ale_parent_tag);
	if (error != 0) {
		device_printf(sc->ale_dev,
		    "could not create parent DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Tx descriptor ring. */
	error = bus_dma_tag_create(
	    sc->ale_cdata.ale_parent_tag, /* parent */
	    ALE_TX_RING_ALIGN, 0,	/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    ALE_TX_RING_SZ,		/* maxsize */
	    1,				/* nsegments */
	    ALE_TX_RING_SZ,		/* maxsegsize */
	    0,				/* flags */
	    &sc->ale_cdata.ale_tx_ring_tag);
	if (error != 0) {
		device_printf(sc->ale_dev,
		    "could not create Tx ring DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Rx pages. */
	for (i = 0; i < ALE_RX_PAGES; i++) {
		error = bus_dma_tag_create(
		    sc->ale_cdata.ale_parent_tag, /* parent */
		    ALE_RX_PAGE_ALIGN, 0,	/* alignment, boundary */
		    BUS_SPACE_MAXADDR,		/* lowaddr */
		    BUS_SPACE_MAXADDR,		/* highaddr */
		    NULL, NULL,			/* filter, filterarg */
		    sc->ale_pagesize,		/* maxsize */
		    1,				/* nsegments */
		    sc->ale_pagesize,		/* maxsegsize */
		    0,				/* flags */
		    &sc->ale_cdata.ale_rx_page[i].page_tag);
		if (error != 0) {
			device_printf(sc->ale_dev,
			    "could not create Rx page %d DMA tag.\n", i);
			goto fail;
		}
	}

	/* Create DMA tag for Tx coalescing message block. */
	error = bus_dma_tag_create(
	    sc->ale_cdata.ale_parent_tag, /* parent */
	    ALE_CMB_ALIGN, 0,		/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    ALE_TX_CMB_SZ,		/* maxsize */
	    1,				/* nsegments */
	    ALE_TX_CMB_SZ,		/* maxsegsize */
	    0,				/* flags */
	    &sc->ale_cdata.ale_tx_cmb_tag);
	if (error != 0) {
		device_printf(sc->ale_dev,
		    "could not create Tx CMB DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Rx coalescing message block. */
	for (i = 0; i < ALE_RX_PAGES; i++) {
		error = bus_dma_tag_create(
		    sc->ale_cdata.ale_parent_tag, /* parent */
		    ALE_CMB_ALIGN, 0,		/* alignment, boundary */
		    BUS_SPACE_MAXADDR,		/* lowaddr */
		    BUS_SPACE_MAXADDR,		/* highaddr */
		    NULL, NULL,			/* filter, filterarg */
		    ALE_RX_CMB_SZ,		/* maxsize */
		    1,				/* nsegments */
		    ALE_RX_CMB_SZ,		/* maxsegsize */
		    0,				/* flags */
		    &sc->ale_cdata.ale_rx_page[i].cmb_tag);
		if (error != 0) {
			device_printf(sc->ale_dev,
			    "could not create Rx page %d CMB DMA tag.\n", i);
			goto fail;
		}
	}

	/* Allocate DMA'able memory and load the DMA map for Tx ring. */
	error = bus_dmamem_alloc(sc->ale_cdata.ale_tx_ring_tag,
	    (void **)&sc->ale_cdata.ale_tx_ring,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO,
	    &sc->ale_cdata.ale_tx_ring_map);
	if (error != 0) {
		device_printf(sc->ale_dev,
		    "could not allocate DMA'able memory for Tx ring.\n");
		goto fail;
	}
	ctx.ale_busaddr = 0;
	error = bus_dmamap_load(sc->ale_cdata.ale_tx_ring_tag,
	    sc->ale_cdata.ale_tx_ring_map, sc->ale_cdata.ale_tx_ring,
	    ALE_TX_RING_SZ, ale_dmamap_cb, &ctx, 0);
	if (error != 0 || ctx.ale_busaddr == 0) {
		device_printf(sc->ale_dev,
		    "could not load DMA'able memory for Tx ring.\n");
		goto fail;
	}
	sc->ale_cdata.ale_tx_ring_paddr = ctx.ale_busaddr;

	/* Rx pages. */
	for (i = 0; i < ALE_RX_PAGES; i++) {
		error = bus_dmamem_alloc(sc->ale_cdata.ale_rx_page[i].page_tag,
		    (void **)&sc->ale_cdata.ale_rx_page[i].page_addr,
		    BUS_DMA_WAITOK | BUS_DMA_ZERO,
		    &sc->ale_cdata.ale_rx_page[i].page_map);
		if (error != 0) {
			device_printf(sc->ale_dev,
			    "could not allocate DMA'able memory for "
			    "Rx page %d.\n", i);
			goto fail;
		}
		ctx.ale_busaddr = 0;
		error = bus_dmamap_load(sc->ale_cdata.ale_rx_page[i].page_tag,
		    sc->ale_cdata.ale_rx_page[i].page_map,
		    sc->ale_cdata.ale_rx_page[i].page_addr,
		    sc->ale_pagesize, ale_dmamap_cb, &ctx, 0);
		if (error != 0 || ctx.ale_busaddr == 0) {
			device_printf(sc->ale_dev,
			    "could not load DMA'able memory for "
			    "Rx page %d.\n", i);
			goto fail;
		}
		sc->ale_cdata.ale_rx_page[i].page_paddr = ctx.ale_busaddr;
	}

	/* Tx CMB. */
	error = bus_dmamem_alloc(sc->ale_cdata.ale_tx_cmb_tag,
	    (void **)&sc->ale_cdata.ale_tx_cmb,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO,
	    &sc->ale_cdata.ale_tx_cmb_map);
	if (error != 0) {
		device_printf(sc->ale_dev,
		    "could not allocate DMA'able memory for Tx CMB.\n");
		goto fail;
	}
	ctx.ale_busaddr = 0;
	error = bus_dmamap_load(sc->ale_cdata.ale_tx_cmb_tag,
	    sc->ale_cdata.ale_tx_cmb_map, sc->ale_cdata.ale_tx_cmb,
	    ALE_TX_CMB_SZ, ale_dmamap_cb, &ctx, 0);
	if (error != 0 || ctx.ale_busaddr == 0) {
		device_printf(sc->ale_dev,
		    "could not load DMA'able memory for Tx CMB.\n");
		goto fail;
	}
	sc->ale_cdata.ale_tx_cmb_paddr = ctx.ale_busaddr;

	/* Rx CMB. */
	for (i = 0; i < ALE_RX_PAGES; i++) {
		error = bus_dmamem_alloc(sc->ale_cdata.ale_rx_page[i].cmb_tag,
		    (void **)&sc->ale_cdata.ale_rx_page[i].cmb_addr,
		    BUS_DMA_WAITOK | BUS_DMA_ZERO,
		    &sc->ale_cdata.ale_rx_page[i].cmb_map);
		if (error != 0) {
			device_printf(sc->ale_dev, "could not allocate "
			    "DMA'able memory for Rx page %d CMB.\n", i);
			goto fail;
		}
		ctx.ale_busaddr = 0;
		error = bus_dmamap_load(sc->ale_cdata.ale_rx_page[i].cmb_tag,
		    sc->ale_cdata.ale_rx_page[i].cmb_map,
		    sc->ale_cdata.ale_rx_page[i].cmb_addr,
		    ALE_RX_CMB_SZ, ale_dmamap_cb, &ctx, 0);
		if (error != 0 || ctx.ale_busaddr == 0) {
			device_printf(sc->ale_dev, "could not load DMA'able "
			    "memory for Rx page %d CMB.\n", i);
			goto fail;
		}
		sc->ale_cdata.ale_rx_page[i].cmb_paddr = ctx.ale_busaddr;
	}

	/*
	 * Tx descriptors/RXF0/CMB DMA blocks share the same
	 * high address region of 64bit DMA address space.
	 */
	if (lowaddr != BUS_SPACE_MAXADDR_32BIT &&
	    (error = ale_check_boundary(sc)) != 0) {
		device_printf(sc->ale_dev, "4GB boundary crossed, "
		    "switching to 32bit DMA addressing mode.\n");
		ale_dma_free(sc);
		/*
		 * Limit max allowable DMA address space to 32bit
		 * and try again.
		 */
		lowaddr = BUS_SPACE_MAXADDR_32BIT;
		goto again;
	}

	/*
	 * Create Tx buffer parent tag.
	 * AR81xx allows 64bit DMA addressing of Tx buffers so it
	 * needs separate parent DMA tag as parent DMA address space
	 * could be restricted to be within 32bit address space by
	 * 4GB boundary crossing.
	 */
	error = bus_dma_tag_create(
	    NULL,			/* parent */
	    1, 0,			/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    BUS_SPACE_MAXSIZE_32BIT,	/* maxsize */
	    0,				/* nsegments */
	    BUS_SPACE_MAXSIZE_32BIT,	/* maxsegsize */
	    0,				/* flags */
	    &sc->ale_cdata.ale_buffer_tag);
	if (error != 0) {
		device_printf(sc->ale_dev,
		    "could not create parent buffer DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Tx buffers. */
	error = bus_dma_tag_create(
	    sc->ale_cdata.ale_buffer_tag, /* parent */
	    1, 0,			/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    ALE_TSO_MAXSIZE,		/* maxsize */
	    ALE_MAXTXSEGS,		/* nsegments */
	    ALE_TSO_MAXSEGSIZE,		/* maxsegsize */
	    0,				/* flags */
	    &sc->ale_cdata.ale_tx_tag);
	if (error != 0) {
		device_printf(sc->ale_dev, "could not create Tx DMA tag.\n");
		goto fail;
	}

	/* Create DMA maps for Tx buffers. */
	for (i = 0; i < ALE_TX_RING_CNT; i++) {
		txd = &sc->ale_cdata.ale_txdesc[i];
		txd->tx_m = NULL;
		txd->tx_dmamap = NULL;
		error = bus_dmamap_create(sc->ale_cdata.ale_tx_tag, 0,
		    &txd->tx_dmamap);
		if (error != 0) {
			device_printf(sc->ale_dev,
			    "could not create Tx dmamap.\n");
			goto fail;
		}
	}
fail:
	return (error);
}

static void
ale_dma_free(struct ale_softc *sc)
{
	struct ale_txdesc *txd;
	int i;

	/* Tx buffers. */
	if (sc->ale_cdata.ale_tx_tag != NULL) {
		for (i = 0; i < ALE_TX_RING_CNT; i++) {
			txd = &sc->ale_cdata.ale_txdesc[i];
			if (txd->tx_dmamap != NULL) {
				bus_dmamap_destroy(sc->ale_cdata.ale_tx_tag,
				    txd->tx_dmamap);
				txd->tx_dmamap = NULL;
			}
		}
		bus_dma_tag_destroy(sc->ale_cdata.ale_tx_tag);
		sc->ale_cdata.ale_tx_tag = NULL;
	}
	/* Tx descriptor ring. */
	if (sc->ale_cdata.ale_tx_ring_tag != NULL) {
		if (sc->ale_cdata.ale_tx_ring_map != NULL)
			bus_dmamap_unload(sc->ale_cdata.ale_tx_ring_tag,
			    sc->ale_cdata.ale_tx_ring_map);
		if (sc->ale_cdata.ale_tx_ring_map != NULL &&
		    sc->ale_cdata.ale_tx_ring != NULL)
			bus_dmamem_free(sc->ale_cdata.ale_tx_ring_tag,
			    sc->ale_cdata.ale_tx_ring,
			    sc->ale_cdata.ale_tx_ring_map);
		sc->ale_cdata.ale_tx_ring = NULL;
		sc->ale_cdata.ale_tx_ring_map = NULL;
		bus_dma_tag_destroy(sc->ale_cdata.ale_tx_ring_tag);
		sc->ale_cdata.ale_tx_ring_tag = NULL;
	}
	/* Rx page block. */
	for (i = 0; i < ALE_RX_PAGES; i++) {
		if (sc->ale_cdata.ale_rx_page[i].page_tag != NULL) {
			if (sc->ale_cdata.ale_rx_page[i].page_map != NULL)
				bus_dmamap_unload(
				    sc->ale_cdata.ale_rx_page[i].page_tag,
				    sc->ale_cdata.ale_rx_page[i].page_map);
			if (sc->ale_cdata.ale_rx_page[i].page_map != NULL &&
			    sc->ale_cdata.ale_rx_page[i].page_addr != NULL)
				bus_dmamem_free(
				    sc->ale_cdata.ale_rx_page[i].page_tag,
				    sc->ale_cdata.ale_rx_page[i].page_addr,
				    sc->ale_cdata.ale_rx_page[i].page_map);
			sc->ale_cdata.ale_rx_page[i].page_addr = NULL;
			sc->ale_cdata.ale_rx_page[i].page_map = NULL;
			bus_dma_tag_destroy(
			    sc->ale_cdata.ale_rx_page[i].page_tag);
			sc->ale_cdata.ale_rx_page[i].page_tag = NULL;
		}
	}
	/* Rx CMB. */
	for (i = 0; i < ALE_RX_PAGES; i++) {
		if (sc->ale_cdata.ale_rx_page[i].cmb_tag != NULL) {
			if (sc->ale_cdata.ale_rx_page[i].cmb_map != NULL)
				bus_dmamap_unload(
				    sc->ale_cdata.ale_rx_page[i].cmb_tag,
				    sc->ale_cdata.ale_rx_page[i].cmb_map);
			if (sc->ale_cdata.ale_rx_page[i].cmb_map != NULL &&
			    sc->ale_cdata.ale_rx_page[i].cmb_addr != NULL)
				bus_dmamem_free(
				    sc->ale_cdata.ale_rx_page[i].cmb_tag,
				    sc->ale_cdata.ale_rx_page[i].cmb_addr,
				    sc->ale_cdata.ale_rx_page[i].cmb_map);
			sc->ale_cdata.ale_rx_page[i].cmb_addr = NULL;
			sc->ale_cdata.ale_rx_page[i].cmb_map = NULL;
			bus_dma_tag_destroy(
			    sc->ale_cdata.ale_rx_page[i].cmb_tag);
			sc->ale_cdata.ale_rx_page[i].cmb_tag = NULL;
		}
	}
	/* Tx CMB. */
	if (sc->ale_cdata.ale_tx_cmb_tag != NULL) {
		if (sc->ale_cdata.ale_tx_cmb_map != NULL)
			bus_dmamap_unload(sc->ale_cdata.ale_tx_cmb_tag,
			    sc->ale_cdata.ale_tx_cmb_map);
		if (sc->ale_cdata.ale_tx_cmb_map != NULL &&
		    sc->ale_cdata.ale_tx_cmb != NULL)
			bus_dmamem_free(sc->ale_cdata.ale_tx_cmb_tag,
			    sc->ale_cdata.ale_tx_cmb,
			    sc->ale_cdata.ale_tx_cmb_map);
		sc->ale_cdata.ale_tx_cmb = NULL;
		sc->ale_cdata.ale_tx_cmb_map = NULL;
		bus_dma_tag_destroy(sc->ale_cdata.ale_tx_cmb_tag);
		sc->ale_cdata.ale_tx_cmb_tag = NULL;
	}
	if (sc->ale_cdata.ale_buffer_tag != NULL) {
		bus_dma_tag_destroy(sc->ale_cdata.ale_buffer_tag);
		sc->ale_cdata.ale_buffer_tag = NULL;
	}
	if (sc->ale_cdata.ale_parent_tag != NULL) {
		bus_dma_tag_destroy(sc->ale_cdata.ale_parent_tag);
		sc->ale_cdata.ale_parent_tag = NULL;
	}
}

static int
ale_shutdown(device_t dev)
{
	return (ale_suspend(dev));
}

#ifdef notyet

/*
 * Note, this driver resets the link speed to 10/100Mbps by
 * restarting auto-negotiation in suspend/shutdown phase but we
 * don't know whether that auto-negotiation would succeed or not
 * as driver has no control after powering off/suspend operation.
 * If the renegotiation fail WOL may not work. Running at 1Gbps
 * will draw more power than 375mA at 3.3V which is specified in
 * PCI specification and that would result in complete
 * shutdowning power to ethernet controller.
 *
 * TODO
 * Save current negotiated media speed/duplex/flow-control to
 * softc and restore the same link again after resuming. PHY
 * handling such as power down/resetting to 100Mbps may be better
 * handled in suspend method in phy driver.
 */
static void
ale_setlinkspeed(struct ale_softc *sc)
{
	struct mii_data *mii;
	int aneg, i;

	mii = device_get_softc(sc->ale_miibus);
	mii_pollstat(mii);
	aneg = 0;
	if ((mii->mii_media_status & (IFM_ACTIVE | IFM_AVALID)) ==
	    (IFM_ACTIVE | IFM_AVALID)) {
		switch IFM_SUBTYPE(mii->mii_media_active) {
		case IFM_10_T:
		case IFM_100_TX:
			return;
		case IFM_1000_T:
			aneg++;
			break;
		default:
			break;
		}
	}
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr, MII_100T2CR, 0);
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    MII_ANAR, ANAR_TX_FD | ANAR_TX | ANAR_10_FD | ANAR_10 | ANAR_CSMA);
	ale_miibus_writereg(sc->ale_dev, sc->ale_phyaddr,
	    MII_BMCR, BMCR_RESET | BMCR_AUTOEN | BMCR_STARTNEG);
	DELAY(1000);
	if (aneg != 0) {
		/*
		 * Poll link state until ale(4) get a 10/100Mbps link.
		 */
		for (i = 0; i < MII_ANEGTICKS_GIGE; i++) {
			mii_pollstat(mii);
			if ((mii->mii_media_status & (IFM_ACTIVE | IFM_AVALID))
			    == (IFM_ACTIVE | IFM_AVALID)) {
				switch (IFM_SUBTYPE(
				    mii->mii_media_active)) {
				case IFM_10_T:
				case IFM_100_TX:
					ale_mac_config(sc);
					return;
				default:
					break;
				}
			}
			ALE_UNLOCK(sc);
			pause("alelnk", hz);
			ALE_LOCK(sc);
		}
		if (i == MII_ANEGTICKS_GIGE)
			device_printf(sc->ale_dev,
			    "establishing a link failed, WOL may not work!");
	}
	/*
	 * No link, force MAC to have 100Mbps, full-duplex link.
	 * This is the last resort and may/may not work.
	 */
	mii->mii_media_status = IFM_AVALID | IFM_ACTIVE;
	mii->mii_media_active = IFM_ETHER | IFM_100_TX | IFM_FDX;
	ale_mac_config(sc);
}

static void
ale_setwol(struct ale_softc *sc)
{
	struct ifnet *ifp;
	uint32_t reg, pmcs;
	uint16_t pmstat;
	int pmc;

	ALE_LOCK_ASSERT(sc);

	if (pci_find_extcap(sc->ale_dev, PCIY_PMG, &pmc) != 0) {
		/* Disable WOL. */
		CSR_WRITE_4(sc, ALE_WOL_CFG, 0);
		reg = CSR_READ_4(sc, ALE_PCIE_PHYMISC);
		reg |= PCIE_PHYMISC_FORCE_RCV_DET;
		CSR_WRITE_4(sc, ALE_PCIE_PHYMISC, reg);
		/* Force PHY power down. */
		CSR_WRITE_2(sc, ALE_GPHY_CTRL,
		    GPHY_CTRL_EXT_RESET | GPHY_CTRL_HIB_EN |
		    GPHY_CTRL_HIB_PULSE | GPHY_CTRL_PHY_PLL_ON |
		    GPHY_CTRL_SEL_ANA_RESET | GPHY_CTRL_PHY_IDDQ |
		    GPHY_CTRL_PCLK_SEL_DIS | GPHY_CTRL_PWDOWN_HW);
		return;
	}

	ifp = sc->ale_ifp;
	if ((ifp->if_capenable & IFCAP_WOL) != 0) {
		if ((sc->ale_flags & ALE_FLAG_FASTETHER) == 0)
			ale_setlinkspeed(sc);
	}

	pmcs = 0;
	if ((ifp->if_capenable & IFCAP_WOL_MAGIC) != 0)
		pmcs |= WOL_CFG_MAGIC | WOL_CFG_MAGIC_ENB;
	CSR_WRITE_4(sc, ALE_WOL_CFG, pmcs);
	reg = CSR_READ_4(sc, ALE_MAC_CFG);
	reg &= ~(MAC_CFG_DBG | MAC_CFG_PROMISC | MAC_CFG_ALLMULTI |
	    MAC_CFG_BCAST);
	if ((ifp->if_capenable & IFCAP_WOL_MCAST) != 0)
		reg |= MAC_CFG_ALLMULTI | MAC_CFG_BCAST;
	if ((ifp->if_capenable & IFCAP_WOL) != 0)
		reg |= MAC_CFG_RX_ENB;
	CSR_WRITE_4(sc, ALE_MAC_CFG, reg);

	if ((ifp->if_capenable & IFCAP_WOL) == 0) {
		/* WOL disabled, PHY power down. */
		reg = CSR_READ_4(sc, ALE_PCIE_PHYMISC);
		reg |= PCIE_PHYMISC_FORCE_RCV_DET;
		CSR_WRITE_4(sc, ALE_PCIE_PHYMISC, reg);
		CSR_WRITE_2(sc, ALE_GPHY_CTRL,
		    GPHY_CTRL_EXT_RESET | GPHY_CTRL_HIB_EN |
		    GPHY_CTRL_HIB_PULSE | GPHY_CTRL_SEL_ANA_RESET |
		    GPHY_CTRL_PHY_IDDQ | GPHY_CTRL_PCLK_SEL_DIS |
		    GPHY_CTRL_PWDOWN_HW);
	}
	/* Request PME. */
	pmstat = pci_read_config(sc->ale_dev, pmc + PCIR_POWER_STATUS, 2);
	pmstat &= ~(PCIM_PSTAT_PME | PCIM_PSTAT_PMEENABLE);
	if ((ifp->if_capenable & IFCAP_WOL) != 0)
		pmstat |= PCIM_PSTAT_PME | PCIM_PSTAT_PMEENABLE;
	pci_write_config(sc->ale_dev, pmc + PCIR_POWER_STATUS, pmstat, 2);
}

#endif	/* notyet */

static int
ale_suspend(device_t dev)
{
	struct ale_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);
	ale_stop(sc);
#ifdef notyet
	ale_setwol(sc);
#endif
	lwkt_serialize_exit(ifp->if_serializer);
	return (0);
}

static int
ale_resume(device_t dev)
{
	struct ale_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint16_t cmd;

	lwkt_serialize_enter(ifp->if_serializer);

	/*
	 * Clear INTx emulation disable for hardwares that
	 * is set in resume event. From Linux.
	 */
	cmd = pci_read_config(sc->ale_dev, PCIR_COMMAND, 2);
	if ((cmd & 0x0400) != 0) {
		cmd &= ~0x0400;
		pci_write_config(sc->ale_dev, PCIR_COMMAND, cmd, 2);
	}

#ifdef notyet
	if (pci_find_extcap(sc->ale_dev, PCIY_PMG, &pmc) == 0) {
		uint16_t pmstat;
		int pmc;

		/* Disable PME and clear PME status. */
		pmstat = pci_read_config(sc->ale_dev,
		    pmc + PCIR_POWER_STATUS, 2);
		if ((pmstat & PCIM_PSTAT_PMEENABLE) != 0) {
			pmstat &= ~PCIM_PSTAT_PMEENABLE;
			pci_write_config(sc->ale_dev,
			    pmc + PCIR_POWER_STATUS, pmstat, 2);
		}
	}
#endif

	/* Reset PHY. */
	ale_phy_reset(sc);
	if ((ifp->if_flags & IFF_UP) != 0)
		ale_init(sc);

	lwkt_serialize_exit(ifp->if_serializer);
	return (0);
}

static int
ale_encap(struct ale_softc *sc, struct mbuf **m_head)
{
	struct ale_txdesc *txd, *txd_last;
	struct tx_desc *desc;
	struct mbuf *m;
	bus_dma_segment_t txsegs[ALE_MAXTXSEGS];
	struct ale_dmamap_ctx ctx;
	bus_dmamap_t map;
	uint32_t cflags, poff, vtag;
	int error, i, nsegs, prod;

	M_ASSERTPKTHDR((*m_head));

	m = *m_head;
	cflags = vtag = 0;
	poff = 0;

	prod = sc->ale_cdata.ale_tx_prod;
	txd = &sc->ale_cdata.ale_txdesc[prod];
	txd_last = txd;
	map = txd->tx_dmamap;

	ctx.nsegs = ALE_MAXTXSEGS;
	ctx.segs = txsegs;
	error =  bus_dmamap_load_mbuf(sc->ale_cdata.ale_tx_tag, map,
				      *m_head, ale_dmamap_buf_cb, &ctx,
				      BUS_DMA_NOWAIT);
	if (error == EFBIG) {
		m = m_defrag(*m_head, M_NOWAIT);
		if (m == NULL) {
			m_freem(*m_head);
			*m_head = NULL;
			return (ENOMEM);
		}
		*m_head = m;

		ctx.nsegs = ALE_MAXTXSEGS;
		ctx.segs = txsegs;
		error =  bus_dmamap_load_mbuf(sc->ale_cdata.ale_tx_tag, map,
					      *m_head, ale_dmamap_buf_cb, &ctx,
					      BUS_DMA_NOWAIT);
		if (error != 0) {
			m_freem(*m_head);
			*m_head = NULL;
			return (error);
		}
	} else if (error != 0) {
		return (error);
	}
	nsegs = ctx.nsegs;

	if (nsegs == 0) {
		m_freem(*m_head);
		*m_head = NULL;
		return (EIO);
	}

	/* Check descriptor overrun. */
	if (sc->ale_cdata.ale_tx_cnt + nsegs >= ALE_TX_RING_CNT - 2) {
		bus_dmamap_unload(sc->ale_cdata.ale_tx_tag, map);
		return (ENOBUFS);
	}
	bus_dmamap_sync(sc->ale_cdata.ale_tx_tag, map, BUS_DMASYNC_PREWRITE);

	m = *m_head;
	/* Configure Tx checksum offload. */
	if ((m->m_pkthdr.csum_flags & ALE_CSUM_FEATURES) != 0) {
		/*
		 * AR81xx supports Tx custom checksum offload feature
		 * that offloads single 16bit checksum computation.
		 * So you can choose one among IP, TCP and UDP.
		 * Normally driver sets checksum start/insertion
		 * position from the information of TCP/UDP frame as
		 * TCP/UDP checksum takes more time than that of IP.
		 * However it seems that custom checksum offload
		 * requires 4 bytes aligned Tx buffers due to hardware
		 * bug.
		 * AR81xx also supports explicit Tx checksum computation
		 * if it is told that the size of IP header and TCP
		 * header(for UDP, the header size does not matter
		 * because it's fixed length). However with this scheme
		 * TSO does not work so you have to choose one either
		 * TSO or explicit Tx checksum offload. I chosen TSO
		 * plus custom checksum offload with work-around which
		 * will cover most common usage for this consumer
		 * ethernet controller. The work-around takes a lot of
		 * CPU cycles if Tx buffer is not aligned on 4 bytes
		 * boundary, though.
		 */
		cflags |= ALE_TD_CXSUM;
		/* Set checksum start offset. */
		cflags |= (poff << ALE_TD_CSUM_PLOADOFFSET_SHIFT);
		/* Set checksum insertion position of TCP/UDP. */
		cflags |= ((poff + m->m_pkthdr.csum_data) <<
		    ALE_TD_CSUM_XSUMOFFSET_SHIFT);
	}

	/* Configure VLAN hardware tag insertion. */
	if ((m->m_flags & M_VLANTAG) != 0) {
		vtag = ALE_TX_VLAN_TAG(m->m_pkthdr.ether_vlantag);
		vtag = ((vtag << ALE_TD_VLAN_SHIFT) & ALE_TD_VLAN_MASK);
		cflags |= ALE_TD_INSERT_VLAN_TAG;
	}

	desc = NULL;
	for (i = 0; i < nsegs; i++) {
		desc = &sc->ale_cdata.ale_tx_ring[prod];
		desc->addr = htole64(txsegs[i].ds_addr);
		desc->len = htole32(ALE_TX_BYTES(txsegs[i].ds_len) | vtag);
		desc->flags = htole32(cflags);
		sc->ale_cdata.ale_tx_cnt++;
		ALE_DESC_INC(prod, ALE_TX_RING_CNT);
	}
	/* Update producer index. */
	sc->ale_cdata.ale_tx_prod = prod;

	/* Finally set EOP on the last descriptor. */
	prod = (prod + ALE_TX_RING_CNT - 1) % ALE_TX_RING_CNT;
	desc = &sc->ale_cdata.ale_tx_ring[prod];
	desc->flags |= htole32(ALE_TD_EOP);

	/* Swap dmamap of the first and the last. */
	txd = &sc->ale_cdata.ale_txdesc[prod];
	map = txd_last->tx_dmamap;
	txd_last->tx_dmamap = txd->tx_dmamap;
	txd->tx_dmamap = map;
	txd->tx_m = m;

	/* Sync descriptors. */
	bus_dmamap_sync(sc->ale_cdata.ale_tx_ring_tag,
	    sc->ale_cdata.ale_tx_ring_map, BUS_DMASYNC_PREWRITE);

	return (0);
}

static void
ale_start(struct ifnet *ifp, struct ifaltq_subque *ifsq)
{
        struct ale_softc *sc = ifp->if_softc;
	struct mbuf *m_head;
	int enq;

	ASSERT_ALTQ_SQ_DEFAULT(ifp, ifsq);
	ASSERT_SERIALIZED(ifp->if_serializer);

	if ((sc->ale_flags & ALE_FLAG_LINK) == 0) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	if ((ifp->if_flags & IFF_RUNNING) == 0 || ifq_is_oactive(&ifp->if_snd))
		return;

	/* Reclaim transmitted frames. */
	if (sc->ale_cdata.ale_tx_cnt >= ALE_TX_DESC_HIWAT)
		ale_txeof(sc);

	enq = 0;
	while (!ifq_is_empty(&ifp->if_snd)) {
		m_head = ifq_dequeue(&ifp->if_snd);
		if (m_head == NULL)
			break;

		/*
		 * Pack the data into the transmit ring. If we
		 * don't have room, set the OACTIVE flag and wait
		 * for the NIC to drain the ring.
		 */
		if (ale_encap(sc, &m_head)) {
			if (m_head == NULL)
				break;
			ifq_prepend(&ifp->if_snd, m_head);
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		enq = 1;

		/*
		 * If there's a BPF listener, bounce a copy of this frame
		 * to him.
		 */
		ETHER_BPF_MTAP(ifp, m_head);
	}

	if (enq) {
		/* Kick. */
		CSR_WRITE_4(sc, ALE_MBOX_TPD_PROD_IDX,
		    sc->ale_cdata.ale_tx_prod);

		/* Set a timeout in case the chip goes out to lunch. */
		ifp->if_timer = ALE_TX_TIMEOUT;
	}
}

static void
ale_watchdog(struct ifnet *ifp)
{
	struct ale_softc *sc = ifp->if_softc;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if ((sc->ale_flags & ALE_FLAG_LINK) == 0) {
		if_printf(ifp, "watchdog timeout (lost link)\n");
		IFNET_STAT_INC(ifp, oerrors, 1);
		ale_init(sc);
		return;
	}

	if_printf(ifp, "watchdog timeout -- resetting\n");
	IFNET_STAT_INC(ifp, oerrors, 1);
	ale_init(sc);

	if (!ifq_is_empty(&ifp->if_snd))
		if_devstart(ifp);
}

static int
ale_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data, struct ucred *cr)
{
	struct ale_softc *sc;
	struct ifreq *ifr;
	struct mii_data *mii;
	int error, mask;

	ASSERT_SERIALIZED(ifp->if_serializer);

	sc = ifp->if_softc;
	ifr = (struct ifreq *)data;
	error = 0;

	switch (cmd) {
	case SIOCSIFMTU:
		if (ifr->ifr_mtu < ETHERMIN || ifr->ifr_mtu > ALE_JUMBO_MTU ||
		    ((sc->ale_flags & ALE_FLAG_JUMBO) == 0 &&
		    ifr->ifr_mtu > ETHERMTU))
			error = EINVAL;
		else if (ifp->if_mtu != ifr->ifr_mtu) {
			ifp->if_mtu = ifr->ifr_mtu;
			if ((ifp->if_flags & IFF_RUNNING) != 0)
				ale_init(sc);
		}
		break;

	case SIOCSIFFLAGS:
		if ((ifp->if_flags & IFF_UP) != 0) {
			if ((ifp->if_flags & IFF_RUNNING) != 0) {
				if (((ifp->if_flags ^ sc->ale_if_flags)
				    & (IFF_PROMISC | IFF_ALLMULTI)) != 0)
					ale_rxfilter(sc);
			} else {
				if ((sc->ale_flags & ALE_FLAG_DETACH) == 0)
					ale_init(sc);
			}
		} else {
			if ((ifp->if_flags & IFF_RUNNING) != 0)
				ale_stop(sc);
		}
		sc->ale_if_flags = ifp->if_flags;
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if ((ifp->if_flags & IFF_RUNNING) != 0)
			ale_rxfilter(sc);
		break;

	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		mii = device_get_softc(sc->ale_miibus);
		error = ifmedia_ioctl(ifp, ifr, &mii->mii_media, cmd);
		break;

	case SIOCSIFCAP:
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;
		if ((mask & IFCAP_TXCSUM) != 0 &&
		    (ifp->if_capabilities & IFCAP_TXCSUM) != 0) {
			ifp->if_capenable ^= IFCAP_TXCSUM;
			if ((ifp->if_capenable & IFCAP_TXCSUM) != 0)
				ifp->if_hwassist |= ALE_CSUM_FEATURES;
			else
				ifp->if_hwassist &= ~ALE_CSUM_FEATURES;
		}
		if ((mask & IFCAP_RXCSUM) != 0 &&
		    (ifp->if_capabilities & IFCAP_RXCSUM) != 0)
			ifp->if_capenable ^= IFCAP_RXCSUM;

		if ((mask & IFCAP_VLAN_HWTAGGING) != 0 &&
		    (ifp->if_capabilities & IFCAP_VLAN_HWTAGGING) != 0) {
			ifp->if_capenable ^= IFCAP_VLAN_HWTAGGING;
			ale_rxvlan(sc);
		}
		break;

	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}
	return (error);
}

static void
ale_mac_config(struct ale_softc *sc)
{
	struct mii_data *mii;
	uint32_t reg;

	mii = device_get_softc(sc->ale_miibus);
	reg = CSR_READ_4(sc, ALE_MAC_CFG);
	reg &= ~(MAC_CFG_FULL_DUPLEX | MAC_CFG_TX_FC | MAC_CFG_RX_FC |
	    MAC_CFG_SPEED_MASK);
	/* Reprogram MAC with resolved speed/duplex. */
	switch (IFM_SUBTYPE(mii->mii_media_active)) {
	case IFM_10_T:
	case IFM_100_TX:
		reg |= MAC_CFG_SPEED_10_100;
		break;
	case IFM_1000_T:
		reg |= MAC_CFG_SPEED_1000;
		break;
	}
	if ((IFM_OPTIONS(mii->mii_media_active) & IFM_FDX) != 0) {
		reg |= MAC_CFG_FULL_DUPLEX;
#ifdef notyet
		if ((IFM_OPTIONS(mii->mii_media_active) & IFM_ETH_TXPAUSE) != 0)
			reg |= MAC_CFG_TX_FC;
		if ((IFM_OPTIONS(mii->mii_media_active) & IFM_ETH_RXPAUSE) != 0)
			reg |= MAC_CFG_RX_FC;
#endif
	}
	CSR_WRITE_4(sc, ALE_MAC_CFG, reg);
}

static void
ale_stats_clear(struct ale_softc *sc)
{
	struct smb sb;
	uint32_t *reg;
	int i;

	for (reg = &sb.rx_frames, i = 0; reg <= &sb.rx_pkts_filtered; reg++) {
		CSR_READ_4(sc, ALE_RX_MIB_BASE + i);
		i += sizeof(uint32_t);
	}
	/* Read Tx statistics. */
	for (reg = &sb.tx_frames, i = 0; reg <= &sb.tx_mcast_bytes; reg++) {
		CSR_READ_4(sc, ALE_TX_MIB_BASE + i);
		i += sizeof(uint32_t);
	}
}

static void
ale_stats_update(struct ale_softc *sc)
{
	struct ale_hw_stats *stat;
	struct smb sb, *smb;
	struct ifnet *ifp;
	uint32_t *reg;
	int i;

	ifp = &sc->arpcom.ac_if;
	stat = &sc->ale_stats;
	smb = &sb;

	/* Read Rx statistics. */
	for (reg = &sb.rx_frames, i = 0; reg <= &sb.rx_pkts_filtered; reg++) {
		*reg = CSR_READ_4(sc, ALE_RX_MIB_BASE + i);
		i += sizeof(uint32_t);
	}
	/* Read Tx statistics. */
	for (reg = &sb.tx_frames, i = 0; reg <= &sb.tx_mcast_bytes; reg++) {
		*reg = CSR_READ_4(sc, ALE_TX_MIB_BASE + i);
		i += sizeof(uint32_t);
	}

	/* Rx stats. */
	stat->rx_frames += smb->rx_frames;
	stat->rx_bcast_frames += smb->rx_bcast_frames;
	stat->rx_mcast_frames += smb->rx_mcast_frames;
	stat->rx_pause_frames += smb->rx_pause_frames;
	stat->rx_control_frames += smb->rx_control_frames;
	stat->rx_crcerrs += smb->rx_crcerrs;
	stat->rx_lenerrs += smb->rx_lenerrs;
	stat->rx_bytes += smb->rx_bytes;
	stat->rx_runts += smb->rx_runts;
	stat->rx_fragments += smb->rx_fragments;
	stat->rx_pkts_64 += smb->rx_pkts_64;
	stat->rx_pkts_65_127 += smb->rx_pkts_65_127;
	stat->rx_pkts_128_255 += smb->rx_pkts_128_255;
	stat->rx_pkts_256_511 += smb->rx_pkts_256_511;
	stat->rx_pkts_512_1023 += smb->rx_pkts_512_1023;
	stat->rx_pkts_1024_1518 += smb->rx_pkts_1024_1518;
	stat->rx_pkts_1519_max += smb->rx_pkts_1519_max;
	stat->rx_pkts_truncated += smb->rx_pkts_truncated;
	stat->rx_fifo_oflows += smb->rx_fifo_oflows;
	stat->rx_rrs_errs += smb->rx_rrs_errs;
	stat->rx_alignerrs += smb->rx_alignerrs;
	stat->rx_bcast_bytes += smb->rx_bcast_bytes;
	stat->rx_mcast_bytes += smb->rx_mcast_bytes;
	stat->rx_pkts_filtered += smb->rx_pkts_filtered;

	/* Tx stats. */
	stat->tx_frames += smb->tx_frames;
	stat->tx_bcast_frames += smb->tx_bcast_frames;
	stat->tx_mcast_frames += smb->tx_mcast_frames;
	stat->tx_pause_frames += smb->tx_pause_frames;
	stat->tx_excess_defer += smb->tx_excess_defer;
	stat->tx_control_frames += smb->tx_control_frames;
	stat->tx_deferred += smb->tx_deferred;
	stat->tx_bytes += smb->tx_bytes;
	stat->tx_pkts_64 += smb->tx_pkts_64;
	stat->tx_pkts_65_127 += smb->tx_pkts_65_127;
	stat->tx_pkts_128_255 += smb->tx_pkts_128_255;
	stat->tx_pkts_256_511 += smb->tx_pkts_256_511;
	stat->tx_pkts_512_1023 += smb->tx_pkts_512_1023;
	stat->tx_pkts_1024_1518 += smb->tx_pkts_1024_1518;
	stat->tx_pkts_1519_max += smb->tx_pkts_1519_max;
	stat->tx_single_colls += smb->tx_single_colls;
	stat->tx_multi_colls += smb->tx_multi_colls;
	stat->tx_late_colls += smb->tx_late_colls;
	stat->tx_excess_colls += smb->tx_excess_colls;
	stat->tx_abort += smb->tx_abort;
	stat->tx_underrun += smb->tx_underrun;
	stat->tx_desc_underrun += smb->tx_desc_underrun;
	stat->tx_lenerrs += smb->tx_lenerrs;
	stat->tx_pkts_truncated += smb->tx_pkts_truncated;
	stat->tx_bcast_bytes += smb->tx_bcast_bytes;
	stat->tx_mcast_bytes += smb->tx_mcast_bytes;

	/* Update counters in ifnet. */
	IFNET_STAT_INC(ifp, opackets, smb->tx_frames);

	IFNET_STAT_INC(ifp, collisions, smb->tx_single_colls +
	    smb->tx_multi_colls * 2 + smb->tx_late_colls +
	    smb->tx_abort * HDPX_CFG_RETRY_DEFAULT);

	/*
	 * XXX
	 * tx_pkts_truncated counter looks suspicious. It constantly
	 * increments with no sign of Tx errors. This may indicate
	 * the counter name is not correct one so I've removed the
	 * counter in output errors.
	 */
	IFNET_STAT_INC(ifp, oerrors, smb->tx_abort + smb->tx_late_colls +
	    smb->tx_underrun);

	IFNET_STAT_INC(ifp, ipackets, smb->rx_frames);

	IFNET_STAT_INC(ifp, ierrors, smb->rx_crcerrs + smb->rx_lenerrs +
	    smb->rx_runts + smb->rx_pkts_truncated +
	    smb->rx_fifo_oflows + smb->rx_rrs_errs +
	    smb->rx_alignerrs);
}

static void
ale_intr(void *xsc)
{
	struct ale_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint32_t status;

	ASSERT_SERIALIZED(ifp->if_serializer);

	status = CSR_READ_4(sc, ALE_INTR_STATUS);
	if ((status & ALE_INTRS) == 0)
		return;

	/* Acknowledge and disable interrupts. */
	CSR_WRITE_4(sc, ALE_INTR_STATUS, status | INTR_DIS_INT);

	if ((ifp->if_flags & IFF_RUNNING) != 0) {
		int error;

		error = ale_rxeof(sc);
		if (error) {
			sc->ale_stats.reset_brk_seq++;
			ale_init(sc);
			return;
		}

		if ((status & (INTR_DMA_RD_TO_RST | INTR_DMA_WR_TO_RST)) != 0) {
			if ((status & INTR_DMA_RD_TO_RST) != 0)
				device_printf(sc->ale_dev,
				    "DMA read error! -- resetting\n");
			if ((status & INTR_DMA_WR_TO_RST) != 0)
				device_printf(sc->ale_dev,
				    "DMA write error! -- resetting\n");
			ale_init(sc);
			return;
		}

		ale_txeof(sc);
		if (!ifq_is_empty(&ifp->if_snd))
			if_devstart(ifp);
	}

	/* Re-enable interrupts. */
	CSR_WRITE_4(sc, ALE_INTR_STATUS, 0x7FFFFFFF);
}

static void
ale_txeof(struct ale_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct ale_txdesc *txd;
	uint32_t cons, prod;
	int prog;

	if (sc->ale_cdata.ale_tx_cnt == 0)
		return;

	bus_dmamap_sync(sc->ale_cdata.ale_tx_ring_tag,
	    sc->ale_cdata.ale_tx_ring_map, BUS_DMASYNC_POSTREAD);
	if ((sc->ale_flags & ALE_FLAG_TXCMB_BUG) == 0) {
		bus_dmamap_sync(sc->ale_cdata.ale_tx_cmb_tag,
		    sc->ale_cdata.ale_tx_cmb_map, BUS_DMASYNC_POSTREAD);
		prod = *sc->ale_cdata.ale_tx_cmb & TPD_CNT_MASK;
	} else
		prod = CSR_READ_2(sc, ALE_TPD_CONS_IDX);
	cons = sc->ale_cdata.ale_tx_cons;
	/*
	 * Go through our Tx list and free mbufs for those
	 * frames which have been transmitted.
	 */
	for (prog = 0; cons != prod; prog++,
	     ALE_DESC_INC(cons, ALE_TX_RING_CNT)) {
		if (sc->ale_cdata.ale_tx_cnt <= 0)
			break;
		prog++;
		ifq_clr_oactive(&ifp->if_snd);
		sc->ale_cdata.ale_tx_cnt--;
		txd = &sc->ale_cdata.ale_txdesc[cons];
		if (txd->tx_m != NULL) {
			/* Reclaim transmitted mbufs. */
			bus_dmamap_unload(sc->ale_cdata.ale_tx_tag,
			    txd->tx_dmamap);
			m_freem(txd->tx_m);
			txd->tx_m = NULL;
		}
	}

	if (prog > 0) {
		sc->ale_cdata.ale_tx_cons = cons;
		/*
		 * Unarm watchdog timer only when there is no pending
		 * Tx descriptors in queue.
		 */
		if (sc->ale_cdata.ale_tx_cnt == 0)
			ifp->if_timer = 0;
	}
}

static void
ale_rx_update_page(struct ale_softc *sc, struct ale_rx_page **page,
    uint32_t length, uint32_t *prod)
{
	struct ale_rx_page *rx_page;

	rx_page = *page;
	/* Update consumer position. */
	rx_page->cons += roundup(length + sizeof(struct rx_rs),
	    ALE_RX_PAGE_ALIGN);
	if (rx_page->cons >= ALE_RX_PAGE_SZ) {
		/*
		 * End of Rx page reached, let hardware reuse
		 * this page.
		 */
		rx_page->cons = 0;
		*rx_page->cmb_addr = 0;
		bus_dmamap_sync(rx_page->cmb_tag, rx_page->cmb_map,
				BUS_DMASYNC_PREWRITE);
		CSR_WRITE_1(sc, ALE_RXF0_PAGE0 + sc->ale_cdata.ale_rx_curp,
		    RXF_VALID);
		/* Switch to alternate Rx page. */
		sc->ale_cdata.ale_rx_curp ^= 1;
		rx_page = *page =
		    &sc->ale_cdata.ale_rx_page[sc->ale_cdata.ale_rx_curp];
		/* Page flipped, sync CMB and Rx page. */
		bus_dmamap_sync(rx_page->page_tag, rx_page->page_map,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_sync(rx_page->cmb_tag, rx_page->cmb_map,
		    BUS_DMASYNC_POSTREAD);
		/* Sync completed, cache updated producer index. */
		*prod = *rx_page->cmb_addr;
	}
}


/*
 * It seems that AR81xx controller can compute partial checksum.
 * The partial checksum value can be used to accelerate checksum
 * computation for fragmented TCP/UDP packets. Upper network stack
 * already takes advantage of the partial checksum value in IP
 * reassembly stage. But I'm not sure the correctness of the
 * partial hardware checksum assistance due to lack of data sheet.
 * In addition, the Rx feature of controller that requires copying
 * for every frames effectively nullifies one of most nice offload
 * capability of controller.
 */
static void
ale_rxcsum(struct ale_softc *sc, struct mbuf *m, uint32_t status)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct ip *ip;
	char *p;

	m->m_pkthdr.csum_flags |= CSUM_IP_CHECKED;
	if ((status & ALE_RD_IPCSUM_NOK) == 0)
		m->m_pkthdr.csum_flags |= CSUM_IP_VALID;

	if ((sc->ale_flags & ALE_FLAG_RXCSUM_BUG) == 0) {
		if (((status & ALE_RD_IPV4_FRAG) == 0) &&
		    ((status & (ALE_RD_TCP | ALE_RD_UDP)) != 0) &&
		    ((status & ALE_RD_TCP_UDPCSUM_NOK) == 0)) {
			m->m_pkthdr.csum_flags |=
			    CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
			m->m_pkthdr.csum_data = 0xffff;
		}
	} else {
		if ((status & (ALE_RD_TCP | ALE_RD_UDP)) != 0 &&
		    (status & ALE_RD_TCP_UDPCSUM_NOK) == 0) {
			p = mtod(m, char *);
			p += ETHER_HDR_LEN;
			if ((status & ALE_RD_802_3) != 0)
				p += LLC_SNAPFRAMELEN;
			if ((ifp->if_capenable & IFCAP_VLAN_HWTAGGING) == 0 &&
			    (status & ALE_RD_VLAN) != 0)
				p += EVL_ENCAPLEN;
			ip = (struct ip *)p;
			if (ip->ip_off != 0 && (status & ALE_RD_IPV4_DF) == 0)
				return;
			m->m_pkthdr.csum_flags |= CSUM_DATA_VALID |
			    CSUM_PSEUDO_HDR;
			m->m_pkthdr.csum_data = 0xffff;
		}
	}
	/*
	 * Don't mark bad checksum for TCP/UDP frames
	 * as fragmented frames may always have set
	 * bad checksummed bit of frame status.
	 */
}

/* Process received frames. */
static int
ale_rxeof(struct ale_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct ale_rx_page *rx_page;
	struct rx_rs *rs;
	struct mbuf *m;
	uint32_t length, prod, seqno, status, vtags;
	int prog;

	rx_page = &sc->ale_cdata.ale_rx_page[sc->ale_cdata.ale_rx_curp];
	bus_dmamap_sync(rx_page->cmb_tag, rx_page->cmb_map,
			BUS_DMASYNC_POSTREAD);
	bus_dmamap_sync(rx_page->page_tag, rx_page->page_map,
			BUS_DMASYNC_POSTREAD);
	/*
	 * Don't directly access producer index as hardware may
	 * update it while Rx handler is in progress. It would
	 * be even better if there is a way to let hardware
	 * know how far driver processed its received frames.
	 * Alternatively, hardware could provide a way to disable
	 * CMB updates until driver acknowledges the end of CMB
	 * access.
	 */
	prod = *rx_page->cmb_addr;
	for (prog = 0; ; prog++) {
		if (rx_page->cons >= prod)
			break;
		rs = (struct rx_rs *)(rx_page->page_addr + rx_page->cons);
		seqno = ALE_RX_SEQNO(le32toh(rs->seqno));
		if (sc->ale_cdata.ale_rx_seqno != seqno) {
			/*
			 * Normally I believe this should not happen unless
			 * severe driver bug or corrupted memory. However
			 * it seems to happen under certain conditions which
			 * is triggered by abrupt Rx events such as initiation
			 * of bulk transfer of remote host. It's not easy to
			 * reproduce this and I doubt it could be related
			 * with FIFO overflow of hardware or activity of Tx
			 * CMB updates. I also remember similar behaviour
			 * seen on RealTek 8139 which uses resembling Rx
			 * scheme.
			 */
			if (bootverbose)
				device_printf(sc->ale_dev,
				    "garbled seq: %u, expected: %u -- "
				    "resetting!\n", seqno,
				    sc->ale_cdata.ale_rx_seqno);
			return (EIO);
		}
		/* Frame received. */
		sc->ale_cdata.ale_rx_seqno++;
		length = ALE_RX_BYTES(le32toh(rs->length));
		status = le32toh(rs->flags);
		if ((status & ALE_RD_ERROR) != 0) {
			/*
			 * We want to pass the following frames to upper
			 * layer regardless of error status of Rx return
			 * status.
			 *
			 *  o IP/TCP/UDP checksum is bad.
			 *  o frame length and protocol specific length
			 *     does not match.
			 */
			if ((status & (ALE_RD_CRC | ALE_RD_CODE |
			    ALE_RD_DRIBBLE | ALE_RD_RUNT | ALE_RD_OFLOW |
			    ALE_RD_TRUNC)) != 0) {
				ale_rx_update_page(sc, &rx_page, length, &prod);
				continue;
			}
		}
		/*
		 * m_devget(9) is major bottle-neck of ale(4)(It comes
		 * from hardware limitation). For jumbo frames we could
		 * get a slightly better performance if driver use
		 * m_getjcl(9) with proper buffer size argument. However
		 * that would make code more complicated and I don't
		 * think users would expect good Rx performance numbers
		 * on these low-end consumer ethernet controller.
		 */
		m = m_devget((char *)(rs + 1), length - ETHER_CRC_LEN,
		    ETHER_ALIGN, ifp, NULL);
		if (m == NULL) {
			IFNET_STAT_INC(ifp, iqdrops, 1);
			ale_rx_update_page(sc, &rx_page, length, &prod);
			continue;
		}
		if ((ifp->if_capenable & IFCAP_RXCSUM) != 0 &&
		    (status & ALE_RD_IPV4) != 0)
			ale_rxcsum(sc, m, status);
		if ((ifp->if_capenable & IFCAP_VLAN_HWTAGGING) != 0 &&
		    (status & ALE_RD_VLAN) != 0) {
			vtags = ALE_RX_VLAN(le32toh(rs->vtags));
			m->m_pkthdr.ether_vlantag = ALE_RX_VLAN_TAG(vtags);
			m->m_flags |= M_VLANTAG;
		}

		/* Pass it to upper layer. */
		ifp->if_input(ifp, m, NULL, -1);

		ale_rx_update_page(sc, &rx_page, length, &prod);
	}
	return 0;
}

static void
ale_tick(void *xsc)
{
	struct ale_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mii_data *mii;

	lwkt_serialize_enter(ifp->if_serializer);

	mii = device_get_softc(sc->ale_miibus);
	mii_tick(mii);
	ale_stats_update(sc);

	callout_reset(&sc->ale_tick_ch, hz, ale_tick, sc);

	lwkt_serialize_exit(ifp->if_serializer);
}

static void
ale_reset(struct ale_softc *sc)
{
	uint32_t reg;
	int i;

	/* Initialize PCIe module. From Linux. */
	CSR_WRITE_4(sc, 0x1008, CSR_READ_4(sc, 0x1008) | 0x8000);

	CSR_WRITE_4(sc, ALE_MASTER_CFG, MASTER_RESET);
	for (i = ALE_RESET_TIMEOUT; i > 0; i--) {
		DELAY(10);
		if ((CSR_READ_4(sc, ALE_MASTER_CFG) & MASTER_RESET) == 0)
			break;
	}
	if (i == 0)
		device_printf(sc->ale_dev, "master reset timeout!\n");

	for (i = ALE_RESET_TIMEOUT; i > 0; i--) {
		if ((reg = CSR_READ_4(sc, ALE_IDLE_STATUS)) == 0)
			break;
		DELAY(10);
	}

	if (i == 0)
		device_printf(sc->ale_dev, "reset timeout(0x%08x)!\n", reg);
}

static void
ale_init(void *xsc)
{
	struct ale_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mii_data *mii;
	uint8_t eaddr[ETHER_ADDR_LEN];
	bus_addr_t paddr;
	uint32_t reg, rxf_hi, rxf_lo;

	ASSERT_SERIALIZED(ifp->if_serializer);

	mii = device_get_softc(sc->ale_miibus);

	/*
	 * Cancel any pending I/O.
	 */
	ale_stop(sc);

	/*
	 * Reset the chip to a known state.
	 */
	ale_reset(sc);

	/* Initialize Tx descriptors, DMA memory blocks. */
	ale_init_rx_pages(sc);
	ale_init_tx_ring(sc);

	/* Reprogram the station address. */
	bcopy(IF_LLADDR(ifp), eaddr, ETHER_ADDR_LEN);
	CSR_WRITE_4(sc, ALE_PAR0,
	    eaddr[2] << 24 | eaddr[3] << 16 | eaddr[4] << 8 | eaddr[5]);
	CSR_WRITE_4(sc, ALE_PAR1, eaddr[0] << 8 | eaddr[1]);

	/*
	 * Clear WOL status and disable all WOL feature as WOL
	 * would interfere Rx operation under normal environments.
	 */
	CSR_READ_4(sc, ALE_WOL_CFG);
	CSR_WRITE_4(sc, ALE_WOL_CFG, 0);

	/*
	 * Set Tx descriptor/RXF0/CMB base addresses. They share
	 * the same high address part of DMAable region.
	 */
	paddr = sc->ale_cdata.ale_tx_ring_paddr;
	CSR_WRITE_4(sc, ALE_TPD_ADDR_HI, ALE_ADDR_HI(paddr));
	CSR_WRITE_4(sc, ALE_TPD_ADDR_LO, ALE_ADDR_LO(paddr));
	CSR_WRITE_4(sc, ALE_TPD_CNT,
	    (ALE_TX_RING_CNT << TPD_CNT_SHIFT) & TPD_CNT_MASK);

	/* Set Rx page base address, note we use single queue. */
	paddr = sc->ale_cdata.ale_rx_page[0].page_paddr;
	CSR_WRITE_4(sc, ALE_RXF0_PAGE0_ADDR_LO, ALE_ADDR_LO(paddr));
	paddr = sc->ale_cdata.ale_rx_page[1].page_paddr;
	CSR_WRITE_4(sc, ALE_RXF0_PAGE1_ADDR_LO, ALE_ADDR_LO(paddr));

	/* Set Tx/Rx CMB addresses. */
	paddr = sc->ale_cdata.ale_tx_cmb_paddr;
	CSR_WRITE_4(sc, ALE_TX_CMB_ADDR_LO, ALE_ADDR_LO(paddr));
	paddr = sc->ale_cdata.ale_rx_page[0].cmb_paddr;
	CSR_WRITE_4(sc, ALE_RXF0_CMB0_ADDR_LO, ALE_ADDR_LO(paddr));
	paddr = sc->ale_cdata.ale_rx_page[1].cmb_paddr;
	CSR_WRITE_4(sc, ALE_RXF0_CMB1_ADDR_LO, ALE_ADDR_LO(paddr));

	/* Mark RXF0 is valid. */
	CSR_WRITE_1(sc, ALE_RXF0_PAGE0, RXF_VALID);
	CSR_WRITE_1(sc, ALE_RXF0_PAGE1, RXF_VALID);
	/*
	 * No need to initialize RFX1/RXF2/RXF3. We don't use
	 * multi-queue yet.
	 */

	/* Set Rx page size, excluding guard frame size. */
	CSR_WRITE_4(sc, ALE_RXF_PAGE_SIZE, ALE_RX_PAGE_SZ);

	/* Tell hardware that we're ready to load DMA blocks. */
	CSR_WRITE_4(sc, ALE_DMA_BLOCK, DMA_BLOCK_LOAD);

	/* Set Rx/Tx interrupt trigger threshold. */
	CSR_WRITE_4(sc, ALE_INT_TRIG_THRESH, (1 << INT_TRIG_RX_THRESH_SHIFT) |
	    (4 << INT_TRIG_TX_THRESH_SHIFT));
	/*
	 * XXX
	 * Set interrupt trigger timer, its purpose and relation
	 * with interrupt moderation mechanism is not clear yet.
	 */
	CSR_WRITE_4(sc, ALE_INT_TRIG_TIMER,
	    ((ALE_USECS(10) << INT_TRIG_RX_TIMER_SHIFT) |
	    (ALE_USECS(1000) << INT_TRIG_TX_TIMER_SHIFT)));

	/* Configure interrupt moderation timer. */
	reg = ALE_USECS(sc->ale_int_rx_mod) << IM_TIMER_RX_SHIFT;
	reg |= ALE_USECS(sc->ale_int_tx_mod) << IM_TIMER_TX_SHIFT;
	CSR_WRITE_4(sc, ALE_IM_TIMER, reg);
	reg = CSR_READ_4(sc, ALE_MASTER_CFG);
	reg &= ~(MASTER_CHIP_REV_MASK | MASTER_CHIP_ID_MASK);
	reg &= ~(MASTER_IM_RX_TIMER_ENB | MASTER_IM_TX_TIMER_ENB);
	if (ALE_USECS(sc->ale_int_rx_mod) != 0)
		reg |= MASTER_IM_RX_TIMER_ENB;
	if (ALE_USECS(sc->ale_int_tx_mod) != 0)
		reg |= MASTER_IM_TX_TIMER_ENB;
	CSR_WRITE_4(sc, ALE_MASTER_CFG, reg);
	CSR_WRITE_2(sc, ALE_INTR_CLR_TIMER, ALE_USECS(1000));

	/* Set Maximum frame size of controller. */
	if (ifp->if_mtu < ETHERMTU)
		sc->ale_max_frame_size = ETHERMTU;
	else
		sc->ale_max_frame_size = ifp->if_mtu;
	sc->ale_max_frame_size += ETHER_HDR_LEN + EVL_ENCAPLEN + ETHER_CRC_LEN;
	CSR_WRITE_4(sc, ALE_FRAME_SIZE, sc->ale_max_frame_size);

	/* Configure IPG/IFG parameters. */
	CSR_WRITE_4(sc, ALE_IPG_IFG_CFG,
	    ((IPG_IFG_IPGT_DEFAULT << IPG_IFG_IPGT_SHIFT) & IPG_IFG_IPGT_MASK) |
	    ((IPG_IFG_MIFG_DEFAULT << IPG_IFG_MIFG_SHIFT) & IPG_IFG_MIFG_MASK) |
	    ((IPG_IFG_IPG1_DEFAULT << IPG_IFG_IPG1_SHIFT) & IPG_IFG_IPG1_MASK) |
	    ((IPG_IFG_IPG2_DEFAULT << IPG_IFG_IPG2_SHIFT) & IPG_IFG_IPG2_MASK));

	/* Set parameters for half-duplex media. */
	CSR_WRITE_4(sc, ALE_HDPX_CFG,
	    ((HDPX_CFG_LCOL_DEFAULT << HDPX_CFG_LCOL_SHIFT) &
	    HDPX_CFG_LCOL_MASK) |
	    ((HDPX_CFG_RETRY_DEFAULT << HDPX_CFG_RETRY_SHIFT) &
	    HDPX_CFG_RETRY_MASK) | HDPX_CFG_EXC_DEF_EN |
	    ((HDPX_CFG_ABEBT_DEFAULT << HDPX_CFG_ABEBT_SHIFT) &
	    HDPX_CFG_ABEBT_MASK) |
	    ((HDPX_CFG_JAMIPG_DEFAULT << HDPX_CFG_JAMIPG_SHIFT) &
	    HDPX_CFG_JAMIPG_MASK));

	/* Configure Tx jumbo frame parameters. */
	if ((sc->ale_flags & ALE_FLAG_JUMBO) != 0) {
		if (ifp->if_mtu < ETHERMTU)
			reg = sc->ale_max_frame_size;
		else if (ifp->if_mtu < 6 * 1024)
			reg = (sc->ale_max_frame_size * 2) / 3;
		else
			reg = sc->ale_max_frame_size / 2;
		CSR_WRITE_4(sc, ALE_TX_JUMBO_THRESH,
		    roundup(reg, TX_JUMBO_THRESH_UNIT) >>
		    TX_JUMBO_THRESH_UNIT_SHIFT);
	}

	/* Configure TxQ. */
	reg = (128 << (sc->ale_dma_rd_burst >> DMA_CFG_RD_BURST_SHIFT))
	    << TXQ_CFG_TX_FIFO_BURST_SHIFT;
	reg |= (TXQ_CFG_TPD_BURST_DEFAULT << TXQ_CFG_TPD_BURST_SHIFT) &
	    TXQ_CFG_TPD_BURST_MASK;
	CSR_WRITE_4(sc, ALE_TXQ_CFG, reg | TXQ_CFG_ENHANCED_MODE | TXQ_CFG_ENB);

	/* Configure Rx jumbo frame & flow control parameters. */
	if ((sc->ale_flags & ALE_FLAG_JUMBO) != 0) {
		reg = roundup(sc->ale_max_frame_size, RX_JUMBO_THRESH_UNIT);
		CSR_WRITE_4(sc, ALE_RX_JUMBO_THRESH,
		    (((reg >> RX_JUMBO_THRESH_UNIT_SHIFT) <<
		    RX_JUMBO_THRESH_MASK_SHIFT) & RX_JUMBO_THRESH_MASK) |
		    ((RX_JUMBO_LKAH_DEFAULT << RX_JUMBO_LKAH_SHIFT) &
		    RX_JUMBO_LKAH_MASK));
		reg = CSR_READ_4(sc, ALE_SRAM_RX_FIFO_LEN);
		rxf_hi = (reg * 7) / 10;
		rxf_lo = (reg * 3)/ 10;
		CSR_WRITE_4(sc, ALE_RX_FIFO_PAUSE_THRESH,
		    ((rxf_lo << RX_FIFO_PAUSE_THRESH_LO_SHIFT) &
		    RX_FIFO_PAUSE_THRESH_LO_MASK) |
		    ((rxf_hi << RX_FIFO_PAUSE_THRESH_HI_SHIFT) &
		     RX_FIFO_PAUSE_THRESH_HI_MASK));
	}

	/* Disable RSS. */
	CSR_WRITE_4(sc, ALE_RSS_IDT_TABLE0, 0);
	CSR_WRITE_4(sc, ALE_RSS_CPU, 0);

	/* Configure RxQ. */
	CSR_WRITE_4(sc, ALE_RXQ_CFG,
	    RXQ_CFG_ALIGN_32 | RXQ_CFG_CUT_THROUGH_ENB | RXQ_CFG_ENB);

	/* Configure DMA parameters. */
	reg = 0;
	if ((sc->ale_flags & ALE_FLAG_TXCMB_BUG) == 0)
		reg |= DMA_CFG_TXCMB_ENB;
	CSR_WRITE_4(sc, ALE_DMA_CFG,
	    DMA_CFG_OUT_ORDER | DMA_CFG_RD_REQ_PRI | DMA_CFG_RCB_64 |
	    sc->ale_dma_rd_burst | reg |
	    sc->ale_dma_wr_burst | DMA_CFG_RXCMB_ENB |
	    ((DMA_CFG_RD_DELAY_CNT_DEFAULT << DMA_CFG_RD_DELAY_CNT_SHIFT) &
	    DMA_CFG_RD_DELAY_CNT_MASK) |
	    ((DMA_CFG_WR_DELAY_CNT_DEFAULT << DMA_CFG_WR_DELAY_CNT_SHIFT) &
	    DMA_CFG_WR_DELAY_CNT_MASK));

	/*
	 * Hardware can be configured to issue SMB interrupt based
	 * on programmed interval. Since there is a callout that is
	 * invoked for every hz in driver we use that instead of
	 * relying on periodic SMB interrupt.
	 */
	CSR_WRITE_4(sc, ALE_SMB_STAT_TIMER, ALE_USECS(0));

	/* Clear MAC statistics. */
	ale_stats_clear(sc);

	/*
	 * Configure Tx/Rx MACs.
	 *  - Auto-padding for short frames.
	 *  - Enable CRC generation.
	 *  Actual reconfiguration of MAC for resolved speed/duplex
	 *  is followed after detection of link establishment.
	 *  AR81xx always does checksum computation regardless of
	 *  MAC_CFG_RXCSUM_ENB bit. In fact, setting the bit will
	 *  cause Rx handling issue for fragmented IP datagrams due
	 *  to silicon bug.
	 */
	reg = MAC_CFG_TX_CRC_ENB | MAC_CFG_TX_AUTO_PAD | MAC_CFG_FULL_DUPLEX |
	    ((MAC_CFG_PREAMBLE_DEFAULT << MAC_CFG_PREAMBLE_SHIFT) &
	    MAC_CFG_PREAMBLE_MASK);
	if ((sc->ale_flags & ALE_FLAG_FASTETHER) != 0)
		reg |= MAC_CFG_SPEED_10_100;
	else
		reg |= MAC_CFG_SPEED_1000;
	CSR_WRITE_4(sc, ALE_MAC_CFG, reg);

	/* Set up the receive filter. */
	ale_rxfilter(sc);
	ale_rxvlan(sc);

	/* Acknowledge all pending interrupts and clear it. */
	CSR_WRITE_4(sc, ALE_INTR_MASK, ALE_INTRS);
	CSR_WRITE_4(sc, ALE_INTR_STATUS, 0xFFFFFFFF);
	CSR_WRITE_4(sc, ALE_INTR_STATUS, 0);

	sc->ale_flags &= ~ALE_FLAG_LINK;

	/* Switch to the current media. */
	mii_mediachg(mii);

	callout_reset(&sc->ale_tick_ch, hz, ale_tick, sc);

	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
}

static void
ale_stop(struct ale_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct ale_txdesc *txd;
	uint32_t reg;
	int i;

	ASSERT_SERIALIZED(ifp->if_serializer);

	/*
	 * Mark the interface down and cancel the watchdog timer.
	 */
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	ifp->if_timer = 0;

	callout_stop(&sc->ale_tick_ch);
	sc->ale_flags &= ~ALE_FLAG_LINK;

	ale_stats_update(sc);

	/* Disable interrupts. */
	CSR_WRITE_4(sc, ALE_INTR_MASK, 0);
	CSR_WRITE_4(sc, ALE_INTR_STATUS, 0xFFFFFFFF);

	/* Disable queue processing and DMA. */
	reg = CSR_READ_4(sc, ALE_TXQ_CFG);
	reg &= ~TXQ_CFG_ENB;
	CSR_WRITE_4(sc, ALE_TXQ_CFG, reg);
	reg = CSR_READ_4(sc, ALE_RXQ_CFG);
	reg &= ~RXQ_CFG_ENB;
	CSR_WRITE_4(sc, ALE_RXQ_CFG, reg);
	reg = CSR_READ_4(sc, ALE_DMA_CFG);
	reg &= ~(DMA_CFG_TXCMB_ENB | DMA_CFG_RXCMB_ENB);
	CSR_WRITE_4(sc, ALE_DMA_CFG, reg);
	DELAY(1000);

	/* Stop Rx/Tx MACs. */
	ale_stop_mac(sc);

	/* Disable interrupts again? XXX */
	CSR_WRITE_4(sc, ALE_INTR_STATUS, 0xFFFFFFFF);

	/*
	 * Free TX mbufs still in the queues.
	 */
	for (i = 0; i < ALE_TX_RING_CNT; i++) {
		txd = &sc->ale_cdata.ale_txdesc[i];
		if (txd->tx_m != NULL) {
			bus_dmamap_unload(sc->ale_cdata.ale_tx_tag,
			    txd->tx_dmamap);
			m_freem(txd->tx_m);
			txd->tx_m = NULL;
		}
        }
}

static void
ale_stop_mac(struct ale_softc *sc)
{
	uint32_t reg;
	int i;

	reg = CSR_READ_4(sc, ALE_MAC_CFG);
	if ((reg & (MAC_CFG_TX_ENB | MAC_CFG_RX_ENB)) != 0) {
		reg &= ~MAC_CFG_TX_ENB | MAC_CFG_RX_ENB;
		CSR_WRITE_4(sc, ALE_MAC_CFG, reg);
	}

	for (i = ALE_TIMEOUT; i > 0; i--) {
		reg = CSR_READ_4(sc, ALE_IDLE_STATUS);
		if (reg == 0)
			break;
		DELAY(10);
	}
	if (i == 0)
		device_printf(sc->ale_dev,
		    "could not disable Tx/Rx MAC(0x%08x)!\n", reg);
}

static void
ale_init_tx_ring(struct ale_softc *sc)
{
	struct ale_txdesc *txd;
	int i;

	sc->ale_cdata.ale_tx_prod = 0;
	sc->ale_cdata.ale_tx_cons = 0;
	sc->ale_cdata.ale_tx_cnt = 0;

	bzero(sc->ale_cdata.ale_tx_ring, ALE_TX_RING_SZ);
	bzero(sc->ale_cdata.ale_tx_cmb, ALE_TX_CMB_SZ);
	for (i = 0; i < ALE_TX_RING_CNT; i++) {
		txd = &sc->ale_cdata.ale_txdesc[i];
		txd->tx_m = NULL;
	}
	*sc->ale_cdata.ale_tx_cmb = 0;
	bus_dmamap_sync(sc->ale_cdata.ale_tx_cmb_tag,
	    sc->ale_cdata.ale_tx_cmb_map,
	    BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(sc->ale_cdata.ale_tx_ring_tag,
	    sc->ale_cdata.ale_tx_ring_map,
	    BUS_DMASYNC_PREWRITE);
}

static void
ale_init_rx_pages(struct ale_softc *sc)
{
	struct ale_rx_page *rx_page;
	int i;

	sc->ale_cdata.ale_rx_seqno = 0;
	sc->ale_cdata.ale_rx_curp = 0;

	for (i = 0; i < ALE_RX_PAGES; i++) {
		rx_page = &sc->ale_cdata.ale_rx_page[i];
		bzero(rx_page->page_addr, sc->ale_pagesize);
		bzero(rx_page->cmb_addr, ALE_RX_CMB_SZ);
		rx_page->cons = 0;
		*rx_page->cmb_addr = 0;
		bus_dmamap_sync(rx_page->page_tag, rx_page->page_map,
				BUS_DMASYNC_PREWRITE);
		bus_dmamap_sync(rx_page->cmb_tag, rx_page->cmb_map,
				BUS_DMASYNC_PREWRITE);
	}
}

static void
ale_rxvlan(struct ale_softc *sc)
{
	struct ifnet *ifp;
	uint32_t reg;

	ifp = &sc->arpcom.ac_if;
	reg = CSR_READ_4(sc, ALE_MAC_CFG);
	reg &= ~MAC_CFG_VLAN_TAG_STRIP;
	if ((ifp->if_capenable & IFCAP_VLAN_HWTAGGING) != 0)
		reg |= MAC_CFG_VLAN_TAG_STRIP;
	CSR_WRITE_4(sc, ALE_MAC_CFG, reg);
}

static void
ale_rxfilter(struct ale_softc *sc)
{
	struct ifnet *ifp;
	struct ifmultiaddr *ifma;
	uint32_t crc;
	uint32_t mchash[2];
	uint32_t rxcfg;

	ifp = &sc->arpcom.ac_if;

	rxcfg = CSR_READ_4(sc, ALE_MAC_CFG);
	rxcfg &= ~(MAC_CFG_ALLMULTI | MAC_CFG_BCAST | MAC_CFG_PROMISC);
	if ((ifp->if_flags & IFF_BROADCAST) != 0)
		rxcfg |= MAC_CFG_BCAST;
	if ((ifp->if_flags & (IFF_PROMISC | IFF_ALLMULTI)) != 0) {
		if ((ifp->if_flags & IFF_PROMISC) != 0)
			rxcfg |= MAC_CFG_PROMISC;
		if ((ifp->if_flags & IFF_ALLMULTI) != 0)
			rxcfg |= MAC_CFG_ALLMULTI;
		CSR_WRITE_4(sc, ALE_MAR0, 0xFFFFFFFF);
		CSR_WRITE_4(sc, ALE_MAR1, 0xFFFFFFFF);
		CSR_WRITE_4(sc, ALE_MAC_CFG, rxcfg);
		return;
	}

	/* Program new filter. */
	bzero(mchash, sizeof(mchash));

	TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;
		crc = ether_crc32_le(LLADDR((struct sockaddr_dl *)
		    ifma->ifma_addr), ETHER_ADDR_LEN);
		mchash[crc >> 31] |= 1 << ((crc >> 26) & 0x1f);
	}

	CSR_WRITE_4(sc, ALE_MAR0, mchash[0]);
	CSR_WRITE_4(sc, ALE_MAR1, mchash[1]);
	CSR_WRITE_4(sc, ALE_MAC_CFG, rxcfg);
}

static int
sysctl_hw_ale_int_mod(SYSCTL_HANDLER_ARGS)
{
	return (sysctl_int_range(oidp, arg1, arg2, req,
	    ALE_IM_TIMER_MIN, ALE_IM_TIMER_MAX));
}

static void
ale_dmamap_buf_cb(void *xctx, bus_dma_segment_t *segs, int nsegs,
		  bus_size_t mapsz __unused, int error)
{
	struct ale_dmamap_ctx *ctx = xctx;
	int i;

	if (error)
		return;

	if (nsegs > ctx->nsegs) {
		ctx->nsegs = 0;
		return;
	}

	ctx->nsegs = nsegs;
	for (i = 0; i < nsegs; ++i)
		ctx->segs[i] = segs[i];
}
