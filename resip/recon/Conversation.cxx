#include "Conversation.hxx"
#include "Participant.hxx"
#include "LocalParticipant.hxx"
#include "RemoteParticipant.hxx"
#include "MediaResourceParticipant.hxx"
#include "UserAgent.hxx"
#include "RelatedConversationSet.hxx"
#include "ReconSubsystem.hxx"

#include <rutil/Log.hxx>
#include <rutil/Logger.hxx>
#include <rutil/WinLeakCheck.hxx>

using namespace recon;
using namespace resip;

#define RESIPROCATE_SUBSYSTEM ReconSubsystem::RECON

Conversation::Conversation(ConversationHandle handle,
                           ConversationManager& conversationManager,
                           RelatedConversationSet* relatedConversationSet,
                           ConversationHandle sharedMediaInterfaceConvHandle,
                           ConversationManager::AutoHoldMode autoHoldMode)
: mHandle(handle),
  mConversationManager(conversationManager),
  mDestroying(false),
  mNumLocalParticipants(0),
  mNumRemoteParticipants(0),
  mNumMediaParticipants(0),
  mAutoHoldMode(autoHoldMode),
  mBridgeMixer(0),
  mSharingMediaInterfaceWithAnotherConversation(false)
{
   mConversationManager.registerConversation(this);

   if(relatedConversationSet)
   {
      mRelatedConversationSet = relatedConversationSet;
      mRelatedConversationSet->addRelatedConversation(mHandle, this);
   }
   else
   {
      mRelatedConversationSet = new RelatedConversationSet(mConversationManager, mHandle, this);
   }
   InfoLog(<< "Conversation created, handle=" << mHandle);

   if(dynamic_cast<SipXConversationManager&>(mConversationManager).getMediaInterfaceMode() == SipXConversationManager::sipXConversationMediaInterfaceMode)
   {
      // Check if sharedMediaInterfaceConvHandle was passed in, and if so use the same media interface and bridge mixer that, that
      // conversation is using
      if (sharedMediaInterfaceConvHandle != 0)
      {
         Conversation* sharedFlowConversation = mConversationManager.getConversation(sharedMediaInterfaceConvHandle);
         if (sharedFlowConversation)
         {
            mMediaInterface = sharedFlowConversation->getMediaInterface();
            mBridgeMixer = sharedFlowConversation->getBridgeMixerShared();
            mSharingMediaInterfaceWithAnotherConversation = true;
         }
      }
      
      if(!mSharingMediaInterfaceWithAnotherConversation)
      {
         dynamic_cast<SipXConversationManager&>(mConversationManager).createMediaInterfaceAndMixer(false /* giveFocus?*/,    // Focus will be given when local participant is added
                                                           mMediaInterface,
                                                           mBridgeMixer);
      }
      InfoLog(<< "mBridgeMixer " << mBridgeMixer.use_count() << " " << mBridgeMixer.get());
   }
}

Conversation::~Conversation()
{
   mConversationManager.unregisterConversation(this);
   if(mRelatedConversationSet)
   {
      mRelatedConversationSet->removeConversation(mHandle);
   }
   mConversationManager.onConversationDestroyed(mHandle);
   mBridgeMixer.reset();       // Make sure the mixer is destroyed before the media interface
   mMediaInterface.reset();
   InfoLog(<< "Conversation destroyed, handle=" << mHandle);
}

Participant* 
Conversation::getParticipant(ParticipantHandle partHandle)
{
   ParticipantMap::iterator it = mParticipants.find(partHandle);
   if(it != mParticipants.end())
   {
      return it->second.getParticipant();
   }
   else
   {
      return 0;
   }
}

void 
Conversation::addParticipant(Participant* participant, unsigned int inputGain, unsigned int outputGain)
{
   // If participant doesn't already exist in this conversation - then add them
   if(getParticipant(participant->getParticipantHandle()) == 0)
   {
      participant->addToConversation(this, inputGain, outputGain);
   }
}

void 
Conversation::removeParticipant(Participant* participant)
{
   // If participant exists in this conversation - then remove them
   if(getParticipant(participant->getParticipantHandle()) != 0)
   {
      participant->removeFromConversation(this);  // Can cause this to be deleted
   }
}

void 
Conversation::modifyParticipantContribution(Participant* participant, unsigned int inputGain, unsigned int outputGain)
{
   ParticipantMap::iterator it = mParticipants.find(participant->getParticipantHandle());
   if(it != mParticipants.end())
   {
      it->second.setInputGain(inputGain);
      it->second.setOutputGain(outputGain);
      participant->applyBridgeMixWeights();
   }
}

bool 
Conversation::shouldHold() 
{ 
   switch (mAutoHoldMode)
   {
   case ConversationManager::AutoHoldDisabled:
      return false;
   case ConversationManager::AutoHoldBroadcastOnly:
      return true;
   default:  // Enabled
      // We should be offering a hold SDP if there are no remote participants at all   OR
      // there is no LocalParticipant in the conversation and there are no other remote or media participants
      return mNumRemoteParticipants == 0 ||
             (mNumLocalParticipants == 0 && (mNumRemoteParticipants + mNumMediaParticipants) <= 1);
   }
}  

bool 
Conversation::broadcastOnly() 
{ 
   return mAutoHoldMode == ConversationManager::AutoHoldBroadcastOnly;
}  

void
Conversation::notifyRemoteParticipantsOfHoldChange()
{
   ParticipantMap::iterator i;
   for(i = mParticipants.begin(); i != mParticipants.end(); i++)
   {
      RemoteParticipant* remoteParticipant = dynamic_cast<RemoteParticipant*>(i->second.getParticipant());
      if(remoteParticipant)
      {
         remoteParticipant->checkHoldCondition();
      }
   }
}

void 
Conversation::createRelatedConversation(RemoteParticipant* newForkedParticipant, ParticipantHandle origParticipantHandle)
{
   // Create new Related Conversation
   ConversationHandle relatedConvHandle = mConversationManager.getNewConversationHandle();
   Conversation* conversation = new Conversation(relatedConvHandle, mConversationManager, mRelatedConversationSet, 
                                                 // If this conversation is sharing a media interface, then any related 
                                                 // conversations will as well (use our handle as the original handle
                                                 // passed in contructor could be gone)
                                                 mSharingMediaInterfaceWithAnotherConversation ? mHandle: 0, 
                                                 mAutoHoldMode);

   // Copy all participants to new Conversation, except origParticipant
   ParticipantMap::iterator i;
   for(i = mParticipants.begin(); i != mParticipants.end(); i++)
   {
      if(i->second.getParticipant()->getParticipantHandle() != origParticipantHandle)
      {
         conversation->addParticipant(i->second.getParticipant(), i->second.getInputGain(), i->second.getOutputGain());
      }
   }
   // add new forked participant to new Conversation
   conversation->addParticipant(newForkedParticipant);

   // Send onRelatedConversation event
   mConversationManager.onRelatedConversation(relatedConvHandle, newForkedParticipant->getParticipantHandle(), 
                                              mHandle, origParticipantHandle);
}

void 
Conversation::join(Conversation* conversation)
{
   // Copy all participants to new Conversation
   ParticipantMap::iterator i;
   for(i = mParticipants.begin(); i != mParticipants.end(); i++)
   {
      conversation->addParticipant(i->second.getParticipant(), i->second.getInputGain(), i->second.getOutputGain()); // checks for duplicates
   }
   destroy();  // destroy this conversation
}

void 
Conversation::destroy()
{
   if(mParticipants.size() == 0)
   {
      delete this;
   }
   else
   {
      // End each Participant - for local participants just remove them
      mDestroying = true;
      ParticipantMap temp = mParticipants;  // Need to copy since member list can be changed by terminating participants
      ParticipantMap::iterator i;
      for(i = temp.begin(); i != temp.end(); i++)
      {
         LocalParticipant* localParticipant = dynamic_cast<LocalParticipant*>(i->second.getParticipant());
         if(localParticipant)
         {
            removeParticipant(localParticipant);
         }
         else
         {
            if(i->second.getParticipant()->getNumConversations() == 1)
            {
               // Destroy participant if it is only in this conversation
               i->second.getParticipant()->destroyParticipant();
            }
            else
            {
               removeParticipant(i->second.getParticipant());  // Can cause "this" to get deleted
            }
         }
      }
   }
}

void 
Conversation::registerParticipant(Participant *participant, unsigned int inputGain, unsigned int outputGain)
{
   // Only increment count if registering new participant
   if(getParticipant(participant->getParticipantHandle()) == 0)
   {
      bool prevShouldHold = shouldHold();
      RemoteParticipant* remote;
      MediaResourceParticipant* media;
      if(dynamic_cast<LocalParticipant*>(participant))
      {
         mNumLocalParticipants++;
      }
      else if((remote = dynamic_cast<RemoteParticipant*>(participant)))
      {
         mNumRemoteParticipants++;
      }
      else if((media = dynamic_cast<MediaResourceParticipant*>(participant)))
      {
         mNumMediaParticipants++;
      }
      if(prevShouldHold != shouldHold())
      {
         notifyRemoteParticipantsOfHoldChange();  // Note: no need to notify party just added
      }
   }

   mParticipants[participant->getParticipantHandle()] = ConversationParticipantAssignment(participant, inputGain, outputGain);

   InfoLog(<< "Participant handle=" << participant->getParticipantHandle() << " added to conversation handle=" << mHandle << " (BridgePort=" << participant->getConnectionPortOnBridge() << ")");

   participant->applyBridgeMixWeights();
}

void 
Conversation::unregisterParticipant(Participant *participant)
{
   if(getParticipant(participant->getParticipantHandle()) != 0)
   {
      mParticipants.erase(participant->getParticipantHandle());  // No need to notify this party, remove from map first

      RemoteParticipant* remote;
      MediaResourceParticipant* media;
      bool prevShouldHold = shouldHold();
      if(dynamic_cast<LocalParticipant*>(participant))
      {
         mNumLocalParticipants--;
      }
      else if((remote = dynamic_cast<RemoteParticipant*>(participant)))
      {
         mNumRemoteParticipants--;
      }
      else if((media = dynamic_cast<MediaResourceParticipant*>(participant)))
      {
         mNumMediaParticipants--;
      }
      else
      {
         assert(false);
      }
      if(!mDestroying && prevShouldHold != shouldHold())
      {
         notifyRemoteParticipantsOfHoldChange();
      }

      participant->applyBridgeMixWeights(this);
      InfoLog(<< "Participant handle=" << participant->getParticipantHandle() << " removed from conversation handle=" << mHandle);

      if(mDestroying && mParticipants.size() == 0)
      {
         delete this;
      }
   }
}


/* ====================================================================

 Copyright (c) 2021, SIP Spectrum, Inc. www.sipspectrum.com
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
