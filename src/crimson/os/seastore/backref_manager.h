// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/cached_extent.h"
#include "crimson/os/seastore/segment_manager_group.h"
#include "crimson/os/seastore/transaction.h"

namespace crimson::os::seastore {

/**
 * Abstract interface for managing back references that map paddr_t to laddr_t
 */
class BackrefManager {
public:
  using base_ertr = crimson::errorator<
    crimson::ct_error::input_output_error>;
  using base_iertr = trans_iertr<base_ertr>;

  using mkfs_iertr = base_iertr;
  using mkfs_ret = mkfs_iertr::future<>;
  virtual mkfs_ret mkfs(
    Transaction &t) = 0;

  /**
   * Fetches mappings for paddr_t in range [offset, offset + len)
   *
   * Future will not resolve until all pins have resolved
   */
  using get_mappings_iertr = base_iertr;
  using get_mappings_ret = get_mappings_iertr::future<backref_pin_list_t>;
  virtual get_mappings_ret get_mappings(
    Transaction &t,
    paddr_t offset,
    paddr_t end) = 0;

  /**
   * Fetches the mapping for paddr_t
   *
   * Future will not resolve until the pin has resolved
   */
  using get_mapping_iertr = base_iertr::extend<
    crimson::ct_error::enoent>;
  using get_mapping_ret = get_mapping_iertr::future<BackrefPinRef>;
  virtual get_mapping_ret  get_mapping(
    Transaction &t,
    paddr_t offset) = 0;

  /**
   * rewrite_extent
   *
   * rewrite extent into passed transaction
   */
  using rewrite_extent_iertr = base_iertr;
  using rewrite_extent_ret = rewrite_extent_iertr::future<>;
  virtual rewrite_extent_ret rewrite_extent(
    Transaction &t,
    CachedExtentRef extent) = 0;

  /**
   * Insert new paddr_t -> laddr_t mapping
   */
  using new_mapping_iertr = base_iertr;
  using new_mapping_ret = new_mapping_iertr::future<BackrefPinRef>;
  virtual new_mapping_ret new_mapping(
    Transaction &t,
    paddr_t key,
    extent_len_t len,
    laddr_t val,
    extent_types_t type) = 0;

  /**
   * Check if a CachedExtent is alive, should be called
   * after replay on each cached extent.
   *
   * @return returns whether the extent is alive
   */
  using init_cached_extent_iertr = base_iertr;
  using init_cached_extent_ret = init_cached_extent_iertr::future<bool>;
  virtual init_cached_extent_ret init_cached_extent(
    Transaction &t,
    CachedExtentRef e) = 0;

  virtual Cache::backref_buf_entry_query_set_t
  get_cached_backrefs_in_range(
    paddr_t start,
    paddr_t end) = 0;
  virtual const backref_set_t& get_cached_backrefs() = 0;

  virtual Cache::backref_extent_buf_entry_query_set_t
  get_cached_backref_extents_in_range(
    paddr_t start,
    paddr_t end) = 0;

  using retrieve_backref_extents_iertr = trans_iertr<
    crimson::errorator<
      crimson::ct_error::input_output_error>
    >;
  using retrieve_backref_extents_ret =
    retrieve_backref_extents_iertr::future<>;
  virtual retrieve_backref_extents_ret retrieve_backref_extents(
    Transaction &t,
    Cache::backref_extent_buf_entry_query_set_t &&backref_extents,
    std::vector<CachedExtentRef> &extents) = 0;

  virtual void cache_new_backref_extent(paddr_t paddr, extent_types_t type) = 0;

  /**
   * merge in-cache paddr_t -> laddr_t mappings to the on-disk backref tree
   */
  using merge_cached_backrefs_iertr = base_iertr;
  using merge_cached_backrefs_ret = merge_cached_backrefs_iertr::future<journal_seq_t>;
  virtual merge_cached_backrefs_ret merge_cached_backrefs(
    Transaction &t,
    const journal_seq_t &limit,
    const uint64_t max) = 0;

  struct remove_mapping_result_t {
    paddr_t offset = P_ADDR_NULL;
    extent_len_t len = 0;
    laddr_t laddr = L_ADDR_NULL;
  };

  /**
   * delete the mapping for paddr_t offset
   */
  using remove_mapping_iertr = base_iertr::extend<
    crimson::ct_error::enoent>;
  using remove_mapping_ret = remove_mapping_iertr::future<remove_mapping_result_t>;
  virtual remove_mapping_ret remove_mapping(
    Transaction &t,
    paddr_t offset) = 0;

  /**
   * scan all extents, including backref extents, logical extents and lba extents,
   * visit them with scan_mapped_space_func_t
   */
  using scan_mapped_space_iertr = base_iertr::extend_ertr<
    SegmentManager::read_ertr>;
  using scan_mapped_space_ret = scan_mapped_space_iertr::future<>;
  using scan_mapped_space_func_t = std::function<
    void(paddr_t, extent_len_t, depth_t, extent_types_t)>;
  virtual scan_mapped_space_ret scan_mapped_space(
    Transaction &t,
    scan_mapped_space_func_t &&f) = 0;

  virtual void complete_transaction(
    Transaction &t,
    std::vector<CachedExtentRef> &to_clear,	///< extents whose pins are to be cleared,
						//   as the results of their retirements
    std::vector<CachedExtentRef> &to_link	///< fresh extents whose pins are to be inserted
						//   into backref manager's pin set
  ) = 0;

  virtual void add_pin(BackrefPin &pin) = 0;
  virtual void remove_pin(BackrefPin &pin) = 0;

  virtual ~BackrefManager() {}
};

using BackrefManagerRef =
  std::unique_ptr<BackrefManager>;

BackrefManagerRef create_backref_manager(
  SegmentManagerGroup &sm_group,
  Cache &cache);

} // namespace crimson::os::seastore::backref
