// MiniVectorDB - page store: buffered pages, write ahead log, recovery.
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mvdb {

// Page size and the fixed page header. Row payloads live in the remainder of
// the page and are addressed through the slot directory.
constexpr uint32_t kPageSize = 65536;
constexpr uint32_t kPageHeaderSize = 64;

// Offsets inside a page header.
constexpr uint32_t kHdrMagic = 0;        // u32 'MDBV'
constexpr uint32_t kHdrPageType = 4;     // u16
constexpr uint32_t kHdrSlotCount = 6;    // u16
constexpr uint32_t kHdrHeapTop = 8;      // u32, lowest byte used by row data
constexpr uint32_t kHdrFreeEnd = 12;     // u32, end of the slot directory
constexpr uint32_t kHdrRowCount = 16;    // u32
constexpr uint32_t kHdrFreeList = 20;    // u32, head of the free hole list
constexpr uint32_t kHdrLsn = 24;         // u64
constexpr uint32_t kPageTypeHeap = 1;

// A slot packs the row length in the high 32 bits and the row offset in the
// low 32 bits. kSlotFree marks an unused slot.
constexpr uint64_t kSlotFree = 0xFFFFFFFFFFFFFFFFull;

inline uint32_t slot_offset(uint64_t slot) { return static_cast<uint32_t>(slot); }
inline uint32_t slot_length(uint64_t slot) { return static_cast<uint32_t>(slot >> 32); }
inline uint64_t make_slot(uint32_t offset, uint32_t length) {
  return (static_cast<uint64_t>(length) << 32) | offset;
}

// Range of bytes modified inside a page, used for redo logging.
struct ByteRange {
  uint32_t offset = 0;
  uint32_t length = 0;
  bool operator<(const ByteRange& other) const { return offset < other.offset; }
};

// The old content of a modified byte range, used to undo a change.
struct UndoEntry {
  uint32_t table_id = 0;
  uint32_t page_id = 0;
  uint32_t offset = 0;
  std::vector<uint8_t> old;
};

// A page together with the byte ranges one statement touched inside it.
struct PageRecord {
  uint64_t key = 0;
  std::vector<ByteRange> ranges;
};

class Storage {
 public:
  explicit Storage(std::string data_dir);
  ~Storage();

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  // Opens (or creates) the data directory, replays the write ahead log and
  // leaves the store consistent with the last checkpoint.
  void open();
  // Flushes everything and releases the file handles.
  void close();

  // Pins the page and returns a pointer to its raw bytes. The pointer stays
  // valid until release() is called for the same page.
  uint8_t* page(uint32_t table_id, uint32_t page_id);
  void release(uint32_t table_id, uint32_t page_id);

  // Writes bytes into a pinned page and records the change in the undo log.
  void write_bytes(uint32_t table_id, uint32_t page_id, uint32_t offset, uint32_t length,
                   const void* source);
  void frame_dirty(uint32_t table_id, uint32_t page_id);

  // Allocates a new page for a table and returns its identifier.
  uint32_t alloc_page(uint32_t table_id);

  // Statement and transaction boundaries.
  void begin_statement();
  void commit_statement();     // the statement succeeded
  void rollback_statement();   // the statement failed, undo just its changes
  void begin_transaction();
  void commit_transaction();
  void rollback_transaction();
  bool in_transaction() const { return txn_active_; }

  // Writes every dirty page to disk and truncates the write ahead log.
  void checkpoint();

  uint64_t page_count(uint32_t table_id);
  bool table_exists(uint32_t table_id) const;

 private:
  struct Frame {
    std::vector<uint8_t> data;
    bool dirty = false;
    uint32_t pin = 0;
    bool resident = false;
    uint32_t table_id = 0;
    uint32_t page_id = 0;
  };

  struct PageFile {
    int fd = -1;
    std::string path;
  };

  void load_manifest();
  void store_manifest();
  void replay_wal();
  void flush_dirty_pages();
  void evict_if_needed();
  Frame* find_frame(uint32_t table_id, uint32_t page_id);
  Frame* load_frame(uint32_t table_id, uint32_t page_id);
  void write_frame(Frame* frame);
  PageFile& file_for(uint32_t table_id);
  void append_wal(uint32_t type, const std::vector<uint8_t>& payload);
  void flush_wal();
  void write_records(const std::vector<PageRecord>& records);
  void apply_page_alloc(uint32_t table_id, uint32_t page_id, const uint8_t* header);
  void apply_page_modify(uint32_t table_id, uint32_t page_id,
                         const std::vector<ByteRange>& ranges, const uint8_t* bytes);
  void restore_undo(const std::vector<UndoEntry>& entries);
  void sync_directory();

  std::string data_dir_;
  std::unordered_map<uint32_t, PageFile> files_;
  // The next free page of every table. The page ids of a table are handed out
  // in order and never reused.
  std::unordered_map<uint32_t, uint32_t> next_page_id_;
  std::vector<std::unique_ptr<Frame>> frames_;
  std::unordered_map<uint64_t, uint32_t> frame_index_;
  std::list<uint64_t> lru_;
  std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_pos_;
  size_t max_frames_ = 512;
  size_t resident_frames_ = 0;

  int wal_fd_ = -1;
  uint64_t lsn_ = 1;
  uint64_t checkpoint_lsn_ = 0;

  bool txn_active_ = false;
  std::vector<UndoEntry> stmt_undo_;
  std::vector<UndoEntry> txn_undo_;
  std::vector<PageRecord> stmt_records_;
  std::vector<PageRecord> txn_records_;
  std::unordered_map<uint64_t, std::vector<ByteRange>> stmt_ranges_;
  std::unordered_map<uint64_t, bool> page_dirty_before_txn_;
  std::unordered_set<uint64_t> sticky_pages_;
  std::vector<uint8_t> wal_buffer_;
};

}  // namespace mvdb
