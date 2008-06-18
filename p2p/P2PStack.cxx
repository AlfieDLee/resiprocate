#include "p2p/P2PStack.hxx"

using namespace p2p;

P2PStack::P2PStack(const Profile& profile) : 
   mProfile(profile),
   mDispatcher(),
   mTransporter(mProfile),
   mChord(mProfile, mDispatcher, mTransporter),
   mForwarder(mProfile, mDispatcher, mTransporter, mChord)
{
   mDispatcher.init(mForwarder);
}

void
P2PStack::run()
{
   while (1)
   {
      //process(1000);
   }
}