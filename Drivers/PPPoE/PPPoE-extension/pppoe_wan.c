/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/* -----------------------------------------------------------------------------
*
*  History :
*
*  Aug 2000 - 	Christophe Allie - created.
*
*  Theory of operation :
*
*  this file implements the pppoe driver for the ppp family
*
*  it's the responsability of the driver to update the statistics
*  whenever that makes sense
*     ifnet.if_lastchange = a packet is present, a packet starts to be sent
*     ifnet.if_ibytes = nb of correct PPP bytes received (does not include escapes...)
*     ifnet.if_obytes = nb of correct PPP bytes sent (does not include escapes...)
*     ifnet.if_ipackets = nb of PPP packet received
*     ifnet.if_opackets = nb of PPP packet sent
*     ifnet.if_ierrors = nb on input packets in error
*     ifnet.if_oerrors = nb on ouptut packets in error
*
----------------------------------------------------------------------------- */


/* -----------------------------------------------------------------------------
Includes
----------------------------------------------------------------------------- */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/malloc.h>
#include <sys/syslog.h>
#include <sys/sockio.h>
#include <sys/kernel.h>
#include <net/if_types.h>
#include <net/dlil.h>
#include <kern/clock.h>



#include "../../../Family/ppp_domain.h"
#include "../../../Family/if_ppplink.h"
#include "../../../Family/ppp_defs.h"
#include "../../../Family/if_ppp.h"
#include "PPPoE.h"
#include "pppoe_rfc.h"
#include "pppoe_wan.h"


/* -----------------------------------------------------------------------------
Definitions
----------------------------------------------------------------------------- */

struct pppoe_wan {
    /* first, the ifnet structure... */
    struct ppp_link 	link;			/* ppp link structure */

    /* administrative info */
    TAILQ_ENTRY(pppoe_wan) next;
    void 		*rfc;			/* pppoe protocol structure */

    /* settings */
    
    /* output data */

    /* input data */

    /* log purpose */
};

/* -----------------------------------------------------------------------------
Forward declarations
----------------------------------------------------------------------------- */

static int	pppoe_wan_output(struct ppp_link *link, struct mbuf *m);
static int 	pppoe_wan_ioctl(struct ppp_link *link, u_int32_t cmd, void *data);
static int 	pppoe_wan_findfreeunit(u_short *freeunit);

/* -----------------------------------------------------------------------------
Globals
----------------------------------------------------------------------------- */

static TAILQ_HEAD(, pppoe_wan) 	pppoe_wan_head;


/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pppoe_wan_init()
{

    TAILQ_INIT(&pppoe_wan_head);
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pppoe_wan_dispose()
{

    // don't detach if links are in use
    if (TAILQ_FIRST(&pppoe_wan_head))
        return EBUSY;
        
    return 0;
}

/* -----------------------------------------------------------------------------
detach pppoe interface dlil layer
----------------------------------------------------------------------------- */
int pppoe_wan_attach(void *rfc, struct ppp_link **link)
{
    int 		ret;	
    struct pppoe_wan  	*wan;
    struct ppp_link  	*lk;
    u_short 		unit;

    // Note : we allocate/find number/insert in queue in that specific order
    // because of funnels and race condition issues

    MALLOC(wan, struct pppoe_wan *, sizeof(struct pppoe_wan), M_DEVBUF, M_WAITOK);
    if (!wan)
        return ENOMEM;

    if (pppoe_wan_findfreeunit(&unit)) {
        FREE(wan, M_DEVBUF);
        return ENOMEM;
    }

    bzero(wan, sizeof(struct pppoe_wan));

    TAILQ_INSERT_TAIL(&pppoe_wan_head, wan, next);
    lk = (struct ppp_link *) wan;
    
    // it's time now to register our brand new link
    lk->lk_name 	= PPPOE_NAME;
    lk->lk_mtu 		= PPPOE_MTU;
    lk->lk_mru 		= PPPOE_MTU;;
    lk->lk_type 	= PPP_TYPE_PPPoE;
    lk->lk_hdrlen 	= 14; // ethernet header len
    //ld->lk_if.link_lk_baudrate = tp->t_ospeed;
    lk->lk_ioctl 	= pppoe_wan_ioctl;
    lk->lk_output 	= pppoe_wan_output;
    lk->lk_unit 	= unit;
    lk->lk_support 	= PPP_LINK_DEL_AC;
    wan->rfc = rfc;

    ret = ppp_link_attach((struct ppp_link *)wan);
    if (ret) {
        log(LOG_INFO, "pppoe_wan_attach, error = %d, (ld = 0x%x)\n", ret, wan);
        TAILQ_REMOVE(&pppoe_wan_head, wan, next);
        FREE(wan, M_DEVBUF);
        return ret;
    }
    
    log(LOG_INFO, "pppoe_wan_attach, link index = %d, (ld = 0x%x)\n", lk->lk_index, lk);

    *link = lk;
    
    return 0;
}

/* -----------------------------------------------------------------------------
detach pppoe interface dlil layer
----------------------------------------------------------------------------- */
void pppoe_wan_detach(struct ppp_link *link)
{
    struct pppoe_wan  	*wan = (struct pppoe_wan *)link;

    ppp_link_detach(link);
    TAILQ_REMOVE(&pppoe_wan_head, wan, next);
    FREE(wan, M_DEVBUF);
}

/* -----------------------------------------------------------------------------
find a free unit in the interface list
----------------------------------------------------------------------------- */
int pppoe_wan_findfreeunit(u_short *freeunit)
{
    struct pppoe_wan  	*wan = TAILQ_FIRST(&pppoe_wan_head);
    u_short 		unit = 0;

    while (wan) {
    	if (wan->link.lk_unit == unit) {
            unit++;
            wan = TAILQ_FIRST(&pppoe_wan_head); // restart
        }
        else 
            wan = TAILQ_NEXT(wan, next); // continue
    }
    *freeunit = unit;
    return 0;
}

/* -----------------------------------------------------------------------------
called from pppoe_rfc when data are present
----------------------------------------------------------------------------- */
int pppoe_wan_input(struct ppp_link *link, struct mbuf *m)
{
    
    link->lk_ipackets++;
    link->lk_ibytes += m->m_pkthdr.len;
    //getmicrotime(&link->lk_last_recv);
    link->lk_last_recv = clock_get_system_value().tv_sec;

    ppp_link_input(link, m);	
    return 0;
}

/* -----------------------------------------------------------------------------
Process an ioctl request to the ppp interface
----------------------------------------------------------------------------- */
int pppoe_wan_ioctl(struct ppp_link *link, u_int32_t cmd, void *data)
{
    //struct pppoe_wan 	*wan = (struct pppoe_wan *)link;;
    int error = 0;
    
    //LOGDBG(ifp, (LOGVAL, "pppoe_wan_ioctl, cmd = 0x%x\n", cmd));

    switch (cmd) {
        default:
            error = ENOTSUP;
    }
    return error;
}

/* -----------------------------------------------------------------------------
This gets called at splnet from if_ppp.c at various times
when there is data ready to be sent
----------------------------------------------------------------------------- */
int pppoe_wan_output(struct ppp_link *link, struct mbuf *m)
{
    struct pppoe_wan 	*wan = (struct pppoe_wan *)link;

    if (pppoe_rfc_output(wan->rfc, m)) {
        link->lk_oerrors++;
        return ENOBUFS;
    }

    link->lk_opackets++;
    link->lk_obytes += m->m_pkthdr.len;
    //getmicrotime(link->lk_last_xmit);
    link->lk_last_xmit = clock_get_system_value().tv_sec;
    return 0;
}