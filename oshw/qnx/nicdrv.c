/*
 * Simple Open EtherCAT Master Library 
 *
 * File    : nicdrv.c
 * Version : 1.3.0
 * Date    : 24-02-2013
 * Copyright (C) 2014 OPAL-RT Technologies, Inc.
 * Copyright (C) 2005-2013 Speciaal Machinefabriek Ketels v.o.f.
 * Copyright (C) 2005-2013 Arthur Ketels
 * Copyright (C) 2008-2009 TU/e Technische Universiteit Eindhoven 
 *
 * SOEM is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * SOEM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * As a special exception, if other files instantiate templates or use macros
 * or inline functions from this file, or you compile this file and link it
 * with other works to produce a work based on this file, this file does not
 * by itself cause the resulting work to be covered by the GNU General Public
 * License. However the source code for this file must still be made available
 * in accordance with section (3) of the GNU General Public License.
 *
 * This exception does not invalidate any other reasons why a work based on
 * this file might be covered by the GNU General Public License.
 *
 * The EtherCAT Technology, the trade name and logo “EtherCAT” are the intellectual
 * property of, and protected by Beckhoff Automation GmbH. You can use SOEM for
 * the sole purpose of creating, using and/or selling or otherwise distributing
 * an EtherCAT network master provided that an EtherCAT Master License is obtained
 * from Beckhoff Automation GmbH.
 *
 * In case you did not receive a copy of the EtherCAT Master License along with
 * SOEM write to Beckhoff Automation GmbH, Eiserstraße 5, D-33415 Verl, Germany
 * (www.beckhoff.com).
 */

/** \file
 * \brief
 * EtherCAT RAW socket driver.
 *
 * Low level interface functions to send and receive EtherCAT packets.
 * EtherCAT has the property that packets are only send by the master,
 * and the send packets allways return in the receive buffer.
 * There can be multiple packets "on the wire" before they return.
 * To combine the received packets with the original send packets a buffer
 * system is installed. The identifier is put in the index item of the
 * EtherCAT header. The index is stored and compared when a frame is recieved.
 * If there is a match the packet can be combined with the transmit packet
 * and returned to the higher level function.
 *
 * The socket layer can exhibit a reversal in the packet order (rare).
 * If the Tx order is A-B-C the return order could be A-C-B. The indexed buffer
 * will reorder the packets automatically.
 *
 * The "redundant" option will configure two sockets and two NIC interfaces.
 * Slaves are connected to both interfaces, one on the IN port and one on the
 * OUT port. Packets are send via both interfaces. Any one of the connections
 * (also an interconnect) can be removed and the slaves are still serviced with
 * packets. The software layer will detect the possible failure modes and
 * compensate. If needed the packets from interface A are resend through interface B.
 * This layer if fully transparent for the higher layers.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h> 
#include <sys/socket.h> 
#include <unistd.h>
#include <sys/time.h> 
#include <time.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
//#include <netpacket/packet.h> //@todo remove
#include <pthread.h>
#include <errno.h>

#include <net/bpf.h>
#include <net/ethertypes.h>
#include <net/if_ether.h>

#include "oshw.h"
#include "osal.h"

/** @todo remove */
#include <stdlib.h>
#include "utils.h"


/** Redundancy modes */
enum
{
   /** No redundancy, single NIC mode */
   ECT_RED_NONE,
   /** Double redundant NIC connecetion */
   ECT_RED_DOUBLE
};

/** BPF settings */
struct bpf_settings settings = {
    .header_complete = 1,
    .immediate = 1,
    .promiscuous = 1,
    .buffer_len = -1,
    .timeout = {
        .tv_sec = 0,
        .tv_usec = 1,
    },
};

/**
 * BPF filter algorithm => drop everything but EtherCAT packages
 * (ether.type == 0x88A4)
 */
#if defined(BIOCSDIRECTION)
    struct bpf_insn insns[] = {
        /* store Ethertype from offset 12 */
        BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 12),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, ETH_P_ECAT, 0, 1),
        BPF_STMT(BPF_RET+BPF_K, (u_int)-1),
        BPF_STMT(BPF_RET+BPF_K, 0),
    };
#else
    /** capture direction not supported by this version of BPF
     * we fall back to filtering the source address
     * @warning if EtherCAT packet are returned without source mac
     *  modification, it will fail (no msg will be captured)
     */
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 12),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, ETH_P_ECAT, 0, 4),
        /* let's look at the source MAC address */
        BPF_STMT(BPF_LD+BPF_H+BPF_ABS, 6),
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, PRIMAC0, 2, 0),  /* primary iface addr */
        BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SECMAC0, 1, 0),  /* secondary iface addr */
        BPF_STMT(BPF_RET+BPF_K, (u_int)-1),
        BPF_STMT(BPF_RET+BPF_K, 0),
    };
#endif

/**
 * BPF program
 */
struct bpf_program filter = {
    .bf_insns = insns,
    .bf_len = (sizeof(insns) / sizeof(struct bpf_insn))
};


/** Open a Berkeley Packet Filter device for raw I/O
 * The prefered low-level interface for capturing/writing network
 * traffic on QNX or BSD is Berkeley Packet Filter (BPF)
 *
 * @return handler to BPF device on success; < 0 otherwise
 */
int open_bfp_device()
{
    char bpfname[16] = {"/dev/bpf\0"};
    int bpf=-1, i=0;

    /* opening autocloning BFP device */
    bpf = open(bpfname, O_RDWR);

    /* no autocloning BPF found: fall back to iteration */
    if (bpf < 0){
        for(i=0; i<128; i++){
            snprintf(bpfname, sizeof(bpfname), "/dev/bpf%d", i);
            bpf = open(bpfname, O_RDWR);

            if(bpf != -1)
                break;
        }
        if(bpf < 0){
            FATAL("Error: could not open any /dev/bpf device.");
        }
    }
    D("Opened BPF device \"%s\" on %d", bpfname, bpf);

    return bpf;
}

int setup_bpf_device(int bpf, const char *ifname)
{
    /* set internal buffer length */
    int buflen = EC_BUFSIZE;
    if( ioctl(bpf, BIOCSBLEN, &buflen) == -1){
        FATAL("Could set buffer length to %d: error %d (%s)", buflen, errno, strerror(errno));
    }

    struct ifreq iface;
    strncpy(iface.ifr_name, ifname, sizeof(ifname));
    if( ioctl(bpf, BIOCSETIF, &iface) > 0){
        FATAL("Could not bind %s to BPF", ifname);
    }
    D("Associated with \"%s\"", ifname);

    /* set immediate: returns on packet arrived instead of when buffer full */
    if( ioctl(bpf, BIOCIMMEDIATE, &settings.immediate) < 0){
        FATAL("Could set IO immediate");
    }

    /* disable ethernet header completion by BPF */
    if( ioctl(bpf, BIOCSHDRCMPLT , &settings.header_complete) < 0){
        FATAL("Could get disable HDRCMPLT");
    }

    if( ioctl(bpf, BIOCSRTIMEOUT , &settings.timeout) < 0){
        FATAL("Could set timeout");
    }
#if defined(BIOCSDIRECTION)
    int direction = PBF_D_OUT;
    if( ioctl(bpf, BIOCSDIRECTION , &direction) < 0){
        FATAL("Could set direction");
    }
#endif
    /* set promiscuous mode */
//    if( ioctl(bpf, BIOCPROMISC, &settings.promiscuous) < 0){
//        FATAL("Could get disable BIOCPROMISC");
//    }

//    struct bpf_version version;
//    if( ioctl(bpf, BIOCVERSION, &version) == -1){
//        FATAL("Could BPF version");
//    }
//    D("BPF version %d.%d", version.bv_major, version.bv_minor);

    /* retrieve internal buffer length */
    if( ioctl(bpf, BIOCGBLEN, &settings.buffer_len) == -1){
        FATAL("Could get buffer length");
    }
    D("Buffer length is %d (%dko).", settings.buffer_len, settings.buffer_len/1024);

    /* set BPF filter */
    if( ioctl(bpf, BIOCSETF, &filter) < 0) {
        FATAL("Could not set BPF filter (type 0x%04x): error %d (%s)", ETH_P_ECAT, errno, strerror(errno));
    }
    D("BPF filter for type 0x%04x set.", ETH_P_ECAT);

    return 0;
}

/** Primary source MAC address used for EtherCAT.
 * This address is not the MAC address used from the NIC.
 * EtherCAT does not care about MAC addressing, but it is used here to
 * differentiate the route the packet traverses through the EtherCAT
 * segment. This is needed to find out the packet flow in redundant
 * configurations. */
const uint16 priMAC[3] = { PRIMAC0, PRIMAC1, PRIMAC2 };
/** Secondary source MAC address used for EtherCAT. */
const uint16 secMAC[3] = { SECMAC0, SECMAC1, SECMAC2 };

/** second MAC word is used for identification */
#define RX_PRIM priMAC[1]
/** second MAC word is used for identification */
#define RX_SEC secMAC[1]

/** Basic setup to connect NIC to socket.
 * @param[in] port        = port context struct
 * @param[in] ifname      = Name of NIC device, f.e. "eth0"
 * @param[in] secondary   = if >0 then use secondary stack instead of primary
 * @return >0 if succeeded
 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary) 
{
   int i;
   int r, rval, ifindex;
   int *pbpf;

   rval = 0;
   if (secondary)
   {
      /* secondary port stuct available? */
      if (port->redport)
      {
         /* when using secondary socket it is automatically a redundant setup */
         pbpf = &(port->redport->sockhandle);
         *pbpf = -1;
         port->redstate                   = ECT_RED_DOUBLE;
         port->redport->stack.sock        = &(port->redport->sockhandle);
         port->redport->stack.txbuf       = &(port->txbuf);
         port->redport->stack.txbuflength = &(port->txbuflength);
         port->redport->stack.tempbuf     = &(port->redport->tempinbuf);
         port->redport->stack.rxbuf       = &(port->redport->rxbuf);
         port->redport->stack.rxbufstat   = &(port->redport->rxbufstat);
         port->redport->stack.rxsa        = &(port->redport->rxsa);
      }
      else
      {
         /* fail */
         return 0;
      }
   }
   else
   {
      pthread_mutex_init(&(port->getindex_mutex), NULL);
      pthread_mutex_init(&(port->tx_mutex)      , NULL);
      pthread_mutex_init(&(port->rx_mutex)      , NULL);
      port->sockhandle        = -1;
      port->lastidx           = 0;
      port->redstate          = ECT_RED_NONE;
      port->stack.sock        = &(port->sockhandle);
      port->stack.txbuf       = &(port->txbuf);
      port->stack.txbuflength = &(port->txbuflength);
      port->stack.tempbuf     = &(port->tempinbuf);
      port->stack.rxbuf       = &(port->rxbuf);
      port->stack.rxbufstat   = &(port->rxbufstat);
      port->stack.rxsa        = &(port->rxsa);
      pbpf = &(port->sockhandle);
   }
   /* we use BPF capture device, with filter on ethertype ETH_P_ECAT */
   *pbpf = open_bfp_device();

   /* bind with ifname, set flags, set filter to ETH_P_ECAT */
   r = setup_bpf_device(*pbpf, ifname);
   
   /* setup ethernet headers in tx buffers so we don't have to repeat it */
   for (i = 0; i < EC_MAXBUF; i++) 
   {
      ec_setupheader(&(port->txbuf[i]));
      port->rxbufstat[i] = EC_BUF_EMPTY;
   }
   ec_setupheader(&(port->txbuf2));
   if (r == 0) rval = 1;
   
   return rval;
}

/** Close sockets used
 * @param[in] port        = port context struct
 * @return 0
 */
int ecx_closenic(ecx_portt *port) 
{
   if (port->sockhandle >= 0) 
      close(port->sockhandle);
   if ((port->redport) && (port->redport->sockhandle >= 0))
      close(port->redport->sockhandle);
   
   return 0;
}

/** Fill buffer with ethernet header structure.
 * Destination MAC is allways broadcast.
 * Ethertype is always ETH_P_ECAT.
 * @param[out] p = buffer
 */
void ec_setupheader(void *p) 
{
   ec_etherheadert *bp;
   bp = p;
   bp->da0 = htons(0xffff);
   bp->da1 = htons(0xffff);
   bp->da2 = htons(0xffff);
   bp->sa0 = htons(priMAC[0]);
   bp->sa1 = htons(priMAC[1]);
   bp->sa2 = htons(priMAC[2]);
   bp->etype = htons(ETH_P_ECAT);
}

/** Get new frame identifier index and allocate corresponding rx buffer.
 * @param[in] port        = port context struct
 * @return new index.
 */
int ecx_getindex(ecx_portt *port)
{
   int idx;
   int cnt;

   pthread_mutex_lock( &(port->getindex_mutex) );

   idx = port->lastidx + 1;
   /* index can't be larger than buffer array */
   if (idx >= EC_MAXBUF) 
   {
      idx = 0;
   }
   cnt = 0;
   /* try to find unused index */
   while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF))
   {
      idx++;
      cnt++;
      if (idx >= EC_MAXBUF) 
      {
         idx = 0;
      }
   }
   port->rxbufstat[idx] = EC_BUF_ALLOC;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = EC_BUF_ALLOC;
   port->lastidx = idx;

   pthread_mutex_unlock( &(port->getindex_mutex) );
   
   return idx;
}

/** Set rx buffer status.
 * @param[in] port        = port context struct
 * @param[in] idx      = index in buffer array
 * @param[in] bufstat  = status to set
 */
void ecx_setbufstat(ecx_portt *port, int idx, int bufstat)
{
   port->rxbufstat[idx] = bufstat;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = bufstat;
}

/** Transmit buffer over socket (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx         = index in tx buffer array
 * @param[in] stacknumber  = 0=Primary 1=Secondary stack
 * @return socket send result
 *
 */
int ecx_outframe(ecx_portt *port, int idx, int stacknumber)
{
   int lp, rval;
   ec_stackT *stack;

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   lp = (*stack->txbuflength)[idx];
   rval = write(*stack->sock, (*stack->txbuf)[idx], lp);
   (*stack->rxbufstat)[idx] = EC_BUF_TX;
//   D("Transmitted: %d (size: %d, id: %d)", rval, lp, idx);
   
   return rval;
}

/** Transmit buffer over socket (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx = index in tx buffer array
 * @return socket send result
 */
int ecx_outframe_red(ecx_portt *port, int idx)
{
   ec_comt *datagramP;
   ec_etherheadert *ehp;
   int rval;

   ehp = (ec_etherheadert *)&(port->txbuf[idx]);
   /* rewrite MAC source address 1 to primary */
   ehp->sa1 = htons(priMAC[1]);
   /* transmit over primary socket*/
   rval = ecx_outframe(port, idx, 0);
   if (port->redstate != ECT_RED_NONE)
   {   
      pthread_mutex_lock( &(port->tx_mutex) );
      ehp = (ec_etherheadert *)&(port->txbuf2);
      /* use dummy frame for secondary socket transmit (BRD) */
      datagramP = (ec_comt*)&(port->txbuf2[ETH_HEADERSIZE]);
      /* write index to frame */
      datagramP->index = idx;
      /* rewrite MAC source address 1 to secondary */
      ehp->sa1 = htons(secMAC[1]);
      /* transmit over secondary socket */
      write(port->redport->sockhandle, &(port->txbuf2), port->txbuflength2);
      pthread_mutex_unlock( &(port->tx_mutex) );
      port->redport->rxbufstat[idx] = EC_BUF_TX;
   }   
   
   return rval;
}

/** Non blocking read of socket. Put frame in temporary buffer.
 * @param[in] port        = port context struct
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return >0 if frame is available and read
 */
static int ecx_recvpkt(ecx_portt *port, int stacknumber)
{
   int lp, bytesrx;
   ec_stackT *stack;

   struct bpf_hdr *packet;
   unsigned char bpfbuffer[EC_MAXECATFRAME];
   memset(bpfbuffer, 0, sizeof(bpfbuffer));

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   lp = sizeof(port->tempinbuf);

   bytesrx = read(*stack->sock, bpfbuffer, EC_MAXECATFRAME);
   packet = &bpfbuffer;

   if(bytesrx < 0 ){
       D("Err reading %d: %d (%s)", *stack->sock, errno, strerror(errno));
   }else{
   /// @todo remote
#if 0
       /** QNX's BPF does not implement BIOCSDIRECTION
        * we have to drop outgoing messages ourself
        * @todo find a way to filter on BPF level to reduce the amount
        *   of messages copied to userspace
        */
       uint16 *sa0 = (bpfbuffer + packet->bh_hdrlen + 3*sizeof(uint16));
       if( *sa0 == priMAC[0] || *sa0 == secMAC[0] ) {
           print_ecat_msg((stack->tempbuf), packet->bh_caplen);
            /* drop */
           return 0;
       }
#endif
       /** BPF writes its header first, we cannot pass the pointer to the real buffer */
       memcpy((stack->tempbuf), bpfbuffer + packet->bh_hdrlen, packet->bh_caplen);
//       print_ecat_msg((stack->tempbuf), packet->bh_caplen);

   }
   port->tempinbufs = packet->bh_caplen;

   return (bytesrx > 0);
}

/** Non blocking receive frame function. Uses RX buffer and index to combine
 * read frame with transmitted frame. To compensate for received frames that
 * are out-of-order all frames are stored in their respective indexed buffer.
 * If a frame was placed in the buffer previously, the function retreives it
 * from that buffer index without calling ec_recvpkt. If the requested index
 * is not already in the buffer it calls ec_recvpkt to fetch it. There are
 * three options now, 1 no frame read, so exit. 2 frame read but other
 * than requested index, store in buffer and exit. 3 frame read with matching
 * index, store in buffer, set completed flag in buffer status and exit.
 * 
 * @param[in] port        = port context struct
 * @param[in] idx         = requested index of frame
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME or EC_OTHERFRAME.
 */
int ecx_inframe(ecx_portt *port, int idx, int stacknumber)
{
   uint16  l;
   int     rval;
   int     idxf;
   ec_etherheadert *ehp;
   ec_comt *ecp;
   ec_stackT *stack;
   ec_bufT *rxbuf;

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   rval = EC_NOFRAME;
   rxbuf = &(*stack->rxbuf)[idx];
   /* check if requested index is already in buffer ? */
   if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD)) 
   {
      l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
      /* return WKC */
      rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
      /* mark as completed */
      (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
   }
   else 
   {
      pthread_mutex_lock(&(port->rx_mutex));
      /* non blocking call to retrieve frame from socket */
      if (ecx_recvpkt(port, stacknumber)) 
      {
         rval = EC_OTHERFRAME;
         ehp =(ec_etherheadert*)(stack->tempbuf);
         /* check if it is an EtherCAT frame */
         if (ehp->etype == htons(ETH_P_ECAT)) 
         {
            ecp =(ec_comt*)(&(*stack->tempbuf)[ETH_HEADERSIZE]); 
            l = etohs(ecp->elength) & 0x0fff;
            idxf = ecp->index;

//            D("Received: %d (id: %d)", (*stack->txbuflength)[idx], idxf);
            /* found index equals reqested index ? */
            if (idxf == idx) 
            {
//                D("Msg id %d correct.", idx);
               /* yes, put it in the buffer array (strip ethernet header) */
               memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idx] - ETH_HEADERSIZE);
               /* return WKC */
               rval = ((*rxbuf)[l] + ((uint16)((*rxbuf)[l + 1]) << 8));
               /* mark as completed */
               (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
               /* store MAC source word 1 for redundant routing info */
               (*stack->rxsa)[idx] = ntohs(ehp->sa1);

//               D("  wkc: %d, from: %d", rval, (*stack->rxsa)[idx]);
            }
            else 
            {
               /* check if index exist? */
               if (idxf < EC_MAXBUF) 
               {
                  rxbuf = &(*stack->rxbuf)[idxf];
                  /* put it in the buffer array (strip ethernet header) */
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idxf] - ETH_HEADERSIZE);
                  /* mark as received */
                  (*stack->rxbufstat)[idxf] = EC_BUF_RCVD;
                  (*stack->rxsa)[idxf] = ntohs(ehp->sa1);

                  D("Msg id %d delayed.", idx);
               }
               else 
               {
                  /* strange things happend */
                   D("Msg id %d not found at all", idx);
               }
            }
         }else{
             D("Incorrect ethertype!");
         }
      }
      pthread_mutex_unlock( &(port->rx_mutex) );
      
   }
   
   /* WKC if mathing frame found */
   return rval;
}

/** Blocking redundant receive frame function. If redundant mode is not active then
 * it skips the secondary stack and redundancy functions. In redundant mode it waits
 * for both (primary and secondary) frames to come in. The result goes in an decision
 * tree that decides, depending on the route of the packet and its possible missing arrival,
 * how to reroute the original packet to get the data in an other try. 
 *
 * @param[in] port        = port context struct
 * @param[in] idx = requested index of frame
 * @param[in] timer = absolute timeout time
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
static int ecx_waitinframe_red(ecx_portt *port, int idx, osal_timert *timer)
{
   osal_timert timer2;
   int wkc  = EC_NOFRAME;
   int wkc2 = EC_NOFRAME;
   int primrx, secrx;
   
   /* if not in redundant mode then always assume secondary is OK */
   if (port->redstate == ECT_RED_NONE)
      wkc2 = 0;
   do 
   {
      /* only read frame if not already in */
      if (wkc <= EC_NOFRAME)
         wkc  = ecx_inframe(port, idx, 0);
      /* only try secondary if in redundant mode */
      if (port->redstate != ECT_RED_NONE)
      {   
         /* only read frame if not already in */
         if (wkc2 <= EC_NOFRAME)
            wkc2 = ecx_inframe(port, idx, 1);
      }   
   /* wait for both frames to arrive or timeout */   
   } while (((wkc <= EC_NOFRAME) || (wkc2 <= EC_NOFRAME)) && !osal_timer_is_expired(timer));
   /* only do redundant functions when in redundant mode */
   if (port->redstate != ECT_RED_NONE)
   {
      /* primrx if the reveived MAC source on primary socket */
      primrx = 0;
      if (wkc > EC_NOFRAME) primrx = port->rxsa[idx];
      /* secrx if the reveived MAC source on psecondary socket */
      secrx = 0;
      if (wkc2 > EC_NOFRAME) secrx = port->redport->rxsa[idx];
      
      /* primary socket got secondary frame and secondary socket got primary frame */
      /* normal situation in redundant mode */
      if ( ((primrx == RX_SEC) && (secrx == RX_PRIM)) )
      {
         /* copy secondary buffer to primary */
         memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         wkc = wkc2;
      }   
      /* primary socket got nothing or primary frame, and secondary socket got secondary frame */
      /* we need to resend TX packet */ 
      if ( ((primrx == 0) && (secrx == RX_SEC)) ||
           ((primrx == RX_PRIM) && (secrx == RX_SEC)) )
      {
         /* If both primary and secondary have partial connection retransmit the primary received
          * frame over the secondary socket. The result from the secondary received frame is a combined
          * frame that traversed all slaves in standard order. */
         if ( (primrx == RX_PRIM) && (secrx == RX_SEC) )
         {   
            /* copy primary rx to tx buffer */
            memcpy(&(port->txbuf[idx][ETH_HEADERSIZE]), &(port->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         }
         osal_timer_start (&timer2, EC_TIMEOUTRET);
         /* resend secondary tx */
         ecx_outframe(port, idx, 1);
         do 
         {
            /* retrieve frame */
            wkc2 = ecx_inframe(port, idx, 1);
         } while ((wkc2 <= EC_NOFRAME) && !osal_timer_is_expired(&timer2));
         if (wkc2 > EC_NOFRAME)
         {   
            /* copy secondary result to primary rx buffer */
            memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
            wkc = wkc2;
         }   
      }
   }
   
   /* return WKC or EC_NOFRAME */
   return wkc;
}   

/** Blocking receive frame function. Calls ec_waitinframe_red().
 * @param[in] port        = port context struct
 * @param[in] idx       = requested index of frame
 * @param[in] timeout   = timeout in us
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
int ecx_waitinframe(ecx_portt *port, int idx, int timeout)
{
   int wkc;
   osal_timert timer;
   
   osal_timer_start (&timer, timeout); 
   wkc = ecx_waitinframe_red(port, idx, &timer);
   /* if nothing received, clear buffer index status so it can be used again */
   if (wkc <= EC_NOFRAME) 
   {
      ecx_setbufstat(port, idx, EC_BUF_EMPTY);
   }
   
   return wkc;
}

/** Blocking send and recieve frame function. Used for non processdata frames.
 * A datagram is build into a frame and transmitted via this function. It waits
 * for an answer and returns the workcounter. The function retries if time is
 * left and the result is WKC=0 or no frame received.
 *
 * The function calls ec_outframe_red() and ec_waitinframe_red().
 *
 * @param[in] port        = port context struct
 * @param[in] idx      = index of frame
 * @param[in] timeout  = timeout in us
 * @return Workcounter or EC_NOFRAME
 */
int ecx_srconfirm(ecx_portt *port, int idx, int timeout)
{
   int wkc = EC_NOFRAME;
   osal_timert timer1, timer2;

   osal_timer_start (&timer1, timeout);
   do
   {
      /* tx frame on primary and if in redundant mode a dummy on secondary */
      ecx_outframe_red(port, idx);
      if (timeout < EC_TIMEOUTRET) 
      {
         osal_timer_start (&timer2, timeout); 
      }
      else 
      {
         /* normally use partial timout for rx */
         osal_timer_start (&timer2, EC_TIMEOUTRET); 
      }
      /* get frame from primary or if in redundant mode possibly from secondary */
      wkc = ecx_waitinframe_red(port, idx, &timer2);
   /* wait for answer with WKC>=0 or otherwise retry until timeout */
   } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired (&timer1));
   /* if nothing received, clear buffer index status so it can be used again */
   if (wkc <= EC_NOFRAME) 
   {
      ecx_setbufstat(port, idx, EC_BUF_EMPTY);
   }
   
   return wkc;
}

#ifdef EC_VER1
int ec_setupnic(const char *ifname, int secondary)
{
   return ecx_setupnic(&ecx_port, ifname, secondary);
}

int ec_closenic(void)
{
   return ecx_closenic(&ecx_port);
}

int ec_getindex(void)
{
   return ecx_getindex(&ecx_port);
}

void ec_setbufstat(int idx, int bufstat)
{
   ecx_setbufstat(&ecx_port, idx, bufstat);
}

int ec_outframe(int idx, int stacknumber)
{
   return ecx_outframe(&ecx_port, idx, stacknumber);
}

int ec_outframe_red(int idx)
{
   return ecx_outframe_red(&ecx_port, idx);
}

int ec_inframe(int idx, int stacknumber)
{
   return ecx_inframe(&ecx_port, idx, stacknumber);
}

int ec_waitinframe(int idx, int timeout)
{
   return ecx_waitinframe(&ecx_port, idx, timeout);
}

int ec_srconfirm(int idx, int timeout)
{
   return ecx_srconfirm(&ecx_port, idx, timeout);
}
#endif
