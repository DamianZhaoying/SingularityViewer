/**
 * @file aicurl.cpp
 * @brief Implementation of AICurl.
 *
 * Copyright (c) 2012, Aleric Inglewood.
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution.
 *
 * CHANGELOG
 *   and additional copyright holders.
 *
 *   17/03/2012
 *   Initial version, written by Aleric Inglewood @ SL
 *
 *   20/03/2012
 *   Added copyright notice for Linden Lab for those parts that were
 *   copied or derived from llcurl.cpp. The code of those parts are
 *   already in their own llcurl.cpp, so they do not ever need to
 *   even look at this file; the reason I added the copyright notice
 *   is to make clear that I am not the author of 100% of this code
 *   and hence I cannot change the license of it.
 */

#include "linden_common.h"

#define OPENSSL_THREAD_DEFINES
#include <openssl/opensslconf.h>	// OPENSSL_THREADS
#include <openssl/crypto.h>

#include "aicurl.h"
#include "llbufferstream.h"
#include "llsdserialize.h"
#include "aithreadsafe.h"
#include "llqueuedthread.h"
#include "lltimer.h"		// ms_sleep
#include "llproxy.h"
#include "llhttpstatuscodes.h"
#ifdef CWDEBUG
#include <libcwd/buf2str.h>
#endif

//==================================================================================
// Local variables.
//

namespace {

struct CertificateAuthority {
  std::string file;
  std::string path;
};

AIThreadSafeSimpleDC<CertificateAuthority> gCertificateAuthority;
typedef AIAccess<CertificateAuthority> CertificateAuthority_wat;
typedef AIAccessConst<CertificateAuthority> CertificateAuthority_rat;

enum gSSLlib_type {
  ssl_unknown,
  ssl_openssl,
  ssl_gnutls,
  ssl_nss
};

// No locking needed: initialized before threads are created, and subsequently only read.
gSSLlib_type gSSLlib;
bool gSetoptParamsNeedDup;

} // namespace

// See http://www.openssl.org/docs/crypto/threads.html:
// CRYPTO_THREADID and associated functions were introduced in OpenSSL 1.0.0 to replace
// (actually, deprecate) the previous CRYPTO_set_id_callback(), CRYPTO_get_id_callback(),
// and CRYPTO_thread_id() functions which assumed thread IDs to always be represented by
// 'unsigned long'.
#define HAVE_CRYPTO_THREADID (OPENSSL_VERSION_NUMBER >= (1 << 28))

//-----------------------------------------------------------------------------------
// Needed for thread-safe openSSL operation.

// Must be defined in global namespace.
struct CRYPTO_dynlock_value
{
  AIRWLock rwlock;
};

namespace {

AIRWLock* ssl_rwlock_array;

// OpenSSL locking function.
void ssl_locking_function(int mode, int n, char const* file, int line)
{
  if ((mode & CRYPTO_LOCK))
  {
	if ((mode & CRYPTO_READ))
	  ssl_rwlock_array[n].rdlock();
	else
	  ssl_rwlock_array[n].wrlock();
  }
  else
  {
    if ((mode & CRYPTO_READ))
	  ssl_rwlock_array[n].rdunlock();
	else
	  ssl_rwlock_array[n].wrunlock();
  }
}

#if HAVE_CRYPTO_THREADID
// OpenSSL uniq id function.
void ssl_id_function(CRYPTO_THREADID* thread_id)
{
#if 1	// apr_os_thread_current() returns an unsigned long.
  CRYPTO_THREADID_set_numeric(thread_id, apr_os_thread_current());
#else	// if it would return a pointer.
  CRYPTO_THREADID_set_pointer(thread_id, apr_os_thread_current());
#endif
}
#endif // HAVE_CRYPTO_THREADID

// OpenSSL allocate and initialize dynamic crypto lock.
CRYPTO_dynlock_value* ssl_dyn_create_function(char const* file, int line)
{
  return new CRYPTO_dynlock_value;
}

// OpenSSL destroy dynamic crypto lock.
void ssl_dyn_destroy_function(CRYPTO_dynlock_value* l, char const* file, int line)
{
  delete l;
}

// OpenSSL dynamic locking function.
void ssl_dyn_lock_function(int mode, CRYPTO_dynlock_value* l, char const* file, int line)
{
  if ((mode & CRYPTO_LOCK))
  {
	if ((mode & CRYPTO_READ))
	  l->rwlock.rdlock();
	else
	  l->rwlock.wrlock();
  }
  else
  {
    if ((mode & CRYPTO_READ))
	  l->rwlock.rdunlock();
	else
	  l->rwlock.wrunlock();
  }
}

typedef void (*ssl_locking_function_type)(int, int, char const*, int);
#if HAVE_CRYPTO_THREADID
typedef void (*ssl_id_function_type)(CRYPTO_THREADID*);
#else
typedef unsigned long (*ulong_thread_id_function_type)(void);
#endif
typedef CRYPTO_dynlock_value* (*ssl_dyn_create_function_type)(char const*, int);
typedef void (*ssl_dyn_destroy_function_type)(CRYPTO_dynlock_value*, char const*, int);
typedef void (*ssl_dyn_lock_function_type)(int, CRYPTO_dynlock_value*, char const*, int);

ssl_locking_function_type     old_ssl_locking_function;
#if HAVE_CRYPTO_THREADID
ssl_id_function_type          old_ssl_id_function;
#else
ulong_thread_id_function_type old_ulong_thread_id_function;
#endif
ssl_dyn_create_function_type  old_ssl_dyn_create_function;
ssl_dyn_destroy_function_type old_ssl_dyn_destroy_function;
ssl_dyn_lock_function_type    old_ssl_dyn_lock_function;

// Initialize OpenSSL library for thread-safety.
void ssl_init(void)
{
  // The version identifier format is: MMNNFFPPS: major minor fix patch status.
  int const compiled_openSLL_major = (OPENSSL_VERSION_NUMBER >> 28) & 0xff;
  int const compiled_openSLL_minor = (OPENSSL_VERSION_NUMBER >> 20) & 0xff;
  int const linked_openSLL_major = (SSLeay() >> 28) & 0xff;
  int const linked_openSLL_minor = (SSLeay() >> 20) & 0xff;
  // Check if dynamically loaded version is compatible with the one we compiled against.
  // As off version 1.0.0 also minor versions are compatible.
  if (linked_openSLL_major != compiled_openSLL_major ||
	  (compiled_openSLL_major == 0 && linked_openSLL_minor != compiled_openSLL_minor))
  {
	llerrs << "The viewer was compiled against " << OPENSSL_VERSION_TEXT <<
	    " but linked against " << SSLeay_version(SSLEAY_VERSION) <<
		". Those versions are not compatible." << llendl;
  }
  // Static locks vector.
  ssl_rwlock_array = new AIRWLock[CRYPTO_num_locks()];
  // Static locks callbacks.
  old_ssl_locking_function = CRYPTO_get_locking_callback();
#if HAVE_CRYPTO_THREADID
  old_ssl_id_function = CRYPTO_THREADID_get_callback();
#else
  old_ulong_thread_id_function = CRYPTO_get_id_callback();
#endif
  CRYPTO_set_locking_callback(&ssl_locking_function);
  // Setting this avoids the need for a thread-local error number facility, which is hard to check.
#if HAVE_CRYPTO_THREADID
  CRYPTO_THREADID_set_callback(&ssl_id_function);
#else
  CRYPTO_set_id_callback(&apr_os_thread_current);
#endif
  // Dynamic locks callbacks.
  old_ssl_dyn_create_function = CRYPTO_get_dynlock_create_callback();
  old_ssl_dyn_lock_function = CRYPTO_get_dynlock_lock_callback();
  old_ssl_dyn_destroy_function = CRYPTO_get_dynlock_destroy_callback();
  CRYPTO_set_dynlock_create_callback(&ssl_dyn_create_function);
  CRYPTO_set_dynlock_lock_callback(&ssl_dyn_lock_function);
  CRYPTO_set_dynlock_destroy_callback(&ssl_dyn_destroy_function);
  llinfos << "Successful initialization of " <<
	  SSLeay_version(SSLEAY_VERSION) << " (0x" << std::hex << SSLeay() << ")." << llendl;
}

// Cleanup OpenSSL library thread-safety.
void ssl_cleanup(void)
{
  // Dynamic locks callbacks.
  CRYPTO_set_dynlock_destroy_callback(old_ssl_dyn_destroy_function);
  CRYPTO_set_dynlock_lock_callback(old_ssl_dyn_lock_function);
  CRYPTO_set_dynlock_create_callback(old_ssl_dyn_create_function);
  // Static locks callbacks.
#if HAVE_CRYPTO_THREADID
  CRYPTO_THREADID_set_callback(old_ssl_id_function);
#else
  CRYPTO_set_id_callback(old_ulong_thread_id_function);
#endif
  CRYPTO_set_locking_callback(old_ssl_locking_function);
  // Static locks vector.
  delete [] ssl_rwlock_array;
}

} // namespace openSSL
//-----------------------------------------------------------------------------------

static unsigned int encoded_version(int major, int minor, int patch)
{
  return (major << 16) | (minor << 8) | patch;
}

//==================================================================================
// External API
//

#undef AICurlPrivate

namespace AICurlInterface {

// MAIN-THREAD
void initCurl(F32 curl_request_timeout, S32 max_number_handles)
{
  DoutEntering(dc::curl, "AICurlInterface::initCurl(" << curl_request_timeout << ", " << max_number_handles << ")");

  llassert(LLThread::getRunning() == 0);		// We must not call curl_global_init unless we are the only thread.
  CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
  if (res != CURLE_OK)
  {
	llerrs << "curl_global_init(CURL_GLOBAL_ALL) failed." << llendl;
  }

  // Print version and do some feature sanity checks.
  {
	curl_version_info_data* version_info = curl_version_info(CURLVERSION_NOW);

	llassert_always(version_info->age >= 0);
	if (version_info->age < 1)
	{
	  llwarns << "libcurl's age is 0; no ares support." << llendl;
	}
	llassert_always((version_info->features & CURL_VERSION_SSL));	// SSL support, added in libcurl 7.10.
	if (!(version_info->features & CURL_VERSION_ASYNCHDNS));		// Asynchronous name lookups (added in libcurl 7.10.7).
	{
	  llwarns << "libcurl was not compiled with support for asynchronous name lookups!" << llendl;
	}

	llinfos << "Successful initialization of libcurl " <<
		version_info->version << " (0x" << std::hex << version_info->version_num << "), (" <<
	    version_info->ssl_version << ", libz/" << version_info->libz_version << ")." << llendl;

	// Detect SSL library used.
	gSSLlib = ssl_unknown;
	std::string ssl_version(version_info->ssl_version);
	if (ssl_version.find("OpenSSL") != std::string::npos)
	  gSSLlib = ssl_openssl;										// See http://www.openssl.org/docs/crypto/threads.html#DESCRIPTION
	else if (ssl_version.find("GnuTLS") != std::string::npos)
	  gSSLlib = ssl_gnutls;											// See http://www.gnu.org/software/gnutls/manual/html_node/Thread-safety.html
	else if (ssl_version.find("NSS") != std::string::npos)
	  gSSLlib = ssl_nss;											// Supposedly thread-safe without any requirements.

	// Set up thread-safety requirements of underlaying SSL library.
	// See http://curl.haxx.se/libcurl/c/libcurl-tutorial.html
	switch (gSSLlib)
	{
	  case ssl_unknown:
	  {
		llerrs << "Unknown SSL library \"" << version_info->ssl_version << "\", required actions for thread-safe handling are unknown! Bailing out." << llendl;
	  }
	  case ssl_openssl:
	  {
#ifndef OPENSSL_THREADS
		llerrs << "OpenSSL was not configured with thread support! Bailing out." << llendl;
#endif
		ssl_init();
	  }
	  case ssl_gnutls:
	  {
		// I don't think we ever get here, do we? --Aleric
		llassert_always(gSSLlib != ssl_gnutls);
		// If we do, then didn't curl_global_init already call gnutls_global_init?
		// It seems there is nothing to do for us here.
	  }
	  case ssl_nss:
	  {
	    break;	// No requirements.
	  }
	}

	// Before version 7.17.0, strings were not copied. Instead the user was forced keep them available until libcurl no longer needed them.
	gSetoptParamsNeedDup = (version_info->version_num < encoded_version(7, 17, 0));
	if (gSetoptParamsNeedDup)
	{
	  llwarns << "Your libcurl version is too old." << llendl;
	}
	llassert_always(!gSetoptParamsNeedDup);		// Might add support later.
  }
}

// MAIN-THREAD
void cleanupCurl(void)
{
  using AICurlPrivate::stopCurlThread;
  using AICurlPrivate::curlThreadIsRunning;

  DoutEntering(dc::curl, "AICurlInterface::cleanupCurl()");

  stopCurlThread();
  ssl_cleanup();

  llassert(LLThread::getRunning() <= (curlThreadIsRunning() ? 1 : 0));		// We must not call curl_global_cleanup unless we are the only thread left.
  curl_global_cleanup();
}

// THREAD-SAFE
std::string getVersionString(void)
{
  // libcurl is thread safe, no locking needed.
  return curl_version();
}

// THREAD-SAFE
void setCAFile(std::string const& file)
{
  CertificateAuthority_wat CertificateAuthority_w(gCertificateAuthority);
  CertificateAuthority_w->file = file;
}

// This function is not called from anywhere, but provided as part of AICurlInterface because setCAFile is.
// THREAD-SAFE
void setCAPath(std::string const& path)
{
  CertificateAuthority_wat CertificateAuthority_w(gCertificateAuthority);
  CertificateAuthority_w->path = path;
}

// THREAD-SAFE
std::string strerror(CURLcode errorcode)
{
  // libcurl is thread safe, no locking needed.
  return curl_easy_strerror(errorcode);
}

//-----------------------------------------------------------------------------
// class Responder
//

Responder::Responder(void) : mReferenceCount(0)
{
  DoutEntering(dc::curl, "AICurlInterface::Responder() with this = " << (void*)this);
}

Responder::~Responder()
{
  DoutEntering(dc::curl, "AICurlInterface::Responder::~Responder() with this = " << (void*)this << "; mReferenceCount = " << mReferenceCount);
  llassert(mReferenceCount == 0);
}

void Responder::setURL(std::string const& url)
{
  // setURL is called from llhttpclient.cpp (request()), before calling any of the below (of course).
  // We don't need locking here therefore; it's a case of initializing before use.
  mURL = url;
}

// Called with HTML header.
// virtual
void Responder::completedHeader(U32, std::string const&, LLSD const&)
{
  // Nothing.
}

// Called with HTML body.
// virtual
void Responder::completedRaw(U32 status, std::string const& reason, LLChannelDescriptors const& channels, LLIOPipe::buffer_ptr_t const& buffer)
{
  LLSD content;
  LLBufferStream istr(channels, buffer.get());
  if (!LLSDSerialize::fromXML(content, istr))
  {
	llinfos << "Failed to deserialize LLSD. " << mURL << " [" << status << "]: " << reason << llendl;
  }

  // Allow derived class to override at this point.
  completed(status, reason, content);
}

void Responder::fatalError(std::string const& reason)
{
  llwarns << "Responder::fatalError(\"" << reason << "\") is called (" << mURL << "). Passing it to Responder::completed with fake HTML error status and empty HTML body!" << llendl;
  completed(U32_MAX, reason, LLSD());
}

// virtual
void Responder::completed(U32 status, std::string const& reason, LLSD const& content)
{
  // HTML status good?
  if (200 <= status && status < 300)
  {
	// Allow derived class to override at this point.
	result(content);
  }
  else
  {
	// Allow derived class to override at this point.
	errorWithContent(status, reason, content);
  }
}

// virtual
void Responder::errorWithContent(U32 status, std::string const& reason, LLSD const&)
{
  // Allow derived class to override at this point.
  error(status, reason);
}

// virtual
void Responder::error(U32 status, std::string const& reason)
{
  llinfos << mURL << " [" << status << "]: " << reason << llendl;
}

// virtual
void Responder::result(LLSD const&)
{
  // Nothing.
}

// Friend functions.

void intrusive_ptr_add_ref(Responder* responder)
{
  responder->mReferenceCount++;
}

void intrusive_ptr_release(Responder* responder)
{
  if (--responder->mReferenceCount == 0)
  {
	delete responder;
  }
}

} // namespace AICurlInterface
//==================================================================================


//==================================================================================
// Local implementation.
//

namespace AICurlPrivate {

//static
LLAtomicU32 Stats::easy_calls;
LLAtomicU32 Stats::easy_errors;
LLAtomicU32 Stats::easy_init_calls;
LLAtomicU32 Stats::easy_init_errors;
LLAtomicU32 Stats::easy_cleanup_calls;
LLAtomicU32 Stats::multi_calls;
LLAtomicU32 Stats::multi_errors;

//static
void Stats::print(void)
{
  llinfos << "============ CURL  STATS ============" << llendl;
  llinfos << "  Curl multi       errors/calls      : " << std::dec << multi_errors << "/" << multi_calls << llendl;
  llinfos << "  Curl easy        errors/calls      : " << std::dec << easy_errors << "/" << easy_calls << llendl;
  llinfos << "  curl_easy_init() errors/calls      : " << std::dec << easy_init_errors << "/" << easy_init_calls << llendl;
  llinfos << "  Current number of curl easy handles: " << std::dec << (easy_init_calls - easy_init_errors - easy_cleanup_calls) << llendl;
  llinfos << "========= END OF CURL STATS =========" << llendl;
}

// THREAD-SAFE
void handle_multi_error(CURLMcode code) 
{
  Stats::multi_errors++;
  llinfos << "curl multi error detected: " << curl_multi_strerror(code) <<
	  "; (errors/calls = " << Stats::multi_errors << "/" << Stats::multi_calls << ")" << llendl;
}

//=============================================================================
// AICurlEasyRequest (base classes)
//

//-----------------------------------------------------------------------------
// CurlEasyHandle

// THREAD-SAFE
//static
void CurlEasyHandle::handle_easy_error(CURLcode code)
{
  char* error_buffer = LLThreadLocalData::tldata().mCurlErrorBuffer;
  llinfos << "curl easy error detected: " << curl_easy_strerror(code);
  if (error_buffer && *error_buffer != '\0')
  {
	llcont << ": " << error_buffer;
  }
  Stats::easy_errors++;
  llcont << "; (errors/calls = " << Stats::easy_errors << "/" << Stats::easy_calls << ")" << llendl;
}

// Throws AICurlNoEasyHandle.
CurlEasyHandle::CurlEasyHandle(void) : mActiveMultiHandle(NULL), mErrorBuffer(NULL)
{
  mEasyHandle = curl_easy_init();
#if 0
  // Fake curl_easy_init() failures: throw once every 10 times (for debugging purposes).
  static int count = 0;
  if (mEasyHandle && (++count % 10) == 5)
  {
    curl_easy_cleanup(mEasyHandle);
	mEasyHandle = NULL;
  }
#endif
  Stats::easy_init_calls++;
  if (!mEasyHandle)
  {
	Stats::easy_init_errors++;
	throw AICurlNoEasyHandle("curl_easy_init() returned NULL");
  }
}

CurlEasyHandle::~CurlEasyHandle()
{
  llassert(!mActiveMultiHandle);
  curl_easy_cleanup(mEasyHandle);
  Stats::easy_cleanup_calls++;
}

//static
char* CurlEasyHandle::getTLErrorBuffer(void)
{
  LLThreadLocalData& tldata = LLThreadLocalData::tldata();
  if (!tldata.mCurlErrorBuffer)
  {
	tldata.mCurlErrorBuffer = new char[CURL_ERROR_SIZE];
  }
  return tldata.mCurlErrorBuffer;
}

void CurlEasyHandle::setErrorBuffer(void)
{
  char* error_buffer = getTLErrorBuffer();
  if (mErrorBuffer != error_buffer)
  {
	mErrorBuffer = error_buffer;
	CURLcode res = curl_easy_setopt(mEasyHandle, CURLOPT_ERRORBUFFER, error_buffer);
	if (res != CURLE_OK)
	{
	  llwarns << "curl_easy_setopt(" << (void*)mEasyHandle << "CURLOPT_ERRORBUFFER, " << (void*)error_buffer << ") failed with error " << res << llendl;
	  mErrorBuffer = NULL;
	}
  }
}

CURLcode CurlEasyHandle::getinfo(CURLINFO info, void* data)
{
  setErrorBuffer();
  return check_easy_code(curl_easy_getinfo(mEasyHandle, info, data));
}

char* CurlEasyHandle::escape(char* url, int length)
{
  return curl_easy_escape(mEasyHandle, url, length);
}

char* CurlEasyHandle::unescape(char* url, int inlength , int* outlength)
{
  return curl_easy_unescape(mEasyHandle, url, inlength, outlength);
}

CURLcode CurlEasyHandle::perform(void)
{
  llassert(!mActiveMultiHandle);
  setErrorBuffer();
  return check_easy_code(curl_easy_perform(mEasyHandle));
}

CURLcode CurlEasyHandle::pause(int bitmask)
{
  setErrorBuffer();
  return check_easy_code(curl_easy_pause(mEasyHandle, bitmask));
}

CURLMcode CurlEasyHandle::add_handle_to_multi(AICurlEasyRequest_wat& curl_easy_request_w, CURLM* multi)
{
  llassert_always(!mActiveMultiHandle && multi);
  mActiveMultiHandle = multi;
  CURLMcode res = check_multi_code(curl_multi_add_handle(multi, mEasyHandle));
  added_to_multi_handle(curl_easy_request_w);
  return res;
}

CURLMcode CurlEasyHandle::remove_handle_from_multi(AICurlEasyRequest_wat& curl_easy_request_w, CURLM* multi)
{
  llassert_always(mActiveMultiHandle && mActiveMultiHandle == multi);
  mActiveMultiHandle = NULL;
  CURLMcode res = check_multi_code(curl_multi_remove_handle(multi, mEasyHandle));
  removed_from_multi_handle(curl_easy_request_w);
  return res;
}

void intrusive_ptr_add_ref(ThreadSafeCurlEasyRequest* threadsafe_curl_easy_request)
{
  threadsafe_curl_easy_request->mReferenceCount++;
}

void intrusive_ptr_release(ThreadSafeCurlEasyRequest* threadsafe_curl_easy_request)
{
  if (--threadsafe_curl_easy_request->mReferenceCount == 0)
  {
	delete threadsafe_curl_easy_request;
  }
}

//-----------------------------------------------------------------------------
// CurlEasyReqest

void CurlEasyRequest::setoptString(CURLoption option, std::string const& value)
{
  llassert(!gSetoptParamsNeedDup);
  setopt(option, value.c_str());
}

void CurlEasyRequest::setPost(char const* postdata, S32 size)
{
  setopt(CURLOPT_POST, 1L);
  setopt(CURLOPT_POSTFIELDS, static_cast<void*>(const_cast<char*>(postdata)));
  setopt(CURLOPT_POSTFIELDSIZE, size);
}

ThreadSafeCurlEasyRequest* CurlEasyRequest::get_lockobj(void)
{
  return static_cast<ThreadSafeCurlEasyRequest*>(AIThreadSafeSimpleDC<CurlEasyRequest>::wrapper_cast(this));
}

//static
size_t CurlEasyRequest::headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  CurlEasyRequest* self = static_cast<CurlEasyRequest*>(userdata);
  ThreadSafeCurlEasyRequest* lockobj = self->get_lockobj();
  AICurlEasyRequest_wat lock_self(*lockobj);
  return self->mHeaderCallback(ptr, size, nmemb, self->mHeaderCallbackUserData);
}

void CurlEasyRequest::setHeaderCallback(curl_write_callback callback, void* userdata)
{
  mHeaderCallback = callback;
  mHeaderCallbackUserData = userdata;
  setopt(CURLOPT_HEADERFUNCTION, callback ? &CurlEasyRequest::headerCallback : NULL);
  setopt(CURLOPT_WRITEHEADER, userdata ? this : NULL);
}

//static
size_t CurlEasyRequest::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  CurlEasyRequest* self = static_cast<CurlEasyRequest*>(userdata);
  ThreadSafeCurlEasyRequest* lockobj = self->get_lockobj();
  AICurlEasyRequest_wat lock_self(*lockobj);
  return self->mWriteCallback(ptr, size, nmemb, self->mWriteCallbackUserData);
}

void CurlEasyRequest::setWriteCallback(curl_write_callback callback, void* userdata)
{
  mWriteCallback = callback;
  mWriteCallbackUserData = userdata;
  setopt(CURLOPT_WRITEFUNCTION, callback ? &CurlEasyRequest::writeCallback : NULL);
  setopt(CURLOPT_WRITEDATA, userdata ? this : NULL);
}

//static
size_t CurlEasyRequest::readCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  CurlEasyRequest* self = static_cast<CurlEasyRequest*>(userdata);
  ThreadSafeCurlEasyRequest* lockobj = self->get_lockobj();
  AICurlEasyRequest_wat lock_self(*lockobj);
  return self->mReadCallback(ptr, size, nmemb, self->mReadCallbackUserData);
}

void CurlEasyRequest::setReadCallback(curl_read_callback callback, void* userdata)
{
  mReadCallback = callback;
  mReadCallbackUserData = userdata;
  setopt(CURLOPT_READFUNCTION, callback ? &CurlEasyRequest::readCallback : NULL);
  setopt(CURLOPT_READDATA, userdata ? this : NULL);
}

//static
CURLcode CurlEasyRequest::SSLCtxCallback(CURL* curl, void* sslctx, void* userdata)
{
  CurlEasyRequest* self = static_cast<CurlEasyRequest*>(userdata);
  ThreadSafeCurlEasyRequest* lockobj = self->get_lockobj();
  AICurlEasyRequest_wat lock_self(*lockobj);
  return self->mSSLCtxCallback(curl, sslctx, self->mSSLCtxCallbackUserData);
}

void CurlEasyRequest::setSSLCtxCallback(curl_ssl_ctx_callback callback, void* userdata)
{
  mSSLCtxCallback = callback;
  mSSLCtxCallbackUserData = userdata;
  setopt(CURLOPT_SSL_CTX_FUNCTION, callback ? &CurlEasyRequest::SSLCtxCallback : NULL);
  setopt(CURLOPT_SSL_CTX_DATA, userdata ? this : NULL);
}

#define llmaybewarns lllog(LLApp::isExiting() ? LLError::LEVEL_INFO : LLError::LEVEL_WARN, NULL, NULL, false)

static size_t noHeaderCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  llmaybewarns << "Calling noHeaderCallback(); curl session aborted." << llendl;
  return 0;							// Cause a CURL_WRITE_ERROR
}

static size_t noWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  llmaybewarns << "Calling noWriteCallback(); curl session aborted." << llendl;
  return 0;							// Cause a CURL_WRITE_ERROR
}

static size_t noReadCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  llmaybewarns << "Calling noReadCallback(); curl session aborted." << llendl;
  return CURL_READFUNC_ABORT;		// Cause a CURLE_ABORTED_BY_CALLBACK
}

static CURLcode noSSLCtxCallback(CURL* curl, void* sslctx, void* parm)
{
  llmaybewarns << "Calling noSSLCtxCallback(); curl session aborted." << llendl;
  return CURLE_ABORTED_BY_CALLBACK;
}

void CurlEasyRequest::revokeCallbacks(void)
{
  if (mHeaderCallback == &noHeaderCallback &&
	  mWriteCallback == &noWriteCallback &&
	  mReadCallback == &noReadCallback &&
	  mSSLCtxCallback == &noSSLCtxCallback)
  {
	// Already revoked.
	return;
  }
  mHeaderCallback = &noHeaderCallback;
  mWriteCallback = &noWriteCallback;
  mReadCallback = &noReadCallback;
  mSSLCtxCallback = &noSSLCtxCallback;
  if (active() && !LLApp::isExiting())
  {
	llwarns << "Revoking callbacks on a still active CurlEasyRequest object!" << llendl;
  }
  curl_easy_setopt(getEasyHandle(), CURLOPT_HEADERFUNCTION, &noHeaderCallback);
  curl_easy_setopt(getEasyHandle(), CURLOPT_WRITEHEADER, &noWriteCallback);
  curl_easy_setopt(getEasyHandle(), CURLOPT_READFUNCTION, &noReadCallback);
  curl_easy_setopt(getEasyHandle(), CURLOPT_SSL_CTX_FUNCTION, &noSSLCtxCallback);
}

CurlEasyRequest::~CurlEasyRequest()
{
  // If the CurlEasyRequest object is destructed then we need to revoke all callbacks, because
  // we can't set the lock anymore, and neither will mHeaderCallback, mWriteCallback etc,
  // be available anymore.
  send_events_to(NULL);
  revokeCallbacks();
  // This wasn't freed yet if the request never finished.
  curl_slist_free_all(mHeaders);
}

void CurlEasyRequest::resetState(void)
{
  // This function should not revoke the event call backs!
  revokeCallbacks();
  reset();
  curl_slist_free_all(mHeaders);
  mHeaders = NULL;
  mRequestFinalized = false;
  mEventsTarget = NULL;
  mResult = CURLE_FAILED_INIT;
  applyDefaultOptions();
}

void CurlEasyRequest::addHeader(char const* header)
{
  llassert(!mRequestFinalized);
  mHeaders = curl_slist_append(mHeaders, header);
}

#ifdef CWDEBUG
static int curl_debug_callback(CURL*, curl_infotype infotype, char* buf, size_t size, void* user_ptr)
{
  using namespace ::libcwd;

  CurlEasyRequest* request = (CurlEasyRequest*)user_ptr;
  std::ostringstream marker;
  marker << (void*)request->get_lockobj();
  libcw_do.push_marker();
  libcw_do.marker().assign(marker.str().data(), marker.str().size());
  LibcwDoutScopeBegin(LIBCWD_DEBUGCHANNELS, libcw_do, dc::curlio|cond_nonewline_cf(infotype == CURLINFO_TEXT))
  switch (infotype)
  {
	case CURLINFO_TEXT:
	  LibcwDoutStream << "* ";
	  break;
	case CURLINFO_HEADER_IN:
	  LibcwDoutStream << "H> ";
	  break;
	case CURLINFO_HEADER_OUT:
	  LibcwDoutStream << "H< ";
	  break;
	case CURLINFO_DATA_IN:
	  LibcwDoutStream << "D> ";
	  break;
	case CURLINFO_DATA_OUT:
	  LibcwDoutStream << "D< ";
	  break;
	case CURLINFO_SSL_DATA_IN:
	  LibcwDoutStream << "S> ";
	  break;
	case CURLINFO_SSL_DATA_OUT:
	  LibcwDoutStream << "S< ";
	  break;
	default:
	  LibcwDoutStream << "?? ";
  }
  if (infotype == CURLINFO_TEXT)
	LibcwDoutStream.write(buf, size);
  else if (infotype == CURLINFO_HEADER_IN || infotype == CURLINFO_HEADER_OUT)
	LibcwDoutStream << libcwd::buf2str(buf, size);
  else if (infotype == CURLINFO_DATA_IN || infotype == CURLINFO_DATA_OUT)
	LibcwDoutStream << size << " bytes: \"" << libcwd::buf2str(buf, size) << '"';
  else
	LibcwDoutStream << size << " bytes";
  LibcwDoutScopeEnd;
  libcw_do.pop_marker();
  return 0;
}
#endif

void CurlEasyRequest::applyProxySettings(void)
{
  LLProxy& proxy = *LLProxy::getInstance();

  // Do a faster unlocked check to see if we are supposed to proxy.
  if (proxy.HTTPProxyEnabled())
  {
	// We think we should proxy, read lock the shared proxy members.
	LLProxy::Shared_crat proxy_r(proxy.shared_lockobj());

	// Now test again to verify that the proxy wasn't disabled between the first check and the lock.
	if (proxy.HTTPProxyEnabled())
	{
	  setopt(CURLOPT_PROXY, proxy.getHTTPProxy(proxy_r).getIPString().c_str());
	  setopt(CURLOPT_PROXYPORT, proxy.getHTTPProxy(proxy_r).getPort());

	  if (proxy.getHTTPProxyType(proxy_r) == LLPROXY_SOCKS)
	  {
		setopt(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
		if (proxy.getSelectedAuthMethod(proxy_r) == METHOD_PASSWORD)
		{
		  std::string auth_string = proxy.getSocksUser(proxy_r) + ":" + proxy.getSocksPwd(proxy_r);
		  setopt(CURLOPT_PROXYUSERPWD, auth_string.c_str());
		}
	  }
	  else
	  {
		setopt(CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
	  }
	}
  }
}

void CurlEasyRequest::applyDefaultOptions(void)
{
  CertificateAuthority_rat CertificateAuthority_r(gCertificateAuthority);
  setoptString(CURLOPT_CAINFO, CertificateAuthority_r->file);
  // This option forces openssl to use TLS version 1.
  // The Linden Lab servers don't support later TLS versions, and libopenssl-1.0.1c has
  // a bug where renegotiation fails (see http://rt.openssl.org/Ticket/Display.html?id=2828),
  // causing the connection to fail completely without this hack.
  // For a commandline test of the same, observe the difference between:
  // openssl s_client       -connect login.agni.lindenlab.com:443 -CAfile packaged/app_settings/CA.pem -debug
  // and
  // openssl s_client -tls1 -connect login.agni.lindenlab.com:443 -CAfile packaged/app_settings/CA.pem -debug
  setopt(CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1);
  setopt(CURLOPT_NOSIGNAL, 1);
  // The old code did this for the 'buffered' version, but I think it's nonsense.
  //setopt(CURLOPT_DNS_CACHE_TIMEOUT, 0);
  // Set the CURL options for either SOCKS or HTTP proxy.
  applyProxySettings();
  // Cause libcurl to print all its I/O traffic on the debug channel.
  Debug(
	if (dc::curlio.is_on())
	{
	  setopt(CURLOPT_VERBOSE, 1);
	  setopt(CURLOPT_DEBUGFUNCTION, &curl_debug_callback);
	  setopt(CURLOPT_DEBUGDATA, this);
	}
  );
}

void CurlEasyRequest::finalizeRequest(std::string const& url)
{
  llassert(!mRequestFinalized);
  mResult = CURLE_FAILED_INIT;		// General error code, the final code is written here in MultiHandle::check_run_count when msg is CURLMSG_DONE.
  mRequestFinalized = true;
  lldebugs << url << llendl;
  setopt(CURLOPT_HTTPHEADER, mHeaders);
  setoptString(CURLOPT_URL, url);
  // The following line is a bit tricky: we store a pointer to the object without increasing its reference count.
  // Of course we could increment the reference count, but that doesn't help much: if then this pointer would
  // get "lost" we'd have a memory leak. Either way we must make sure that it is impossible that this pointer
  // will be used if the object is deleted [In fact, since this is finalizeRequest() and not addRequest(),
  // incrementing the reference counter would be wrong: if addRequest() is never called then the object is
  // destroyed shortly after and this pointer is never even used.]
  // This pointer is used in MultiHandle::check_run_count, which means that addRequest() was called and
  // the reference counter was increased and the object is being kept alive, see the comments above
  // command_queue in aicurlthread.cpp. In fact, this object survived until MultiHandle::add_easy_request
  // was called and is kept alive by MultiHandle::mAddedEasyRequests. The only way to get deleted after
  // that is when MultiHandle::remove_easy_request is called, which first removes the easy handle from
  // the multi handle. So that it's (hopefully) no longer possible that info_read() in
  // MultiHandle::check_run_count returns this easy handle, after the object is destroyed by deleting
  // it from MultiHandle::mAddedEasyRequests.
  setopt(CURLOPT_PRIVATE, get_lockobj());
}

void CurlEasyRequest::getTransferInfo(AICurlInterface::TransferInfo* info)
{
  // Curl explicitly demands a double for these info's.
  double size, total_time, speed;
  getinfo(CURLINFO_SIZE_DOWNLOAD, &size);
  getinfo(CURLINFO_TOTAL_TIME, &total_time);
  getinfo(CURLINFO_SPEED_DOWNLOAD, &speed);
  // Convert to F64.
  info->mSizeDownload = size;
  info->mTotalTime = total_time;
  info->mSpeedDownload = speed;
}

void CurlEasyRequest::getResult(CURLcode* result, AICurlInterface::TransferInfo* info)
{
  *result = mResult;
  if (info && mResult != CURLE_FAILED_INIT)
  {
	getTransferInfo(info);
  }
}

void CurlEasyRequest::added_to_multi_handle(AICurlEasyRequest_wat& curl_easy_request_w)
{
  if (mEventsTarget)
	mEventsTarget->added_to_multi_handle(curl_easy_request_w);
}

void CurlEasyRequest::finished(AICurlEasyRequest_wat& curl_easy_request_w)
{
  if (mEventsTarget)
	mEventsTarget->finished(curl_easy_request_w);
}

void CurlEasyRequest::removed_from_multi_handle(AICurlEasyRequest_wat& curl_easy_request_w)
{
  if (mEventsTarget)
	mEventsTarget->removed_from_multi_handle(curl_easy_request_w);
}

//-----------------------------------------------------------------------------
// CurlResponderBuffer

static unsigned int const MAX_REDIRECTS = 5;
static S32 const CURL_REQUEST_TIMEOUT = 30;		// Seconds per operation.

LLChannelDescriptors const CurlResponderBuffer::sChannels;

CurlResponderBuffer::CurlResponderBuffer()
{
  ThreadSafeBufferedCurlEasyRequest* lockobj = get_lockobj();
  AICurlEasyRequest_wat curl_easy_request_w(*lockobj);
  curl_easy_request_w->send_events_to(this);
}

// The callbacks need to be revoked when the CurlResponderBuffer is destructed (because that is what the callbacks use).
// The AIThreadSafeSimple<CurlResponderBuffer> is destructed first (right to left), so when we get here then the
// ThreadSafeCurlEasyRequest base class of ThreadSafeBufferedCurlEasyRequest is still intact and we can create
// and use curl_easy_request_w.
CurlResponderBuffer::~CurlResponderBuffer()
{
  ThreadSafeBufferedCurlEasyRequest* lockobj = get_lockobj();
  AICurlEasyRequest_wat curl_easy_request_w(*lockobj);				// Wait 'til possible callbacks have returned.
  curl_easy_request_w->send_events_to(NULL);
  curl_easy_request_w->revokeCallbacks();
  if (mResponder)
  {	
	llwarns << "Calling ~CurlResponderBuffer() with active responder!" << llendl;
	llassert(false);	// Does this ever happen? And if so, what does it mean?
	// FIXME: Does this really mean it timed out?
	mResponder->completedRaw(HTTP_REQUEST_TIME_OUT, "Request timeout, aborted.", sChannels, mOutput);
	mResponder = NULL;
  }
}

void CurlResponderBuffer::resetState(AICurlEasyRequest_wat& curl_easy_request_w)
{
  if (mResponder)
  {	
	llwarns << "Calling CurlResponderBuffer::resetState() for active easy handle!" << llendl;
	llassert(false);	// Does this ever happen? And if so, what does it mean?
	// FIXME: Does this really mean it timed out?
	mResponder->completedRaw(HTTP_REQUEST_TIME_OUT, "Request timeout, aborted.", sChannels, mOutput);
	mResponder = NULL;
  }

  curl_easy_request_w->resetState();

  mOutput.reset();
  
  mInput.str("");
  mInput.clear();
  
  mHeaderOutput.str("");
  mHeaderOutput.clear();
}

ThreadSafeBufferedCurlEasyRequest* CurlResponderBuffer::get_lockobj(void)
{
  return static_cast<ThreadSafeBufferedCurlEasyRequest*>(AIThreadSafeSimple<CurlResponderBuffer>::wrapper_cast(this));
}

void CurlResponderBuffer::prepRequest(AICurlEasyRequest_wat& curl_easy_request_w, std::vector<std::string> const& headers, AICurlInterface::ResponderPtr responder, S32 time_out, bool post)
{
  if (post)
  {
	curl_easy_request_w->setoptString(CURLOPT_ENCODING, "");
  }

  mOutput.reset(new LLBufferArray);
  mOutput->setThreaded(true);

  ThreadSafeBufferedCurlEasyRequest* lockobj = get_lockobj();
  curl_easy_request_w->setWriteCallback(&curlWriteCallback, lockobj);
  curl_easy_request_w->setReadCallback(&curlReadCallback, lockobj);
  curl_easy_request_w->setHeaderCallback(&curlHeaderCallback, lockobj);

  // Allow up to five redirects.
  if (responder && responder->followRedir())
  {
	curl_easy_request_w->setopt(CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_request_w->setopt(CURLOPT_MAXREDIRS, MAX_REDIRECTS);
  }

  curl_easy_request_w->setopt(CURLOPT_SSL_VERIFYPEER, true);
  // Don't verify host name so urls with scrubbed host names will work (improves DNS performance).
  curl_easy_request_w->setopt(CURLOPT_SSL_VERIFYHOST, 0);

  curl_easy_request_w->setopt(CURLOPT_TIMEOUT, llmax(time_out, CURL_REQUEST_TIMEOUT));

  // Keep responder alive.
  mResponder = responder;

  if (!post)
  {
	curl_easy_request_w->addHeader("Connection: keep-alive");
	curl_easy_request_w->addHeader("Keep-alive: 300");
	// Add other headers.
	for (std::vector<std::string>::const_iterator iter = headers.begin(); iter != headers.end(); ++iter)
	{
	  curl_easy_request_w->addHeader((*iter).c_str());
	}
  }
}

//static
size_t CurlResponderBuffer::curlWriteCallback(char* data, size_t size, size_t nmemb, void* user_data)
{
  ThreadSafeBufferedCurlEasyRequest* lockobj = static_cast<ThreadSafeBufferedCurlEasyRequest*>(user_data);

  // We need to lock the curl easy request object too, because that lock is used
  // to make sure that callbacks and destruction aren't done simultaneously.
  AICurlEasyRequest_wat buffered_easy_request_w(*lockobj);

  AICurlResponderBuffer_wat buffer_w(*lockobj);
  S32 n = size * nmemb;
  buffer_w->getOutput()->append(sChannels.in(), (U8 const*)data, n);
  return n;
}

//static
size_t CurlResponderBuffer::curlReadCallback(char* data, size_t size, size_t nmemb, void* user_data)
{
  ThreadSafeBufferedCurlEasyRequest* lockobj = static_cast<ThreadSafeBufferedCurlEasyRequest*>(user_data);

  // We need to lock the curl easy request object too, because that lock is used
  // to make sure that callbacks and destruction aren't done simultaneously.
  AICurlEasyRequest_wat buffered_easy_request_w(*lockobj);

  AICurlResponderBuffer_wat buffer_w(*lockobj);
  S32 n = size * nmemb;
  S32 startpos = buffer_w->getInput().tellg();
  buffer_w->getInput().seekg(0, std::ios::end);
  S32 endpos = buffer_w->getInput().tellg();
  buffer_w->getInput().seekg(startpos, std::ios::beg);
  S32 maxn = endpos - startpos;
  n = llmin(n, maxn);
  buffer_w->getInput().read(data, n);
  return n;
}

//static
size_t CurlResponderBuffer::curlHeaderCallback(char* data, size_t size, size_t nmemb, void* user_data)
{
  ThreadSafeBufferedCurlEasyRequest* lockobj = static_cast<ThreadSafeBufferedCurlEasyRequest*>(user_data);

  // We need to lock the curl easy request object too, because that lock is used
  // to make sure that callbacks and destruction aren't done simultaneously.
  AICurlEasyRequest_wat buffered_easy_request_w(*lockobj);

  AICurlResponderBuffer_wat buffer_w(*lockobj);
  size_t n = size * nmemb;
  buffer_w->getHeaderOutput().write(data, n);
  return n;
}

void CurlResponderBuffer::added_to_multi_handle(AICurlEasyRequest_wat& curl_easy_request_w)
{
  Dout(dc::curl, "Calling CurlResponderBuffer::added_to_multi_handle(@" << (void*)&*curl_easy_request_w << ") for this = " << (void*)this);
}

void CurlResponderBuffer::finished(AICurlEasyRequest_wat& curl_easy_request_w)
{
  Dout(dc::curl, "Calling CurlResponderBuffer::finished(@" << (void*)&*curl_easy_request_w << ") for this = " << (void*)this);
}

void CurlResponderBuffer::removed_from_multi_handle(AICurlEasyRequest_wat& curl_easy_request_w)
{
  Dout(dc::curl, "Calling CurlResponderBuffer::removed_from_multi_handle(@" << (void*)&*curl_easy_request_w << ") for this = " << (void*)this);

  // Lock self.
  ThreadSafeBufferedCurlEasyRequest* lockobj = get_lockobj();
  llassert(dynamic_cast<ThreadSafeBufferedCurlEasyRequest*>(static_cast<ThreadSafeCurlEasyRequest*>(ThreadSafeCurlEasyRequest::wrapper_cast(&*curl_easy_request_w))) == lockobj);
  AICurlResponderBuffer_wat buffer_w(*lockobj);
  llassert(&*buffer_w == this);

  processOutput(curl_easy_request_w);
}

void CurlResponderBuffer::processOutput(AICurlEasyRequest_wat& curl_easy_request_w)
{
  U32 responseCode = 0;	
  std::string responseReason;
  
  CURLcode code;
  curl_easy_request_w->getResult(&code);
  if (code == CURLE_OK)
  {
	curl_easy_request_w->getinfo(CURLINFO_RESPONSE_CODE, &responseCode);
	//*TODO: get reason from first line of mHeaderOutput
  }
  else
  {
	responseCode = 499;
	responseReason = AICurlInterface::strerror(code) + " : ";
	if (code == CURLE_FAILED_INIT)
	{
	  responseReason += "Curl Easy Handle initialization failed.";
	}
	else
	{
	  responseReason += curl_easy_request_w->getErrorString();
	}
	curl_easy_request_w->setopt(CURLOPT_FRESH_CONNECT, TRUE);
  }

  if (mResponder)
  {	
	mResponder->completedRaw(responseCode, responseReason, sChannels, mOutput);
	mResponder = NULL;
  }

  resetState(curl_easy_request_w);
}

//-----------------------------------------------------------------------------
// CurlMultiHandle

LLAtomicU32 CurlMultiHandle::sTotalMultiHandles;

CurlMultiHandle::CurlMultiHandle(void)
{
  DoutEntering(dc::curl, "CurlMultiHandle::CurlMultiHandle() [" << (void*)this << "].");
  mMultiHandle = curl_multi_init();
  Stats::multi_calls++;
  if (!mMultiHandle)
  {
	Stats::multi_errors++;
	throw AICurlNoMultiHandle("curl_multi_init() returned NULL");
  }
  sTotalMultiHandles++;
}

CurlMultiHandle::~CurlMultiHandle()
{
  curl_multi_cleanup(mMultiHandle);
  Stats::multi_calls++;
  int total = --sTotalMultiHandles;
  Dout(dc::curl, "Called CurlMultiHandle::~CurlMultiHandle() [" << (void*)this << "], " << total << " remaining.");
  if (total == 0)
	Stats::print();
}

} // namespace AICurlPrivate
