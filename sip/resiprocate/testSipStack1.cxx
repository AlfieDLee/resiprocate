#include <iostream>

#ifndef WIN32
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <sipstack/SipStack.hxx>
#include <sipstack/Uri.hxx>
#include <util/Logger.hxx>
#include <sipstack/Helper.hxx>

using namespace Vocal2;
using namespace std;

#define VOCAL_SUBSYSTEM Subsystem::SIP

int
main(int argc, char* argv[])
{
   Log::initialize(Log::COUT, Log::DEBUG, argv[0]);
   DebugLog (<< "hi there");
   
   initNetwork();
	
   SipStack sipStack;
   SipMessage* msg=NULL;

  while (1)
   {
      struct timeval tv;
      fd_set fdSet; int fdSetSize;
      FD_ZERO(&fdSet); fdSetSize=0;
       
      sipStack.buildFdSet(&fdSet,&fdSetSize);
	  
      tv.tv_sec=0;
      tv.tv_usec= 1000 * sipStack.getTimeTillNextProcess();
	  
      int  err = select(fdSetSize, &fdSet, NULL, NULL, &tv);
      int e = errno;
      if ( err == -1 )
      {
         // error occured
         cerr << "Error " << e << " " << strerror(e) << " in select" << endl;
      }
      
      //DebugLog ( << "Try TO PROCESS " );
      sipStack.process(&fdSet);

      //DebugLog ( << "Try TO receive " );
      msg = sipStack.receive();
      if ( msg )
      {
         DebugLog ( << "got message: " << *msg);
	   
         msg->encode(cerr);	  
      }

#if 0
      static int count=0;
      if ( count++ >= 10 )
      {   
	      DebugLog ( << "Try to send a message" );
	      count = 0;
	      
	      NameAddr dest;
	      NameAddr from;
	      NameAddr contact;
	      from.uri().scheme() = Data("sip");
	      from.uri().host() = Data("foo.com");
	      from.uri().port() = 5060;
	      from.uri().user() = Data("fluffy");
	      from.uri().param(p_transport) == "udp";
	      
	      dest = from;
	      contact = from;
	            
	      SipMessage message = Helper::makeInvite( dest ,
						       from,
						       contact);
	      
	      sipStack.send( message );
	      
	       DebugLog ( << "Sent Msg" << message );
      }
#endif
   }
}


/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 */
