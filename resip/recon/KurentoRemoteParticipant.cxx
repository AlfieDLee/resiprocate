#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <media/kurento/Object.hxx>

#include "KurentoConversationManager.hxx"

#include "KurentoRemoteParticipant.hxx"
#include "Conversation.hxx"
#include "UserAgent.hxx"
#include "DtmfEvent.hxx"
#include "ReconSubsystem.hxx"

#include <rutil/Log.hxx>
#include <rutil/Logger.hxx>
#include <rutil/DnsUtil.hxx>
#include <rutil/Random.hxx>
#include <resip/stack/DtmfPayloadContents.hxx>
#include <resip/stack/SipFrag.hxx>
#include <resip/stack/ExtensionHeader.hxx>
#include <resip/dum/DialogUsageManager.hxx>
#include <resip/dum/ClientInviteSession.hxx>
#include <resip/dum/ServerInviteSession.hxx>
#include <resip/dum/ClientSubscription.hxx>
#include <resip/dum/ServerOutOfDialogReq.hxx>
#include <resip/dum/ServerSubscription.hxx>

#include <rutil/WinLeakCheck.hxx>

#include <utility>

using namespace recon;
using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM ReconSubsystem::RECON

/* Technically, there are a range of features that need to be implemented
   to be fully (S)AVPF compliant.
   However, it is speculated that (S)AVPF peers will communicate with legacy
   systems that just fudge the RTP/SAVPF protocol in their SDP.  Enabling
   this define allows such behavior to be tested. 

   http://www.ietf.org/mail-archive/web/rtcweb/current/msg01145.html
   "1) RTCWEB end-point will always signal AVPF or SAVPF. I signalling
   gateway to legacy will change that by removing the F to AVP or SAVP."

   http://www.ietf.org/mail-archive/web/rtcweb/current/msg04380.html
*/
//#define RTP_SAVPF_FUDGE

// UAC
KurentoRemoteParticipant::KurentoRemoteParticipant(ParticipantHandle partHandle,
                                     KurentoConversationManager& kurentoConversationManager,
                                     DialogUsageManager& dum,
                                     RemoteParticipantDialogSet& remoteParticipantDialogSet)
: Participant(partHandle, kurentoConversationManager),
  RemoteParticipant(partHandle, kurentoConversationManager, dum, remoteParticipantDialogSet),
  KurentoParticipant(partHandle, kurentoConversationManager)
{
   InfoLog(<< "KurentoRemoteParticipant created (UAC), handle=" << mHandle);
}

// UAS - or forked leg
KurentoRemoteParticipant::KurentoRemoteParticipant(KurentoConversationManager& kurentoConversationManager,
                                     DialogUsageManager& dum, 
                                     RemoteParticipantDialogSet& remoteParticipantDialogSet)
: Participant(kurentoConversationManager),
  RemoteParticipant(kurentoConversationManager, dum, remoteParticipantDialogSet),
  KurentoParticipant(kurentoConversationManager)
{
   InfoLog(<< "KurentoRemoteParticipant created (UAS or forked leg), handle=" << mHandle);
}

KurentoRemoteParticipant::~KurentoRemoteParticipant()
{
   // Note:  Ideally this call would exist in the Participant Base class - but this call requires 
   //        dynamic_casts and virtual methods to function correctly during destruction.
   //        If the call is placed in the base Participant class then these things will not
   //        function as desired because a classes type changes as the descructors unwind.
   //        See https://stackoverflow.com/questions/10979250/usage-of-this-in-destructor.
   unregisterFromAllConversations();

   InfoLog(<< "KurentoRemoteParticipant destroyed, handle=" << mHandle);
}

int 
KurentoRemoteParticipant::getConnectionPortOnBridge()
{
   if(getDialogSet().getActiveRemoteParticipantHandle() == mHandle)
   {
      return -1;  // FIXME Kurento
   }
   else
   {
      // If this is not active fork leg, then we don't want to effect the bridge mixer.  
      // Note:  All forked endpoints/participants have the same connection port on the bridge
      return -1;
   }
}

int 
KurentoRemoteParticipant::getMediaConnectionId()
{ 
   return getKurentoDialogSet().getMediaConnectionId();
}

void
KurentoRemoteParticipant::buildSdpOffer(bool holdSdp, SdpContents& offer)
{
   // FIXME Kurento - this needs to be async

   // FIXME Kurento - use holdSdp

   // FIXME Kurento - include video, SRTP, WebRTC?

   try
   {
      // FIXME: replace the following code for new media/kurento async API

      //kurento_client::KurentoClient& client = mKurentoConversationManager.getKurentoClient();

      //client.createRtpEndpoint("RtpEndpoint");
      //setEndpointId(client.getRtpEndpointId().c_str());
      //client.invokeConnect();
      //client.gatherCandidates();
      //client.invokeProcessOffer();

      std::string _offer; // FIXME
      //std::string _offer(client.getMReturnedSdp());

      StackLog(<<"offer FROM Kurento: " << _offer.c_str());

      HeaderFieldValue hfv(_offer.data(), _offer.size());
      Mime type("application", "sdp");
      offer = SdpContents(hfv, type);

      setProposedSdp(offer);
   }
   catch(exception& e)
   {
      ErrLog(<<"something went wrong: " << e.what());
   }
}

AsyncBool
KurentoRemoteParticipant::buildSdpAnswer(const SdpContents& offer, ContinuationAnswerReady c)
{
   AsyncBool valid = False;

   std::shared_ptr<SdpContents> offerMangled = std::make_shared<SdpContents>(offer);
   while(offerMangled->session().media().size() > 2)
   {
      // FIXME hack to remove BFCP
      DebugLog(<<"more than 2 media descriptors, removing the last");
      offerMangled->session().media().pop_back();
   }

   try
   {
      // do some checks on the offer
      // check for video, check for WebRTC
      typedef std::map<resip::Data, int> MediaSummary;
      MediaSummary mediaMap;
      std::set<resip::Data> mediumTransports;
      for(SdpContents::Session::MediumContainer::const_iterator it = offerMangled->session().media().cbegin();
               it != offerMangled->session().media().cend();
               it++)
      {
         const SdpContents::Session::Medium& m = *it;
         if(mediaMap.find(m.name()) != mediaMap.end())
         {
            mediaMap[m.name()]++;
         }
         else
         {
            mediaMap[m.name()] = 1;
         }
         mediumTransports.insert(m.protocol());
      }
      for(MediaSummary::const_iterator it = mediaMap.begin();
               it != mediaMap.end();
               it++)
      {
         StackLog(<<"found medium type " << it->first << " "  << it->second << " instance(s)");
      }
      bool isWebRTC = std::find(mediumTransports.cbegin(),
               mediumTransports.end(),
               "RTP/SAVPF") != mediumTransports.end();
      DebugLog(<<"peer is " << (isWebRTC ? "WebRTC":"not WebRTC"));

      std::ostringstream offerMangledBuf;
      offerMangledBuf << *offerMangled;
      std::shared_ptr<std::string> offerMangledStr = std::make_shared<std::string>(offerMangledBuf.str());

      StackLog(<<"offer TO Kurento: " << *offerMangledStr);

      mIceGatheringDone = true;
      if(isWebRTC)  // FIXME - maybe an endpoint already exists?  If so, use it
      {
         // delay while ICE gathers candidates from STUN and TURN
         mIceGatheringDone = false;
         mEndpoint.reset(new kurento::WebRtcEndpoint(mKurentoConversationManager.mPipeline));
      }
      else
      {
         mEndpoint.reset(new kurento::RtpEndpoint(mKurentoConversationManager.mPipeline));
      }

      // FIXME - add listeners for Kurento events

      kurento::ContinuationString cOnAnswerReady = [this, offerMangled, isWebRTC, c](const std::string& answer){
         StackLog(<<"answer FROM Kurento: " << answer);
         HeaderFieldValue hfv(answer.data(), answer.size());
         Mime type("application", "sdp");
         std::unique_ptr<SdpContents> _answer(new SdpContents(hfv, type));
         setLocalSdp(*_answer);
         setRemoteSdp(*offerMangled);
         updateConversation();
         c(true, std::move(_answer));
      };

      mEndpoint->create([this, offerMangled, offerMangledStr, isWebRTC, c, cOnAnswerReady]{
         mEndpoint->connect([this, offerMangled, offerMangledStr, isWebRTC, c, cOnAnswerReady]{
            mEndpoint->processOffer([this, offerMangled, isWebRTC, c, cOnAnswerReady](const std::string& answer){
               if(isWebRTC)
               {
                  std::shared_ptr<kurento::WebRtcEndpoint> webRtc = std::static_pointer_cast<kurento::WebRtcEndpoint>(mEndpoint);

                  std::shared_ptr<kurento::EventContinuation> elIceGatheringDone =
                        std::make_shared<kurento::EventContinuation>([this, cOnAnswerReady]{
                     mIceGatheringDone = true;
                     mEndpoint->getLocalSessionDescriptor(cOnAnswerReady);
                  });
                  webRtc->addOnIceGatheringDoneListener(elIceGatheringDone, [this](){});

                  webRtc->gatherCandidates([]{
                     // FIXME - handle the case where it fails
                     // on success, we continue from the IceGatheringDone event handler
                  }); // gatherCandidates
               }
               else
               {
                  cOnAnswerReady(answer);
               }
            }, *offerMangledStr); // processOffer
         }, *mEndpoint); // connect
      }); // create

      valid = Async;
   }
   catch(exception& e)
   {
      ErrLog(<<"something went wrong: " << e.what()); // FIXME - add try/catch to Continuation
   }

   return valid;
}

void
KurentoRemoteParticipant::adjustRTPStreams(bool sendingOffer)
{
   // FIXME Kurento - implement, may need to break up this method into multiple parts

   // FIXME Kurento - sometimes true
   setRemoteHold(false);

}

void
KurentoRemoteParticipant::addToConversation(Conversation *conversation, unsigned int inputGain, unsigned int outputGain)
{
   RemoteParticipant::addToConversation(conversation, inputGain, outputGain);
}

void
KurentoRemoteParticipant::updateConversation()
{
   const ConversationMap& cm = getConversations();
   Conversation* conversation = cm.begin()->second;
   Conversation::ParticipantMap& m = conversation->getParticipants();
   if(m.size() < 2)
   {
      DebugLog(<<"we are first in the conversation");
      return;
   }
   if(m.size() > 2)
   {
      WarningLog(<<"participants already here, can't join, size = " << m.size());
      return;
   }
   DebugLog(<<"joining a Conversation with an existing Participant");

   if(mEndpoint.get() == 0)
   {
      DebugLog(<<"our endpoint is not initialized"); // FIXME
      return;
   }
   mEndpoint->disconnect([this, conversation]{
      // Find the other Participant / endpoint

      // FIXME - only two Participants currently supported,
      // match the Participants by room name instead

      Conversation::ParticipantMap& m = conversation->getParticipants();
      KurentoRemoteParticipant* krp = 0;
      Conversation::ParticipantMap::iterator it = m.begin();
      while(it != m.end() && krp == 0)
      {
         krp = dynamic_cast<KurentoRemoteParticipant*>(it->second.getParticipant());
         if(krp == this)
         {
            krp = 0;
         }
      }
      resip_assert(krp);
      std::shared_ptr<kurento::Element> otherEndpoint(krp->mEndpoint);

      otherEndpoint->disconnect([this, otherEndpoint]{
        mEndpoint->connect([]{}, *otherEndpoint);
        otherEndpoint->connect([]{}, *mEndpoint);
      }, *otherEndpoint); // otherEndpoint->disconnect()
   }, *mEndpoint);  // mEndpoint->disconnect()

}

void
KurentoRemoteParticipant::removeFromConversation(Conversation *conversation)
{
   RemoteParticipant::removeFromConversation(conversation);
}

bool
KurentoRemoteParticipant::mediaStackPortAvailable()
{
   return true; // FIXME Kurento - can we check with Kurento somehow?
}


/* ====================================================================

 Copyright (c) 2021, SIP Spectrum, Inc.
 Copyright (c) 2021, Daniel Pocock https://danielpocock.com
 Copyright (c) 2007-2008, Plantronics, Inc.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are 
 met:

 1. Redistributions of source code must retain the above copyright 
    notice, this list of conditions and the following disclaimer. 

 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution. 

 3. Neither the name of Plantronics nor the names of its contributors 
    may be used to endorse or promote products derived from this 
    software without specific prior written permission. 

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 ==================================================================== */
