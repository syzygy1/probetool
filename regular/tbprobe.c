/*
  Copyright (c) 2013-2018, 2025, 2026 Ronald de Man
  This file may be redistributed and/or modified without restrictions.
*/

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#endif
#include <x86intrin.h>

#include "tbprobe.h"
#include "tbinterface.h"

#define INLINE static inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))

#undef min
#undef max
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

typedef uint64_t Bitboard;

INLINE int popcnt(Bitboard b)
{
  return __builtin_popcountll(b);
}

INLINE Bitboard bit(int sq)
{
  return 1ULL << sq;
}


/* Some platform-specific code for dealing with locks and files */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE map_t;
typedef HANDLE FD;
#define FD_ERR INVALID_HANDLE_VALUE
#define SEP_STR ";"

#define LOCK_T HANDLE
#define LOCK_INIT(x) do { x = CreateMutex(NULL, FALSE, NULL); } while (0)
#define LOCK_DESTROY(x) CloseHandle(x)
#define LOCK(x) WaitForSingleObject(x, INFINITE)
#define UNLOCK(x) ReleaseMutex(x)

#else

typedef size_t map_t;
typedef int FD;
#define FD_ERR -1
#define SEP_STR ":"

#define LOCK_T pthread_mutex_t
#define LOCK_INIT(x) pthread_mutex_init(&(x), NULL)
#define LOCK_DESTROY(x) pthread_mutex_destroy(&(x))
#define LOCK(x) pthread_mutex_lock(&(x))
#define UNLOCK(x) pthread_mutex_unlock(&(x))

#endif

static FD open_file(const char *name)
{
#ifndef _WIN32
  return open(name, O_RDONLY);

#else
  return CreateFile(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
      FILE_FLAG_RANDOM_ACCESS, NULL);

#endif
}

static void close_file(FD fd)
{
#ifndef _WIN32
  close(fd);

#else
  CloseHandle(fd);

#endif
}

static size_t file_size(FD fd)
{
#ifndef _WIN32
  struct stat statbuf;
  fstat(fd, &statbuf);
  return statbuf.st_size;

#else
  DWORD sizeLow, sizeHigh;
  sizeLow = GetFileSize(fd, &sizeHigh);
  return ((uint64_t)sizeHigh << 32) | sizeLow;

#endif
}

static const void *map_file(FD fd, map_t *map)
{
#ifndef _WIN32
  *map = file_size(fd);
  void *data = mmap(NULL, *map, PROT_READ, MAP_SHARED, fd, 0);
#ifdef MADV_RANDOM
  madvise(data, *map, MADV_RANDOM);
#endif
  return data == MAP_FAILED ? NULL : data;

#else
  DWORD sizeLow, sizeHigh;
  sizeLow = GetFileSize(fd, &sizeHigh);
  *map = CreateFileMapping(fd, NULL, PAGE_READONLY, sizeHigh, sizeLow, NULL);
  if (*map == NULL)
    return NULL;
  return MapViewOfFile(*map, FILE_MAP_READ, 0, 0, 0);

#endif
}

static void unmap_file(const void *data, map_t map)
{
  if (!data) return;

#ifndef _WIN32
  munmap((void *)data, map);

#else
  UnmapViewOfFile(data);
  CloseHandle(map);

#endif
}


/* Functions for dealing with endianness */

INLINE bool is_little_endian(void)
{
  int num = 1;
  return *(uint8_t *)&num == 1;
}

INLINE uint64_t from_le_u64(uint64_t v)
{
  return is_little_endian() ? v : __builtin_bswap64(v);
}

INLINE uint32_t from_le_u32(uint32_t v)
{
  return is_little_endian() ? v : __builtin_bswap32(v);
}

INLINE uint16_t from_le_u16(uint16_t v)
{
  return is_little_endian() ? v : __builtin_bswap16(v);
}

INLINE uint64_t from_be_u64(uint64_t v)
{
  return is_little_endian() ? __builtin_bswap64(v) : v;
}

INLINE uint32_t from_be_u32(uint32_t v)
{
  return is_little_endian() ? __builtin_bswap32(v) : v;
}

INLINE uint64_t read_be_u64(const void *p)
{
  uint64_t  v;
  memcpy(&v, p, sizeof v);
  return from_be_u64(v);
}

INLINE uint64_t read_le_u64(const void *p)
{
  uint64_t  v;
  memcpy(&v, p, sizeof v);
  return from_le_u64(v);
}

INLINE uint32_t read_le_u32(const void *p)
{
  uint32_t  v;
  memcpy(&v, p, sizeof v);
  return from_le_u32(v);
}

INLINE uint16_t read_le_u16(const void *p)
{
  uint16_t  v;
  memcpy(&v, p, sizeof v);
  return from_le_u16(v);
}


/* rANS decompression code */

struct RansEntry {
  uint16_t alias_div;
  uint16_t start;
};

struct RansDecode {
  struct RansEntry entry[4096];
  uint16_t freq[4096];
};

INLINE void rans_dec_init(uint64_t *r, const uint8_t **p,
    const uint8_t *const end)
{
  while (read_le_u32(*p) == 0 && *p + 8 < end)
    *p += 4;
  *r = read_le_u64(*p);
  *p += 8;
}

INLINE uint16_t rans_dec_get(uint64_t *r, struct RansDecode *d)
{
  uint64_t x = *r;

  // Figure out symbol via alias table.
  int s = (x >> 4) & 4095; // bucket id
  uint16_t remainder = x & 15;
  if (remainder >= (d->entry[s].alias_div & 15)) {
    // switch to alias symbol
    remainder += d->entry[s].start; // can wrap around by design
    s = d->entry[s].alias_div >> 4;
  }

  // Update state.
  uint16_t freq = d->freq[s];
  *r = freq * (x >> 16) + remainder;

  return s;
}

INLINE void rans_dec_renorm(uint64_t *r, const uint8_t **p)
{
  uint64_t x = *r;
  // Branchless renormalization is a win.
  int64_t s = x < (1ULL << 31);
  uint64_t tx = x;
  tx = (tx << 32) | (uint64_t)read_le_u32(*p);
  *p += s << 2;
  x = (-s & (tx - x)) + x;
  *r = x;
}

// Based on https://github.com/rygorous/ryg_rans but with a more compact
// alias data structure.
static void make_alias_table(struct RansDecode *d)
{
  uint32_t sum = 1u << 16;
  uint16_t tgt_sum = sum / 4096;

  uint16_t remaining[4096];
  uint16_t alias[4096];
  uint8_t divider[4096];
  for (int i = 0; i < 4096; i++) {
    remaining[i] = d->freq[i];
    divider[i] = tgt_sum;
    alias[i] = i;
  }

  int cur_large = 0, cur_small = 0;
  for (;;) {
    while (cur_large < 4096 && remaining[cur_large] <= tgt_sum)
      cur_large++;
    if (cur_large == 4096) break;
    while (remaining[cur_small] >= tgt_sum)
      cur_small = (cur_small + 1) & 4095;
    alias[cur_small] = cur_large;
    divider[cur_small] = remaining[cur_small];
    remaining[cur_large] -= tgt_sum - remaining[cur_small];
    remaining[cur_small] = tgt_sum;
  }

  uint16_t assigned[4096];
  for (int b = 0; b < 4096; b++) {
    assigned[b] = divider[b];
    d->entry[b].alias_div = (alias[b] << 4) | (divider[b] & 15);
  }
  for (int b = 0; b < 4096; b++) {
    int s = alias[b];
    d->entry[b].start = s != b ? assigned[s] - divider[b] : 0;
    assigned[s] += tgt_sum - divider[b];
  }

  // sanity check
  for (int i = 0; i < 4096; i++)
    assert(assigned[i] == d->freq[i]);
}

// Decode value v in the range 0 <= v < range
INLINE uint32_t rans_dec_val_uni(uint64_t *r, uint32_t v_min, uint32_t v_max)
{
  if (v_min == v_max)
    return v_min;
  uint32_t range = v_max - v_min + 1;

  uint64_t x = *r;
  uint32_t w = x & 0xffff;

  uint32_t v = (w * range + range - 1) >> 16;
  uint32_t cum_freq = (v << 16) / range;
  uint32_t freq = ((v + 1) << 16) / range - cum_freq;
  assert(cum_freq <= w && w < cum_freq + freq);
  *r = freq * (x >> 16) + (w - cum_freq);

  return v + v_min;
}

INLINE uint16_t rans_dec_val_top(uint64_t *r, uint32_t v_min, uint32_t v_max,
    uint32_t f)
{
  if (v_min == v_max)
    return v_min;

  uint64_t x = *r;

  uint32_t w = x & 0xffff;
  uint32_t v = v_min + min(w / f, v_max - v_min);
  uint32_t cum_freq = (v - v_min) * f;
  uint32_t freq = v == v_max ? 65536 - cum_freq : f;
  assert(cum_freq <= w && w < cum_freq + freq);
  *r = freq * (x >> 16) + (w - cum_freq);

  return v;
}

// Decompress a table of up to 4095 symbol frequencies.
static const uint8_t *read_freq_table(struct RansDecode *d, int *num_syms,
    const uint8_t *p)
{
  uint64_t rans;

  rans_dec_init(&rans, &p, p + sizeof(uint64_t));
  int num = rans_dec_val_uni(&rans, 2, 4095);
  *num_syms = num;
  rans_dec_renorm(&rans, &p);
  d->freq[0] = rans_dec_val_uni(&rans, 0,
      (1 << 16) - ((1 << 16) + num - 1) / num);
  for (int s = 1; s < num; s++) {
    rans_dec_renorm(&rans, &p);
    d->freq[s] = rans_dec_val_top(&rans, 1, d->freq[s - 1], 1);
  }
  int R = 1 << 16;
  for (int s = 0; s < num; s++) {
    int P = s > 0 ? d->freq[s - 1] : INT32_MAX;
    if (d->freq[s] > d->freq[s + 1] && s < num - 1) {
      d->freq[s] = min(R, P) - d->freq[s];
      R -= d->freq[s];
      continue;
    }
    rans_dec_renorm(&rans, &p);
    int v_max = min(R, P);
    int v_min = max((R + num - s - 1) / (num - s), v_max - d->freq[s]);
    if (v_max * (v_max - v_min + 1) >= (num - s))
      d->freq[s] = rans_dec_val_uni(&rans, v_min, v_max);
    else {
      d->freq[s] = rans_dec_val_top(&rans, v_min, v_max,
          (v_max << 16) / (num - s));
    }
    R -= d->freq[s];
  }

  return p;
}


/* TB initialization and probing code */

enum {
  TB_PIECES = 8,
  TB_SETS = 6,
  TB_HASHBITS = 13,
  TB_MAX_TABLES = 4031
};

enum { FAIL = 0, OK = 1, CHANGE_STM = -1, ZEROING_IS_BEST = 2 };

enum { WDL = TB_WDL, DTM = TB_DTM, DTZ = TB_DTZ };
//enum { WL_BOTH = 0, WL_WTM, WL_BTM, W_ONLY, L_ONLY };
enum {
  LT_PIECE = 0, LT_PAWN_FILE, LT_PAWN_RANK,
  LT_PIECE_K, LT_PIECE_KK,
  LT_PAWN_P, LT_PAWN_PK, LT_PAWN_PP, LT_PAWN_PvP
};

enum {
  TWO_SIDED = 0x01,
  WTM_OR_BTM = 0x02,
  WIN_OR_LOSS = 0x04,
  WIN_ONLY = 0x08,
  WTM_ONLY = 0x10
};

int TB_NumTables[3];
int TB_MaxCardinality[3];
uint64_t TB_ProbeCount[3];

static const char *suffix[] = { ".rtbw", ".rtbm", ".rtbz" };
static uint32_t magic[] = { 0x5d23e871, 0x88ac504b, 0xa50c66d7 };
static uint32_t magic2[] = { 0xe5c0db4d, 0x97ad8bad, 0x432c57d6 };

enum { STARTBITS = 8 };

struct PairsData {
  const uint8_t *indexTable;
  const uint16_t *sizeTable;
  const uint8_t *data;
  uint8_t *symLen;
  const uint8_t *symPat;
  uint8_t blockSize;
  uint8_t idxBits;
  uint8_t comprType;
  union {
    // Huffman
    struct {
      const uint16_t *offset;
      uint8_t minLen;
      uint8_t start[1 << STARTBITS];
    };
    // rANS
    struct RansDecode *rans;
    // constant
    uint8_t constValue[2];
  };
  // for Huffman
  uint64_t base[]; // must be base[1] in C++
};

// TODO: Instead of including indexing info in each TbTable2 struct,
// use an identifier that identifies a precomputed index struct.
// Perhaps also for TbTable structs.

struct EncInfo {
  size_t factor[TB_PIECES];
  uint8_t pieces[TB_PIECES];
  uint8_t norm[TB_PIECES];
};

struct TbTable {
  struct PairsData *precomp;
  struct EncInfo ei;
};

struct WdlTable {
  struct PairsData *precomp;
  struct EncInfo ei;
};

struct DtmTable {
  struct PairsData *precomp;
  struct EncInfo ei;
  const uint16_t *dtmMap;
  uint16_t dtmMapIdx[2];
  uint8_t dtmFlags;
};

struct DtzTable {
  struct PairsData *precomp;
  struct EncInfo ei;
  union {
    const uint8_t *dtzMap;
    const uint16_t *dtzMap16;
  };
  uint16_t dtzMapIdx[4];
  uint8_t dtzFlags;
};

struct TbTable2 {
  struct PairsData *precomp;
  uint32_t factor[TB_SETS];
  uint8_t first[TB_SETS];
  uint8_t mult[TB_SETS];
  uint8_t part_id;
};

struct WdlTable2 {
  struct PairsData *precomp;
  uint32_t factor[TB_SETS];
  uint8_t first[TB_SETS];
  uint8_t mult[TB_SETS];
  uint8_t part_id;
};

struct DtmTable2 {
  struct PairsData *precomp;
  uint32_t factor[TB_SETS];
  uint8_t first[TB_SETS];
  uint8_t mult[TB_SETS];
  uint8_t part_id;
  uint8_t mapped;
  uint8_t distFormat;
  uint16_t dtmMapIdx[2];
  const uint16_t *dtmMap;
};

struct DtzTable2 {
  struct PairsData *precomp;
  uint32_t factor[TB_SETS];
  uint8_t first[TB_SETS];
  uint8_t mult[TB_SETS];
  uint8_t part_id;
  uint8_t mapped;
  uint8_t distFormat;
  uint16_t dtzMapIdx[4];
  union {
    const uint8_t *dtzMap;
    const uint16_t *dtzMap16;
  };
};

static const size_t TABLE_SIZE[3] = {
  sizeof(struct WdlTable), sizeof(struct DtmTable), sizeof(struct DtzTable)
};

static const size_t TABLE_SIZE2[3] = {
  sizeof(struct WdlTable2), sizeof(struct DtmTable2), sizeof(struct DtzTable2)
};

struct TbTableConst {
  struct PairsData *precomp;
  uint16_t constVal;
  uint8_t distFormat;
};

static const struct TbTableConst constTable[5] = {
  { NULL, 0, 0 }, { NULL, 1, 0 }, { NULL, 2, 0 }, { NULL, 3, 0 }, { NULL, 4, 0 }
};

// For an old-format file, we allocate and initialize all 1/2/4/6/8/12
// TbTable structs at once.
//
// For a new-format file, we allocate and initialize the TbTable structs
// on demand.

struct Tbase {
  const void *data;
  map_t mapping;
  uint8_t pt[TB_PIECES];
  uint8_t layout;
  uint8_t distFormat;
  uint8_t version;
  bool flipped;
  uint8_t offset;
  void *_Atomic table[];
};

// Per piece combination a struct that keeps track of which TB files
// have been found and initialized, wdl/dtm/dtz.

struct TbEntry {
  uint64_t key;
  bool hasDtm, hasDtz, hasPawns, symmetric; // pack in one byte?
  union {
    bool kk_enc;
    uint8_t pawns[2];
  };
  uint8_t num, numsets;
  struct Tbase *_Atomic tbase[3];
};

struct HashEntry {
  uint64_t key;
  struct TbEntry *ptr;
};

static LOCK_T mutex;
static int initialized = 0;
static int numPaths = 0;
static char *pathString = NULL;
static char **paths = NULL;

static int numTBs;

static struct TbEntry *tbEntry;
static struct HashEntry tbHash[1 << TB_HASHBITS];

static void init_indices(void);

static char PieceToChar[] = {
  0, 'P', 'N', 'B', 'R', 'Q', 'K', 0
};

static uint32_t Binomial[8][64];

static FD open_tb(const char *str, const char *suffixStr)
{
  char name[256];

  for (int i = 0; i < numPaths; i++) {
    strcpy(name, paths[i]);
    strcat(name, "/");
    strcat(name, str);
    strcat(name, suffixStr);
    FD fd = open_file(name);
    if (fd != FD_ERR) return fd;
  }
  return FD_ERR;
}

static bool test_tb(const char *str, const char *suffix)
{
  FD fd = open_tb(str, suffix);
  if (fd != FD_ERR) {
    size_t size = file_size(fd);
    close_file(fd);
    if ((size & 63) != 16) {
      fprintf(stderr, "Incomplete tablebase file %s%s\n", str, suffix);
      fd = FD_ERR;
    }
  }
  return fd != FD_ERR;
}

static const void *map_tb(const char *name, const char *suffix, map_t *mapping)
{
  FD fd = open_tb(name, suffix);
  if (fd == FD_ERR)
    return NULL;

  const void *data = map_file(fd, mapping);
  if (data == NULL) {
    fprintf(stderr, "Could not map %s%s into memory.\n", name, suffix);
    exit(EXIT_FAILURE);
  }

  close_file(fd);

  return data;
}

static void add_to_hash(void *ptr, uint64_t key)
{
  int idx = key >> (64 - TB_HASHBITS);
  while (tbHash[idx].ptr)
    idx = (idx + 1) & ((1 << TB_HASHBITS) - 1);

  tbHash[idx].key = key;
  tbHash[idx].ptr = ptr;
}

#define Swap(a,b) {int tmp=a;a=b;b=tmp;}

static void detect_tb(char *str)
{
  if (!test_tb(str, suffix[WDL]))
    return;

  int pcs[16];
  for (int i = 0; i < 16; i++)
    pcs[i] = 0;
  int color = 0;
  for (char *s = str; *s; s++)
    if (*s == 'v')
      color = 8;
    else
      for (int i = 1; i <= 6; i++)
        if (*s == PieceToChar[i]) {
          pcs[i | color]++;
          break;
        }

  uint64_t key  = TB_material_key_from_counts(pcs, pcs + 8);
  uint64_t key2 = TB_material_key_from_counts(pcs + 8, pcs);

  struct TbEntry *entry = &tbEntry[numTBs++];
  *entry = (struct TbEntry){ 0 };
  entry->hasPawns = pcs[1] || pcs[9];
  entry->key = key;
  entry->symmetric = key == key2;
  for (int i = 0; i < 16; i++)
    entry->num += pcs[i];
  for (int i = 2; i < 6; i++)
    entry->numsets += (pcs[i] != 0) + (pcs[i + 8] != 0);

  TB_NumTables[WDL]++;
  TB_NumTables[DTM] += entry->hasDtm = test_tb(str, suffix[DTM]);
  TB_NumTables[DTZ] += entry->hasDtz = test_tb(str, suffix[DTZ]);

  TB_MaxCardinality[WDL] = max(TB_MaxCardinality[WDL], entry->num);
  if (entry->hasDtm)
    TB_MaxCardinality[DTM] = max(TB_MaxCardinality[DTM], entry->num);
  if (entry->hasDtz)
    TB_MaxCardinality[DTZ] = max(TB_MaxCardinality[DTZ], entry->num);

  for (int type = 0; type < 3; type++)
    atomic_init(&entry->tbase[type], NULL);

  if (!entry->hasPawns) {
    int j = 0;
    for (int i = 0; i < 16; i++)
      if (pcs[i] == 1) j++;
    entry->kk_enc = j == 2;
  } else {
    entry->pawns[0] = pcs[1]; // number of white pawns
    entry->pawns[1] = pcs[9]; // number of black pawns
    if (pcs[9] && (!pcs[1] || pcs[1] > pcs[9]))
      Swap(entry->pawns[0], entry->pawns[1]);
  }

  add_to_hash(entry, key);
  if (key != key2)
    add_to_hash(entry, key2);
}

INLINE int num_tables(struct TbEntry *entry, const int type)
{
  return entry->hasPawns ? type == DTM ? 6 : 4 : 1;
}

static void free_tb_table(struct TbTable *table)
{
  free(table->precomp);
}

static void free_tbase(struct TbEntry *entry, struct Tbase *tb)
{
  (void)entry;
  unmap_file(tb->data, tb->mapping);
  int num =  tb->layout == LT_PIECE ? 1
           : tb->layout == LT_PIECE_K ? 10
           : tb->layout == LT_PIECE_KK ? 462
           : tb->layout == LT_PAWN_FILE ? 4 : 6;
  if (tb->distFormat & TWO_SIDED)
    num *= 2;
  for (int i = 0; i < num; i++) {
    struct TbTable *table = atomic_load_explicit(&tb->table[i],
        memory_order_relaxed);
    if ((uintptr_t)table > 1) free_tb_table(table);
  }
  free(tb);
}

static void free_tb_entry(struct TbEntry *entry)
{
  for (int i = 0; i < 3; i++) {
    struct Tbase *tb = atomic_load_explicit(&entry->tbase[i],
        memory_order_relaxed);
    if (tb) free_tbase(entry, tb);
  }
}

void TB_free(void)
{
  TB_init(NULL);
  free(tbEntry);
}

void TB_release(void)
{
  for (int i = 0; i < numTBs; i++)
    free_tb_entry(&tbEntry[i]);
}

static void create_piece_string(char *s, int n, uint32_t idx)
{
  s[n] = 0;
  if (n == 0) return;
  for (int k = n - 1; k > 0; k--) {
    int l = 0;
    while (idx >= Binomial[k + 1][k + 1 + l])
      l++;
    idx -= Binomial[k + 1][k + l];
    s[n - 1 - k] = PieceToChar[l + 1];
  }
  s[n - 1] = PieceToChar[idx + 1];
}

void TB_init(const char *pathList)
{
  if (!initialized) {
    init_indices();
    initialized = 1;
  }

  // if pathString is set, we need to clean up first.
  if (pathString) {
    free(pathString);
    free(paths);

    TB_release();

    LOCK_DESTROY(mutex);

    pathString = NULL;
  }

  for (int i = 0; i < 3; i++)
    TB_NumTables[i] = TB_MaxCardinality[i] = 0;
  numTBs = 0;

  // if path is an empty string or equals "<empty>", we are done.
  if (!pathList || strlen(pathList) == 0 || !strcmp(pathList, "<empty>"))
    return;

  pathString = malloc(strlen(pathList) + 1);
  strcpy(pathString, pathList);
  char *p = pathString;
  for (p = strtok(p, SEP_STR); p; p = strtok(NULL, SEP_STR))
    numPaths++;
  paths = malloc(numPaths * sizeof(*paths));
  p = pathString;
  for (int i = 0; i < numPaths; i++, p += strlen(p) + 1)
    paths[i] = p;

  LOCK_INIT(mutex);

  if (!tbEntry) {
    tbEntry = malloc(TB_MAX_TABLES * sizeof *tbEntry);
    if (!tbEntry) {
      fprintf(stderr, "Out of memory.\n");
      exit(EXIT_FAILURE);
    }
  }

  for (int i = 0; i < (1 << TB_HASHBITS); i++)
    tbHash[i] = (struct HashEntry){ 0 };

  char white[16], black[16], name[40];

  for (int p = 1; p <= 6; p++)
    for (int q = 0; q <= min(p, 6 - p); q++)
      for (int k = Binomial[4][p + 4] - 1; k >= 0; k--) {
        create_piece_string(white, p, k);
        for (int l = q < p ? (int)Binomial[4][q + 4] - 1 : k; l >= 0; l--) {
          create_piece_string(black, q, l);
          sprintf(name, "K%svK%s", white, black);
          detect_tb(name);
        }
      }
}

static const int8_t OffDiag[] = {
  0,-1,-1,-1,-1,-1,-1,-1,
  1, 0,-1,-1,-1,-1,-1,-1,
  1, 1, 0,-1,-1,-1,-1,-1,
  1, 1, 1, 0,-1,-1,-1,-1,
  1, 1, 1, 1, 0,-1,-1,-1,
  1, 1, 1, 1, 1, 0,-1,-1,
  1, 1, 1, 1, 1, 1, 0,-1,
  1, 1, 1, 1, 1, 1, 1, 0
};

static const uint8_t Triangle[] = {
  6, 0, 1, 2, 2, 1, 0, 6,
  0, 7, 3, 4, 4, 3, 7, 0,
  1, 3, 8, 5, 5, 8, 3, 1,
  2, 4, 5, 9, 9, 5, 4, 2,
  2, 4, 5, 9, 9, 5, 4, 2,
  1, 3, 8, 5, 5, 8, 3, 1,
  0, 7, 3, 4, 4, 3, 7, 0,
  6, 0, 1, 2, 2, 1, 0, 6
};

static const uint8_t FlipDiag[] = {
   0,  8, 16, 24, 32, 40, 48, 56,
   1,  9, 17, 25, 33, 41, 49, 57,
   2, 10, 18, 26, 34, 42, 50, 58,
   3, 11, 19, 27, 35, 43, 51, 59,
   4, 12, 20, 28, 36, 44, 52, 60,
   5, 13, 21, 29, 37, 45, 53, 61,
   6, 14, 22, 30, 38, 46, 54, 62,
   7, 15, 23, 31, 39, 47, 55, 63
};

static const uint8_t Lower[] = {
  28,  0,  1,  2,  3,  4,  5,  6,
   0, 29,  7,  8,  9, 10, 11, 12,
   1,  7, 30, 13, 14, 15, 16, 17,
   2,  8, 13, 31, 18, 19, 20, 21,
   3,  9, 14, 18, 32, 22, 23, 24,
   4, 10, 15, 19, 22, 33, 25, 26,
   5, 11, 16, 20, 23, 25, 34, 27,
   6, 12, 17, 21, 24, 26, 27, 35
};

static const uint8_t Diag[] = {
   0,  0,  0,  0,  0,  0,  0,  8,
   0,  1,  0,  0,  0,  0,  9,  0,
   0,  0,  2,  0,  0, 10,  0,  0,
   0,  0,  0,  3, 11,  0,  0,  0,
   0,  0,  0, 12,  4,  0,  0,  0,
   0,  0, 13,  0,  0,  5,  0,  0,
   0, 14,  0,  0,  0,  0,  6,  0,
  15,  0,  0,  0,  0,  0,  0,  7
};

static const uint8_t Flap[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  6, 12, 18, 18, 12,  6,  0,
     1,  7, 13, 19, 19, 13,  7,  1,
     2,  8, 14, 20, 20, 14,  8,  2,
     3,  9, 15, 21, 21, 15,  9,  3,
     4, 10, 16, 22, 22, 16, 10,  4,
     5, 11, 17, 23, 23, 17, 11,  5,
     0,  0,  0,  0,  0,  0,  0,  0  },
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  1,  2,  3,  3,  2,  1,  0,
     4,  5,  6,  7,  7,  6,  5,  4,
     8,  9, 10, 11, 11, 10,  9,  8,
    12, 13, 14, 15, 15, 14, 13, 12,
    16, 17, 18, 19, 19, 18, 17, 16,
    20, 21, 22, 23, 23, 22, 21, 20,
     0,  0,  0,  0,  0,  0,  0,  0  }
};

static const uint8_t PawnTwist[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 35, 23, 11, 10, 22, 34, 46,
    45, 33, 21,  9,  8, 20, 32, 44,
    43, 31, 19,  7,  6, 18, 30, 42,
    41, 29, 17,  5,  4, 16, 28, 40,
    39, 27, 15,  3,  2, 14, 26, 38,
    37, 25, 13,  1,  0, 12, 24, 36,
     0,  0,  0,  0,  0,  0,  0,  0 },
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 45, 43, 41, 40, 42, 44, 46,
    39, 37, 35, 33, 32, 34, 36, 38,
    31, 29, 27, 25, 24, 26, 28, 30,
    23, 21, 19, 17, 16, 18, 20, 22,
    15, 13, 11,  9,  8, 10, 12, 14,
     7,  5,  3,  1,  0,  2,  4,  6,
     0,  0,  0,  0,  0,  0,  0,  0 }
};

static const int16_t KKIdx[10][64] = {
  { -1, -1, -1,  0,  1,  2,  3,  4,
    -1, -1, -1,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33,
    34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 53, 54, 55, 56, 57 },
  { 58, -1, -1, -1, 59, 60, 61, 62,
    63, -1, -1, -1, 64, 65, 66, 67,
    68, 69, 70, 71, 72, 73, 74, 75,
    76, 77, 78, 79, 80, 81, 82, 83,
    84, 85, 86, 87, 88, 89, 90, 91,
    92, 93, 94, 95, 96, 97, 98, 99,
   100,101,102,103,104,105,106,107,
   108,109,110,111,112,113,114,115},
  {116,117, -1, -1, -1,118,119,120,
   121,122, -1, -1, -1,123,124,125,
   126,127,128,129,130,131,132,133,
   134,135,136,137,138,139,140,141,
   142,143,144,145,146,147,148,149,
   150,151,152,153,154,155,156,157,
   158,159,160,161,162,163,164,165,
   166,167,168,169,170,171,172,173 },
  {174, -1, -1, -1,175,176,177,178,
   179, -1, -1, -1,180,181,182,183,
   184, -1, -1, -1,185,186,187,188,
   189,190,191,192,193,194,195,196,
   197,198,199,200,201,202,203,204,
   205,206,207,208,209,210,211,212,
   213,214,215,216,217,218,219,220,
   221,222,223,224,225,226,227,228 },
  {229,230, -1, -1, -1,231,232,233,
   234,235, -1, -1, -1,236,237,238,
   239,240, -1, -1, -1,241,242,243,
   244,245,246,247,248,249,250,251,
   252,253,254,255,256,257,258,259,
   260,261,262,263,264,265,266,267,
   268,269,270,271,272,273,274,275,
   276,277,278,279,280,281,282,283 },
  {284,285,286,287,288,289,290,291,
   292,293, -1, -1, -1,294,295,296,
   297,298, -1, -1, -1,299,300,301,
   302,303, -1, -1, -1,304,305,306,
   307,308,309,310,311,312,313,314,
   315,316,317,318,319,320,321,322,
   323,324,325,326,327,328,329,330,
   331,332,333,334,335,336,337,338 },
  { -1, -1,339,340,341,342,343,344,
    -1, -1,345,346,347,348,349,350,
    -1, -1,441,351,352,353,354,355,
    -1, -1, -1,442,356,357,358,359,
    -1, -1, -1, -1,443,360,361,362,
    -1, -1, -1, -1, -1,444,363,364,
    -1, -1, -1, -1, -1, -1,445,365,
    -1, -1, -1, -1, -1, -1, -1,446 },
  { -1, -1, -1,366,367,368,369,370,
    -1, -1, -1,371,372,373,374,375,
    -1, -1, -1,376,377,378,379,380,
    -1, -1, -1,447,381,382,383,384,
    -1, -1, -1, -1,448,385,386,387,
    -1, -1, -1, -1, -1,449,388,389,
    -1, -1, -1, -1, -1, -1,450,390,
    -1, -1, -1, -1, -1, -1, -1,451 },
  {452,391,392,393,394,395,396,397,
    -1, -1, -1, -1,398,399,400,401,
    -1, -1, -1, -1,402,403,404,405,
    -1, -1, -1, -1,406,407,408,409,
    -1, -1, -1, -1,453,410,411,412,
    -1, -1, -1, -1, -1,454,413,414,
    -1, -1, -1, -1, -1, -1,455,415,
    -1, -1, -1, -1, -1, -1, -1,456 },
  {457,416,417,418,419,420,421,422,
    -1,458,423,424,425,426,427,428,
    -1, -1, -1, -1, -1,429,430,431,
    -1, -1, -1, -1, -1,432,433,434,
    -1, -1, -1, -1, -1,435,436,437,
    -1, -1, -1, -1, -1,459,438,439,
    -1, -1, -1, -1, -1, -1,460,440,
    -1, -1, -1, -1, -1, -1, -1,461 }
};

static const uint8_t FileToFile[8] = { 0, 1, 2, 3, 3, 2, 1, 0 };
static const int WdlToMap[5] = { 1, 3, 0, 2, 0 };
static const uint8_t PAFlags[5] = { 8, 0, 0, 0, 4 };

static size_t PawnIdx[2][6][24];
static size_t PawnFactorFile[6][4];
static size_t PawnFactorRank[6][6];

static uint8_t MirrorMask[64];
static int16_t KKMap[64][64];
static bool FlipTest[64][64];

static uint8_t Off10[10][64];
//static int16_t TableP[24][64];


// Perfect indexing code for positions with Ks on the diagonal starts here.

typedef uint64_t Bitboard;

INLINE int lsb(Bitboard b)
{
  return __builtin_ctzll(b);
}

INLINE int pop_lsb(Bitboard *b)
{
  int s = lsb(*b);
  *b &= *b - 1;
  return s;
}

INLINE int rank_among_free(uint8_t sq, uint64_t occ)
{
  return sq - popcnt(occ & ((1ULL << sq) - 1));
}

static const uint8_t partition[30][7] = {
  { 0 },
  { 1 },
  { 2 }, { 1, 1},
  { 3 }, { 2, 1}, { 1, 1, 1 },
  { 4 }, { 3, 1}, { 2, 2 }, { 2, 1, 1 }, { 1, 1, 1, 1 },
  { 5 }, { 4, 1}, { 3, 2 }, { 3, 1, 1 }, { 2, 2, 1 }, { 2, 1, 1, 1},
         { 1, 1, 1, 1, 1},
  { 6 }, { 5, 1}, { 4, 2 }, { 4, 1, 1 }, { 3, 3 }, { 3, 2, 1 }, { 3, 1, 1, 1},
         { 2, 2, 2 }, { 2, 2, 1, 1 }, { 2, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1, 1}
};

static int8_t next_partition[30][8];
static uint8_t transition_id[30][8];

uint64_t reflection_size[30];

// Per transition, one case per number of 2-orbits filled.
struct TransitionCase {
  uint8_t d;
  uint8_t rem;
  uint64_t diag_tail;
  uint64_t diag_block;
  uint64_t broken_tail;
  uint64_t per_full_block;
  uint64_t prefix;
};

// There are 201 valid cases.
static struct TransitionCase transition_cases[45][12][4];

INLINE Bitboard flip_main_diag(Bitboard b)
{
  Bitboard t;

  t  = (b ^ (b << 28)) & UINT64_C(0x0f0f0f0f00000000);
  b ^= t ^ (t >> 28);

  t  = (b ^ (b << 14)) & UINT64_C(0x3333000033330000);
  b ^= t ^ (t >> 14);

  t  = (b ^ (b << 7)) & UINT64_C(0x5500550055005500);
  b ^= t ^ (t >> 7);

  return b;
}

INLINE uint64_t binom(int n, int k)
{
  return (k < 0 || k > n) ? 0 : Binomial[k][n];
}

// Fold (p,s) into the range 0...11.
// p = number of empty 2-orbits, s = number of empty 1-orbits.
INLINE int fold_ps(int p, int s)
{
  int used_p = 28 - p, used_s = 6 - s;
  return used_p * 7 - used_p * used_p + used_s;
}

static uint8_t unfold_ps[12][2];

static int find_partition(int len, uint8_t mult[])
{
  uint8_t m[6] = { 0 };

  for (int i = 0; i < len; i++)
    m[i] = mult[i];

  for (int i = 0; i < len; i++)
    for (int j = i + 1; j < len; j++)
      if (m[i] < m[j])
        Swap(m[i], m[j]);

  for (int i = 0; i < 30; i++)
    if (memcmp(m, partition[i], 6) == 0)
      return i;

  abort();
}

static uint64_t count_broken_residual_cases(int m, int p, int s, int d)
{
  int rem = m - 2 * d;
  int one_min = max(1, rem - s);
  int one_max = min(p - d, rem);
  uint64_t total = 0;
  for (int one = one_min; one <= one_max; one++) {
    int f = rem - one;
    total += (binom(p - d, one) * binom(s, f)) << (one - 1);
  }
  return total;
}

static uint64_t count_trivial_part(int part_id, int free)
{
  const uint8_t *mult = partition[part_id];
  uint64_t r = 1;
  for (int i = 0; mult[i]; i++) {
    int m = mult[i];
    r *= binom(free, m);
    free -= m;
  }
  return r;
}

static void init_perfect_ranker(void)
{
  int id = 0;
  for (int used_p = 0; 2 * used_p <= 5; used_p++)
    for (int used_s = 0; 2 * used_p + used_s <= 5; used_s++) {
      unfold_ps[id][0] = 28 - used_p;
      unfold_ps[id][1] = 6 - used_s;
      id++;
    }

  memset(next_partition, -1, sizeof next_partition);

  uint8_t mult[7];
  for (int p = 0; p < 30; p++) {
    int m = 0;
    for (int i = 0; partition[p][i]; i++) {
      if (partition[p][i] == m)
        continue;
      m = partition[p][i];
      int k = 0;
      for (int j = 0; partition[p][j]; j++)
        if (j != i)
          mult[k++] = partition[p][j];
      next_partition[p][m] = find_partition(k, mult);
    }
  }

  uint64_t count[30][4][7] = { 0 };

  for (int p = 25; p <= 28; p++)
    for (int s = 0; s <= 6; s++)
      count[0][p - 25][s] = 1;

  for (int id = 1; id < 30; id++) {
    const uint8_t *part = partition[id];
    int m = part[0];
    int next_part = next_partition[id][m];
    for (int p = 25; p <= 28; p++)
      for (int s = 0; s <= 6; s++) {
        uint64_t total = 0;
        for (int d = 0; d <= min(p, m / 2); d++) {
          int rem = m - 2 * d;
          uint64_t per_full = 0;
          if (rem <= s)
            per_full += binom(s, rem) * count[next_part][p - d - 25][s - rem];
          uint64_t broken_cases = count_broken_residual_cases(m, p, s, d);
          if (broken_cases != 0)
            per_full += broken_cases * count_trivial_part(next_part,
                2 * p + s - m);
          total += binom(p, d) * per_full;
        }
        count[id][p - 25][s] = total;
      }
    reflection_size[id] = count[id][28 - 25][6];
  }

  id = 0;
  for (int part_id = 1; part_id < 30; part_id++) {
    for (int m = 1; m <= TB_SETS; m++) {
      int next_part = next_partition[part_id][m];
      if (next_part < 0)
        continue;
      for (int ps = 0; ps < 12; ps++) {
        int p = unfold_ps[ps][0];
        int s = unfold_ps[ps][1];
        uint64_t prefix = 0;
        for (int d = 0; d <= min(p, m / 2); d++) {
          int rem = m - 2 * d;
          uint64_t diag_tail = 0;
          uint64_t diag_block = 0;
          if (rem <= s) {
            diag_tail = count[next_part][p - d - 25][s - rem];
            diag_block = binom(s, rem) * diag_tail;
          }
          uint64_t broken_cases = count_broken_residual_cases(m, p, s, d);
          uint64_t broken_tail =
            broken_cases == 0 ? 0 : count_trivial_part(next_part, 2 * p + s - m);
          uint64_t per_full = diag_block + broken_cases * broken_tail;
          uint64_t block = binom(p, d) * per_full;
          transition_cases[id][ps][d] = (struct TransitionCase) {
            .d = d,
            .rem = rem,
            .diag_tail = diag_tail,
            .diag_block = diag_block,
            .broken_tail = broken_tail,
            .per_full_block = per_full,
            .prefix = prefix
          };
          prefix += block;
        }
      }
      transition_id[part_id][m] = id++;
    }
  }
}

static uint64_t rank_combination(Bitboard subset, Bitboard universe)
{
  uint64_t dense = _pext_u64(subset, universe);

  uint64_t r = 0;
  for (int j = 1; dense; j++)
    r += Binomial[j][pop_lsb(&dense)];
  return r;
}

static uint64_t rank_trivial_from(uint8_t *restrict sq, int k, Bitboard occ,
    int numsets, const struct TbTable2 *table)
{
  uint64_t idx = 0;
  for (; k < numsets; k++) {
    size_t s;
    int i = table->first[k];
    int m = table->mult[k];
    if (m == 1) {
      s = rank_among_free(sq[i], occ);
      occ |= bit(sq[i]);
    } else if (m == 2) {
      Bitboard b = ~occ;
      Bitboard b1 = bit(sq[i]) | bit(sq[i + 1]);
      occ |= b1;
      b1 = _pext_u64(b1, b);
      s = pop_lsb(&b1);
      s += Binomial[2][lsb(b1)];
    } else {
      Bitboard b = ~occ, b1 = 0;
      for (int j = 0; j < m; j++)
        b1 |= bit(sq[i + j]);
      occ |= b1;
      b1 = _pext_u64(b1, b);
      s = 0;
      for (int j = 1; b1; j++)
        s += Binomial[j][pop_lsb(&b1)];
    }
    idx = idx * table->factor[k] + s;
  }
  return idx;
}

INLINE uint64_t count_broken_residual_before(int rem, int p, int s, int one)
{
  uint64_t total = 0;
  int one_min = max(1, rem - s);
  for (int oo = one_min; oo < one; oo++) {
    int f = rem - oo;
    total += binom(p, oo) * binom(s, f) * (1ull << (oo - 1));
  }
  return total;
}

#define LOWER_DIAG_MASK UINT64_C(0x0080c0e0f0f8fcfe)
#define MAIN_DIAG_MASK  UINT64_C(0x8040201008040201)

static uint64_t rank_reflection(uint8_t *restrict sq, Bitboard occ,
    int numsets, const struct TbTable2 *table)
{
  int part_id = table->part_id;
  Bitboard pair_mask = LOWER_DIAG_MASK;
  Bitboard diag_mask = MAIN_DIAG_MASK & ~occ;
  int p = 28, s = 6;

  uint64_t rank = 0;
  for (int k = 0; k < numsets; k++) {
    int m = table->mult[k];
    Bitboard bb = 0;
    for (int i = 0; i < m; i++)
      bb |= bit(sq[table->first[k] + i]);
    occ |= bb;
    int tid = transition_id[part_id][m];

    Bitboard mirror = flip_main_diag(bb);
    Bitboard full_mask = bb & mirror & pair_mask;
    Bitboard one_mask = (bb ^ mirror) & pair_mask;
    int d = popcnt(full_mask);    // Number of 2-orbits fully filled.

    const struct TransitionCase *c = &transition_cases[tid][fold_ps(p, s)][d];

    rank += c->prefix;
    rank += rank_combination(full_mask, pair_mask) * c->per_full_block;
    pair_mask &= ~full_mask;
    p -= d;

    if (!one_mask) {
      rank += rank_combination(bb, diag_mask) * c->diag_tail;
      diag_mask &= ~bb;
      s = popcnt(diag_mask);
      part_id = next_partition[part_id][m];
      continue;
    }

    int one = popcnt(one_mask);     // Number of 2-orbits half filled.
    int f = popcnt(bb & diag_mask); // Number of 1-orbits filled.
    rank += c->diag_block;
    uint64_t r = count_broken_residual_before(c->rem, p, s, one);

    uint64_t rone = rank_combination(one_mask, pair_mask);
    r += (rone * binom(s, f) + rank_combination(bb, diag_mask)) << (one - 1);
    rank += r * c->broken_tail;

    // Canonical orientation: orient_mask <= bitwise complement within oo bits.
    uint32_t orient_mask = _pext_u64(bb, one_mask);
    uint32_t mask = (1u << one) - 1u;
    uint32_t comp = (~orient_mask) & mask;
    uint32_t canon = orient_mask < comp ? orient_mask : comp;
    // Among 2^oo orientations paired with complements, canonical ones are
    // exactly 0 .. 2^(oo-1)-1 after min(x,~x) for this ordering.
    assert(canon < (1u << (one - 1)));
    rank += canon * c->broken_tail;

    if (comp < orient_mask) {
      for (int i = 2; i < TB_PIECES; i++)
        sq[i] = FlipDiag[sq[i]];
      occ = flip_main_diag(occ);
    }
    return rank + rank_trivial_from(sq, k + 1, occ, numsets, table);
  }
  return rank;
}

static void init_indices(void)
{
  int i, j;

  // Binomial[k][n] = Bin(n, k)
  for (j = 0; j < 64; j++)
    Binomial[0][j] = 1;
  for (i = 1; i < 8; i++)
    for (j = 1; j < 64; j++)
      Binomial[i][j] = Binomial[i - 1][j - 1] + Binomial[i][j - 1];

  for (i = 0; i < 6; i++) {
    size_t s = 0;
    for (j = 0; j < 24; j++) {
      PawnIdx[0][i][j] = s;
      s += Binomial[i][PawnTwist[0][(1 + (j % 6)) * 8 + (j / 6)]];
      if ((j + 1) % 6 == 0) {
        PawnFactorFile[i][j / 6] = s;
        s = 0;
      }
    }
  }

  for (i = 0; i < 6; i++) {
    size_t s = 0;
    for (j = 0; j < 24; j++) {
      PawnIdx[1][i][j] = s;
      s += Binomial[i][PawnTwist[1][(1 + (j / 4)) * 8 + (j % 4)]];
      if ((j + 1) % 4 == 0) {
        PawnFactorRank[i][j / 4] = s;
        s = 0;
      }
    }
  }

  const uint64_t A1D1D4 = 0x080c0e0full;
  const uint64_t A1D4   = 0x08040201ull;
  const uint64_t LOWER  = 0x80c0e0f0f8fcfeffull;

  for (int s = 0; s < 64; s++)
    MirrorMask[s] = ((s & 0x04) ? 0x07 : 0x00) | ((s & 0x20) ? 0x38 : 0x00);

  for (int i = 0; i < 64; i++)
    for (int j = 0; j < 64; j++) {
      int s1 = i ^ MirrorMask[i];
      int s2 = j ^ MirrorMask[i];
      if (!(bit(s1) & A1D1D4) || ((bit(s1) & A1D4) && !(bit(s2) & LOWER))) {
        FlipTest[i][j] = true;
        s1 = FlipDiag[s1];
        s2 = FlipDiag[s2];
      }
      KKMap[i][j] = KKIdx[Triangle[s1]][s2];
    }

  for (int i = 0; i < 10; i++) {
    int n = 0;
    for (int j = 0; j < 64; j++) {
      if (KKIdx[i][j] < 0) continue;
      Off10[i][j] = n++;
    }
  }

  init_perfect_ranker();
}

INLINE int leading_pawn(uint8_t *restrict p, struct TbEntry *entry,
    const int lt)
{
  for (int i = 1; i < entry->pawns[0]; i++)
    if (Flap[lt - LT_PAWN_FILE][p[0]] > Flap[lt - LT_PAWN_FILE][p[i]])
      Swap(p[0], p[i]);

  return lt == LT_PAWN_FILE ? FileToFile[p[0] & 7] : (p[0] - 8) >> 3;
}

INLINE void sort_squares(int n, uint8_t *restrict sq)
{
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (sq[i] > sq[j])
        Swap(sq[i], sq[j]);
}

INLINE size_t encode(uint8_t *restrict p, struct EncInfo *ei,
    struct TbEntry *entry, const int lt)
{
  int n = entry->num;
  size_t idx;
  Bitboard occ;
  int k;

  if (p[0] & 0x04)
    for (int i = 0; i < n; i++)
      p[i] ^= 0x07;

  if (lt == LT_PIECE) {
    if (p[0] & 0x20)
      for (int i = 0; i < n; i++)
        p[i] ^= 0x38;

    for (int i = 0; i < n; i++)
      if (OffDiag[p[i]]) {
        if (OffDiag[p[i]] > 0 && i < (entry->kk_enc ? 2 : 3))
          for (int j = 0; j < n; j++)
            p[j] = FlipDiag[p[j]];
        break;
      }

    occ = bit(p[0]) | bit(p[1]);
    if (entry->kk_enc) {
      idx = KKIdx[Triangle[p[0]]][p[1]];
      k = 2;
    } else {
      int s1 = (p[1] > p[0]);
      int s2 = (p[2] > p[0]) + (p[2] > p[1]);

      if (OffDiag[p[0]])
        idx = Triangle[p[0]] * 63*62 + (p[1] - s1) * 62 + (p[2] - s2);
      else if (OffDiag[p[1]])
        idx = 6*63*62 + Diag[p[0]] * 28*62 + Lower[p[1]] * 62 + p[2] - s2;
      else if (OffDiag[p[2]])
        idx =  6*63*62 + 4*28*62 + Diag[p[0]] * 7*28
             + (Diag[p[1]] - s1) * 28 + Lower[p[2]];
      else
        idx =  6*63*62 + 4*28*62 + 4*7*28 + Diag[p[0]] * 7*6
             + (Diag[p[1]] - s1) * 6 + (Diag[p[2]] - s2);
      k = 3;
      occ |= bit(p[2]);
    }
    idx *= ei->factor[0];
  } else {
    const int enc = lt - LT_PAWN_FILE;
    for (int i = 1; i < entry->pawns[0]; i++)
      for (int j = i + 1; j < entry->pawns[0]; j++)
        if (PawnTwist[enc][p[i]] < PawnTwist[enc][p[j]])
          Swap(p[i], p[j]);

    k = entry->pawns[0];
    idx = PawnIdx[enc][k - 1][Flap[enc][p[0]]];
    for (int i = 1; i < k; i++)
      idx += Binomial[k-i][PawnTwist[enc][p[i]]];
    idx *= ei->factor[0];

    occ = 0;
    for (int i = 0; i < k; i++)
      occ |= bit(p[i]);

    // Pawns of other color
    if (entry->pawns[1]) {
      int t = k + entry->pawns[1];
      sort_squares(entry->pawns[1], &p[k]);
      size_t s = 0;
      for (int i = k; i < t; i++) {
        int rank = rank_among_free(p[i], occ);
        s += Binomial[i - k + 1][rank - 8];
      }
      idx += s * ei->factor[k];
      for (; k < t; k++)
        occ |= bit(p[k]);
    }
  }

  for (; k < n;) {
    int t = k + ei->norm[k];
    sort_squares(ei->norm[k], &p[k]);
    size_t s = 0;
    for (int i = k; i < t; i++) {
      int rank = rank_among_free(p[i], occ);
      s += Binomial[i - k + 1][rank];
    }
    idx += s * ei->factor[k];
    for (; k < t; k++)
      occ |= bit(p[k]);
  }

  return idx;
}

static NOINLINE size_t encode_piece(uint8_t *restrict p, struct EncInfo *ei,
    struct TbEntry *entry)
{
  return encode(p, ei, entry, LT_PIECE);
}

static NOINLINE size_t encode_pawn_f(uint8_t *restrict p, struct EncInfo *ei,
    struct TbEntry *entry)
{
  return encode(p, ei, entry, LT_PAWN_FILE);
}

static NOINLINE size_t encode_pawn_r(uint8_t *restrict p, struct EncInfo *ei,
    struct TbEntry *entry)
{
  return encode(p, ei, entry, LT_PAWN_RANK);
}

static size_t init_enc_info(struct EncInfo *ei, struct TbEntry *entry,
    const uint8_t *tb, int shift, int t, const int lt)
{
  bool morePawns = lt != LT_PIECE && entry->pawns[1] > 0;

  for (int i = 0; i < entry->num; i++) {
    ei->pieces[i] = (tb[i + 1 + morePawns] >> shift) & 0x0f;
    ei->norm[i] = 0;
  }

  int order = (tb[0] >> shift) & 0x0f;
  int order2 = morePawns ? (tb[1] >> shift) & 0x0f : 0x0f;

  int k = ei->norm[0] =  lt != LT_PIECE ? entry->pawns[0]
                       : entry->kk_enc ? 2 : 3;

  if (morePawns) {
    ei->norm[k] = entry->pawns[1];
    k += ei->norm[k];
  }

  for (int i = k; i < entry->num; i += ei->norm[i])
    for (int j = i; j < entry->num && ei->pieces[j] == ei->pieces[i]; j++)
      ei->norm[i]++;

  int n = 64 - k;
  size_t f = 1;

  for (int i = 0; k < entry->num || i == order || i == order2; i++) {
    if (i == order) {
      ei->factor[0] = f;
      f *=  lt == LT_PAWN_FILE ? PawnFactorFile[ei->norm[0] - 1][t]
          : lt == LT_PAWN_RANK ? PawnFactorRank[ei->norm[0] - 1][t]
          : entry->kk_enc ? 462 : 31332;
    } else if (i == order2) {
      ei->factor[ei->norm[0]] = f;
      f *= Binomial[ei->norm[ei->norm[0]]][48 - ei->norm[0]];
    } else {
      ei->factor[k] = f;
      f *= Binomial[ei->norm[k]][n];
      n -= ei->norm[k];
      k += ei->norm[k];
    }
  }

  return f;
}

static void calc_symlen(struct PairsData *d, uint32_t s, bool *tmp)
{
  const uint32_t w = read_le_u32(d->symPat + 3 * s);
  uint32_t s2 = (w >> 12) & 0xfff;
  if (s2 == 0x0fff)
    d->symLen[s] = 0;
  else {
    uint32_t s1 = w & 0xfff;
    if (!tmp[s1]) calc_symlen(d, s1, tmp);
    if (!tmp[s2]) calc_symlen(d, s2, tmp);
    d->symLen[s] = d->symLen[s1] + d->symLen[s2] + 1;
  }
  tmp[s] = true;
}

struct PairsData *setup_huffman(const uint8_t **ptr)
{
  const uint8_t *data = *ptr;
  int max_len = data[8];
  int min_len = data[9];
  int h = max_len - min_len + 1;
  uint32_t num_syms = read_le_u16(&data[10 + 2 * h]);
  uint16_t *offset = (uint16_t *)(&data[10]);

  int hh = h;

  uint64_t tmp_base[32];
  for (int i = h - 1; i < hh; i++)
    tmp_base[i] = 0;
  for (int i = h - 2; i >= 0; i--)
    tmp_base[i] = (tmp_base[i + 1] + offset[i] - offset[i + 1]) / 2;
  for (int i = 0; i < h; i++)
    tmp_base[i] <<= 64 - (min_len + i);

  struct PairsData *d = malloc(sizeof(*d) + hh * sizeof(uint64_t) + num_syms);
  d->comprType = 0;
  for (int i = 0; i < hh; i++)
    d->base[i] = tmp_base[i];
  d->offset = offset;
  d->symLen = (uint8_t *)(d + 1) + h * sizeof(uint64_t);
  d->symPat = &data[12 + 2 * h];
  d->minLen = min_len;
  *ptr = &data[12 + 2 * h + 3 * num_syms + (num_syms & 1)];

  bool tmp[4096] = { 0 };
  for (uint32_t s = 0; s < num_syms; s++)
    if (!tmp[s])
      calc_symlen(d, s, tmp);

  for (uint64_t i = 0; i < (1 << STARTBITS); i++) {
    uint64_t code = ((i + 1) << (64 - STARTBITS)) - 1;
    int l = 0;
    while (code < d->base[l]) l++;
    d->start[i] = l + d->minLen;
  }

  d->offset -= d->minLen;

  return d;
}

static struct PairsData *setup_rans(const uint8_t **ptr)
{
  const uint8_t *data = *ptr;

  struct PairsData *d = malloc(sizeof(struct PairsData));
  d->comprType = 1;

  int num_syms;
  d->rans = calloc(1, sizeof(struct RansDecode));
  const uint8_t *p = read_freq_table(d->rans, &num_syms, data + 8);
  make_alias_table(d->rans);

  d->symPat = p;
  d->symLen = malloc(num_syms);
  bool tmp[4096] = { 0 };
  for (int s = 0; s < num_syms; s++)
    if (!tmp[s])
      calc_symlen(d, s, tmp);

  *ptr = p + 3 * num_syms + (num_syms & 1);

  return d;
}

static struct PairsData *setup_pairs(const uint8_t **ptr, size_t tb_size,
    size_t *size, uint8_t *flags, int type, bool new)
{
  struct PairsData *d;
  const uint8_t *data = *ptr;

  if (!new) {
    *flags = data[0];
    if (data[0] & 0x80) {
      d = malloc(sizeof(*d));
      d->idxBits = 0;
      d->constValue[0] = type == WDL ? data[1] : 0;
      d->constValue[1] = 0;
      d->comprType = 2;
      *ptr = data + 2;
      size[0] = size[1] = size[2] = 0;
      return d;
    }
    d = (data[0] & 0x40) ? setup_rans(ptr) : setup_huffman(ptr);
  } else {
    d = data[0] == 2 ? setup_rans(ptr) : setup_huffman(ptr);
  }

  uint8_t blockSize = data[1];
  uint8_t idxBits = data[2];
  uint32_t realNumBlocks = read_le_u32(&data[4]);
  uint32_t numBlocks = realNumBlocks + data[3];

  d->blockSize = blockSize;
  d->idxBits = idxBits;

  size_t numIndices = (tb_size + (1ULL << idxBits) - 1) >> idxBits;
  size[0] = 6ULL * numIndices;
  size[1] = 2ULL * numBlocks;
  size[2] = (size_t)realNumBlocks << blockSize;

  return d;
}

static NOINLINE struct Tbase *init_old_layout(struct TbEntry *entry,
    struct Tbase *tb, int type, const uint8_t *data, bool new)
{
  bool split;

  if (!new) {
    split = type != DTZ && (data[0] & 0x01);
    tb->flipped = false;
    tb->distFormat = split ? TWO_SIDED : 0;
    tb->distFormat |=  type == DTM ? data[0] & 0x04 ? WIN_OR_LOSS : 0
                     : type == DTZ ? WTM_OR_BTM // Actual stm info in table.
                     : 0;
  } else {
    split = !entry->symmetric;
    if (type != WDL) {
      tb->distFormat = *++data;
      split = tb->distFormat & TWO_SIDED;
    }
    tb->flipped = false;
  }

  data++;

  size_t tb_size[6][2];
  struct TbTable *table[12];

  int num = entry->hasPawns ? type == DTM ? 6 : 4 : 1;
  for (int t = 0; t < num; t++) {
    table[t] = malloc(TABLE_SIZE[type]);
    atomic_store_explicit(&tb->table[t], table[t], memory_order_relaxed);
  }
  if (split)
    for (int t = 0; t < num; t++) {
      table[num + t] = malloc(TABLE_SIZE[type]);
      atomic_store_explicit(&tb->table[t + num], table[t + num],
          memory_order_relaxed);
    }

  for (int t = 0; t < num; t++) {
    struct EncInfo *ei = &table[t]->ei;
    tb_size[t][0] = init_enc_info(ei, entry, data, 0, t, tb->layout);
    if (split) {
      ei = &table[num + t]->ei;
      tb_size[t][1] = init_enc_info(ei, entry, data, 4, t, tb->layout);
    }
    data += entry->num + 1 + (entry->hasPawns && entry->pawns[1]);
  }
  data += (uintptr_t)data & 1;

  size_t size[6][2][3];
  for (int t = 0; t < num; t++) {
    uint8_t flags;
    table[t]->precomp = setup_pairs(&data, tb_size[t][0], size[t][0], &flags,
        type, false);
    if (type == DTZ) {
      struct DtzTable *dtz = (struct DtzTable *)table[t];
      dtz->dtzFlags = flags;
    }
    if (split) {
      table[num + t]->precomp = setup_pairs(&data, tb_size[t][1], size[t][1],
          &flags, type, false);
      if (type == DTZ) {
        struct DtzTable *dtz = (struct DtzTable *)table[num + t];
        dtz->dtzFlags = flags;
      }
    }
  }

  // This may have to be revisted later.
  if (type == DTM && !(tb->distFormat & WIN_OR_LOSS)) {
    const void *map = data;
    for (int t = 0; t < num; t++) {
      struct DtmTable *dtm = (struct DtmTable *)table[t];
      dtm->dtmMap = map;
      for (int i = 0; i < 2; i++) {
        dtm->dtmMapIdx[i] = (uint16_t *)data + 1 - dtm->dtmMap;
        data += 2 + 2 * read_le_u16(data);
      }
      if (split) {
        struct DtmTable *dtm = (struct DtmTable *)table[num + t];
        dtm->dtmMap = map;
        for (int i = 0; i < 2; i++) {
          dtm->dtmMapIdx[i] = (uint16_t *)data + 1 - dtm->dtmMap;
          data += 2 + 2 * read_le_u16(data);
        }
      }
    }
  }

  if (type == DTZ) {
    const void *map = data;
    for (int t = 0; t < num; t++) {
      struct DtzTable *dtz = (struct DtzTable *)table[t];
      if (dtz->dtzFlags & 2) {
        if (!(dtz->dtzFlags & 16)) {
          dtz->dtzMap = map;
          for (int i = 0; i < 4; i++) {
            dtz->dtzMapIdx[i] = data + 1 - dtz->dtzMap;
            data += 1 + data[0];
          }
        } else {
          dtz->dtzMap16 = map;
          data += (uintptr_t)data & 0x01;
          for (int i = 0; i < 4; i++) {
            dtz->dtzMapIdx[i] = (uint16_t *)data + 1 - dtz->dtzMap16;
            data += 2 + 2 * read_le_u16(data);
          }
        }
      }
    }
    data += (uintptr_t)data & 0x01;
    if (split) {
      for (int t = 0; t < num; t++) {
        struct DtzTable *dtz = (struct DtzTable *)table[num + t];
        if (dtz->dtzFlags & 2) {
          if (!(dtz->dtzFlags & 16)) {
            dtz->dtzMap = map;
            for (int i = 0; i < 4; i++) {
              dtz->dtzMapIdx[i] = data + 1 - dtz->dtzMap;
              data += 1 + data[0];
            }
          } else {
            dtz->dtzMap16 = map;
            data += (uintptr_t)data & 0x01;
            for (int i = 0; i < 4; i++) {
              dtz->dtzMapIdx[i] = (uint16_t *)data + 1 - dtz->dtzMap16;
              data += 2 + 2 * read_le_u16(data);
            }
          }
        }
      }
      data += (uintptr_t)data & 0x01;
    }
  }

  for (int t = 0; t < num; t++) {
    table[t]->precomp->indexTable = data;
    data += size[t][0][0];
    if (split) {
      table[num + t]->precomp->indexTable = data;
      data += size[t][1][0];
    }
  }

  for (int t = 0; t < num; t++) {
    table[t]->precomp->sizeTable = (uint16_t *)data;
    data += size[t][0][1];
    if (split) {
      table[num + t]->precomp->sizeTable = (uint16_t *)data;
      data += size[t][1][1];
    }
  }

  for (int t = 0; t < num; t++) {
    data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~0x3fULL);
    table[t]->precomp->data = data;
    data += size[t][0][2];
    if (split) {
      data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~0x3fULL);
      table[num + t]->precomp->data = data;
      data += size[t][1][2];
    }
  }

  // To be looked into later.
#if 0
  if (type == DTM && be->hasPawns) {
    int count[16];
    for (int i = 0; i < 16; i++)
      count[i] = 0;
    for (int i = 0; i < be->num; i++)
      count[ei[0].pieces[i]]++;
    PWN_E(be)->dtmSwitched =
      TB_material_key_from_counts(count, count + 8) != be->key;
  }
#endif

  return tb;
}

static NOINLINE struct Tbase *init_tb(struct TbEntry *entry, const char *str,
    const int type)
{
  map_t mapping;
  const uint8_t *restrict data = map_tb(str, suffix[type], &mapping);
  if (!data) return NULL;

  if (read_le_u32(data) == magic[type]) {
    int num = entry->hasPawns ? type == DTM ? 6 : 4 : 1;
    if (type != DTZ && !entry->symmetric)
      num *= 2;
    struct Tbase *tb = calloc(1, sizeof(struct Tbase) + num * sizeof(void *));
    tb->data = data;
    tb->mapping = mapping;
    tb->layout =  !entry->hasPawns ? LT_PIECE
                : type == DTM ? LT_PAWN_RANK : LT_PAWN_FILE;
    return init_old_layout(entry, tb, type, data + 4, false);
  }

  if (read_le_u32(data) == magic2[type]) {
    if (data[4] == 0) { // May need further changes for DTM.
      int num = entry->hasPawns ? type == DTM ? 6 : 4 : 1;
      if (!entry->symmetric && (type == WDL || (data[5] & TWO_SIDED)))
        num *= 2;
      struct Tbase *tb = calloc(1, sizeof(struct Tbase) + num * sizeof(void *));
      tb->data = data;
      tb->mapping = mapping;
      tb->layout =  !entry->hasPawns ? LT_PIECE
                  : type == DTM ? LT_PAWN_RANK : LT_PAWN_FILE;
      return init_old_layout(entry, tb, type, data + 4, true);
    }

    // Check version.
    int version = data[4];
    if (version > 1)
      return NULL;

    const uint8_t *p = data + 4 + entry->num;
    int layout = *p++;
    int distFormat;
    if (type != WDL && layout <= LT_PIECE_KK)
      distFormat = *p++;
    int num =  layout == LT_PIECE_K   ? 10
             : layout == LT_PIECE_KK  ? 462
             : layout == LT_PAWN_P    ? 24
             : layout == LT_PAWN_PK   ? 1512
             : layout == LT_PAWN_PP   ? 576 : 1128;
    p = (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7);
    if (!entry->symmetric) {
      if (type != WDL && layout <= LT_PIECE_KK)
        num *= (distFormat & TWO_SIDED) ? 2 : 1;
      else
        num *= 2;
    }

    struct Tbase *tbase = calloc(1, sizeof *tbase + num * sizeof(void *));
    tbase->data = data;
    tbase->mapping = mapping;
    tbase->version = version;
    tbase->layout = layout;
    if (type != WDL && layout <= LT_PIECE_KK)
      tbase->distFormat = distFormat;
    tbase->offset = (uint64_t *)p - (uint64_t *)data;
    tbase->pt[0] = 6;
    tbase->pt[1] = 14;
    for (int i = 2; i < entry->num; i++)
      tbase->pt[i] = (data[4 + i] & 0x07) | ((data[4 + i] & 0x80) >> 4);
    int c[16] = { 0 };
    for (int i = 0; i < entry->num; i++)
      c[tbase->pt[i]]++;
    tbase->flipped = TB_material_key_from_counts(c, c + 8) != entry->key;
    return tbase;
  }

  fprintf(stderr, "Invalid tablebase file.\n");
  unmap_file(data, mapping);
  return NULL;
}

static NOINLINE struct TbTable2 *init_new_table(struct TbEntry *entry,
    struct Tbase *tb, int type, int tidx, int tsq)
{
  const uint64_t *offsets = (uint64_t *)tb->data + tb->offset;
  if (offsets[tidx] == 0)
    return (struct TbTable2 *)1;
  const uint8_t *data = (uint8_t *)tb->data + offsets[tidx];

  if (data[0] == 0xff) {
    if (type != WDL) {
      struct TbTableConst *tbl = malloc(sizeof *tbl);
      *tbl = (struct TbTableConst){
        .precomp = NULL, .constVal = data[1], .distFormat = data[2]
      };
      return (struct TbTable2 *)tbl;
    }
    return (struct TbTable2 *)&constTable[data[1]];
  }

  struct TbTable2 *table = malloc(TABLE_SIZE2[type]);

  int mapped = 0, distFormat;
  if (type != WDL) {
    mapped = *data++;
    distFormat = tb->layout >= LT_PAWN_P ? *data++ : 0;
  }

  uint8_t first[TB_SETS];
  uint8_t mult[TB_SETS];
  int k = 0;
  for (int i = 2, l = 0; i < entry->num; i++) {
    if (tb->pt[i] != l) {
      l = tb->pt[i];
      first[k] = i;
      mult[k] = 0;
      k++;
    }
    if (k > 0)
      mult[k - 1]++;
  }

  static const uint8_t knum[] = { 58, 58, 58, 55, 55, 55, 33, 30, 30, 30 };
  uint64_t tb_size = 1;
  if (tb->layout == LT_PIECE_KK || tb->layout == LT_PIECE_KK) {
    for (int i = 0, n = 62; i < k; i++) {
      table->first[i] = first[data[i]];
      int m = mult[data[i]];
      table->mult[i] = m;
      table->factor[i] = Binomial[m][n];
      tb_size *= table->factor[i];
      n -= m;
    }
    if (tb->layout == LT_PIECE_KK && tsq >= 441) {
      table->part_id = find_partition(k, mult);
      tb_size = reflection_size[table->part_id];
    }
    data += k;
  }
  else if (tb->layout == LT_PIECE_K) {
    for (int i = 0, n = 62; i < k + 1; i++) {
      if (data[i] == 0) {
        table->first[i] = table->mult[i] = 0;
        table->factor[i] = knum[tsq];
      } else {
        table->first[i] = first[data[i] - 1];
        int m = mult[data[i] - 1];
        table->mult[i] = m;
        table->factor[i] = Binomial[m][n];
        n -= m;
      }
      tb_size *= table->factor[i];
    }
    data += k + 1;
  }
  else if (tb->layout == LT_PAWN_P) {
    for (int i = 0, n = 63; i < k + 1; i++) {
      int l = data[i];
      if (l < 2) {
        table->first[i] = l;
        table->mult[i] = 1;
      } else {
        table->first[i] = first[l - 1];
        table->mult[i] = mult[l - 1];
      }
      table->factor[i] = Binomial[table->mult[i]][n];
      n -= table->mult[i];
      tb_size *= table->factor[i];
    }
    data += k + 1;
  }
  else if (tb->layout == LT_PAWN_PK) {
    for (int i = 0, n = 62; i < k; i++) {
      int l = data[i];
      if (l == 0) {
        table->first[i] = 1;
        table->mult[i] = 1;
      } else {
        table->first[i] = first[l];
        table->mult[i] = mult[l];
      }
      table->factor[i] = Binomial[table->mult[i]][n];
      n -= table->mult[i];
      tb_size *= table->factor[i];
    }
    data += k;
  }
  data += (uintptr_t)data & 1;

  size_t size[3];
  table->precomp = setup_pairs(&data, tb_size, size, NULL, type, true);

  if (type == DTM) {
    struct DtmTable2 *dtm = (struct DtmTable2 *)table;
    dtm->mapped = mapped;
    dtm->distFormat = distFormat;
    if (mapped) {
      dtm->dtmMap = (uint16_t *)data;
      for (int i = 0; i < 2; i++) {
        dtm->dtmMapIdx[i] = (uint16_t *)data + 1 - dtm->dtmMap;
        data += 2 + 2 * read_le_u16(data);
      }
    }
  }

  if (type == DTZ) {
    struct DtzTable2 *dtz = (struct DtzTable2 *)table;
    dtz->mapped = mapped;
    dtz->distFormat = distFormat;
    if (mapped == 1) {
      dtz->dtzMap = data;
      for (int i = 0; i < 4; i++) {
        dtz->dtzMapIdx[i] = data + 1 - dtz->dtzMap;
        data += 1 + data[0];
      }
      data += (uintptr_t)data & 1;
    }
    else if (mapped == 2) {
      dtz->dtzMap16 = (uint16_t *)data;
      for (int i = 0; i < 4; i++) {
        dtz->dtzMapIdx[i] = (uint16_t *)data + 1 - dtz->dtzMap16;
        data += 2 + 2 * read_le_u16(data);
      }
    }
  }

  table->precomp->indexTable = data;
  data += size[0];
  table->precomp->sizeTable = (uint16_t *)data;
  data += size[1];
  data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~0x3fULL);
  table->precomp->data = data;

  return table;
}

INLINE const uint8_t *decompress_rans(const struct PairsData *d, uint64_t idx)
{
  uint32_t mainIdx = idx >> d->idxBits;
  int litIdx =  (int)((uint32_t)idx & ((1u << d->idxBits) - 1))
              - (int)(1u << (d->idxBits - 1));
  uint32_t block = read_le_u32(d->indexTable + 6 * mainIdx);
  litIdx += read_le_u16(d->indexTable + 6 * mainIdx + 4);

  if (litIdx < 0)
    while (litIdx < 0)
      litIdx += d->sizeTable[--block] + 1;
  else
    while (litIdx > d->sizeTable[block])
      litIdx -= d->sizeTable[block++] + 1;

  // Since the symbols in the block are decoded in reverse order, we
  // need to count starting from the end.
  litIdx -= d->sizeTable[block] + 1;

  const uint8_t *p = d->data + ((size_t)block << d->blockSize);
  const uint8_t *end = p + ((size_t)1 << d->blockSize);
  int sym;
  uint64_t rans;
  rans_dec_init(&rans, &p, end);
  for (; litIdx < 0 && p < end; litIdx += d->symLen[sym] + 1) {
    sym = rans_dec_get(&rans, d->rans);
    rans_dec_renorm(&rans, &p);
  }
  for (; litIdx < 0; litIdx += d->symLen[sym] + 1)
    sym = rans_dec_get(&rans, d->rans);

  const uint8_t *symPat = d->symPat;
  while (d->symLen[sym] != 0) {
    uint32_t w = read_le_u32(symPat + 3 * sym);
    int s1 = w & 0xfff;
    if (litIdx < (int)d->symLen[s1] + 1)
      sym = s1;
    else {
      litIdx -= (int)d->symLen[s1] + 1;
      sym = (w >> 12) & 0xfff;
    }
  }

  return &symPat[3 * sym];
}

INLINE const uint8_t *decompress_huff(const struct PairsData *d, uint64_t idx)
{
  uint32_t mainIdx = idx >> d->idxBits;
  int litIdx =  (int)((uint32_t)idx & ((1u << d->idxBits) - 1))
              - (int)(1u << (d->idxBits - 1));
  uint32_t block = read_le_u32(d->indexTable + 6 * mainIdx);
  litIdx += read_le_u16(d->indexTable + 6 * mainIdx + 4);

  // Add/subtract block sizes until 0 <= litIdx <= d->sizeTable[block].
  if (litIdx < 0)
    while (litIdx < 0)
      litIdx += d->sizeTable[--block] + 1;
  else
    while (litIdx > d->sizeTable[block])
      litIdx -= d->sizeTable[block++] + 1;

  const uint8_t *ptr = d->data + ((size_t)block << d->blockSize);

  const uint16_t *offset = d->offset;
  const uint64_t *base = d->base - d->minLen;
  const uint8_t *symLen = d->symLen;
  uint32_t sym, bitCnt = 0;

  uint64_t bitBuf = read_be_u64(ptr), pending = bitBuf;
  ptr += 8;
  for (;;) {
    int l = d->start[bitBuf >> (64 - STARTBITS)];
    while (bitBuf < base[l]) l++;
    sym = from_le_u16(offset[l]) + ((bitBuf - base[l]) >> (64 - l));
    if (litIdx < (int)symLen[sym] + 1)
      break;
    litIdx -= (int)symLen[sym] + 1;
    bitBuf = pending << l;
    bitCnt += l;
    pending = bitBuf | read_be_u64(ptr) >> (64 - bitCnt);
    ptr += (bitCnt >> 5) * sizeof(uint32_t);
    bitCnt &= 31;
  }

  const uint8_t *symPat = d->symPat;
  while (symLen[sym] != 0) {
    uint32_t w = read_le_u32(symPat + 3 * sym);
    int s1 = w & 0xfff;
    if (litIdx < (int)symLen[s1] + 1)
      sym = s1;
    else {
      litIdx -= (int)symLen[s1] + 1;
      sym = (w >> 12) & 0xfff;
    }
  }

  return &symPat[3 * sym];
}

static const uint8_t *decompress_pairs(const struct PairsData *d, size_t idx)
{
  switch (d->comprType) {
  case 0:
    return decompress_huff(d, idx);
  case 1:
    return decompress_rans(d, idx);
  default:
    return d->constValue;
  }
}

INLINE int probe_table(TB_Position *pos, const int s, int *result,
    const int type)
{
  // Disable DTM for now.
  if (type == DTM) {
    *result = FAIL;
    return 0;
  }

  // Test for KvK.
  if (type == WDL && TB_bare_kings(pos))
    return 0;

  // Obtain the position's material-signature key.
  uint64_t key = TB_material_key(pos);

  int hashIdx = key >> (64 - TB_HASHBITS);
  while (tbHash[hashIdx].key && tbHash[hashIdx].key != key)
    hashIdx = (hashIdx + 1) & ((1 << TB_HASHBITS) - 1);
  if (!tbHash[hashIdx].ptr) {
    *result = FAIL;
    return 0;
  }

  struct TbEntry *entry = tbHash[hashIdx].ptr;
  if ((type == DTM && !entry->hasDtm) || (type == DTZ && !entry->hasDtz)) {
    *result = FAIL;
    return 0;
  }

  struct Tbase *tb;

  // Use double-checked locking to reduce locking overhead.
  if (!(tb = atomic_load_explicit(&entry->tbase[type], memory_order_acquire))) {
    LOCK(mutex);
    if (!(tb = atomic_load_explicit(&entry->tbase[type], memory_order_relaxed)))
    {
      char str[16], str2[16];
      TB_material_string(pos, str);
      if (entry->key != key) { // KRvKQ -> KQvKR
        char *s = strchr(str, 'v');
        sprintf(str2, "%sv%.*s", s + 1, (int)(s - str), str);
      }
      if (!(tb = init_tb(entry, entry->key == key ? str : str2, type))) {
        tbHash[hashIdx].ptr = NULL; // Mark as deleted.
        *result = FAIL;
        UNLOCK(mutex);
        return 0;
      }
      atomic_store_explicit(&entry->tbase[type], tb, memory_order_release);
    }
    UNLOCK(mutex);
  }

  if (   (type == DTM || (type == DTZ && tb->layout <= LT_PIECE_KK))
      && (tb->distFormat & WIN_OR_LOSS)
      && (bool)(tb->distFormat & WIN_ONLY) != (s > 0))
  {
    // A 1-ply search is needed.
    *result = CHANGE_STM;
    return 0;
  }

  uint8_t p[TB_PIECES];
  bool flip = !entry->symmetric ? (key != entry->key) != tb->flipped
                                : !TB_white_to_move(pos);
  bool btm_side = TB_white_to_move(pos) == flip;

  if (tb->layout <= LT_PAWN_RANK) {

    uint64_t idx;
    struct EncInfo *ei;
    struct TbTable *table;

    if (!entry->hasPawns) {
      int t =  type == WDL ? btm_side
             : !(tb->distFormat & WTM_OR_BTM) ? btm_side : 0;
      table = atomic_load_explicit(&tb->table[t], memory_order_relaxed);
      if (type == DTZ) {
        struct DtzTable *dtz = (struct DtzTable *)table;
        if ((dtz->dtzFlags & 1) != btm_side && !entry->symmetric) {
          *result = CHANGE_STM;
          return 0;
        }
      }
      ei = &table->ei;
      TB_list_squares(pos, ei->pieces, flip, p);
      idx = encode_piece(p, ei, entry);
    } else {
      table = atomic_load_explicit(&tb->table[0], memory_order_relaxed);
      ei = &table->ei;
      TB_list_squares(pos, ei->pieces, flip, p);
      int t = leading_pawn(p, entry, tb->layout);
      t +=  !(btm_side && (tb->distFormat & TWO_SIDED)) ? 0
          : tb->layout == LT_PAWN_FILE ? 4 : 6;
      table = atomic_load_explicit(&tb->table[t], memory_order_relaxed);
      if (type == DTZ) {
        struct DtzTable *dtz = (struct DtzTable *)table;
        if ((dtz->dtzFlags & 1) != btm_side && !entry->symmetric) {
          *result = CHANGE_STM;
          return 0;
        }
      }
      ei = &table->ei;
      TB_list_squares(pos, ei->pieces, flip, p);
      // Bring the leading pawn back to the front.
      leading_pawn(p, entry, tb->layout);
      idx = type != DTM ? encode_pawn_f(p, ei, entry)
                        : encode_pawn_r(p, ei, entry);
    }

    TB_ProbeCount[type]++;

    const uint8_t *w = decompress_pairs(table->precomp, idx);

    if (type == WDL)
      return (int)w[0] - 2;

    int v = read_le_u16(w) & 0xfff;

    if (type == DTM) {
      struct DtmTable *dtm = (struct DtmTable *)table;
      if (!(tb->distFormat & WIN_OR_LOSS))
        v = from_le_u16(dtm->dtmMap[dtm->dtmMapIdx[s] + v]);
    }

    if (type == DTZ) {
      struct DtzTable *dtz = (struct DtzTable *)table;
      if (dtz->dtzFlags & 2) {
        int m = WdlToMap[s + 2];
        if (!(dtz->dtzFlags & 16))
          v = dtz->dtzMap[dtz->dtzMapIdx[m] + v];
        else
          v = dtz->dtzMap16[dtz->dtzMapIdx[m] + v];
      }
      if (!(dtz->dtzFlags & PAFlags[s + 2]) || (s & 1))
        v *= 2;
    }

    return v;

  } else { /* PIECE_K || PIECE_KK || PAWN_P || PAWN_PK || PAWN_PvP || PAWN_PP */

    if (   (type == DTM || (type == DTZ && tb->layout <= LT_PIECE_KK))
        && (tb->distFormat & WTM_OR_BTM)
        && (bool)(tb->distFormat & WTM_ONLY) == btm_side)
    {
      *result = CHANGE_STM;
      return 0;
    }

    struct TbTable2 *table;
    TB_list_squares(pos, tb->pt, flip, p);

    int t, tsq;
    uint64_t idx = 0;
    Bitboard occ;
    if (tb->layout <= LT_PIECE_KK) {

      if (tb->layout == LT_PIECE_K && btm_side)
        Swap(p[0], p[1]);

      // Normalize the pawnless position.
      uint8_t mask = MirrorMask[p[0]];
      for (int i = 0; i < entry->num; i++)
        p[i] ^= mask;

      if (FlipTest[p[0]][p[1]])
        for (int i = 0; i < entry->num; i++)
          p[i] = FlipDiag[p[i]];

      tsq = tb->layout == LT_PIECE_K ? Triangle[p[0]] : KKMap[p[0]][p[1]];

      occ = bit(p[0]) | bit(p[1]);

      t =  (   (type == WDL && !entry->symmetric)
            || (type != WDL && (tb->distFormat & TWO_SIDED)))
         ? 2 * tsq + btm_side : tsq;

    } else if (tb->layout <= LT_PAWN_PK) {

      // Normalize the single-pawn position.
      if (p[2] & 0x04)
        for (int i = 0; i < entry->num; i++)
          p[i] ^= 0x07;

      if (tb->layout == LT_PAWN_P) {
        occ = bit(p[2]);
        tsq = Flap[0][p[2]];
      } else {
        if (btm_side)
          Swap(p[0], p[1]);
        tsq = Flap[0][p[2]] * 63 + p[0] - (p[0] > p[2]);
        occ = bit(p[0]) | bit(p[2]);
      }
      t = !entry->symmetric ? 2 * tsq + btm_side : tsq;

    } else {
      t = tsq = 0;
      occ = 0;
      // LT_PAWN_PP and LT_PAWN_PvP
    }

    if (!(table = atomic_load_explicit(&tb->table[t], memory_order_acquire))) {
      LOCK(mutex);
      if (!(table = atomic_load_explicit(&tb->table[t], memory_order_relaxed)))
      {
        table = init_new_table(entry, tb, type, t, tsq);
        atomic_store_explicit(&tb->table[t], table, memory_order_release);
      }
      UNLOCK(mutex);
    }

    if (type != WDL && (uintptr_t)table == 1) {
      *result = CHANGE_STM;
      return 0;
    }

    if (!table->precomp) {
      struct TbTableConst *tbl = (struct TbTableConst *)table;
      if (    type != WDL
          && tbl->distFormat
          && (bool)(tbl->distFormat & WIN_ONLY) != (s > 0))
        *result = CHANGE_STM;
      return (int)((struct TbTableConst *)table)->constVal
        + (type == WDL ? -2 : 0);
    }

    if (type == DTM) {
      struct DtmTable2 *dtm = (struct DtmTable2 *)table;
      if (dtm->distFormat && (bool)(dtm->distFormat & WIN_ONLY) != (s > 0)) {
        *result = CHANGE_STM;
        return 0;
      }
    }

    if (type == DTZ) {
      struct DtzTable2 *dtz = (struct DtzTable2 *)table;
      if (dtz->distFormat && (bool)(dtz->distFormat & WIN_ONLY) != (s > 0)) {
        *result = CHANGE_STM;
        return 0;
      }
    }

    // Calculate index.
    if (tb->layout != LT_PIECE_KK || tsq < 441) {
      static const int extra[] = { 1, 0, 2, 1, 0, 0 };
      int numsets =  entry->numsets + extra[tb->layout - LT_PIECE_K];
      for (int k = 0; k < numsets; k++) {
        size_t s = 0;
        if (table->mult[k] == 0)
          s = Off10[tsq][p[1]];
        else {
          int m = table->first[k];
          sort_squares(table->mult[k], &p[m]);
          Bitboard occ2 = occ;
          for (int i = 0; i < table->mult[k]; i++, m++) {
            int rank = rank_among_free(p[m], occ);
            occ2 |= bit(p[m]);
            s += Binomial[i + 1][rank];
          }
          occ = occ2;
        }
        idx = idx * table->factor[k] + s;
      }
    } else {
      idx = rank_reflection(p, occ, entry->numsets, table);
    }

    TB_ProbeCount[type]++;

    const uint8_t *w = decompress_pairs(table->precomp, idx);

    if (type == WDL)
      return (int)w[0] - 2;

    int v = read_le_u16(w) & 0xfff;

    if (type == DTM) {
      struct DtmTable2 *dtm = (struct DtmTable2 *)table;
      if (dtm->mapped)
        v = from_le_u16(dtm->dtmMap[dtm->dtmMapIdx[s] + v]);
    }

    if (type == DTZ) {
      struct DtzTable2 *dtz = (struct DtzTable2 *)table;
      if (dtz->mapped) {
        int m = WdlToMap[s + 2];
        v = dtz->mapped == 1 ? dtz->dtzMap  [dtz->dtzMapIdx[m] + v]
                             : dtz->dtzMap16[dtz->dtzMapIdx[m] + v];
      }
      if (s & 1)
        v *= 2;
    }

    return v;
  }
}

static NOINLINE int probe_wdl_table(TB_Position *pos, int *result)
{
  return probe_table(pos, 0, result, WDL);
}

static NOINLINE int probe_dtm_table(TB_Position *pos, bool won, int *result)
{
  return probe_table(pos, won, result, DTM);
}

static NOINLINE int probe_dtz_table(TB_Position *pos, int wdl, int *result)
{
  return probe_table(pos, wdl, result, DTZ);
}

// probe_ab() is never called for positions with en passant captures.
static int probe_ab(TB_Position *pos, int alpha, int beta, int *result)
{
  assert(!TB_has_en_passant(pos));

  int num = TB_generate_captures(pos);

  for (int m = 0; m < num; m++) {
    if (!TB_do_move(pos, m))
      continue;
    int v = -probe_ab(pos, -beta, -alpha, result);
    TB_undo_move(pos, m);
    if (*result == FAIL) return 0;
    alpha = max(alpha, v);
    if (alpha >= beta)
      return alpha;
  }

  int v = probe_wdl_table(pos, result);

  return max(alpha, v);
}

static int probe_wdl(TB_Position *pos, int *result)
{
  *result = OK;

  int num = TB_generate_captures(pos);
  int bestCap = -3, bestEp = -3;

  // We do capture resolution, letting bestCap keep track of the best
  // capture without ep rights and letting bestEp keep track of still
  // better ep captures if they exist.

  for (int m = 0; m < num; m++) {
    if (!TB_do_move(pos, m))
      continue;
    int v = -probe_ab(pos, -2, -bestCap, result);
    TB_undo_move(pos, m);
    if (*result == FAIL)
      return 0;
    if (v > bestCap) {
      if (v == 2) {
        *result = ZEROING_IS_BEST;
        return 2;
      }
      if (!TB_move_is_ep(pos, m))
        bestCap = v;
      else
        bestEp = max(bestEp, v);
    }
  }

  // Since there is no winning capture, a non-capture might be the best
  // move. Therefore we need to probe the table.
 
  int v = probe_wdl_table(pos, result);
  if (*result == FAIL) return 0;

  // Now max(v, bestCap) is the WDL value of the position without ep rights.
  // If the position without ep rights is not stalemate or no ep captures
  // exist, then the value of the position is max(v, bestCap, bestEp).
  // If the position without ep rights is stalemate and bestEp > -3,
  // then the value of the position is bestEp (and we will have v == 0).

  bool legalCaps = bestCap > -3;

  if (bestEp > bestCap) {
    if (bestEp > v) { // ep capture (possibly blessed losing) is best.
      *result = ZEROING_IS_BEST;
      return bestEp;
    }
    bestCap = bestEp;
  }

  // Now max(v, bestCap) is the WDL value of the position unless
  // the position without ep rights is stalemate and bestEp > -3.

  if (bestCap >= v) {
    // No need to test for the stalemate case here: either there are
    // non-ep captures, or bestCap == bestEp >= v anyway.
    if (bestCap > 0)
      *result = ZEROING_IS_BEST;
    return bestCap;
  }

  // Now handle the stalemate case.
  if (bestEp > -3 && v == 0 && !legalCaps) {
    // Check for stalemate in the position without ep rights.
    // We already know that there are no legal non-ep captures.
    if (TB_in_check(pos))
      goto no_stalemate;
    int num = TB_generate_quiets(pos, 0);
    for (int m = 0; m < num; m++)
      if (TB_move_is_legal(pos, m))
        goto no_stalemate;

    // Stalemate detected -> ep capture is best.
    *result = ZEROING_IS_BEST;
    return bestEp;
  }

no_stalemate:
  // Stalemate / en passant not an issue, so v is the correct value.
  return v;
}

// Probe the WDL table for a particular position.
//
// The caller should verify that the probe was successful by checking
// the value of *success.
//
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
int TB_probe_wdl(TB_Position *pos, bool *success)
{
  int result, v = probe_wdl(pos, &result);
  *success = result != FAIL;
  return v;
}

static int probe_dtm_win(TB_Position *pos, int alpha, int beta, int *result);

// Probe a position known to lose.
// Losing DTM values are negative -> we want to minimize them.
static int probe_dtm_loss(TB_Position *pos, int alpha, int beta, int *result)
{
  bool legalCaps = false, legalEpCaps = false;

  int v, num = TB_generate_captures(pos);

  for (int m = 0; m < num; m++) {
    if (!TB_do_move(pos, m))
      continue;
    v = -probe_dtm_win(pos, max(1, -beta), -alpha, result);
    beta = min(beta, v);
    TB_undo_move(pos, m);
    if (TB_move_is_ep(pos, m))
      legalEpCaps = true;
    else
      legalCaps = true;
    if (beta <= alpha || *result == FAIL)
      return beta;
  }

  // If there are en passant captures, the position without ep rights may
  // be a draw by stalemate. If it is, we must avoid probing the DTM table.
  if (legalEpCaps && !legalCaps) {
    num = TB_generate_quiets(pos, 0);
    for (int m = 0; m < num; m++)
      if (TB_move_is_legal(pos, m))
        goto no_stalemate;
    return beta;
  }

no_stalemate:
  v = -probe_dtm_table(pos, false, result);
  if (*result != CHANGE_STM)
    return min(beta, v);

  *result = OK;
  if (!legalEpCaps || legalCaps)
    num = TB_generate_quiets(pos, 0);
  for (int m = 0; m < num; m++) {
    if (!TB_do_move(pos, m))
      continue;
    v = -probe_dtm_win(pos, max(1, -beta), -alpha, result);
    beta = min(beta, v);
    TB_undo_move(pos, m);
    if (beta <= alpha || *result == FAIL)
      return beta;
  }

  return beta;
}

// Probe a position known to win.
// Winning DTM values are positive -> we want to minimize them.
static int probe_dtm_win(TB_Position *pos, int alpha, int beta, int *result)
{
  if (beta <= alpha)
    return beta;

  bool legalCaps = false, legalEpCaps = false;

  int v, num = TB_generate_captures(pos);

  for (int m = 0; m < num; m++) {
    if (!TB_do_move(pos, m))
      continue;
    if (probe_ab(pos, -1, 0, result) < 0 && *result != FAIL) {
      v = 1 - probe_dtm_loss(pos, 1 - beta, 1 - alpha, result);
      beta = min(beta, v);
    }
    TB_undo_move(pos, m);
    if (TB_move_is_ep(pos, m))
      legalEpCaps = true;
    else
      legalCaps = true;
    if (beta <= alpha || *result == FAIL)
      return beta;
  }

  // If there are en passant captures, the position without ep rights may
  // be a draw by stalemate. If it is, we must avoid probing the DTM table.
  if (legalEpCaps && !legalCaps) {
    num = TB_generate_quiets(pos, 0);
    for (int m = 0; m < num; m++)
      if (TB_move_is_legal(pos, m))
        goto no_stalemate;
    return beta;
  }

no_stalemate:
  v = -probe_dtm_table(pos, true, result);
  if (*result != CHANGE_STM)
    return min(beta, v);

  *result = OK;
  if (!legalEpCaps || legalCaps)
    num = TB_generate_quiets(pos, 0);
  for (int m = 0; m < num; m++) {
    if (!TB_do_move(pos, m))
      continue;
    if (   (TB_has_en_passant(pos) ? probe_wdl(pos, result)
                                   : probe_ab(pos, -1, 0, result)) < 0
        && *result != FAIL)
    {
      int v = 1 - probe_dtm_loss(pos, 1 - beta, 1 - alpha, result);
      beta = min(beta, v);
    }
    TB_undo_move(pos, m);
    if (beta <= alpha || *result == FAIL)
      break;
  }

  return beta;
}

// Probe the DTM table for a non-drawn position.
// 'won' must be true if the position is a win or cursed win and
// false if the position is a loss or blessed loss.
// The value returned is the number of moves to mate. Positive if winning,
// negative if losing.
int TB_probe_dtm(TB_Position *pos, bool won, bool *success)
{
  int result = OK;
  int dtm = won ? probe_dtm_win (pos, 1, 10000, &result)
                : probe_dtm_loss(pos, -10000, 0, &result);
  *success = result != FAIL;
  return dtm;
}

// Test whether the current position is a DTM-optimal successor of the
// parent position. The (signed) dtm value passed must be the expected
// DTM value of a DTM-optimal succesor. If the parent position has DTM
// value d, then pass (d > 0) - d.
bool TB_probe_dtm_test(TB_Position *pos, int dtm, bool *success)
{
  int result = OK;

  // If dtm > 0, we assume the position is winning (which is the case if
  // the parent position is losing).
  int v =  dtm > 0 ? probe_dtm_win(pos, dtm - 1, dtm, &result) >= dtm
         : (   (TB_has_en_passant(pos) ? probe_wdl(pos, &result)
                                       : probe_ab(pos, -1, 0, &result)) >= 0
            || result == FAIL) ? false
         : probe_dtm_loss(pos, dtm - 1, dtm, &result) >= dtm;
  *success = result != FAIL;

  return v;
}

static int WdlToDtz[] = { -1, -101, 0, 101, 1 };

// This function assumes probe_wdl() has been called and *result has
// been checked for ZEROING_IS_BEST (or != OK). In other words, when this
// function is called, there is no winning capture, and ep capture is not
// the best move.
int probe_dtz(TB_Position *pos, int wdl, int *result)
{
  int num = 0;

  // If winning, check for a winning pawn move.
  if (wdl > 0) {
    // Generate all quiet moves including promotions.
    num = TB_generate_quiets(pos, 0);

    for (int m = 0; m < num; m++) {
      // We check only pawn moves here.
      if (!TB_move_is_pawn_move(pos, m))
        continue;
      if (!TB_do_move(pos, m))
        continue;
      // TODO: add alpha/beta bounds to next call
      int v = -probe_wdl(pos, result);
      TB_undo_move(pos, m);
      if (v == wdl || *result == FAIL)
        return WdlToDtz[wdl + 2];
    }
  }

  // If we are here, we know that the best move is not an ep capture.
  // In other words, the value of wdl corresponds to the WDL value of
  // the position without ep rights. It is therefore safe to probe the
  // DTZ table with the current value of wdl.

  int dtz = probe_dtz_table(pos, wdl, result);
  if (*result != CHANGE_STM)
    return WdlToDtz[wdl + 2] + ((wdl > 0) ? dtz : -dtz);

  *result = OK;
  // CHANGE_STM means we need to probe DTZ for the other side to move.
  int best = INT32_MAX;
  // If wdl > 0, we have already generated quiet moves.
  if (wdl < 0) {
    // If (blessed) loss, the worst case is a losing capture or pawn move
    // as the "best" move, meaning dtz is -1 or -101.
    // In case of mate, this will cause -1 to be returned.
    best = WdlToDtz[wdl + 2];
    // If wdl < 0, we still have to generate quiet moves.
    num = TB_generate_quiets(pos, 0);
  }

  for (int m = 0; m < num; m++) {
    // We can skip pawn moves. If wdl > 0, we already checked them, and
    // they were worse than wdl. If wdl < 0, the initial value
    // of best already takes account of them.
    if (TB_move_is_pawn_move(pos, m))
      continue;
    if (!TB_do_move(pos, m))
      continue;
    int wdl_next = probe_wdl(pos, result);
    // Only look further at move m if it does not worsen the position.
    if (wdl <= -wdl_next) {
      int v =  *result != OK ? -WdlToDtz[wdl_next + 2]
             : -probe_dtz(pos, wdl_next, result);
      if (v == 1 && TB_in_check(pos) && TB_no_legal_moves(pos))
        best = 1;
      else if (wdl > 0)
        best = min(best, v + 1);
      else
        best = min(best, v - 1);
    }
    TB_undo_move(pos, m);
    if (*result == FAIL)
      break;
  }

  return best;
}

// Probe the DTZ table for a particular position.
// If *success == true, the probe was successful.
// The return value is from the point of view of the side to move:
//         n < -100 : loss, but draw under 50-move rule
// -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
//         0        : draw
//     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
//   100 < n        : win, but draw under 50-move rule
//
// If the position mate, -1 is returned instead of 0.
//
// The return value n can be off by 1: a return value -n can mean a loss
// in n+1 ply and a return value +n can mean a win in n+1 ply. This
// cannot happen for tables with positions exactly on the "edge" of
// the 50-move rule.
//
// This means that if dtz > 0 is returned, the position is certainly
// a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
// picks moves that preserve dtz + 50-move-counter <= 99.
//
// If n = 100 immediately after a capture or pawn move, then the position
// is also certainly a win, and during the whole phase until the next
// capture or pawn move, the inequality to be preserved is
// dtz + 50-movecounter <= 100.
//
// In short, if a move is available resulting in dtz + 50-move-counter <= 99,
// then do not accept moves leading to dtz + 50-move-counter == 100.
//
int TB_probe_dtz(TB_Position *pos, bool *success)
{
  int result = OK;
  int wdl = probe_wdl(pos, &result);
  int dtz =  wdl == 0 || result != OK ? WdlToDtz[wdl + 2]
           : probe_dtz(pos, wdl, &result);
  *success = result != FAIL;
  return dtz;
}
