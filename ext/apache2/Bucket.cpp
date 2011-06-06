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


struct DechunkState {
	PassengerBucketState *passengerBucketState;
	unsigned int iteration;
	const char **str;
	apr_size_t *len;
	apr_bucket *bucket;
	apr_status_t result;
};

static void
onChunkData(const char *data, size_t size, void *userData) {
	DechunkState *state = (DechunkState *) userData;
	
	if (state->result != APR_SUCCESS) {
		// Previous callback set error state.
		return;
	}
	
	char *buf = (char *) apr_bucket_alloc(size, state->bucket->list);
	if (buf == NULL) {
		state->result = APR_ENOMEM;
		return;
	}
	
	memcpy(buf, data, size);
	
	if (state->iteration == 0) {
		*state->str = buf;
		*state->len = (apr_size_t) size;
		
		state->bucket = apr_bucket_heap_make(state->bucket, data, size, apr_bucket_free);
		apr_bucket_heap *h = (apr_bucket_heap *) bucket->data;
		h->alloc_len = size; /* note the real buffer size */
	}
	
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
}

static apr_status_t
bucket_read2(apr_bucket *bucket, const char **str, apr_size_t *len, apr_read_type_e block) {
	BucketData *data;
	PassengerBucketState *state;
	char buf[1024 * 32];
	ssize_t ret;
	
	data  = (BucketData *) bucket->data;
	state = data->state.get();
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
	
	do {
		ret = read(state->stream, buf, sizeof(buf));
	} while (ret == -1 && errno == EINTR);
	
	
	
	if (ret > 0) {
		DechunkingState dechunkState;
		dechunkState.passengerBucketState = state;
		dechunkState.iteration = 0;
		dechunkState.str = str;
		dechunkState.len = len;
		dechunkState.bucket = bucket;
		dechunkState.result = APR_SUCCESS;
		state->dechunker.onData   = onChunkData;
		state->dechunker.userData = &dechunkState;
		assert(state->dechunker.acceptingInput());
		size_t fed = state->dechunker.feed(buf, ret);
		return dechunkState.result;
		
	} else if (ret == 0) {
		state->completed = true;
		delete data;
		bucket->data = NULL;
		
		apr_bucket_free(buf);
		
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
		apr_bucket_free(buf);
		return APR_FROM_OS_ERROR(e);
	}
}



static apr_status_t
bucket_read(apr_bucket *bucket, const char **str, apr_size_t *len, apr_read_type_e block) {
	PassengerBucketStatePtr state;
	BucketData *data;
	char *buf;
	ssize_t ret;
	
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
	
	if (state->chunked) {
		if (state->remainingChunks.empty()) {
			buf = (char *) alloca(APR_BUCKET_BUFF_SIZE);
			do {
				do {
					ret = read(state->stream, buf, APR_BUCKET_BUFF_SIZE);
				} while (ret == -1 && errno == EINTR);
			} while ();
		} else {
			buf = state->remainingChunks.front().data();
			ret = (ssize_t) state->remainingChunks.front().size();
			state->remainingChunks.pop();
			if (state->remainingChunk.empty()) {
				apr_bucket_free(state->chunkBuffer);
				state->chunkBuffer = NULL;
			}
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
		
		if (state->chunked) {
			vector<StaticString> chunks;
			size_t i;
			
			state->dechunker.onData = onChunkData;
			state->dechunker.userData = &chunks;
			state->dechunker.feed(buf, ret);
			
			for (i = 1; i < chunks.size(); i++) {
				
			}
		} else {
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
		}
		
	} else if (ret == 0) {
		state->completed = true;
		delete data;
		bucket->data = NULL;
		
		if (!state->chunked) {
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
		if (!state->chunked) {
			apr_bucket_free(buf);
		}
		return APR_FROM_OS_ERROR(e);
	}
}

static apr_status_t
bucket_read(apr_bucket *bucket, const char **str, apr_size_t *len, apr_read_type_e block) {
	char *buf;
	ssize_t ret;
	BucketData *data;
	PassengerBucketStatePtr state;
	
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
	
	buf = (char *) apr_bucket_alloc(APR_BUCKET_BUFF_SIZE, bucket->list);
	if (buf == NULL) {
		return APR_ENOMEM;
	}
	
	do {
		ret = read(state->stream, buf, APR_BUCKET_BUFF_SIZE);
	} while (ret == -1 && errno == EINTR);
	
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
		
		apr_bucket_free(buf);
		
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
		apr_bucket_free(buf);
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
