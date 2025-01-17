/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * Copyright (c) 2008 The DragonFly Project.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
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
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)autoconf.c	7.1 (Berkeley) 5/9/91
 * $FreeBSD: src/sys/i386/i386/autoconf.c,v 1.146.2.2 2001/06/07 06:05:58 dd Exp $
 */

/*
 * Setup the system to run on the current machine.
 *
 * Configure() is called at boot time and initializes the vba
 * device tables and the memory controller monitoring.  Available
 * devices are determined (from possibilities mentioned in ioconf.c),
 * and the drivers are initialized.
 */
#include "opt_bootp.h"
#include "opt_ffs.h"
#include "opt_cd9660.h"
#include "opt_nfs.h"
#include "opt_nfsroot.h"
#include "opt_rootdevname.h"

#include "use_isa.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bootmaj.h>
#include <sys/bus.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/diskslice.h>
#include <sys/reboot.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/cons.h>
#include <sys/thread.h>
#include <sys/device.h>
#include <sys/machintr.h>

#include <vm/vm_kern.h>
#include <vm/vm_extern.h>
#include <vm/vm_pager.h>

#if 0
#include <machine/pcb.h>
#include <machine/pcb_ext.h>
#endif
#include <machine/smp.h>
#include <machine/globaldata.h>
#include <machine/md_var.h>

#if NISA > 0
#include <bus/isa/isavar.h>

device_t isa_bus_device = NULL;
#endif

static void cpu_startup (void *);
static void configure_first (void *);
static void configure (void *);
static void configure_final (void *);

#if defined(FFS) && defined(FFS_ROOT)
static void	setroot (void);
#endif

#if defined(NFS) && defined(NFS_ROOT)
#if !defined(BOOTP_NFSROOT)
static void	pxe_setup_nfsdiskless(void);
#endif
#endif

SYSINIT(cpu, SI_BOOT2_START_CPU, SI_ORDER_FIRST, cpu_startup, NULL);
SYSINIT(configure1, SI_SUB_CONFIGURE, SI_ORDER_FIRST, configure_first, NULL);
/* SI_ORDER_SECOND is hookable */
SYSINIT(configure2, SI_SUB_CONFIGURE, SI_ORDER_THIRD, configure, NULL);
/* SI_ORDER_MIDDLE is hookable */
SYSINIT(configure3, SI_SUB_CONFIGURE, SI_ORDER_ANY, configure_final, NULL);

cdev_t	rootdev = NULL;
cdev_t	dumpdev = NULL;

/*
 * nfsroot.iosize may be set in loader.conf, 32768 is recommended to
 * be able to max-out a GigE link if the server supports it.  Many servers
 * do not so the default is 8192.
 *
 * nfsroot.rahead defaults to something reasonable, can be overridden.
 */
static int nfsroot_iosize = 8192;
TUNABLE_INT("nfsroot.iosize", &nfsroot_iosize);
static int nfsroot_rahead = 4;
TUNABLE_INT("nfsroot.rahead", &nfsroot_rahead);

/*
 *
 */
static void
cpu_startup(void *dummy)
{
	vm_offset_t buffer_sva;
	vm_offset_t buffer_eva;
	vm_offset_t pager_sva;
	vm_offset_t pager_eva;

	kprintf("%s", version);
	kprintf("real memory = %ju (%juK bytes)\n",
	    (uintmax_t)ptoa(Maxmem), (uintmax_t)(ptoa(Maxmem) / 1024));

	if (nbuf == 0) {
		int factor = 4 * BKVASIZE / 1024;
		int kbytes = Maxmem * (PAGE_SIZE / 1024);

		nbuf = 50;
		if (kbytes > 4096)
			nbuf += min((kbytes - 4096) / factor, 65536 / factor);
		if (kbytes > 65536)
			nbuf += (kbytes - 65536) * 2 / (factor * 5);
		if (maxbcache && nbuf > maxbcache / BKVASIZE)
			nbuf = maxbcache / BKVASIZE;
	}
	if (nbuf > (virtual_end - virtual_start) / (BKVASIZE * 2)) {
		nbuf = (virtual_end - virtual_start) / (BKVASIZE * 2);
		kprintf("Warning: nbufs capped at %ld\n", nbuf);
	}

	nswbuf = lmax(lmin(nbuf / 4, 256), 16);
#ifdef NSWBUF_MIN
	if (nswbuf < NSWBUF_MIN)
		nswbuf = NSWBUF_MIN;
#endif

	/*
	 * Allocate memory for the buffer cache
	 */
	buf = (void *)kmem_alloc(&kernel_map, nbuf * sizeof(struct buf));
	swbuf = (void *)kmem_alloc(&kernel_map, nswbuf * sizeof(struct buf));


#ifdef DIRECTIO
        ffs_rawread_setup();
#endif
	kmem_suballoc(&kernel_map, &clean_map, &clean_sva, &clean_eva,
		      (nbuf*BKVASIZE*2) + (nswbuf*MAXPHYS) + pager_map_size);
	kmem_suballoc(&clean_map, &buffer_map, &buffer_sva, &buffer_eva,
		      (nbuf*BKVASIZE*2));
	buffer_map.system_map = 1;
	kmem_suballoc(&clean_map, &pager_map, &pager_sva, &pager_eva,
		      (nswbuf*MAXPHYS) + pager_map_size);
	pager_map.system_map = 1;
	kprintf("avail memory = %lu (%luK bytes)\n", ptoa(vmstats.v_free_count),
		ptoa(vmstats.v_free_count) / 1024);
	mp_start();
	mp_announce();
	cpu_setregs();
}

/*
 * Determine i/o configuration for a machine.
 */
static void
configure_first(void *dummy)
{
}

static void
configure(void *dummy)
{
        /*
	 * Final interrupt support acviation, then enable hardware interrupts.
	 */
	MachIntrABI.finalize();
	cpu_enable_intr();

	/*
	 * This will configure all devices, generally starting with the
	 * nexus (i386/i386/nexus.c).  The nexus ISA code explicitly
	 * dummies up the attach in order to delay legacy initialization
	 * until after all other busses/subsystems have had a chance
	 * at those resources.
	 */
	root_bus_configure();

#if NISA > 0
	/*
	 * Explicitly probe and attach ISA last.  The isa bus saves
	 * it's device node at attach time for us here.
	 */
	if (isa_bus_device)
		isa_probe_children(isa_bus_device);
#endif

	/*
	 * Allow lowering of the ipl to the lowest kernel level if we
	 * panic (or call tsleep() before clearing `cold').  No level is
	 * completely safe (since a panic may occur in a critical region
	 * at splhigh()), but we want at least bio interrupts to work.
	 */
	safepri = TDPRI_KERN_USER;
}

static void
configure_final(void *dummy)
{
	cninit_finish();

	if (bootverbose)
		kprintf("Device configuration finished.\n");
}

#ifdef BOOTP
void bootpc_init(void);
#endif
/*
 * Do legacy root filesystem discovery.
 */
void
cpu_rootconf(void)
{
#ifdef BOOTP
        bootpc_init();
#endif
#if defined(NFS) && defined(NFS_ROOT)
#if !defined(BOOTP_NFSROOT)
	pxe_setup_nfsdiskless();
	if (nfs_diskless_valid)
#endif
		rootdevnames[0] = "nfs:";
#endif
#if defined(FFS) && defined(FFS_ROOT)
        if (!rootdevnames[0])
                setroot();
#endif
}
SYSINIT(cpu_rootconf, SI_SUB_ROOT_CONF, SI_ORDER_FIRST, cpu_rootconf, NULL);

u_long	bootdev = 0;		/* not a cdev_t - encoding is different */

#if defined(FFS) && defined(FFS_ROOT)

/*
 * The boot code uses old block device major numbers to pass bootdev to
 * us.  We have to translate these to character device majors because
 * we don't have block devices any more.
 */
static int
boot_translate_majdev(int bmajor)
{
	static int conv[] = { BOOTMAJOR_CONVARY };

	if (bmajor >= 0 && bmajor < NELEM(conv))
		return(conv[bmajor]);
	return(-1);
}

/*
 * Attempt to find the device from which we were booted.
 * If we can do so, and not instructed not to do so,
 * set rootdevs[] and rootdevnames[] to correspond to the
 * boot device(s).
 *
 * This code survives in order to allow the system to be
 * booted from legacy environments that do not correctly
 * populate the kernel environment. There are significant
 * restrictions on the bootability of the system in this
 * situation; it can only be mounting root from a 'da'
 * 'wd' or 'fd' device, and the root filesystem must be ufs.
 */
static void
setroot(void)
{
	int majdev, mindev, unit, slice, part;
	cdev_t newrootdev, dev;
	char partname[2];
	char *sname;

	if ((bootdev & B_MAGICMASK) != B_DEVMAGIC) {
		kprintf("no B_DEVMAGIC (bootdev=%#lx)\n", bootdev);
		return;
	}
	majdev = boot_translate_majdev(B_TYPE(bootdev));
	if (bootverbose) {
		kprintf("bootdev: %08lx type=%ld unit=%ld "
			"slice=%ld part=%ld major=%d\n",
			bootdev, B_TYPE(bootdev), B_UNIT(bootdev),
			B_SLICE(bootdev), B_PARTITION(bootdev), majdev);
	}
	dev = udev2dev(makeudev(majdev, 0), 0);
	if (!dev_is_good(dev))
		return;
	unit = B_UNIT(bootdev);
	slice = B_SLICE(bootdev);
	if (slice == WHOLE_DISK_SLICE)
		slice = COMPATIBILITY_SLICE;
	if (slice < 0 || slice >= MAX_SLICES) {
		kprintf("bad slice\n");
		return;
	}

	part = B_PARTITION(bootdev);
	mindev = dkmakeminor(unit, slice, part);
	newrootdev = udev2dev(makeudev(majdev, mindev), 0);
	if (!dev_is_good(newrootdev))
		return;
	sname = dsname(newrootdev, unit, slice, part, partname);
	rootdevnames[0] = kmalloc(strlen(sname) + 6, M_DEVBUF, M_WAITOK);
	ksprintf(rootdevnames[0], "ufs:%s%s", sname, partname);

	/*
	 * For properly dangerously dedicated disks (ones with a historical
	 * bogus partition table), the boot blocks will give slice = 4, but
	 * the kernel will only provide the compatibility slice since it
	 * knows that slice 4 is not a real slice.  Arrange to try mounting
	 * the compatibility slice as root if mounting the slice passed by
	 * the boot blocks fails.  This handles the dangerously dedicated
	 * case and perhaps others.
	 */
	if (slice == COMPATIBILITY_SLICE)
		return;
	slice = COMPATIBILITY_SLICE;
	sname = dsname(newrootdev, unit, slice, part, partname);
	rootdevnames[1] = kmalloc(strlen(sname) + 6, M_DEVBUF, M_WAITOK);
	ksprintf(rootdevnames[1], "ufs:%s%s", sname, partname);
}
#endif

#if defined(NFS) && defined(NFS_ROOT)
#if !defined(BOOTP_NFSROOT)

#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <vfs/nfs/rpcv2.h>
#include <vfs/nfs/nfsproto.h>
#include <vfs/nfs/nfs.h>
#include <vfs/nfs/nfsdiskless.h>

extern struct nfs_diskless	nfs_diskless;

/*
 * Convert a kenv variable to a sockaddr.  If the kenv variable does not
 * exist the sockaddr will remain zerod out (callers typically just check
 * sin_len).  A network address of 0.0.0.0 is equivalent to failure.
 */
static int
inaddr_to_sockaddr(char *ev, struct sockaddr_in *sa)
{
	u_int32_t	a[4];
	char		*cp;

	bzero(sa, sizeof(*sa));

	if ((cp = kgetenv(ev)) == NULL)
		return(1);
	if (ksscanf(cp, "%d.%d.%d.%d", &a[0], &a[1], &a[2], &a[3]) != 4)
		return(1);
	if (a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0)
		return(1);
	/* XXX is this ordering correct? */
	sa->sin_addr.s_addr = (a[3] << 24) + (a[2] << 16) + (a[1] << 8) + a[0];
	sa->sin_len = sizeof(*sa);
	sa->sin_family = AF_INET;
	return(0);
}

static int
hwaddr_to_sockaddr(char *ev, struct sockaddr_dl *sa)
{
	char		*cp;
	u_int32_t	a[6];

	bzero(sa, sizeof(*sa));
	sa->sdl_len = sizeof(*sa);
	sa->sdl_family = AF_LINK;
	sa->sdl_type = IFT_ETHER;
	sa->sdl_alen = ETHER_ADDR_LEN;
	if ((cp = kgetenv(ev)) == NULL)
		return(1);
	if (ksscanf(cp, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
		return(1);
	sa->sdl_data[0] = a[0];
	sa->sdl_data[1] = a[1];
	sa->sdl_data[2] = a[2];
	sa->sdl_data[3] = a[3];
	sa->sdl_data[4] = a[4];
	sa->sdl_data[5] = a[5];
	return(0);
}

static int
decode_nfshandle(char *ev, u_char *fh)
{
	u_char	*cp;
	int	len, val;

	if (((cp = kgetenv(ev)) == NULL) || (strlen(cp) < 2) || (*cp != 'X'))
		return(0);
	len = 0;
	cp++;
	for (;;) {
		if (*cp == 'X')
			return(len);
		if ((ksscanf(cp, "%2x", &val) != 1) || (val > 0xff))
			return(0);
		*(fh++) = val;
		len++;
		cp += 2;
		if (len > NFSX_V2FH)
		    return(0);
	}
}

/*
 * Populate the essential fields in the nfsv3_diskless structure.
 *
 * The loader is expected to export the following environment variables:
 *
 * boot.netif.ip		IP address on boot interface
 * boot.netif.netmask		netmask on boot interface
 * boot.netif.gateway		default gateway (optional)
 * boot.netif.hwaddr		hardware address of boot interface
 * boot.netif.name		name of boot interface (instead of hw addr)
 * boot.nfsroot.server		IP address of root filesystem server
 * boot.nfsroot.path		path of the root filesystem on server
 * boot.nfsroot.nfshandle	NFS handle for root filesystem on server
 */
static void
pxe_setup_nfsdiskless(void)
{
	struct nfs_diskless	*nd = &nfs_diskless;
	struct ifnet		*ifp;
	struct ifaddr		*ifa;
	struct sockaddr_dl	*sdl, ourdl;
	struct sockaddr_in	myaddr, netmask;
	char			*cp;

	/* set up interface */
	if (inaddr_to_sockaddr("boot.netif.ip", &myaddr))
		return;
	if (inaddr_to_sockaddr("boot.netif.netmask", &netmask)) {
		kprintf("PXE: no netmask\n");
		return;
	}
	bcopy(&myaddr, &nd->myif.ifra_addr, sizeof(myaddr));
	bcopy(&myaddr, &nd->myif.ifra_broadaddr, sizeof(myaddr));
	((struct sockaddr_in *) &nd->myif.ifra_broadaddr)->sin_addr.s_addr =
		myaddr.sin_addr.s_addr | ~ netmask.sin_addr.s_addr;
	bcopy(&netmask, &nd->myif.ifra_mask, sizeof(netmask));

	if ((cp = kgetenv("boot.netif.name")) != NULL) {
		ifnet_lock();
		ifp = ifunit(cp);
		if (ifp) {
			strlcpy(nd->myif.ifra_name, ifp->if_xname,
			    sizeof(nd->myif.ifra_name));
			ifnet_unlock();
			goto match_done;
		}
		ifnet_unlock();
		kprintf("PXE: cannot find interface %s\n", cp);
		return;
	}

	if (hwaddr_to_sockaddr("boot.netif.hwaddr", &ourdl)) {
		kprintf("PXE: no hardware address\n");
		return;
	}
	ifa = NULL;
	ifnet_lock();
	TAILQ_FOREACH(ifp, &ifnetlist, if_link) {
		struct ifaddr_container *ifac;

		TAILQ_FOREACH(ifac, &ifp->if_addrheads[mycpuid], ifa_link) {
			ifa = ifac->ifa;

			if ((ifa->ifa_addr->sa_family == AF_LINK) &&
			    (sdl = ((struct sockaddr_dl *)ifa->ifa_addr))) {
				if ((sdl->sdl_type == ourdl.sdl_type) &&
				    (sdl->sdl_alen == ourdl.sdl_alen) &&
				    !bcmp(sdl->sdl_data + sdl->sdl_nlen,
					  ourdl.sdl_data + ourdl.sdl_nlen,
					  sdl->sdl_alen)) {
					strlcpy(nd->myif.ifra_name,
					    ifp->if_xname,
					    sizeof(nd->myif.ifra_name));
					ifnet_unlock();
					goto match_done;
				}
			}
		}
	}
	ifnet_unlock();
	kprintf("PXE: no interface\n");
	return;	/* no matching interface */
match_done:
	/* set up gateway */
	inaddr_to_sockaddr("boot.netif.gateway", &nd->mygateway);

	/* XXX set up swap? */

	/* set up root mount */
	nd->root_args.rsize = nfsroot_iosize;
	nd->root_args.wsize = nfsroot_iosize;
	nd->root_args.sotype = SOCK_STREAM;
	nd->root_args.readahead = nfsroot_rahead;
	nd->root_args.flags = NFSMNT_WSIZE | NFSMNT_RSIZE | NFSMNT_RESVPORT |
			      NFSMNT_READAHEAD;
	if (inaddr_to_sockaddr("boot.nfsroot.server", &nd->root_saddr)) {
		kprintf("PXE: no server\n");
		return;
	}
	nd->root_saddr.sin_port = htons(NFS_PORT);

	/*
	 * A tftp-only loader may pass NFS path information without a
	 * root handle.  Generate a warning but continue configuring.
	 */
	if (decode_nfshandle("boot.nfsroot.nfshandle", &nd->root_fh[0]) == 0) {
		kprintf("PXE: Warning, no NFS handle passed from loader\n");
	}
	if ((cp = kgetenv("boot.nfsroot.path")) != NULL)
		strncpy(nd->root_hostnam, cp, MNAMELEN - 1);

	nfs_diskless_valid = 1;
}

#endif
#endif
