////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

#include "Cache/Cache.h"

#include "Basics/ScopeGuard.h"
#include "Basics/SpinLocker.h"
#include "Basics/SpinUnlocker.h"
#include "Basics/voc-errors.h"
#include "Cache/CachedValue.h"
#include "Cache/Common.h"
#include "Cache/Manager.h"
#include "Cache/Metadata.h"
#include "Cache/PlainCache.h"
#include "Cache/Table.h"
#include "Cache/TransactionalCache.h"
#include "Random/RandomGenerator.h"
#include "RestServer/SharedPRNGFeature.h"

namespace arangodb::cache {

using SpinLocker = ::arangodb::basics::SpinLocker;
using SpinUnlocker = ::arangodb::basics::SpinUnlocker;

Cache::Cache(Manager* manager, std::uint64_t id, Metadata&& metadata,
             std::shared_ptr<Table> table, bool enableWindowedStats,
             std::function<Table::BucketClearer(Metadata*)> bucketClearer,
             std::size_t slotsPerBucket)
    : _shutdown(false),
      _enableWindowedStats(enableWindowedStats),
      _findHits(),
      _findMisses(),
      _manager(manager),
      _id(id),
      _metadata(std::move(metadata)),
      _table(std::move(table)),
      _bucketClearer(bucketClearer(&_metadata)),
      _slotsPerBucket(slotsPerBucket),
      _insertsTotal(),
      _insertEvictions(),
      _migrateRequestTime(
          std::chrono::steady_clock::now().time_since_epoch().count()),
      _resizeRequestTime(
          std::chrono::steady_clock::now().time_since_epoch().count()) {
  TRI_ASSERT(_table != nullptr);
  _table->setTypeSpecifics(_bucketClearer, _slotsPerBucket);
  _table->enable();
  if (_enableWindowedStats) {
    try {
      _findStats = std::make_unique<StatBuffer>(manager->sharedPRNG(),
                                                findStatsCapacity);
    } catch (std::bad_alloc const&) {
      _enableWindowedStats = false;
    }
  }
}

std::uint64_t Cache::size() const noexcept {
  if (ADB_UNLIKELY(isShutdown())) {
    return 0;
  }

  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  return _metadata.allocatedSize;
}

std::uint64_t Cache::usageLimit() const noexcept {
  if (ADB_UNLIKELY(isShutdown())) {
    return 0;
  }

  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  return _metadata.softUsageLimit;
}

std::uint64_t Cache::usage() const noexcept {
  if (ADB_UNLIKELY(isShutdown())) {
    return 0;
  }

  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  return _metadata.usage;
}

std::pair<std::uint64_t, std::uint64_t> Cache::sizeAndUsage() const noexcept {
  if (ADB_UNLIKELY(isShutdown())) {
    return {0, 0};
  }

  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  return {_metadata.allocatedSize, _metadata.usage};
}

void Cache::sizeHint(uint64_t numElements) {
  if (ADB_UNLIKELY(isShutdown())) {
    return;
  }

  std::uint64_t numBuckets = static_cast<std::uint64_t>(
      static_cast<double>(numElements) /
      (static_cast<double>(_slotsPerBucket) * _manager->idealUpperFillRatio()));
  std::uint32_t requestedLogSize = 0;
  for (; (static_cast<std::uint64_t>(1) << requestedLogSize) < numBuckets;
       requestedLogSize++) {
  }
  requestMigrate(requestedLogSize);
}

std::pair<double, double> Cache::hitRates() {
  double lifetimeRate = std::nan("");
  double windowedRate = std::nan("");

  std::uint64_t currentMisses = _findMisses.value(std::memory_order_relaxed);
  std::uint64_t currentHits = _findHits.value(std::memory_order_relaxed);
  if (currentMisses + currentHits > 0) {
    lifetimeRate = 100 * (static_cast<double>(currentHits) /
                          static_cast<double>(currentHits + currentMisses));
  }

  if (_enableWindowedStats && _findStats) {
    auto stats = _findStats->getFrequencies();
    if (stats.size() == 1) {
      if (stats[0].first == static_cast<std::uint8_t>(Stat::findHit)) {
        windowedRate = 100.0;
      } else {
        windowedRate = 0.0;
      }
    } else if (stats.size() == 2) {
      if (stats[0].first == static_cast<std::uint8_t>(Stat::findHit)) {
        currentHits = stats[0].second;
        currentMisses = stats[1].second;
      } else {
        currentHits = stats[1].second;
        currentMisses = stats[0].second;
      }
      if (currentHits + currentMisses > 0) {
        windowedRate = 100 * (static_cast<double>(currentHits) /
                              static_cast<double>(currentHits + currentMisses));
      }
    }
  }

  return std::pair<double, double>(lifetimeRate, windowedRate);
}

bool Cache::isResizing() const noexcept {
  if (ADB_UNLIKELY(isShutdown())) {
    return false;
  }

  return isResizingFlagSet();
}

bool Cache::isResizingFlagSet() const noexcept {
  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  return _metadata.isResizing();
}

bool Cache::isMigrating() const noexcept {
  if (ADB_UNLIKELY(isShutdown())) {
    return false;
  }

  return isMigratingFlagSet();
}

bool Cache::isMigratingFlagSet() const noexcept {
  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  return _metadata.isMigrating();
}

bool Cache::isResizingOrMigratingFlagSet() const noexcept {
  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  return _metadata.isResizing() || _metadata.isMigrating();
}

void Cache::destroy(std::shared_ptr<Cache> const& cache) {
  if (cache != nullptr) {
    cache->shutdown();
  }
}

void Cache::requestGrow() {
  // fail fast if inside banned window
  if (isShutdown() ||
      std::chrono::steady_clock::now().time_since_epoch().count() <=
          _resizeRequestTime.load()) {
    return;
  }

  SpinLocker taskGuard(SpinLocker::Mode::Write, _taskLock,
                       static_cast<std::size_t>(Cache::triesSlow));
  if (taskGuard.isLocked()) {
    if (std::chrono::steady_clock::now().time_since_epoch().count() >
        _resizeRequestTime.load()) {
      bool ok = false;
      {
        SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
        ok = !_metadata.isResizing();
      }
      if (ok) {
        auto result = _manager->requestGrow(this);
        _resizeRequestTime.store(result.second.time_since_epoch().count());
      }
    }
  }
}

void Cache::requestMigrate(std::uint32_t requestedLogSize) {
  // fail fast if inside banned window
  if (isShutdown() ||
      std::chrono::steady_clock::now().time_since_epoch().count() <=
          _migrateRequestTime.load()) {
    return;
  }

  SpinLocker taskGuard(SpinLocker::Mode::Write, _taskLock);
  if (std::chrono::steady_clock::now().time_since_epoch().count() >
      _migrateRequestTime.load()) {
    std::shared_ptr<cache::Table> table = this->table();
    TRI_ASSERT(table != nullptr);

    bool ok = false;
    {
      SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
      ok = !_metadata.isMigrating() && (requestedLogSize != table->logSize());
    }
    if (ok) {
      auto result = _manager->requestMigrate(this, requestedLogSize);
      _migrateRequestTime.store(result.second.time_since_epoch().count());
    }
  }
}

void Cache::freeValue(CachedValue* value) noexcept {
  while (!value->isFreeable()) {
    std::this_thread::yield();
  }

  delete value;
}

bool Cache::reclaimMemory(std::uint64_t size) noexcept {
  SpinLocker metaGuard(SpinLocker::Mode::Read, _metadata.lock());
  _metadata.adjustUsageIfAllowed(-static_cast<std::int64_t>(size));
  return (_metadata.softUsageLimit >= _metadata.usage);
}

void Cache::recordStat(Stat stat) {
  if ((_manager->sharedPRNG().rand() & static_cast<unsigned long>(7)) != 0) {
    return;
  }

  switch (stat) {
    case Stat::findHit: {
      _findHits.add(1, std::memory_order_relaxed);
      if (_enableWindowedStats && _findStats) {
        _findStats->insertRecord(static_cast<std::uint8_t>(Stat::findHit));
      }
      _manager->reportHitStat(Stat::findHit);
      break;
    }
    case Stat::findMiss: {
      _findMisses.add(1, std::memory_order_relaxed);
      if (_enableWindowedStats && _findStats) {
        _findStats->insertRecord(static_cast<std::uint8_t>(Stat::findMiss));
      }
      _manager->reportHitStat(Stat::findMiss);
      break;
    }
    default: {
      break;
    }
  }
}

bool Cache::reportInsert(bool hadEviction) {
  bool shouldMigrate = false;
  if (hadEviction) {
    _insertEvictions.add(1, std::memory_order_relaxed);
  }
  _insertsTotal.add(1, std::memory_order_relaxed);
  if ((_manager->sharedPRNG().rand() & _evictionMask) == 0) {
    std::uint64_t total = _insertsTotal.value(std::memory_order_relaxed);
    std::uint64_t evictions = _insertEvictions.value(std::memory_order_relaxed);
    if (total > 0 && total > evictions &&
        ((static_cast<double>(evictions) / static_cast<double>(total)) >
         _evictionRateThreshold)) {
      shouldMigrate = true;
      std::shared_ptr<cache::Table> table = this->table();
      TRI_ASSERT(table != nullptr);
      table->signalEvictions();
    }
    _insertEvictions.reset(std::memory_order_relaxed);
    _insertsTotal.reset(std::memory_order_relaxed);
  }

  return shouldMigrate;
}

Metadata& Cache::metadata() { return _metadata; }

std::shared_ptr<Table> Cache::table() const {
  return std::atomic_load_explicit(&_table, std::memory_order_acquire);
}

void Cache::shutdown() {
  SpinLocker taskGuard(SpinLocker::Mode::Write, _taskLock);
  auto handle = shared_from_this();  // hold onto self-reference to prevent
                                     // pre-mature shared_ptr destruction
  TRI_ASSERT(handle.get() == this);
  if (!_shutdown.exchange(true)) {
    while (true) {
      if (!isResizingOrMigratingFlagSet()) {
        break;
      }

      SpinUnlocker taskUnguard(SpinUnlocker::Mode::Write, _taskLock);

      // sleep a bit without holding the locks
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }

    std::shared_ptr<cache::Table> table = this->table();
    if (table != nullptr) {
      std::shared_ptr<Table> extra =
          table->setAuxiliary(std::shared_ptr<Table>());
      if (extra != nullptr) {
        extra->clear();
        _manager->reclaimTable(std::move(extra), false);
      }
      table->clear();
      _manager->reclaimTable(std::move(table), false);
    }

    {
      SpinLocker metaGuard(SpinLocker::Mode::Write, _metadata.lock());
      _metadata.changeTable(0);
    }
    _manager->unregisterCache(_id);
    std::atomic_store_explicit(&_table, std::shared_ptr<cache::Table>(),
                               std::memory_order_release);
  }
}

bool Cache::canResize() noexcept {
  if (ADB_UNLIKELY(isShutdown())) {
    return false;
  }

  return !isResizingOrMigratingFlagSet();
}

/// TODO Improve freeing algorithm
/// Currently we pick a bucket at random, free something if possible, then
/// repeat. In a table with a low fill ratio, this will inevitably waste a lot
/// of time visiting empty buckets. If we get unlucky, we can go an arbitrarily
/// long time without fixing anything. We may wish to make the walk a bit more
/// like visiting the buckets in the order of a fixed random permutation. This
/// should be achievable by picking a random start bucket S, and a suitably
/// large number P co-prime to the size of the table N to use as a constant
/// offset for each subsequent step. (The sequence of numbers S, ((S + P) % N)),
/// ((S + 2P) % N)... (S + (N-1)P) % N should form a permuation of [1, N].
/// That way we still visit the buckets in a sufficiently random order, but we
/// are guaranteed to make progress in a finite amount of time.
bool Cache::freeMemory() {
  TRI_ASSERT(isResizingFlagSet());

  if (ADB_UNLIKELY(isShutdown())) {
    return false;
  }

  auto cb = [this](std::uint64_t reclaimed) -> bool {
    if (reclaimed > 0) {
      bool underLimit = reclaimMemory(reclaimed);
      if (underLimit) {
        // we have free enough memory.
        // don't continue
        return false;
      }
    }
    // check if shutdown is in progress. then give up
    return !isShutdown();
  };

  bool underLimit = reclaimMemory(0ULL);
  if (!underLimit) {
    underLimit = freeMemoryWhile(cb);
  }
  return underLimit;
}

bool Cache::migrate(std::shared_ptr<Table> newTable) {
  TRI_ASSERT(isMigratingFlagSet());

  auto migratingGuard = scopeGuard([this]() noexcept {
    // unmarking migrating flag if necessary
    SpinLocker metaGuard(SpinLocker::Mode::Write, _metadata.lock());

    TRI_ASSERT(_metadata.isMigrating());
    _metadata.toggleMigrating();
    TRI_ASSERT(!_metadata.isMigrating());
  });

  if (ADB_UNLIKELY(isShutdown())) {
    // will trigger the scopeGuard
    return false;
  }

  newTable->setTypeSpecifics(_bucketClearer, _slotsPerBucket);
  newTable->enable();

  std::shared_ptr<cache::Table> table = this->table();
  TRI_ASSERT(table != nullptr);
  std::shared_ptr<Table> oldAuxiliary = table->setAuxiliary(newTable);
  TRI_ASSERT(oldAuxiliary == nullptr);

  // do the actual migration
  for (std::uint64_t i = 0; i < table->size();
       i++) {  // need uint64 for end condition
    migrateBucket(table->primaryBucket(static_cast<uint32_t>(i)),
                  table->auxiliaryBuckets(static_cast<uint32_t>(i)), *newTable);
  }

  // swap tables
  std::shared_ptr<Table> oldTable;
  {
    SpinLocker taskGuard(SpinLocker::Mode::Write, _taskLock);
    oldTable = this->table();
    std::atomic_store_explicit(&_table, newTable, std::memory_order_release);
    oldTable->setAuxiliary(std::shared_ptr<Table>());
  }

  TRI_ASSERT(oldTable != nullptr);

  // unmarking migrating flag
  {
    SpinLocker metaGuard(SpinLocker::Mode::Write, _metadata.lock());
    _metadata.changeTable(newTable->memoryUsage());
    TRI_ASSERT(_metadata.isMigrating());
    _metadata.toggleMigrating();
    TRI_ASSERT(!_metadata.isMigrating());
  }
  migratingGuard.cancel();

  // clear out old table and release it
  oldTable->clear();
  _manager->reclaimTable(std::move(oldTable), false);

  return true;
}

}  // namespace arangodb::cache
