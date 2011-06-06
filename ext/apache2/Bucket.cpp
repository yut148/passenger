/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2010 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#include "Bucket.h"
#include <Logging.h>
#include <cassert>

namespace Passenger {

static void bucket_destroy(void *data);
static apr_status_t bucket_read(apr_bucket *a, const char **str, apr_size_t *len, apr_read_type_e block);

static const apr_bucket_type_t apr_bucket_type_passenger_pipe = {
	"PASSENGER_PIPE",
	5,
	apr_bucket_type_t::APR_BUCKET_DATA, 
	bucket_destroy,
	bucket_read,
	apr_bucket_setaside_notimpl,
	apr_bucket_split_notimpl,
	apr_bucket_copy_notimpl
};

struct BucketData {
	PassengerBucketStatePtr state;
	
	~BucketData() {
		/* The session referenced by 'state' is an ApplicationPoolServer::RemoteSession.
		 * The only reason why its destructor might fail is when sending
		 * the 'close' command failed. We don't care about that, and we
		 * don't want C++ exceptions to propagate onto a C stack (this
		 * bucket is probably used by Apache's bucket brigade code, which
		 * is written in C), so we ignore all errors in the session's
		 * destructor.
		 */
		try {
			state.reset();
		} catch (const SystemException &e) {
			// Do nothing.
		}
	}
};

static void
bucket_destroy(void *data) {
	BucketData *bucket_data = (BucketData *) data;
	if (data != NULL) {
		delete bucket_data;
	}
}

static void
onChunkData(const char *data, size_t size, void *userData) {
	PassengerBucketState *state = (PassengerBucketState *) userData;
	state->chunks.push(StaticString(data, size));
}

void
PassengerBucketState::enableDechunking() {
	chunkBufferSize = 1024 * 32;
	chunkBuffer = new char[chunkBufferSize];
	dechunker.onData = onChunkData;
	dechunker.userData = this;
}

static apr_status_t
bucket_read(apr_bucket *bucket, const char **str, apr_size_t *len, apr_read_type_e block) {
	PassengerBucketStatePtr state;
	BucketData *data;
	char *buf = NULL;
	ssize_t ret;
	bool sync = false;
	
	data  = (BucketData *) bucket->data;
	state = data->state;
	*str  = NULL;
	*len  = 0;
	
	if (block == APR_NONBLOCK_READ) {
		/*
		 * The bucket brigade that Hooks::handleRequest() passes using
		 * ap_pass_brigade() is always passed through ap_content_length_filter,
		 * which is a filter which attempts to read all data from the
		 * bucket brigade and computes the Content-Length header from
		 * that. We don't want this to happen; because suppose that the
		 * Rails application sends back 1 GB of data, then
		 * ap_content_length_filter will buffer this entire 1 GB of data
		 * in memory before passing it to the HTTP client.
		 *
		 * ap_content_length_filter aborts and passes the bucket brigade
		 * down the filter chain when it encounters an APR_EAGAIN, except
		 * for the first read. So by returning APR_EAGAIN on every
		 * non-blocking read request, we can prevent ap_content_length_filter
		 * from buffering all data.
		 */
		return APR_EAGAIN;
	}
	
	if (state->chunkBuffer != NULL) {
		int e = 0;
		
		if (state->chunks.empty()) {
			bool done = false;
			do {
				do {
					ret = read(state->stream, state->chunkBuffer,
						state->chunkBufferSize);
				} while (ret == -1 && errno == EINTR);
				if (ret <= 0) {
					e = errno;
					done = true;
				} else {
					P_WARN("Feeding");
					state->dechunker.feed(state->chunkBuffer, ret);
					P_WARN("Parsed " << state->chunks.size() << " chunks");
				}
			} while (!done && state->chunks.empty());
		}
		
		if (e != 0) {
			ret = -1;
			errno = e;
		} else if (!state->chunks.empty()) {
			StaticString chunk = state->chunks.front();
			buf = (char *) apr_bucket_alloc(chunk.size(), bucket->list);
			ret = (ssize_t) chunk.size();
			memcpy(buf, chunk.data(), chunk.size());
			state->chunks.pop();
			sync = state->chunks.empty();
		} else {
			ret = 0;
		}
		
	} else {
		buf = (char *) apr_bucket_alloc(APR_BUCKET_BUFF_SIZE, bucket->list);
		if (buf == NULL) {
			return APR_ENOMEM;
		}
		do {
			ret = read(state->stream, buf, APR_BUCKET_BUFF_SIZE);
		} while (ret == -1 && errno == EINTR);
	}
	
	if (ret > 0) {
		apr_bucket_heap *h;
		
		*str = buf;
		*len = ret;
		bucket->data = NULL;
		
		/* Change the current bucket (which is a Passenger Bucket) into a heap bucket
		 * that contains the data that we just read. This newly created heap bucket
		 * will be the first in the bucket list.
		 */
		bucket = apr_bucket_heap_make(bucket, buf, *len, apr_bucket_free);
		h = (apr_bucket_heap *) bucket->data;
		h->alloc_len = APR_BUCKET_BUFF_SIZE; /* note the real buffer size */
		
		if (sync) {
			/* After having emitted all chunks, instruct filters like
			 * mod_deflate to flush.
			 */
			//APR_BUCKET_INSERT_AFTER(bucket, passenger_bucket_create(
			//	data->state, bucket->list));
		}
		
		/* And after this newly created bucket we insert a new Passenger Bucket
		 * which can read the next chunk from the stream.
		 */
		APR_BUCKET_INSERT_AFTER(bucket, passenger_bucket_create(
			data->state, bucket->list));
		
		/* The newly created Passenger Bucket has a reference to the session
		 * object, so we can delete data here.
		 */
		delete data;
		
		return APR_SUCCESS;
		
	} else if (ret == 0) {
		state->completed = true;
		delete data;
		bucket->data = NULL;
		
		if (buf != NULL) {
			apr_bucket_free(buf);
		}
		
		bucket = apr_bucket_immortal_make(bucket, "", 0);
		*str = (const char *) bucket->data;
		*len = 0;
		return APR_SUCCESS;
		
	} else /* ret == -1 */ {
		int e = errno;
		state->completed = true;
		state->errorCode = e;
		delete data;
		bucket->data = NULL;
		if (buf != NULL) {
			apr_bucket_free(buf);
		}
		return APR_FROM_OS_ERROR(e);
	}
}

static apr_bucket *
passenger_bucket_make(apr_bucket *bucket, PassengerBucketStatePtr state) {
	BucketData *data = new BucketData();
	data->state    = state;
	
	bucket->type   = &apr_bucket_type_passenger_pipe;
	bucket->length = (apr_size_t)(-1);
	bucket->start  = -1;
	bucket->data   = data;
	return bucket;
}

apr_bucket *
passenger_bucket_create(PassengerBucketStatePtr state, apr_bucket_alloc_t *list) {
	apr_bucket *bucket;
	
	bucket = (apr_bucket *) apr_bucket_alloc(sizeof(*bucket), list);
	APR_BUCKET_INIT(bucket);
	bucket->free = apr_bucket_free;
	bucket->list = list;
	return passenger_bucket_make(bucket, state);
}

} // namespace Passenger
