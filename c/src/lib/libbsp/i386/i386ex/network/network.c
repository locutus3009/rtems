/* uti596.c: An 82596 ethernet driver for rtems-bsd. */
void dump_scb(void);

#ifdef DBG_VERSION
#define BREAKPOINT()  asm("   int $3"); 
#else
#define BREAKPOINT()
#endif

#define KERNEL


int forceResetRX = 0;
int forceResetTX = 0;


/*
    
  EII: Oct 16 : Version 0.0


*/

#define DMA_MODE_CASCADE 0xC0   /* pass thru DREQ->HRQ, DACK<-HLDA only */
#define DMA_MASK_REG     0x0A
#define DMA_MODE_REG     0x0B
#define DMA_ENABLE       0x0
#define DMA_DISABLE      0x4   

struct i596_rfd *pISR_Rfd;  


#define MY_PRINT_1( msg, p1 ) {  char buffer[80];  int i,j; sprintf(buffer, msg, p1 );j=strlen(buffer); for ( i=0;i<j;i++) outbyte(buffer[i]); }

void show_buffers (void);
void show_queues(void);

void outbyte(char);
void dumpQ(void);

#define UTI_596_ASSERT( condition, str )
/*
 * {  if (!( condition ) ) printf(str); }
 */
int count_rx = 0;

/* static char *version = "uti596.c:v0.0 11/13/97\n"; */

#include <bsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <rtems/error.h>
#include <rtems/rtems_bsdnet.h>

#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include "uti596.h"
#include "netexterns.h"

#include <asm.h>

int reset_bit_test = 0;

/* #include "../misc/utils.h" */

static struct uti596_softc uti596_softc;

void uti596rxStart(int);
int uti596packetNumber = 0;

static    int scbStatus;
static    rtems_status_code sc;
static    struct i596_cmd *pIsrCmd;
static    struct i596_rfd *pIsrRfd;

/*
 * Initial 596 configuration
 */
char uti596initSetup[] = {
	0x0E,	/* length, prefetch off ( no RBD's ) */
	0xC8,	/* fifo to 8, monitor off */
	0x40,	/* don't save bad frames ( was save= 80, use intel's 40 )*/
	0x2E,	/* No source address insertion, 8 byte preamble */
	0x00,	/* priority and backoff defaults */
	0x60,	/* interframe spacing */
	0x00,	/* slot time LSB */
	0xf2,	/* slot time and retries */
	0x0C,	/* */
	0x08,	/* collision detect */
	0x40,	/* minimum frame length */
	0xfb,	/* tried C8 same as byte 1 in bits 6-7, else ignored*/
	0x00,
	0x3f	/*  no multi IA */ };
/*
 *  Externally defined symbols
 */
#define RX_BUF_COUNT     15
#define TX_BUF_COUNT     4
#define TX_BD_PER_BUF    4

#define INTERRUPT_EVENT         RTEMS_EVENT_1
#define START_TRANSMIT_EVENT	RTEMS_EVENT_2

#define RBUF_SIZE	1520

#define UTI_596_RFD_NUMBER 4

/*
 * Local Routines
 */

/* These are extern, and non-inline  for testing purposes */

void        uti596addCmd                  (struct i596_cmd *pCmd);
void        uti596_initMem                (struct uti596_softc *); 
void        uti596_init                   (void * );
int         uti596initRxBufs              (int num);
int         uti596_initRFA                (int num);
int         uti596initRxBufs              (int num);
static int  uti596_ioctl                  (struct ifnet *, int, caddr_t);
rtems_isr   uti596DynamicInterruptHandler (rtems_vector_number);

void        uti596_txDaemon               (void *);
void        uti596_rxDaemon               (void *);

void        uti596_stop                   (struct uti596_softc *);
static void uti596_start                  (struct ifnet *);

void        uti596reset            (void);

void         uti596_stats      (struct uti596_softc *);

void uti596_initialize_hardware(struct uti596_softc *);

void uti596clearListStatus(struct i596_rfd *);
void uti596addPolledCmd(struct i596_cmd *);

void uti596supplyFD(struct i596_rfd *);

struct i596_rfd * uti596dequeue( struct i596_rfd ** );
void uti596append( struct i596_rfd ** , struct i596_rfd * ); 

#ifdef DEBUG_INIT
static void         print_eth              (unsigned char *);
static void         print_hdr              (unsigned char *);
static void         print_pkt              (unsigned char *);
#endif

void send_packet(struct ifnet *, struct mbuf *);

#define UTI_596_IRQ 5
#define UTI_596_ETH_MIN_SIZE 60

/* Waits for the command word to clear.  The command word is cleared AFTER the interrupt is
 * generated. This allows the CPU to issue the next command
 */
#define  UTI_WAIT_COMMAND_ACCEPTED(duration,function)\
{   int waitcount = duration; \
    while (  uti596_softc.scb.command ) \
      if (--waitcount == 0) \
	{ \
	  printk("%s: i82596 timed out with status %x, cmd %x.\n", function, \
                  uti596_softc.scb.status,  uti596_softc.scb.command); \
	  break; \
    	} \
}



static void nopOn(const rtems_irq_connect_data* notUsed)
{
  /*
   * code should be moved from initialize_hardware
   * to this location ?
   */
}

static int uti596_isOn(const rtems_irq_connect_data* irq)
{
  return BSP_irq_enabled_at_i8259s (irq->name);
}


/***********************************************************************
 *  Function:   uti596initRFA(int num) ( New )
 *
 *  Description:
 *              attempts to allocate and initialize ( chain together )
 *              the requested number of FD's
 *
 *  Algorithm:
 *
 ***********************************************************************/


int uti596_initRFA(int num)
{
    struct i596_rfd *pRfd;
    int i = 0;


#ifdef DBG_596
    printf ("%s: uti596_initRFA %d.\n", num);
#endif

    /* 
     * Initialize the first rfd in the rfa
     */
    pRfd = (struct i596_rfd *) calloc (1,sizeof (struct i596_rfd));
    if ( !pRfd ) {
      printf("Can't allocate all buffers: only %d allocated\n", i);
      return 0;
    }
    else {
      uti596_softc.countRFD = 1;
      uti596_softc.pBeginRFA = uti596_softc.pEndRFA = pRfd;
    printf ( "First Rfd allocated is: %p\n", 
	     uti596_softc.pBeginRFA);
    }

    for (i = 1; i < num; i++) {
      pRfd = (struct i596_rfd *) calloc (1,sizeof (struct i596_rfd) );
      if ( pRfd != NULL ) {
	uti596_softc.countRFD++;
	uti596_softc.pEndRFA -> next = pRfd; /* link it in   */	  
	uti596_softc.pEndRFA         = pRfd; /* move the end */
#ifdef DBG_596_RFA
	printf("Allocated RFD @ %p\n", pRfd);
#endif
      }
      else {	
	printf("Can't allocate all buffers: only %d allocated\n", i);
	break;
      }
    } /* end for */
    
    uti596_softc.pEndRFA -> next = I596_NULL;
    UTI_596_ASSERT(uti596_softc.countRFD == UTI_596_RFD_NUMBER,"INIT:WRONG RFD COUNT\n" ); 
    
#ifdef DBG_596_RFA
    printf ( "Head of RFA is buffer %p\nEnd of RFA is buffer %p \n", 
	     uti596_softc.pBeginRFA, 
	     uti596_softc.pEndRFA );
#endif
    /* initialize the Rfd's */
    for ( pRfd = uti596_softc.pBeginRFA;
	  pRfd != I596_NULL;
	  pRfd = pRfd -> next ) {
	  
      pRfd->cmd = 0x0000;
      pRfd->stat = 0x0000;
      pRfd->pRbd =  I596_NULL;
      pRfd->count = 0;  /* number of bytes in buffer: usually less than size */
      pRfd->size = 1532;      /* was 1532;  buffer size ( All RBD ) */
      if ( pRfd -> data == NULL )
	printf("Can't allocate the RFD data buffer\n");
    }

    /* mark the last FD */ 
    uti596_softc.pEndRFA -> cmd = CMD_EOL; /* moved jan 13 from before the init stuff */

#ifdef DBG_596_RFA
    printf ( "Head of RFA is buffer @ %p \n", uti596_softc.pBeginRFA );
#endif


    uti596_softc.pSavedRfdQueue = 
      uti596_softc.pEndSavedQueue = I596_NULL;   /* initially empty */
    
    uti596_softc.savedCount = 0;

    return (i); /* the number of allocated buffers */
    
}
/***********************************************************************
 *  Function:   uti596supplyFD
 *
 *  Description: returns a buffer to the receive frame pool.
 *               call this with Inetrrupts disabled!
 *
 *  Algorithm:
 *
 ***********************************************************************/
void uti596supplyFD(struct i596_rfd * pRfd )
{
 struct i596_rfd *pLastRfd;


 UTI_596_ASSERT(pRfd != I596_NULL, "Supplying NULL RFD!\n");
 pRfd -> cmd  = CMD_EOL;
 pRfd -> pRbd = I596_NULL;
 pRfd -> next = I596_NULL;
 pRfd -> stat = 0x0000;      /* clear STAT_C and STAT_B bits */

 /* 
  * Check if the list is empty:
  */
 if ( uti596_softc.pBeginRFA == I596_NULL ) {  
   /* Init a list w/ one entry */
   uti596_softc.pBeginRFA = uti596_softc.pEndRFA = pRfd;
   UTI_596_ASSERT(uti596_softc.countRFD == 0, "Null begin, but non-zero count\n");
   if ( uti596_softc.pLastUnkRFD != I596_NULL )
     printf("LastUnkRFD is NOT NULL!!\n");
   uti596_softc.countRFD = 1;
   return;
 }
 /*
  * Check if the last RFD is used/read by the 596.
  */
 pLastRfd = uti596_softc.pEndRFA;
 
 if (    pLastRfd != I596_NULL && 
      ! (pLastRfd -> stat & ( STAT_C | STAT_B ) )) { /* C = complete, B = busy (prefetched) */
   
   /* 
    * Not yet too late to add it 
    */
   pLastRfd -> next = pRfd;
   pLastRfd -> cmd &= ~CMD_EOL;  /* RESET_EL : reset EL bit to 0  */
   uti596_softc.countRFD++;  /* Lets assume we add it successfully 
				    If not, the RFD may be used, and may decrement countRFD < 0 !!*/
   /*    if (uti596_softc.countRFD > UTI_596_RFD_NUMBER) {
     printf( "Supply: Count too high, count == %d, when adding %p\n",uti596_softc.countRFD, pRfd);
     dumpQ();
   }
   */
   /*
    * Check if the last RFD was used while appending.
    */
   if ( pLastRfd -> stat & ( STAT_C | STAT_B ) ) { /* completed or was prefetched */
     /* 
      * Either the EL bit of the last rfd has been read by the 82596,
      * and it will stop after reception,( true when RESET_EL not reached ) or
      * the EL bit was NOT read by the 82596 and it will use the linked
      * RFD for the next reception. ( true is RESET_EL was reached )
      * So, it is unknown whether or not the linked rfd will be used.
      * Therefore, the end of list CANNOT be updated.
      */
     UTI_596_ASSERT ( uti596_softc.pLastUnkRFD == I596_NULL, "Too many Unk RFD's\n" );
     uti596_softc.pLastUnkRFD = pRfd;              
     return;
   }
   else {
     /*
      * The RFD being added was not touched by the 82596
      */
     if (uti596_softc.pLastUnkRFD != I596_NULL ) {
       /*       printf("Before appending\n");
       dumpQ(); */
       uti596append(&uti596_softc.pSavedRfdQueue, pRfd); /* Only here! saved Q */
       uti596_softc.pEndSavedQueue = pRfd;
       uti596_softc.savedCount++;
       uti596_softc.countRFD--;
       /*       printf("Last UNK must be NULL: %p\n",uti596_softc.pLastUnkRFD);
       dumpQ(); */
     }
     else {
       uti596_softc.pEndRFA = pRfd;           /* the RFA has been extended */
       if ( ( uti596_softc.scb.status & SCB_STAT_RNR || 
	      uti596_softc.scb.status & RU_NO_RESOURCES ) &&
	    uti596_softc.countRFD > 1 ) {   /* was == 2 */
	 uti596_softc.pBeginRFA -> cmd &= ~CMD_EOL;  /* Ensure that beginRFA is not EOL */
	 
	 UTI_596_ASSERT(uti596_softc.pEndRFA -> next == I596_NULL, "supply: List is buggered\n");
	 UTI_596_ASSERT(uti596_softc.pEndRFA -> cmd & CMD_EOL, "supply: No EOL at end.\n");
	 UTI_596_ASSERT(uti596_softc.scb.command == 0, "Supply: scb command must be zero\n");
#ifdef DBG_START
	 printf("B");
#endif
	 uti596rxStart(SCB_STAT_RNR);                     /*Ack the RNR interrupt */
	 
       }
     }
       return;
     
   }
 } 
 else {
   /* 
    * too late , pLastRfd in use ( or NULL ), 
    * in either case, EL bit has been read, and RNR condition will occur 
    */
   uti596append( &uti596_softc.pSavedRfdQueue, pRfd); /* save it for RNR */
   
   uti596_softc.pEndSavedQueue = pRfd;                /* reset end of saved queue */  
   uti596_softc.savedCount++;
   
    
   return;
 }
}

void uti596rxStart(int cmd) 
{
  UTI_596_ASSERT(uti596_softc.pBeginRFA != I596_NULL, "rx start w/ NULL begin! \n");
  uti596_softc.scb.pRfd = uti596_softc.pBeginRFA;
  uti596_softc.scb.command = RX_START | cmd;  /* Don't ack RNR! The receiver should be stopped in this case */
  UTI_596_ASSERT( !(uti596_softc.scb.status & SCB_STAT_FR),"FRAME RECEIVED INT COMING!\n");
  outport_byte(CHAN_ATTN, 0); 
}

static void
uti596_start (struct ifnet *ifp)
{
	struct uti596_softc *sc = ifp->if_softc;

	rtems_event_send (sc->txDaemonTid, START_TRANSMIT_EVENT);
	ifp->if_flags |= IFF_OACTIVE;
}

void
uti596_initialize_hardware(struct uti596_softc *sc)
{
  int boguscnt = 1000;
  rtems_status_code status_code;
  
  /* reset the board  */
  outport_word( PORT_ADDR, 0 );
  outport_word( PORT_ADDR, 0 );

  sc->pScp = (struct i596_scp *)
    calloc(1,sizeof(struct i596_scp) + 0xf);
#ifdef DBG_INIT
  printf("initialize_hardware:Scp address initially %p\n", sc->pScp);
#endif
  sc->pScp = (struct i596_scp *)
    ((((int)uti596_softc.pScp) + 0xf) & 0xfffffff0);
  

#ifdef DBG_INIT
  printf("initialize_hardware:change scp address to : %p\n",sc->pScp);
#endif

  
  /* change the scp address */

  printf("Change the SCP address\n");
  
  /*
   * Set the DMA mode to enable the 82596 to become a bus-master
   */
  outport_byte(DMA_MASK_REG,DMA_DISABLE);      /* disable_dma */
  outport_byte(DMA_MODE_REG,DMA_MODE_CASCADE); /* set dma mode */
  outport_byte(DMA_MASK_REG,DMA_ENABLE);       /* enable dma */

  /* reset the board  */
  outport_word( PORT_ADDR, 0 );
  outport_word( PORT_ADDR, 0 );
  
  outport_word(PORT_ADDR, ((((int)uti596_softc.pScp) &  0xffff) | 2 ));
  outport_word(PORT_ADDR, (( (int)uti596_softc.pScp) >> 16 ) & 0xffff );
  
  /* This is linear mode, LOCK function is disabled  */
  
  sc->pScp->sysbus = 0x00540000;
  sc->pScp->iscp   = &sc->iscp;
  sc->iscp.scb     = &sc->scb;
  sc->iscp.stat    = 0x0001;
  
  sc->pCmdHead     = sc->scb.pCmd = I596_NULL;
  
#ifdef DBG_596
  printf("Starting i82596.\n");
#endif
  
  /* Pass the scb address to the 596 */
  outport_word(CHAN_ATTN,0);
  
  while (sc->iscp.stat)
    if (--boguscnt == 0)
      {
	printf("initialize_hardware: timed out with status %4.4lx\n", 
	       sc->iscp.stat );
	break;
      }
  
  /* clear the command word */
  sc->scb.command = 0;
      
  /*
   * Set up interrupts ( NEW irq style )
   */
  sc->irqInfo.name = UTI_596_IRQ;
  sc->irqInfo.hdl  = ( void * ) uti596DynamicInterruptHandler;
  sc->irqInfo.on   = nopOn;
  sc->irqInfo.off  = nopOn;
  sc->irqInfo.isOn = uti596_isOn;
  
  status_code = BSP_install_rtems_irq_handler (&sc->irqInfo);
  if (!status_code)
    rtems_panic ("Can't attach uti596 interrupt handler for irq %d\n",
		  sc->irqInfo.name);

  /* Initialize the 82596 memory ( Transmit buffers ) */
  uti596_initMem(sc);
  
  /* 
   * Create a semaphore to guard the critical sections within 596raw
   * IS THIS NEEDED W/ FreeBSD?
   */
  /*  sc->semaphore_name = rtems_build_name('I','5','9','6');
  status_code = rtems_semaphore_create(sc->semaphore_name,
				       UTI596_MUTEX,
				       RTEMS_DEFAULT_ATTRIBUTES, 
				       RTEMS_NO_PRIORITY,
				       &sc->semaphore_id);
				       */  
#ifdef DBG_INIT
  printf("After attach, status of board = 0x%x\n", sc->scb.status );
#endif
  outport_word(0x380, 0xf); /* reset the LED's */
}


/***********************************************************************
 *  Function:   uti596_initMem
 *
 *  Description:
 *             creates the necessary descriptors for the
 *             uti596 board, hooks the interrupt, and starts the board.
 *             Assumes that interrupts are hooked.
 *
 *  Algorithm:
 *
 ***********************************************************************/


 void
uti596_initMem(struct uti596_softc * sc)
{
    int i,count;
    struct i596_tbd *pTbd;

    sc->resetDone = 0; /* ??? */

    /*
     * Set up receive frame area (RFA) 
     */
    i = uti596_initRFA( sc->rxBdCount );
    if ( i < sc->rxBdCount  ) 
      printf("init_rfd: only able to allocate %d receive frame descriptors\n", i);
    
    sc->scb.pRfd =  sc->pBeginRFA; 

#ifdef DBG_INIT
    printf(" IRQ %d.\n", sc->irqInfo.name);
#endif    
    
    /*
     * Diagnose the health of the board
     */
    uti596Diagnose(1);

    /*
     * set up the i596 config command
     */
#ifdef DBG_INIT
    printf("Configuring\n");
#endif

    sc->set_conf.cmd.command = CmdConfigure;
    memcpy (sc->set_conf.data, uti596initSetup, 14);
    uti596addPolledCmd( (struct i596_cmd *) &sc->set_conf);

    /****
    * POLL
    ****/

    count = 2000;
    while( !( sc->set_conf.cmd.status & STAT_C ) && --count )
      printf(".");

    if ( count )
      printf("Configure OK, count = %d\n",count);
    else
      printf("***Configure failed\n");

    /*******/

    /* 
     * Create the IA setup command
     */

#ifdef DBG_INIT
    printf("Setting Address\n");
#endif
    sc->set_add.cmd.command = CmdSASetup;
    for ( i=0; i<6; i++)
      sc->set_add.data[i]=sc->arpcom.ac_enaddr[i];

    sc->cmdOk = 0;
    uti596addPolledCmd((struct i596_cmd *)&sc->set_add);
        /*******/
    
    count = 2000;
    while( !(sc->set_add.cmd.status & STAT_C ) && --count)
      printf(".");
    
    if ( count )
      printf ("Set Address OK, count= %d\n",count);
    else
      printf("Set Address Failed\n");
    /*******/

#ifdef DBG_INIT
    printf( "After initialization, status and command: 0x%x, 0x%x\n", 
	    sc->scb.status, sc->scb.status);
      
#endif

    /* initialize transmit buffer descriptors*/
    sc->pLastUnkRFD = I596_NULL;
    sc->pTxCmd         = (struct tx_cmd *) calloc (1,sizeof (struct tx_cmd) );
    sc->pTbd           = (struct i596_tbd *) calloc (1,sizeof (struct i596_tbd) );
    sc->pTxCmd -> pTbd = sc->pTbd;
    sc->pTxCmd->cmd.command  = CMD_FLEX|CmdTx;
    sc->pTxCmd->pad          = 0;
    sc->pTxCmd->size         = 0; /* all bytes are in list of TBD's */                         

    pTbd = sc->pTbd;

    for ( i=0; i<sc->txBdCount; i++)
      pTbd = pTbd -> next = (struct i596_tbd *) calloc (1,sizeof (struct i596_tbd) );
      
    pTbd -> next = I596_NULL;

    memset ( &sc->zeroes, 0, 64);   

#ifdef DBG_596
    printf( "After receiver start, status and command: 0x%x, 0x%x\n", 
	    sc->scb.status, sc->scb.status);
#endif
    printf("uti596_initMem allows ISR's\n");
    sc->resetDone = 1; /* now need ISR  */

}



/***********************************************************************
 *  Function:   uti596dump
 *
 *  Description: Dump 596 registers
 *
 *  Algorithm: 
 ***********************************************************************/
/* static */ int
uti596dump(char * pDumpArea)
{
  struct i596_dump dumpCmd;
  int boguscnt = 25000000; /* over a second! */


#ifdef DBG_596  
printf("uti596dump:\n");
#endif

  dumpCmd.cmd.command = CmdDump;
  dumpCmd.cmd.next    = I596_NULL;
  dumpCmd.pData       = pDumpArea;
  uti596_softc.cmdOk = 0;
  uti596addCmd        ( (struct i596_cmd *)&dumpCmd);
  while (1)
    if ( --boguscnt == 0){
      printf("Dump command was not processed within spin loop delay\n");
      return 0;
    }
    else {
      if ( uti596_softc.cmdOk )
	return 1; /* successful completion */
    }
      
  
}

/***********************************************************************
 *  Function:   uti596_rxDaemon
 *
 *  Description: Receiver task
 *
 *  Algorithm: Extract the packet from an RFD, and place into an 
 *             mbuf chain.  Place the mbuf chain in the network task
 *             queue. Assumes that the frame check sequence is removed
 *             by the 82596.
 *
 ***********************************************************************/

/* static */ void
uti596_rxDaemon(void *arg)
{
  struct uti596_softc *sc = (struct uti596_softc *)arg;
  struct ifnet *ifp = &sc->arpcom.ac_if;
  struct mbuf *m;

  struct i596_rfd *pRfd;
  ISR_Level level;
  int tid,i;
  rtems_event_set events;
  struct ether_header *eh;
  
  int frames = 0;
  
#ifdef DBG_596
  printf ("uti596_rxDaemon\n");
  printf("&scb = %p, pRfd = %p\n", &sc->scb,sc->scb.pRfd);
#endif    

  rtems_task_ident (0, 0, &tid);

#ifdef DBG_596
  printf("RX tid = 0x%x\n", tid);
#endif    

    for(;;) {
      /*
       * Wait for packet.
       */
#ifdef DBG_596
      printf("Receiver sleeps\n");
#endif

      rtems_bsdnet_event_receive (INTERRUPT_EVENT,
				  RTEMS_WAIT|RTEMS_EVENT_ANY,
				  RTEMS_NO_TIMEOUT,
				  &events);
      
#ifdef DBG_596
      printf("Receiver wakes\n");
#endif
      /* 
       * While received frames are available. Note that the frame may be
       * a fragment, so it is NOT a complete packet.
       */
      pRfd = uti596dequeue( &sc->pInboundFrameQueue);
      while ( pRfd && 
	      pRfd != I596_NULL &&
	      pRfd -> stat & STAT_C )
	{

#ifdef DEBUG_INIT
	  printf("\nReceived packet:\n");
	  print_eth( pRfd->data);
#endif
	  if ( pRfd->stat & STAT_OK){
	    /*	 a good frame. Allocate an mbuf to take it from the queue */

	    int pkt_len = pRfd->count & 0x3fff; /* the actual # of bytes received */

#ifdef DBG_596
	    printf("Good frame, @%p, data @%p length %d\n", pRfd, pRfd -> data , pkt_len);
#endif
	    frames++;

	    
            /* 
	     * Allocate an mbuf to give to the stack
             * The format of the data portion of the RFD is:
             * <ethernet header, payload>.  
	     * The FRAME CHECK SEQUENCE / CRC is stripped by the uti596. 
             * This is to be optimized later.... should not have to memcopy!
	     */
	    MGETHDR(m, M_WAIT, MT_DATA);
	    MCLGET(m, M_WAIT);

	    m->m_pkthdr.rcvif = ifp;
	    /* move everything into an mbuf */
	    memcpy(m->m_data, 
		   pRfd->data, 
		   pkt_len);

	    m->m_len = m->m_pkthdr.len = pkt_len - sizeof(struct ether_header) - 4;

	    /* move the header to an mbuf */
	    eh = mtod (m, struct ether_header *);
	    m->m_data += sizeof(struct ether_header);

#ifdef DBG_596
	      printf("m->m_ext: %p pRfd -> data: %p\n",
		     m->m_ext,  pRfd -> data);
#endif
#ifdef DEBUG_INIT_2
	    printf("mbuf contains:\n");
	    print_eth( (char *) (((int)m->m_data)-sizeof(struct ether_header)));
	    for ( i = 0; i<20; i++)
	      printf(".");

#endif
#ifdef DBG_VERSION
	  BREAKPOINT();
#endif
	    ether_input (ifp, eh, m);

          } /* end if STAT_OK */
	   
	  else {
	    /* 
	     * A bad frame is present: Note that this could be the last RFD!
	     */
#ifdef DBG_596
	    printf("Bad frame\n");
#endif
	    /* 
	     * FIX ME: use the statistics from the SCB
	     */
	    sc->stats.rx_errors++;
	    if ((sc->scb.pRfd->stat) & 0x0001) 
	      sc->stats.collisions++;
	    if ((sc->scb.pRfd->stat) & 0x0080) 
	      sc->stats.rx_length_errors++;
	    if ((sc->scb.pRfd->stat) & 0x0100) 
	      sc->stats.rx_over_errors++;
	    if ((sc->scb.pRfd->stat) & 0x0200) 
	      sc->stats.rx_fifo_errors++;
	    if ((sc->scb.pRfd->stat) & 0x0400) 
	      sc->stats.rx_frame_errors++;
	    if ((sc->scb.pRfd->stat) & 0x0800) 
	      sc->stats.rx_crc_errors++;
	    if ((sc->scb.pRfd->stat) & 0x1000) 
	      sc->stats.rx_length_errors++;
	  }

	  UTI_596_ASSERT(pRfd != I596_NULL, "Supplying NULL RFD\n");

#ifdef DBG_SUPPLY_FD 
	  printf("Supply FD Starting\n");
#endif
	  _ISR_Disable(level);
	  uti596supplyFD ( pRfd );   /* Return RFD to RFA. CAN WE REALLY?*/
	  _ISR_Enable(level);
#ifdef DBG_SUPPLY_FD 
	  printf("Supply FD Complete\n");
#endif
#ifdef DBG_VERSION
	  BREAKPOINT();
#endif

	  pRfd = uti596dequeue( &sc->pInboundFrameQueue); /* grab next frame */
	  
	} /* end while */
    } /* end for(;;)*/
    
#ifdef DBG_596
    printf ("frames %d\n", frames);
#endif
    
}

 /***********************************************************************
  *  Function:   uti596clearListStatus
  *
  *  Description:
  *             Clear the stat fields for all rfd's
  *  Algorithm: 
  *
  ***********************************************************************/

void
uti596clearListStatus(struct i596_rfd *pRfd)
{

  while ( pRfd != I596_NULL ) {
    pRfd -> stat = 0;           /* clear the status field */
    pRfd = pRfd-> next; 
  }
}


 /* static inline */ void uti596reset_bit_test()
 {
   reset_bit_test = 1;
 }

void uti596reset(void)
 {
   int i;

#ifdef DBG_596_RESET
     printf ("i596reset\n");
#endif

  uti596_initialize_hardware(&uti596_softc); /* resets the ethernet hardware, now must re-config */
  uti596Diagnose(1); /* verbose diagnosis */

  uti596_softc.set_conf.cmd.command = CmdConfigure;
  memcpy (uti596_softc.set_conf.data, uti596initSetup, 14);
  uti596addPolledCmd( (struct i596_cmd *) &uti596_softc.set_conf);
  
  while( !uti596_softc.set_conf.cmd.status & STAT_C )
    printf(".");

  printf("Configure completed\n");
#ifdef DBG_596
  printf("Setting Address\n");
#endif
  uti596_softc.set_add.cmd.command = CmdSASetup;
  for ( i=0; i<6; i++)
    uti596_softc.set_add.data[i]= uti596_softc.arpcom.ac_enaddr[i];
  
  uti596_softc.cmdOk = 0;
  uti596addCmd((struct i596_cmd *)&uti596_softc.set_add);
  /*******/
  while( !uti596_softc.set_add.cmd.status & STAT_C )
    printf(".");
  /*******/
  
#ifdef DBG_596
  printf( "After initialization, status and command: 0x%x, 0x%x\n", 
	  uti596_softc.scb.status, uti596_softc.scb.status);
  
#endif
  if ( uti596_softc.pLastUnkRFD != I596_NULL ) {
    uti596_softc. pEndRFA =  uti596_softc. pLastUnkRFD; /* The end position can be updated */
    uti596_softc. pLastUnkRFD = I596_NULL;           
  }
  
  /* clear status in RFA */
  uti596clearListStatus(uti596_softc.pBeginRFA );
  uti596clearListStatus(uti596_softc.pSavedRfdQueue );
  uti596_softc.scb.pRfd =  uti596_softc.pBeginRFA; 

  uti596_softc.scb.command = RX_START;
  outport_word(CHAN_ATTN,0 ); 
  UTI_WAIT_COMMAND_ACCEPTED(4000, "init");
  
  uti596_softc.started = 1;
 }
 
 /***********************************************************************
  *  Function:   uti596addCmd
  *
  *  Description:
  *             This routine adds a command onto the end of the
  *             command chain
  *
  *  Algorithm:
  *            Add the command to the end of and existing chain,
  *            or start the chain and issue a CUC_START
  *
  *
  ***********************************************************************/

 /* static */ void uti596addCmd(struct i596_cmd *pCmd)
 {
     ISR_Level level;

 #ifdef DBG_596
     printf("Adding command 0x%x\n", pCmd -> command );
 #endif

#ifdef DEBUG_ADD
     
     switch ( pCmd -> command & 0x7 ){ /* check bottom 7 bits */
     case CmdConfigure:
       printf("ADD: Configure Command 0x%x\n", pCmd->command);
       break;
     case CmdSASetup:
       printf("ADD: Set Address Command 0x%x\n", pCmd->command);
       break;
     case CmdMulticastList:
       printf("ADD: Multi-cast list 0x%x\n", pCmd->command);
       break;
     case CmdNOp:
        printf("ADD: NO op 0x%x\n", pCmd->command);
	break;
     case CmdTDR:
        printf("ADD: TDR 0x%x\n", pCmd->command);
        break;
     case CmdDump:
       printf("ADD: Dump 0x%x\n", pCmd->command);
       break;
     case CmdDiagnose:
       printf("ADD: Diagnose 0x%x\n", pCmd->command);
       break;
     case CmdTx:
       break;
     default:
       printf("****Unknown Command encountered 0x%x\n", pCmd->command);
       break;
     } /* end switch */
       
#endif



     pCmd->status = 0;
     pCmd->command |= (CMD_EOL | CMD_INTR ); /* all commands last in list & return an interrupt */
     if ( reset_bit_test ){
       pCmd->command |= 0x0080;
       printf("Issuing cmd: %#x\n",  pCmd->command);
       reset_bit_test = 0;
     }

     pCmd->next = I596_NULL;

     _ISR_Disable(level);
     if (uti596_softc.pCmdHead == I596_NULL)
       {
	 uti596_softc.pCmdHead =      
	   uti596_softc.pCmdTail =
	   uti596_softc.scb.pCmd = pCmd;
#ifdef DBG_596
	 printf("First Cmd\n");
#endif
	 UTI_WAIT_COMMAND_ACCEPTED(10000,"add command"); /* wait for acceptance of previous command */
	 /* CHANGED TO |= Mar. 27 4:20 pm, was just =. changed back jun 18 1998. The wait assures command = 0 */
	 uti596_softc.scb.command = CUC_START;
	 outport_word (CHAN_ATTN,0);
     _ISR_Enable(level);
       }
     else 
       {
#ifdef DBG_596
	 printf("Chained Cmd\n");
#endif
	 uti596_softc.pCmdTail->next = pCmd; 
	 uti596_softc.pCmdTail = pCmd;           /* added Jan 30 */
     _ISR_Enable(level);
       }


#ifdef DBG_596
	 printf("Scb status & command 0x%x 0x%x\n", 
		uti596_softc.scb.status,
		uti596_softc.scb.command );
#endif
	 
 }
 
 /***********************************************************************
  *  Function:   uti596addPolledCmd
  *
  *  Description:
  *             This routine issues a single command then polls for it's
  *             completion.  TO BE CALLED FROM ISR ONLY
  *
  *  Algorithm:
  *            Give the command to the driver. ( CUC_START is ALWAYS required )
  *            Poll for completion. 
  *
  ***********************************************************************/

void uti596addPolledCmd(struct i596_cmd *pCmd)
 {

 #ifdef DBG_596
     printf("Adding command 0x%x\n", pCmd -> command );
 #endif

#ifdef DEBUG_POLLED_CMD
     
     switch ( pCmd -> command & 0x7 ){ /* check bottom 7 bits */
     case CmdConfigure:
       printf("PolledCMD: Configure Command 0x%x\n", pCmd->command);
       break;
     case CmdSASetup:
       printf("PolledCMD: Set CMDress Command 0x%x\n", pCmd->command);
       break;
     case CmdMulticastList:
       printf("PolledCMD: Multi-cast list 0x%x\n", pCmd->command);
       break;
     case CmdNOp:
        printf("PolledCMD: NO op 0x%x\n", pCmd->command);
	break;
     case CmdTDR:
        printf("PolledCMD: TDR 0x%x\n", pCmd->command);
        break;
     case CmdDump:
       printf("PolledCMD: Dump 0x%x\n", pCmd->command);
       break;
     case CmdDiagnose:
       printf("PolledCMD: Diagnose 0x%x\n", pCmd->command);
       break;
     case CmdTx:
       break;
     default:
       printf("PolledCMD: ****Unknown Command encountered 0x%x\n", pCmd->command);
       break;
     } /* end switch */
       
#endif

     pCmd->status = 0;
     pCmd->command |=  CMD_EOL ; /* only command in list*/

     pCmd->next = I596_NULL;

     uti596_softc.pCmdHead = uti596_softc.pCmdTail = uti596_softc.scb.pCmd = pCmd;

     UTI_WAIT_COMMAND_ACCEPTED(10000,"Add Polled command: wait prev"); /* ensure prev command complete */

     uti596_softc.scb.command = CUC_START;
     outport_word (CHAN_ATTN,0);

     UTI_WAIT_COMMAND_ACCEPTED(2000000,"Add Polled command: start"); /* ensure previous command complete */
     /*     if ( !(pCmd -> status & STAT_C ) ){
	    printf("****FAILED poll command\n"); 
	    }
	    */
     uti596_softc.pCmdHead = uti596_softc.pCmdTail = uti596_softc.scb.pCmd = I596_NULL;

#ifdef DBG_596
     printf("Scb status & command 0x%x 0x%x\n", 
	    uti596_softc.scb.status,
	    uti596_softc.scb.command );
#endif
	 
 }
/*
 * Driver transmit daemon
 */
void
uti596_txDaemon (void *arg)
{
	struct uti596_softc *sc = (struct uti596_softc *)arg;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mbuf *m;
	rtems_event_set events;

	for (;;) {
	  /*
	   * Wait for packet from stack
	   */
	  rtems_bsdnet_event_receive (START_TRANSMIT_EVENT, 
				      RTEMS_EVENT_ANY | RTEMS_WAIT, 
				      RTEMS_NO_TIMEOUT, &events);
	  
	  /*
	   * Send packets till queue is empty
	   */
	  for (;;) { 
	    /*
	     * Get the next mbuf chain to transmit.
	     */
	    IF_DEQUEUE(&ifp->if_snd, m);
	    if (!m)
	      break;
	    
	    send_packet (ifp, m); /* blocks */
	  }  
	  ifp->if_flags &= ~IFF_OACTIVE; /* no more to send, mark output inactive  */
	}
}

 
 /***********************************************************************
  *  Function:   send_packet
  *
  *  Description: Send a raw ethernet packet
  *              
  *  Algorithm:
  *             increment some stats counters,
  *             create the transmit command,
  *             add the command to the CBL,
  *             wait for event
  *
  ***********************************************************************/

void send_packet(struct ifnet *ifp, struct mbuf *m)
{
  struct i596_tbd *pPrev = I596_NULL,
    *pRemainingTbdList,
    *pTbd;
  struct mbuf *n, *input_m = m;
  
  struct uti596_softc *sc = ifp->if_softc; /* is this available from ifp ?*/

  struct mbuf *l = NULL; 
  unsigned int length = 0;
  rtems_status_code status;
  int bd_count = 0;
  rtems_event_set events;

  /*
   * Block all competing threads. IS THIS NOW NEEDED? I THINK NOT
   */
  
  /*   while(1) {
#ifdef DBG_RAW
    rtems_task_ident (0, 0, &tid);
    printf("0x%x Waiting for semaphore\n", tid);
#endif
      status = rtems_semaphore_obtain(sc->semaphore_id,
				    RTEMS_WAIT, 
				    RTEMS_NO_TIMEOUT);
    
    if ( status == RTEMS_SUCCESSFUL ){
#ifdef DBG_RAW
      printf("0x%x Got semaphore\n", tid);
#endif
      break;
    }
    else 
      {
#ifdef DBG_RAW
	printf("0x%x Waits on semaphore\n", tid);
#endif
	sc->txRawWait++;
      }
  }
  */
  /* 
   * For all mbufs in the chain, 
   *  fill a transmit buffer descriptor
   */
  pTbd = sc->pTxCmd->pTbd;

  do {              
    if (m->m_len) {
      /*
       * Fill in the buffer descriptor
       */
      length    += m->m_len;
      pTbd->data = mtod (m, void *);
      pTbd->size = m->m_len;
      pPrev      = pTbd;
      pTbd       = pTbd -> next;
      l          = m;
      m          = m->m_next;
    }
    else {
      /*
       * Just toss empty mbufs
       */
      MFREE (m, n);
      m = n;
      if (l != NULL)
	l->m_next = m;
    }
  } while( m != NULL && ++bd_count < 16 );


  /* This should never happen */
  if ( bd_count == 16 ) {
    printf("TX ERROR:Too many mbufs in the packet!!!\n");
    printf("Must coalesce!\n");
  }
  
  
  if ( length < UTI_596_ETH_MIN_SIZE ) {
    pTbd->data = sc->zeroes;       /* add padding to pTbd */
    pTbd->size = UTI_596_ETH_MIN_SIZE - length; /* zeroes have no effect on the CRC */
  }
  else
    pTbd = pPrev; /* Don't use pTbd in the send routine */
  
  /*  Disconnect the packet from the list of Tbd's  */
  pRemainingTbdList = pTbd->next;
  pTbd->next  = I596_NULL;     
  pTbd->size |= UTI_596_END_OF_FRAME;    
  
#ifdef DBG_RAW
  printf("RAW:Add cmd and sleep\n");
#endif
  
  sc->rawsndcnt++;     
    
#ifdef DBG_RAW
  printf ("sending packet\n");
#endif
  
  /* Sending Zero length packet: shouldn't happen */
  if (pTbd->size <= 0) return ;


#ifdef DEBUG_INIT_2
  printf("\nTransmitter adds packet\n");
  print_hdr    ( sc->pTxCmd->pTbd->data ); /* print the first part */
  print_pkt    ( sc->pTxCmd->pTbd->next->data ); /* print the first part */
  /*  print_echo(sc->pTxCmd->pTbd->data); */
#endif
#ifdef DBG_VERSION
	  BREAKPOINT();
#endif


  /* add the command to the output command queue */
  uti596addCmd ( (struct i596_cmd *) sc->pTxCmd );
  
  /* sleep until the command has been processed or Timeout encountered. */
  status= rtems_bsdnet_event_receive (INTERRUPT_EVENT,
				      RTEMS_WAIT|RTEMS_EVENT_ANY,
				      RTEMS_NO_TIMEOUT,
				      &events);
   
  if ( status != RTEMS_SUCCESSFUL ) {
    printf("Could not sleep %s\n", rtems_status_text(status)); 
  }
  
#ifdef DBG_RAW
  printf("RAW: wake\n");
#endif
  
  sc->txInterrupts++;
      
#ifdef DEBUG_INIT
  printf("\nTransmitter issued packet\n");
  print_hdr    ( sc->pTxCmd->pTbd -> data ); /* print the first part */
  print_pkt    ( sc->pTxCmd->pTbd ->next-> data ); /* print the first part */
#endif

  if ( sc->pTxCmd -> cmd.status & STAT_OK )
    sc->stats.tx_packets++;
  else
    {
#ifdef DBG_RAW
      printf("******Driver Error 0x%x\n", sc->pTxCmd -> cmd.status );
#endif
      sc->stats.tx_errors++;
      if ( sc->pTxCmd->cmd.status  & 0x0020 ) 
	sc->stats.tx_retries_exceeded++;
      if (!(sc->pTxCmd->cmd.status & 0x0040)) 
	sc->stats.tx_heartbeat_errors++;
      if ( sc->pTxCmd->cmd.status  & 0x0400 ) 
	sc->stats.tx_carrier_errors++;
      if ( sc->pTxCmd->cmd.status  & 0x0800 ) 
	sc->stats.collisions++;
      if ( sc->pTxCmd->cmd.status  & 0x1000 ) 
	sc->stats.tx_aborted_errors++;
    } /* end if stat_ok */	 
  
  /* 
   * Restore the transmited buffer descriptor chain.
   */
  pTbd -> next = pRemainingTbdList;
  
  /*
   * Free the mbufs used by the sender.
   */
  m = input_m;
  while ( m != NULL ) {
    MFREE(m,n);
    m = n;
  }

  /* We have finished with this critical section */
  /* rtems_semaphore_release(sc->semaphore_id); */
  
}

 /***********************************************************************
  *  Function:   print_eth
  *
  *  Description:
  *              Print the contents of an ethernet packet header
  *              CANNOT BE CALLED FROM ISR
  *
  *  Algorithm:
  *            Print Destination, Src, and type of packet
  *
  ***********************************************************************/

 /* static */ void print_eth(unsigned char *add)
 {
     int i;
     short int length;

     printf("Packet Location %p\n", add);

     printf ("Dest  ");

     for (i = 0; i < 6; i++)
	 printf(" %2.2X", add[i]);

     printf ("\n");

     printf ("Source");

     for (i = 6; i < 12; i++)
	 printf(" %2.2X", add[i]);

     printf ("\n");

     printf ("frame type %2.2X%2.2X\n", add[12], add[13]);

     if ( add[12] == 0x08 && add[13] == 0x06 )
       { /* an ARP */
	 printf("Hardware type : %2.2X%2.2X\n", add[14],add[15]);
	 printf("Protocol type : %2.2X%2.2X\n", add[16],add[17]);
	 printf("Hardware size : %2.2X\n", add[18]);
	 printf("Protocol size : %2.2X\n", add[19]);
	 printf("op            : %2.2X%2.2X\n", add[20],add[21]);

	 printf("Sender Enet addr: ");

	 for ( i=0; i< 5 ; i++)
	   printf( "%x:", add[22 + i]);

	 printf("%x\n", add[27]);

	 printf("Sender IP addr: ");
	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[28 + i]);

	 printf("%u\n", add[31]);

	 printf("Target Enet addr: ");
	 for ( i=0; i< 5 ; i++)
	   printf( "%x:", add[32 + i]);
	 printf("%x\n", add[37]);

	 printf("Target IP addr: ");

	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[38 + i]);
	 printf("%u\n", add[41]);

	}

     if ( add[12] == 0x08 && add[13] == 0x00 )
       { /* an IP packet */
	 printf("*********************IP HEADER******************\n");
	 printf("IP version/IPhdr length: %2.2X TOS: %2.2X\n", add[14] , add[15]);
	 printf("IP total length: %2.2X %2.2X, decimal %d\n", add[16], add[17], length = (add[16]<<8 | add[17] )); 
	 printf("IP identification: %2.2X %2.2X, 3-bit flags and offset %2.2X %2.2X\n",
		add[18],add[19], add[20], add[21]);
	 printf("IP TTL: %2.2X, protocol: %2.2X, checksum: %2.2X %2.2X \n",
		add[22],add[23],add[24],add[25]);
	 printf("IP packet type: %2.2X code %2.2X\n", add[34],add[35]);

	 printf("Source IP address: ");
	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[26 + i]);

	 printf("%u\n", add[29]);

	 printf("Destination IP address: ");
	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[30 + i]);
	 printf("%u\n", add[33]);

	 /* printf("********************IP Packet Data*******************\n");
	 	 length -=20;
	 for ( i=0; i < length ; i++)
	   printf("0x%2.2x ", add[34+i]);
	 printf("\n");

	 printf("ICMP checksum: %2.2x %2.2x\n", add[36], add[37]);
	 printf("ICMP identifier: %2.2x %2.2x\n", add[38], add[39]);
	 printf("ICMP sequence nbr: %2.2x %2.2x\n", add[40], add[41]);
	 */
	}


 }
#ifdef DEBUG_INIT

 /***********************************************************************
  *  Function:   print_eth
  *
  *  Description:
  *              Print the contents of an ethernet packet header
  *              CANNOT BE CALLED FROM ISR
  *
  *  Algorithm:
  *            Print Destination, Src, and type of packet
  *
  ***********************************************************************/

 /* static */ void print_hdr(unsigned char *add)
 {
     int i;
     short int length;

     printf("Header Location %p\n", add);

     printf ("Dest  ");

     for (i = 0; i < 6; i++)
	 printf(" %2.2X", add[i]);

     printf ("\n");

     printf ("Source");

     for (i = 6; i < 12; i++)
	 printf(" %2.2X", add[i]);

     printf ("\n");

     printf ("frame type %2.2X%2.2X\n", add[12], add[13]);


 }
 /***********************************************************************
  *  Function:   print_pkt
  *
  *  Description:
  *              Print the contents of an ethernet packet header
  *              CANNOT BE CALLED FROM ISR
  *
  *  Algorithm:
  *            Print Destination, Src, and type of packet
  *
  ***********************************************************************/

 /* static */ void print_pkt(unsigned char *add)
 {
     int i;
     short int length;

     printf("Data Location %p\n", add);

     if ( add[0] == 0x08 && add[1] == 0x06 )
       { /* an ARP */
	 printf("Hardware type : %2.2X%2.2X\n", add[14],add[15]);
	 printf("Protocol type : %2.2X%2.2X\n", add[16],add[17]);
	 printf("Hardware size : %2.2X\n", add[18]);
	 printf("Protocol size : %2.2X\n", add[19]);
	 printf("op            : %2.2X%2.2X\n", add[20],add[21]);

	 printf("Sender Enet addr: ");

	 for ( i=0; i< 5 ; i++)
	   printf( "%x:", add[22 + i]);

	 printf("%x\n", add[27]);

	 printf("Sender IP addr: ");
	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[28 + i]);

	 printf("%u\n", add[31]);

	 printf("Target Enet addr: ");
	 for ( i=0; i< 5 ; i++)
	   printf( "%x:", add[32 + i]);
	 printf("%x\n", add[37]);

	 printf("Target IP addr: ");

	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[38 + i]);
	 printf("%u\n", add[41]);

	}

     if ( add[0] == 0x08 && add[1] == 0x00 )
       { /* an IP packet */
	 printf("*********************IP HEADER******************\n");
	 printf("IP version/IPhdr length: %2.2X TOS: %2.2X\n", add[14] , add[15]);
	 printf("IP total length: %2.2X %2.2X, decimal %d\n", add[16], add[17], length = (add[16]<<8 | add[17] )); 
	 printf("IP identification: %2.2X %2.2X, 3-bit flags and offset %2.2X %2.2X\n",
		add[18],add[19], add[20], add[21]);
	 printf("IP TTL: %2.2X, protocol: %2.2X, checksum: %2.2X %2.2X \n",
		add[22],add[23],add[24],add[25]);
	 printf("IP packet type: %2.2X code %2.2X\n", add[34],add[35]);

	 printf("Source IP address: ");
	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[26 + i]);

	 printf("%u\n", add[29]);

	 printf("Destination IP address: ");
	 for ( i=0; i< 3 ; i++)
	   printf( "%u.", add[30 + i]);
	 printf("%u\n", add[33]);

	 printf("********************IP Packet Data*******************\n");
	 length -=20;
	 for ( i=0; i < length ; i++)
	   printf("0x%2.2x ", add[34+i]);
	 printf("\n");

	 printf("ICMP checksum: %2.2x %2.2x\n", add[36], add[37]);
	 printf("ICMP identifier: %2.2x %2.2x\n", add[38], add[39]);
	 printf("ICMP sequence nbr: %2.2x %2.2x\n", add[40], add[41]);
	}


 }
 /***********************************************************************
  *  Function:   print_echo
  *
  *  Description:
  *              Print the contents of an ethernet packet header
  *              CANNOT BE CALLED FROM ISR
  *
  *  Algorithm:
  *            Prints only echo packets
  *
  ***********************************************************************/

 /* static */ void print_echo(unsigned char *add)
 {
     int i;
     short int length;

     if ( add[12] == 0x08 && add[13] == 0x00 ){ /* an IP packet */
       
       printf("Packet Location %p\n", add);
       
       printf ("Dest  ");
       
       for (i = 0; i < 6; i++)
	 printf(" %2.2X", add[i]);
       
       printf ("\n");
       
       printf ("Source");
       
       for (i = 6; i < 12; i++)
	 printf(" %2.2X", add[i]);
       
       printf ("\n");
       
       printf ("frame type %2.2X%2.2X\n", add[12], add[13]);
       
       printf("*********************IP HEADER******************\n");
       printf("IP version/IPhdr length: %2.2X TOS: %2.2X\n", add[14] , add[15]);
       printf("IP total length: %2.2X %2.2X, decimal %d\n", add[16], add[17], length = (add[16]<<8 | add[17] )); 
       printf("IP identification: %2.2X %2.2X, 3-bit flags and offset %2.2X %2.2X\n",
	      add[18],add[19], add[20], add[21]);
       printf("IP TTL: %2.2X, protocol: %2.2X, checksum: %2.2X %2.2X \n",
	      add[22],add[23],add[24],add[25]);
       printf("IP packet type: %2.2X code %2.2X\n", add[34],add[35]);
       
       printf("Source IP address: ");
       for ( i=0; i< 3 ; i++)
	 printf( "%u.", add[26 + i]);
       
       printf("%u\n", add[29]);
       
       printf("Destination IP address: ");
       for ( i=0; i< 3 ; i++)
	 printf( "%u.", add[30 + i]);
       printf("%u\n", add[33]);
       
       printf("********************IP Packet Data*******************\n");
       length -=20;
       for ( i=0; i < length ; i++)
	 printf("0x%2.2x ", add[34+i]);
       printf("\n");
       
       printf("ICMP checksum: %2.2x %2.2x\n", add[36], add[37]);
       printf("ICMP identifier: %2.2x %2.2x\n", add[38], add[39]);
       printf("ICMP sequence nbr: %2.2x %2.2x\n", add[40], add[41]);
     }   
 }
#endif
 
 /***********************************************************************
  *  Function:   uti596_attach
  *
  *  Description:
  *              User requested attach of driver to hardware
  *
  *  Algorithm:
  *
  *              Check that the board is present
  *              parse and store the command line parameters
  *                 argv[0]: interface label, e.g., "uti596"
  *                 argv[1]: maximum transmission unit, bytes, e.g., "1500"
  *                 argv[2]: IP address (optional)
  *              initialize required rx and tx buffers
  *              hook interrupt
  *              issue start command and some diagnostics
  *              return       
  *
  ***********************************************************************/


int uti596_attach(struct rtems_bsdnet_ifconfig * pConfig )
{
  struct uti596_softc *sc = &uti596_softc;          /* soft config */
  struct ifnet * ifp = &sc->arpcom.ac_if;
  int i = 0;
  
#ifdef DBG_ATTACH
  printf("attach");
#endif

  
  sc->started = 0; /* The NIC is not started yet */
  sc->ioAddr = IO_ADDR;

  /* Indicate to ULCS that this is initialized */  
  ifp->if_softc = sc;  
  
  /* Assign the name */
  ifp->if_name = "uti";
  
  /* Assign the unit number */
  ifp->if_unit = 1;
  
  /* Assign mtu */
  if ( pConfig -> mtu )
    ifp->if_mtu = pConfig -> mtu;
  else
    ifp->if_mtu = ETHERMTU;
  
  /* Assign and possibly override the hw address */

  if ( !pConfig->hardware_address) { /* Read the ethernet address from the board */
    for (i = 0; i < 8; i++)
      inport_byte(NIC_ADDR+i,sc->arpcom.ac_enaddr[i] );
  }
  else {
    /* hwaddr override */ 
    memcpy (sc->arpcom.ac_enaddr, pConfig->hardware_address, ETHER_ADDR_LEN);
  }
  
  /* Test for valid hwaddr */
  if(memcmp(sc->arpcom.ac_enaddr,"\xAA\x55\x01",3)!= 0)/* b0 of byte 0 != 0 => multicast */
    return ENODEV;
  
  /* Assign requested receive buffer descriptor count */
  if (pConfig->rbuf_count)
    sc->rxBdCount = pConfig->rbuf_count;
  else
    sc->rxBdCount = RX_BUF_COUNT;

  /* Assign requested tx buffer descriptor count */
  if (pConfig->xbuf_count)
    sc->txBdCount = pConfig->xbuf_count;
  else
    sc->txBdCount = TX_BUF_COUNT * TX_BD_PER_BUF;

  /* ignore_broadcast is an unused feature... accept broadcast */
  /* sc->acceptBroadcast = !pConfig->ignore_broadcast; */

  ifp->if_snd.ifq_maxlen = ifqmaxlen;
  ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX;

  /* to init_hardware */
  sc->started = 1;
  sc->pInboundFrameQueue = I596_NULL;
 
 
  ifp->if_ioctl = uti596_ioctl;
  ifp->if_init  = uti596_init;
  ifp->if_start    = uti596_start;
  ifp->if_output   = ether_output;
  
  sc->scb.command = 0;
  
  /*
   * Attach the interface
   */
  if_attach (ifp);
  ether_ifattach (ifp);
  return 1;

 }
 
 /***********************************************************************
  *  Function:   uti596DynamicInterruptHandler
  *
  *  Description:
  *             This is the interrupt handler for the uti596 board
  *
  *  Algorithm:
  *
  ***********************************************************************/

 /* static */ rtems_isr uti596DynamicInterruptHandler(rtems_vector_number irq)
 {
#ifdef DEBUG_ISR
 printk("I");
#endif

 UTI_WAIT_COMMAND_ACCEPTED(20000, "****ERROR:on ISR entry");

 scbStatus = uti596_softc.scb.status & 0xf000;

 if ( scbStatus ){
   /* acknowledge interrupts */
   /*   printk("***INFO: ACK %x\n", scbStatus);*/
   uti596_softc.scb.command = scbStatus;
   outport_word(CHAN_ATTN, 0);
   
   if( uti596_softc.resetDone ) { 
     /* stack is attached */
     UTI_WAIT_COMMAND_ACCEPTED(20000, "****ERROR:ACK");
   }
   else {
     printk("\n***INFO:ISR won't wait\n");
     return;
   }
 }
 else {
   printk("\n***WARNING: Nothing to ACK\n");
   return;
 }
  
 if ( (scbStatus & SCB_STAT_CX) && !(scbStatus & SCB_STAT_CNA) )
   printk("\n*****ERROR:Command Complete, CNA available: %#x\n", scbStatus);
 
 if ( !(scbStatus & SCB_STAT_CX) && (scbStatus & SCB_STAT_CNA) )
   printk("\n*****ERROR: CNA went unavailable, NO CX:%#x\n",scbStatus);
 
 if ( scbStatus & SCB_STAT_RNR )
   printk("\n*****WARNING: RNR %x\n",scbStatus);
 
 /* 
  * Receive Unit Control
  */
 if ( scbStatus & SCB_STAT_FR ) { /* a frame has been received */
   uti596_softc.rxInterrupts++;
   
#ifdef DBG_FR
   printk("\nISR:FR\n");
#endif
   if ( uti596_softc.pBeginRFA == I596_NULL ||  !( uti596_softc.pBeginRFA -> stat & STAT_C ) ||
	forceResetRX ) {

     /*      if ( !forceResetRX )
       printk("****ERROR: FR interrupt and NO complete FR!\n");
     else
       printk("****INFO: RX reset requested\n");
       */
     forceResetRX = 0;
     printk("****INFO: Resetting board ( rx ) commented out\n");
     dump_scb();
     /* uti596reset(); */
   }
   else
     while ( uti596_softc.pBeginRFA != I596_NULL && 
	     ( uti596_softc.pBeginRFA -> stat & STAT_C)) {
       
#ifdef DBG_ISR
       printk("ISR:pBeginRFA != NULL\n");
#endif
       count_rx ++;
       if ( count_rx > 1)
	 printk("****WARNING: Received 2 frames on 1 interrupt \n");

       /* 
	* Give Received Frame to the ULCS 
	*/	
       uti596_softc.countRFD--;
       
       if ( uti596_softc.countRFD < 0 )
	 printk("Count < 0 !!!: count == %d, beginRFA = %p\n",
		uti596_softc.countRFD, uti596_softc.pBeginRFA);
       
       uti596_softc.stats.rx_packets++;
       pIsrRfd = uti596_softc.pBeginRFA -> next; /* the append destroys the link */
       uti596append( &uti596_softc.pInboundFrameQueue , uti596_softc.pBeginRFA );
       
       /*
	* if we have just received the a frame int he last unknown RFD,
	* then it is certain that the RFA is empty.
	*/
       if ( uti596_softc.pLastUnkRFD == uti596_softc.pBeginRFA ) {
	 UTI_596_ASSERT(uti596_softc.pLastUnkRFD != I596_NULL,"****ERROR:LastUnk is NULL, begin ptr @ end!\n");
	 uti596_softc.pEndRFA = uti596_softc.pLastUnkRFD = I596_NULL;
       }
       
#ifdef DBG_ISR
       printk("Wake %#x\n",uti596_softc.rxDaemonTid);
#endif
       sc = rtems_event_send(uti596_softc.rxDaemonTid, INTERRUPT_EVENT);
       if ( sc != RTEMS_SUCCESSFUL )
	 rtems_panic ("Can't notify rxDaemon: %s\n", 
		      rtems_status_text (sc)); 
#ifdef DBG_RAW_ISR
       else
	 printk("Rx Wake: %#x\n",uti596_softc.rxDaemonTid);
#endif
       
       uti596_softc.pBeginRFA = pIsrRfd;
     } /* end while */
   
   if ( uti596_softc.pBeginRFA == I596_NULL ){ /* adjust the pEndRFA to reflect an empty list */
     if ( uti596_softc.pLastUnkRFD == I596_NULL && uti596_softc.countRFD != 0 )
       printk("Last Unk is NULL, BeginRFA is null, and count == %d\n",uti596_softc.countRFD);
     
     uti596_softc.pEndRFA = I596_NULL;
     if ( uti596_softc.countRFD != 0 ) {
       printk("****ERROR:Count is %d, but begin ptr is NULL\n",uti596_softc.countRFD );
       dumpQ();
     }
   }
   
 } /* end scb_stat_fr */
 
 /*
  * Check For Command Complete
  */
 if (  scbStatus & SCB_STAT_CX ){
#ifdef DBG_ISR
   printk("ISR:CU\n");
#endif   
   
   pIsrCmd = uti596_softc.pCmdHead;
   
   /* 
    * For ALL completed commands
    */
   if ( pIsrCmd !=  I596_NULL && pIsrCmd->status & STAT_C && !forceResetTX  ){
     
#ifdef DBG_RAW_ISR
       printk("ISR:pIsrCmd != NULL\n");
#endif
     
     /* 
      * Adjust the drivers command block list 
      */
     uti596_softc.pCmdHead = pIsrCmd -> next;
     
     /*
      * If there are MORE commands to process, 
      * the serialization in the raw routine has failed.
      * ( Perhaps AddCmd is bad? )
      */
     UTI_596_ASSERT(uti596_softc.pCmdHead == I596_NULL,
		    "****ERROR: command serialization failed\n");
     /*
      * What if the command did not complete OK?
      */
     switch ( pIsrCmd->command & 0x7)
       {
       case CmdConfigure:

	 /*	 printk("****INFO:Configure OK\n"); */
	 uti596_softc.cmdOk = 1;
	 break;
	 
       case CmdDump:
	 
#ifdef DBG_ISR
	   printk("dump!\n");
#endif
	 uti596_softc.cmdOk = 1;
	 break;

       case CmdDiagnose:
	 
#ifdef DBG_ISR
	   printk("diagnose!\n");
#endif
	 uti596_softc.cmdOk = 1;
	 break;
	 
       case CmdSASetup:

	 /*	 printk("****INFO:Set address interrupt\n");	 */
	 if ( pIsrCmd -> status & STAT_OK )
	   uti596_softc.cmdOk = 1;
	 else
	   printk("****ERROR:SET ADD FAILED\n");
	 break;
	 
       case CmdTx:
	 {
	   UTI_596_ASSERT(uti596_softc.txDaemonTid, "****ERROR:Null txDaemonTid\n");
#ifdef DBG_ISR
	   printk("wake TX:0x%x\n",uti596_softc.txDaemonTid);
#endif
	   if ( uti596_softc.txDaemonTid ){  /* Ensure that the transmitter is waiting */
	     sc = rtems_event_send (uti596_softc.txDaemonTid, 
				    INTERRUPT_EVENT);

	   if ( sc != RTEMS_SUCCESSFUL )
	     printk("****ERROR:Could NOT send event to tid 0x%x : %s\n",
		    uti596_softc.txDaemonTid, rtems_status_text (sc) );
#ifdef DBG_RAW_ISR
	   else
	     printk("****INFO:Tx wake: %#x\n",uti596_softc.txDaemonTid);
#endif	   
	   }
	 } /* End case Cmd_Tx */
	 break;
	 
       case CmdMulticastList:

	 printk("***ERROR:Multicast?!\n");
	 pIsrCmd->next = I596_NULL;
	 break;
	 
       case CmdTDR:
	 {
	   unsigned long status = *( (unsigned long *)pIsrCmd)+1;
	   printk("****ERROR:TDR?!\n");
	   
	   if (status & STAT_C)
	     {
	       /*
		* mark the TDR command successful
		*/
	       uti596_softc.cmdOk = 1;
	     }
	   else
	     {
	       if (status & 0x4000)
		 printk("****WARNING:Transceiver problem.\n");
	       if (status & 0x2000)
		 printk("****WARNING:Termination problem.\n");
	       if (status & 0x1000)
		 printk("****WARNING:Short circuit.\n");
	       
	       /*	       printk("****INFO:Time %ld.\n", status & 0x07ff); */
	     }
	 }
	 break;
	 
       default: 
	 /* 
	  * This should never be reached 
	  */
	 printk("CX but NO known command\n");
       } /* end switch */
     pIsrCmd = uti596_softc.pCmdHead; /* next command */ 
     if ( pIsrCmd != I596_NULL )
       printk("****WARNING: more commands in list, but no start to NIC\n");
   } /* end if completed */
   else {

     /*     if ( !forceResetTX )
       printk("*****ERROR:CX but no command is completed\n");
     else
       printk("*****INFO:forcing an ethernet card reset\n");
     */
     /* Reset the ethernet card, and wake the transmitter (if necessary) */

     printk("****INFO: Resetting board ( tx )\n");
     forceResetTX = 0;
     uti596reset();
     if ( uti596_softc.txDaemonTid){  /* Ensure that a transmitter is waiting */
       /*  printk("*****INFO:Wake Transmitter\n"); */
       sc = rtems_event_send (uti596_softc.txDaemonTid, 
			      INTERRUPT_EVENT);
       
       if ( sc != RTEMS_SUCCESSFUL )
	 printk("****ERROR:Could NOT send event to tid 0x%x : %s\n",uti596_softc.txDaemonTid, rtems_status_text (sc) );
#ifdef DBG_RAW_ISR
       else
	 printk("****INFO:Tx wake: %#x\n",uti596_softc.txDaemonTid);
#endif	   
     }
   } 
 }  /* end if command complete */   
 
 
 /* if the receiver has stopped, 
  * check if this is a No Resources scenario, 
  * Try to add more RFD's ( no RBDs are used )
  */
 if ( uti596_softc.started ) {
   if ( scbStatus & SCB_STAT_RNR ){
#ifdef DBG_ISR
     printk("INFO:RNR: status %#x \n", uti596_softc.scb.status );
#endif
     /*
      * THE RECEIVER IS OFF!
      */
     if ( uti596_softc.pLastUnkRFD != I596_NULL  ){ /* We have an unknown RFD, it is not inbound*/ 
       if ( uti596_softc.pLastUnkRFD -> stat & (STAT_C | STAT_B )) /* in use */
	 uti596_softc.pEndRFA = uti596_softc.pLastUnkRFD;      /* update end */
       else {      
	 /* 
	  *  It is NOT in use, and since RNR, we know EL bit of pEndRFA was read!
	  *  So, unlink it from the RFA and move it to the saved queue.
	  *  But pBegin can equal LastUnk!
	  */
	
	   if ( uti596_softc.pEndRFA != I596_NULL ){      /* check added feb24. */
#ifdef DEBUG_RFA
	     if (uti596_softc.pEndRFA -> next != uti596_softc.pLastUnkRFD){
	       printk("***ERROR:UNK: %p not end->next: %p, end: %p\n",
		      uti596_softc.pLastUnkRFD,uti596_softc.pEndRFA -> next,uti596_softc.pEndRFA);
	       dumpQ();
	       printk("***INFO:countRFD now %d\n",	 uti596_softc.countRFD);
	       printk("\n\n");

	     }
#endif
	     uti596_softc.pEndRFA -> next = I596_NULL;   /* added feb 16 */
	   }
	 uti596append( &uti596_softc.pSavedRfdQueue, uti596_softc.pLastUnkRFD );
	 uti596_softc.savedCount++;
	 uti596_softc.pEndSavedQueue = uti596_softc.pLastUnkRFD;
	 uti596_softc.countRFD--;                    /* It was not in the RFA */
	 /* 
	  * The Begin pointer CAN advance this far. We must resynch the CPU side
	  * with the chip.
	  */
	 if ( uti596_softc.pBeginRFA == uti596_softc.pLastUnkRFD ) {
#ifdef DEBUG_RFA
	   if ( uti596_softc.countRFD != 0 ) {
	     printk("****INFO:About to set begin to NULL, with count == %d\n", uti596_softc.countRFD );
	     dumpQ();
	     printk("\n\n");
	   }
#endif
	   uti596_softc.pBeginRFA = I596_NULL;
	   UTI_596_ASSERT(uti596_softc.countRFD == 0,"****ERROR:Count must be zero here!\n");
	 }
       }

       uti596_softc.pLastUnkRFD = I596_NULL;
 
     } /* end if exists UnkRFD */

     /* 
      * Append the saved queue to  the RFA.  
      * Any further RFD's being supplied will be added to
      * this new list.
      */
     if ( uti596_softc.pSavedRfdQueue != I596_NULL ) { /* entries to add */
       if ( uti596_softc.pBeginRFA == I596_NULL ) {    /* add at beginning to list */
#ifdef DEBUG_RFA
	 if(uti596_softc.countRFD != 0) {
	   printk("****ERROR:Begin pointer is NULL, but count == %d\n",uti596_softc.countRFD);
	   dumpQ();
	 }
#endif
	 uti596_softc.pBeginRFA      = uti596_softc.pSavedRfdQueue; 
	 uti596_softc.pEndRFA        = uti596_softc.pEndSavedQueue;
	 uti596_softc.pSavedRfdQueue = uti596_softc.pEndSavedQueue = I596_NULL;  /* Reset the End */
       }
       else {  
#ifdef DEBUG_RFA
	 if ( uti596_softc.countRFD <= 0) {
	   printk("****ERROR:Begin pointer is not NULL, but count == %d\n",uti596_softc.countRFD);
	   dumpQ();
	 }
#endif
	 UTI_596_ASSERT( uti596_softc.pEndRFA != I596_NULL, "****WARNING: END RFA IS NULL\n");
	 UTI_596_ASSERT( uti596_softc.pEndRFA->next == I596_NULL, "****ERROR:END RFA -> next must be NULL\n");
	 
	 uti596_softc.pEndRFA->next   = uti596_softc.pSavedRfdQueue; 
	 uti596_softc.pEndRFA->cmd   &= ~CMD_EOL;      /* clear the end of list */ 
	 uti596_softc.pEndRFA         = uti596_softc.pEndSavedQueue;
	 uti596_softc.pSavedRfdQueue  = uti596_softc.pEndSavedQueue = I596_NULL; /* Reset the End */
#ifdef DEBUG_ISR
	 printk("count: %d, saved: %d \n", uti596_softc.countRFD , uti596_softc.savedCount);
#endif
 
       }
       /*       printk("Isr: countRFD = %d\n",uti596_softc.countRFD); */
       uti596_softc.countRFD += uti596_softc.savedCount;
       /* printk("Isr: after countRFD = %d\n",uti596_softc.countRFD); */
       uti596_softc.savedCount = 0;
     }

 
#ifdef DBG_596_RFD
     printk("The list starts here %p\n",uti596_softc.pBeginRFA );
#endif
     
     if ( uti596_softc.countRFD > 1 &&                              /* there is at least TWO */
	  !( uti596_softc.pBeginRFA -> stat & (STAT_C | STAT_B ))) { /* restart when not in use !! */
       
       UTI_596_ASSERT(uti596_softc.scb.command == 0, "****ERROR:scb command must be zero\n");
       uti596_softc.scb.pRfd = uti596_softc.pBeginRFA;
       /* start RX here  */
       printk("****INFO: ISR Starting receiver\n"); 
       uti596_softc.scb.command = RX_START; /* should this also be CU start? */
       outport_word(CHAN_ATTN, 0);
    }
     else {
       printk("****WARNING: Receiver NOT Started -- countRFD = %d\n", uti596_softc.countRFD);
       printk("82596 cmd: 0x%x, status: 0x%x RFA len: %d\n",
	      uti596_softc.scb.command, 
	      uti596_softc.scb.status,
	      uti596_softc.countRFD);
       
       printk("\nRFA: \n");
       for ( pISR_Rfd = uti596_softc.pBeginRFA;
	     pISR_Rfd != I596_NULL;
	     pISR_Rfd = pISR_Rfd->next) 
	 printk("Frame @ %p, status: %2.2x, cmd: %2.2x\n",
		pISR_Rfd, pISR_Rfd->stat, pISR_Rfd->cmd);
       
       printk("\nInbound: \n");
       for ( pISR_Rfd = uti596_softc.pInboundFrameQueue;
	     pISR_Rfd != I596_NULL;
	     pISR_Rfd = pISR_Rfd->next) 
	 printk("Frame @ %p, status: %2.2x, cmd: %2.2x\n",
		pISR_Rfd, pISR_Rfd->stat, pISR_Rfd->cmd);
       
       
       printk("\nSaved: \n");
       for ( pISR_Rfd = uti596_softc.pSavedRfdQueue;
	     pISR_Rfd != I596_NULL;
	     pISR_Rfd = pISR_Rfd->next) 
	 printk("Frame @ %p, status: %2.2x, cmd: %2.2x\n",
		pISR_Rfd, pISR_Rfd->stat, pISR_Rfd->cmd);
       printk("\nUnknown: %p\n",uti596_softc.pLastUnkRFD);
     }
   } /* end stat_rnr */	 

 } /* end if receiver started */
 UTI_596_ASSERT(uti596_softc.scb.status == scbStatus, "****WARNING:scbStatus change!\n");
 
#ifdef DBG_ISR
   printk("X\n");
#endif
 count_rx=0;
 return;
 }


/***********************************************************************
 *  Function:   void uti596dequeue
 *
 *  Description:
 *              removes an RFD from the received frame queue,
 *
 *  Algorithm:
 *
 ***********************************************************************/

struct i596_rfd * uti596dequeue( struct i596_rfd ** ppQ )
 {
   ISR_Level level;

   struct i596_rfd * pRfd;
   _ISR_Disable(level);
   
   /* invalid address, or empty queue or emptied queue */
   if( ppQ == NULL || *ppQ == NULL || *ppQ == I596_NULL) {
     _ISR_Enable(level);
     return I596_NULL;
   }
   
     pRfd = *ppQ;            /* The dequeued buffer           */
     *ppQ = pRfd->next;      /* advance the queue pointer     */
     pRfd->next = I596_NULL; /* unlink the rfd being returned */


   _ISR_Enable(level);
   return pRfd;
}	

/***********************************************************************
 *  Function:   void uti596append
 *
 *  Description:
 *              adds an RFD to the end of the received frame queue,
 *              for processing by the rxproc.
 *              Also removes this RFD from the RFA 
 *
 *  Algorithm:
 *
 ***********************************************************************/

void uti596append( struct i596_rfd ** ppQ , struct i596_rfd * pRfd ) 
{

  struct i596_rfd *p;

  if ( pRfd != NULL && pRfd != I596_NULL) {
    pRfd -> next = I596_NULL;
    pRfd -> cmd |= CMD_EOL;    /* set EL bit */
    
    if ( *ppQ == NULL || *ppQ == I596_NULL ) {
      /* Empty or emptied */
      *ppQ = pRfd;
    }
    else
      {
	for ( p=*ppQ; p -> next != I596_NULL; p=p->next)
	  ;
	
	p->cmd &= ~CMD_EOL; /* Clear EL bit at end */
	p->next = pRfd;
      }
  }
  else
    printf("Illegal attempt to append: %p\n", pRfd);
}


/***********************************************************************
 *  Function:   uti596stop
 *
 *  Description:
 *             stop the driver
 *
 *  Algorithm:
 *             mark driver as not started,
 *             mark transmitter as busy
 *             abort any transmissions/receptions
 *             clean-up all buffers ( RFD's et. al. )
 *             
 *             
 * 
 *
 ***********************************************************************/


/* static */ 
void uti596_stop(struct uti596_softc *sc)
{
    sc->started  = 0;

#ifdef DBG_596
	printf("%s: Shutting down ethercard, status was %4.4x.\n",
	       uti596_softc.pIface->name, uti596_softc.scb.status);
#endif

    printf("Stopping interface\n");
    sc->scb.command = CUC_ABORT|RX_ABORT;
    outport_word( CHAN_ATTN, 0 );

    
}


static int
uti596_ioctl (struct ifnet *ifp, int command, caddr_t data)
{
	struct uti596_softc *sc = ifp->if_softc;
	int error = 0;

	switch (command) {
	case SIOCGIFADDR:
	case SIOCSIFADDR:
	        printk("SIOCSIFADDR\n");
		ether_ioctl (ifp, command, data);
		break;

	case SIOCSIFFLAGS:
	        printk("SIOCSIFFLAGS\n");
		switch (ifp->if_flags & (IFF_UP | IFF_RUNNING)) {
		case IFF_RUNNING:
	        printk("IFF_RUNNING\n");
			uti596_stop (sc);
			break;

		case IFF_UP:
	        printk("IFF_UP\n");
			uti596_init (sc);
			break;

		case IFF_UP | IFF_RUNNING:
	        printk("IFF_UP and RUNNING\n");
			uti596_stop (sc);
			uti596_init (sc);
			break;

		default:
	        printk("default\n");

			break;
		}
		break;

	case SIO_RTEMS_SHOW_STATS:
	        printk("show stats\n");
		uti596_stats (sc);
		break;
		
	/*
	 * FIXME: All sorts of multicast commands need to be added here!
	 */
	default:
	        printk("default: EINVAL\n");
		error = EINVAL;
		break;
	}
	return error;
}


/***********************************************************************
 *  Function:   uti596_stats
 *
 *  Description:
 *             print out the collected data
 *
 *  Algorithm:
 *            use printf
 *
 ***********************************************************************/


void 
uti596_stats(struct uti596_softc *sc)
      {
        printf(" CPU Reports:\n");
	printf ("Tx raw send count:%-8lu",  sc->rawsndcnt);
	printf ("Rx Interrupts:%-8lu",  sc->rxInterrupts);
	printf ("Tx Interrupts:%-8lu\n",  sc->txInterrupts);
	printf ("Rx Packets:%-8u",  sc->stats.rx_packets);
	printf ("Tx Attempts:%-u\n",  sc->stats.tx_packets);
	
	printf ("Rx Dropped:%-8u",     sc->stats.rx_dropped);
	printf ("Rx IP Packets:%-8u",  sc->stats.rx_packets);
	printf ("Tx Errors:%-8u\n",      sc->stats.tx_errors);
	printf ("Tx aborted:%-8u",     sc->stats.tx_aborted_errors);
	printf ("Tx Dropped:%-8u\n",     sc->stats.tx_dropped);
	printf ("Tx IP packets:%-8u",  sc->stats.tx_packets);
	
	printf ("Collisions Detected:%-8u\n",  sc->stats.collisions);
	printf ("Tx Heartbeat Errors:%-8u",  sc->stats.tx_heartbeat_errors);
	printf ("Tx Carrier Errors:%-8u\n",    sc->stats.tx_carrier_errors);
	printf ("Tx Aborted Errors:%-8u",    sc->stats.tx_aborted_errors);
	printf ("Rx Length Errors:%-8u\n",     sc->stats.rx_length_errors);
	printf ("Rx Overrun Errors:%-8u",    sc->stats.rx_over_errors);
	printf ("Rx Fifo Errors:%-8u\n",       sc->stats.rx_fifo_errors);
	printf ("Rx Framing Errors:%-8u",    sc->stats.rx_frame_errors);
	printf ("Rx crc errors:%-8u\n",        sc->stats.rx_crc_errors);
	printf ("TX WAITS: %-8lu\n", sc->txRawWait);

	printf("NIC reports\n");
	dump_scb();
      }

void dumpQ(void) {

  struct i596_rfd *pRfd;

	   printf("savedQ:\n");
	   for( pRfd = uti596_softc.pSavedRfdQueue;
		pRfd != I596_NULL;
		pRfd = pRfd -> next) 
	     printf("pRfd: %p, stat: 0x%x cmd: 0x%x\n",pRfd,pRfd -> stat,pRfd -> cmd);
	   printf("Inbound:\n");
	   for( pRfd = uti596_softc.pInboundFrameQueue;
		pRfd != I596_NULL;
		pRfd = pRfd -> next) 
	     printf("pRfd: %p, stat: 0x%x cmd: 0x%x\n",pRfd,pRfd -> stat,pRfd -> cmd);
	    printf("Last Unk: %p\n", uti596_softc.pLastUnkRFD );
	 
	   printf("RFA:\n");
	   for( pRfd = uti596_softc.pBeginRFA;
		pRfd != I596_NULL;
		pRfd = pRfd -> next) 
	     printf("pRfd: %p, stat: 0x%x cmd: 0x%x\n",pRfd,pRfd -> stat,pRfd -> cmd);
}

void uti596Diagnose(int verbose){
  struct i596_cmd diagnose;
  int count=10000;

  diagnose.command = CmdDiagnose;
  diagnose.status = 0;
  uti596addPolledCmd(&diagnose);
  while( !( diagnose.status & STAT_C ) && count ) {
    if(verbose)
      printf(".");
    count --;
  }
  if(verbose)
    printf("Status diagnostic: 0x%2.2x\n", diagnose.status);

}
void show_buffers (void) 
{
    struct i596_rfd *pRfd;

      printf("82596 cmd: 0x%x, status: 0x%x RFA len: %d\n",
	     uti596_softc.scb.command, 
	     uti596_softc.scb.status,
	     uti596_softc.countRFD);
 
      printf("\nRFA: \n");
      for ( pRfd = uti596_softc.pBeginRFA;
	    pRfd != I596_NULL;
	    pRfd = pRfd->next) 
	printf("Frame @ %p, status: %2.2x, cmd: %2.2x\n",
	       pRfd, pRfd->stat, pRfd->cmd);
      
      printf("\nInbound: \n");
      for ( pRfd = uti596_softc.pInboundFrameQueue;
	    pRfd != I596_NULL;
	    pRfd = pRfd->next) 
	printf("Frame @ %p, status: %2.2x, cmd: %2.2x\n",
	       pRfd, pRfd->stat, pRfd->cmd);
      

      printf("\nSaved: \n");
      for ( pRfd = uti596_softc.pSavedRfdQueue;
	    pRfd != I596_NULL;
	    pRfd = pRfd->next) 
	printf("Frame @ %p, status: %2.2x, cmd: %2.2x\n",
	       pRfd, pRfd->stat, pRfd->cmd);
    printf("\nUnknown: %p\n",uti596_softc.pLastUnkRFD);

}
void show_queues(void)
{
    struct i596_rfd *pRfd;


      printf("CMD: 0x%x, Status: 0x%x\n", 
	     uti596_softc.scb.command,
	     uti596_softc.scb.status);
      printf("saved Q\n");
      
      for ( pRfd = uti596_softc.pSavedRfdQueue;
	    pRfd != I596_NULL &&
	      pRfd != NULL;
	    pRfd = pRfd->next)
	printf("0x%p\n", pRfd);
      
      printf("End saved Q 0x%p\n", uti596_softc.pEndSavedQueue);
      
      printf("\nRFA:\n");
      for ( pRfd = uti596_softc.pBeginRFA;
	    pRfd != I596_NULL &&
	      pRfd != NULL;
	    pRfd = pRfd->next)
	printf("0x%p\n", pRfd);
      
      printf("uti596_softc.pEndRFA: %p\n",uti596_softc.pEndRFA);
}


void uti596_init(void * arg){

  struct uti596_softc  *sc = arg;
  struct ifnet *ifp = &sc->arpcom.ac_if;

  if (sc->txDaemonTid == 0) {

    uti596_initialize_hardware(sc);

    /*
     * Start driver tasks
     */

    sc->txDaemonTid = rtems_bsdnet_newproc ("UTtx", 4096, uti596_txDaemon, sc);
    sc->rxDaemonTid = rtems_bsdnet_newproc ("UTrx", 4096, uti596_rxDaemon, sc);
 
  }
  /* 
   * Enable receiver
   */
  sc->scb.pRfd = sc -> pBeginRFA;
  sc->scb.command = RX_START;
  outport_word(CHAN_ATTN,0 ); 
  UTI_WAIT_COMMAND_ACCEPTED(4000, "init:RX_START"); 
  /*
   * Tell the world that we're running.
   */
  ifp->if_flags |= IFF_RUNNING;
  
}
void dump_scb(){
  printk("status 0x%x\n",uti596_softc.scb.status);
  printk("command 0x%x\n",uti596_softc.scb.command);
  printk("cmd 0x%x\n",(int)uti596_softc.scb.pCmd);
  printk("rfd 0x%x\n",(int)uti596_softc.scb.pRfd);  
  printk("crc_err 0x%x\n",uti596_softc.scb.crc_err);
  printk("align_err 0x%x\n",uti596_softc.scb.align_err);
  printk("resource_err 0x%x\n",uti596_softc.scb.resource_err );
  printk("over_err 0x%x\n",uti596_softc.scb.over_err);
  printk("rcvdt_err 0x%x\n",uti596_softc.scb.rcvdt_err);
  printk("short_err 0x%x\n",uti596_softc.scb.short_err);
  printk("t_on 0x%x\n",uti596_softc.scb.t_on);
  printk("t_off 0x%x\n",uti596_softc.scb.t_off);
}

