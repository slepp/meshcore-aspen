#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lfs.h>
#include <flash_cache.h>

#if LFS_VERSION != 0x00010007
#error "The note erase-cost check requires pinned Adafruit LittleFS v1.7"
#endif

enum { BLOCK_SIZE = 128, PAGES = 7, NOTE_BYTES = 3404 };
static unsigned char flash[PAGES * FLASH_CACHE_SIZE], cache_bytes[FLASH_CACHE_SIZE];
static unsigned erases[PAGES];
void* pvPortMalloc(size_t size) { return malloc(size); }
void vPortFree(void* pointer) { free(pointer); }

static bool erase_page(uint32_t address) {
  assert(address % FLASH_CACHE_SIZE == 0 && address < sizeof(flash));
  ++erases[address / FLASH_CACHE_SIZE];
  memset(flash + address, 0xff, FLASH_CACHE_SIZE);
  return true;
}
static uint32_t program_page(uint32_t address, const void* input, uint32_t length) {
  assert(address + length <= sizeof(flash));
  memcpy(flash + address, input, length);
  return length;
}
static uint32_t read_page(void* output, uint32_t address, uint32_t length) {
  assert(address + length <= sizeof(flash));
  memcpy(output, flash + address, length);
  return length;
}
static bool verify_page(uint32_t address, const void* input, uint32_t length) {
  return !memcmp(flash + address, input, length);
}
static flash_cache_t cache = {
  .erase = erase_page, .program = program_page, .read = read_page, .verify = verify_page,
  .cache_addr = FLASH_CACHE_INVALID_ADDR, .cache_buf = cache_bytes
};
static int read_block(const struct lfs_config* config, lfs_block_t block,
                      lfs_off_t offset, void* output, lfs_size_t size) {
  (void)config;
  return flash_cache_read(&cache, output, block * BLOCK_SIZE + offset, size) == (int)size ? 0 : -1;
}
static int write_block(const struct lfs_config* config, lfs_block_t block,
                       lfs_off_t offset, const void* input, lfs_size_t size) {
  (void)config;
  return flash_cache_write(&cache, block * BLOCK_SIZE + offset, input, size) == (int)size ? 0 : -1;
}
static int erase_block(const struct lfs_config* config, lfs_block_t block) {
  (void)config;
  const unsigned char erased = 0xff;
  for (unsigned i = 0; i < BLOCK_SIZE; ++i)
    assert(flash_cache_write(&cache, block * BLOCK_SIZE + i, &erased, 1) == 1);
  return 0;
}
static int sync_blocks(const struct lfs_config* config) {
  (void)config;
  flash_cache_flush(&cache);
  return 0;
}
// Same logical block geometry and physical cache as InternalFileSystem.cpp.
static const struct lfs_config config = {
  .read = read_block, .prog = write_block, .erase = erase_block, .sync = sync_blocks,
  .read_size = BLOCK_SIZE, .prog_size = BLOCK_SIZE, .block_size = BLOCK_SIZE,
  .block_count = sizeof(flash) / BLOCK_SIZE, .lookahead = 128
};

static void write_file(lfs_t* fs, const char* path, size_t size, unsigned char value) {
  unsigned char buffer[178];
  memset(buffer, value, sizeof(buffer));
  lfs_file_t file;
  assert(lfs_file_open(fs, &file, path, LFS_O_WRONLY | LFS_O_CREAT) == 0);
  for (size_t left = size; left;) {
    size_t bytes = left < sizeof(buffer) ? left : sizeof(buffer);
    assert(lfs_file_write(fs, &file, buffer, bytes) == (lfs_ssize_t)bytes);
    left -= bytes;
  }
  assert(lfs_file_close(fs, &file) == 0);
}

static void write_note_stage(lfs_t* fs, unsigned char value) {
  unsigned char record[NOTE_BYTES];
  memset(record, value, sizeof(record));
  lfs_file_t file;
  assert(lfs_file_open(fs, &file, "/pine-notes.new", LFS_O_WRONLY | LFS_O_CREAT) == 0);
  assert(lfs_file_write(fs, &file, record, 8) == 8);
  assert(lfs_file_write(fs, &file, record + 8, 4) == 4);
  for (unsigned i = 0; i < 16; ++i)
    assert(lfs_file_write(fs, &file, record + 12 + i * 178, 178) == 178);
  for (unsigned i = 0; i < 8; ++i)
    assert(lfs_file_write(fs, &file, record + 12 + 16 * 178 + i * 68, 68) == 68);
  assert(lfs_file_close(fs, &file) == 0);
}

int main(void) {
  memset(flash, 0xff, sizeof(flash));
  lfs_t fs;
  assert(lfs_format(&fs, &config) == 0 && lfs_mount(&fs, &config) == 0);
  write_file(&fs, "/_main.id", 132, 1);
  write_file(&fs, "/_nrfbot.id", 132, 2);
  write_file(&fs, "/prefs.json", 256, 3);
  write_file(&fs, "/pine-ble", 24, 4);
  write_file(&fs, "/pine-notes", NOTE_BYTES, 5);
  unsigned min_root = ~0u, max_root = 0, total_root = 0;
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    unsigned before = erases[0];
    int removed = lfs_remove(&fs, "/pine-notes.new");
    assert(removed == 0 || removed == LFS_ERR_NOENT);
    write_note_stage(&fs, attempt + 6);
    unsigned char readback[NOTE_BYTES];
    lfs_file_t file;
    assert(lfs_file_open(&fs, &file, "/pine-notes.new", LFS_O_RDONLY) == 0);
    assert(lfs_file_read(&fs, &file, readback, sizeof(readback)) == sizeof(readback));
    assert(lfs_file_close(&fs, &file) == 0);
    for (unsigned i = 0; i < sizeof(readback); ++i) assert(readback[i] == attempt + 6);
    assert(lfs_rename(&fs, "/pine-notes.new", "/pine-notes") == 0);
    unsigned cost = erases[0] - before;
    if (cost < min_root) min_root = cost;
    if (cost > max_root) max_root = cost;
    total_root += cost;
  }
  printf("note backend: pinned lfs1.7 + actual 4KiB flash cache; 64 atomic 3404B replacements; "
         "fixed root-page erases min=%u max=%u total=%u\n", min_root, max_root, total_root);
  for (unsigned page = 0; page < PAGES; ++page) printf("page %u erases=%u\n", page, erases[page]);
  assert(min_root >= 3 && max_root >= 5 && max_root <= 7);
  for (unsigned role = 1; role <= 2; ++role) {
    const char* path = role == 1 ? "/_main.id" : "/_nrfbot.id";
    lfs_file_t file;
    unsigned char identity[132];
    assert(lfs_file_open(&fs, &file, path, LFS_O_RDONLY) == 0);
    assert(lfs_file_read(&fs, &file, identity, sizeof(identity)) == sizeof(identity));
    assert(lfs_file_close(&fs, &file) == 0);
    for (unsigned i = 0; i < sizeof(identity); ++i) assert(identity[i] == role);
  }
  assert(lfs_unmount(&fs) == 0);
}
