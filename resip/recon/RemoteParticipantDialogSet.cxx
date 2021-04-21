#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

// sipX includes
#if (_MSC_VER >= 1600)
#include <stdint.h>       // Use Visual Studio's stdint.h
#define _MSC_STDINT_H_    // This define will ensure that stdint.h in sipXport tree is not used
#endif

#include "ConversationManager.hxx"
#include "ReconSubsystem.hxx"
#include "RemoteParticipantDialogSet.hxx"
#include "RemoteParticipant.hxx"
#include "Conversation.hxx"
#include "UserAgent.hxx"
#include "MediaStreamEvent.hxx"

#include "sdp/SdpHelperResip.hxx"
#include "sdp/Sdp.hxx"

#include <rutil/Log.hxx>
#include <rutil/Logger.hxx>
#include <rutil/Random.hxx>
#include <rutil/DnsUtil.hxx>
#include <resip/stack/SipFrag.hxx>
#include <resip/dum/DialogUsageManager.hxx>
#include <resip/dum/ServerInviteSession.hxx>

#include <rutil/WinLeakCheck.hxx>

#include <utility>

using namespace recon;
using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM ReconSubsystem::RECON

RemoteParticipantDialogSet::RemoteParticipantDialogSet(ConversationManager& conversationManager,        
                                                       ConversationManager::ParticipantForkSelectMode forkSelectMode,
                                                       std::shared_ptr<ConversationProfile> conversationProfile) :
   AppDialogSet(conversationManager.getUserAgent()->getDialogUsageManager()),
   mConversationManager(conversationManager),
   mUACOriginalRemoteParticipant(0),
   mNumDialogs(0),
   mForkSelectMode(forkSelectMode),
   mConversationProfile(std::move(conversationProfile)),
   mUACConnectedDialogId(Data::Empty, Data::Empty, Data::Empty),
   mActiveRemoteParticipantHandle(0),
   mProposedSdp(0)
{

   InfoLog(<< "RemoteParticipantDialogSet created.");
}

RemoteParticipantDialogSet::~RemoteParticipantDialogSet() 
{
   // If we have no dialogs and mUACOriginalRemoteParticipant is set, then we have not passed 
   // ownership of mUACOriginalRemoteParticipant to DUM - so we need to delete the participant
   if(mNumDialogs == 0 && mUACOriginalRemoteParticipant)
   {
      delete mUACOriginalRemoteParticipant;
   }

   // Delete Sdp memory
   if(mProposedSdp) delete mProposedSdp;

   InfoLog(<< "RemoteParticipantDialogSet destroyed.  mActiveRemoteParticipantHandle=" << mActiveRemoteParticipantHandle);
}

std::shared_ptr<UserProfile> 
RemoteParticipantDialogSet::selectUASUserProfile(const SipMessage& msg)
{
   return mConversationManager.getUserAgent()->getIncomingConversationProfile(msg);
}

void 
RemoteParticipantDialogSet::processMediaStreamReadyEvent(const StunTuple& rtpTuple, const StunTuple& rtcpTuple)
{
   if(mPendingInvite.get() != 0)
   {
      // Pending Invite Request
      doSendInvite(mPendingInvite);
      mPendingInvite.reset();
   }

   if(mPendingOfferAnswer.mSdp != nullptr)
   {
      // Pending Offer or Answer
      doProvideOfferAnswer(mPendingOfferAnswer.mOffer, 
                           std::move(mPendingOfferAnswer.mSdp), 
                           mPendingOfferAnswer.mInviteSessionHandle, 
                           mPendingOfferAnswer.mPostOfferAnswerAccept, 
                           mPendingOfferAnswer.mPostAnswerAlert);
      resip_assert(mPendingOfferAnswer.mSdp == nullptr);
   }
}

void 
RemoteParticipantDialogSet::processMediaStreamErrorEvent(unsigned int errorCode)
{
   InfoLog( << "processMediaStreamErrorEvent, error=" << errorCode);

   // Note:  in the case of an initial invite we must issue the invite in order for dum to cleanup state
   //         properly - this is not ideal, since it may cause endpoint phone device to ring a little 
   //         before receiving the cancel
   if(mPendingInvite.get() != 0)
   {
      // Pending Invite Request - Falling back to using local address/port - but then end() immediate
      doSendInvite(mPendingInvite);
      mPendingInvite.reset();
   }

   // End call
   if(mNumDialogs > 0)
   {
      std::map<DialogId, RemoteParticipant*>::iterator it;
      for(it = mDialogs.begin(); it != mDialogs.end(); it++)
      {
         it->second->destroyParticipant();
      }
   }
   else
   {
      end();
   }
}

void 
RemoteParticipantDialogSet::sendInvite(std::shared_ptr<SipMessage> invite)
{
   if(!isAsyncMediaSetup())
   {
      doSendInvite(std::move(invite));
   }
   else
   {
      // Wait until media stream is ready
      mPendingInvite = std::move(invite);
   }
}

void 
RemoteParticipantDialogSet::doSendInvite(std::shared_ptr<SipMessage> invite)
{
   // Fix up address and port in SDP if we have remote info
   // Note:  the only time we don't is if there was an error preparing the media stream
   if(!isAsyncMediaSetup())
   {
      SdpContents* sdp  = dynamic_cast<SdpContents*>(invite->getContents());
      fixUpSdp(sdp);
   }

   // Send the invite
   mDum.send(std::move(invite));
}

void 
RemoteParticipantDialogSet::provideOffer(std::unique_ptr<resip::SdpContents> offer, resip::InviteSessionHandle& inviteSessionHandle, bool postOfferAccept)
{
   if(!isAsyncMediaSetup())
   {
      doProvideOfferAnswer(true /* offer */, std::move(offer), inviteSessionHandle, postOfferAccept, false);
   }
   else
   {
      resip_assert(mPendingOfferAnswer.mSdp == nullptr);
      mPendingOfferAnswer.mOffer = true;
      mPendingOfferAnswer.mSdp = std::move(offer);
      mPendingOfferAnswer.mInviteSessionHandle = inviteSessionHandle;
      mPendingOfferAnswer.mPostOfferAnswerAccept = postOfferAccept;
      mPendingOfferAnswer.mPostAnswerAlert = false;
   }
}

void 
RemoteParticipantDialogSet::provideAnswer(std::unique_ptr<resip::SdpContents> answer, resip::InviteSessionHandle& inviteSessionHandle, bool postAnswerAccept, bool postAnswerAlert)
{
   if(!isAsyncMediaSetup())
   {
      doProvideOfferAnswer(false /* offer */, std::move(answer), inviteSessionHandle, postAnswerAccept, postAnswerAlert);
   }
   else
   {
      resip_assert(mPendingOfferAnswer.mSdp == nullptr);
      mPendingOfferAnswer.mOffer = false;
      mPendingOfferAnswer.mSdp = std::move(answer);
      mPendingOfferAnswer.mInviteSessionHandle = inviteSessionHandle;
      mPendingOfferAnswer.mPostOfferAnswerAccept = postAnswerAccept;
      mPendingOfferAnswer.mPostAnswerAlert = postAnswerAlert;
   }
}

void 
RemoteParticipantDialogSet::doProvideOfferAnswer(bool offer, std::unique_ptr<resip::SdpContents> sdp, resip::InviteSessionHandle& inviteSessionHandle, bool postOfferAnswerAccept, bool postAnswerAlert)
{
   if(inviteSessionHandle.isValid() && !inviteSessionHandle->isTerminated())
   {
      // Fix up address and port in SDP if we have remote info
      // Note:  the only time we don't is if there was an error preparing the media stream
      if(!isAsyncMediaSetup())
      {
         fixUpSdp(sdp.get());
      }

      if(offer)
      {
         inviteSessionHandle->provideOffer(*sdp);
      }
      else
      {
         inviteSessionHandle->provideAnswer(*sdp);
      }

      // Adjust RTP Streams
      dynamic_cast<RemoteParticipant*>(inviteSessionHandle->getAppDialog().get())->adjustRTPStreams(offer);

      // Do Post Answer Operations
      ServerInviteSession* sis = dynamic_cast<ServerInviteSession*>(inviteSessionHandle.get());
      if(sis)
      {
         if(postAnswerAlert)
         {
            sis->provisional(180,true);
         }
         if(postOfferAnswerAccept)
         {
            sis->accept();
         }
      }
   }
}

void 
RemoteParticipantDialogSet::accept(resip::InviteSessionHandle& inviteSessionHandle)
{
   // If we have a pending answer, then just flag to accept when complete
   if(mPendingOfferAnswer.mSdp != nullptr &&
      !mPendingOfferAnswer.mOffer)
   {
      mPendingOfferAnswer.mPostOfferAnswerAccept = true;
   }
   else if(inviteSessionHandle.isValid())
   {
      ServerInviteSession* sis = dynamic_cast<ServerInviteSession*>(inviteSessionHandle.get());
      if(sis)
      {
         sis->accept();
      }
   }
}

void 
RemoteParticipantDialogSet::setActiveDestination(const char* address, unsigned short rtpPort, unsigned short rtcpPort)
{
#ifdef DISABLE_FLOWMANAGER_IF_NO_NAT_TRAVERSAL
   if(mNatTraversalMode != MediaStream::NoNatTraversal)
   {
#else
   if(!mMediaStream)
      WarningLog(<<"mMediaStream == NULL, no RTP will be transmitted");
#endif
   if(mMediaStream && mMediaStream->getRtpFlow())
   {
      mMediaStream->getRtpFlow()->setActiveDestination(address, rtpPort);
   }
   if(mMediaStream && mMediaStream->getRtcpFlow())
   {
      mMediaStream->getRtcpFlow()->setActiveDestination(address, rtcpPort);
   }
#ifdef DISABLE_FLOWMANAGER_IF_NO_NAT_TRAVERSAL
   }
   else
   {
      getMediaInterface()->getInterface()->setConnectionDestination(mMediaConnectionId,
                                      address, 
                                      rtpPort,  /* audio rtp port */
                                      rtcpPort, /* audio rtcp port */
                                      -1 /* video rtp port */, 
                                      -1 /* video rtcp port */);
   }
#endif
}

RemoteParticipant* 
RemoteParticipantDialogSet::createUACOriginalRemoteParticipant(ParticipantHandle handle)
{
   resip_assert(!mUACOriginalRemoteParticipant);
   RemoteParticipant *participant = new RemoteParticipant(handle, mConversationManager, mDum, *this);  
   mUACOriginalRemoteParticipant = participant;
   mActiveRemoteParticipantHandle = participant->getParticipantHandle();  // Store this since it may not be safe to access mUACOriginalRemoteParticipant pointer after corresponding Dialog has been created
   return participant;
}

AppDialog* 
RemoteParticipantDialogSet::createAppDialog(const SipMessage& msg)
{
   mNumDialogs++;

   if(mUACOriginalRemoteParticipant)  // UAC DialogSet
   {
      // Need to either return participant already created, or create a new one.
      if(mNumDialogs > 1)
      {
         // forking occured and we now have multiple dialogs in this dialog set
         RemoteParticipant* participant = new RemoteParticipant(mConversationManager, mDum, *this);

         InfoLog(<< "Forking occurred for original UAC participant handle=" << mUACOriginalRemoteParticipant->getParticipantHandle() << 
                    " this is leg number " << mNumDialogs << " new handle=" << participant->getParticipantHandle());

         // Create Related Conversations for each conversation UACOriginalRemoteParticipant was in when first Dialog is created
         std::list<ConversationHandle>::iterator it;
         for(it = mUACOriginalConversationHandles.begin(); it != mUACOriginalConversationHandles.end(); it++)
         {
            Conversation* conversation = mConversationManager.getConversation(*it);
            if(conversation)
            {
               conversation->createRelatedConversation(participant, mActiveRemoteParticipantHandle);
            }
         }

         mDialogs[DialogId(msg)] = participant;
         return participant;
      }
      else
      {
         // Get Conversations from Remote Participant and store - used later for creating related conversations
         const Participant::ConversationMap& conversations = mUACOriginalRemoteParticipant->getConversations();
         Participant::ConversationMap::const_iterator it;
         for(it = conversations.begin(); it != conversations.end(); it++)
         {
            mUACOriginalConversationHandles.push_back(it->second->getHandle());
         }

         mDialogs[DialogId(msg)] = mUACOriginalRemoteParticipant;
         return mUACOriginalRemoteParticipant;
      }
   }
   else
   {
      RemoteParticipant *participant = new RemoteParticipant(mConversationManager, mDum, *this);
      mActiveRemoteParticipantHandle = participant->getParticipantHandle();
      mDialogs[DialogId(msg)] = participant;  // Note:  !slg! DialogId is not quite right here, since there is no To Tag on the INVITE
      return participant;
   }
}

void 
RemoteParticipantDialogSet::setProposedSdp(ParticipantHandle handle, const resip::SdpContents& sdp)
{
   if(mProposedSdp) delete mProposedSdp;
   mProposedSdp = 0;
   InfoLog(<< "setProposedSdp: handle=" << handle << ", proposedSdp=" << sdp);
   //mProposedSdp = SdpHelperResip::createSdpFromResipSdp(sdp);
   mProposedSdp = new SdpContents(sdp);
}

void 
RemoteParticipantDialogSet::setUACConnected(const DialogId& dialogId, ParticipantHandle partHandle)
{
   resip_assert(mUACConnectedDialogId.getCallId().empty());
   mUACConnectedDialogId = dialogId;
   mActiveRemoteParticipantHandle = partHandle;
   if(mForkSelectMode == ConversationManager::ForkSelectAutomatic)
   {
      std::map<DialogId, RemoteParticipant*>::iterator it;
      for(it = mDialogs.begin(); it != mDialogs.end(); it++)
      {
         if(it->first != dialogId)
         {
            InfoLog(<< "Connected to forked leg " << dialogId << " - stale dialog " << it->first << " and related conversation(s) will be ended.");      
            it->second->destroyConversations();
         }
      }
   }
}

bool 
RemoteParticipantDialogSet::isUACConnected()
{
   return !mUACConnectedDialogId.getCallId().empty();
}

bool 
RemoteParticipantDialogSet::isStaleFork(const DialogId& dialogId)
{
   return (!mUACConnectedDialogId.getCallId().empty() && dialogId != mUACConnectedDialogId);
}

void 
RemoteParticipantDialogSet::removeDialog(const DialogId& dialogId)
{
   std::map<DialogId, RemoteParticipant*>::iterator it = mDialogs.find(dialogId);
   if(it != mDialogs.end())
   {
      if(it->second == mUACOriginalRemoteParticipant) mUACOriginalRemoteParticipant = 0;
      mDialogs.erase(it);
   }

   // If we have no more dialogs and we never went connected - make sure we cancel the Invite transaction
   if(mDialogs.size() == 0 && !isUACConnected())
   {
      end();
   }
}

ConversationManager::ParticipantForkSelectMode 
RemoteParticipantDialogSet::getForkSelectMode()
{
   return mForkSelectMode;
}

////////////////////////////////////////////////////////////////////////////////
// DialogSetHandler ///////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
void 
RemoteParticipantDialogSet::onTrying(AppDialogSetHandle, const SipMessage& msg)
{
   if(!isUACConnected() && mUACOriginalRemoteParticipant)
   {
      InfoLog(<< "onTrying: handle=" << mUACOriginalRemoteParticipant->getParticipantHandle() << ", " << msg.brief());
      //mConversationManager.onParticipantProceeding(mHandle, msg);
   }
}

void 
RemoteParticipantDialogSet::onNonDialogCreatingProvisional(AppDialogSetHandle, const SipMessage& msg)
{
   resip_assert(msg.header(h_StatusLine).responseCode() != 100);
   // It possible to get a provisional from another fork after receiving a 200 - if so, don't generate an event
   if(!isUACConnected() && mUACOriginalRemoteParticipant)
   {
      InfoLog(<< "onNonDialogCreatingProvisional: handle=" << mUACOriginalRemoteParticipant->getParticipantHandle() << ", " << msg.brief());
      if(mUACOriginalRemoteParticipant->getParticipantHandle()) mConversationManager.onParticipantAlerting(mUACOriginalRemoteParticipant->getParticipantHandle(), msg);
   }
}


/* ====================================================================

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
