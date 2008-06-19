#if defined(HAVE_CONFIG_H)
#include "resiprocate/config.hxx"
#endif

#include "resiprocate/Contents.hxx"
#include "resiprocate/OctetContents.hxx"
#include "resiprocate/HeaderFieldValueList.hxx"
#include "resiprocate/SipMessage.hxx"
#include "resiprocate/UnknownHeaderType.hxx"
#include "resiprocate/os/Coders.hxx"
#include "resiprocate/os/CountStream.hxx"
#include "resiprocate/os/Logger.hxx"
#include "resiprocate/os/MD5Stream.hxx"
#include "resiprocate/os/compat.hxx"
#include "resiprocate/os/vmd5.hxx"
#include "resiprocate/os/Coders.hxx"
#include "resiprocate/os/Random.hxx"

#ifndef NEW_MSG_HEADER_SCANNER
#include "resiprocate/Preparse.hxx"
#else
#include "resiprocate/MsgHeaderScanner.hxx"
#endif

using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM Subsystem::SIP

SipMessage::SipMessage(const Transport* fromWire)
   : mIsExternal(fromWire != 0),
     mTransport(fromWire),
     mStartLine(0),
     mContentsHfv(0),
     mContents(0),
     mRFC2543TransactionId(),
     mRequest(false),
     mResponse(false),
     mCreatedTime(Timer::getTimeMicroSec()),
     mTarget(0),
     mTlsDomain(Data::Empty)
{
   for (int i = 0; i < Headers::MAX_HEADERS; i++)
   {
      mHeaders[i] = 0;
   }
}

SipMessage::SipMessage(const SipMessage& from)
   : mIsExternal(from.mIsExternal),
     mTransport(from.mTransport),
     mSource(from.mSource),
     mDestination(from.mDestination),
     mStartLine(0),
     mContentsHfv(0),
     mContents(0),
     mRFC2543TransactionId(from.mRFC2543TransactionId),
     mRequest(from.mRequest),
     mResponse(from.mResponse),
     mCreatedTime(Timer::getTimeMicroSec()),
     mTarget(0),
     mTlsDomain(from.mTlsDomain)
{
   if (this != &from)
   {
      for (int i = 0; i < Headers::MAX_HEADERS; i++)
      {
         if (from.mHeaders[i] != 0)
         {
            mHeaders[i] = new HeaderFieldValueList(*from.mHeaders[i]);
         }
         else
         {
            mHeaders[i] = 0;
         }
      }
  
      for (UnknownHeaders::const_iterator i = from.mUnknownHeaders.begin();
           i != from.mUnknownHeaders.end(); i++)
      {
         mUnknownHeaders.push_back(pair<Data, HeaderFieldValueList*>(
                                      i->first,
                                      new HeaderFieldValueList(*i->second)));
      }
      if (from.mStartLine != 0)
      {
         mStartLine = new HeaderFieldValueList(*from.mStartLine); 
      }
      if (from.mContents != 0)
      {
         mContents = from.mContents->clone();
      }
      else if (from.mContentsHfv != 0)
      {
         mContentsHfv = new HeaderFieldValue(*from.mContentsHfv);
      }
      else
      {
         // no body to copy
      }
      if (from.mTarget != 0)
      {
	 mTarget = new Uri(*from.mTarget);
      }
   }
}

SipMessage::~SipMessage()
{
   //DebugLog (<< "Deleting SipMessage: " << brief());
   
   for (int i = 0; i < Headers::MAX_HEADERS; i++)
   {
      delete mHeaders[i];
   }

   for (UnknownHeaders::iterator i = mUnknownHeaders.begin();
        i != mUnknownHeaders.end(); i++)
   {
      delete i->second;
   }
   
   for (vector<char*>::iterator i = mBufferList.begin();
        i != mBufferList.end(); i++)
   {
      delete [] *i;
   }
   
   delete mStartLine;
   delete mContents;
   delete mContentsHfv;
   delete mTarget;
}

SipMessage*
SipMessage::make(const Data& data, 
                 bool isExternal)
{
   Transport* external = (Transport*)(0xFFFF);
   SipMessage* msg = new SipMessage(isExternal ? external : 0);

   size_t len = data.size();
   char *buffer = new char[len + 5];

   msg->addBuffer(buffer);

   memcpy(buffer,data.data(), len);

#ifndef NEW_MSG_HEADER_SCANNER // {

   using namespace PreparseConst;
   Preparse pre;

   if (pre.process(*msg, buffer, len) || pre.isFragmented())
   {
      InfoLog(<< "Preparser failed: isfrag=" << pre.isFragmented() << " buff=" << buffer);
      
      delete msg;
      msg = 0;
   }
   else
   {
       size_t used = pre.nBytesUsed();
       assert(pre.nBytesUsed() == pre.nDiscardOffset());
       
       // no pp error
       if (pre.isHeadersComplete() &&
           used < len)
      {
         // body is present .. add it up.
         // NB. The Sip Message uses an overlay (again)
         // for the body. It ALSO expects that the body
         // will be contiguous (of course).
         msg->setBody(buffer+used, len-used);
      }
   }
   return msg;

#else //defined (NEW_MSG_HEADER_SCANNER) } {
   MsgHeaderScanner msgHeaderScanner;

   msgHeaderScanner.prepareForMessage(msg);

   char *unprocessedCharPtr;
   if (msgHeaderScanner.scanChunk(buffer, len, &unprocessedCharPtr) != MsgHeaderScanner::scrEnd)
   {
      InfoLog(<<"Preparse Rejecting buffer as unparsable / fragmented.");
      DebugLog(<< data);
      delete msg; 
      msg = 0; 
      return 0;
   }

   // no pp error
   unsigned int used = unprocessedCharPtr - buffer;

   if (used < len)
   {
      // body is present .. add it up.
      // NB. The Sip Message uses an overlay (again)
      // for the body. It ALSO expects that the body
      // will be contiguous (of course).
      // it doesn't need a new buffer in UDP b/c there
      // will only be one datagram per buffer. (1:1 strict)

      msg->setBody(buffer+used,len-used);
      //DebugLog(<<"added " << len-used << " byte body");
   }

   return msg;
#endif //defined (NEW_MSG_HEADER_SCANNER) }
}

const Data& 
SipMessage::getTransactionId() const
{
   // !jf! this should be caught early on just after preparsing and the message
   // should be rejected. 
   assert(!header(h_Vias).empty());
   if( exists(h_Vias) && header(h_Vias).front().exists(p_branch) 
       && header(h_Vias).front().param(p_branch).hasMagicCookie() )
   {
      assert (!header(h_Vias).front().param(p_branch).getTransactionId().empty());
      return header(h_Vias).front().param(p_branch).getTransactionId();
   }
   else
   {
      if (mRFC2543TransactionId.empty())
      {
         compute2543TransactionHash();
      }
      return mRFC2543TransactionId;
   }
}

void
SipMessage::compute2543TransactionHash() const
{
   assert (mRFC2543TransactionId.empty());
   
   /*  From rfc3261, 17.2.3
       The INVITE request matches a transaction if the Request-URI, To tag,
       From tag, Call-ID, CSeq, and top Via header field match those of the
       INVITE request which created the transaction.  In this case, the
       INVITE is a retransmission of the original one that created the
       transaction.  

       The ACK request matches a transaction if the Request-URI, From tag,
       Call-ID, CSeq number (not the method), and top Via header field match
       those of the INVITE request which created the transaction, and the To
       tag of the ACK matches the To tag of the response sent by the server
       transaction.  

       Matching is done based on the matching rules defined for each of those
       header fields.  Inclusion of the tag in the To header field in the ACK
       matching process helps disambiguate ACK for 2xx from ACK for other
       responses at a proxy, which may have forwarded both responses (This
       can occur in unusual conditions.  Specifically, when a proxy forked a
       request, and then crashes, the responses may be delivered to another
       proxy, which might end up forwarding multiple responses upstream).  An
       ACK request that matches an INVITE transaction matched by a previous
       ACK is considered a retransmission of that previous ACK.

       For all other request methods, a request is matched to a transaction
       if the Request-URI, To tag, From tag, Call-ID, CSeq (including the
       method), and top Via header field match those of the request that
       created the transaction.  Matching is done based on the matching
   */

   // If it is here and isn't a request, leave the transactionId empty, this
   // will cause the Transaction to send it statelessly

   if (isRequest())
   {
      MD5Stream strm;
      // See section 17.2.3 Matching Requests to Server Transactions in rfc 3261
         
      strm << header(h_RequestLine).uri().scheme();
      strm << header(h_RequestLine).uri().user();
      strm << header(h_RequestLine).uri().host();
      strm << header(h_RequestLine).uri().port();
      strm << header(h_RequestLine).uri().password();
      strm << header(h_RequestLine).uri().commutativeParameterHash();

      if (exists(h_Vias) && !header(h_Vias).empty())
      {
         strm << header(h_Vias).front().protocolName();
         strm << header(h_Vias).front().protocolVersion();
         strm << header(h_Vias).front().transport();
         strm << header(h_Vias).front().sentHost();
         strm << header(h_Vias).front().sentPort();
         strm << header(h_Vias).front().commutativeParameterHash();
      }
         
      if (header(h_From).exists(p_tag))
      {
         strm << header(h_From).param(p_tag);
      }

      // Only include the totag for non-invite requests
      if (header(h_RequestLine).getMethod() != INVITE && 
          header(h_RequestLine).getMethod() != ACK && 
          header(h_RequestLine).getMethod() != CANCEL &&
          header(h_To).exists(p_tag))
      {
         strm << header(h_To).param(p_tag);
      }

      strm << header(h_CallId).value();

      if (header(h_RequestLine).getMethod() == ACK || 
          header(h_RequestLine).getMethod() == CANCEL)
      {
         strm << INVITE;
         strm << header(h_CSeq).sequence();
      }
      else
      {
         strm << header(h_CSeq).method();
         strm << header(h_CSeq).sequence();
      }
           
      mRFC2543TransactionId = strm.getHex();
   }
   else
   {
      InfoLog (<< "Trying to compute a transaction id on a 2543 response. Drop the response");
      DebugLog (<< *this);
      throw Exception("Drop invalid 2543 response", __FILE__, __LINE__);
   }
}

const Data&
SipMessage::getRFC2543TransactionId() const
{
   if (mRFC2543TransactionId.empty())
   {
      compute2543TransactionHash();
   }
   return mRFC2543TransactionId;
}

void
SipMessage::setRFC2543TransactionId(const Data& tid)
{
   mRFC2543TransactionId = tid;
}

bool
SipMessage::isRequest() const
{
   return mRequest;
}

bool
SipMessage::isResponse() const
{
   return mResponse;
}

Data
SipMessage::brief() const
{
   Data result(128, true);

   static const Data request("SipRequest: ");
   static const Data response("SipResponse: ");
   static const Data tid(" tid=");
   static const Data cseq(" cseq=");
   static const Data slash(" / ");
   static const Data wire(" from(wire)");
   static const Data tu(" from(tu)");

   // !dlb! should be checked earlier 
   if (!exists(h_CSeq))

   {
      result = "MALFORMED; missing CSeq";
      return result;
   }
   
   if (isRequest()) 
   {
      result += request;
      MethodTypes meth = header(h_RequestLine).getMethod();
      if (meth != UNKNOWN)
      {
         result += MethodNames[meth];
      }
      else
      {
         result += header(h_RequestLine).unknownMethodName();
      }
      
      result += Symbols::SPACE;
      result += header(h_RequestLine).uri().getAor();
   }
   else if (isResponse())
   {
      result += response;
      result += Data(header(h_StatusLine).responseCode());
   }
   if (exists(h_Vias))
   {
     result += tid;
     result += getTransactionId();
   }
   else
     result += " MISSING-TID ";

   result += cseq;
   if (header(h_CSeq).method() != UNKNOWN)
   {
      result += MethodNames[header(h_CSeq).method()];
   }
   else
   {
      result += header(h_CSeq).unknownMethodName();
   }
   
   result += slash;
   result += Data(header(h_CSeq).sequence());
   result += mIsExternal ? wire : tu;
   
   return result;
}

bool
SipMessage::isClientTransaction() const
{
   assert(mRequest || mResponse);
   return ((mIsExternal && mResponse) || (!mIsExternal && mRequest));
}


// dynamic_cast &str to DataStream* to avoid CountStream?

std::ostream& 
SipMessage::encode(std::ostream& str) const
{
   if (mStartLine != 0)
   {
      mStartLine->encode(Data::Empty, str);
   }

   for (int i = 0; i < Headers::MAX_HEADERS; i++)
   {
      if (i != Headers::ContentLength) // !dlb! hack...
      {
         if (mHeaders[i] != 0)
         {
            mHeaders[i]->encode(i, str);
         }
      }
      else
      {
         if (mContents != 0)
         {
            CountStream cs;
            mContents->encode(cs);
            cs.flush();
            str << "Content-Length: " << cs.size() << "\r\n";
         }
         else if (mContentsHfv != 0)
         {
            str << "Content-Length: " << mContentsHfv->mFieldLength << "\r\n";
         }
         else
         {
            str << "Content-Length: 0\r\n";
         }
      }
   }

   for (UnknownHeaders::const_iterator i = mUnknownHeaders.begin(); 
        i != mUnknownHeaders.end(); i++)
   {
      i->second->encode(i->first, str);
   }

   str << Symbols::CRLF;
   
   if (mContents != 0)
   {
      mContents->encode(str);
   }
   else if (mContentsHfv != 0)
   {
      mContentsHfv->encode(str);
   }
   
   return str;
}

std::ostream& 
SipMessage::encodeEmbedded(std::ostream& str) const
{
   bool first = true;
   for (int i = 0; i < Headers::MAX_HEADERS; i++)
   {
      if (i != Headers::ContentLength)
      {
         if (mHeaders[i] != 0)
         {
            if (first)
            {
               str << Symbols::QUESTION;
               first = false;
            }
            else
            {
               str << Symbols::AMPERSAND;
            }
            mHeaders[i]->encodeEmbedded(Headers::getHeaderName(i), str);
         }
      }
   }

   for (UnknownHeaders::const_iterator i = mUnknownHeaders.begin(); 
        i != mUnknownHeaders.end(); i++)
   {
      if (first)
      {
         str << Symbols::QUESTION;
         first = false;
      }
      else
      {
         str << Symbols::AMPERSAND;
      }
      i->second->encodeEmbedded(i->first, str);
   }

   if (mContents != 0)
   {
      if (first)
      {
         str << Symbols::QUESTION;
      }
      else
      {
         str << Symbols::AMPERSAND;
      }
      str << "body=";
      // !dlb! encode escaped for characters
      Data contents;
      {
         DataStream s(contents);
         mContents->encode(s);
      }
      str << Embedded::encode(contents);
   }
   else if (mContentsHfv != 0)
   {
      if (first)
      {
         str << Symbols::QUESTION;
      }
      else
      {
         str << Symbols::AMPERSAND;
      }
      str << "body=";
      // !dlb! encode escaped for characters
      Data contents;
      {
         DataStream s(contents);
         mContentsHfv->encode(str);
      }
      str << Embedded::encode(contents);
   }
   
   return str;
}

void
SipMessage::addBuffer(char* buf)
{
   mBufferList.push_back(buf);
}

void 
SipMessage::setStartLine(const char* st, int len)
{
   mStartLine = new HeaderFieldValueList;
   mStartLine-> push_back(new HeaderFieldValue(st, len));
   ParseBuffer pb(st, len);
   const char* start;
   start = pb.skipWhitespace();
   pb.skipNonWhitespace();
   MethodTypes method = getMethodType(start, pb.position() - start);
   if (method == UNKNOWN) //probably a status line
   {
      start = pb.skipChar(Symbols::SPACE[0]);
      pb.skipNonWhitespace();
      if ((pb.position() - start) == 3)
      {
         mStartLine->setParserContainer(new ParserContainer<StatusLine>(mStartLine, Headers::NONE));
         //!dcm! should invoke the statusline parser here once it does limited validation
         mResponse = true;
      }
   }
   if (!mResponse)
   {
      mStartLine->setParserContainer(new ParserContainer<RequestLine>(mStartLine, Headers::NONE));
      //!dcm! should invoke the responseline parser here once it does limited validation
      mRequest = true;
   }
}

void 
SipMessage::setBody(const char* start, int len)
{
   mContentsHfv = new HeaderFieldValue(start, len);
}

void
SipMessage::setContents(auto_ptr<Contents> contents)
{
   Contents* contentsP = contents.release();

   delete mContents;
   mContents = 0;
   delete mContentsHfv;
   mContentsHfv = 0;

   if (contentsP == 0)
   {
      // The semantics of setContents(0) are to delete message contents
      remove(h_ContentType);
      remove(h_ContentDisposition);
      remove(h_ContentTransferEncoding);
      remove(h_ContentLanguages);
      return;
   }

   mContents = contentsP;

   // copy contents headers into message
   if (mContents->exists(h_ContentDisposition))
   {
      header(h_ContentDisposition) = mContents->header(h_ContentDisposition);
   }
   if (mContents->exists(h_ContentTransferEncoding))
   {
      header(h_ContentTransferEncoding) = mContents->header(h_ContentTransferEncoding);
   }
   if (mContents->exists(h_ContentLanguages))
   {
      header(h_ContentLanguages) = mContents->header(h_ContentLanguages);
   }
   if (mContents->exists(h_ContentType))
   {
      header(h_ContentType) = mContents->header(h_ContentType);
      assert( header(h_ContentType).type() == mContents->getType().type() );
      assert( header(h_ContentType).subType() == mContents->getType().subType() );
   }
   else
   {
      header(h_ContentType) = mContents->getType();
   }
}

void 
SipMessage::setContents(const Contents* contents)
{ 
   if (contents)
   {
      setContents(auto_ptr<Contents>(contents->clone()));
   }
   else
   {
      setContents(auto_ptr<Contents>(0));
   }
}

Contents*
SipMessage::getContents() const
{
   if (mContents == 0 && mContentsHfv != 0)
   {
      if (!exists(h_ContentType))
      {
         DebugLog(<< "SipMessage::getContents: ContentType header does not exist");
         return 0;
      }
      DebugLog(<< "SipMessage::getContents: " << header(h_ContentType));

      if ( Contents::getFactoryMap().find(header(h_ContentType)) == Contents::getFactoryMap().end() )
      {
         InfoLog(<< "SipMessage::getContents: got content type ("
                 <<  header(h_ContentType) << ") that is not known, "
                 << "returning as opaque application/octet-stream");
         mContents = Contents::getFactoryMap()[OctetContents::getStaticType()]->create(mContentsHfv, OctetContents::getStaticType());
      }
      else
      {
         mContents = Contents::getFactoryMap()[header(h_ContentType)]->create(mContentsHfv, header(h_ContentType));
      }
      
      // copy contents headers into the contents
      if (exists(h_ContentDisposition))
      {
         mContents->header(h_ContentDisposition) = header(h_ContentDisposition);
      }
      if (exists(h_ContentTransferEncoding))
      {
         mContents->header(h_ContentTransferEncoding) = header(h_ContentTransferEncoding);
      }
      if (exists(h_ContentLanguages))
      {
         mContents->header(h_ContentLanguages) = header(h_ContentLanguages);
      }
      if (exists(h_ContentType))
      {
         mContents->header(h_ContentType) = header(h_ContentType);
      }
      // !dlb! Content-Transfer-Encoding?
   }
   return mContents;
}

auto_ptr<Contents>
SipMessage::releaseContents()
{
   auto_ptr<Contents> ret(getContents());
   // the buffer may go away...
   ret->checkParsed();
   mContents = 0;
   // ...here
   setContents(auto_ptr<Contents>(0));
   
   return ret;
}

// unknown header interface
const StringCategories& 
SipMessage::header(const UnknownHeaderType& headerName) const
{
   for (UnknownHeaders::iterator i = mUnknownHeaders.begin();
        i != mUnknownHeaders.end(); i++)
   {
      // !dlb! case sensitive?
      if (i->first == headerName.getName())
      {
         HeaderFieldValueList* hfvs = i->second;
         if (hfvs->getParserContainer() == 0)
         {
            hfvs->setParserContainer(new ParserContainer<StringCategory>(hfvs, Headers::NONE));
         }
         return *dynamic_cast<ParserContainer<StringCategory>*>(hfvs->getParserContainer());
      }
   }
   // missing extension header
   assert(false);

   return *(StringCategories*)0;
}

StringCategories& 
SipMessage::header(const UnknownHeaderType& headerName)
{
   for (UnknownHeaders::iterator i = mUnknownHeaders.begin();
        i != mUnknownHeaders.end(); i++)
   {
      // !dlb! case sensitive?
      if (i->first == headerName.getName())
      {
         HeaderFieldValueList* hfvs = i->second;
         if (hfvs->getParserContainer() == 0)
         {
            hfvs->setParserContainer(new ParserContainer<StringCategory>(hfvs, Headers::NONE));
         }
         return *dynamic_cast<ParserContainer<StringCategory>*>(hfvs->getParserContainer());
      }
   }

   // create the list empty
   HeaderFieldValueList* hfvs = new HeaderFieldValueList;
   hfvs->setParserContainer(new ParserContainer<StringCategory>(hfvs, Headers::NONE));
   mUnknownHeaders.push_back(make_pair(headerName.getName(), hfvs));
   return *dynamic_cast<ParserContainer<StringCategory>*>(hfvs->getParserContainer());
}

bool
SipMessage::exists(const UnknownHeaderType& symbol) const
{
   for (UnknownHeaders::iterator i = mUnknownHeaders.begin();
        i != mUnknownHeaders.end(); i++)
   {
      if (i->first == symbol.getName())
      {
         return true;
      }
   }
   return false;
}

void
SipMessage::remove(const UnknownHeaderType& headerName)
{
   for (UnknownHeaders::iterator i = mUnknownHeaders.begin();
        i != mUnknownHeaders.end(); i++)
   {
      if (i->first == headerName.getName())
      {
         delete i->second;
         mUnknownHeaders.erase(i);
         return;
      }
   }
}

void
SipMessage::addHeader(Headers::Type header, const char* headerName, int headerLen, 
                      const char* start, int len)
{
   if (header != Headers::UNKNOWN)
   {
      if (mHeaders[header] == 0)
      {
         mHeaders[header] = new HeaderFieldValueList;
      }
      if (len)
      {
         mHeaders[header]->push_back(new HeaderFieldValue(start, len));
      }
   }
   else
   {
      for (UnknownHeaders::iterator i = mUnknownHeaders.begin();
           i != mUnknownHeaders.end(); i++)
      {
         if (strncasecmp(i->first.data(), headerName, headerLen) == 0)
         {
            // add to end of list
            if (len)
            {
               i->second->push_back(new HeaderFieldValue(start, len));
            }
            return;
         }
      }

      // didn't find it, add an entry
      HeaderFieldValueList *hfvs = new HeaderFieldValueList();
      if (len)
      {
         hfvs->push_back(new HeaderFieldValue(start, len));
      }
      mUnknownHeaders.push_back(pair<Data, HeaderFieldValueList*>(Data(headerName, headerLen),
                                                                  hfvs));
   }
}

Data&
SipMessage::getEncoded() 
{
   return mEncoded;
}

RequestLine& 
SipMessage::header(const RequestLineType& l)
{
   assert (!isResponse());
   if (mStartLine == 0 )
   { 
      mStartLine = new HeaderFieldValueList;
      mStartLine->push_back(new HeaderFieldValue);
      mStartLine->setParserContainer(new ParserContainer<RequestLine>(mStartLine, Headers::NONE));
      mRequest = true;
   }
   return dynamic_cast<ParserContainer<RequestLine>*>(mStartLine->getParserContainer())->front();
}

const RequestLine& 
SipMessage::header(const RequestLineType& l) const
{
   assert (!isResponse());
   if (mStartLine == 0 )
   { 
      // request line missing
      assert(false);
   }
   return dynamic_cast<ParserContainer<RequestLine>*>(mStartLine->getParserContainer())->front();
}

StatusLine& 
SipMessage::header(const StatusLineType& l)
{
   assert (!isRequest());
   if (mStartLine == 0 )
   { 
      mStartLine = new HeaderFieldValueList;
      mStartLine->push_back(new HeaderFieldValue);
      mStartLine->setParserContainer(new ParserContainer<StatusLine>(mStartLine, Headers::NONE));
      mResponse = true;
   }
   return dynamic_cast<ParserContainer<StatusLine>*>(mStartLine->getParserContainer())->front();
}

const StatusLine& 
SipMessage::header(const StatusLineType& l) const
{
   assert (!isRequest());
   if (mStartLine == 0 )
   { 
      // status line missing
      assert(false);
   }
   return dynamic_cast<ParserContainer<StatusLine>*>(mStartLine->getParserContainer())->front();
}

HeaderFieldValueList* 
SipMessage::ensureHeaders(Headers::Type type, bool single)
{
   HeaderFieldValueList* hfvs = mHeaders[type];
   
   // empty?
   if (hfvs == 0)
   {
      // create the list with a new component
      hfvs = new HeaderFieldValueList;
      mHeaders[type] = hfvs;
      if (single)
      {
         HeaderFieldValue* hfv = new HeaderFieldValue;
         hfvs->push_back(hfv);
      }
   }
   // !dlb! not thrilled about checking this every access
   else if (single)
   {
      if (hfvs->parsedEmpty())
      {
         // create an unparsed shared header field value // !dlb! when will this happen?
         hfvs->push_back(new HeaderFieldValue(Data::Empty.data(), 0));
      }
   }

   return hfvs;
}

HeaderFieldValueList* 
SipMessage::ensureHeaders(Headers::Type type, bool single) const
{
   HeaderFieldValueList* hfvs = mHeaders[type];
   
   // empty?
   if (hfvs == 0)
   {
      // header missing
      // assert(false);
      throw Exception("Missing header", __FILE__, __LINE__);
   }
   // !dlb! not thrilled about checking this every access
   else if (single)
   {
      if (hfvs->parsedEmpty())
      {
         // !dlb! when will this happen?
         // assert(false);
         throw Exception("Empty header", __FILE__, __LINE__);
      }
   }

   return hfvs;
}

// type safe header accessors
bool    
SipMessage::exists(const HeaderBase& headerType) const 
{
   return mHeaders[headerType.getTypeNum()] != 0;
};

void
SipMessage::remove(const HeaderBase& headerType)
{
   delete mHeaders[headerType.getTypeNum()]; 
   mHeaders[headerType.getTypeNum()] = 0; 
};

#ifndef PARTIAL_TEMPLATE_SPECIALIZATION

#undef defineHeader
#define defineHeader(_header)                                                                                   \
const _header##_Header::Type&                                                                                   \
SipMessage::header(const _header##_Header& headerType) const                                                    \
{                                                                                                               \
   HeaderFieldValueList* hfvs = ensureHeaders(headerType.getTypeNum(), true);                                   \
   if (hfvs->getParserContainer() == 0)                                                                         \
   {                                                                                                            \
      hfvs->setParserContainer(new ParserContainer<_header##_Header::Type>(hfvs, headerType.getTypeNum()));     \
   }                                                                                                            \
   return dynamic_cast<ParserContainer<_header##_Header::Type>*>(hfvs->getParserContainer())->front();          \
}                                                                                                               \
                                                                                                                \
_header##_Header::Type&                                                                                         \
SipMessage::header(const _header##_Header& headerType)                                                          \
{                                                                                                               \
   HeaderFieldValueList* hfvs = ensureHeaders(headerType.getTypeNum(), true);                                   \
   if (hfvs->getParserContainer() == 0)                                                                         \
   {                                                                                                            \
      hfvs->setParserContainer(new ParserContainer<_header##_Header::Type>(hfvs, headerType.getTypeNum()));     \
   }                                                                                                            \
   return dynamic_cast<ParserContainer<_header##_Header::Type>*>(hfvs->getParserContainer())->front();          \
}

#undef defineMultiHeader
#define defineMultiHeader(_header)                                                                                      \
const ParserContainer<_header##_MultiHeader::Type>&                                                                     \
SipMessage::header(const _header##_MultiHeader& headerType) const                                                       \
{                                                                                                                       \
   HeaderFieldValueList* hfvs = ensureHeaders(headerType.getTypeNum(), false);                                          \
   if (hfvs->getParserContainer() == 0)                                                                                 \
   {                                                                                                                    \
      hfvs->setParserContainer(new ParserContainer<_header##_MultiHeader::Type>(hfvs, headerType.getTypeNum()));        \
   }                                                                                                                    \
   return *dynamic_cast<ParserContainer<_header##_MultiHeader::Type>*>(hfvs->getParserContainer());                     \
}                                                                                                                       \
                                                                                                                        \
ParserContainer<_header##_MultiHeader::Type>&                                                                           \
SipMessage::header(const _header##_MultiHeader& headerType)                                                             \
{                                                                                                                       \
   HeaderFieldValueList* hfvs = ensureHeaders(headerType.getTypeNum(), false);                                          \
   if (hfvs->getParserContainer() == 0)                                                                                 \
   {                                                                                                                    \
      hfvs->setParserContainer(new ParserContainer<_header##_MultiHeader::Type>(hfvs, headerType.getTypeNum()));        \
   }                                                                                                                    \
   return *dynamic_cast<ParserContainer<_header##_MultiHeader::Type>*>(hfvs->getParserContainer());                     \
}

defineHeader(CSeq);
defineHeader(CallId);
defineHeader(AuthenticationInfo);
defineHeader(ContentDisposition);
defineHeader(ContentTransferEncoding);
defineHeader(ContentEncoding);
defineHeader(ContentLength);
defineHeader(ContentType);
defineHeader(Date);
defineHeader(Event);
defineHeader(Expires);
defineHeader(From);
defineHeader(InReplyTo);
defineHeader(MIMEVersion);
defineHeader(MaxForwards);
defineHeader(MinExpires);
defineHeader(Organization);
defineHeader(Priority);
defineHeader(ReferTo);
defineHeader(ReferredBy);
defineHeader(Replaces);
defineHeader(ReplyTo);
defineHeader(RetryAfter);
defineHeader(Server);
defineHeader(Subject);
defineHeader(Timestamp);
defineHeader(To);
defineHeader(UserAgent);
defineHeader(Warning);

defineMultiHeader(SecurityClient);
defineMultiHeader(SecurityServer);
defineMultiHeader(SecurityVerify);

defineMultiHeader(Authorization);
defineMultiHeader(ProxyAuthenticate);
defineMultiHeader(WWWAuthenticate);
defineMultiHeader(ProxyAuthorization);

defineMultiHeader(Accept);
defineMultiHeader(AcceptEncoding);
defineMultiHeader(AcceptLanguage);
defineMultiHeader(AlertInfo);
defineMultiHeader(Allow);
defineMultiHeader(AllowEvents);
defineMultiHeader(CallInfo);
defineMultiHeader(Contact);
defineMultiHeader(ContentLanguage);
defineMultiHeader(ErrorInfo);
defineMultiHeader(ProxyRequire);
defineMultiHeader(RecordRoute);
defineMultiHeader(Require);
defineMultiHeader(Route);
defineMultiHeader(SubscriptionState);
defineMultiHeader(Supported);
defineMultiHeader(Unsupported);
defineMultiHeader(Via);
#endif

const HeaderFieldValueList*
SipMessage::getRawHeader(Headers::Type headerType) const
{
   return mHeaders[headerType];
}

void
SipMessage::setRawHeader(const HeaderFieldValueList* hfvs, Headers::Type headerType)
{
   if (mHeaders[headerType] != hfvs)
   {
      delete mHeaders[headerType];
      mHeaders[headerType] = new HeaderFieldValueList(*hfvs);
   }
}

void
SipMessage::setTarget(const Uri& uri)
{
   if (mTarget)
   {
      DebugLog(<< "SipMessage::setTarget: replacing forced target");
      *mTarget = uri;
   }
   else
   {
      mTarget = new Uri(uri);
   }
}

void
SipMessage::clearTarget()
{
   delete mTarget;
   mTarget = 0;
}

const Uri&
SipMessage::getTarget() const
{
   assert(mTarget);
   return *mTarget;
}

bool
SipMessage::hasTarget() const
{
   return (mTarget != 0);
}

#if defined(DEBUG) && defined(DEBUG_MEMORY)
namespace resip
{

void*
operator new(size_t size)
{
   void * p = std::operator new(size);
   DebugLog(<<"operator new | " << hex << p << " | "
            << dec << size);
   if (size == 60)
   {
      3;
   }
   
   return p;
}

void operator delete(void* p)
{
   DebugLog(<<"operator delete | " << hex << p << dec);
   return std::operator delete( p );
}

void operator delete[](void* p)
{
   DebugLog(<<"operator delete [] | " << hex << p << dec);
   return std::operator delete[] ( p );
}
 
}

#endif

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