/*-
 * Copyright (c) 2005 Nate Lawson
 * Copyright (c) 2000 Munehiro Matsuda
 * Copyright (c) 2000 Takanori Watanabe
 * Copyright (c) 2000 Mitsuru IWASAKI <iwasaki@FreeBSD.org>
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
 * $FreeBSD: head/sys/dev/acpica/acpi_cmbat.c 246128 2013-01-30 18:01:20Z sbz $
 */

#include "opt_acpi.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>

#include <sys/rman.h>

#include "acpi.h"
#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>

/* Number of times to retry initialization before giving up. */
#define ACPI_CMBAT_RETRY_MAX	6

/* Check the battery once a minute. */
#define	CMBAT_POLLRATE		(60 * hz)

/* Hooks for the ACPICA debugging infrastructure */
#define	_COMPONENT	ACPI_BATTERY
ACPI_MODULE_NAME("BATTERY")

#define	ACPI_BATTERY_BST_CHANGE	0x80
#define	ACPI_BATTERY_BIF_CHANGE	0x81

struct acpi_cmbat_softc {
    device_t	    dev;
    int		    flags;
    int		    bix_present;

    struct acpi_bif bif;
    struct acpi_bst bst;
    struct timespec bst_lastupdated;
};

ACPI_SERIAL_DECL(cmbat, "ACPI cmbat");

static int		acpi_cmbat_probe(device_t dev);
static int		acpi_cmbat_attach(device_t dev);
static int		acpi_cmbat_detach(device_t dev);
static int		acpi_cmbat_resume(device_t dev);
static void		acpi_cmbat_notify_handler(ACPI_HANDLE h, UINT32 notify,
			    void *context);
static int		acpi_cmbat_info_expired(struct timespec *lastupdated);
static void		acpi_cmbat_info_updated(struct timespec *lastupdated);
static void		acpi_cmbat_get_bst(void *arg);
static void		acpi_cmbat_get_bif_task(void *arg);
static void		acpi_cmbat_get_bif(void *arg);
static int		acpi_cmbat_bst(device_t dev, struct acpi_bst *bstp);
static int		acpi_cmbat_bif(device_t dev, struct acpi_bif *bifp);
static void		acpi_cmbat_init_battery(void *arg);

static device_method_t acpi_cmbat_methods[] = {
    /* Device interface */
    DEVMETHOD(device_probe,	acpi_cmbat_probe),
    DEVMETHOD(device_attach,	acpi_cmbat_attach),
    DEVMETHOD(device_detach,	acpi_cmbat_detach),
    DEVMETHOD(device_resume,	acpi_cmbat_resume),

    /* ACPI battery interface */
    DEVMETHOD(acpi_batt_get_info, acpi_cmbat_bif),
    DEVMETHOD(acpi_batt_get_status, acpi_cmbat_bst),

    DEVMETHOD_END
};

static driver_t acpi_cmbat_driver = {
    "battery",
    acpi_cmbat_methods,
    sizeof(struct acpi_cmbat_softc),
};

static devclass_t acpi_cmbat_devclass;
DRIVER_MODULE(acpi_cmbat, acpi, acpi_cmbat_driver, acpi_cmbat_devclass, NULL, NULL);
MODULE_DEPEND(acpi_cmbat, acpi, 1, 1, 1);

static int
acpi_cmbat_probe(device_t dev)
{
    static char *cmbat_ids[] = { "PNP0C0A", NULL };

    if (acpi_disabled("cmbat") ||
	ACPI_ID_PROBE(device_get_parent(dev), dev, cmbat_ids) == NULL)
	return (ENXIO);

    device_set_desc(dev, "ACPI Control Method Battery");
    return (0);
}

static int
acpi_cmbat_attach(device_t dev)
{
    int		error;
    ACPI_HANDLE	handle, h;
    struct acpi_cmbat_softc *sc;

    sc = device_get_softc(dev);
    handle = acpi_get_handle(dev);
    sc->dev = dev;

    ACPI_SERIAL_INIT(cmbat);

    timespecclear(&sc->bst_lastupdated);

    sc->bix_present = ACPI_SUCCESS(AcpiGetHandle(handle, "_BIX", &h));
    if (sc->bix_present)
	device_printf(dev, "supports extended information\n");

    error = acpi_battery_register(dev);
    if (error != 0) {
    	device_printf(dev, "registering battery failed\n");
	return (error);
    }

    /*
     * Install a system notify handler in addition to the device notify.
     * Toshiba notebook uses this alternate notify for its battery.
     */
    AcpiInstallNotifyHandler(handle, ACPI_ALL_NOTIFY,
	acpi_cmbat_notify_handler, dev);

    AcpiOsExecute(OSL_NOTIFY_HANDLER, acpi_cmbat_init_battery, dev);

    return (0);
}

static int
acpi_cmbat_detach(device_t dev)
{
    ACPI_HANDLE	handle;

    handle = acpi_get_handle(dev);
    AcpiRemoveNotifyHandler(handle, ACPI_ALL_NOTIFY, acpi_cmbat_notify_handler);
    acpi_battery_remove(dev);
    return (0);
}

static int
acpi_cmbat_resume(device_t dev)
{

    AcpiOsExecute(OSL_NOTIFY_HANDLER, acpi_cmbat_init_battery, dev);
    return (0);
}

static void
acpi_cmbat_notify_handler(ACPI_HANDLE h, UINT32 notify, void *context)
{
    struct acpi_cmbat_softc *sc;
    device_t dev;

    dev = (device_t)context;
    sc = device_get_softc(dev);

    switch (notify) {
    case ACPI_NOTIFY_DEVICE_CHECK:
    case ACPI_BATTERY_BST_CHANGE:
	/*
	 * Clear the last updated time.  The next call to retrieve the
	 * battery status will get the new value for us.
	 */
	timespecclear(&sc->bst_lastupdated);
	break;
    case ACPI_NOTIFY_BUS_CHECK:
    case ACPI_BATTERY_BIF_CHANGE:
	/*
	 * Queue a callback to get the current battery info from thread
	 * context.  It's not safe to block in a notify handler.
	 */
	AcpiOsExecute(OSL_NOTIFY_HANDLER, acpi_cmbat_get_bif_task, dev);
	break;
    }

    acpi_UserNotify("CMBAT", h, notify);
}

static int
acpi_cmbat_info_expired(struct timespec *lastupdated)
{
    struct timespec	curtime;

    ACPI_SERIAL_ASSERT(cmbat);

    if (lastupdated == NULL)
	return (TRUE);
    if (!timespecisset(lastupdated))
	return (TRUE);

    getnanotime(&curtime);
    timespecsub(&curtime, lastupdated);
    return (curtime.tv_sec < 0 ||
	    curtime.tv_sec > acpi_battery_get_info_expire());
}

static void
acpi_cmbat_info_updated(struct timespec *lastupdated)
{

    ACPI_SERIAL_ASSERT(cmbat);

    if (lastupdated != NULL)
	getnanotime(lastupdated);
}

static void
acpi_cmbat_get_bst(void *arg)
{
    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
    ACPI_OBJECT	*res;
    ACPI_HANDLE	h;
    ACPI_BUFFER	bst_buffer;
    device_t dev;

    ACPI_SERIAL_ASSERT(cmbat);

    dev = arg;
    sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
    bst_buffer.Pointer = NULL;
    bst_buffer.Length = ACPI_ALLOCATE_BUFFER;

    if (!acpi_cmbat_info_expired(&sc->bst_lastupdated))
	goto end;

    as = AcpiEvaluateObject(h, "_BST", NULL, &bst_buffer);
    if (ACPI_FAILURE(as)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "error fetching current battery status -- %s\n",
		    AcpiFormatException(as));
	goto end;
    }

    res = (ACPI_OBJECT *)bst_buffer.Pointer;
    if (!ACPI_PKG_VALID(res, 4)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "battery status corrupted\n");
	goto end;
    }

    if (acpi_PkgInt32(res, 0, &sc->bst.state) != 0)
	goto end;
    if (acpi_PkgInt32(res, 1, &sc->bst.rate) != 0)
	goto end;
    if (acpi_PkgInt32(res, 2, &sc->bst.cap) != 0)
	goto end;
    if (acpi_PkgInt32(res, 3, &sc->bst.volt) != 0)
	goto end;
    acpi_cmbat_info_updated(&sc->bst_lastupdated);

    /* Clear out undefined/extended bits that might be set by hardware. */
    sc->bst.state &= ACPI_BATT_STAT_BST_MASK;
    if ((sc->bst.state & ACPI_BATT_STAT_INVALID) == ACPI_BATT_STAT_INVALID)
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
	    "battery reports simultaneous charging and discharging\n");

    /* XXX If all batteries are critical, perhaps we should suspend. */
    if (sc->bst.state & ACPI_BATT_STAT_CRITICAL) {
    	if ((sc->flags & ACPI_BATT_STAT_CRITICAL) == 0) {
	    sc->flags |= ACPI_BATT_STAT_CRITICAL;
	    device_printf(dev, "critically low charge!\n");
	}
    } else
	sc->flags &= ~ACPI_BATT_STAT_CRITICAL;

end:
    if (bst_buffer.Pointer != NULL)
	AcpiOsFree(bst_buffer.Pointer);
}

/* XXX There should be a cleaner way to do this locking. */
static void
acpi_cmbat_get_bif_task(void *arg)
{

    ACPI_SERIAL_BEGIN(cmbat);
    acpi_cmbat_get_bif(arg);
    ACPI_SERIAL_END(cmbat);
}

static void
acpi_cmbat_get_bif(void *arg)
{
    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
    ACPI_OBJECT	*res;
    ACPI_HANDLE	h;
    ACPI_BUFFER	info_buffer;
    device_t dev;
    int i;

    ACPI_SERIAL_ASSERT(cmbat);

    dev = arg;
    sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
    info_buffer.Pointer = NULL;
    info_buffer.Length = ACPI_ALLOCATE_BUFFER;

    as = AcpiEvaluateObject(h, sc->bix_present ? "_BIX" : "_BIF", NULL,
	&info_buffer);
    if (ACPI_FAILURE(as)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "error fetching current %sbattery info -- %s\n",
		    sc->bix_present ? "extended " : "",
		    AcpiFormatException(as));
	goto end;
    }

    res = (ACPI_OBJECT *)info_buffer.Pointer;
    if (!ACPI_PKG_VALID(res, sc->bix_present ? 20 : 13)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "%sbattery info corrupted\n",
		    sc->bix_present ? "extended " : "");
	goto end;
    }

    i = sc->bix_present ? 1 : 0; /* _BIX: Skip Revision field. */
    if (acpi_PkgInt32(res, i++, &sc->bif.units) != 0)
	goto end;
    if (acpi_PkgInt32(res, i++, &sc->bif.dcap) != 0)
	goto end;
    if (acpi_PkgInt32(res, i++, &sc->bif.lfcap) != 0)
	goto end;
    if (acpi_PkgInt32(res, i++, &sc->bif.btech) != 0)
	goto end;
    if (acpi_PkgInt32(res, i++, &sc->bif.dvol) != 0)
	goto end;
    if (acpi_PkgInt32(res, i++, &sc->bif.wcap) != 0)
	goto end;
    if (acpi_PkgInt32(res, i++, &sc->bif.lcap) != 0)
	goto end;
    if (sc->bix_present)
	i += 6;	   /* _BIX: Continue with Battery Capacity Granularity 1. */
    if (acpi_PkgInt32(res, i++, &sc->bif.gra1) != 0)
	goto end;
    if (acpi_PkgInt32(res, i++, &sc->bif.gra2) != 0)
	goto end;
    if (acpi_PkgStr(res,  i++, sc->bif.model, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;
    if (acpi_PkgStr(res, i++, sc->bif.serial, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;
    if (acpi_PkgStr(res, i++, sc->bif.type, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;
    if (acpi_PkgStr(res, i++, sc->bif.oeminfo, ACPI_CMBAT_MAXSTRLEN) != 0)
	goto end;
    /* _BIX: Ignore Battery Swapping Capability field. */

end:
    if (info_buffer.Pointer != NULL)
	AcpiOsFree(info_buffer.Pointer);
}

static int
acpi_cmbat_bif(device_t dev, struct acpi_bif *bifp)
{
    struct acpi_cmbat_softc *sc;

    sc = device_get_softc(dev);

    /*
     * Just copy the data.  The only value that should change is the
     * last-full capacity, so we only update when we get a notify that says
     * the info has changed.  Many systems apparently take a long time to
     * process a _BIF call so we avoid it if possible.
     */
    ACPI_SERIAL_BEGIN(cmbat);
    bifp->units = sc->bif.units;
    bifp->dcap = sc->bif.dcap;
    bifp->lfcap = sc->bif.lfcap;
    bifp->btech = sc->bif.btech;
    bifp->dvol = sc->bif.dvol;
    bifp->wcap = sc->bif.wcap;
    bifp->lcap = sc->bif.lcap;
    bifp->gra1 = sc->bif.gra1;
    bifp->gra2 = sc->bif.gra2;
    strncpy(bifp->model, sc->bif.model, sizeof(sc->bif.model));
    strncpy(bifp->serial, sc->bif.serial, sizeof(sc->bif.serial));
    strncpy(bifp->type, sc->bif.type, sizeof(sc->bif.type));
    strncpy(bifp->oeminfo, sc->bif.oeminfo, sizeof(sc->bif.oeminfo));
    ACPI_SERIAL_END(cmbat);

    return (0);
}

static int
acpi_cmbat_bst(device_t dev, struct acpi_bst *bstp)
{
    struct acpi_cmbat_softc *sc;

    sc = device_get_softc(dev);

    ACPI_SERIAL_BEGIN(cmbat);
    if (acpi_BatteryIsPresent(dev)) {
	acpi_cmbat_get_bst(dev);
	bstp->state = sc->bst.state;
	bstp->rate = sc->bst.rate;
	bstp->cap = sc->bst.cap;
	bstp->volt = sc->bst.volt;
    } else
	bstp->state = ACPI_BATT_STAT_NOT_PRESENT;
    ACPI_SERIAL_END(cmbat);

    return (0);
}

static void
acpi_cmbat_init_battery(void *arg)
{
    struct acpi_cmbat_softc *sc;
    int		retry, valid;
    device_t	dev;

    dev = (device_t)arg;
    sc = device_get_softc(dev);
    ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		"battery initialization start\n");

    /*
     * Try repeatedly to get valid data from the battery.  Since the
     * embedded controller isn't always ready just after boot, we may have
     * to wait a while.
     */
    for (retry = 0; retry < ACPI_CMBAT_RETRY_MAX; retry++, AcpiOsSleep(10000)) {
	/* batteries on DOCK can be ejected w/ DOCK during retrying */
	if (!device_is_attached(dev))
	    return;

	if (!acpi_BatteryIsPresent(dev))
	    continue;

	/*
	 * Only query the battery if this is the first try or the specific
	 * type of info is still invalid.
	 */
	ACPI_SERIAL_BEGIN(cmbat);
	if (retry == 0 || !acpi_battery_bst_valid(&sc->bst)) {
	    timespecclear(&sc->bst_lastupdated);
	    acpi_cmbat_get_bst(dev);
	}
	if (retry == 0 || !acpi_battery_bif_valid(&sc->bif))
	    acpi_cmbat_get_bif(dev);

	valid = acpi_battery_bst_valid(&sc->bst) &&
	    acpi_battery_bif_valid(&sc->bif);
	ACPI_SERIAL_END(cmbat);

	if (valid)
	    break;
    }

    if (retry == ACPI_CMBAT_RETRY_MAX) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "battery initialization failed, giving up\n");
    } else {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
		    "battery initialization done, tried %d times\n", retry + 1);
    }
}
