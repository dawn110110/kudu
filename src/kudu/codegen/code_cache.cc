// Copyright 2014 Cloudera inc.
// Confidential Cloudera Information: Covered by NDA.

#include "kudu/codegen/code_cache.h"

#include "kudu/codegen/jit_wrapper.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/cache.h"
#include "kudu/util/faststring.h"
#include "kudu/util/slice.h"

namespace kudu {
namespace codegen {

CodeCache::CodeCache(size_t capacity)
  : cache_(NewLRUCache(capacity, "code_cache")) {}

CodeCache::~CodeCache() {}

namespace {

void CodeCacheDeleter(const Slice& key, void* value) {
  // The Cache from cache.h deletes the memory that it allocates for its
  // own copy of key, but it expects its users to delete their own
  // void* values. To delete, we just release our shared ownership.
  static_cast<JITWrapper*>(value)->Release();
}

} // anonymous namespace

Status CodeCache::AddEntry(const scoped_refptr<JITWrapper>& value) {
  // Get the key
  faststring key;
  RETURN_NOT_OK(value->EncodeOwnKey(&key));

  // Because Cache only accepts void* values, we store just the JITWrapper*
  // and increase its ref count.
  value->AddRef();

  // Insert into cache and release the handle (we have a local copy of a refptr)
  Cache::Handle* inserted = cache_->Insert(key, value.get(), 1, CodeCacheDeleter);
  cache_->Release(inserted);
  return Status::OK();
}

scoped_refptr<JITWrapper> CodeCache::Lookup(const Slice& key) {
  // Look up in Cache after generating key, returning NULL if not found.
  Cache::Handle* found = cache_->Lookup(key, Cache::EXPECT_IN_CACHE);
  if (!found) return scoped_refptr<JITWrapper>();

  // Retrieve the value
  scoped_refptr<JITWrapper> value =
    static_cast<JITWrapper*>(cache_->Value(found));

  // No need to hold on to handle after we have our copy
  cache_->Release(found);

  return value;
}

} // namespace codegen
} // namespace kudu