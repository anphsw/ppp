/*
 * if_ppp.c - Point-to-Point Protocol (PPP) Asynchronous driver.
 *
 * Copyright (c) 1989 Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by Carnegie Mellon University.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Drew D. Perkins
 * Carnegie Mellon University
 * 4910 Forbes Ave.
 * Pittsburgh, PA 15213
 * (412) 268-8576
 * ddp@andrew.cmu.edu
 *
 * Based on:
 *	@(#)if_sl.c	7.6.1.2 (Berkeley) 2/15/89
 *
 * Copyright (c) 1987 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Serial Line interface
 *
 * Rick Adams
 * Center for Seismic Studies
 * 1300 N 17th Street, Suite 1450
 * Arlington, Virginia 22209
 * (703)276-7900
 * rick@seismo.ARPA
 * seismo!rick
 *
 * Pounded on heavily by Chris Torek (chris@mimsy.umd.edu, umcp-cs!chris).
 * Converted to 4.3BSD Beta by Chris Torek.
 * Other changes made at Berkeley, based in part on code by Kirk Smith.
 *
 * Converted to 4.3BSD+ 386BSD by Brad Parker (brad@cayman.com)
 * Added VJ tcp header compression; more unified ioctls
 *
 * Extensively modified by Paul Mackerras (paulus@cs.anu.edu.au).
 * Cleaned up a lot of the mbuf-related code to fix bugs that
 * caused system crashes and packet corruption.  Changed pppstart
 * so that it doesn't just give up with a collision if the whole
 * packet doesn't fit in the output ring buffer.
 *
 * Added priority queueing for interactive IP packets, following
 * the model of if_sl.c, plus hooks for bpf.
 * Paul Mackerras (paulus@cs.anu.edu.au).
 *
 * Ultrix port by Per Sundstrom <sundstrom@stkhlm.enet.dec.com>,
 * Robert Olsson <robert@robur.slu.se> and Paul Mackerras.
 */

/* $Id: if_ppp.c,v 1.2 1994/11/21 04:50:36 paulus Exp $ */
/* from if_sl.c,v 1.11 84/10/04 12:54:47 rick Exp */

#include "ppp.h"
#if NPPP > 0

#define VJC
#define PPP_COMPRESS

#include "../h/param.h"
#include "../h/user.h"
#include "../h/proc.h"
#include "../h/mbuf.h"
#include "../h/buf.h"
#include "../h/socket.h"
#include "../h/ioctl.h"
#include "../h/systm.h"

#include "../net/net/if.h"
#include "../net/net/netisr.h"
#include "../net/net/route.h"

#if INET
#include "../net/netinet/in.h"
#include "../net/netinet/in_systm.h"
#include "../net/netinet/in_var.h"
#include "../net/netinet/ip.h"
#endif

#ifdef VJC
#include "slcompress.h"
#endif

#include "ppp_defs.h"
#include "if_ppp.h"
#include "if_pppvar.h"

#ifdef PPP_COMPRESS
#define PACKETPTR	struct mbuf *
#include "ppp-comp.h"
#endif

void	pppattach __P((void));
int	pppioctl __P((struct ppp_softc *sc, int cmd, caddr_t data, int flag,
		      struct proc *));
int	pppoutput __P((struct ifnet *ifp, struct mbuf *m0,
		       struct sockaddr *dst));
int	pppsioctl __P((struct ifnet *ifp, int cmd, caddr_t data));
void	pppintr __P((void));

static void	ppp_outpkt __P((struct ppp_softc *));
static int	ppp_ccp __P((struct ppp_softc *, struct mbuf *m, int rcvd));
static void	ppp_ccp_closed __P((struct ppp_softc *));
static void	ppp_inproc __P((struct ppp_softc *, struct mbuf *));
static void	pppdumpm __P((struct mbuf *m0));

/*
 * Some useful mbuf macros not in mbuf.h.
 */
#define M_IS_CLUSTER(m) ((m)->m_off > MMAXOFF)

#define M_TRAILINGSPACE(m) \
	((M_IS_CLUSTER(m) ? (u_int)(m)->m_clptr + M_CLUSTERSZ : MSIZE) \
	 - ((m)->m_off + (m)->m_len))

#define M_OFFSTART(m)	\
	(M_IS_CLUSTER(m) ? (u_int)(m)->m_clptr : MMINOFF)

#define M_DATASIZE(m)	\
	(M_IS_CLUSTER(m) ? M_CLUSTERSZ : MLEN)

/*
 * The following disgusting hack gets around the problem that IP TOS
 * can't be set yet.  We want to put "interactive" traffic on a high
 * priority queue.  To decide if traffic is interactive, we check that
 * a) it is TCP and b) one of its ports is telnet, rlogin or ftp control.
 */
static u_short interactive_ports[8] = {
	0,	513,	0,	0,
	0,	21,	0,	23,
};
#define INTERACTIVE(p) (interactive_ports[(p) & 7] == (p))

#ifdef PPP_COMPRESS
/*
 * List of compressors we know about.
 */

extern struct compressor ppp_bsd_compress;

struct compressor *ppp_compressors[] = {
    &ppp_bsd_compress,
    NULL
};
#endif /* PPP_COMPRESS */

/*
 * Called from boot code to establish ppp interfaces.
 */
void
pppattach()
{
    register struct ppp_softc *sc;
    register int i = 0;

    for (sc = ppp_softc; i < NPPP; sc++) {
	sc->sc_if.if_name = "ppp";
	sc->sc_if.if_unit = i++;
	sc->sc_if.if_mtu = PPP_MTU;
	sc->sc_if.if_flags = IFF_POINTOPOINT;
	sc->sc_if.if_type = IFT_PPP;
	sc->sc_if.if_ioctl = pppsioctl;
	sc->sc_if.if_output = pppoutput;
	sc->sc_if.if_snd.ifq_maxlen = IFQ_MAXLEN;
	sc->sc_inq.ifq_maxlen = IFQ_MAXLEN;
	sc->sc_fastq.ifq_maxlen = IFQ_MAXLEN;
	sc->sc_rawq.ifq_maxlen = IFQ_MAXLEN;
	if_attach(&sc->sc_if);
#if NBPFILTER > 0
	bpfattach(&sc->sc_bpf, &sc->sc_if, DLT_PPP, PPP_HDRLEN);
#endif
    }
}

/*
 * Allocate a ppp interface unit and initialize it.
 */
struct ppp_softc *
pppalloc(pid)
    pid_t pid;
{
    int nppp, i;
    struct ppp_softc *sc;

    for (nppp = 0, sc = ppp_softc; nppp < NPPP; nppp++, sc++)
	if (sc->sc_xfer == pid) {
	    sc->sc_xfer = 0;
	    break;
	}
    if (nppp >= NPPP)
	for (nppp = 0, sc = ppp_softc; nppp < NPPP; nppp++, sc++)
	    if (sc->sc_devp == NULL)
		break;
    if (nppp >= NPPP)
	return NULL;

    sc->sc_flags = 0;
    sc->sc_mru = PPP_MRU;
#ifdef VJC
    sl_compress_init(&sc->sc_comp);
#endif
#ifdef PPP_COMPRESS
    sc->sc_xc_state = NULL;
    sc->sc_rc_state = NULL;
#endif /* PPP_COMPRESS */
    for (i = 0; i < NUM_NP; ++i)
	sc->sc_npmode[i] = NPMODE_ERROR;
    sc->sc_if.if_flags |= IFF_RUNNING;

    return sc;
}

/*
 * Deallocate a ppp unit.
 */
void
pppdealloc(sc)
    struct ppp_softc *sc;
{
    struct mbuf *m;

    if_down(&sc->sc_if);
    sc->sc_if.if_flags &= ~(IFF_UP|IFF_RUNNING);
    sc->sc_devp = NULL;
    sc->sc_xfer = 0;
    for (;;) {
	IF_DEQUEUE(&sc->sc_rawq, m);
	if (m == NULL)
	    break;
	m_freem(m);
    }
    for (;;) {
	IF_DEQUEUE(&sc->sc_inq, m);
	if (m == NULL)
	    break;
	m_freem(m);
    }
    for (;;) {
	IF_DEQUEUE(&sc->sc_fastq, m);
	if (m == NULL)
	    break;
	m_freem(m);
    }
    if (sc->sc_togo != NULL) {
	m_freem(sc->sc_togo);
	sc->sc_togo = NULL;
    }
#ifdef PPP_COMPRESS
    ppp_ccp_closed(sc);
    sc->sc_xc_state = NULL;
    sc->sc_rc_state = NULL;
#endif /* PPP_COMPRESS */
}

/*
 * Ioctl routine for generic ppp devices.
 */
int
pppioctl(sc, cmd, data, flag)
    struct ppp_softc *sc;
    caddr_t data;
    int cmd, flag;
{
    struct proc *p = u.u_procp;
    int s, error, flags, mru, nb, npx;
    struct ppp_option_data *odp;
    struct compressor **cp;
    struct npioctl *npi;
    u_char ccp_option[CCP_MAX_OPTION_LENGTH];

    switch (cmd) {
    case FIONREAD:
	*(int *)data = sc->sc_inq.ifq_len;
	break;

    case PPPIOCGUNIT:
	*(int *)data = sc->sc_if.if_unit;
	break;

    case PPPIOCGFLAGS:
	*(u_int *)data = sc->sc_flags;
	break;

    case PPPIOCSFLAGS:
	if (!suser())
	    return EPERM;
	flags = *(int *)data & SC_MASK;
	s = splnet();
	if (sc->sc_flags & SC_CCP_OPEN && !(flags & SC_CCP_OPEN))
	    ppp_ccp_closed(sc);
	splimp();
	sc->sc_flags = (sc->sc_flags & ~SC_MASK) | flags;
	splx(s);
	break;

    case PPPIOCSMRU:
	if (!suser())
	    return EPERM;
	mru = *(int *)data;
	if (mru >= PPP_MRU && mru <= PPP_MAXMRU)
	    sc->sc_mru = mru;
	break;

    case PPPIOCGMRU:
	*(int *)data = sc->sc_mru;
	break;

#ifdef VJC
    case PPPIOCSMAXCID:
	if (!suser())
	    return EPERM;
	s = splnet();
	sl_compress_setup(&sc->sc_comp, *(int *)data);
	splx(s);
	break;
#endif

    case PPPIOCXFERUNIT:
	if (!suser())
	    return EPERM;
	sc->sc_xfer = p->p_pid;
	break;

#ifdef PPP_COMPRESS
    case PPPIOCSCOMPRESS:
	if (!suser())
	    return EPERM;
	odp = (struct ppp_option_data *) data;
	nb = odp->length;
	if (nb > sizeof(ccp_option))
	    nb = sizeof(ccp_option);
	if (error = copyin(odp->ptr, ccp_option, nb))
	    return (error);
	if (ccp_option[1] < 2)	/* preliminary check on the length byte */
	    return (EINVAL);
	for (cp = ppp_compressors; *cp != NULL; ++cp)
	    if ((*cp)->compress_proto == ccp_option[0]) {
		/*
		 * Found a handler for the protocol - try to allocate
		 * a compressor or decompressor.
		 */
		error = 0;
		s = splnet();
		if (odp->transmit) {
		    if (sc->sc_xc_state != NULL)
			(*sc->sc_xcomp->comp_free)(sc->sc_xc_state);
		    sc->sc_xcomp = *cp;
		    sc->sc_xc_state = (*cp)->comp_alloc(ccp_option, nb);
		    if (sc->sc_xc_state == NULL) {
			if (sc->sc_flags & SC_DEBUG)
			    printf("ppp%d: comp_alloc failed\n",
			       sc->sc_if.if_unit);
			error = ENOBUFS;
		    }
		    splimp();
		    sc->sc_flags &= ~SC_COMP_RUN;
		} else {
		    if (sc->sc_rc_state != NULL)
			(*sc->sc_rcomp->decomp_free)(sc->sc_rc_state);
		    sc->sc_rcomp = *cp;
		    sc->sc_rc_state = (*cp)->decomp_alloc(ccp_option, nb);
		    if (sc->sc_rc_state == NULL) {
			if (sc->sc_flags & SC_DEBUG)
			    printf("ppp%d: decomp_alloc failed\n",
			       sc->sc_if.if_unit);
			error = ENOBUFS;
		    }
		    splimp();
		    sc->sc_flags &= ~SC_DECOMP_RUN;
		}
		splx(s);
		return (error);
	    }
	if (sc->sc_flags & SC_DEBUG)
	    printf("ppp%d: no compressor for [%x %x %x], %x\n",
		   sc->sc_if.if_unit, ccp_option[0], ccp_option[1],
		   ccp_option[2], nb);
	return (EINVAL);	/* no handler found */
#endif /* PPP_COMPRESS */

    case PPPIOCGNPMODE:
    case PPPIOCSNPMODE:
	npi = (struct npioctl *) data;
	switch (npi->protocol) {
	case PPP_IP:
	    npx = NP_IP;
	    break;
	default:
	    return EINVAL;
	}
	if (cmd == PPPIOCGNPMODE) {
	    npi->mode = sc->sc_npmode[npx];
	} else {
	    if (!suser())
		return EPERM;
	    if (npi->mode != sc->sc_npmode[npx]) {
		s = splimp();
		sc->sc_npmode[npx] = npi->mode;
		if (npi->mode != NPMODE_QUEUE)
		    (*sc->sc_start)(sc);
		splx(s);
	    }
	}
	break;

    default:
	return (-1);
    }
    return (0);
}

/*
 * Process an ioctl request to the ppp network interface.
 */
int
pppsioctl(ifp, cmd, data)
    register struct ifnet *ifp;
    int cmd;
    caddr_t data;
{
    struct proc *p = u.u_procp;
    register struct ppp_softc *sc = &ppp_softc[ifp->if_unit];
    register struct ifaddr *ifa = (struct ifaddr *)data;
    register struct ifreq *ifr = (struct ifreq *)data;
    struct ppp_stats *psp;
    struct ppp_comp_stats *pcp;
    int s = splimp(), error = 0;

    switch (cmd) {
    case SIOCSIFFLAGS:
	if ((ifp->if_flags & IFF_RUNNING) == 0)
	    ifp->if_flags &= ~IFF_UP;
	break;

    case SIOCSIFADDR:
	if (ifa->ifa_addr->sa_family != AF_INET)
	    error = EAFNOSUPPORT;
	break;

    case SIOCSIFDSTADDR:
	if (ifa->ifa_addr->sa_family != AF_INET)
	    error = EAFNOSUPPORT;
	break;

    case SIOCSIFMTU:
	if (!suser())
	    return EPERM;
	sc->sc_if.if_mtu = ifr->ifr_mtu;
	break;

    case SIOCGIFMTU:
	ifr->ifr_mtu = sc->sc_if.if_mtu;
	break;

    case SIOCGPPPSTATS:
	psp = &((struct ifpppstatsreq *) data)->stats;
	bzero(psp, sizeof(*psp));
	psp->p.ppp_ibytes = sc->sc_bytesrcvd;
	psp->p.ppp_ipackets = ifp->if_ipackets;
	psp->p.ppp_ierrors = ifp->if_ierrors;
	psp->p.ppp_obytes = sc->sc_bytessent;
	psp->p.ppp_opackets = ifp->if_opackets;
	psp->p.ppp_oerrors = ifp->if_oerrors;
#ifdef VJC
	psp->vj.vjs_packets = sc->sc_comp.sls_packets;
	psp->vj.vjs_compressed = sc->sc_comp.sls_compressed;
	psp->vj.vjs_searches = sc->sc_comp.sls_searches;
	psp->vj.vjs_misses = sc->sc_comp.sls_misses;
	psp->vj.vjs_uncompressedin = sc->sc_comp.sls_uncompressedin;
	psp->vj.vjs_compressedin = sc->sc_comp.sls_compressedin;
	psp->vj.vjs_errorin = sc->sc_comp.sls_errorin;
	psp->vj.vjs_tossed = sc->sc_comp.sls_tossed;
#endif /* VJC */
	break;

#ifdef PPP_COMPRESS
    case SIOCGPPPCSTATS:
	pcp = &((struct ifpppcstatsreq *) data)->stats;
	bzero(pcp, sizeof(*pcp));
	if (sc->sc_xc_state != NULL)
	    (*sc->sc_xcomp->comp_stat)(sc->sc_xc_state, &pcp->c);
	if (sc->sc_rc_state != NULL)
	    (*sc->sc_rcomp->decomp_stat)(sc->sc_rc_state, &pcp->d);
	break;
#endif /* PPP_COMPRESS */

    default:
	error = EINVAL;
    }
    splx(s);
    return (error);
}

/*
 * Queue a packet.  Start transmission if not active.
 * Packet is placed in Information field of PPP frame.
 */
int
pppoutput(ifp, m0, dst)
    struct ifnet *ifp;
    struct mbuf *m0;
    struct sockaddr *dst;
{
    register struct ppp_softc *sc = &ppp_softc[ifp->if_unit];
    struct ppp_header *ph;
    int protocol, address, control;
    u_char *cp;
    int s, error;
    struct ip *ip;
    struct ifqueue *ifq;
    enum NPmode mode;

    if (sc->sc_devp == NULL || (ifp->if_flags & IFF_RUNNING) == 0
	|| (ifp->if_flags & IFF_UP) == 0 && dst->sa_family != AF_UNSPEC) {
	error = ENETDOWN;	/* sort of */
	goto bad;
    }

    /*
     * Compute PPP header.
     */
    ifq = &ifp->if_snd;
    switch (dst->sa_family) {
#ifdef INET
    case AF_INET:
	address = PPP_ALLSTATIONS;
	control = PPP_UI;
	protocol = PPP_IP;
	mode = sc->sc_npmode[NP_IP];
	
	/*
	 * If this is a TCP packet to or from an "interactive" port,
	 * put the packet on the fastq instead.
	 */
	if ((ip = mtod(m0, struct ip *))->ip_p == IPPROTO_TCP) {
	    register int p = ntohl(((int *)ip)[ip->ip_hl]);
	    if (INTERACTIVE(p & 0xffff) || INTERACTIVE(p >> 16))
		ifq = &sc->sc_fastq;
	}
	break;
#endif
    case AF_UNSPEC:
	address = PPP_ADDRESS(dst->sa_data);
	control = PPP_CONTROL(dst->sa_data);
	protocol = PPP_PROTOCOL(dst->sa_data);
	mode = NPMODE_PASS;
	break;
    default:
	printf("ppp%d: af%d not supported\n", ifp->if_unit, dst->sa_family);
	error = EAFNOSUPPORT;
	goto bad;
    }

    /*
     * Drop this packet, or return an error, if necessary.
     */
    if (mode == NPMODE_ERROR) {
	error = ENETDOWN;
	goto bad;
    }
    if (mode == NPMODE_DROP) {
	error = 0;
	goto bad;
    }

    /*
     * Add PPP header.  If no space in first mbuf, allocate another.
     */
    if (M_IS_CLUSTER(m0) || m0->m_off < MMINOFF + PPP_HDRLEN) {
	struct mbuf *m;

	MGET(m, M_DONTWAIT, MT_DATA);
	if (m == NULL) {
	    m_freem(m0);
	    return (ENOBUFS);
	}
	m->m_len = 0;
	m->m_next = m0;
	m0 = m;
    } else
	m0->m_off -= PPP_HDRLEN;

    cp = mtod(m0, u_char *);
    *cp++ = address;
    *cp++ = control;
    *cp++ = protocol >> 8;
    *cp++ = protocol & 0xff;
    m0->m_len += PPP_HDRLEN;

    if (sc->sc_flags & SC_LOG_OUTPKT) {
	printf("ppp%d output: ", ifp->if_unit);
	pppdumpm(m0);
    }

#if NBPFILTER > 0
    /*
     * See if bpf wants to look at the packet.
     */
    if (sc->sc_bpf)
	bpf_mtap(sc->sc_bpf, m0);
#endif

    /*
     * Put the packet on the appropriate queue.
     */
    s = splimp();		/* splnet should be OK now */
    if (IF_QFULL(ifq)) {
	IF_DROP(ifq);
	splx(s);
	sc->sc_if.if_oerrors++;
	error = ENOBUFS;
	goto bad;
    }
    IF_ENQUEUE(ifq, m0);

    /*
     * Tell the device to send it out.
     */
    if (mode == NPMODE_PASS)
	(*sc->sc_start)(sc);

    splx(s);
    return (0);

bad:
    m_freem(m0);
    return (error);
}

/*
 * Get a packet to send.  This procedure is intended to be called
 * at spltty()/splimp(), so it takes little time.  If there isn't
 * a packet waiting to go out, it schedules a software interrupt
 * to prepare a new packet; the device start routine gets called
 * again when a packet is ready.
 */
struct mbuf *
ppp_dequeue(sc)
    struct ppp_softc *sc;
{
    struct mbuf *m;

    m = sc->sc_togo;
    if (m) {
	/*
	 * Had a packet waiting - send it.
	 */
	sc->sc_togo = NULL;
	sc->sc_flags |= SC_TBUSY;
	return m;
    }
    /*
     * Remember we wanted a packet and schedule a software interrupt.
     */
    sc->sc_flags &= ~SC_TBUSY;
    schednetisr(NETISR_PPP);
    return NULL;
}

/*
 * Software interrupt routine, called at splnet().
 */
void
pppintr()
{
    struct ppp_softc *sc;
    int i, s;
    struct mbuf *m;

    sc = ppp_softc;
    for (i = 0; i < NPPP; ++i, ++sc) {
	if (!(sc->sc_flags & SC_TBUSY) && sc->sc_togo == NULL
	    && (sc->sc_if.if_snd.ifq_head || sc->sc_fastq.ifq_head))
	    ppp_outpkt(sc);
	for (;;) {
	    IF_DEQUEUE(&sc->sc_rawq, m);
	    if (m == NULL)
		break;
	    ppp_inproc(sc, m);
	}
    }
}

/*
 * Grab another packet off a queue and apply VJ compression,
 * packet compression, address/control and/or protocol compression
 * if enabled.  Should be called at splnet.
 */
static void
ppp_outpkt(sc)
    struct ppp_softc *sc;
{
    int s;
    struct mbuf *m, *mp, **mpp;
    u_char *cp;
    int address, control, protocol;
    struct ifqueue *ifq;
    enum NPmode mode;

    /*
     * Scan through the send queues looking for a packet
     * which can be sent: first the fast queue, then the normal queue.
     */
    ifq = &sc->sc_fastq;
    for (;;) {
	mpp = &ifq->ifq_head;
	mp = NULL;
	while ((m = *mpp) != NULL) {
	    switch (PPP_PROTOCOL(mtod(m, u_char *))) {
	    case PPP_IP:
		mode = sc->sc_npmode[NP_IP];
		break;
	    default:
		mode = NPMODE_PASS;
	    }
	    if (mode == NPMODE_PASS)
		break;
	    switch (mode) {
	    case NPMODE_DROP:
	    case NPMODE_ERROR:
		*mpp = m->m_nextpkt;
		--ifq->ifq_len;
		m_freem(m);
		break;
	    case NPMODE_QUEUE:
		mpp = &m->m_nextpkt;
		mp = m;
		break;
	    }
	}
	if (m != NULL)
	    break;

	if (ifq == &sc->sc_if.if_snd)
	    break;
	/* Finished the fast queue; do the normal queue. */
	ifq = &sc->sc_if.if_snd;
    }

    if (m == NULL)
	return;

    if ((*mpp = m->m_nextpkt) == NULL)
	ifq->ifq_tail = mp;
    m->m_nextpkt = NULL;
    --ifq->ifq_len;

    /*
     * Extract the ppp header of the new packet.
     * The ppp header will be in one mbuf.
     */
    cp = mtod(m, u_char *);
    address = PPP_ADDRESS(cp);
    control = PPP_CONTROL(cp);
    protocol = PPP_PROTOCOL(cp);

    switch (protocol) {
#ifdef VJC
    case PPP_IP:
	/*
	 * If the packet is a TCP/IP packet, see if we can compress it.
	 */
	if (sc->sc_flags & SC_COMP_TCP) {
	    struct ip *ip;
	    int type;

	    mp = m;
	    ip = (struct ip *) (cp + PPP_HDRLEN);
	    if (mp->m_len <= PPP_HDRLEN) {
		mp = mp->m_next;
		if (mp == NULL)
		    break;
		ip = mtod(mp, struct ip *);
	    }
	    /* this code assumes the IP/TCP header is in one non-shared mbuf */
	    if (ip->ip_p == IPPROTO_TCP) {
		type = sl_compress_tcp(mp, ip, &sc->sc_comp,
				       !(sc->sc_flags & SC_NO_TCP_CCID));
		switch (type) {
		case TYPE_UNCOMPRESSED_TCP:
		    protocol = PPP_VJC_UNCOMP;
		    break;
		case TYPE_COMPRESSED_TCP:
		    protocol = PPP_VJC_COMP;
		    cp = mtod(m, u_char *);
		    cp[0] = address;	/* header has moved */
		    cp[1] = control;
		    cp[2] = 0;
		    break;
		}
		cp[3] = protocol;	/* update protocol in PPP header */
	    }
	}
	break;
#endif	/* VJC */

#ifdef PPP_COMPRESS
    case PPP_CCP:
	ppp_ccp(sc, m, 0);
	break;
#endif	/* PPP_COMPRESS */
    }

#ifdef PPP_COMPRESS
    if (protocol != PPP_LCP && protocol != PPP_CCP
	&& sc->sc_xc_state && (sc->sc_flags & SC_COMP_RUN)) {
	struct mbuf *mcomp;
	int slen, clen;

	slen = 0;
	for (mp = m; mp != NULL; mp = mp->m_next)
	    slen += mp->m_len;
	clen = (*sc->sc_xcomp->compress)
	    (sc->sc_xc_state, &mcomp, m, slen,
	     (sc->sc_flags & SC_CCP_UP? sc->sc_if.if_mtu: 0));
	if (mcomp != NULL) {
	    m_freem(m);
	    m = mcomp;
	    cp = mtod(m, u_char *);
	    protocol = cp[3];
	}
    }
#endif	/* PPP_COMPRESS */

    /*
     * Compress the address/control and protocol, if possible.
     */
    if (sc->sc_flags & SC_COMP_AC && address == PPP_ALLSTATIONS &&
	control == PPP_UI && protocol != PPP_ALLSTATIONS &&
	protocol != PPP_LCP) {
	/* can compress address/control */
	m->m_off += 2;
	m->m_len -= 2;
    }
    if (sc->sc_flags & SC_COMP_PROT && protocol < 0xFF) {
	/* can compress protocol */
	if (mtod(m, u_char *) == cp) {
	    cp[2] = cp[1];	/* move address/control up */
	    cp[1] = cp[0];
	}
	++m->m_off;
	--m->m_len;
    }

    s = splimp();
    sc->sc_togo = m;
    (*sc->sc_start)(sc);
    splx(s);
}

#ifdef PPP_COMPRESS
/*
 * Handle a CCP packet.  `rcvd' is 1 if the packet was received,
 * 0 if it is about to be transmitted.
 */
static int
ppp_ccp(sc, m, rcvd)
    struct ppp_softc *sc;
    struct mbuf *m;
    int rcvd;
{
    u_char *dp, *ep;
    struct mbuf *mp;
    int slen, s;
    struct bsd_db *db;

    /*
     * Get a pointer to the data after the PPP header.
     */
    if (m->m_len <= PPP_HDRLEN) {
	mp = m->m_next;
	if (mp == NULL)
	    return;
	dp = (mp != NULL)? mtod(mp, u_char *): NULL;
    } else {
	mp = m;
	dp = mtod(mp, u_char *) + PPP_HDRLEN;
    }

    ep = mtod(mp, u_char *) + mp->m_len;
    if (dp + CCP_HDRLEN > ep)
	return;
    slen = CCP_LENGTH(dp);
    if (dp + slen > ep) {
	if (sc->sc_flags & SC_DEBUG)
	    printf("if_ppp/ccp: not enough data in mbuf (%x+%x > %x+%x)\n",
		   dp, slen, mtod(mp, u_char *), mp->m_len);
	return;
    }

    switch (CCP_CODE(dp)) {
    case CCP_CONFREQ:
    case CCP_TERMREQ:
    case CCP_TERMACK:
	/* CCP must be going down - disable compression */
	if (sc->sc_flags & SC_CCP_UP) {
	    s = splimp();
	    sc->sc_flags &= ~(SC_CCP_UP | SC_COMP_RUN | SC_DECOMP_RUN);
	    splx(s);
	}
	break;

    case CCP_CONFACK:
	if (sc->sc_flags & SC_CCP_OPEN && !(sc->sc_flags & SC_CCP_UP)
	    && slen >= CCP_HDRLEN + CCP_OPT_MINLEN
	    && slen >= CCP_OPT_LENGTH(dp + CCP_HDRLEN) + CCP_HDRLEN) {
	    if (!rcvd) {
		/* we're agreeing to send compressed packets. */
		if (sc->sc_xc_state != NULL
		    && (*sc->sc_xcomp->comp_init)
			(sc->sc_xc_state, dp + CCP_HDRLEN, slen - CCP_HDRLEN,
			 sc->sc_if.if_unit, sc->sc_flags & SC_DEBUG)) {
		    s = splimp();
		    sc->sc_flags |= SC_COMP_RUN;
		    splx(s);
		}
	    } else {
		/* peer is agreeing to send compressed packets. */
		if (sc->sc_rc_state != NULL
		    && (*sc->sc_rcomp->decomp_init)
			(sc->sc_rc_state, dp + CCP_HDRLEN, slen - CCP_HDRLEN,
			 sc->sc_if.if_unit, sc->sc_mru,
			 sc->sc_flags & SC_DEBUG)) {
		    s = splimp();
		    sc->sc_flags |= SC_DECOMP_RUN;
		    sc->sc_flags &= ~(SC_DC_ERROR | SC_DC_FERROR);
		    splx(s);
		}
	    }
	}
	break;

    case CCP_RESETACK:
	if (sc->sc_flags & SC_CCP_UP) {
	    if (!rcvd) {
		if (sc->sc_xc_state && (sc->sc_flags & SC_COMP_RUN))
		    (*sc->sc_xcomp->comp_reset)(sc->sc_xc_state);
	    } else {
		if (sc->sc_rc_state && (sc->sc_flags & SC_DECOMP_RUN)) {
		    (*sc->sc_rcomp->decomp_reset)(sc->sc_rc_state);
		    s = splimp();
		    sc->sc_flags &= ~SC_DC_ERROR;
		    splx(s);
		}
	    }
	}
	break;
    }
}

/*
 * CCP is down; free (de)compressor state if necessary.
 */
static void
ppp_ccp_closed(sc)
    struct ppp_softc *sc;
{
    if (sc->sc_xc_state) {
	(*sc->sc_xcomp->comp_free)(sc->sc_xc_state);
	sc->sc_xc_state = NULL;
    }
    if (sc->sc_rc_state) {
	(*sc->sc_rcomp->decomp_free)(sc->sc_rc_state);
	sc->sc_rc_state = NULL;
    }
}
#endif /* PPP_COMPRESS */

/*
 * PPP packet input routine.
 * The caller has checked and removed the FCS and has inserted
 * the address/control bytes and the protocol high byte if they
 * were omitted.  Should be called at splimp/spltty.
 */
#define M_ERRMARK	0x4000	/* steal a bit in mbuf m_flags */

void
ppppktin(sc, m, lost)
    struct ppp_softc *sc;
    struct mbuf *m;
    int lost;
{
    if (lost)
	m->m_flags |= M_ERRMARK;
    IF_ENQUEUE(&sc->sc_rawq, m);
    schednetisr(NETISR_PPP);
}

/*
 * Process a received PPP packet, doing decompression as necessary.
 */
#define COMPTYPE(proto)	((proto) == PPP_VJC_COMP? TYPE_COMPRESSED_TCP: \
			 TYPE_UNCOMPRESSED_TCP)

static void
ppp_inproc(sc, m)
    struct ppp_softc *sc;
    struct mbuf *m;
{
    struct ifqueue *inq, *lock;
    int s, ilen, xlen, proto, rv;
    u_char *cp, adrs, ctrl;
    struct mbuf *mp, *dmp, *pc;
    u_char *iphdr;
    u_int hlen;

    sc->sc_if.if_ipackets++;

    if (sc->sc_flags & SC_LOG_INPKT) {
	printf("ppp%d: got %d bytes\n", sc->sc_if.if_unit, ilen);
	pppdumpm(m);
    }

    cp = mtod(m, u_char *);
    adrs = PPP_ADDRESS(cp);
    ctrl = PPP_CONTROL(cp);
    proto = PPP_PROTOCOL(cp);

    if (m->m_flags & M_ERRMARK) {
	m->m_flags &= ~M_ERRMARK;
	s = splimp();
	sc->sc_flags |= SC_VJ_RESET;
	splx(s);
    }

#ifdef PPP_COMPRESS
    /*
     * Decompress this packet if necessary, update the receiver's
     * dictionary, or take appropriate action on a CCP packet.
     */
    if (proto == PPP_COMP && sc->sc_rc_state && (sc->sc_flags & SC_DECOMP_RUN)
	&& !(sc->sc_flags & SC_DC_ERROR) && !(sc->sc_flags & SC_DC_FERROR)) {
	/* decompress this packet */
	rv = (*sc->sc_rcomp->decompress)(sc->sc_rc_state, m, &dmp);
	if (dmp != NULL) {
	    m_freem(m);
	    m = dmp;
	    cp = mtod(m, u_char *);
	    proto = PPP_PROTOCOL(cp);

	} else {
	    /* pass the compressed packet up to pppd, which may take
	       CCP down or issue a Reset-Req. */
	    if (sc->sc_flags & SC_DEBUG)
		printf("ppp%d: decompress failed %d\n", sc->sc_if.if_unit, rv);
	    s = splimp();
	    sc->sc_flags |= SC_VJ_RESET;
	    switch (rv) {
	    case DECOMP_OK:
		/* no error, but no decompressed packet produced */
		splx(s);
		m_freem(m);
		return;
	    case DECOMP_ERROR:
		sc->sc_flags |= SC_DC_ERROR;
		break;
	    case DECOMP_FATALERROR:
		sc->sc_flags |= SC_DC_FERROR;
		break;
	    }
	    splx(s);
	}

    } else {
	if (sc->sc_rc_state && (sc->sc_flags & SC_DECOMP_RUN)) {
	    (*sc->sc_rcomp->incomp)(sc->sc_rc_state, m);
	}
	if (proto == PPP_CCP) {
	    ppp_ccp(sc, m, 1);
	}
    }
#endif

    ilen = 0;
    for (mp = m; mp != NULL; mp = mp->m_next)
	ilen += mp->m_len;

#ifdef VJC
    if (sc->sc_flags & SC_VJ_RESET) {
	/*
	 * If we've missed a packet, we must toss subsequent compressed
	 * packets which don't have an explicit connection ID.
	 */
	sl_uncompress_tcp(NULL, 0, TYPE_ERROR, &sc->sc_comp);
	s = splimp();
	sc->sc_flags &= ~SC_VJ_RESET;
	splx(s);
    }

    /*
     * See if we have a VJ-compressed packet to uncompress.
     */
    if (proto == PPP_VJC_COMP) {
	if (sc->sc_flags & SC_REJ_COMP_TCP)
	    goto bad;

	xlen = sl_uncompress_tcp_core(cp + PPP_HDRLEN, m->m_len - PPP_HDRLEN,
				      ilen - PPP_HDRLEN, TYPE_COMPRESSED_TCP,
				      &sc->sc_comp, &iphdr, &hlen);

	if (xlen <= 0) {
	    if (sc->sc_flags & SC_DEBUG)
		printf("ppp%d: VJ uncompress failed on type comp\n",
			sc->sc_if.if_unit);
	    goto bad;
	}

	/* Copy the PPP and IP headers into a new mbuf. */
	MGET(mp, M_DONTWAIT, MT_DATA);
	if (mp == NULL)
	    goto bad;
	mp->m_len = 0;
	mp->m_next = NULL;
	if (hlen + PPP_HDRLEN > MHLEN) {
	    MCLGET(mp, pc);
	    if (M_TRAILINGSPACE(mp) < hlen + PPP_HDRLEN) {
		m_freem(mp);
		goto bad;	/* lose if big headers and no clusters */
	    }
	}
	cp = mtod(mp, u_char *);
	cp[0] = adrs;
	cp[1] = ctrl;
	cp[2] = 0;
	cp[3] = PPP_IP;
	proto = PPP_IP;
	bcopy(iphdr, cp + PPP_HDRLEN, hlen);
	mp->m_len = hlen + PPP_HDRLEN;

	/*
	 * Trim the PPP and VJ headers off the old mbuf
	 * and stick the new and old mbufs together.
	 */
	m->m_off += PPP_HDRLEN + xlen;
	m->m_len -= PPP_HDRLEN + xlen;
	if (m->m_len <= M_TRAILINGSPACE(mp)) {
	    bcopy(mtod(m, u_char *), mtod(mp, u_char *) + mp->m_len, m->m_len);
	    mp->m_len += m->m_len;
	    MFREE(m, mp->m_next);
	} else
	    mp->m_next = m;
	m = mp;
	ilen += hlen - xlen;

    } else if (proto == PPP_VJC_UNCOMP) {
	if (sc->sc_flags & SC_REJ_COMP_TCP)
	    goto bad;

	xlen = sl_uncompress_tcp_core(cp + PPP_HDRLEN, m->m_len - PPP_HDRLEN,
				      ilen - PPP_HDRLEN, TYPE_UNCOMPRESSED_TCP,
				      &sc->sc_comp, &iphdr, &hlen);

	if (xlen < 0) {
	    if (sc->sc_flags & SC_DEBUG)
		printf("ppp%d: VJ uncompress failed on type uncomp\n",
			sc->sc_if.if_unit);
	    goto bad;
	}

	proto = PPP_IP;
	cp[3] = PPP_IP;
    }
#endif /* VJC */

    /*
     * If the packet will fit in an ordinary mbuf, don't waste a
     * whole cluster on it.
     */
    if (ilen <= MLEN && M_IS_CLUSTER(m)) {
	MGET(mp, M_DONTWAIT, MT_DATA);
	if (mp != NULL) {
	    m_copydata(m, 0, ilen, mtod(mp, caddr_t));
	    m_freem(m);
	    m = mp;
	    m->m_len = ilen;
	}
    }

#if NBPFILTER > 0
    /* See if bpf wants to look at the packet. */
    if (sc->sc_bpf)
	bpf_mtap(sc->sc_bpf, m);
#endif

    switch (proto) {
#ifdef INET
    case PPP_IP:
	/*
	 * IP packet - take off the ppp header and pass it up to IP.
	 */
	if ((sc->sc_if.if_flags & IFF_UP) == 0
	    || sc->sc_npmode[NP_IP] != NPMODE_PASS) {
	    /* interface is down - drop the packet. */
	    m_freem(m);
	    return;
	}
	m->m_off += PPP_HDRLEN;
	m->m_len -= PPP_HDRLEN;
	schednetisr(NETISR_IP);
	inq = &ipintrq;
	break;
#endif

    default:
	/*
	 * Some other protocol - place on input queue for read().
	 */
	inq = &sc->sc_inq;
	rv = 1;
	break;
    }

    /*
     * Put the packet on the appropriate input queue.
     */
    s = splimp();
    lock = inq;
    smp_lock(&lock->lk_ifqueue, LK_RETRY);
    if (IF_QFULL(inq)) {
	IF_DROP(inq);
	/* XXX should we unlock here? */
	splx(s);
	if (sc->sc_flags & SC_DEBUG)
	    printf("ppp%d: input queue full\n", sc->sc_if.if_unit);
	goto bad;
    }
    IF_ENQUEUEIF(inq, m, &sc->sc_if);
    smp_unlock(&lock->lk_ifqueue);
    splx(s);

    if (rv)
	(*sc->sc_ctlp)(sc);

    return;

 bad:
    m_freem(m);
    sc->sc_if.if_ierrors++;
}

#define MAX_DUMP_BYTES	128

static void
pppdumpm(m0)
    struct mbuf *m0;
{
    char buf[3*MAX_DUMP_BYTES+4];
    char *bp = buf;
    struct mbuf *m;
    static char digits[] = "0123456789abcdef";

    for (m = m0; m; m = m->m_next) {
	int l = m->m_len;
	u_char *rptr = mtod(m, u_char *);

	while (l--) {
	    if (bp > buf + sizeof(buf) - 4)
		goto done;
	    *bp++ = digits[*rptr >> 4]; /* convert byte to ascii hex */
	    *bp++ = digits[*rptr++ & 0xf];
	}

	if (m->m_next) {
	    if (bp > buf + sizeof(buf) - 3)
		goto done;
	    *bp++ = '|';
	} else
	    *bp++ = ' ';
    }
done:
    if (m)
	*bp++ = '>';
    *bp = 0;
    printf("%s\n", buf);
}

#endif	/* NPPP > 0 */