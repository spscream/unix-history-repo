/*-
 * Copyright (c) 1999 The NetBSD Foundation, Inc.
 * Copyright (c) 2001-2003 Thomas Moestl <tmm@FreeBSD.org>.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Paul Kranenburg.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *	from: NetBSD: hme.c,v 1.29 2002/05/05 03:02:38 thorpej Exp
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * HME Ethernet module driver.
 *
 * The HME is e.g. part of the PCIO PCI multi function device.
 * It supports TX gathering and TX and RX checksum offloading.
 * RX buffers must be aligned at a programmable offset modulo 16. We choose 2
 * for this offset: mbuf clusters are usually on about 2^11 boundaries, 2 bytes
 * are skipped to make sure the header after the ethernet header is aligned on a
 * natural boundary, so this ensures minimal wastage in the most common case.
 *
 * Also, apparently, the buffers must extend to a DMA burst boundary beyond the
 * maximum packet size (this is not verified). Buffers starting on odd
 * boundaries must be mapped so that the burst can start on a natural boundary.
 *
 * STP2002QFP-UG says that Ethernet hardware supports TCP checksum offloading.
 * In reality, we can do the same technique for UDP datagram too. However,
 * the hardware doesn't compensate the checksum for UDP datagram which can yield
 * to 0x0. As a safe guard, UDP checksum offload is disabled by default. It
 * can be reactivated by setting special link option link0 with ifconfig(8).
 */
#define HME_CSUM_FEATURES	(CSUM_TCP)
#define HMEDEBUG
#define	KTR_HME		KTR_CT2		/* XXX */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/ktr.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_vlan_var.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include <machine/bus.h>

#include <dev/hme/if_hmereg.h>
#include <dev/hme/if_hmevar.h>

static void	hme_start(struct ifnet *);
static void	hme_start_locked(struct ifnet *);
static void	hme_stop(struct hme_softc *);
static int	hme_ioctl(struct ifnet *, u_long, caddr_t);
static void	hme_tick(void *);
static void	hme_watchdog(struct ifnet *);
static void	hme_init(void *);
static void	hme_init_locked(void *);
static int	hme_add_rxbuf(struct hme_softc *, unsigned int, int);
static int	hme_meminit(struct hme_softc *);
static int	hme_mac_bitflip(struct hme_softc *, u_int32_t, u_int32_t,
    u_int32_t, u_int32_t);
static void	hme_mifinit(struct hme_softc *);
static void	hme_reset(struct hme_softc *);
static void	hme_setladrf(struct hme_softc *, int);

static int	hme_mediachange(struct ifnet *);
static void	hme_mediastatus(struct ifnet *, struct ifmediareq *);

static int	hme_load_txmbuf(struct hme_softc *, struct mbuf *);
static void	hme_read(struct hme_softc *, int, int, u_int32_t);
static void	hme_eint(struct hme_softc *, u_int);
static void	hme_rint(struct hme_softc *);
static void	hme_tint(struct hme_softc *);
static void	hme_txcksum(struct mbuf *, u_int32_t *);
static void	hme_rxcksum(struct mbuf *, u_int32_t);

static void	hme_cdma_callback(void *, bus_dma_segment_t *, int, int);
static void	hme_txdma_callback(void *, bus_dma_segment_t *, int,
    bus_size_t, int);

devclass_t hme_devclass;

static int hme_nerr;

DRIVER_MODULE(miibus, hme, miibus_driver, miibus_devclass, 0, 0);
MODULE_DEPEND(hme, miibus, 1, 1, 1);

#define	HME_SPC_READ_4(spc, sc, offs) \
	bus_space_read_4((sc)->sc_ ## spc ## t, (sc)->sc_ ## spc ## h, \
	    (offs))
#define	HME_SPC_WRITE_4(spc, sc, offs, v) \
	bus_space_write_4((sc)->sc_ ## spc ## t, (sc)->sc_ ## spc ## h, \
	    (offs), (v))

#define	HME_SEB_READ_4(sc, offs)	HME_SPC_READ_4(seb, (sc), (offs))
#define	HME_SEB_WRITE_4(sc, offs, v)	HME_SPC_WRITE_4(seb, (sc), (offs), (v))
#define	HME_ERX_READ_4(sc, offs)	HME_SPC_READ_4(erx, (sc), (offs))
#define	HME_ERX_WRITE_4(sc, offs, v)	HME_SPC_WRITE_4(erx, (sc), (offs), (v))
#define	HME_ETX_READ_4(sc, offs)	HME_SPC_READ_4(etx, (sc), (offs))
#define	HME_ETX_WRITE_4(sc, offs, v)	HME_SPC_WRITE_4(etx, (sc), (offs), (v))
#define	HME_MAC_READ_4(sc, offs)	HME_SPC_READ_4(mac, (sc), (offs))
#define	HME_MAC_WRITE_4(sc, offs, v)	HME_SPC_WRITE_4(mac, (sc), (offs), (v))
#define	HME_MIF_READ_4(sc, offs)	HME_SPC_READ_4(mif, (sc), (offs))
#define	HME_MIF_WRITE_4(sc, offs, v)	HME_SPC_WRITE_4(mif, (sc), (offs), (v))

#define	HME_MAXERR	5
#define	HME_WHINE(dev, ...) do {					\
	if (hme_nerr++ < HME_MAXERR)					\
		device_printf(dev, __VA_ARGS__);			\
	if (hme_nerr == HME_MAXERR) {					\
		device_printf(dev, "too may errors; not reporting any "	\
		    "more\n");						\
	}								\
} while(0)

/* Support oversized VLAN frames. */
#define HME_MAX_FRAMESIZE (ETHER_MAX_LEN + ETHER_VLAN_ENCAP_LEN)

int
hme_config(struct hme_softc *sc)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct mii_softc *child;
	bus_size_t size;
	int error, rdesc, tdesc, i;

	/*
	 * HME common initialization.
	 *
	 * hme_softc fields that must be initialized by the front-end:
	 *
	 * the DMA bus tag:
	 *	sc_dmatag
	 *
	 * the bus handles, tags and offsets (splitted for SBus compatability):
	 *	sc_seb{t,h,o}	(Shared Ethernet Block registers)
	 *	sc_erx{t,h,o}	(Receiver Unit registers)
	 *	sc_etx{t,h,o}	(Transmitter Unit registers)
	 *	sc_mac{t,h,o}	(MAC registers)
	 *	sc_mif{t,h,o}	(Management Interface registers)
	 *
	 * the maximum bus burst size:
	 *	sc_burst
	 *
	 */

	HME_LOCK_ASSERT(sc, MA_NOTOWNED);
	/* Make sure the chip is stopped. */
	HME_LOCK(sc);
	hme_stop(sc);
	HME_UNLOCK(sc);

	/*
	 * Allocate DMA capable memory
	 * Buffer descriptors must be aligned on a 2048 byte boundary;
	 * take this into account when calculating the size. Note that
	 * the maximum number of descriptors (256) occupies 2048 bytes,
	 * so we allocate that much regardless of HME_N*DESC.
	 */
	size =	4096;

	error = bus_dma_tag_create(NULL, 1, 0, BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, size, HME_NTXDESC + HME_NRXDESC + 1,
	    BUS_SPACE_MAXSIZE_32BIT, 0, NULL, NULL, &sc->sc_pdmatag);
	if (error)
		return (error);

	error = bus_dma_tag_create(sc->sc_pdmatag, 2048, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL, size,
	    1, BUS_SPACE_MAXSIZE_32BIT, BUS_DMA_ALLOCNOW, busdma_lock_mutex,
	    &Giant, &sc->sc_cdmatag);
	if (error)
		goto fail_ptag;

	error = bus_dma_tag_create(sc->sc_pdmatag, max(0x10, sc->sc_burst), 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL, MCLBYTES,
	    HME_NRXDESC, BUS_SPACE_MAXSIZE_32BIT, BUS_DMA_ALLOCNOW,
	    NULL, NULL, &sc->sc_rdmatag);
	if (error)
		goto fail_ctag;

	error = bus_dma_tag_create(sc->sc_pdmatag, max(0x10, sc->sc_burst), 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL, MCLBYTES,
	    HME_NTXDESC, BUS_SPACE_MAXSIZE_32BIT, BUS_DMA_ALLOCNOW,
	    NULL, NULL, &sc->sc_tdmatag);
	if (error)
		goto fail_rtag;

	/* Allocate control/TX DMA buffer */
	error = bus_dmamem_alloc(sc->sc_cdmatag, (void **)&sc->sc_rb.rb_membase,
	    0, &sc->sc_cdmamap);
	if (error != 0) {
		device_printf(sc->sc_dev, "DMA buffer alloc error %d\n", error);
		goto fail_ttag;
	}

	/* Load the buffer */
	sc->sc_rb.rb_dmabase = 0;
	if ((error = bus_dmamap_load(sc->sc_cdmatag, sc->sc_cdmamap,
	     sc->sc_rb.rb_membase, size, hme_cdma_callback, sc, 0)) != 0 ||
	    sc->sc_rb.rb_dmabase == 0) {
		device_printf(sc->sc_dev, "DMA buffer map load error %d\n",
		    error);
		goto fail_free;
	}
	CTR2(KTR_HME, "hme_config: dma va %p, pa %#lx", sc->sc_rb.rb_membase,
	    sc->sc_rb.rb_dmabase);

	/*
	 * Prepare the RX descriptors. rdesc serves as marker for the last
	 * processed descriptor and may be used later on.
	 */
	for (rdesc = 0; rdesc < HME_NRXDESC; rdesc++) {
		sc->sc_rb.rb_rxdesc[rdesc].hrx_m = NULL;
		error = bus_dmamap_create(sc->sc_rdmatag, 0,
		    &sc->sc_rb.rb_rxdesc[rdesc].hrx_dmamap);
		if (error != 0)
			goto fail_rxdesc;
	}
	error = bus_dmamap_create(sc->sc_rdmatag, 0,
	    &sc->sc_rb.rb_spare_dmamap);
	if (error != 0)
		goto fail_rxdesc;
	/* Same for the TX descs. */
	for (tdesc = 0; tdesc < HME_NTXQ; tdesc++) {
		sc->sc_rb.rb_txdesc[tdesc].htx_m = NULL;
		error = bus_dmamap_create(sc->sc_tdmatag, 0,
		    &sc->sc_rb.rb_txdesc[tdesc].htx_dmamap);
		if (error != 0)
			goto fail_txdesc;
	}

	sc->sc_csum_features = HME_CSUM_FEATURES;
	/* Initialize ifnet structure. */
	ifp->if_softc = sc;
	if_initname(ifp, device_get_name(sc->sc_dev),
	    device_get_unit(sc->sc_dev));
	ifp->if_mtu = ETHERMTU;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_start = hme_start;
	ifp->if_ioctl = hme_ioctl;
	ifp->if_init = hme_init;
	ifp->if_watchdog = hme_watchdog;
	IFQ_SET_MAXLEN(&ifp->if_snd, HME_NTXQ);
	ifp->if_snd.ifq_drv_maxlen = HME_NTXQ;
	IFQ_SET_READY(&ifp->if_snd);

	HME_LOCK(sc);
	hme_mifinit(sc);
	HME_UNLOCK(sc);

	if ((error = mii_phy_probe(sc->sc_dev, &sc->sc_miibus, hme_mediachange,
	    hme_mediastatus)) != 0) {
		device_printf(sc->sc_dev, "phy probe failed: %d\n", error);
		goto fail_rxdesc;
	}
	sc->sc_mii = device_get_softc(sc->sc_miibus);

	/*
	 * Walk along the list of attached MII devices and
	 * establish an `MII instance' to `phy number'
	 * mapping. We'll use this mapping in media change
	 * requests to determine which phy to use to program
	 * the MIF configuration register.
	 */
	for (child = LIST_FIRST(&sc->sc_mii->mii_phys); child != NULL;
	     child = LIST_NEXT(child, mii_list)) {
		/*
		 * Note: we support just two PHYs: the built-in
		 * internal device and an external on the MII
		 * connector.
		 */
		if (child->mii_phy > 1 || child->mii_inst > 1) {
			device_printf(sc->sc_dev, "cannot accommodate "
			    "MII device %s at phy %d, instance %d\n",
			    device_get_name(child->mii_dev),
			    child->mii_phy, child->mii_inst);
			continue;
		}

		sc->sc_phys[child->mii_inst] = child->mii_phy;
	}

	/* Attach the interface. */
	ether_ifattach(ifp, sc->sc_arpcom.ac_enaddr);

	/*
	 * Tell the upper layer(s) we support long frames/checksum offloads.
	 */
	ifp->if_data.ifi_hdrlen = sizeof(struct ether_vlan_header);
	ifp->if_capabilities |= IFCAP_VLAN_MTU | IFCAP_HWCSUM;
	ifp->if_hwassist |= sc->sc_csum_features;
	ifp->if_capenable |= IFCAP_VLAN_MTU | IFCAP_HWCSUM;

	callout_init(&sc->sc_tick_ch, CALLOUT_MPSAFE);
	return (0);

fail_txdesc:
	for (i = 0; i < tdesc; i++) {
		bus_dmamap_destroy(sc->sc_tdmatag,
		    sc->sc_rb.rb_txdesc[i].htx_dmamap);
	}
	bus_dmamap_destroy(sc->sc_rdmatag, sc->sc_rb.rb_spare_dmamap);
fail_rxdesc:
	for (i = 0; i < rdesc; i++) {
		bus_dmamap_destroy(sc->sc_rdmatag,
		    sc->sc_rb.rb_rxdesc[i].hrx_dmamap);
	}
	bus_dmamap_unload(sc->sc_cdmatag, sc->sc_cdmamap);
fail_free:
	bus_dmamem_free(sc->sc_cdmatag, sc->sc_rb.rb_membase, sc->sc_cdmamap);
fail_ttag:
	bus_dma_tag_destroy(sc->sc_tdmatag);
fail_rtag:
	bus_dma_tag_destroy(sc->sc_rdmatag);
fail_ctag:
	bus_dma_tag_destroy(sc->sc_cdmatag);
fail_ptag:
	bus_dma_tag_destroy(sc->sc_pdmatag);
	return (error);
}

void
hme_detach(struct hme_softc *sc)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	int i;

	HME_LOCK_ASSERT(sc, MA_NOTOWNED);

	ether_ifdetach(ifp);
	HME_LOCK(sc);
	hme_stop(sc);
	HME_UNLOCK(sc);
	device_delete_child(sc->sc_dev, sc->sc_miibus);

	for (i = 0; i < HME_NTXQ; i++) {
		bus_dmamap_destroy(sc->sc_tdmatag,
		    sc->sc_rb.rb_txdesc[i].htx_dmamap);
	}
	bus_dmamap_destroy(sc->sc_rdmatag, sc->sc_rb.rb_spare_dmamap);
	for (i = 0; i < HME_NRXDESC; i++) {
		bus_dmamap_destroy(sc->sc_rdmatag,
		    sc->sc_rb.rb_rxdesc[i].hrx_dmamap);
	}
	bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap, BUS_DMASYNC_POSTREAD);
	bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap, BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(sc->sc_cdmatag, sc->sc_cdmamap);
	bus_dmamem_free(sc->sc_cdmatag, sc->sc_rb.rb_membase, sc->sc_cdmamap);
	bus_dma_tag_destroy(sc->sc_tdmatag);
	bus_dma_tag_destroy(sc->sc_rdmatag);
	bus_dma_tag_destroy(sc->sc_cdmatag);
	bus_dma_tag_destroy(sc->sc_pdmatag);
}

void
hme_suspend(struct hme_softc *sc)
{

	HME_LOCK(sc);
	hme_stop(sc);
	HME_UNLOCK(sc);
}

void
hme_resume(struct hme_softc *sc)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;

	HME_LOCK(sc);
	if ((ifp->if_flags & IFF_UP) != 0)
		hme_init_locked(ifp);
	HME_UNLOCK(sc);
}

static void
hme_cdma_callback(void *xsc, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct hme_softc *sc = (struct hme_softc *)xsc;

	if (error != 0)
		return;
	KASSERT(nsegs == 1, ("hme_cdma_callback: bad dma segment count"));
	sc->sc_rb.rb_dmabase = segs[0].ds_addr;
}

static void
hme_tick(void *arg)
{
	struct hme_softc *sc = arg;
	int s;

	s = splnet();
	mii_tick(sc->sc_mii);
	splx(s);

	callout_reset(&sc->sc_tick_ch, hz, hme_tick, sc);
}

static void
hme_reset(struct hme_softc *sc)
{
	int s;

	HME_LOCK(sc);
	s = splnet();
	hme_init_locked(sc);
	splx(s);
	HME_UNLOCK(sc);
}

static void
hme_stop(struct hme_softc *sc)
{
	u_int32_t v;
	int n;

	callout_stop(&sc->sc_tick_ch);

	/* Reset transmitter and receiver */
	HME_SEB_WRITE_4(sc, HME_SEBI_RESET, HME_SEB_RESET_ETX |
	    HME_SEB_RESET_ERX);

	for (n = 0; n < 20; n++) {
		v = HME_SEB_READ_4(sc, HME_SEBI_RESET);
		if ((v & (HME_SEB_RESET_ETX | HME_SEB_RESET_ERX)) == 0)
			return;
		DELAY(20);
	}

	device_printf(sc->sc_dev, "hme_stop: reset failed\n");
}

/*
 * Discard the contents of an mbuf in the RX ring, freeing the buffer in the
 * ring for subsequent use.
 */
static __inline void
hme_discard_rxbuf(struct hme_softc *sc, int ix)
{

	/*
	 * Dropped a packet, reinitialize the descriptor and turn the
	 * ownership back to the hardware.
	 */
	HME_XD_SETFLAGS(sc->sc_pci, sc->sc_rb.rb_rxd, ix, HME_XD_OWN |
	    HME_XD_ENCODE_RSIZE(HME_DESC_RXLEN(sc, &sc->sc_rb.rb_rxdesc[ix])));
}

static int
hme_add_rxbuf(struct hme_softc *sc, unsigned int ri, int keepold)
{
	struct hme_rxdesc *rd;
	struct mbuf *m;
	bus_dma_segment_t segs[1];
	bus_dmamap_t map;
	uintptr_t b;
	int a, unmap, nsegs;

	rd = &sc->sc_rb.rb_rxdesc[ri];
	unmap = rd->hrx_m != NULL;
	if (unmap && keepold) {
		/*
		 * Reinitialize the descriptor flags, as they may have been
		 * altered by the hardware.
		 */
		hme_discard_rxbuf(sc, ri);
		return (0);
	}
	if ((m = m_getcl(M_DONTWAIT, MT_DATA, M_PKTHDR)) == NULL)
		return (ENOBUFS);
	m->m_len = m->m_pkthdr.len = m->m_ext.ext_size;
	b = mtod(m, uintptr_t);
	/*
	 * Required alignment boundary. At least 16 is needed, but since
	 * the mapping must be done in a way that a burst can start on a
	 * natural boundary we might need to extend this.
	 */
	a = max(HME_MINRXALIGN, sc->sc_burst);
	/*
	 * Make sure the buffer suitably aligned. The 2 byte offset is removed
	 * when the mbuf is handed up. XXX: this ensures at least 16 byte
	 * alignment of the header adjacent to the ethernet header, which
	 * should be sufficient in all cases. Nevertheless, this second-guesses
	 * ALIGN().
	 */
	m_adj(m, roundup2(b, a) - b);
	if (bus_dmamap_load_mbuf_sg(sc->sc_rdmatag, sc->sc_rb.rb_spare_dmamap,
	    m, segs, &nsegs, 0) != 0) {
		m_freem(m);
		return (ENOBUFS);
	}
	/* If nsegs is wrong then the stack is corrupt */
	KASSERT(nsegs == 1, ("Too many segments returned!"));
	if (unmap) {
		bus_dmamap_sync(sc->sc_rdmatag, rd->hrx_dmamap,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->sc_rdmatag, rd->hrx_dmamap);
	}
	map = rd->hrx_dmamap;
	rd->hrx_dmamap = sc->sc_rb.rb_spare_dmamap;
	sc->sc_rb.rb_spare_dmamap = map;
	bus_dmamap_sync(sc->sc_rdmatag, rd->hrx_dmamap, BUS_DMASYNC_PREREAD);
	HME_XD_SETADDR(sc->sc_pci, sc->sc_rb.rb_rxd, ri, segs[0].ds_addr);
	rd->hrx_m = m;
	HME_XD_SETFLAGS(sc->sc_pci, sc->sc_rb.rb_rxd, ri, HME_XD_OWN |
	    HME_XD_ENCODE_RSIZE(HME_DESC_RXLEN(sc, rd)));
	return (0);
}

static int
hme_meminit(struct hme_softc *sc)
{
	struct hme_ring *hr = &sc->sc_rb;
	struct hme_txdesc *td;
	bus_addr_t dma;
	caddr_t p;
	unsigned int i;
	int error;

	p = hr->rb_membase;
	dma = hr->rb_dmabase;

	/*
	 * Allocate transmit descriptors
	 */
	hr->rb_txd = p;
	hr->rb_txddma = dma;
	p += HME_NTXDESC * HME_XD_SIZE;
	dma += HME_NTXDESC * HME_XD_SIZE;
	/* We have reserved descriptor space until the next 2048 byte boundary.*/
	dma = (bus_addr_t)roundup((u_long)dma, 2048);
	p = (caddr_t)roundup((u_long)p, 2048);

	/*
	 * Allocate receive descriptors
	 */
	hr->rb_rxd = p;
	hr->rb_rxddma = dma;
	p += HME_NRXDESC * HME_XD_SIZE;
	dma += HME_NRXDESC * HME_XD_SIZE;
	/* Again move forward to the next 2048 byte boundary.*/
	dma = (bus_addr_t)roundup((u_long)dma, 2048);
	p = (caddr_t)roundup((u_long)p, 2048);

	/*
	 * Initialize transmit buffer descriptors
	 */
	for (i = 0; i < HME_NTXDESC; i++) {
		HME_XD_SETADDR(sc->sc_pci, hr->rb_txd, i, 0);
		HME_XD_SETFLAGS(sc->sc_pci, hr->rb_txd, i, 0);
	}

	STAILQ_INIT(&sc->sc_rb.rb_txfreeq);
	STAILQ_INIT(&sc->sc_rb.rb_txbusyq);
	for (i = 0; i < HME_NTXQ; i++) {
		td = &sc->sc_rb.rb_txdesc[i];
		if (td->htx_m != NULL) {
			m_freem(td->htx_m);
			bus_dmamap_sync(sc->sc_tdmatag, td->htx_dmamap,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_tdmatag, td->htx_dmamap);
			td->htx_m = NULL;
		}
		STAILQ_INSERT_TAIL(&sc->sc_rb.rb_txfreeq, td, htx_q);
	}

	/*
	 * Initialize receive buffer descriptors
	 */
	for (i = 0; i < HME_NRXDESC; i++) {
		error = hme_add_rxbuf(sc, i, 1);
		if (error != 0)
			return (error);
	}

	bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap, BUS_DMASYNC_PREREAD);
	bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap, BUS_DMASYNC_PREWRITE);

	hr->rb_tdhead = hr->rb_tdtail = 0;
	hr->rb_td_nbusy = 0;
	hr->rb_rdtail = 0;
	CTR2(KTR_HME, "hme_meminit: tx ring va %p, pa %#lx", hr->rb_txd,
	    hr->rb_txddma);
	CTR2(KTR_HME, "hme_meminit: rx ring va %p, pa %#lx", hr->rb_rxd,
	    hr->rb_rxddma);
	CTR2(KTR_HME, "rx entry 1: flags %x, address %x",
	    *(u_int32_t *)hr->rb_rxd, *(u_int32_t *)(hr->rb_rxd + 4));
	CTR2(KTR_HME, "tx entry 1: flags %x, address %x",
	    *(u_int32_t *)hr->rb_txd, *(u_int32_t *)(hr->rb_txd + 4));
	return (0);
}

static int
hme_mac_bitflip(struct hme_softc *sc, u_int32_t reg, u_int32_t val,
    u_int32_t clr, u_int32_t set)
{
	int i = 0;

	val &= ~clr;
	val |= set;
	HME_MAC_WRITE_4(sc, reg, val);
	if (clr == 0 && set == 0)
		return (1);	/* just write, no bits to wait for */
	do {
		DELAY(100);
		i++;
		val = HME_MAC_READ_4(sc, reg);
		if (i > 40) {
			/* After 3.5ms, we should have been done. */
			device_printf(sc->sc_dev, "timeout while writing to "
			    "MAC configuration register\n");
			return (0);
		}
	} while ((val & clr) != 0 && (val & set) != set);
	return (1);
}

/*
 * Initialization of interface; set up initialization block
 * and transmit/receive descriptor rings.
 */
static void
hme_init(void *xsc)
{
	struct hme_softc *sc = (struct hme_softc *)xsc;

	HME_LOCK(sc);
	hme_init_locked(sc);
	HME_UNLOCK(sc);
}

static void
hme_init_locked(void *xsc)
{
	struct hme_softc *sc = (struct hme_softc *)xsc;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	u_int8_t *ea;
	u_int32_t n, v;

	HME_LOCK_ASSERT(sc, MA_OWNED);
	/*
	 * Initialization sequence. The numbered steps below correspond
	 * to the sequence outlined in section 6.3.5.1 in the Ethernet
	 * Channel Engine manual (part of the PCIO manual).
	 * See also the STP2002-STQ document from Sun Microsystems.
	 */

	/* step 1 & 2. Reset the Ethernet Channel */
	hme_stop(sc);

	/* Re-initialize the MIF */
	hme_mifinit(sc);

#if 0
	/* Mask all MIF interrupts, just in case */
	HME_MIF_WRITE_4(sc, HME_MIFI_IMASK, 0xffff);
#endif

	/* step 3. Setup data structures in host memory */
	if (hme_meminit(sc) != 0) {
		device_printf(sc->sc_dev, "out of buffers; init aborted.");
		return;
	}

	/* step 4. TX MAC registers & counters */
	HME_MAC_WRITE_4(sc, HME_MACI_NCCNT, 0);
	HME_MAC_WRITE_4(sc, HME_MACI_FCCNT, 0);
	HME_MAC_WRITE_4(sc, HME_MACI_EXCNT, 0);
	HME_MAC_WRITE_4(sc, HME_MACI_LTCNT, 0);
	HME_MAC_WRITE_4(sc, HME_MACI_TXSIZE, HME_MAX_FRAMESIZE);

	/* Load station MAC address */
	ea = sc->sc_arpcom.ac_enaddr;
	HME_MAC_WRITE_4(sc, HME_MACI_MACADDR0, (ea[0] << 8) | ea[1]);
	HME_MAC_WRITE_4(sc, HME_MACI_MACADDR1, (ea[2] << 8) | ea[3]);
	HME_MAC_WRITE_4(sc, HME_MACI_MACADDR2, (ea[4] << 8) | ea[5]);

	/*
	 * Init seed for backoff
	 * (source suggested by manual: low 10 bits of MAC address)
	 */
	v = ((ea[4] << 8) | ea[5]) & 0x3fff;
	HME_MAC_WRITE_4(sc, HME_MACI_RANDSEED, v);


	/* Note: Accepting power-on default for other MAC registers here.. */

	/* step 5. RX MAC registers & counters */
	hme_setladrf(sc, 0);

	/* step 6 & 7. Program Descriptor Ring Base Addresses */
	HME_ETX_WRITE_4(sc, HME_ETXI_RING, sc->sc_rb.rb_txddma);
	/* Transmit Descriptor ring size: in increments of 16 */
	HME_ETX_WRITE_4(sc, HME_ETXI_RSIZE, HME_NTXDESC / 16 - 1);

	HME_ERX_WRITE_4(sc, HME_ERXI_RING, sc->sc_rb.rb_rxddma);
	HME_MAC_WRITE_4(sc, HME_MACI_RXSIZE, HME_MAX_FRAMESIZE);

	/* step 8. Global Configuration & Interrupt Mask */
	HME_SEB_WRITE_4(sc, HME_SEBI_IMASK,
	    ~(/*HME_SEB_STAT_GOTFRAME | HME_SEB_STAT_SENTFRAME |*/
		HME_SEB_STAT_HOSTTOTX |
		HME_SEB_STAT_RXTOHOST |
		HME_SEB_STAT_TXALL |
		HME_SEB_STAT_TXPERR |
		HME_SEB_STAT_RCNTEXP |
		HME_SEB_STAT_ALL_ERRORS ));

	switch (sc->sc_burst) {
	default:
		v = 0;
		break;
	case 16:
		v = HME_SEB_CFG_BURST16;
		break;
	case 32:
		v = HME_SEB_CFG_BURST32;
		break;
	case 64:
		v = HME_SEB_CFG_BURST64;
		break;
	}
	/*
	 * Blindly setting 64bit transfers may hang PCI cards(Cheerio?).
	 * Allowing 64bit transfers breaks TX checksum offload as well.
	 * Don't know this comes from hardware bug or driver's DMAing
	 * scheme.
	 *
	 * if (sc->sc_pci == 0)
 	 *	v |= HME_SEB_CFG_64BIT;
	 */
	HME_SEB_WRITE_4(sc, HME_SEBI_CFG, v);

	/* step 9. ETX Configuration: use mostly default values */

	/* Enable DMA */
	v = HME_ETX_READ_4(sc, HME_ETXI_CFG);
	v |= HME_ETX_CFG_DMAENABLE;
	HME_ETX_WRITE_4(sc, HME_ETXI_CFG, v);

	/* step 10. ERX Configuration */
	v = HME_ERX_READ_4(sc, HME_ERXI_CFG);

	/* Encode Receive Descriptor ring size: four possible values */
	v &= ~HME_ERX_CFG_RINGSIZEMSK;
	switch (HME_NRXDESC) {
	case 32:
		v |= HME_ERX_CFG_RINGSIZE32;
		break;
	case 64:
		v |= HME_ERX_CFG_RINGSIZE64;
		break;
	case 128:
		v |= HME_ERX_CFG_RINGSIZE128;
		break;
	case 256:
		v |= HME_ERX_CFG_RINGSIZE256;
		break;
	default:
		printf("hme: invalid Receive Descriptor ring size\n");
		break;
	}

	/* Enable DMA, fix RX first byte offset. */
	v &= ~HME_ERX_CFG_FBO_MASK;
	v |= HME_ERX_CFG_DMAENABLE | (HME_RXOFFS << HME_ERX_CFG_FBO_SHIFT);
	/* RX TCP/UDP checksum offset */
	n = (ETHER_HDR_LEN + sizeof(struct ip)) / 2;
	n = (n << HME_ERX_CFG_CSUMSTART_SHIFT) & HME_ERX_CFG_CSUMSTART_MASK;
	v |= n;
	CTR1(KTR_HME, "hme_init: programming ERX_CFG to %x", (u_int)v);
	HME_ERX_WRITE_4(sc, HME_ERXI_CFG, v);

	/* step 11. XIF Configuration */
	v = HME_MAC_READ_4(sc, HME_MACI_XIF);
	v |= HME_MAC_XIF_OE;
	/* If an external transceiver is connected, enable its MII drivers */
	if ((HME_MIF_READ_4(sc, HME_MIFI_CFG) & HME_MIF_CFG_MDI1) != 0)
		v |= HME_MAC_XIF_MIIENABLE;
	CTR1(KTR_HME, "hme_init: programming XIF to %x", (u_int)v);
	HME_MAC_WRITE_4(sc, HME_MACI_XIF, v);

	/* step 12. RX_MAC Configuration Register */
	v = HME_MAC_READ_4(sc, HME_MACI_RXCFG);
	v |= HME_MAC_RXCFG_ENABLE;
	v &= ~(HME_MAC_RXCFG_DCRCS);
	CTR1(KTR_HME, "hme_init: programming RX_MAC to %x", (u_int)v);
	HME_MAC_WRITE_4(sc, HME_MACI_RXCFG, v);

	/* step 13. TX_MAC Configuration Register */
	v = HME_MAC_READ_4(sc, HME_MACI_TXCFG);
	v |= (HME_MAC_TXCFG_ENABLE | HME_MAC_TXCFG_DGIVEUP);
	CTR1(KTR_HME, "hme_init: programming TX_MAC to %x", (u_int)v);
	HME_MAC_WRITE_4(sc, HME_MACI_TXCFG, v);

	/* step 14. Issue Transmit Pending command */

#ifdef HMEDEBUG
	/* Debug: double-check. */
	CTR4(KTR_HME, "hme_init: tx ring %#x, rsz %#x, rx ring %#x, "
	    "rxsize %#x", HME_ETX_READ_4(sc, HME_ETXI_RING),
	    HME_ETX_READ_4(sc, HME_ETXI_RSIZE),
	    HME_ERX_READ_4(sc, HME_ERXI_RING),
	    HME_MAC_READ_4(sc, HME_MACI_RXSIZE));
	CTR3(KTR_HME, "hme_init: intr mask %#x, erx cfg %#x, etx cfg %#x",
	    HME_SEB_READ_4(sc, HME_SEBI_IMASK),
	    HME_ERX_READ_4(sc, HME_ERXI_CFG),
	    HME_ETX_READ_4(sc, HME_ETXI_CFG));
	CTR2(KTR_HME, "hme_init: mac rxcfg %#x, maci txcfg %#x",
	    HME_MAC_READ_4(sc, HME_MACI_RXCFG),
	    HME_MAC_READ_4(sc, HME_MACI_TXCFG));
#endif

	/* Set the current media. */
	/*
	 * HME_UNLOCK(sc);	
	 * mii_mediachg(sc->sc_mii);
	 * HME_LOCK(sc);	
	 */

	/* Start the one second timer. */
	callout_reset(&sc->sc_tick_ch, hz, hme_tick, sc);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;
	ifp->if_timer = 0;
	hme_start_locked(ifp);
}

struct hme_txdma_arg {
	struct hme_softc	*hta_sc;
	struct hme_txdesc	*hta_htx;
	int			hta_ndescs;
};

/*
 * XXX: this relies on the fact that segments returned by bus_dmamap_load_mbuf()
 * are readable from the nearest burst boundary on (i.e. potentially before
 * ds_addr) to the first boundary beyond the end. This is usually a safe
 * assumption to make, but is not documented.
 */
static void
hme_txdma_callback(void *xsc, bus_dma_segment_t *segs, int nsegs,
    bus_size_t totsz, int error)
{
	struct hme_txdma_arg *ta = xsc;
	struct hme_txdesc *htx;
	bus_size_t len = 0;
	caddr_t txd;
	u_int32_t flags = 0;
	int i, tdhead, pci;

	if (error != 0)
		return;

	tdhead = ta->hta_sc->sc_rb.rb_tdhead;
	pci = ta->hta_sc->sc_pci;
	txd = ta->hta_sc->sc_rb.rb_txd;
	htx = ta->hta_htx;

	if (ta->hta_sc->sc_rb.rb_td_nbusy + nsegs >= HME_NTXDESC) {
		ta->hta_ndescs = -1;
		return;
	}
	ta->hta_ndescs = nsegs;

	for (i = 0; i < nsegs; i++) {
		if (segs[i].ds_len == 0)
			continue;

		/* Fill the ring entry. */
		flags = HME_XD_ENCODE_TSIZE(segs[i].ds_len);
		if (len == 0)
			flags |= HME_XD_SOP;
		if (len + segs[i].ds_len == totsz)
			flags |= HME_XD_EOP;
		CTR5(KTR_HME, "hme_txdma_callback: seg %d/%d, ri %d, "
		    "flags %#x, addr %#x", i + 1, nsegs, tdhead, (u_int)flags,
		    (u_int)segs[i].ds_addr);
		HME_XD_SETFLAGS(pci, txd, tdhead, flags);
		HME_XD_SETADDR(pci, txd, tdhead, segs[i].ds_addr);

		ta->hta_sc->sc_rb.rb_td_nbusy++;
		htx->htx_lastdesc = tdhead;
		tdhead = (tdhead + 1) % HME_NTXDESC;
		len += segs[i].ds_len;
	}
	ta->hta_sc->sc_rb.rb_tdhead = tdhead;
	KASSERT((flags & HME_XD_EOP) != 0,
	    ("hme_txdma_callback: missed end of packet!"));
}

/* TX TCP/UDP checksum */
static void
hme_txcksum(struct mbuf *m, u_int32_t *cflags)
{
	struct ip *ip;
	u_int32_t offset, offset2;
	caddr_t p;

	for(; m && m->m_len == 0; m = m->m_next)
		;
	if (m == NULL || m->m_len < ETHER_HDR_LEN) {
		printf("hme_txcksum: m_len < ETHER_HDR_LEN\n");
		return; /* checksum will be corrupted */
	}
	if (m->m_len < ETHER_HDR_LEN + sizeof(u_int32_t)) {
		if (m->m_len != ETHER_HDR_LEN) {
			printf("hme_txcksum: m_len != ETHER_HDR_LEN\n");
			return;	/* checksum will be corrupted */
		}
		/* XXX */
		for(m = m->m_next; m && m->m_len == 0; m = m->m_next)
			;
		if (m == NULL)
			return; /* checksum will be corrupted */
		ip = mtod(m, struct ip *);
	} else {
		p = mtod(m, caddr_t);
		p += ETHER_HDR_LEN;
		ip = (struct ip *)p;
	}
	offset2 = m->m_pkthdr.csum_data;
	offset = (ip->ip_hl << 2) + ETHER_HDR_LEN;
	*cflags = offset << HME_XD_TXCKSUM_SSHIFT;
	*cflags |= ((offset + offset2) << HME_XD_TXCKSUM_OSHIFT); 
	*cflags |= HME_XD_TXCKSUM;
}

/*
 * Routine to dma map an mbuf chain, set up the descriptor rings accordingly and
 * start the transmission.
 * Returns 0 on success, -1 if there were not enough free descriptors to map
 * the packet, or an errno otherwise.
 */
static int
hme_load_txmbuf(struct hme_softc *sc, struct mbuf *m0)
{
	struct hme_txdma_arg cba;
	struct hme_txdesc *td;
	int error, si, ri;
	u_int32_t flags, cflags = 0;

	si = sc->sc_rb.rb_tdhead;
	if ((td = STAILQ_FIRST(&sc->sc_rb.rb_txfreeq)) == NULL)
		return (-1);
	if ((m0->m_pkthdr.csum_flags & sc->sc_csum_features) != 0)
		hme_txcksum(m0, &cflags);
	cba.hta_sc = sc;
	cba.hta_htx = td;
	if ((error = bus_dmamap_load_mbuf(sc->sc_tdmatag, td->htx_dmamap,
	     m0, hme_txdma_callback, &cba, 0)) != 0)
		goto fail;
	if (cba.hta_ndescs == -1) {
		error = -1;
		goto fail;
	}
	bus_dmamap_sync(sc->sc_tdmatag, td->htx_dmamap,
	    BUS_DMASYNC_PREWRITE);

	STAILQ_REMOVE_HEAD(&sc->sc_rb.rb_txfreeq, htx_q);
	STAILQ_INSERT_TAIL(&sc->sc_rb.rb_txbusyq, td, htx_q);
	td->htx_m = m0;

	/* Turn descriptor ownership to the hme, back to forth. */
	ri = sc->sc_rb.rb_tdhead;
	CTR2(KTR_HME, "hme_load_mbuf: next desc is %d (%#x)",
	    ri, HME_XD_GETFLAGS(sc->sc_pci, sc->sc_rb.rb_txd, ri));
	do {
		ri = (ri + HME_NTXDESC - 1) % HME_NTXDESC;
		flags = HME_XD_GETFLAGS(sc->sc_pci, sc->sc_rb.rb_txd, ri) |
		    HME_XD_OWN | cflags;
		CTR3(KTR_HME, "hme_load_mbuf: activating ri %d, si %d (%#x)",
		    ri, si, flags);
		HME_XD_SETFLAGS(sc->sc_pci, sc->sc_rb.rb_txd, ri, flags);
	} while (ri != si);

	/* start the transmission. */
	HME_ETX_WRITE_4(sc, HME_ETXI_PENDING, HME_ETX_TP_DMAWAKEUP);
	return (0);
fail:
	bus_dmamap_unload(sc->sc_tdmatag, td->htx_dmamap);
	return (error);
}

/*
 * Pass a packet to the higher levels.
 */
static void
hme_read(struct hme_softc *sc, int ix, int len, u_int32_t flags)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct mbuf *m;

	if (len <= sizeof(struct ether_header) ||
	    len > HME_MAX_FRAMESIZE) {
#ifdef HMEDEBUG
		HME_WHINE(sc->sc_dev, "invalid packet size %d; dropping\n",
		    len);
#endif
		ifp->if_ierrors++;
		hme_discard_rxbuf(sc, ix);
		return;
	}

	m = sc->sc_rb.rb_rxdesc[ix].hrx_m;
	CTR1(KTR_HME, "hme_read: len %d", len);

	if (hme_add_rxbuf(sc, ix, 0) != 0) {
		/*
		 * hme_add_rxbuf will leave the old buffer in the ring until
		 * it is sure that a new buffer can be mapped. If it can not,
		 * drop the packet, but leave the interface up.
		 */
		ifp->if_iqdrops++;
		hme_discard_rxbuf(sc, ix);
		return;
	}

	ifp->if_ipackets++;

	m->m_pkthdr.rcvif = ifp;
	m->m_pkthdr.len = m->m_len = len + HME_RXOFFS;
	m_adj(m, HME_RXOFFS);
	/* RX TCP/UDP checksum */
	if (ifp->if_capenable & IFCAP_RXCSUM)
		hme_rxcksum(m, flags);
	/* Pass the packet up. */
	HME_UNLOCK(sc);
	(*ifp->if_input)(ifp, m);
	HME_LOCK(sc);
}

static void
hme_start(struct ifnet *ifp)
{
	struct hme_softc *sc = ifp->if_softc;

	HME_LOCK(sc);
	hme_start_locked(ifp);
	HME_UNLOCK(sc);
}

static void
hme_start_locked(struct ifnet *ifp)
{
	struct hme_softc *sc = (struct hme_softc *)ifp->if_softc;
	struct mbuf *m;
	int error, enq = 0;

	if ((ifp->if_flags & (IFF_RUNNING | IFF_OACTIVE)) != IFF_RUNNING)
		return;

	error = 0;
	for (;;) {
		IFQ_DRV_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;

		error = hme_load_txmbuf(sc, m);
		if (error == -1) {
			ifp->if_flags |= IFF_OACTIVE;
			IFQ_DRV_PREPEND(&ifp->if_snd, m);
			break;
		} else if (error > 0) {
			printf("hme_start: error %d while loading mbuf\n",
			    error);
		} else {
			enq = 1;
			BPF_MTAP(ifp, m);
		}
	}

	if (sc->sc_rb.rb_td_nbusy == HME_NTXDESC || error == -1)
		ifp->if_flags |= IFF_OACTIVE;
	/* Set watchdog timer if a packet was queued */
	if (enq) {
		bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap,
		    BUS_DMASYNC_PREWRITE);
		ifp->if_timer = 5;
	}
}

/*
 * Transmit interrupt.
 */
static void
hme_tint(struct hme_softc *sc)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct hme_txdesc *htx;
	unsigned int ri, txflags;

	/*
	 * Unload collision counters
	 */
	ifp->if_collisions +=
		HME_MAC_READ_4(sc, HME_MACI_NCCNT) +
		HME_MAC_READ_4(sc, HME_MACI_FCCNT) +
		HME_MAC_READ_4(sc, HME_MACI_EXCNT) +
		HME_MAC_READ_4(sc, HME_MACI_LTCNT);

	/*
	 * then clear the hardware counters.
	 */
	HME_MAC_WRITE_4(sc, HME_MACI_NCCNT, 0);
	HME_MAC_WRITE_4(sc, HME_MACI_FCCNT, 0);
	HME_MAC_WRITE_4(sc, HME_MACI_EXCNT, 0);
	HME_MAC_WRITE_4(sc, HME_MACI_LTCNT, 0);

	htx = STAILQ_FIRST(&sc->sc_rb.rb_txbusyq);
	bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap, BUS_DMASYNC_POSTREAD);
	/* Fetch current position in the transmit ring */
	for (ri = sc->sc_rb.rb_tdtail;; ri = (ri + 1) % HME_NTXDESC) {
		if (sc->sc_rb.rb_td_nbusy <= 0) {
			CTR0(KTR_HME, "hme_tint: not busy!");
			break;
		}

		txflags = HME_XD_GETFLAGS(sc->sc_pci, sc->sc_rb.rb_txd, ri);
		CTR2(KTR_HME, "hme_tint: index %d, flags %#x", ri, txflags);

		if ((txflags & HME_XD_OWN) != 0)
			break;

		CTR0(KTR_HME, "hme_tint: not owned");
		--sc->sc_rb.rb_td_nbusy;
		ifp->if_flags &= ~IFF_OACTIVE;

		/* Complete packet transmitted? */
		if ((txflags & HME_XD_EOP) == 0)
			continue;

		KASSERT(htx->htx_lastdesc == ri,
		    ("hme_tint: ring indices skewed: %d != %d!",
		     htx->htx_lastdesc, ri));
		bus_dmamap_sync(sc->sc_tdmatag, htx->htx_dmamap,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_tdmatag, htx->htx_dmamap);

		ifp->if_opackets++;
		m_freem(htx->htx_m);
		htx->htx_m = NULL;
		STAILQ_REMOVE_HEAD(&sc->sc_rb.rb_txbusyq, htx_q);
		STAILQ_INSERT_TAIL(&sc->sc_rb.rb_txfreeq, htx, htx_q);
		htx = STAILQ_FIRST(&sc->sc_rb.rb_txbusyq);
	}
	/* Turn off watchdog */
	if (sc->sc_rb.rb_td_nbusy == 0)
		ifp->if_timer = 0;

	/* Update ring */
	sc->sc_rb.rb_tdtail = ri;

	hme_start_locked(ifp);

	if (sc->sc_rb.rb_td_nbusy == 0)
		ifp->if_timer = 0;
}

/*
 * RX TCP/UDP checksum 
 */
static void
hme_rxcksum(struct mbuf *m, u_int32_t flags)
{
	struct ether_header *eh;
	struct ip *ip;
	struct udphdr *uh;
	int32_t hlen, len, pktlen;
	u_int16_t cksum, *opts;
	u_int32_t temp32;

	pktlen = m->m_pkthdr.len;
	if (pktlen < sizeof(struct ether_header) + sizeof(struct ip))
		return;
	eh = mtod(m, struct ether_header *);
	if (eh->ether_type != htons(ETHERTYPE_IP))
		return;
	ip = (struct ip *)(eh + 1);
	if (ip->ip_v != IPVERSION)
		return;

	hlen = ip->ip_hl << 2;
	pktlen -= sizeof(struct ether_header);
	if (hlen < sizeof(struct ip))
		return;
	if (ntohs(ip->ip_len) < hlen)
		return;
	if (ntohs(ip->ip_len) != pktlen)
		return;
	if (ip->ip_off & htons(IP_MF | IP_OFFMASK))
		return;	/* can't handle fragmented packet */

	switch (ip->ip_p) {
	case IPPROTO_TCP:
		if (pktlen < (hlen + sizeof(struct tcphdr)))
			return;
		break;
	case IPPROTO_UDP:
		if (pktlen < (hlen + sizeof(struct udphdr)))
			return;
		uh = (struct udphdr *)((caddr_t)ip + hlen);
		if (uh->uh_sum == 0)
			return; /* no checksum */
		break;
	default:
		return;
	}

	cksum = ~(flags & HME_XD_RXCKSUM);
	/* checksum fixup for IP options */
	len = hlen - sizeof(struct ip);
	if (len > 0) {
		opts = (u_int16_t *)(ip + 1);
		for (; len > 0; len -= sizeof(u_int16_t), opts++) {
			temp32 = cksum - *opts;
			temp32 = (temp32 >> 16) + (temp32 & 65535);
			cksum = temp32 & 65535;
		}
	}
	m->m_pkthdr.csum_flags |= CSUM_DATA_VALID;
	m->m_pkthdr.csum_data = cksum;
}

/*
 * Receive interrupt.
 */
static void
hme_rint(struct hme_softc *sc)
{
	caddr_t xdr = sc->sc_rb.rb_rxd;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	unsigned int ri, len;
	int progress = 0;
	u_int32_t flags;

	/*
	 * Process all buffers with valid data.
	 */
	bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap, BUS_DMASYNC_POSTREAD);
	for (ri = sc->sc_rb.rb_rdtail;; ri = (ri + 1) % HME_NRXDESC) {
		flags = HME_XD_GETFLAGS(sc->sc_pci, xdr, ri);
		CTR2(KTR_HME, "hme_rint: index %d, flags %#x", ri, flags);
		if ((flags & HME_XD_OWN) != 0)
			break;

		progress++;
		if ((flags & HME_XD_OFL) != 0) {
			device_printf(sc->sc_dev, "buffer overflow, ri=%d; "
			    "flags=0x%x\n", ri, flags);
			ifp->if_ierrors++;
			hme_discard_rxbuf(sc, ri);
		} else {
			len = HME_XD_DECODE_RSIZE(flags);
			hme_read(sc, ri, len, flags);
		}
	}
	if (progress) {
		bus_dmamap_sync(sc->sc_cdmatag, sc->sc_cdmamap,
		    BUS_DMASYNC_PREWRITE);
	}
	sc->sc_rb.rb_rdtail = ri;
}

static void
hme_eint(struct hme_softc *sc, u_int status)
{

	if ((status & HME_SEB_STAT_MIFIRQ) != 0) {
		device_printf(sc->sc_dev, "XXXlink status changed\n");
		return;
	}

	HME_WHINE(sc->sc_dev, "error signaled, status=%#x\n", status);
}

void
hme_intr(void *v)
{
	struct hme_softc *sc = (struct hme_softc *)v;
	u_int32_t status;

	HME_LOCK(sc);
	status = HME_SEB_READ_4(sc, HME_SEBI_STAT);
	CTR1(KTR_HME, "hme_intr: status %#x", (u_int)status);

	if ((status & HME_SEB_STAT_ALL_ERRORS) != 0)
		hme_eint(sc, status);

	if ((status & (HME_SEB_STAT_TXALL | HME_SEB_STAT_HOSTTOTX)) != 0)
		hme_tint(sc);

	if ((status & HME_SEB_STAT_RXTOHOST) != 0)
		hme_rint(sc);
	HME_UNLOCK(sc);
}


static void
hme_watchdog(struct ifnet *ifp)
{
	struct hme_softc *sc = ifp->if_softc;
#ifdef HMEDEBUG
	u_int32_t status;
#endif

	HME_LOCK(sc);
#ifdef HMEDEBUG
	status = HME_SEB_READ_4(sc, HME_SEBI_STAT);
	CTR1(KTR_HME, "hme_watchdog: status %x", (u_int)status);
#endif
	device_printf(sc->sc_dev, "device timeout\n");
	++ifp->if_oerrors;
	HME_UNLOCK(sc);

	hme_reset(sc);
}

/*
 * Initialize the MII Management Interface
 */
static void
hme_mifinit(struct hme_softc *sc)
{
	u_int32_t v;

	HME_LOCK_ASSERT(sc, MA_OWNED);

	/* Configure the MIF in frame mode */
	v = HME_MIF_READ_4(sc, HME_MIFI_CFG);
	v &= ~HME_MIF_CFG_BBMODE;
	HME_MIF_WRITE_4(sc, HME_MIFI_CFG, v);
}

/*
 * MII interface
 */
int
hme_mii_readreg(device_t dev, int phy, int reg)
{
	struct hme_softc *sc = device_get_softc(dev);
	int n;
	u_int32_t v;

	HME_LOCK(sc);
	/* Select the desired PHY in the MIF configuration register */
	v = HME_MIF_READ_4(sc, HME_MIFI_CFG);
	/* Clear PHY select bit */
	v &= ~HME_MIF_CFG_PHY;
	if (phy == HME_PHYAD_EXTERNAL)
		/* Set PHY select bit to get at external device */
		v |= HME_MIF_CFG_PHY;
	HME_MIF_WRITE_4(sc, HME_MIFI_CFG, v);

	/* Construct the frame command */
	v = (MII_COMMAND_START << HME_MIF_FO_ST_SHIFT) |
	    HME_MIF_FO_TAMSB |
	    (MII_COMMAND_READ << HME_MIF_FO_OPC_SHIFT) |
	    (phy << HME_MIF_FO_PHYAD_SHIFT) |
	    (reg << HME_MIF_FO_REGAD_SHIFT);

	HME_MIF_WRITE_4(sc, HME_MIFI_FO, v);
	for (n = 0; n < 100; n++) {
		DELAY(1);
		v = HME_MIF_READ_4(sc, HME_MIFI_FO);
		if (v & HME_MIF_FO_TALSB) {
			HME_UNLOCK(sc);
			return (v & HME_MIF_FO_DATA);
		}
	}

	device_printf(sc->sc_dev, "mii_read timeout\n");
	HME_UNLOCK(sc);
	return (0);
}

int
hme_mii_writereg(device_t dev, int phy, int reg, int val)
{
	struct hme_softc *sc = device_get_softc(dev);
	int n;
	u_int32_t v;

	HME_LOCK(sc);
	/* Select the desired PHY in the MIF configuration register */
	v = HME_MIF_READ_4(sc, HME_MIFI_CFG);
	/* Clear PHY select bit */
	v &= ~HME_MIF_CFG_PHY;
	if (phy == HME_PHYAD_EXTERNAL)
		/* Set PHY select bit to get at external device */
		v |= HME_MIF_CFG_PHY;
	HME_MIF_WRITE_4(sc, HME_MIFI_CFG, v);

	/* Construct the frame command */
	v = (MII_COMMAND_START << HME_MIF_FO_ST_SHIFT)	|
	    HME_MIF_FO_TAMSB				|
	    (MII_COMMAND_WRITE << HME_MIF_FO_OPC_SHIFT)	|
	    (phy << HME_MIF_FO_PHYAD_SHIFT)		|
	    (reg << HME_MIF_FO_REGAD_SHIFT)		|
	    (val & HME_MIF_FO_DATA);

	HME_MIF_WRITE_4(sc, HME_MIFI_FO, v);
	for (n = 0; n < 100; n++) {
		DELAY(1);
		v = HME_MIF_READ_4(sc, HME_MIFI_FO);
		if (v & HME_MIF_FO_TALSB) {
			HME_UNLOCK(sc);
			return (1);
		}
	}

	device_printf(sc->sc_dev, "mii_write timeout\n");
	HME_UNLOCK(sc);
	return (0);
}

void
hme_mii_statchg(device_t dev)
{
	struct hme_softc *sc = device_get_softc(dev);
	int instance;
	int phy;
	u_int32_t v;

	HME_LOCK(sc);
	instance = IFM_INST(sc->sc_mii->mii_media.ifm_cur->ifm_media);
	phy = sc->sc_phys[instance];
#ifdef HMEDEBUG
	if (sc->sc_debug)
		printf("hme_mii_statchg: status change: phy = %d\n", phy);
#endif

	/* Select the current PHY in the MIF configuration register */
	v = HME_MIF_READ_4(sc, HME_MIFI_CFG);
	v &= ~HME_MIF_CFG_PHY;
	if (phy == HME_PHYAD_EXTERNAL)
		v |= HME_MIF_CFG_PHY;
	HME_MIF_WRITE_4(sc, HME_MIFI_CFG, v);

	/* Set the MAC Full Duplex bit appropriately */
	v = HME_MAC_READ_4(sc, HME_MACI_TXCFG);
	if (!hme_mac_bitflip(sc, HME_MACI_TXCFG, v, HME_MAC_TXCFG_ENABLE, 0)) {
		HME_UNLOCK(sc);
		return;
	}
	if ((IFM_OPTIONS(sc->sc_mii->mii_media_active) & IFM_FDX) != 0)
		v |= HME_MAC_TXCFG_FULLDPLX;
	else
		v &= ~HME_MAC_TXCFG_FULLDPLX;
	HME_MAC_WRITE_4(sc, HME_MACI_TXCFG, v);
	if (!hme_mac_bitflip(sc, HME_MACI_TXCFG, v, 0, HME_MAC_TXCFG_ENABLE)) {
		HME_UNLOCK(sc);
		return;
	}
	HME_UNLOCK(sc);
}

static int
hme_mediachange(struct ifnet *ifp)
{
	struct hme_softc *sc = ifp->if_softc;

	return (mii_mediachg(sc->sc_mii));
}

static void
hme_mediastatus(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct hme_softc *sc = ifp->if_softc;

	HME_LOCK(sc);
	if ((ifp->if_flags & IFF_UP) == 0) {
		HME_UNLOCK(sc);
		return;
	}

	HME_UNLOCK(sc);
	mii_pollstat(sc->sc_mii);
	HME_LOCK(sc);
	ifmr->ifm_active = sc->sc_mii->mii_media_active;
	ifmr->ifm_status = sc->sc_mii->mii_media_status;
	HME_UNLOCK(sc);
}

/*
 * Process an ioctl request.
 */
static int
hme_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct hme_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	int s, error = 0;

	HME_LOCK(sc);
	s = splnet();

	switch (cmd) {
	case SIOCSIFFLAGS:
		if ((ifp->if_flags & IFF_UP) == 0 &&
		    (ifp->if_flags & IFF_RUNNING) != 0) {
			/*
			 * If interface is marked down and it is running, then
			 * stop it.
			 */
			hme_stop(sc);
			ifp->if_flags &= ~IFF_RUNNING;
		} else if ((ifp->if_flags & IFF_UP) != 0 &&
		    	   (ifp->if_flags & IFF_RUNNING) == 0) {
			/*
			 * If interface is marked up and it is stopped, then
			 * start it.
			 */
			hme_init_locked(sc);
		} else if ((ifp->if_flags & IFF_UP) != 0) {
			/*
			 * Reset the interface to pick up changes in any other
			 * flags that affect hardware registers.
			 */
			hme_init_locked(sc);
		}
		if ((ifp->if_flags & IFF_LINK0) != 0)
			sc->sc_csum_features |= CSUM_UDP;
		else
			sc->sc_csum_features &= ~CSUM_UDP;
		if ((ifp->if_capenable & IFCAP_TXCSUM) != 0)
			ifp->if_hwassist = sc->sc_csum_features;
#ifdef HMEDEBUG
		sc->sc_debug = (ifp->if_flags & IFF_DEBUG) != 0 ? 1 : 0;
#endif
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		hme_setladrf(sc, 1);
		error = 0;
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		HME_UNLOCK(sc);
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_mii->mii_media, cmd);
		HME_LOCK(sc);
		break;
	case SIOCSIFCAP:
		ifp->if_capenable = ifr->ifr_reqcap;
		if ((ifp->if_capenable & IFCAP_TXCSUM) != 0)
			ifp->if_hwassist = sc->sc_csum_features;
		else
			ifp->if_hwassist = 0;
		break;
	default:
		HME_UNLOCK(sc);
		error = ether_ioctl(ifp, cmd, data);
		HME_LOCK(sc);
		break;
	}

	splx(s);
	HME_UNLOCK(sc);
	return (error);
}

/*
 * Set up the logical address filter.
 */
static void
hme_setladrf(struct hme_softc *sc, int reenable)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct ifmultiaddr *inm;
	u_int32_t crc;
	u_int32_t hash[4];
	u_int32_t macc;

	HME_LOCK_ASSERT(sc, MA_OWNED);
	/* Clear hash table */
	hash[3] = hash[2] = hash[1] = hash[0] = 0;

	/* Get current RX configuration */
	macc = HME_MAC_READ_4(sc, HME_MACI_RXCFG);

	/*
	 * Disable the receiver while changing it's state as the documentation
	 * mandates.
	 * We then must wait until the bit clears in the register. This should
	 * take at most 3.5ms.
	 */
	if (!hme_mac_bitflip(sc, HME_MACI_RXCFG, macc, HME_MAC_RXCFG_ENABLE, 0))
		return;
	/* Disable the hash filter before writing to the filter registers. */
	if (!hme_mac_bitflip(sc, HME_MACI_RXCFG, macc,
	    HME_MAC_RXCFG_HENABLE, 0))
		return;

	if (reenable)
		macc |= HME_MAC_RXCFG_ENABLE;
	else
		macc &= ~HME_MAC_RXCFG_ENABLE;

	if ((ifp->if_flags & IFF_PROMISC) != 0) {
		/* Turn on promiscuous mode; turn off the hash filter */
		macc |= HME_MAC_RXCFG_PMISC;
		macc &= ~HME_MAC_RXCFG_HENABLE;
		ifp->if_flags |= IFF_ALLMULTI;
		goto chipit;
	}

	/* Turn off promiscuous mode; turn on the hash filter */
	macc &= ~HME_MAC_RXCFG_PMISC;
	macc |= HME_MAC_RXCFG_HENABLE;

	/*
	 * Set up multicast address filter by passing all multicast addresses
	 * through a crc generator, and then using the high order 6 bits as an
	 * index into the 64 bit logical address filter.  The high order bit
	 * selects the word, while the rest of the bits select the bit within
	 * the word.
	 */

	TAILQ_FOREACH(inm, &sc->sc_arpcom.ac_if.if_multiaddrs, ifma_link) {
		if (inm->ifma_addr->sa_family != AF_LINK)
			continue;
		crc = ether_crc32_le(LLADDR((struct sockaddr_dl *)
		    inm->ifma_addr), ETHER_ADDR_LEN);

		/* Just want the 6 most significant bits. */
		crc >>= 26;

		/* Set the corresponding bit in the filter. */
		hash[crc >> 4] |= 1 << (crc & 0xf);
	}

	ifp->if_flags &= ~IFF_ALLMULTI;

chipit:
	/* Now load the hash table into the chip */
	HME_MAC_WRITE_4(sc, HME_MACI_HASHTAB0, hash[0]);
	HME_MAC_WRITE_4(sc, HME_MACI_HASHTAB1, hash[1]);
	HME_MAC_WRITE_4(sc, HME_MACI_HASHTAB2, hash[2]);
	HME_MAC_WRITE_4(sc, HME_MACI_HASHTAB3, hash[3]);
	hme_mac_bitflip(sc, HME_MACI_RXCFG, macc, 0,
	    macc & (HME_MAC_RXCFG_ENABLE | HME_MAC_RXCFG_HENABLE));
}
