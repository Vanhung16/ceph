// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <iostream>
#include <optional>
#include <vector>
#include <utility>
#include <functional>

#include <boost/intrusive_ptr.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

#include <seastar/core/future.hh>

#include "include/ceph_assert.h"
#include "include/buffer.h"

#include "crimson/osd/exceptions.h"

#include "crimson/os/seastore/logging.h"
#include "crimson/os/seastore/async_cleaner.h"
#include "crimson/os/seastore/seastore_types.h"
#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/lba_manager.h"
#include "crimson/os/seastore/backref_manager.h"
#include "crimson/os/seastore/journal.h"
#include "crimson/os/seastore/extent_placement_manager.h"
#include "crimson/os/seastore/device.h"
#include "crimson/os/seastore/segment_manager_group.h"

namespace crimson::os::seastore {
class Journal;

struct tm_make_config_t {
  bool is_test;
  journal_type_t j_type;
  bool epm_prefer_ool;
  reclaim_gen_t default_generation;

  static tm_make_config_t get_default() {
    return tm_make_config_t {
      false,
      journal_type_t::SEGMENT_JOURNAL,
      false
    };
  }
  static tm_make_config_t get_test_segmented_journal() {
    LOG_PREFIX(get_test_segmented_journal);
    SUBWARN(seastore_tm, "test mode enabled!");
    return tm_make_config_t {
      true,
      journal_type_t::SEGMENT_JOURNAL,
      false
    };
  }
  static tm_make_config_t get_test_cb_journal() {
    LOG_PREFIX(get_test_cb_journal);
    SUBWARN(seastore_tm, "test mode enabled!");
    return tm_make_config_t {
      true,
      journal_type_t::CIRCULARBOUNDED_JOURNAL,
      true
    };
  }

  tm_make_config_t(const tm_make_config_t &) = default;
  tm_make_config_t &operator=(const tm_make_config_t &) = default;
private:
  tm_make_config_t(
    bool is_test,
    journal_type_t j_type,
    bool epm_prefer_ool)
    : is_test(is_test), j_type(j_type),
      epm_prefer_ool(epm_prefer_ool)
  {}
};

template <typename F>
auto repeat_eagain(F &&f) {
  return seastar::do_with(
    std::forward<F>(f),
    [](auto &f)
  {
    return crimson::repeat([&f] {
      return std::invoke(f
      ).safe_then([] {
        return seastar::stop_iteration::yes;
      }).handle_error(
        [](const crimson::ct_error::eagain &e) {
          return seastar::stop_iteration::no;
        },
        crimson::ct_error::pass_further_all{}
      );
    });
  });
}

/**
 * TransactionManager
 *
 * Abstraction hiding reading and writing to persistence.
 * Exposes transaction based interface with read isolation.
 */
class TransactionManager : public AsyncCleaner::ExtentCallbackInterface {
public:
  using base_ertr = Cache::base_ertr;
  using base_iertr = Cache::base_iertr;

  TransactionManager(
    AsyncCleanerRef async_cleaner,
    JournalRef journal,
    CacheRef cache,
    LBAManagerRef lba_manager,
    ExtentPlacementManagerRef &&epm,
    BackrefManagerRef&& backref_manager);

  /// Writes initial metadata to disk
  using mkfs_ertr = base_ertr;
  mkfs_ertr::future<> mkfs();

  /// Reads initial metadata from disk
  using mount_ertr = base_ertr;
  mount_ertr::future<> mount();

  /// Closes transaction_manager
  using close_ertr = base_ertr;
  close_ertr::future<> close();

  /// Creates empty transaction
  TransactionRef create_transaction(
      Transaction::src_t src,
      const char* name) final {
    return cache->create_transaction(src, name, false);
  }

  /// Creates empty weak transaction
  TransactionRef create_weak_transaction(
      Transaction::src_t src,
      const char* name) {
    return cache->create_transaction(src, name, true);
  }

  /// Resets transaction
  void reset_transaction_preserve_handle(Transaction &t) {
    return cache->reset_transaction_preserve_handle(t);
  }

  /**
   * get_pin
   *
   * Get the logical pin at offset
   */
  using get_pin_iertr = LBAManager::get_mapping_iertr;
  using get_pin_ret = LBAManager::get_mapping_iertr::future<LBAPinRef>;
  get_pin_ret get_pin(
    Transaction &t,
    laddr_t offset) {
    LOG_PREFIX(TransactionManager::get_pin);
    SUBTRACET(seastore_tm, "{}", t, offset);
    return lba_manager->get_mapping(t, offset);
  }

  /**
   * get_pins
   *
   * Get logical pins overlapping offset~length
   */
  using get_pins_iertr = LBAManager::get_mappings_iertr;
  using get_pins_ret = get_pins_iertr::future<lba_pin_list_t>;
  get_pins_ret get_pins(
    Transaction &t,
    laddr_t offset,
    extent_len_t length) {
    LOG_PREFIX(TransactionManager::get_pins);
    SUBDEBUGT(seastore_tm, "{}~{}", t, offset, length);
    return lba_manager->get_mappings(
      t, offset, length);
  }

  /**
   * pin_to_extent
   *
   * Get extent mapped at pin.
   */
  using pin_to_extent_iertr = get_pin_iertr::extend_ertr<
    SegmentManager::read_ertr>;
  template <typename T>
  using pin_to_extent_ret = pin_to_extent_iertr::future<
    TCachedExtentRef<T>>;
  template <typename T>
  pin_to_extent_ret<T> pin_to_extent(
    Transaction &t,
    LBAPinRef pin) {
    LOG_PREFIX(TransactionManager::pin_to_extent);
    SUBTRACET(seastore_tm, "getting extent {}", t, *pin);
    using ret = pin_to_extent_ret<T>;
    auto &pref = *pin;
    return cache->get_extent<T>(
      t,
      pref.get_val(),
      pref.get_length(),
      [this, pin=std::move(pin)](T &extent) mutable {
	assert(!extent.has_pin());
	assert(!extent.has_been_invalidated());
	assert(!pin->has_been_invalidated());
	extent.set_pin(std::move(pin));
	lba_manager->add_pin(extent.get_pin());
      }
    ).si_then([FNAME, &t](auto ref) mutable -> ret {
      SUBTRACET(seastore_tm, "got extent -- {}", t, *ref);
      return pin_to_extent_ret<T>(
	interruptible::ready_future_marker{},
	std::move(ref));
    });
  }

  /**
   * read_extent
   *
   * Read extent of type T at offset~length
   */
  using read_extent_iertr = get_pin_iertr::extend_ertr<
    SegmentManager::read_ertr>;
  template <typename T>
  using read_extent_ret = read_extent_iertr::future<
    TCachedExtentRef<T>>;
  template <typename T>
  read_extent_ret<T> read_extent(
    Transaction &t,
    laddr_t offset,
    extent_len_t length) {
    LOG_PREFIX(TransactionManager::read_extent);
    SUBTRACET(seastore_tm, "{}~{}", t, offset, length);
    return get_pin(
      t, offset
    ).si_then([this, FNAME, &t, offset, length] (auto pin) {
      if (length != pin->get_length() || !pin->get_val().is_real()) {
        SUBERRORT(seastore_tm,
            "offset {} len {} got wrong pin {}",
            t, offset, length, *pin);
        ceph_assert(0 == "Should be impossible");
      }
      return this->pin_to_extent<T>(t, std::move(pin));
    });
  }

  /**
   * read_extent
   *
   * Read extent of type T at offset
   */
  template <typename T>
  read_extent_ret<T> read_extent(
    Transaction &t,
    laddr_t offset) {
    LOG_PREFIX(TransactionManager::read_extent);
    SUBTRACET(seastore_tm, "{}", t, offset);
    return get_pin(
      t, offset
    ).si_then([this, FNAME, &t, offset] (auto pin) {
      if (!pin->get_val().is_real()) {
        SUBERRORT(seastore_tm,
            "offset {} got wrong pin {}",
            t, offset, *pin);
        ceph_assert(0 == "Should be impossible");
      }
      return this->pin_to_extent<T>(t, std::move(pin));
    });
  }

  /// Obtain mutable copy of extent
  LogicalCachedExtentRef get_mutable_extent(Transaction &t, LogicalCachedExtentRef ref) {
    LOG_PREFIX(TransactionManager::get_mutable_extent);
    auto ret = cache->duplicate_for_write(
      t,
      ref)->cast<LogicalCachedExtent>();
    if (!ret->has_pin()) {
      SUBDEBUGT(seastore_tm,
	"duplicating extent for write -- {} -> {}",
	t,
	*ref,
	*ret);
      ret->set_pin(ref->get_pin().duplicate());
    } else {
      SUBTRACET(seastore_tm,
	"extent is already duplicated -- {}",
	t,
	*ref);
      assert(ref->is_pending());
      assert(&*ref == &*ret);
    }
    return ret;
  }


  using ref_iertr = LBAManager::ref_iertr;
  using ref_ret = ref_iertr::future<unsigned>;

  /// Add refcount for ref
  ref_ret inc_ref(
    Transaction &t,
    LogicalCachedExtentRef &ref);

  /// Add refcount for offset
  ref_ret inc_ref(
    Transaction &t,
    laddr_t offset);

  /// Remove refcount for ref
  ref_ret dec_ref(
    Transaction &t,
    LogicalCachedExtentRef &ref);

  /// Remove refcount for offset
  ref_ret dec_ref(
    Transaction &t,
    laddr_t offset);

  /// remove refcount for list of offset
  using refs_ret = ref_iertr::future<std::vector<unsigned>>;
  refs_ret dec_ref(
    Transaction &t,
    std::vector<laddr_t> offsets);

  /**
   * alloc_extent
   *
   * Allocates a new block of type T with the minimum lba range of size len
   * greater than laddr_hint.
   */
  using alloc_extent_iertr = LBAManager::alloc_extent_iertr;
  template <typename T>
  using alloc_extent_ret = alloc_extent_iertr::future<TCachedExtentRef<T>>;
  template <typename T>
  alloc_extent_ret<T> alloc_extent(
    Transaction &t,
    laddr_t laddr_hint,
    extent_len_t len,
    placement_hint_t placement_hint = placement_hint_t::HOT) {
    LOG_PREFIX(TransactionManager::alloc_extent);
    SUBTRACET(seastore_tm, "{} len={}, placement_hint={}, laddr_hint={}",
              t, T::TYPE, len, placement_hint, laddr_hint);
    ceph_assert(is_aligned(laddr_hint, (uint64_t)epm->get_block_size()));
    auto ext = cache->alloc_new_extent<T>(
      t,
      len,
      placement_hint,
      0);
    return lba_manager->alloc_extent(
      t,
      laddr_hint,
      len,
      ext->get_paddr()
    ).si_then([ext=std::move(ext), laddr_hint, &t, FNAME](auto &&ref) mutable {
      ext->set_pin(std::move(ref));
      SUBDEBUGT(seastore_tm, "new extent: {}, laddr_hint: {}", t, *ext, laddr_hint);
      return alloc_extent_iertr::make_ready_future<TCachedExtentRef<T>>(
	std::move(ext));
    });
  }

  /**
   * map_existing_extent
   *
   * Allocates a new extent at given existing_paddr that must be absolute and
   * reads disk to fill the extent.
   * The common usage is that remove the LogicalCachedExtent (laddr~length at paddr)
   * and map extent to multiple new extents.
   * placement_hint and generation should follow the original extent.
   */
  using map_existing_extent_iertr =
    alloc_extent_iertr::extend_ertr<Device::read_ertr>;
  template <typename T>
  using map_existing_extent_ret =
    map_existing_extent_iertr::future<TCachedExtentRef<T>>;
  template <typename T>
  map_existing_extent_ret<T> map_existing_extent(
    Transaction &t,
    laddr_t laddr_hint,
    paddr_t existing_paddr,
    extent_len_t length,
    placement_hint_t placement_hint = placement_hint_t::HOT,
    reclaim_gen_t gen = DIRTY_GENERATION) {
    LOG_PREFIX(TransactionManager::map_existing_extent);
    ceph_assert(existing_paddr.is_absolute());
    assert(t.is_retired(existing_paddr, length));

    auto bp = ceph::bufferptr(buffer::create_page_aligned(length));
    bp.zero();

    // ExtentPlacementManager::alloc_new_extent will make a new
    // (relative/temp) paddr, so make extent directly
    auto ext = CachedExtent::make_cached_extent_ref<T>(std::move(bp));

    ext->init(CachedExtent::extent_state_t::EXIST_CLEAN,
	      existing_paddr,
	      placement_hint,
	      gen);

    t.add_fresh_extent(ext);

    return lba_manager->alloc_extent(
      t,
      laddr_hint,
      length,
      existing_paddr
    ).si_then([ext=std::move(ext), laddr_hint, &t, this, FNAME](auto &&ref) {
      SUBDEBUGT(seastore_tm, "map existing extent: {}, laddr_hint: {} pin: {}",
		t, *ext, laddr_hint, *ref);
      ceph_assert(laddr_hint == ref->get_key());
      ext->set_pin(std::move(ref));
      return epm->read(
        ext->get_paddr(),
	ext->get_length(),
	ext->get_bptr()
      ).safe_then([ext=std::move(ext)] {
	return map_existing_extent_iertr::make_ready_future<TCachedExtentRef<T>>
	  (std::move(ext));
      });
    });
  }


  using reserve_extent_iertr = alloc_extent_iertr;
  using reserve_extent_ret = reserve_extent_iertr::future<LBAPinRef>;
  reserve_extent_ret reserve_region(
    Transaction &t,
    laddr_t hint,
    extent_len_t len) {
    LOG_PREFIX(TransactionManager::reserve_region);
    SUBDEBUGT(seastore_tm, "len={}, laddr_hint={}", t, len, hint);
    ceph_assert(is_aligned(hint, (uint64_t)epm->get_block_size()));
    return lba_manager->alloc_extent(
      t,
      hint,
      len,
      P_ADDR_ZERO);
  }

  /* alloc_extents
   *
   * allocates more than one new blocks of type T.
   */
   using alloc_extents_iertr = alloc_extent_iertr;
   template<class T>
   alloc_extents_iertr::future<std::vector<TCachedExtentRef<T>>>
   alloc_extents(
     Transaction &t,
     laddr_t hint,
     extent_len_t len,
     int num) {
     LOG_PREFIX(TransactionManager::alloc_extents);
     SUBDEBUGT(seastore_tm, "len={}, laddr_hint={}, num={}",
               t, len, hint, num);
     return seastar::do_with(std::vector<TCachedExtentRef<T>>(),
       [this, &t, hint, len, num] (auto &extents) {
       return trans_intr::do_for_each(
                       boost::make_counting_iterator(0),
                       boost::make_counting_iterator(num),
         [this, &t, len, hint, &extents] (auto i) {
         return alloc_extent<T>(t, hint, len).si_then(
           [&extents](auto &&node) {
           extents.push_back(node);
         });
       }).si_then([&extents] {
         return alloc_extents_iertr::make_ready_future
                <std::vector<TCachedExtentRef<T>>>(std::move(extents));
       });
     });
  }

  /**
   * submit_transaction
   *
   * Atomically submits transaction to persistence
   */
  using submit_transaction_iertr = base_iertr;
  submit_transaction_iertr::future<> submit_transaction(Transaction &);

  /// AsyncCleaner::ExtentCallbackInterface
  using AsyncCleaner::ExtentCallbackInterface::submit_transaction_direct_ret;
  submit_transaction_direct_ret submit_transaction_direct(
    Transaction &t,
    std::optional<journal_seq_t> seq_to_trim = std::nullopt,
    std::optional<std::pair<paddr_t, paddr_t>> gc_range = std::nullopt) final;

  /**
   * flush
   *
   * Block until all outstanding IOs on handle are committed.
   * Note, flush() machinery must go through the same pipeline
   * stages and locks as submit_transaction.
   */
  seastar::future<> flush(OrderingHandle &handle);

  using AsyncCleaner::ExtentCallbackInterface::get_next_dirty_extents_ret;
  get_next_dirty_extents_ret get_next_dirty_extents(
    Transaction &t,
    journal_seq_t seq,
    size_t max_bytes) final;

  using AsyncCleaner::ExtentCallbackInterface::rewrite_extent_ret;
  rewrite_extent_ret rewrite_extent(
    Transaction &t,
    CachedExtentRef extent,
    reclaim_gen_t target_generation,
    sea_time_point modify_time) final;

  using AsyncCleaner::ExtentCallbackInterface::get_extents_if_live_ret;
  get_extents_if_live_ret get_extents_if_live(
    Transaction &t,
    extent_types_t type,
    paddr_t addr,
    laddr_t laddr,
    seastore_off_t len) final;

  /**
   * read_root_meta
   *
   * Read root block meta entry for key.
   */
  using read_root_meta_iertr = base_iertr;
  using read_root_meta_bare = std::optional<std::string>;
  using read_root_meta_ret = read_root_meta_iertr::future<
    read_root_meta_bare>;
  read_root_meta_ret read_root_meta(
    Transaction &t,
    const std::string &key) {
    return cache->get_root(
      t
    ).si_then([&key, &t](auto root) {
      LOG_PREFIX(TransactionManager::read_root_meta);
      auto meta = root->root.get_meta();
      auto iter = meta.find(key);
      if (iter == meta.end()) {
        SUBDEBUGT(seastore_tm, "{} -> nullopt", t, key);
	return seastar::make_ready_future<read_root_meta_bare>(std::nullopt);
      } else {
        SUBDEBUGT(seastore_tm, "{} -> {}", t, key, iter->second);
	return seastar::make_ready_future<read_root_meta_bare>(iter->second);
      }
    });
  }

  /**
   * update_root_meta
   *
   * Update root block meta entry for key to value.
   */
  using update_root_meta_iertr = base_iertr;
  using update_root_meta_ret = update_root_meta_iertr::future<>;
  update_root_meta_ret update_root_meta(
    Transaction& t,
    const std::string& key,
    const std::string& value) {
    LOG_PREFIX(TransactionManager::update_root_meta);
    SUBDEBUGT(seastore_tm, "seastore_tm, {} -> {}", t, key, value);
    return cache->get_root(
      t
    ).si_then([this, &t, &key, &value](RootBlockRef root) {
      root = cache->duplicate_for_write(t, root)->cast<RootBlock>();

      auto meta = root->root.get_meta();
      meta[key] = value;

      root->root.set_meta(meta);
      return seastar::now();
    });
  }

  /**
   * read_onode_root
   *
   * Get onode-tree root logical address
   */
  using read_onode_root_iertr = base_iertr;
  using read_onode_root_ret = read_onode_root_iertr::future<laddr_t>;
  read_onode_root_ret read_onode_root(Transaction &t) {
    return cache->get_root(t).si_then([&t](auto croot) {
      LOG_PREFIX(TransactionManager::read_onode_root);
      laddr_t ret = croot->get_root().onode_root;
      SUBTRACET(seastore_tm, "{}", t, ret);
      return ret;
    });
  }

  /**
   * write_onode_root
   *
   * Write onode-tree root logical address, must be called after read.
   */
  void write_onode_root(Transaction &t, laddr_t addr) {
    LOG_PREFIX(TransactionManager::write_onode_root);
    SUBDEBUGT(seastore_tm, "{}", t, addr);
    auto croot = cache->get_root_fast(t);
    croot = cache->duplicate_for_write(t, croot)->cast<RootBlock>();
    croot->get_root().onode_root = addr;
  }

  /**
   * read_collection_root
   *
   * Get collection root addr
   */
  using read_collection_root_iertr = base_iertr;
  using read_collection_root_ret = read_collection_root_iertr::future<
    coll_root_t>;
  read_collection_root_ret read_collection_root(Transaction &t) {
    return cache->get_root(t).si_then([&t](auto croot) {
      LOG_PREFIX(TransactionManager::read_collection_root);
      auto ret = croot->get_root().collection_root.get();
      SUBTRACET(seastore_tm, "{}~{}",
                t, ret.get_location(), ret.get_size());
      return ret;
    });
  }

  /**
   * write_collection_root
   *
   * Update collection root addr
   */
  void write_collection_root(Transaction &t, coll_root_t cmroot) {
    LOG_PREFIX(TransactionManager::write_collection_root);
    SUBDEBUGT(seastore_tm, "{}~{}",
              t, cmroot.get_location(), cmroot.get_size());
    auto croot = cache->get_root_fast(t);
    croot = cache->duplicate_for_write(t, croot)->cast<RootBlock>();
    croot->get_root().collection_root.update(cmroot);
  }

  extent_len_t get_block_size() const {
    return epm->get_block_size();
  }

  store_statfs_t store_stat() const {
    return async_cleaner->stat();
  }

  void add_device(Device* dev, bool is_primary) {
    LOG_PREFIX(TransactionManager::add_device);
    SUBDEBUG(seastore_tm, "adding device {}, is_primary={}",
             dev->get_device_id(), is_primary);
    epm->add_device(dev, is_primary);

    if (dev->get_device_type() == device_type_t::SEGMENTED) {
      auto sm = dynamic_cast<SegmentManager*>(dev);
      ceph_assert(sm != nullptr);
      sm_group.add_segment_manager(sm);
    }
  }

  ~TransactionManager();

private:
  friend class Transaction;

  AsyncCleanerRef async_cleaner;
  CacheRef cache;
  LBAManagerRef lba_manager;
  JournalRef journal;
  ExtentPlacementManagerRef epm;
  BackrefManagerRef backref_manager;
  SegmentManagerGroup &sm_group;

  WritePipeline write_pipeline;

  rewrite_extent_ret rewrite_logical_extent(
    Transaction& t,
    LogicalCachedExtentRef extent);

public:
  // Testing interfaces
  auto get_async_cleaner() {
    return async_cleaner.get();
  }

  auto get_lba_manager() {
    return lba_manager.get();
  }

  auto get_backref_manager() {
    return backref_manager.get();
  }

  auto get_cache() {
    return cache.get();
  }
  auto get_journal() {
    return journal.get();
  }
};
using TransactionManagerRef = std::unique_ptr<TransactionManager>;

TransactionManagerRef make_transaction_manager(tm_make_config_t config);
}
