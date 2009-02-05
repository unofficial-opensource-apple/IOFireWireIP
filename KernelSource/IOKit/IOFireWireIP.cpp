/*
 * Copyright (c) 1998-2001 Apple Computer, Inc. All rights reserved.
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
#include "IOFireWireIP.h"
#include "IOFireWireIPCommand.h"
#include "IOFWAsyncStreamRxCommand.h"

extern "C"
{
void _logMbuf(struct mbuf * m);
void _logPkt(void *pkt, UInt16 len);
extern int  in6_cksum(struct mbuf *, __uint8_t, __uint32_t, __uint32_t);
}

#define super IOFWController

OSDefineMetaClassAndStructors(IOFireWireIP, IOFWController);

#pragma mark -
#pragma mark ��� IOService methods ���

bool IOFireWireIP::start(IOService *provider)
{    
    IOReturn	ioStat = kIOReturnSuccess;
	
	if(fStarted)
		return fStarted;
		
	fMaxPktSize = 0;
	netifEnabled = false;
	busifEnabled = false;
	fDoFastRetry = false;

    fDevice = OSDynamicCast(IOFireWireNub, provider);

    if(!fDevice)
        return false;

    fControl = fDevice->getController(); 

	if(!fControl)
		return false;

	fControl->retain();
	
    // Initialize the LCB
    fLcb = (LCB*)IOMalloc(sizeof(LCB));
    if(fLcb == NULL)
        return false;
    
    memset(fLcb, 0, sizeof(LCB));

	fDiagnostics_Symbol = OSSymbol::withCStringNoCopy("Diagnostics");
	
	fDiagnostics = IOFireWireIPDiagnostics::createDiagnostics(this);
	if( fDiagnostics )
	{
		if(fDiagnostics_Symbol)
			setProperty( fDiagnostics_Symbol, fDiagnostics );
		fDiagnostics->release();
	}

	if(ioStat == kIOReturnSuccess) {
	    
		CSRNodeUniqueID	fwuid = fDevice->getUniqueID();
		
		// Construct the ethernet address
		makeEthernetAddress(&fwuid, macAddr, GUID_TYPE);
	}

	// IONetworkingFamily attachments
    if (!super::start(provider))
        return false;
	
    if (getHardwareAddress(&myAddress) != kIOReturnSuccess)
    {	
        return false;
    }

    if(!createMediumState())
	{
		IOLog( "IOFireWireIP::start - Couldn't allocate IONetworkMedium\n" );
		return false;
	}
	
    if (!attachInterface((IONetworkInterface**)&networkInterface, false ))
    {	
        return false;
    }

	fPrivateInterface	= NULL;

	networkInterface->setIfnetMTU( 1 << fDevice->maxPackLog(true) );

	transmitQueue = (IOGatedOutputQueue*)getOutputQueue();
    if ( !transmitQueue ) 
    {
        IOLog( "IOFireWireIP::start - Output queue initialization failed\n" );
        return false;
    }
    transmitQueue->retain();

	// Create the lock
	if(ioStat == kIOReturnSuccess) 
	{
		// Allocate lock
		ipLock = IORecursiveLockAlloc();
		
		if(ipLock == NULL)
			ioStat = kIOReturnNoMemory;
	}
	
	if(ioStat == kIOReturnSuccess) 
	{
		// Add unit notification for units disappearing
		fIPUnitNotifier = IOService::addNotification(gIOPublishNotification, 
													serviceMatching("IOFireWireIPUnit"), 
													&fwIPUnitAttach, this, (void*)IP1394_VERSION, 0);
	}

	if(ioStat == kIOReturnSuccess) 
	{
		// Add unit notification for units disappearing
		fIPv6UnitNotifier = IOService::addNotification(gIOPublishNotification, 
													serviceMatching("IOFireWireIPUnit"), 
													&fwIPUnitAttach, this, (void*)IP1394v6_VERSION, 0);
	}

	// Create config rom entry
	if(ioStat == kIOReturnSuccess)
		ioStat = createIPConfigRomEntry();

	if(ioStat == kIOReturnSuccess) 
	{
		fDevice->getNodeIDGeneration(fLcb->busGeneration, fLcb->ownNodeID); 
		fLcb->ownMaxSpeed = fDevice->FWSpeed();
		fLcb->maxBroadcastPayload = fDevice->maxPackLog(true);
		fLcb->maxBroadcastSpeed = fDevice->FWSpeed();
		fLcb->ownMaxPayload = fDevice->maxPackLog(true);

		IP1394_HDW_ADDR	hwAddr;
		
		CSRNodeUniqueID	fwuid = fDevice->getUniqueID();

		memset(&hwAddr, 0, sizeof(IP1394_HDW_ADDR));
		hwAddr.eui64.hi = OSSwapHostToBigInt32((UInt32)(fwuid >> 32));
		hwAddr.eui64.lo = OSSwapHostToBigInt32((UInt32)(fwuid & 0xffffffff));
		
		hwAddr.maxRec	= fControl->getMaxRec();              
		hwAddr.spd		= fDevice->FWSpeed();

		hwAddr.unicastFifoHi = kUnicastHi;      
		hwAddr.unicastFifoLo = kUnicastLo;
		
		memcpy(&fLcb->ownHardwareAddress, &hwAddr, sizeof(IP1394_HDW_ADDR)); 
		
		UInt32 size = getMaxARDMAPacketSize();
		
		if(size > 0)
			fLcb->ownHardwareAddress.maxRec = getMaxARDMARec(size);			
		
		setProperty(kIOFWHWAddr,  (void *)&fLcb->ownHardwareAddress, sizeof(IP1394_HDW_ADDR));
	}
	
	if(ioStat != kIOReturnSuccess)
	{
		IOLog( "IOFireWireIP::start - failed\n" );
		return false;
	}

    networkInterface->registerService();

	fStarted = true;

    return true;
} // end start

bool IOFireWireIP::finalize(IOOptionBits options)
{	
	return super::finalize(options);
}

void IOFireWireIP::stop(IOService *provider)
{
	if(fDiagnostics_Symbol != NULL)
		fDiagnostics_Symbol->release();		
	
	fDiagnostics_Symbol = 0;

    // Free the firewire stuff
    if (fLocalIP1394v6ConfigDirectory != NULL)
	{
        // clear the unit directory in config rom
        fDevice->getBus()->RemoveUnitDirectory(fLocalIP1394v6ConfigDirectory) ;
		fLocalIP1394v6ConfigDirectory->release();
	} 
	fLocalIP1394v6ConfigDirectory = NULL;

    // Free the firewire stuff
    if (fLocalIP1394ConfigDirectory != NULL)
	{
        // clear the unit directory in config rom
        fDevice->getBus()->RemoveUnitDirectory(fLocalIP1394ConfigDirectory) ;
		fLocalIP1394ConfigDirectory->release();
	} 
	
	fLocalIP1394ConfigDirectory = NULL;

	if(fwOwnAddr != NULL)
		fwOwnAddr->release();
	
	fwOwnAddr = NULL;
	
	if (transmitQueue != NULL)
		transmitQueue->release();
	
	transmitQueue = NULL;
	
	if(fIPv6UnitNotifier != NULL)
		fIPv6UnitNotifier->remove();
	
	fIPv6UnitNotifier = NULL;
	
	// Remove IOFireWireIPUnit notification
	if(fIPUnitNotifier != NULL)
		fIPUnitNotifier->remove();
	
	fIPUnitNotifier = NULL;
	
    if(fLcb != NULL)
        IOFree(fLcb, sizeof(LCB));
		
	fLcb = NULL;
	
	if (networkInterface != NULL)
		networkInterface->release();
		
	networkInterface = NULL;

	if(fControl != NULL)
		fControl->release();

	fControl = NULL;

	super::stop(provider);
}

void IOFireWireIP::free(void)
{
    if (ipLock != NULL) 
        IORecursiveLockFree(ipLock);
		
	ipLock = NULL;

	return super::free();
}

IOReturn IOFireWireIP::message(UInt32 type, IOService *provider, void *argument)
{
    IOReturn res = kIOReturnUnsupported;

    switch (type)
    {                
        case kIOMessageServiceIsTerminated:
        case kIOMessageServiceIsSuspended:
        case kIOMessageServiceIsResumed:
        case kIOMessageServiceIsRequestingClose:
			if(busifEnabled)
				messageClients(type, this);
            res = kIOReturnSuccess;
            break;

        default:
            break;
    }
	
    return res;
}

#pragma mark -
#pragma mark ��� IOFWController methods ���

IOReturn IOFireWireIP::setMaxPacketSize(UInt32 maxSize)
{
	if (maxSize > kIOFWMaxPacketSize)
		return kIOReturnError;
	
    return kIOReturnSuccess;
}

IOReturn IOFireWireIP::getMaxPacketSize(UInt32 * maxSize) const
{
	*maxSize = kIOFWMaxPacketSize;

	return kIOReturnSuccess;
}

IOReturn IOFireWireIP::getHardwareAddress(IOFWAddress *ea)
{
	memcpy(ea->bytes, macAddr, FIREWIRE_ADDR_LEN);

    return kIOReturnSuccess;
} // end getHardwareAddress


bool IOFireWireIP::configureInterface( IONetworkInterface *netif )
{
    IONetworkData	 *nd;

    if ( super::configureInterface( netif ) == false )
        return false;

	/* Grab a pointer to the statistics structure in the interface:	*/
	nd = netif->getNetworkData( kIONetworkStatsKey );
    if (!nd || !(fpNetStats = (IONetworkStats *) nd->getBuffer()))
    {
        IOLog("IOFireWireIP: invalid network statistics\n");
        return false;
    }

	// Get the Ethernet statistics structure:
	nd = netif->getParameter( kIOFWStatsKey );
	if ( !nd || !(fpEtherStats = (IOFWStats*)nd->getBuffer()) )
	{
		IOLog( "IOFireWireIP::configureInterface - invalid ethernet statistics\n" );
        return false;
	}

    /*
     * Set the driver/stack reentrancy flag. This is meant to reduce
     * context switches. May become irrelevant in the future.
     */
    return true;
	
} // end configureInterface

void IOFireWireIP::receivePackets(void * pkt, UInt32 pkt_len, UInt32 options)
{
	if(options == true)
		fPacketsQueued = true;

    IORecursiveLockLock(ipLock);
		
	networkInterface->inputPacket((struct mbuf*)pkt, pkt_len, options);
	networkStatAdd(&fpNetStats->inputPackets);
	
    IORecursiveLockUnlock(ipLock);
}

IOOutputQueue* IOFireWireIP::createOutputQueue()
{
	return IOGatedOutputQueue::withTarget(this, getWorkLoop(), TRANSMIT_QUEUE_SIZE);
}

/*-------------------------------------------------------------------------
 * Override IONetworkController::createWorkLoop() method and create
 * a workloop.
 *-------------------------------------------------------------------------*/
bool IOFireWireIP::createWorkLoop()
{
	workLoop = fDevice->getController()->getWorkLoop();
	
    return ( workLoop != 0 );
} // end createWorkLoop


// Override IOService::getWorkLoop() method to return our workloop.
IOWorkLoop* IOFireWireIP::getWorkLoop() const
{
    return workLoop;
} // end getWorkLoop 

IOOutputAction IOFireWireIP::getOutputHandler() const
{
    return (IOOutputAction) &IOFireWireIP::transmitPacket;
}

bool IOFireWireIP::arpCacheHandler(IP1394_ARP *fwa)
{
    IORecursiveLockLock(ipLock);

	if(not busifEnabled)
	{
	    IORecursiveLockUnlock(ipLock);
		return false;
	}

	bool ret = (*fUpdateARPCache)(fPrivateInterface, fwa);

    IORecursiveLockUnlock(ipLock);
	
	return ret;
}

UInt32 IOFireWireIP::transmitPacket(mbuf_t m, void * param)
{
    IORecursiveLockLock(ipLock);

	if(not busifEnabled)
	{
		freePacket((struct mbuf*)m);
	    IORecursiveLockUnlock(ipLock);
		return kIOReturnOutputDropped;
	}

	IOReturn status = (*fOutAction)(m, (void*)fPrivateInterface);

    IORecursiveLockUnlock(ipLock);

	return status;
}

UInt32 IOFireWireIP::outputPacket(mbuf_t pkt, void * param)
{
	IOReturn status = kIOReturnOutputDropped;

	// Its just a sink, until we get a valid unit on the bus.
	((IOFireWireIP*)param)->freePacket((struct mbuf*)pkt);
	
	return status;
}

/*-------------------------------------------------------------------------
 * Called by IOEthernetInterface client to enable the controller.
 * This method is always called while running on the default workloop
 * thread.
 *-------------------------------------------------------------------------*/
IOReturn IOFireWireIP::enable(IONetworkInterface * netif)
{
	// IOLog("IOFireWireIP: enable \n");
    /*
     * If an interface client has previously enabled us,
     * and we know there can only be one interface client
     * for this driver, then simply return true.
     */
    if (netifEnabled)
    {
        IOLog("IOFireWireIP: already enabled\n");
        return kIOReturnSuccess;
    }
	
    /*
     * Mark the controller as enabled by the interface.
     */
    netifEnabled = true;

	/*
     * Start our IOOutputQueue object.
     */
    transmitQueue->setCapacity( TRANSMIT_QUEUE_SIZE );
    transmitQueue->start();
	
    return kIOReturnSuccess;
	
}// end enable netif


/*-------------------------------------------------------------------------
 * Called by IOEthernetInterface client to disable the controller.
 * This method is always called while running on the default workloop
 * thread.
 *-------------------------------------------------------------------------*/
IOReturn IOFireWireIP::disable(IONetworkInterface * /*netif*/)
{
    netifEnabled = false;

	/*
     * Disable our IOOutputQueue object. This will prevent the
     * outputPacket() method from being called.
     */
    transmitQueue->stop();

    /*
     * Flush all packets currently in the output queue.
     */
    transmitQueue->setCapacity( 0 );
    transmitQueue->flush();
	
    return kIOReturnSuccess;

}// end disable netif


IOReturn IOFireWireIP::getPacketFilters( const OSSymbol	*group, UInt32 *filters ) const
{
	//IOLog("IOFireWireIP: getPacketFilters \n");

	return super::getPacketFilters( group, filters );

}// end getPacketFilters


IOReturn IOFireWireIP::setWakeOnMagicPacket( bool active )
{
	//IOLog("IOFireWireIP: setWakeOnMagicPacket \n");

	return kIOReturnSuccess;

}// end setWakeOnMagicPacket

const OSString * IOFireWireIP::newVendorString() const
{
	//IOLog("IOFireWireIP: newVendorString \n");
	
    return OSString::withCString("Apple");
}

const OSString * IOFireWireIP::newModelString() const
{
	//IOLog("IOFireWireIP: newModelString \n");

    return OSString::withCString("fw+");
}

const OSString * IOFireWireIP::newRevisionString() const
{
	//IOLog("IOFireWireIP: newRevisionString \n");
	
    return OSString::withCString("");

}

IOReturn IOFireWireIP::setPromiscuousMode( bool active )
{

	//IOLog("IOFireWireIP: setPromiscuousMode \n");

	isPromiscuous	= active;

	return kIOReturnSuccess;

} // end setPromiscuousMode

IOReturn IOFireWireIP::setMulticastMode( bool active )
{
	//IOLog("IOFireWireIP: setMulticastMode \n");
	
	multicastEnabled = active;

	return kIOReturnSuccess;
}/* end setMulticastMode */

IOReturn IOFireWireIP::setMulticastList(IOFWAddress *addrs, UInt32 count)
{
    return kIOReturnSuccess;
}

/*!
	@function createMediumState
	@abstract 
	@param none.
	@result create a supported medium information and 
			attach to the IONetworkingFamily.
*/
bool IOFireWireIP::createMediumState()
{
	OSDictionary * mediumDict = OSDictionary::withCapacity(5);
	if (!mediumDict) 
	{
		return false;
	}
	
	IONetworkMedium * medium = IONetworkMedium::medium(
								(IOMediumType)kIOMediumEthernetAuto | 
								kIOMediumOptionFullDuplex, fLcb->maxBroadcastSpeed);
	if (medium) 
	{
		mediumDict->setObject(medium->getKey(), medium);
		setCurrentMedium(medium);
		medium->release();
	}
			
	if (!publishMediumDictionary(mediumDict)) 
	{
		return false;
	}
	
	if( mediumDict )
	{
		mediumDict->release();
	}

	// Link status is Valid and inactive
	return setLinkStatus( kIONetworkLinkValid, getCurrentMedium(), 0 ); 
}

#pragma mark -
#pragma mark ��� IOFirewireIP methods ���


/*!
	@function getBytesFromGUID
	@abstract constructs byte array from the GUID.
	@param fwuid - GUID of the node.
	@param bufAddr - pointer to the buffer.
	@result void.
*/
void IOFireWireIP::getBytesFromGUID(void *guid, u_char *bufAddr, UInt8 type)
{
	u_long lo=0, hi=0;

	if(type == GUID_TYPE)
	{
		CSRNodeUniqueID	*fwuid = (CSRNodeUniqueID*)guid;
	    hi	=	(u_long)(*fwuid >> 32);
		lo	=	(u_long)(*fwuid & 0xffffffff);
	}
	else
	{
		UWIDE *eui64 = (UWIDE*)guid;
		hi	=	eui64->hi;
		lo	=	eui64->lo;
	}

	bufAddr[0] = (unsigned char)((hi >> 24) & 0x000000ff);
	bufAddr[1] = (unsigned char)((hi >> 16) & 0x000000ff);
    bufAddr[2] = (unsigned char)((hi >> 8) & 0x000000ff);
    bufAddr[3] = (unsigned char)((hi) & 0x000000ff);
	
	bufAddr[4] = (unsigned char)((lo >> 24) & 0x000000ff);
	bufAddr[5] = (unsigned char)((lo >> 16) & 0x000000ff);
	bufAddr[6] = (unsigned char)((lo >> 8) & 0x000000ff);
    bufAddr[7] = (unsigned char)((lo) & 0x000000ff);
}

/*!
	@function makeEthernetAddress
	@abstract constructs mac address from the GUID.
	@param fwuid - GUID of the node.
	@param bufAddr - pointer to the buffer.
	@param vendorID - vendorID.
	@result void.
*/
void IOFireWireIP::makeEthernetAddress(CSRNodeUniqueID	*fwuid, u_char *bufAddr, UInt32 vendorID)
{
	getBytesFromGUID(fwuid, bufAddr, GUID_TYPE);
}

void IOFireWireIP::updateMTU(UInt32 mtu)
{
	networkInterface->setIfnetMTU( mtu );
}

UInt32	IOFireWireIP::getMaxARDMAPacketSize()
{
	UInt32	maxARDMASize = 0;
	
	OSObject *regProperty = fControl->getProperty("FWARDMAMax", gIOServicePlane);
	
	if(regProperty != NULL)
	{
		maxARDMASize = ((OSNumber*)regProperty)->unsigned32BitValue();
		maxARDMASize -= sizeof(IP1394_UNFRAG_HDR);
	}
		
	return maxARDMASize;
}

UInt8	IOFireWireIP::getMaxARDMARec(UInt32 size)
{
    UInt8 maxRecLog	 = 1;
	
    while( (size >= 8) && (maxRecLog < 15) )
    {
        size >>= 1;
        maxRecLog++;
    }
	
	return maxRecLog;
}

void IOFireWireIP::registerFWIPPrivateHandlers(IOFireWireIPPrivateHandlers *privateSelf)
{
	if(busifEnabled)
		return;

    IORecursiveLockLock(ipLock);

	fPrivateInterface	= privateSelf->newService;
	fOutAction			= privateSelf->transmitPacket;
	fUpdateARPCache		= privateSelf->updateARPCache;

	busifEnabled		= true;
	
    IORecursiveLockUnlock(ipLock);
}

void IOFireWireIP::deRegisterFWIPPrivateHandlers()
{
	if ( not busifEnabled)
		return;
	
    IORecursiveLockLock(ipLock);
	
	// Last unit is going away
	busifEnabled		= false;
	fPrivateInterface	= NULL;
	fOutAction			= NULL;
	fUpdateARPCache		= NULL;

    IORecursiveLockUnlock(ipLock);
}

/*!
	@function fwIPUnitAttach
	@abstract Callback for a Unit attached of type IP1394
	@param target - callback data.
	@param refcon - callback data.
	@param newService - handle to the new IP1394 unit created.
	@result bool.
*/
bool IOFireWireIP::fwIPUnitAttach(void * target, void * refCon, IOService * newService)
{
	if(target == NULL || newService == NULL)
		return false;

    IOFireWireIP	*fwIPObject = OSDynamicCast(IOFireWireIP, (IOService *)target);
    if ( not fwIPObject )
        return false;

    // Create the device reference block for this IP device
    IOFireWireNub	*fDevice = OSDynamicCast(IOFireWireNub, newService->getProvider());
    if ( not fDevice )
        return false;

	// mismatch of new controller and localnode controller results in false
	if(fwIPObject->fControl != fDevice->getController())
		return false;	

    return true;
}

/*!
	@function createIPConfigRomEntry
	@abstract creates the config rom entry for IP over Firewire.
	@param 	none
	@result IOReturn - kIOReturnSuccess or error if failure.
*/
IOReturn IOFireWireIP::createIPConfigRomEntry()
{
	IOReturn ioStat = kIOReturnSuccess;
    // Create entries for UnitSpecID and UnitSwVersion
    fLocalIP1394ConfigDirectory = IOLocalConfigDirectory::create();

    if (!fLocalIP1394ConfigDirectory){
        ioStat =  kIOReturnError;
    } 
	
    if(ioStat == kIOReturnSuccess)
        ioStat = fLocalIP1394ConfigDirectory->addEntry(kConfigUnitSpecIdKey, IP1394_SPEC_ID) ;

    if(ioStat == kIOReturnSuccess)
        ioStat = fLocalIP1394ConfigDirectory->addEntry(kConfigUnitSwVersionKey, IP1394_VERSION) ;
		
    // lets publish it
    if(ioStat == kIOReturnSuccess)
        ioStat = fControl->AddUnitDirectory(fLocalIP1394ConfigDirectory) ;

    // Create entries for IPv6 UnitSpecID and UnitSwVersion
    fLocalIP1394v6ConfigDirectory = IOLocalConfigDirectory::create();

    if (!fLocalIP1394v6ConfigDirectory){
        ioStat =  kIOReturnError;
    } 

	if(ioStat == kIOReturnSuccess)
		ioStat = fLocalIP1394v6ConfigDirectory->addEntry(kConfigUnitSpecIdKey, IP1394_SPEC_ID) ;
	
	if(ioStat == kIOReturnSuccess)
		ioStat = fLocalIP1394v6ConfigDirectory->addEntry(kConfigUnitSwVersionKey, IP1394v6_VERSION) ;

    if(ioStat == kIOReturnSuccess)
        ioStat = fControl->AddUnitDirectory(fLocalIP1394v6ConfigDirectory) ;

	return ioStat;
}

#ifdef DEBUG

void IOFireWireIP::showIPStatus()
{
	IOLog("	txB %d, rxB %d, txU %d, rxUni %d, rxF %d txF %d qSt %d, qStallCt %d, qRetryCt %d, qGetSize %d, \
				acCmds %d, noCmds %d, noBCmds %d, inacCmds %d, dc %d, subErr %d, callErr %d, \
				stall %d, noRes %d, activeMbufs %d inActiveMbufs %d \
				\n",
		fTxBcast,
		fRxBcast,fTxUni,fRxUni, 
		fRxFragmentPkts,fTxFragmentPkts, transmitQueue->getState(),
		transmitQueue->getStallCount(), transmitQueue->getRetryCount(), 
		transmitQueue->getSize(), 
		fActiveCmds, fNoCommands,fNoBCastCommands, 
		fInActiveCmds, fDoubleCompletes, 
		fSubmitErrs, fCallErrs,  
		fNoResources, activeMbufs, inActiveMbufs
		);
}

// Used for debugging only. Log the mbuf fields.
void _logMbuf(struct mbuf * m)
{
	UInt8	*bytePtr;
	
    if (!m) {
        IOLog("logMbuf: NULL mbuf\n");
        return;
    }
    
    while (m) {
        IOLog("m_next   : %08x\n", (UInt) m->m_next);
        IOLog("m_nextpkt: %08x\n", (UInt) m->m_nextpkt);
        IOLog("m_len    : %d\n",   (UInt) m->m_len);
        IOLog("m_data   : %08x\n", (UInt) m->m_data);
        IOLog("m_type   : %08x\n", (UInt) m->m_type);
        IOLog("m_flags  : %08x\n", (UInt) m->m_flags);
        
        if (m->m_flags & M_PKTHDR)
            IOLog("m_pkthdr.len  : %d\n", (UInt) m->m_pkthdr.len);

        if (m->m_flags & M_EXT) {
            IOLog("m_ext.ext_buf : %08x\n", (UInt) m->m_ext.ext_buf);
            IOLog("m_ext.ext_size: %d\n", (UInt) m->m_ext.ext_size);
        }
		
		IOLog("m_data -> \t\t") ;
		
		if(m->m_data != NULL){
		
			bytePtr = (UInt8*)m->m_data;
						
			for(SInt32 index=0; index < min(m->m_len, 12); index++)
			{
				if ((index & 0x3) == 0)
				{
					IOLog(" ") ;
					if ((index & 0x7) == 0)
					{
						IOLog("   ") ;
						if ((index & 0xF) == 0)
							IOLog("\n\t\t") ;
					} 
				}
				IOLog("%02X", (unsigned char)bytePtr[index]) ;
			}
			IOLog("\n\n") ;
		}
        
        m = m->m_next;
    }
    IOLog("\n");
}
void _logPkt(void *pkt, UInt16 len)
{
	UInt8 	*bytePtr;

	bytePtr = (UInt8*)pkt;
	
	IOLog("pkt {\n") ;

	for(SInt32 index=0; index<len; index++)
	{
		if ((index & 0x3) == 0)
		{
			IOLog(" ") ;
			if ((index & 0x7) == 0)
			{
				IOLog("   ") ;
				if ((index & 0xF) == 0)
					IOLog("\n\t\t") ;
			} 
		}
		IOLog("%02X", (unsigned char)bytePtr[index]) ;
	}
	IOLog("}\n\n") ;
}
#endif 
