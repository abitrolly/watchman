/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include "InMemoryView.h"
#include <folly/ScopeGuard.h>
#include <algorithm>
#include <memory>
#include <thread>
#include "ThreadPool.h"

using folly::Optional;

// Each root gets a number that uniquely identifies it within the process. This
// helps avoid confusion if a root is removed and then added again.
static std::atomic<long> next_root_number{1};

namespace watchman {

InMemoryViewCaches::InMemoryViewCaches(
    const w_string& rootPath,
    size_t maxHashes,
    size_t maxSymlinks,
    std::chrono::milliseconds errorTTL)
    : contentHashCache(rootPath, maxHashes, errorTTL),
      symlinkTargetCache(rootPath, maxSymlinks, errorTTL) {}

InMemoryFileResult::InMemoryFileResult(
    const watchman_file* file,
    InMemoryViewCaches& caches)
    : file_(file), caches_(caches) {}

void InMemoryFileResult::batchFetchProperties(
    const std::vector<std::unique_ptr<FileResult>>& files) {
  std::vector<folly::Future<folly::Unit>> readlinkFutures;
  std::vector<folly::Future<folly::Unit>> sha1Futures;

  // Since we may initiate some async work in the body of the function
  // below, we need to ensure that we wait for it to complete before
  // we return from this scope, even if we are throwing an exception.
  // If we fail to do so, the continuation on the futures that we
  // schedule will access invalid memory and we'll all feel bad.
  SCOPE_EXIT {
    if (!readlinkFutures.empty()) {
      folly::collectAll(readlinkFutures.begin(), readlinkFutures.end()).wait();
    }
    if (!sha1Futures.empty()) {
      folly::collectAll(sha1Futures.begin(), sha1Futures.end()).wait();
    }
  };

  for (auto& f : files) {
    auto* file = dynamic_cast<InMemoryFileResult*>(f.get());

    if (file->neededProperties() & FileResult::Property::SymlinkTarget) {
      if (!file->file_->stat.isSymlink()) {
        // If this file is not a symlink then we yield
        // a nullptr w_string instance rather than propagating an error.
        // This behavior is relied upon by the field rendering code and
        // checked in test_symlink.py.
        file->symlinkTarget_ = w_string();
      } else {
        auto dir = file->dirName();
        dir.advance(file->caches_.symlinkTargetCache.rootPath().size());

        // If dirName is the root, dir.size() will now be zero
        if (dir.size() > 0) {
          // if not at the root, skip the slash character at the
          // front of dir
          dir.advance(1);
        }

        SymlinkTargetCacheKey key{w_string::pathCat({dir, file->baseName()}),
                                  file->file_->otime};

        readlinkFutures.emplace_back(
            caches_.symlinkTargetCache.get(key).thenTry(
                [file](folly::Try<std::shared_ptr<
                           const SymlinkTargetCache::Node>>&& result) {
                  if (result.hasValue()) {
                    file->symlinkTarget_ = result.value()->value();
                  } else {
                    // we don't have a way to report the error for readlink
                    // due to legacy requirements in the interface, so we
                    // just set it to empty.
                    file->symlinkTarget_ = w_string();
                  }
                }));
      }
    }

    if (file->neededProperties() & FileResult::Property::ContentSha1) {
      auto dir = file->dirName();
      dir.advance(file->caches_.contentHashCache.rootPath().size());

      // If dirName is the root, dir.size() will now be zero
      if (dir.size() > 0) {
        // if not at the root, skip the slash character at the
        // front of dir
        dir.advance(1);
      }

      ContentHashCacheKey key{w_string::pathCat({dir, file->baseName()}),
                              size_t(file->file_->stat.size),
                              file->file_->stat.mtime};

      sha1Futures.emplace_back(caches_.contentHashCache.get(key).thenTry(
          [file](folly::Try<std::shared_ptr<const ContentHashCache::Node>>&&
                     result) {
            file->contentSha1_ =
                makeResultWith([&] { return result.value()->value(); });
          }));
    }

    file->clearNeededProperties();
  }
}

Optional<FileInformation> InMemoryFileResult::stat() {
  return file_->stat;
}

Optional<size_t> InMemoryFileResult::size() {
  return file_->stat.size;
}

Optional<struct timespec> InMemoryFileResult::accessedTime() {
  return file_->stat.atime;
}

Optional<struct timespec> InMemoryFileResult::modifiedTime() {
  return file_->stat.mtime;
}

Optional<struct timespec> InMemoryFileResult::changedTime() {
  return file_->stat.ctime;
}

w_string_piece InMemoryFileResult::baseName() {
  return file_->getName();
}

w_string_piece InMemoryFileResult::dirName() {
  if (!dirName_) {
    dirName_ = file_->parent->getFullPath();
  }
  return dirName_;
}

Optional<bool> InMemoryFileResult::exists() {
  return file_->exists;
}

Optional<w_clock_t> InMemoryFileResult::ctime() {
  return file_->ctime;
}

Optional<w_clock_t> InMemoryFileResult::otime() {
  return file_->otime;
}

Optional<w_string> InMemoryFileResult::readLink() {
  if (!symlinkTarget_.has_value()) {
    if (!file_->stat.isSymlink()) {
      // If this file is not a symlink then we immediately yield
      // a nullptr w_string instance rather than propagating an error.
      // This behavior is relied upon by the field rendering code and
      // checked in test_symlink.py.
      symlinkTarget_ = w_string();
      return symlinkTarget_;
    }
    // Need to load the symlink target; batch that up
    accessorNeedsProperties(FileResult::Property::SymlinkTarget);
    return folly::none;
  }
  return symlinkTarget_;
}

Optional<FileResult::ContentHash> InMemoryFileResult::getContentSha1() {
  if (!file_->exists) {
    // Don't return hashes for files that we believe to be deleted.
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  if (!file_->stat.isFile()) {
    // We only want to compute the hash for regular files
    throw std::system_error(std::make_error_code(std::errc::is_a_directory));
  }

  if (contentSha1_.empty()) {
    accessorNeedsProperties(FileResult::Property::ContentSha1);
    return folly::none;
  }
  return contentSha1_.value();
}

InMemoryView::view::view(const w_string& root_path)
    : root_dir(std::make_unique<watchman_dir>(root_path, nullptr)) {}

InMemoryView::InMemoryView(w_root_t* root, std::shared_ptr<Watcher> watcher)
    : cookies_(root->cookies),
      config_(root->config),
      view_(view(root->root_path)),
      rootNumber_(next_root_number++),
      root_path(root->root_path),
      watcher_(watcher),
      caches_(
          root->root_path,
          config_.getInt("content_hash_max_items", 128 * 1024),
          config_.getInt("symlink_target_max_items", 32 * 1024),
          std::chrono::milliseconds(
              config_.getInt("content_hash_negative_cache_ttl_ms", 2000))),
      enableContentCacheWarming_(
          config_.getBool("content_hash_warming", false)),
      maxFilesToWarmInContentCache_(
          size_t(config_.getInt("content_hash_max_warm_per_settle", 1024))),
      syncContentCacheWarming_(
          config_.getBool("content_hash_warm_wait_before_settle", false)),
      scm_(SCM::scmForPath(root->root_path)) {}

void InMemoryView::view::insertAtHeadOfFileList(struct watchman_file* file) {
  file->next = latest_file;
  if (file->next) {
    file->next->prev = &file->next;
  }
  latest_file = file;
  file->prev = &latest_file;
}

void InMemoryView::markFileChanged(
    SyncView::LockedPtr& view,
    watchman_file* file,
    const struct timeval& now) {
  if (file->exists) {
    watcher_->startWatchFile(file);
  }

  file->otime.timestamp = now.tv_sec;
  file->otime.ticks = mostRecentTick_;

  if (view->latest_file != file) {
    // unlink from list
    file->removeFromFileList();

    // and move to the head
    view->insertAtHeadOfFileList(file);
  }
}

const watchman_dir* InMemoryView::resolveDir(
    SyncView::ConstLockedPtr& view,
    const w_string& dir_name) const {
  watchman_dir* dir;
  const char* dir_component;
  const char* dir_end;

  if (dir_name == root_path) {
    return view->root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = view->root_dir.get();
  dir_component += root_path.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    auto sep = (const char*)memchr(dir_component, '/', dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_piece component(
        dir_component, sep ? (sep - dir_component) : (dir_end - dir_component));

    auto child = dir->getChildDir(component);
    if (!child) {
      return nullptr;
    }

    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // Does not exist
      return nullptr;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  return nullptr;
}

watchman_dir* InMemoryView::resolveDir(
    SyncView::LockedPtr& view,
    const w_string& dir_name,
    bool create) {
  watchman_dir *dir, *parent;
  const char* dir_component;
  const char* dir_end;

  if (dir_name == root_path) {
    return view->root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = view->root_dir.get();
  dir_component += root_path.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    auto sep = (const char*)memchr(dir_component, '/', dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_piece component(
        dir_component, sep ? (sep - dir_component) : (dir_end - dir_component));

    auto child = dir->getChildDir(component);

    if (!child && !create) {
      return nullptr;
    }
    if (!child && sep && create) {
      // A component in the middle wasn't present.  Since we're in create
      // mode, we know that the leaf must exist.  The assumption is that
      // we have another pending item for the parent.  We'll create the
      // parent dir now and our other machinery will populate its contents
      // later.
      w_string child_name(dir_component, (uint32_t)(sep - dir_component));

      // Careful! dir->dirs is keyed by non-owning string pieces so the
      // child_name MUST be stored or otherwise kept alive by the watchman_dir
      // instance constructed below!
      auto& new_child = dir->dirs[child_name];
      new_child.reset(new watchman_dir(child_name, dir));

      child = new_child.get();
    }

    parent = dir;
    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // We need to create the dir
      break;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  w_string child_name(dir_component, (uint32_t)(dir_end - dir_component));
  // Careful! parent->dirs is keyed by non-owning string pieces so the
  // child_name MUST be stored or otherwise kept alive by the watchman_dir
  // instance constructed below!
  auto& new_child = parent->dirs[child_name];
  new_child.reset(new watchman_dir(child_name, parent));

  dir = new_child.get();

  return dir;
}

void InMemoryView::markDirDeleted(
    SyncView::LockedPtr& view,
    struct watchman_dir* dir,
    const struct timeval& now,
    bool recursive) {
  if (!dir->last_check_existed) {
    // If we know that it doesn't exist, return early
    return;
  }
  dir->last_check_existed = false;

  for (auto& it : dir->files) {
    auto file = it.second.get();

    if (file->exists) {
      auto full_name = dir->getFullPathToChild(file->getName());
      w_log(W_LOG_DBG, "mark_deleted: %s\n", full_name.c_str());
      file->exists = false;
      markFileChanged(view, file, now);
    }
  }

  if (recursive) {
    for (auto& it : dir->dirs) {
      auto child = it.second.get();

      markDirDeleted(view, child, now, true);
    }
  }
}

watchman_file* InMemoryView::getOrCreateChildFile(
    SyncView::LockedPtr& view,
    watchman_dir* dir,
    const w_string& file_name,
    const struct timeval& now) {
  // file_name is typically a baseName slice; let's use it as-is
  // to look up a child...
  auto it = dir->files.find(file_name);
  if (it != dir->files.end()) {
    return it->second.get();
  }

  // ... but take the shorter string from inside the file that
  // we create as the key.
  auto file = watchman_file::make(file_name, dir);
  auto& file_ptr = dir->files[file->getName()];
  file_ptr = std::move(file);

  file_ptr->ctime.ticks = mostRecentTick_;
  file_ptr->ctime.timestamp = now.tv_sec;

  auto suffix = file_name.asLowerCaseSuffix();
  if (suffix) {
    auto& sufhead = view->suffixes[suffix];
    if (!sufhead) {
      // Create the list head if we don't already have one for this suffix.
      sufhead.reset(new watchman::InMemoryView::file_list_head);
    }

    file_ptr->suffix_next = sufhead->head;
    if (file_ptr->suffix_next) {
      sufhead->head->suffix_prev = &file_ptr->suffix_next;
    }
    sufhead->head = file_ptr.get();
    file_ptr->suffix_prev = &sufhead->head;
  }

  watcher_->startWatchFile(file_ptr.get());

  return file_ptr.get();
}

void InMemoryView::ageOutFile(
    std::unordered_set<w_string>& dirs_to_erase,
    watchman_file* file) {
  auto parent = file->parent;

  auto full_name = parent->getFullPathToChild(file->getName());
  w_log(W_LOG_DBG, "age_out file=%s\n", full_name.c_str());

  // Revise tick for fresh instance reporting
  last_age_out_tick = std::max(last_age_out_tick, file->otime.ticks);

  // If we have a corresponding dir, we want to arrange to remove it, but only
  // after we have unlinked all of the associated file nodes.
  dirs_to_erase.insert(full_name);

  // Remove the entry from the containing file hash; this will free it.
  // We don't need to stop watching it, because we already stopped watching it
  // when we marked it as !exists.
  parent->files.erase(file->getName());
}

void InMemoryView::ageOut(w_perf_t& sample, std::chrono::seconds minAge) {
  struct watchman_file *file, *prior;
  time_t now;
  uint32_t num_aged_files = 0;
  uint32_t num_walked = 0;
  std::unordered_set<w_string> dirs_to_erase;

  time(&now);
  last_age_out_timestamp = now;
  auto view = view_.wlock();

  file = view->latest_file;
  prior = nullptr;
  while (file) {
    ++num_walked;
    if (file->exists || file->otime.timestamp + minAge.count() > now) {
      prior = file;
      file = file->next;
      continue;
    }

    ageOutFile(dirs_to_erase, file);
    num_aged_files++;

    // Go back to last good file node; we can't trust that the
    // value of file->next saved before age_out_file is a valid
    // file node as anything past that point may have also been
    // aged out along with it.
    file = prior;
  }

  for (auto& name : dirs_to_erase) {
    auto parent = resolveDir(view, name.dirName(), false);
    if (parent) {
      parent->dirs.erase(name.baseName());
    }
  }

  if (num_aged_files + dirs_to_erase.size()) {
    w_log(
        W_LOG_ERR,
        "aged %" PRIu32 " files, %" PRIu32 " dirs\n",
        num_aged_files,
        uint32_t(dirs_to_erase.size()));
  }
  sample.add_meta(
      "age_out",
      json_object({{"walked", json_integer(num_walked)},
                   {"files", json_integer(num_aged_files)},
                   {"dirs", json_integer(dirs_to_erase.size())}}));
}

void InMemoryView::timeGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  struct watchman_file* f;

  // Walk back in time until we hit the boundary
  auto view = view_.rlock();
  for (f = view->latest_file; f; f = f->next) {
    ctx->bumpNumWalked();
    // Note that we use <= for the time comparisons in here so that we
    // report the things that changed inclusive of the boundary presented.
    // This is especially important for clients using the coarse unix
    // timestamp as the since basis, as they would be much more
    // likely to miss out on changes if we didn't.
    if (ctx->since.is_timestamp && f->otime.timestamp <= ctx->since.timestamp) {
      break;
    }
    if (!ctx->since.is_timestamp && f->otime.ticks <= ctx->since.clock.ticks) {
      break;
    }

    if (!ctx->fileMatchesRelativeRoot(f)) {
      continue;
    }

    w_query_process_file(
        query, ctx, std::make_unique<InMemoryFileResult>(f, caches_));
  }
}

void InMemoryView::suffixGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  struct watchman_file* f;

  auto view = view_.rlock();
  for (const auto& suff : *query->suffixes) {
    // Head of suffix index for this suffix
    auto it = view->suffixes.find(suff);
    if (it == view->suffixes.end()) {
      continue;
    }

    // Walk and process
    for (f = it->second->head; f; f = f->suffix_next) {
      ctx->bumpNumWalked();
      if (!ctx->fileMatchesRelativeRoot(f)) {
        continue;
      }

      w_query_process_file(
          query, ctx, std::make_unique<InMemoryFileResult>(f, caches_));
    }
  }
}

void InMemoryView::pathGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  w_string_t* relative_root;
  struct watchman_file* f;

  if (query->relative_root) {
    relative_root = query->relative_root;
  } else {
    relative_root = root_path;
  }

  auto view = view_.rlock();

  for (const auto& path : *query->paths) {
    const watchman_dir* dir;
    w_string dir_name;

    // Compose path with root
    auto full_name = w_string::pathCat({relative_root, path.name});

    // special case of root dir itself
    if (w_string_equal(root_path, full_name)) {
      // dirname on the root is outside the root, which is useless
      dir = resolveDir(view, full_name);
      goto is_dir;
    }

    // Ideally, we'd just resolve it directly as a dir and be done.
    // It's not quite so simple though, because we may resolve a dir
    // that had been deleted and replaced by a file.
    // We prefer to resolve the parent and walk down.
    dir_name = full_name.dirName();
    if (!dir_name) {
      continue;
    }

    dir = resolveDir(view, dir_name);

    if (!dir) {
      // Doesn't exist, and never has
      continue;
    }

    if (!dir->files.empty()) {
      auto file_name = path.name.baseName();
      f = dir->getChildFile(file_name);

      // If it's a file (but not an existent dir)
      if (f && (!f->exists || !f->stat.isDir())) {
        ctx->bumpNumWalked();
        w_query_process_file(
            query, ctx, std::make_unique<InMemoryFileResult>(f, caches_));
        continue;
      }
    }

    // Is it a dir?
    if (dir->dirs.empty()) {
      continue;
    }

    dir = dir->getChildDir(full_name.baseName());
  is_dir:
    // We got a dir; process recursively to specified depth
    if (dir) {
      dirGenerator(query, ctx, dir, path.depth);
    }
  }
}

void InMemoryView::dirGenerator(
    w_query* query,
    struct w_query_ctx* ctx,
    const watchman_dir* dir,
    uint32_t depth) const {
  for (auto& it : dir->files) {
    auto file = it.second.get();
    ctx->bumpNumWalked();

    w_query_process_file(
        query, ctx, std::make_unique<InMemoryFileResult>(file, caches_));
  }

  if (depth > 0) {
    for (auto& it : dir->dirs) {
      const auto child = it.second.get();

      dirGenerator(query, ctx, child, depth - 1);
    }
  }
}

void InMemoryView::allFilesGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  struct watchman_file* f;
  auto view = view_.rlock();

  for (f = view->latest_file; f; f = f->next) {
    ctx->bumpNumWalked();
    if (!ctx->fileMatchesRelativeRoot(f)) {
      continue;
    }

    w_query_process_file(
        query, ctx, std::make_unique<InMemoryFileResult>(f, caches_));
  }
}

ClockPosition InMemoryView::getMostRecentRootNumberAndTickValue() const {
  return ClockPosition(rootNumber_, mostRecentTick_);
}

w_string InMemoryView::getCurrentClockString() const {
  char clockbuf[128];
  if (!clock_id_string(
          rootNumber_, mostRecentTick_, clockbuf, sizeof(clockbuf))) {
    throw std::runtime_error("clock string exceeded clockbuf size");
  }
  return w_string(clockbuf, W_STRING_UNICODE);
}

uint32_t InMemoryView::getLastAgeOutTickValue() const {
  return last_age_out_tick;
}

time_t InMemoryView::getLastAgeOutTimeStamp() const {
  return last_age_out_timestamp;
}

void InMemoryView::startThreads(const std::shared_ptr<w_root_t>& root) {
  // Start a thread to call into the watcher API for filesystem notifications
  auto self = std::static_pointer_cast<InMemoryView>(shared_from_this());
  w_log(W_LOG_DBG, "starting threads for %p %s\n", this, root_path.c_str());
  std::thread notifyThreadInstance([self, root]() {
    w_set_thread_name("notify %p %s", self.get(), self->root_path.c_str());
    try {
      self->notifyThread(root);
    } catch (const std::exception& e) {
      watchman::log(watchman::ERR, "Exception: ", e.what(), " cancel root\n");
      root->cancel();
    }
    watchman::log(watchman::DBG, "out of loop\n");
  });
  notifyThreadInstance.detach();

  // Wait for it to signal that the watcher has been initialized
  bool pinged = false;
  pending_.lockAndWait(std::chrono::milliseconds(-1) /* infinite */, pinged);

  // And now start the IO thread
  std::thread ioThreadInstance([self, root]() {
    w_set_thread_name("io %p %s", self.get(), self->root_path.c_str());
    try {
      self->ioThread(root);
    } catch (const std::exception& e) {
      watchman::log(watchman::ERR, "Exception: ", e.what(), " cancel root\n");
      root->cancel();
    }
    watchman::log(watchman::DBG, "out of loop\n");
  });
  ioThreadInstance.detach();
}

void InMemoryView::signalThreads() {
  w_log(W_LOG_DBG, "signalThreads! %p %s\n", this, root_path.c_str());
  stopThreads_ = true;
  watcher_->signalThreads();
  pending_.ping();
}

void InMemoryView::wakeThreads() {
  pending_.ping();
}

bool InMemoryView::doAnyOfTheseFilesExist(
    const std::vector<w_string>& fileNames) const {
  auto view = view_.rlock();
  for (auto& name : fileNames) {
    auto fullName = w_string::pathCat({root_path, name});
    const auto dir = resolveDir(view, fullName.dirName());
    if (!dir) {
      continue;
    }

    auto file = dir->getChildFile(fullName.baseName());
    if (!file) {
      continue;
    }
    if (file->exists) {
      return true;
    }
  }
  return false;
}

const std::shared_ptr<Watcher>& InMemoryView::getWatcher() const {
  return watcher_;
}

const w_string& InMemoryView::getName() const {
  return watcher_->name;
}

SCM* InMemoryView::getSCM() const {
  return scm_.get();
}

void InMemoryView::warmContentCache() {
  if (!enableContentCacheWarming_) {
    return;
  }

  watchman::log(
      watchman::DBG, "considering files for content hash cache warming\n");

  size_t n = 0;
  std::deque<folly::Future<std::shared_ptr<const ContentHashCache::Node>>>
      futures;

  {
    // Walk back in time until we hit the boundary, or hit the limit
    // on the number of files we should warm up.
    auto view = view_.rlock();
    struct watchman_file* f;
    for (f = view->latest_file; f && n < maxFilesToWarmInContentCache_;
         f = f->next) {
      if (f->otime.ticks <= lastWarmedTick_) {
        watchman::log(
            watchman::DBG,
            "warmContentCache: stop because file ticks ",
            f->otime.ticks,
            " is <= lastWarmedTick_ ",
            lastWarmedTick_,
            "\n");
        break;
      }

      if (f->exists && f->stat.isFile()) {
        // Note: we could also add an expression to further constrain
        // the things we warm up here.  Let's see if we need it before
        // going ahead and adding.

        auto dirStr = f->parent->getFullPath();
        w_string_piece dir(dirStr);
        dir.advance(caches_.contentHashCache.rootPath().size());

        // If dirName is the root, dir.size() will now be zero
        if (dir.size() > 0) {
          // if not at the root, skip the slash character at the
          // front of dir
          dir.advance(1);
        }
        ContentHashCacheKey key{w_string::pathCat({dir, f->getName()}),
                                size_t(f->stat.size),
                                f->stat.mtime};

        watchman::log(
            watchman::DBG, "warmContentCache: lookup ", key.relativePath, "\n");
        auto f = caches_.contentHashCache.get(key);
        if (syncContentCacheWarming_) {
          futures.emplace_back(std::move(f));
        }
        ++n;
      }
    }

    lastWarmedTick_ = mostRecentTick_;
  }

  watchman::log(
      watchman::DBG,
      "warmContentCache, lastWarmedTick_ now ",
      lastWarmedTick_,
      " scheduled ",
      n,
      " files for hashing, will wait for ",
      futures.size(),
      " lookups to finish\n");

  if (syncContentCacheWarming_) {
    // Wait for them to finish, but don't use get() because we don't
    // care about any errors that may have occurred.
    folly::collectAll(futures.begin(), futures.end()).wait();
    watchman::log(watchman::DBG, "warmContentCache: hashing complete\n");
  }
}

namespace {
void addCacheStats(json_ref& resp, const CacheStats& stats) {
  resp.set({{"cacheHit", json_integer(stats.cacheHit)},
            {"cacheShare", json_integer(stats.cacheShare)},
            {"cacheMiss", json_integer(stats.cacheMiss)},
            {"cacheEvict", json_integer(stats.cacheEvict)},
            {"cacheStore", json_integer(stats.cacheStore)},
            {"cacheLoad", json_integer(stats.cacheLoad)},
            {"cacheErase", json_integer(stats.cacheErase)},
            {"clearCount", json_integer(stats.clearCount)},
            {"size", json_integer(stats.size)}});
}

} // namespace

void InMemoryView::debugContentHashCache(
    struct watchman_client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(
        client, "wrong number of arguments for 'debug-contenthash'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
  if (!view) {
    send_error_response(client, "root is not an InMemoryView watcher");
    return;
  }

  auto stats = view->caches_.contentHashCache.stats();
  auto resp = make_response();
  addCacheStats(resp, stats);
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "debug-contenthash",
    InMemoryView::debugContentHashCache,
    CMD_DAEMON,
    w_cmd_realpath_root);

void InMemoryView::debugSymlinkTargetCache(
    struct watchman_client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(
        client, "wrong number of arguments for 'debug-symlink-target-cache'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
  if (!view) {
    send_error_response(client, "root is not an InMemoryView watcher");
    return;
  }

  auto stats = view->caches_.symlinkTargetCache.stats();
  auto resp = make_response();
  addCacheStats(resp, stats);
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "debug-symlink-target-cache",
    InMemoryView::debugSymlinkTargetCache,
    CMD_DAEMON,
    w_cmd_realpath_root);
} // namespace watchman
