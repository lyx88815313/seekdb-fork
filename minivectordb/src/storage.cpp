#include "storage.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "core.hpp"

namespace mvdb {

namespace {

constexpr uint32_t kWalMagic = 0x3152574Du;  // 'MWR1'
constexpr uint32_t kWalPageModify = 1;
constexpr uint32_t kWalPageAlloc = 2;
constexpr uint32_t kWalCheckpoint = 3;
constexpr uint32_t kManifestMagic = 0x31564D4Du;  // 'DMV1'
constexpr uint32_t kWalHeaderSize = 24;

uint64_t page_key(uint32_t table_id, uint32_t page_id) {
  return (static_cast<uint64_t>(table_id) << 32) | page_id;
}

void put_u16(uint8_t* p, uint16_t value) { std::memcpy(p, &value, 2); }
void put_u32(uint8_t* p, uint32_t value) { std::memcpy(p, &value, 4); }
void put_u64(uint8_t* p, uint64_t value) { std::memcpy(p, &value, 8); }
uint16_t get_u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t get_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t get_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// Merges byte ranges so that adjacent or overlapping modifications become a
// single redo record.
std::vector<ByteRange> merge_ranges(std::vector<ByteRange> ranges) {
  std::sort(ranges.begin(), ranges.end());
  std::vector<ByteRange> merged;
  for (const ByteRange& range : ranges) {
    if (!merged.empty()) {
      ByteRange& last = merged.back();
      uint32_t last_end = last.offset + last.length;
      if (range.offset <= last_end) {
        uint32_t end = std::max(last_end, range.offset + range.length);
        last.length = end - last.offset;
        continue;
      }
    }
    merged.push_back(range);
  }
  return merged;
}

}  // namespace

Storage::Storage(std::string data_dir) : data_dir_(std::move(data_dir)) {}

Storage::~Storage() { close(); }

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

Storage::PageFile& Storage::file_for(uint32_t table_id) {
  auto it = files_.find(table_id);
  if (it != files_.end()) return it->second;
  PageFile file;
  file.path = data_dir_ + "/t_" + std::to_string(table_id) + ".dat";
  file.fd = ::open(file.path.c_str(), O_RDWR | O_CREAT, 0644);
  if (file.fd < 0) {
    throw_error("io_error", "cannot open table file " + file.path + ": " + std::strerror(errno));
  }
  // The pages of a table are addressed from zero, the file size tells how many
  // of them exist. The next free page continues after the last one.
  const off_t size = ::lseek(file.fd, 0, SEEK_END);
  next_page_id_[table_id] = static_cast<uint32_t>(size / kPageSize);
  auto inserted = files_.emplace(table_id, std::move(file));
  return inserted.first->second;
}

bool Storage::table_exists(uint32_t table_id) const {
  std::error_code ec;
  return std::filesystem::exists(data_dir_ + "/t_" + std::to_string(table_id) + ".dat", ec);
}

void Storage::sync_directory() {
  int fd = ::open(data_dir_.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

void Storage::load_manifest() {
  std::string path = data_dir_ + "/manifest";
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  uint8_t buffer[24];
  ssize_t got = ::read(fd, buffer, sizeof(buffer));
  ::close(fd);
  if (got < static_cast<ssize_t>(24)) return;
  if (get_u32(buffer) != kManifestMagic) return;
  uint32_t stored_crc = get_u32(buffer + 20);
  if (crc32_of(buffer, 20) != stored_crc) return;
  checkpoint_lsn_ = get_u64(buffer + 8);
  lsn_ = std::max<uint64_t>(lsn_, checkpoint_lsn_ + 1);
}

void Storage::store_manifest() {
  uint8_t buffer[24];
  put_u32(buffer, kManifestMagic);
  put_u32(buffer + 4, 1);
  put_u64(buffer + 8, checkpoint_lsn_);
  put_u32(buffer + 20, crc32_of(buffer, 20));
  std::string temp = data_dir_ + "/manifest.tmp";
  int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) throw_error("io_error", "cannot write manifest");
  size_t written = 0;
  while (written < sizeof(buffer)) {
    ssize_t n = ::write(fd, buffer + written, sizeof(buffer) - written);
    if (n <= 0) break;
    written += static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  std::string path = data_dir_ + "/manifest";
  if (::rename(temp.c_str(), path.c_str()) != 0) {
    throw_error("io_error", "cannot replace manifest");
  }
  sync_directory();
}

// ---------------------------------------------------------------------------
// Write ahead log
// ---------------------------------------------------------------------------

void Storage::append_wal(uint32_t type, const std::vector<uint8_t>& payload) {
  uint8_t header[kWalHeaderSize];
  std::memset(header, 0, sizeof(header));
  put_u32(header, kWalMagic);
  put_u32(header + 4, type);
  put_u64(header + 8, lsn_);
  put_u32(header + 16, static_cast<uint32_t>(payload.size()));
  uint32_t crc = crc32_of(payload.data(), payload.size());
  wal_buffer_.insert(wal_buffer_.end(), header, header + kWalHeaderSize);
  wal_buffer_.insert(wal_buffer_.end(), payload.begin(), payload.end());
  uint8_t crc_bytes[4];
  put_u32(crc_bytes, crc);
  wal_buffer_.insert(wal_buffer_.end(), crc_bytes, crc_bytes + 4);
  ++lsn_;
}

void Storage::flush_wal() {
  if (wal_buffer_.empty()) return;
  size_t written = 0;
  while (written < wal_buffer_.size()) {
    ssize_t n = ::write(wal_fd_, wal_buffer_.data() + written, wal_buffer_.size() - written);
    if (n <= 0) {
      if (errno == EINTR) continue;
      throw_error("io_error", std::string("write ahead log write failed: ") + std::strerror(errno));
    }
    written += static_cast<size_t>(n);
  }
  wal_buffer_.clear();
  ::fdatasync(wal_fd_);
}

// Writes one redo record per touched page. The bytes are taken from the
// current page content, which already reflects every change up to this point.
void Storage::write_records(const std::vector<PageRecord>& records) {
  for (const PageRecord& record : records) {
    uint32_t table_id = static_cast<uint32_t>(record.key >> 32);
    uint32_t page_id = static_cast<uint32_t>(record.key & 0xFFFFFFFFu);
    Frame* frame = find_frame(table_id, page_id);
    if (frame == nullptr) continue;
    std::vector<ByteRange> ranges = merge_ranges(record.ranges);
    // The record starts with a fixed header: table id, page id and the number
    // of byte ranges, followed by the ranges themselves and their content.
    std::vector<uint8_t> payload(10 + ranges.size() * 8, 0);
    put_u32(payload.data(), table_id);
    put_u32(payload.data() + 4, page_id);
    put_u16(payload.data() + 8, static_cast<uint16_t>(ranges.size()));
    size_t cursor = 10;
    for (const ByteRange& range : ranges) {
      put_u32(payload.data() + cursor, range.offset);
      put_u32(payload.data() + cursor + 4, range.length);
      cursor += 8;
    }
    for (const ByteRange& range : ranges) {
      payload.insert(payload.end(), frame->data.begin() + range.offset,
                     frame->data.begin() + range.offset + range.length);
    }
    append_wal(kWalPageModify, payload);
  }
}

// ---------------------------------------------------------------------------
// Buffer pool
// ---------------------------------------------------------------------------

Storage::Frame* Storage::find_frame(uint32_t table_id, uint32_t page_id) {
  auto it = frame_index_.find(page_key(table_id, page_id));
  if (it == frame_index_.end()) return nullptr;
  return frames_[it->second].get();
}

Storage::Frame* Storage::load_frame(uint32_t table_id, uint32_t page_id) {
  uint64_t key = page_key(table_id, page_id);
  auto index_it = frame_index_.find(key);
  if (index_it != frame_index_.end()) {
    Frame* frame = frames_[index_it->second].get();
    ++frame->pin;
    lru_.erase(lru_pos_[key]);
    lru_.push_front(key);
    lru_pos_[key] = lru_.begin();
    return frame;
  }

  evict_if_needed();

  uint32_t slot = 0xFFFFFFFF;
  for (uint32_t i = 0; i < frames_.size(); ++i) {
    if (!frames_[i]->resident && frames_[i]->pin == 0) {
      slot = i;
      break;
    }
  }
  if (slot == 0xFFFFFFFF) {
    // No free frame: evict the least recently used unpinned page.
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
      auto fit = frame_index_.find(*it);
      if (fit == frame_index_.end()) continue;
      Frame* victim = frames_[fit->second].get();
      if (victim->pin != 0) continue;
      if (sticky_pages_.count(*it) != 0) continue;
      if (victim->dirty) write_frame(victim);
      victim->resident = false;
      victim->dirty = false;
      victim->data.clear();
      victim->data.shrink_to_fit();
      frame_index_.erase(*it);
      lru_pos_.erase(*it);
      lru_.erase(it);
      resident_frames_--;
      slot = fit->second;
      break;
    }
  }
  if (slot == 0xFFFFFFFF) {
    frames_.emplace_back(new Frame());
    slot = static_cast<uint32_t>(frames_.size() - 1);
  }

  Frame* frame = frames_[slot].get();
  frame->data.assign(kPageSize, 0);
  frame->resident = true;
  frame->dirty = false;
  frame->pin = 1;
  frame->table_id = table_id;
  frame->page_id = page_id;

  PageFile& file = file_for(table_id);
  ssize_t got = ::pread(file.fd, frame->data.data(), kPageSize,
                        static_cast<off_t>(page_id) * kPageSize);
  if (got < 0) {
    throw_error("io_error", "cannot read page");
  }
  if (got < static_cast<ssize_t>(kPageSize)) {
    std::memset(frame->data.data() + got, 0, kPageSize - static_cast<size_t>(got));
  }
  frame_index_[key] = slot;
  ++resident_frames_;
  lru_.push_front(key);
  lru_pos_[key] = lru_.begin();
  return frame;
}

void Storage::evict_if_needed() {
  while (resident_frames_ > max_frames_) {
    bool evicted = false;
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
      auto fit = frame_index_.find(*it);
      if (fit == frame_index_.end()) {
        lru_.erase(it);
        continue;
      }
      Frame* victim = frames_[fit->second].get();
      if (victim->pin != 0) continue;
      if (sticky_pages_.count(*it) != 0) continue;
      if (victim->dirty) write_frame(victim);
      victim->resident = false;
      victim->dirty = false;
      victim->data.clear();
      victim->data.shrink_to_fit();
      frame_index_.erase(*it);
      lru_pos_.erase(*it);
      lru_.erase(it);
      --resident_frames_;
      evicted = true;
      break;
    }
    if (!evicted) break;
  }
}

void Storage::write_frame(Frame* frame) {
  PageFile& file = file_for(frame->table_id);
  size_t written = 0;
  while (written < kPageSize) {
    ssize_t n = ::pwrite(file.fd, frame->data.data() + written, kPageSize - written,
                         static_cast<off_t>(frame->page_id) * kPageSize +
                             static_cast<off_t>(written));
    if (n <= 0) {
      if (errno == EINTR) continue;
      throw_error("io_error", "cannot write page");
    }
    written += static_cast<size_t>(n);
  }
  frame->dirty = false;
}

uint8_t* Storage::page(uint32_t table_id, uint32_t page_id) {
  Frame* frame = load_frame(table_id, page_id);
  return frame->data.data();
}

void Storage::release(uint32_t table_id, uint32_t page_id) {
  Frame* frame = find_frame(table_id, page_id);
  if (frame == nullptr || frame->pin == 0) return;
  --frame->pin;
}

void Storage::write_bytes(uint32_t table_id, uint32_t page_id, uint32_t offset,
                          uint32_t length, const void* source) {
  Frame* frame = find_frame(table_id, page_id);
  if (frame == nullptr) throw_error("internal_error", "write to an unpinned page");
  uint64_t key = page_key(table_id, page_id);

  UndoEntry entry;
  entry.table_id = table_id;
  entry.page_id = page_id;
  entry.offset = offset;
  entry.old.assign(frame->data.begin() + offset, frame->data.begin() + offset + length);
  stmt_undo_.push_back(entry);
  if (txn_active_) txn_undo_.push_back(std::move(entry));

  stmt_ranges_[key].push_back(ByteRange{offset, length});
  sticky_pages_.insert(key);

  std::memcpy(frame->data.data() + offset, source, length);
  frame->dirty = true;
}

uint32_t Storage::alloc_page(uint32_t table_id) {
  PageFile& file = file_for(table_id);
  // The next free page id is tracked in memory. It cannot be derived from the
  // file size, because the file only grows when a page is written back: every
  // allocation would hand out the same page and silently overwrite it.
  const uint32_t page_id = next_page_id_[table_id];
  ++next_page_id_[table_id];
  // The page has to exist in the file, the file size is what tells the
  // database how many pages a table owns.
  if (::ftruncate(file.fd, static_cast<off_t>(page_id + 1) * kPageSize) != 0) {
    throw_error("io_error", "cannot extend the table file");
  }

  uint8_t* bytes = page(table_id, page_id);
  put_u32(bytes + kHdrMagic, 0x4244564Du);  // 'MDBV'
  put_u16(bytes + kHdrPageType, static_cast<uint16_t>(kPageTypeHeap));
  put_u16(bytes + kHdrSlotCount, 0);
  put_u32(bytes + kHdrHeapTop, kPageSize);
  put_u32(bytes + kHdrFreeEnd, kPageHeaderSize);
  put_u32(bytes + kHdrRowCount, 0);
  put_u32(bytes + kHdrFreeList, 0xFFFFFFFFu);
  put_u64(bytes + kHdrLsn, lsn_);
  frame_dirty(table_id, page_id);

  uint64_t key = page_key(table_id, page_id);
  UndoEntry entry;
  entry.table_id = table_id;
  entry.page_id = page_id;
  entry.offset = 0;
  entry.old.assign(kPageSize, 0);
  stmt_undo_.push_back(entry);
  if (txn_active_) txn_undo_.push_back(std::move(entry));
  stmt_ranges_[key].push_back(ByteRange{0, kPageHeaderSize});
  sticky_pages_.insert(key);

  std::vector<uint8_t> payload;
  payload.resize(8 + kPageHeaderSize);
  put_u32(payload.data(), table_id);
  put_u32(payload.data() + 4, page_id);
  std::memcpy(payload.data() + 8, bytes, kPageHeaderSize);
  append_wal(kWalPageAlloc, payload);

  release(table_id, page_id);
  return page_id;
}

void Storage::frame_dirty(uint32_t table_id, uint32_t page_id) {
  Frame* frame = find_frame(table_id, page_id);
  if (frame != nullptr) frame->dirty = true;
}

uint64_t Storage::page_count(uint32_t table_id) {
  auto it = files_.find(table_id);
  if (it == files_.end()) {
    // The file can be present on disk without being open in this process,
    // for example right after a restart. Open it before the size is asked
    // for, otherwise the caller would see an empty table.
    std::error_code ec;
    if (!std::filesystem::exists(data_dir_ + "/t_" + std::to_string(table_id) + ".dat", ec)) {
      return 0;
    }
    file_for(table_id);
    it = files_.find(table_id);
    if (it == files_.end()) return 0;
  }
  return static_cast<uint64_t>(::lseek(it->second.fd, 0, SEEK_END)) / kPageSize;
}

// ---------------------------------------------------------------------------
// Statement and transaction boundaries
// ---------------------------------------------------------------------------

void Storage::begin_statement() {
  stmt_undo_.clear();
  stmt_ranges_.clear();
  stmt_records_.clear();
}

void Storage::commit_statement() {
  std::vector<PageRecord> records;
  for (auto& entry : stmt_ranges_) {
    PageRecord record;
    record.key = entry.first;
    record.ranges = merge_ranges(entry.second);
    records.push_back(std::move(record));
  }
  if (txn_active_) {
    for (PageRecord& record : records) txn_records_.push_back(std::move(record));
  } else {
    write_records(records);
    flush_wal();
  }
  stmt_undo_.clear();
  stmt_ranges_.clear();
  stmt_records_.clear();
}

void Storage::restore_undo(const std::vector<UndoEntry>& entries) {
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    const UndoEntry& entry = *it;
    uint64_t key = page_key(entry.table_id, entry.page_id);
    Frame* frame = find_frame(entry.table_id, entry.page_id);
    if (frame == nullptr) {
      // The page is not resident; load it before restoring.
      frame = load_frame(entry.table_id, entry.page_id);
    }
    std::memcpy(frame->data.data() + entry.offset, entry.old.data(), entry.old.size());
    frame->dirty = true;
    sticky_pages_.insert(key);
  }
}

void Storage::rollback_statement() {
  restore_undo(stmt_undo_);
  stmt_undo_.clear();
  stmt_ranges_.clear();
  stmt_records_.clear();
}

void Storage::begin_transaction() {
  if (txn_active_) return;
  txn_active_ = true;
  txn_undo_.clear();
  txn_records_.clear();
}

void Storage::commit_transaction() {
  if (!txn_active_) return;
  write_records(txn_records_);
  flush_wal();
  txn_active_ = false;
  txn_undo_.clear();
  txn_records_.clear();
  page_dirty_before_txn_.clear();
}

void Storage::rollback_transaction() {
  if (!txn_active_) return;
  restore_undo(txn_undo_);
  txn_active_ = false;
  txn_undo_.clear();
  txn_records_.clear();
  page_dirty_before_txn_.clear();
  wal_buffer_.clear();
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

void Storage::apply_page_alloc(uint32_t table_id, uint32_t page_id, const uint8_t* header) {
  PageFile& file = file_for(table_id);
  // The page has to exist in full: the file size is what tells the database
  // how many pages a table owns.
  off_t end = static_cast<off_t>(page_id + 1) * kPageSize;
  if (::ftruncate(file.fd, end) != 0) {
    throw_error("io_error", "cannot extend the table file during recovery");
  }
  uint32_t& next = next_page_id_[table_id];
  if (next <= page_id) next = page_id + 1;
  off_t offset = static_cast<off_t>(page_id) * kPageSize;
  ssize_t written = ::pwrite(file.fd, header, kPageHeaderSize, offset);
  if (written != static_cast<ssize_t>(kPageHeaderSize)) {
    throw_error("io_error", "cannot restore allocated page");
  }
}

void Storage::apply_page_modify(uint32_t table_id, uint32_t page_id,
                                const std::vector<ByteRange>& ranges, const uint8_t* bytes) {
  PageFile& file = file_for(table_id);
  off_t base = static_cast<off_t>(page_id) * kPageSize;
  const uint8_t* cursor = bytes;
  for (const ByteRange& range : ranges) {
    size_t written = 0;
    while (written < range.length) {
      ssize_t n = ::pwrite(file.fd, cursor + written, range.length - written,
                           base + static_cast<off_t>(range.offset) +
                               static_cast<off_t>(written));
      if (n <= 0) {
        if (errno == EINTR) continue;
        throw_error("io_error", "cannot apply write ahead log record");
      }
      written += static_cast<size_t>(n);
    }
    cursor += range.length;
  }
}

void Storage::replay_wal() {
  if (wal_fd_ < 0) return;
  off_t offset = 0;
  off_t good_offset = 0;
  while (true) {
    uint8_t header[kWalHeaderSize];
    ssize_t got = ::pread(wal_fd_, header, kWalHeaderSize, offset);
    if (got != static_cast<ssize_t>(kWalHeaderSize)) break;
    if (get_u32(header) != kWalMagic) break;
    uint32_t type = get_u32(header + 4);
    uint64_t lsn = get_u64(header + 8);
    uint32_t length = get_u32(header + 16);
    std::vector<uint8_t> payload(length);
    if (length > 0) {
      ssize_t payload_got = ::pread(wal_fd_, payload.data(), length, offset + kWalHeaderSize);
      if (payload_got != static_cast<ssize_t>(length)) break;
    }
    uint32_t stored_crc = 0;
    ssize_t crc_got = ::pread(wal_fd_, &stored_crc, 4, offset + kWalHeaderSize + length);
    if (crc_got != 4) break;
    if (crc32_of(payload.data(), payload.size()) != stored_crc) break;

    if (lsn > checkpoint_lsn_) {
      if (type == kWalPageAlloc && payload.size() >= 8 + kPageHeaderSize) {
        uint32_t table_id = get_u32(payload.data());
        uint32_t page_id = get_u32(payload.data() + 4);
        apply_page_alloc(table_id, page_id, payload.data() + 8);
      } else if (type == kWalPageModify && payload.size() >= 10) {
        uint32_t table_id = get_u32(payload.data());
        uint32_t page_id = get_u32(payload.data() + 4);
        uint16_t count = get_u16(payload.data() + 8);
        size_t cursor = 10 + static_cast<size_t>(count) * 8;
        if (cursor > payload.size()) break;
        std::vector<ByteRange> ranges;
        ranges.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
          uint32_t range_offset = get_u32(payload.data() + 10 + static_cast<size_t>(i) * 8);
          uint32_t range_length = get_u32(payload.data() + 10 + static_cast<size_t>(i) * 8 + 4);
          ranges.push_back(ByteRange{range_offset, range_length});
        }
        apply_page_modify(table_id, page_id, ranges, payload.data() + cursor);
      }
    }
    offset += kWalHeaderSize + length + 4;
    good_offset = offset;
    if (lsn >= lsn_) lsn_ = lsn + 1;
  }
  // Drop a partially written tail so that later appends stay parsable.
  if (good_offset != offset) {
    if (::ftruncate(wal_fd_, good_offset) != 0) { /* the tail is ignored anyway */ }
  }
}

void Storage::flush_dirty_pages() {
  for (auto& frame : frames_) {
    if (frame->resident && frame->dirty) write_frame(frame.get());
  }
  for (auto& entry : files_) {
    ::fdatasync(entry.second.fd);
  }
}

void Storage::checkpoint() {
  if (txn_active_) return;
  flush_dirty_pages();
  std::vector<uint8_t> payload(8);
  put_u64(payload.data(), lsn_);
  append_wal(kWalCheckpoint, payload);
  flush_wal();
  checkpoint_lsn_ = lsn_ - 1;
  store_manifest();
  if (::ftruncate(wal_fd_, 0) != 0) { /* the log restarts empty */ }
  ::lseek(wal_fd_, 0, SEEK_SET);
  wal_buffer_.clear();
}

void Storage::open() {
  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);
  load_manifest();

  std::string wal_path = data_dir_ + "/wal.log";
  wal_fd_ = ::open(wal_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (wal_fd_ < 0) throw_error("io_error", "cannot open the write ahead log");
  ::lseek(wal_fd_, 0, SEEK_END);

  replay_wal();

  const char* pool_env = std::getenv("VECTORDB_BUFFER_POOL_MB");
  if (pool_env != nullptr) {
    long megabytes = std::strtol(pool_env, nullptr, 10);
    if (megabytes > 0) {
      max_frames_ = static_cast<size_t>(megabytes) * 1024 * 1024 / kPageSize;
      if (max_frames_ < 16) max_frames_ = 16;
    }
  }
  frames_.reserve(max_frames_);
}

void Storage::close() {
  if (wal_fd_ >= 0) {
    try {
      checkpoint();
    } catch (...) {
      // Closing must not throw; the data is already in the write ahead log.
    }
    ::close(wal_fd_);
    wal_fd_ = -1;
  }
  for (auto& entry : files_) {
    if (entry.second.fd >= 0) ::close(entry.second.fd);
  }
  files_.clear();
}

}  // namespace mvdb
