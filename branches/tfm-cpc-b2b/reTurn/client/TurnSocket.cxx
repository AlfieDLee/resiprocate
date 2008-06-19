#include "TurnSocket.hxx"
#include "ErrorCode.hxx"
#include <boost/bind.hpp>
using namespace std;

#define UDP_RT0 100  // RTO - Estimate of Roundtrip time - 100ms is recommened for fixed line transport - the initial value should be configurable
                     // Should also be calculation this on the fly
#define UDP_MAX_RETRANSMITS  7       // Defined by RFC3489-bis11
#define TCP_RESPONSE_TIME 7900       // Defined by RFC3489-bis11
#define UDP_FINAL_REQUEST_TIME (UDP_RT0 * 16)  // Defined by RFC3489-bis11

namespace reTurn {

// Initialize static members
unsigned int TurnSocket::UnspecifiedLifetime = 0xFFFFFFFF;
unsigned int TurnSocket::UnspecifiedBandwidth = 0xFFFFFFFF; 
unsigned short TurnSocket::UnspecifiedPort = 0;
asio::ip::address TurnSocket::UnspecifiedIpAddress = asio::ip::address::from_string("0.0.0.0");

TurnSocket::TurnSocket(const asio::ip::address& address, unsigned short port) : 
   mLocalBinding(StunTuple::None /* Set properly by sub class */, address, port),
   mHaveAllocation(false),
   mActiveDestination(0),
   mReadTimer(mIOService)
{
}

TurnSocket::~TurnSocket()
{
}

StunMessage* 
TurnSocket::sendRequestAndGetResponse(StunMessage& request, asio::error_code& errorCode)
{
   unsigned int writesize = request.stunEncodeFramedMessage(mWriteBuffer, sizeof(mWriteBuffer));
   bool sendRequest = true;
   bool reliableTransport = mLocalBinding.getTransportType() != StunTuple::UDP;
   unsigned int timeout = reliableTransport ? TCP_RESPONSE_TIME : UDP_RT0;
   unsigned int totalTime = 0;
   unsigned int requestsSent = 0;
   unsigned int readsize = 0;

   while(true)
   {
      if(sendRequest)
      {
         // Send request to Turn Server
         requestsSent++;
         errorCode = rawWrite(mWriteBuffer, writesize);
         if(errorCode != 0)
         {
            return 0;
         }
         sendRequest = false;
      }
      //cout << "Reading with a timeout of " << timeout << "ms." << endl;

      // Wait for response
      errorCode = rawRead(mReadBuffer, sizeof(mReadBuffer), timeout, &readsize);
      if(errorCode != 0)
      {
         if(errorCode == asio::error::operation_aborted)
         {
            totalTime += timeout;
            //cout << "timeout at: " << totalTime << endl;
            if(reliableTransport || requestsSent == UDP_MAX_RETRANSMITS)
            {
               std::cout << "Timed out waiting for Stun response!" << std::endl;
               errorCode = asio::error_code(reTurn::ResponseTimeout, asio::misc_ecat);
               return 0;
            }

            // timed out and should retransmit - calculate next timeout
            if(requestsSent == UDP_MAX_RETRANSMITS - 1)
            {
               timeout = UDP_FINAL_REQUEST_TIME;
            } 
            else
            {
               timeout = (timeout*2);
            }
            sendRequest = true;
            continue;
         }
         return 0;
      }

      if(readsize > 4 && mReadBuffer[0] == 0)  // Channel 0 is Stun/Turn messaging
      {
         StunMessage* response = new StunMessage(mLocalBinding, mTurnServer, &mReadBuffer[4], readsize-4);

         if(response->isValid())
         {
            if(!response->checkMessageIntegrity(request.mHmacKey))
            {
               std::cout << "Stun response message integrity is bad!" << std::endl;
               delete response;
               errorCode = asio::error_code(reTurn::BadMessageIntegrity, asio::misc_ecat);
               return 0;
            }

            // Check that TID matches request
            if(!(response->mHeader.magicCookieAndTid == request.mHeader.magicCookieAndTid))
            {
               std::cout << "Stun response TID does not match request - discarding!" << std::endl;
               delete response;
               continue;  // read next message
            }
   
            errorCode = asio::error_code(reTurn::Success, asio::misc_ecat);
            return response;
         }
         else
         {
            std::cout << "Stun response message is invalid!" << std::endl;
            delete response;
            errorCode = asio::error_code(reTurn::ErrorParsingMessage, asio::misc_ecat);  
            return 0;
         }
      }
      else if(readsize > 4 && mReadBuffer[0] > 0) // Channel > 0 indicates Turn Data
      {
         // TODO - handle buffering of Turn data that is receive while waiting for a Request/Response
         errorCode = asio::error_code(reTurn::FrameError, asio::misc_ecat);  
         return 0;
      }
      else
      {
         // Less data than frame size received
         errorCode = asio::error_code(reTurn::FrameError, asio::misc_ecat);  
         return 0;
      }
   }
}

asio::error_code 
TurnSocket::requestSharedSecret(const asio::ip::address& turnServerAddress, 
                                unsigned short turnServerPort, 
                                char* username, unsigned int usernameSize, 
                                char* password, unsigned int passwordSize)
{
   asio::error_code errorCode;

   // Should we check here if TLS and deny?

   mTurnServer.setTransportType(mLocalBinding.getTransportType());
   mTurnServer.setAddress(turnServerAddress);
   mTurnServer.setPort(turnServerPort);

   // Connect socket to Turn Server
   errorCode = connect(turnServerAddress, turnServerPort);
   if(errorCode != 0)
   {
      return errorCode;
   }

   // Form Shared Secret request
   StunMessage request;
   request.createHeader(StunMessage::StunClassRequest, StunMessage::SharedSecretRequest);
   request.mHasFingerprint = true;

   // Get Response
   StunMessage* response = sendRequestAndGetResponse(request, errorCode);
   if(response == 0)
   {
      return errorCode;
   }

   // Check if success or not
   if(response->mHasErrorCode)
   {
      errorCode = asio::error_code(response->mErrorCode.errorClass * 100 + response->mErrorCode.number, asio::misc_ecat);
      delete response;
      return errorCode;
   }

   // Copy username and password to callers buffer - checking sizes first
   if(!response->mHasUsername || !response->mHasPassword)
   {
      std::cout << "Stun response message for SharedSecretRequest is missing username and/or password!" << std::endl;
      errorCode = asio::error_code(reTurn::MissingAuthenticationAttributes, asio::misc_ecat);  
      delete response;
      return errorCode;
   }

   if(response->mUsername->size() > usernameSize || response->mPassword->size() > passwordSize)
   {
      std::cout << "Stun response message for SharedSecretRequest contains data that is too large to return!" << std::endl;
      errorCode = asio::error_code(reTurn::BufferTooSmall, asio::misc_ecat);   
      delete response;
      return errorCode;
   }

   // Copy username and password to passed in buffers
   memcpy(username, response->mUsername->c_str(), response->mUsername->size()+1);
   memcpy(password, response->mPassword->c_str(), response->mPassword->size()+1);

   // All was well - return 0 errorCode
   delete response;
   return errorCode;
}

asio::error_code 
TurnSocket::createAllocation(const asio::ip::address& turnServerAddress, 
                             unsigned short turnServerPort, 
                             char* username,
                             char* password,
                             unsigned int lifetime,
                             unsigned int bandwidth,
                             unsigned short requestedPortProps, 
                             unsigned short requestedPort,
                             StunTuple::TransportType requestedTransportType, 
                             const asio::ip::address &requestedIpAddress)
{
   asio::error_code errorCode;

   mHaveAllocation = false;  // toggled to true if successful

   // Store Allocation Properties
   mTurnServer.setTransportType(mLocalBinding.getTransportType());
   mTurnServer.setAddress(turnServerAddress);
   mTurnServer.setPort(turnServerPort);
   mUsername = username;
   mPassword = password;
   mRequestedLifetime = lifetime;
   mRequestedBandwidth = bandwidth;
   mRequestedPortProps = requestedPortProps;
   mRequestedPort = requestedPort;
   mRequestedTransportType = requestedTransportType;
   mRequestedIpAddress = requestedIpAddress;

   // Connect socket to Turn Server
   errorCode = connect(turnServerAddress, turnServerPort);
   if(errorCode != 0)
   {
      return errorCode;
   }

   // Perform allocation
   return refreshAllocation();
}

asio::error_code 
TurnSocket::refreshAllocation()
{
   asio::error_code errorCode;

   // Form Turn Allocate request
   StunMessage request;
   request.createHeader(StunMessage::StunClassRequest, StunMessage::TurnAllocateRequest);
   if(mRequestedLifetime != UnspecifiedLifetime)
   {
      request.mHasTurnLifetime = true;
      request.mTurnLifetime = mRequestedLifetime;
   }
   if(mRequestedBandwidth != UnspecifiedBandwidth)
   {
      request.mHasTurnBandwidth = true;
      request.mTurnBandwidth = mRequestedBandwidth;
   }
   if(mRequestedTransportType != StunTuple::None)
   {
      // TODO - could do some validation here
      request.mHasTurnRequestedTransport = true;
      request.mTurnRequestedTransport = mRequestedTransportType;
   }
   if(mRequestedIpAddress != UnspecifiedIpAddress)
   {
      request.mHasTurnRequestedIp = true;
      request.mTurnRequestedIp.port = 0;  // Not relevant
      if(mRequestedIpAddress.is_v6())
      {
         request.mTurnRequestedIp.family = StunMessage::IPv6Family;  
         memcpy(&request.mTurnRequestedIp.addr.ipv6, mRequestedIpAddress.to_v6().to_bytes().c_array(), sizeof(request.mTurnRequestedIp.addr.ipv6));
      }
      else
      {
         request.mTurnRequestedIp.family = StunMessage::IPv4Family;  
         request.mTurnRequestedIp.addr.ipv4 = mRequestedIpAddress.to_v4().to_ulong();   
      }
   }
   if(mRequestedPortProps != StunMessage::PortPropsNone || mRequestedPort != UnspecifiedPort)
   {
      request.mHasTurnRequestedPortProps = true;
      request.mTurnRequestedPortProps.props = mRequestedPortProps;
      request.mTurnRequestedPortProps.port = mRequestedPort;
   }
   request.mHasFingerprint = true;
   request.mHasMessageIntegrity = true;

   request.setUsername(mUsername.data()); 
   request.mHmacKey = mPassword;

   StunMessage* response = sendRequestAndGetResponse(request, errorCode);
   if(response == 0)
   {
      return errorCode;
   }

   if(response->mHasXorMappedAddress)
   {
      response->applyXorToAddress(response->mXorMappedAddress, response->mXorMappedAddress);
      mReflexiveTuple.setTransportType(mLocalBinding.getTransportType());
      mReflexiveTuple.setPort(response->mXorMappedAddress.port);
      if(response->mXorMappedAddress.family == StunMessage::IPv6Family)
      {
         asio::ip::address_v6::bytes_type bytes;
         memcpy(bytes.c_array(), &response->mXorMappedAddress.addr.ipv6, bytes.size());
         mReflexiveTuple.setAddress(asio::ip::address_v6(bytes));
      }
      else
      {
         mReflexiveTuple.setAddress(asio::ip::address_v4(response->mXorMappedAddress.addr.ipv4));
      }            
   }
   if(response->mHasTurnRelayAddress)
   {
      mRelayTuple.setTransportType(request.mHasTurnRequestedTransport ? (StunTuple::TransportType)request.mTurnRequestedTransport : mLocalBinding.getTransportType());  // Transport Type is requested type or socket type
      mRelayTuple.setPort(response->mTurnRelayAddress.port);
      if(response->mTurnRelayAddress.family == StunMessage::IPv6Family)
      {
         asio::ip::address_v6::bytes_type bytes;
         memcpy(bytes.c_array(), &response->mTurnRelayAddress.addr.ipv6, bytes.size());
         mRelayTuple.setAddress(asio::ip::address_v6(bytes));
      }
      else
      {
         mRelayTuple.setAddress(asio::ip::address_v4(response->mTurnRelayAddress.addr.ipv4));
      } 
   }
   if(response->mHasTurnLifetime)
   {
      mLifetime = response->mTurnLifetime;
   }
   if(response->mHasTurnBandwidth)
   {
      mBandwidth = response->mTurnBandwidth;
   }

   // Check if success or not
   if(response->mHasErrorCode)
   {
      if(mRequestedLifetime != 0)
      {
         mHaveAllocation = false;
      }

      errorCode = asio::error_code(response->mErrorCode.errorClass * 100 + response->mErrorCode.number, asio::misc_ecat);
      delete response;
      return errorCode;
   }

   // All was well - return 0 errorCode
   if(mLifetime != 0)
   {
      mHaveAllocation = true;
   }
   else
   {
      mHaveAllocation = false;
   }
   delete response;
   return errorCode;
}

asio::error_code 
TurnSocket::destroyAllocation()
{
   if(mHaveAllocation)
   {
      mRequestedLifetime = 0;
      mRequestedBandwidth = UnspecifiedBandwidth;
      mRequestedPortProps = StunMessage::PortPropsNone;
      mRequestedPort = UnspecifiedPort;
      mRequestedTransportType = StunTuple::None;
      mRequestedIpAddress = UnspecifiedIpAddress;

      return refreshAllocation();
   }
   else
   {
      return asio::error_code(reTurn::NoAllocation, asio::misc_ecat); 
   }
}

StunTuple& 
TurnSocket::getRelayTuple()
{
   return mRelayTuple;
}

StunTuple& 
TurnSocket::getReflexiveTuple()
{
   return mReflexiveTuple;
}

unsigned int 
TurnSocket::getLifetime()
{
   return mLifetime;
}

unsigned int 
TurnSocket::getBandwidth()
{
   return mBandwidth;
}

asio::error_code 
TurnSocket::setActiveDestination(const asio::ip::address& address, unsigned short port)
{
   asio::error_code errorCode;

   // ensure there is an allocation
   if(!mHaveAllocation)
   {
      return asio::error_code(reTurn::NoAllocation, asio::misc_ecat); 
   }

   // Setup Remote Peer 
   StunTuple remoteTuple(mRelayTuple.getTransportType(), address, port);
   RemotePeer* remotePeer = mChannelManager.findRemotePeerByPeerAddress(remoteTuple);
   if(remotePeer)
   {
      mActiveDestination = remotePeer;
   }
   else
   {
      // No remote peer yet (ie. not data sent or received from remote peer) - so create one
      mActiveDestination = mChannelManager.createRemotePeer(remoteTuple, mChannelManager.getNextChannelNumber(), 0);
      assert(mActiveDestination);
   }

   return errorCode;
}

asio::error_code 
TurnSocket::clearActiveDestination()
{
   asio::error_code errorCode;

   // ensure there is an allocation
   if(!mHaveAllocation)
   {
      return asio::error_code(reTurn::NoAllocation, asio::misc_ecat); 
   }

   mActiveDestination = 0;

   return errorCode;
}

asio::error_code 
TurnSocket::send(const char* buffer, unsigned int size)
{
   // Allow raw data to be sent if there is no allocation
   if(!mHaveAllocation)
   {
      return rawWrite(buffer, size);
   }

   if(!mActiveDestination)
   {
      return asio::error_code(reTurn::NoActiveDestination, asio::misc_ecat); 
   }

   return sendTo(*mActiveDestination, buffer, size);
}

asio::error_code 
TurnSocket::sendTo(const asio::ip::address& address, unsigned short port, const char* buffer, unsigned int size)
{
   // ensure there is an allocation 
   if(!mHaveAllocation)
   {
      return asio::error_code(reTurn::NoAllocation, asio::misc_ecat); 
   }

   // Setup Remote Peer 
   StunTuple remoteTuple(mRelayTuple.getTransportType(), address, port);
   RemotePeer* remotePeer = mChannelManager.findRemotePeerByPeerAddress(remoteTuple);
   if(!remotePeer)
   {
      // No remote peer yet (ie. not data sent or received from remote peer) - so create one
      remotePeer = mChannelManager.createRemotePeer(remoteTuple, mChannelManager.getNextChannelNumber(), 0);
      assert(remotePeer);
   }
   return sendTo(*remotePeer, buffer, size);
}

asio::error_code 
TurnSocket::sendTo(RemotePeer& remotePeer, const char* buffer, unsigned int size)
{
   if(remotePeer.isClientToServerChannelConfirmed())
   {
      // send framed data to active destination
      char framing[4];
      framing[0] = remotePeer.getClientToServerChannel();
      framing[1] = 0x00;  // reserved
      if(mLocalBinding.getTransportType() == StunTuple::UDP)
      {
         // No size in header for UDP
         framing[2] = 0x00;
         framing[3] = 0x00;
      }
      else
      {
         UInt16 turnDataSize = size;
         turnDataSize = htons(turnDataSize);
         memcpy((void*)&framing[2], &turnDataSize, 2);
      }
      std::vector<asio::const_buffer> bufs;
      bufs.push_back(asio::buffer(framing, sizeof(framing)));
      bufs.push_back(asio::buffer(buffer, size));

      return rawWrite(bufs);
   }
   else
   {
      // Data must be wrapped in a Send Indication
      // Wrap data in a SendInd
      StunMessage ind;
      ind.createHeader(StunMessage::StunClassIndication, StunMessage::TurnSendInd);
      ind.mHasTurnRemoteAddress = true;
      ind.mTurnRemoteAddress.port = remotePeer.getPeerTuple().getPort();
      if(remotePeer.getPeerTuple().getAddress().is_v6())
      {
         ind.mTurnRemoteAddress.family = StunMessage::IPv6Family;
         memcpy(&ind.mTurnRemoteAddress.addr.ipv6, remotePeer.getPeerTuple().getAddress().to_v6().to_bytes().c_array(), sizeof(ind.mTurnRemoteAddress.addr.ipv6));
      }
      else
      {
         ind.mTurnRemoteAddress.family = StunMessage::IPv4Family;
         ind.mTurnRemoteAddress.addr.ipv4 = remotePeer.getPeerTuple().getAddress().to_v4().to_ulong();
      }
      ind.mHasTurnChannelNumber = true;
      ind.mTurnChannelNumber = remotePeer.getClientToServerChannel();
      ind.setTurnData(buffer, size);
      ind.mHasFingerprint = true;

      // If not using UDP - then mark channel as confirmed
      if(mLocalBinding.getTransportType() != StunTuple::UDP)
      {
         remotePeer.setClientToServerChannelConfirmed();
      }

      // Send indication to Turn Server
      unsigned int msgsize = ind.stunEncodeFramedMessage(mWriteBuffer, sizeof(mWriteBuffer));
      return rawWrite(mWriteBuffer, msgsize);
   }
}

asio::error_code 
TurnSocket::receive(char* buffer, unsigned int& size, unsigned int timeout, asio::ip::address* sourceAddress, unsigned short* sourcePort)
{
   asio::error_code errorCode;
   bool done = false;

   // TODO - rethink this scheme so that we don't need to copy recieved data
   // TODO - if we loop around more than once - timeout needs to be adjusted

   while(!done)
   {
      done = true;

      // Wait for response
      unsigned int readSize;
      errorCode = rawRead(mReadBuffer, sizeof(mReadBuffer), timeout, &readSize, sourceAddress, sourcePort); // Note: SourceAddress and sourcePort may be overwritten below if from Turn Relay
      if(errorCode != 0)
      {
         return errorCode;
      }

      // Note if this is a UDP RFC3489 back compat socket, then allocations are not allowed and handleRawData will always be used
      if(!mHaveAllocation)
      {
         return handleRawData(mReadBuffer, readSize, readSize, buffer, size);
      }

      // Check Channel
      if(size > 4 && mReadBuffer[0] > 0) // We have received Turn Data
      {
         RemotePeer* remotePeer = mChannelManager.findRemotePeerByServerToClientChannel(buffer[0]);
         if(remotePeer)
         {
            UInt16 dataLen;
            memcpy(&dataLen, &mReadBuffer[2], 2);
            dataLen = ntohs(dataLen);

            if(sourceAddress)
            {
               *sourceAddress = remotePeer->getPeerTuple().getAddress();
            }
            if(sourcePort)
            {
               *sourcePort = remotePeer->getPeerTuple().getPort();
            }
            errorCode = handleRawData(&mReadBuffer[4], readSize-4, dataLen, buffer, size);
         }
         else
         {
            // Invalid ServerToClient Channel - teardown?
            errorCode = asio::error_code(reTurn::InvalidChannelNumberReceived, asio::misc_ecat);  
         }
      }
      else if(size > 4 && mReadBuffer[0] == 0)  // We have received a Stun/Turn Message
      {
         // StunMessage
         StunMessage* stunMsg = new StunMessage(mLocalBinding, mTurnServer, &mReadBuffer[4], readSize-4);
         unsigned int tempsize = size;
         errorCode = handleStunMessage(*stunMsg, buffer, tempsize, sourceAddress, sourcePort);
         if(errorCode == 0 && tempsize == 0)  // Signifies that a Stun/Turn request was received and there is nothing to return to receive caller
         {
            done = false;
         }
         else
         {
            size = tempsize;
         }
      }
      else
      {
         // Less data than frame size received
         errorCode = asio::error_code(reTurn::FrameError, asio::misc_ecat);  
      }
   }
   return errorCode;
}

asio::error_code 
TurnSocket::receiveFrom(const asio::ip::address& address, unsigned short port, char* buffer, unsigned int& size, unsigned int timeout)
{
   asio::ip::address sourceAddress;
   unsigned short sourcePort;
   bool done = false;
   asio::error_code errorCode;

   while(!done)
   {
      done = true;
      errorCode = receive(buffer, size, timeout, &sourceAddress, &sourcePort);
      if(errorCode == 0)
      {
         if(sourceAddress != address || sourcePort != port)
         {
            std::cout << "Recevied message but not from requested address/port - Discarding." << std::endl;            
            done = false;
         }
      }
   }
   return errorCode;
}

asio::error_code 
TurnSocket::handleRawData(char* data, unsigned int dataSize, unsigned int expectedSize, char* buffer, unsigned int& bufferSize)
{
   asio::error_code errorCode;

   if(dataSize != expectedSize)
   {
      // TODO - fix read logic so that we can read in chuncks
      std::cout << "Did not read entire message: read=" << dataSize << " wanted=" << expectedSize << std::endl;
      return asio::error_code(reTurn::ReadError, asio::misc_ecat); 
   }

   if(dataSize > bufferSize) 
   {
     // Passed in buffer is not large enough
     std::cout << "Passed in buffer not large enough." << std::endl;
     return asio::error_code(reTurn::BufferTooSmall, asio::misc_ecat); 
   }

   // Copy data to return buffer
   memcpy(buffer, data, dataSize);
   bufferSize = dataSize;

   return errorCode;
}

asio::error_code 
TurnSocket::handleStunMessage(StunMessage& stunMessage, char* buffer, unsigned int& size, asio::ip::address* sourceAddress, unsigned short* sourcePort)
{
   asio::error_code errorCode;
   if(stunMessage.isValid())
   {
      if(stunMessage.mClass == StunMessage::StunClassIndication && stunMessage.mMethod == StunMessage::TurnDataInd)
      {
         if(!stunMessage.mHasTurnRemoteAddress || !stunMessage.mHasTurnChannelNumber)
         {
            // Missing RemoteAddress or ChannelNumber attribute
            std::cout << "DataInd missing attributes." << std::endl;
            return asio::error_code(reTurn::MissingAttributes, asio::misc_ecat);
         }

         StunTuple remoteTuple;
         remoteTuple.setTransportType(mRelayTuple.getTransportType());
         remoteTuple.setPort(stunMessage.mTurnRemoteAddress.port);
         if(stunMessage.mTurnRemoteAddress.family == StunMessage::IPv6Family)
         {
            asio::ip::address_v6::bytes_type bytes;
            memcpy(bytes.c_array(), &stunMessage.mTurnRemoteAddress.addr.ipv6, bytes.size());
            remoteTuple.setAddress(asio::ip::address_v6(bytes));
         }
         else
         {
            remoteTuple.setAddress(asio::ip::address_v4(stunMessage.mTurnRemoteAddress.addr.ipv4));
         }

         RemotePeer* remotePeer = mChannelManager.findRemotePeerByPeerAddress(remoteTuple);
         if(!remotePeer)
         {
            // Remote Peer not found - discard data
            std::cout << "Data received from unknown RemotePeer - discarding" << std::endl;
            return asio::error_code(reTurn::UnknownRemoteAddress, asio::misc_ecat);
         }

         if(remotePeer->getServerToClientChannel() != 0 && remotePeer->getServerToClientChannel() != stunMessage.mTurnChannelNumber)
         {
            // Mismatched channel number
            std::cout << "Channel number received in DataInd (" << (int)stunMessage.mTurnChannelNumber << ") does not match existing number for RemotePeer (" << (int)remotePeer->getServerToClientChannel() << ")." << std::endl;
            return asio::error_code(reTurn::InvalidChannelNumberReceived, asio::misc_ecat);
          }

         remotePeer->setServerToClientChannel(stunMessage.mTurnChannelNumber);
         remotePeer->setServerToClientChannelConfirmed();
         mChannelManager.addRemotePeerServerToClientChannelLookup(remotePeer);

         if(mLocalBinding.getTransportType() == StunTuple::UDP)
         {
            // If UDP, then send TurnChannelConfirmationInd
            StunMessage channelConfirmationInd;
            channelConfirmationInd.createHeader(StunMessage::StunClassIndication, StunMessage::TurnChannelConfirmationInd);
            channelConfirmationInd.mHasTurnRemoteAddress = true;
            channelConfirmationInd.mTurnRemoteAddress = stunMessage.mTurnRemoteAddress;
            channelConfirmationInd.mHasTurnChannelNumber = true;
            channelConfirmationInd.mTurnChannelNumber = stunMessage.mTurnChannelNumber;
            channelConfirmationInd.mHasFingerprint = true;

            // send channelConfirmationInd to local client
            unsigned int bufferSize = 8 /* Stun Header */ + 36 /* Remote Address (v6) */ + 8 /* TurnChannelNumber */ + 8 /* Fingerprint */ + 4 /* Turn Frame size */;
            resip::Data buffer(bufferSize, resip::Data::Preallocate);
            unsigned int size = channelConfirmationInd.stunEncodeFramedMessage((char*)buffer.data(), bufferSize);

            errorCode = rawWrite(buffer.data(), size);
         }

         if(stunMessage.mHasTurnData)
         {
            if(stunMessage.mTurnData->size() > size)
            {
               // Passed in buffer is not large enough
               std::cout << "Passed in buffer not large enough." << std::endl;
               return asio::error_code(reTurn::BufferTooSmall, asio::misc_ecat);
            }

            memcpy(buffer, stunMessage.mTurnData->data(), stunMessage.mTurnData->size());
            size = (unsigned int)stunMessage.mTurnData->size();

            if(sourceAddress != 0)
            {
               *sourceAddress = remoteTuple.getAddress();
            }
            if(sourcePort != 0)
            {
               *sourcePort = remoteTuple.getPort();
            }
         }
         else
         {
            size = 0;
         }
      }
      else if(stunMessage.mClass == StunMessage::StunClassIndication && stunMessage.mMethod == StunMessage::TurnChannelConfirmationInd)
      {
         if(!stunMessage.mHasTurnRemoteAddress || !stunMessage.mHasTurnChannelNumber)
         {
            // Missing RemoteAddress or ChannelNumber attribute
            std::cout << "DataInd missing attributes." << std::endl;
            return asio::error_code(reTurn::MissingAttributes, asio::misc_ecat);
         }

         StunTuple remoteTuple;
         remoteTuple.setTransportType(mRelayTuple.getTransportType());
         remoteTuple.setPort(stunMessage.mTurnRemoteAddress.port);
         if(stunMessage.mTurnRemoteAddress.family == StunMessage::IPv6Family)
         {
            asio::ip::address_v6::bytes_type bytes;
            memcpy(bytes.c_array(), &stunMessage.mTurnRemoteAddress.addr.ipv6, bytes.size());
            remoteTuple.setAddress(asio::ip::address_v6(bytes));
         }
         else
         {
            remoteTuple.setAddress(asio::ip::address_v4(stunMessage.mTurnRemoteAddress.addr.ipv4));
         }

         RemotePeer* remotePeer = mChannelManager.findRemotePeerByClientToServerChannel(stunMessage.mTurnChannelNumber);
         if(!remotePeer)
         {
            // Remote Peer not found - discard
            std::cout << "Received ChannelConfirmationInd for unknown channel (" << stunMessage.mTurnChannelNumber << ") - discarding" << std::endl;
            return asio::error_code(reTurn::InvalidChannelNumberReceived, asio::misc_ecat);
         }

         if(remotePeer->getPeerTuple() != remoteTuple)
         {
            // Mismatched remote address
            std::cout << "RemoteAddress associated with channel (" << remotePeer->getPeerTuple() << ") does not match ChannelConfirmationInd (" << remoteTuple << ")." << std::endl;
            return asio::error_code(reTurn::UnknownRemoteAddress, asio::misc_ecat);
         }

         remotePeer->setClientToServerChannelConfirmed();
         size = 0;
      }
      else
      {
         // TODO - handle others ????
      }
   }
   else
   {
      std::cout << "Read Invalid StunMsg." << std::endl;
      return asio::error_code(reTurn::ErrorParsingMessage, asio::misc_ecat);
   }
   return errorCode;
}

void 
TurnSocket::startReadTimer(unsigned int timeout)
{
   if(timeout != 0)
   {
      mReadTimer.expires_from_now(boost::posix_time::milliseconds(timeout));
      mReadTimer.async_wait(boost::bind(&TurnSocket::handleRawReadTimeout, this, asio::placeholders::error));
   }
}

void 
TurnSocket::handleRawRead(const asio::error_code& errorCode, size_t bytesRead)
{
   //clog << "handleRawRead: errorCode=" << errorCode << ", bytes=" << bytesRead << endl;
   mBytesRead = bytesRead;
   mReadErrorCode = errorCode;
   mReadTimer.cancel();
}

void 
TurnSocket::handleRawReadTimeout(const asio::error_code& errorCode)
{
   //clog << "handleRawReadTimeout: errorCode=" << errorCode << endl;
   if(errorCode == 0)
   {
      cancelSocket();
   }
}

} // namespace


/* ====================================================================

 Original contribution Copyright (C) 2007 Plantronics, Inc.
 Provided under the terms of the Vovida Software License, Version 2.0.

 The Vovida Software License, Version 2.0 
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution. 
 
 THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 DAMAGE.

 ==================================================================== */

