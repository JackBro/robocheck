/* **********************************************************
 * Copyright (c) 2010-2012 Google, Inc.  All rights reserved.
 * Copyright (c) 2007-2010 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "dr_api.h"
#include "drmemory.h"
#include "utils.h"
#include "shadow.h"
#include <limits.h> /* UINT_MAX */
#include <stddef.h>
#ifdef TOOL_DR_HEAPSTAT
# include "../drheapstat/staleness.h"
#endif

#include "readwrite.h" /* get_own_seg_base */

#ifdef TOOL_DR_MEMORY /* around whole shadow table */

/***************************************************************************
 * BITMAP SUPPORT
 */

typedef uint bitmap_t[];

/* 1 bit per byte */
#define BITMAP_UNIT     32
#define BITMAP_MASK(i)  (1 << ((i) % BITMAP_UNIT))
#define BITMAP_IDX(i)  ((i) / BITMAP_UNIT)

/* returns non-zero value if bit is set */
static inline bool
bitmap_test(bitmap_t bm, uint i)
{
    return bm[BITMAP_IDX(i)] & BITMAP_MASK(i);
}

static inline void
bitmap_set(bitmap_t bm, uint i)
{
    bm[BITMAP_IDX(i)] |= BITMAP_MASK(i);
}

static inline void
bitmap_clear(bitmap_t bm, uint i)
{
    bm[BITMAP_IDX(i)] &= ~BITMAP_MASK(i);
}

/* 2 bits of shadow per real byte: or 4 real bytes shadowed by one shadow byte */
#define BITMAPx2_UNIT     16 /* one uint shadows 16 real bytes */
#define BITMAPx2_SHIFT(i) (((i) % BITMAPx2_UNIT) * 2)
#define BITMAPx2_MASK(i)  (3 << BITMAPx2_SHIFT)
#define BITMAPx2_IDX(i)   ((i) / BITMAPx2_UNIT)
#define BLOCK_AS_BYTE_ARRAY_IDX(i) (BITMAPx2_IDX(i*sizeof(uint)))

/* returns the two bits corresponding to offset i */
static inline uint
bitmapx2_get(bitmap_t bm, uint i)
{
    LOG(6, "bitmapx2_get 0x%04x [%d] => "PFX" == %d\n", i, BITMAPx2_IDX(i),
          bm[BITMAPx2_IDX(i)], ((bm[BITMAPx2_IDX(i)] >> BITMAPx2_SHIFT(i)) & 3));
    return ((bm[BITMAPx2_IDX(i)] >> BITMAPx2_SHIFT(i)) & 3);
}

/* SHADOW_DWORD2BYTE() == "get_2bits" */
static inline uint
set_2bits_inline(uint orig, uint val, uint shift)
{
    ASSERT(val <= 3, "set_2bits usage error");
    ASSERT(shift % 2 == 0, "set_2bits usage error");
    orig &= (((0xfffffffc | val) << shift) | (~(0xffffffff << shift)));
    orig |= (val << shift);
    return orig;
}

/* SHADOW_DWORD2BYTE() == "get_2bits" */
uint
set_2bits(uint orig, uint val, uint shift)
{
    return set_2bits_inline(orig, val, shift);
}

static inline void
bitmapx2_set(bitmap_t bm, uint i, uint val)
{
    uint shift = BITMAPx2_SHIFT(i);
    ASSERT(val <= 3, "internal error");
    /* It's a pain to set 2 bits: first we have to clear in case either
     * of the two is a zero; then we have to set in case either is a 1.
     */
    LOG(6, "bitmapx2_set 0x%04x [%d] to %d: from "PFX" ", i, BITMAPx2_IDX(i),
          val, bm[BITMAPx2_IDX(i)]);
    bm[BITMAPx2_IDX(i)] = set_2bits_inline(bm[BITMAPx2_IDX(i)], val, shift);
    LOG(6, "to "PFX"\n", bm[BITMAPx2_IDX(i)]);
}

/* returns the byte corresponding to offset i */
static inline uint
bitmapx2_byte(bitmap_t bm, uint i)
{
    ASSERT(BITMAPx2_SHIFT(i) %8 == 0, "bitmapx2_byte: index not aligned");
    return (bm[BITMAPx2_IDX(i)] >> BITMAPx2_SHIFT(i)) & 0xff;
}

/* returns the uint corresponding to offset i */
static inline uint
bitmapx2_dword(bitmap_t bm, uint i)
{
    ASSERT(BITMAPx2_SHIFT(i) == 0, "bitmapx2_dword: index not aligned");
    return bm[BITMAPx2_IDX(i)];
}

/***************************************************************************
 * BYTE-TO-BYTE SHADOWING SUPPORT
 */

/* It would be simpler to use a char[] for shadow_block_t but for simpler
 * runtime parametrization we stick with uint[] and extract bytes.
 */

static inline void
bytemap_4to1_set(bitmap_t bm, uint i, uint val)
{
    char *bytes = (char *) bm;
    ASSERT(val <= UCHAR_MAX, "internal error");
    bytes[BLOCK_AS_BYTE_ARRAY_IDX(i)] = val;
}

/* returns the byte corresponding to offset i */
static inline uint
bytemap_4to1_byte(bitmap_t bm, uint i)
{
    char *bytes = (char *) bm;
    return bytes[BLOCK_AS_BYTE_ARRAY_IDX(i)];
}

/* returns the uint corresponding to offset i */
static inline uint
bytemap_4to1_dword(bitmap_t bm, uint i)
{
    ASSERT(BITMAPx2_SHIFT(i) == 0, "bytemap_4to1_dword: index not aligned");
    return bm[BITMAPx2_IDX(i)];
}

/***************************************************************************
 * MEMORY SHADOWING DATA STRUCTURES
 */

/* We divide the 32-bit address space uniformly into 16-bit units */
#define SHADOW_SPLIT_BITS 16

/* Holds shadow state for a 64K unit of memory (the basic allocation
 * unit size on Windows)
 */
#define ALLOC_UNIT (1 << (SHADOW_SPLIT_BITS))
typedef uint shadow_block_t[BITMAPx2_IDX(ALLOC_UNIT)];

/* 2 shadow bits per app byte */
#define SHADOW_GRANULARITY 4

/* Shadow state for 4GB address space
 * FIXME: drop top 1GB, or top 2GB if not /3GB, since only user space.
 * We allocate a full table with a slot for every possible 64K unit,
 * so we don't need to store tags, handle resize, or handle hash collisions.
 * Note that this arrangement is hardcoded into the inlined instrumentation
 * routines in fastpath.c.
 */
#define TABLE_ENTRIES (1 << (32 - (SHADOW_SPLIT_BITS)))
/* We store the displacement (shadow minus app) from the base to
 * shrink instrumentation size (PR 553724)
 */
ptr_int_t shadow_table[TABLE_ENTRIES];
#define TABLE_IDX(addr) (((ptr_uint_t)(addr) & 0xffff0000) >> (SHADOW_SPLIT_BITS))
#define ADDR_OF_BASE(table_idx) ((ptr_uint_t)(table_idx) << (SHADOW_SPLIT_BITS))

static void *shadow_lock;

/* PR 448701: special blocks for all-identical 64K chunks */
static shadow_block_t *special_unaddressable;
static shadow_block_t *special_undefined;
static shadow_block_t *special_defined;
static shadow_block_t *special_bitlevel;

#define SHADOW_BLOCK_ALLOC_SZ (sizeof(shadow_block_t) + 2*SHADOW_REDZONE_SIZE)

#ifdef STATISTICS
uint shadow_block_alloc;
/* b/c of PR 580017 we no longer free any non-specials so this is always 0 */
uint shadow_block_free;
uint num_special_unaddressable;
uint num_special_undefined;
uint num_special_defined;
#endif

/* these are filled in in shadow_table_init() b/c the consts vary dynamically */
uint val_to_dword[4];
uint val_to_qword[4];
uint val_to_dqword[4];

const char * const shadow_name[] = {
    "defined",
    "unaddressable",
    "bitlevel",
    "undefined",
    "mixed", /* SHADOW_MIXED */
    "unknown", /* SHADOW_UNKNOWN */
};

static inline bool
block_is_special(shadow_block_t *block)
{
    return (block == special_unaddressable ||
            block == special_undefined ||
            block == special_defined ||
            block == special_bitlevel);
}

static bool
is_in_special_shadow_block_helper(app_pc pc, shadow_block_t *block)
{
    return (pc >= (app_pc) block && pc < (((app_pc)(block)) + sizeof(*block)));
}

bool
is_in_special_shadow_block(app_pc pc)
{
    return (special_unaddressable != NULL &&
            (is_in_special_shadow_block_helper(pc, special_unaddressable) ||
             is_in_special_shadow_block_helper(pc, special_undefined) ||
             is_in_special_shadow_block_helper(pc, special_defined) ||
             is_in_special_shadow_block_helper(pc, special_bitlevel)));
}

static shadow_block_t *
val_to_special(uint val)
{
    if (val == SHADOW_UNADDRESSABLE)
        return special_unaddressable;
    if (val == SHADOW_UNDEFINED)
        return special_undefined;
    if (val == SHADOW_DEFINED)
        return special_defined;
    if (val == SHADOW_DEFINED_BITLEVEL)
        return special_bitlevel;
    ASSERT(false, "internal shadow val error");
    return NULL;
}

static shadow_block_t *
create_special_block(uint dwordval)
{
    IF_DEBUG(bool ok;)
    shadow_block_t *block = (shadow_block_t *)
        nonheap_alloc(SHADOW_BLOCK_ALLOC_SZ, DR_MEMPROT_READ|DR_MEMPROT_WRITE,
                      HEAPSTAT_SHADOW);
    LOG(2, "special %x = "PFX"\n", dwordval, block);
    /* Set the redzone to bitlevel so we always exit (if unaddr we won't
     * exit on a push)
     */
    memset(block, SHADOW_DWORD_BITLEVEL, SHADOW_REDZONE_SIZE);
    memset(((byte*)block) + SHADOW_BLOCK_ALLOC_SZ - SHADOW_REDZONE_SIZE,
           SHADOW_DWORD_BITLEVEL, SHADOW_REDZONE_SIZE);
    block = (shadow_block_t *) (((byte*)block) + SHADOW_REDZONE_SIZE);
    memset(block, dwordval, sizeof(*block));
    IF_DEBUG(ok = )
        dr_memory_protect(block, SHADOW_BLOCK_ALLOC_SZ, DR_MEMPROT_READ);
    ASSERT(ok, "-w failed: will have inconsistencies in shadow data");
    return block;
}

/* FIXME: share w/ staleness.c */
/* if past init, caller must hold shadow_lock */
static void
set_shadow_table(uint idx, shadow_block_t *block)
{
    /* We store the displacement (shadow minus app) (PR 553724) */
    shadow_table[idx] = ((ptr_int_t)block) - (ADDR_OF_BASE(idx) / SHADOW_GRANULARITY);
    LOG(3, "setting shadow table idx %d for block "PFX" to "PFX"\n",
        idx, block, shadow_table[idx]);
}

static shadow_block_t *
get_shadow_table(uint idx)
{
    /* We store the displacement (shadow minus app) (PR 553724) */
    ASSERT(options.shadowing, "shadowing disabled");
    return (shadow_block_t *)
        (shadow_table[idx] + (ADDR_OF_BASE(idx) / SHADOW_GRANULARITY));
}

static void
shadow_table_init(void)
{
    uint i;

    val_to_dword[0] = SHADOW_DWORD_DEFINED;
    val_to_dword[1] = SHADOW_DWORD_UNADDRESSABLE;
    val_to_dword[2] = SHADOW_DWORD_BITLEVEL;
    val_to_dword[3] = SHADOW_DWORD_UNDEFINED;

    val_to_qword[0] = SHADOW_QWORD_DEFINED;
    val_to_qword[1] = SHADOW_QWORD_UNADDRESSABLE;
    val_to_qword[2] = SHADOW_QWORD_BITLEVEL;
    val_to_qword[3] = SHADOW_QWORD_UNDEFINED;

    val_to_dqword[0] = SHADOW_DQWORD_DEFINED;
    val_to_dqword[1] = SHADOW_DQWORD_UNADDRESSABLE;
    val_to_dqword[2] = SHADOW_DQWORD_BITLEVEL;
    val_to_dqword[3] = SHADOW_DQWORD_UNDEFINED;

    special_unaddressable = create_special_block(SHADOW_DWORD_UNADDRESSABLE);
    special_undefined = create_special_block(SHADOW_DWORD_UNDEFINED);
    special_defined = create_special_block(SHADOW_DWORD_DEFINED);
    special_bitlevel = create_special_block(SHADOW_DWORD_BITLEVEL);
    for (i = 0; i < TABLE_ENTRIES; i++)
        set_shadow_table(i, special_unaddressable);
    shadow_lock = dr_mutex_create();
}

static void
shadow_table_exit(void)
{
    uint i;
    shadow_block_t *block;
    for (i = 0; i < TABLE_ENTRIES; i++) {
        block = get_shadow_table(i);
        if (!block_is_special(block)) {
            global_free(((byte*)block) - SHADOW_REDZONE_SIZE,
                        SHADOW_BLOCK_ALLOC_SZ, HEAPSTAT_SHADOW);
        }
    }
    nonheap_free(((byte*)special_unaddressable) - SHADOW_REDZONE_SIZE,
                 SHADOW_BLOCK_ALLOC_SZ, HEAPSTAT_SHADOW);
    nonheap_free(((byte*)special_undefined) - SHADOW_REDZONE_SIZE,
                 SHADOW_BLOCK_ALLOC_SZ, HEAPSTAT_SHADOW);
    nonheap_free(((byte*)special_defined) - SHADOW_REDZONE_SIZE,
                 SHADOW_BLOCK_ALLOC_SZ, HEAPSTAT_SHADOW);
    nonheap_free(((byte*)special_bitlevel) - SHADOW_REDZONE_SIZE,
                 SHADOW_BLOCK_ALLOC_SZ, HEAPSTAT_SHADOW);
    dr_mutex_destroy(shadow_lock);
}

size_t
get_shadow_block_size(void)
{
    return sizeof(shadow_block_t);
}

bool
shadow_get_special(app_pc addr, uint *val)
{
    shadow_block_t *block = get_shadow_table(TABLE_IDX(addr));
    if (val != NULL)
        *val = shadow_get_byte(addr);
    return block_is_special(block);
}

/* Returns false already non-special (can't go back: see below) */
static bool
shadow_set_special(app_pc addr, uint val)
{
    /* PR 580017: We cannot replace a non-special with a special: more
     * accurately, we cannot free a non-special b/c we could have a
     * use-after-free in our own code.  Rather than a fancy delayed deletion
     * algorithm, or having specials be files that are mmapped at the same
     * address as non-specials thus supported swapping back and forth w/o
     * changing the address (which saves pagefile but not address space: so
     * should do it for 64-bit), we only replace specials with other specials.
     * This still covers the biggest win for specials, the initial unaddr and
     * the initial libraries.  Note that we do not want large stack
     * allocs/deallocs to use specials anyway as the subsequent faults are perf
     * hits (observed in gcc).
     */
    shadow_block_t *block;
    bool res = false;
    /* grab lock to synch w/ special-to-non-special transition */
    dr_mutex_lock(shadow_lock);
    block = get_shadow_table(TABLE_IDX(addr));
    if (block_is_special(block)) {
        set_shadow_table(TABLE_IDX(addr), val_to_special(val));
        res = true;
#ifdef STATISTICS
        if (val == SHADOW_UNADDRESSABLE)
            STATS_INC(num_special_unaddressable);
        if (val == SHADOW_UNDEFINED)
            STATS_INC(num_special_undefined);
        if (val == SHADOW_DEFINED)
            STATS_INC(num_special_defined);
#endif
    }
    /* else, leave non-special */
    dr_mutex_unlock(shadow_lock);
    return res;
}

/* Returns the two bits for the byte at the passed-in address */
uint
shadow_get_byte(app_pc addr)
{
    shadow_block_t *block = get_shadow_table(TABLE_IDX(addr));
    ptr_uint_t idx = ((ptr_uint_t)addr) % ALLOC_UNIT;
    if (!MAP_4B_TO_1B)
        return bitmapx2_get(*block, idx);
    else
        return bytemap_4to1_byte(*block, idx);
}

uint
shadow_get_dword(app_pc addr)
{
    shadow_block_t *block = get_shadow_table(TABLE_IDX(addr));
    ptr_uint_t idx = ((ptr_uint_t)ALIGN_BACKWARD(addr, 4)) % ALLOC_UNIT;
    if (!MAP_4B_TO_1B)
        return bitmapx2_byte(*block, idx);
    else /* just return byte */
        return bytemap_4to1_byte(*block, idx);
}

/* Sets the two bits for the byte at the passed-in address */
void
shadow_set_byte(app_pc addr, uint val)
{
    shadow_block_t *block = get_shadow_table(TABLE_IDX(addr));
    ASSERT(val <= 4, "invalid shadow value");
    /* Note that we can come here for SHADOW_SPECIAL_DEFINED, for mmap
     * regions used for calloc (we mark headers as unaddressable), etc.
     */
    if (block_is_special(block)) {
        uint blockval = shadow_get_byte(addr);
        uint dwordval = val_to_dword[blockval];
        /* Avoid replacing special on nop write */
        if (val == shadow_get_byte(addr)) {
            LOG(5, "writing "PFX" => nop (already special %d)\n", addr, val);
            return;
        }
        /* check again with lock.  we only need synch on the special-to-non-special
         * transition (we never go the other way).
         *  can still have races between app access and shadow update,
         * but if race between thread shadow updates there's a race in the app.
         */
        dr_mutex_lock(shadow_lock);
        block = get_shadow_table(TABLE_IDX(addr));
        if (block_is_special(block)) {
            ASSERT(val_to_special(blockval) == block, "internal error");
            LOG(2, "replacing shadow special "PFX" block for write @"PFX" %d\n",
                block, addr, val);
            block = (shadow_block_t *) global_alloc(SHADOW_BLOCK_ALLOC_SZ,
                                                    HEAPSTAT_SHADOW);
            ASSERT(block != NULL, "internal error");
            /* Set the redzone to bitlevel so we always exit (if unaddr we won't
             * exit on a push)
             */
            memset(block, SHADOW_DWORD_BITLEVEL, SHADOW_REDZONE_SIZE);
            memset(((byte*)block) + SHADOW_BLOCK_ALLOC_SZ - SHADOW_REDZONE_SIZE,
                   SHADOW_DWORD_BITLEVEL, SHADOW_REDZONE_SIZE);
            block = (shadow_block_t *) (((byte*)block) + SHADOW_REDZONE_SIZE);
            ASSERT(ALIGNED(block, 4), "esp fastpath assumes block aligned to 4");
            STATS_INC(shadow_block_alloc);
            memset(block, dwordval, sizeof(*block));
            set_shadow_table(TABLE_IDX(addr), block);
        }
        dr_mutex_unlock(shadow_lock);
    }
    LOG(5, "writing "PFX" ("PIFX") => %d\n", addr, ((ptr_uint_t)addr) % ALLOC_UNIT, val);
    if (!MAP_4B_TO_1B)
        bitmapx2_set(*block, ((ptr_uint_t)addr) % ALLOC_UNIT, val);
    else
        bytemap_4to1_set(*block, ((ptr_uint_t)addr) % ALLOC_UNIT, val);
}

byte *
shadow_translation_addr(app_pc addr)
{
    shadow_block_t *block = get_shadow_table(TABLE_IDX(addr));
    size_t mod = ((ptr_uint_t)addr) % ALLOC_UNIT;
    return ((byte *)(*block)) + BLOCK_AS_BYTE_ARRAY_IDX(mod);
}

byte *
shadow_translation_addr_using_offset(app_pc addr, byte *target)
{
    shadow_block_t *block = get_shadow_table(TABLE_IDX(addr));
    LOG(2, "for addr="PFX" target="PFX" => block "PFX" offs "PFX"\n",
        addr, target, block,
        (((ptr_uint_t)target) & ((ALLOC_UNIT -1) * sizeof(uint) / BITMAPx2_UNIT)));
    return ((byte *)(*block)) +
        (((ptr_uint_t)target) & ((ALLOC_UNIT -1) * sizeof(uint) / BITMAPx2_UNIT));
}

/* Returns a pointer to an always-bitlevel shadow block */
byte *
shadow_bitlevel_addr(void)
{
    /* For PR 493257 we need this pointer to have redzone bitlevel on both sides */
    return (byte *) special_bitlevel + SHADOW_REDZONE_SIZE;
}

byte *
shadow_replace_special(app_pc addr)
{
    shadow_block_t *block;
    size_t mod = ((ptr_uint_t)addr) % ALLOC_UNIT;
    uint val = shadow_get_byte(addr);
    /* kind of a hack to get shadow_set_byte to replace, since it won't re-instate */
    shadow_set_byte(addr, (val == SHADOW_DEFINED) ? SHADOW_UNDEFINED : SHADOW_DEFINED);
    shadow_set_byte(addr, val);
    block = get_shadow_table(TABLE_IDX(addr));
    return ((byte *)(*block)) + BLOCK_AS_BYTE_ARRAY_IDX(mod);
}

/* Sets the two bits for each byte in the range [start, end) */
void
shadow_set_range(app_pc start, app_pc end, uint val)
{
    app_pc pc = start;
    ASSERT(options.shadowing, "shadowing disabled");
    ASSERT(val <= 4, "invalid shadow value");
    LOG(2, "set range "PFX"-"PFX" => "PIFX"\n", start, end, val);
    DOLOG(2, {
        if (end - start > 0x10000000)
            LOG(2, "WARNING: set range of very large range "PFX"-"PFX"\n", start, end);
    });
    while (pc < end && pc >= start/*overflow*/) {
        shadow_block_t *block = get_shadow_table(TABLE_IDX(pc));
        bool is_special = block_is_special(block);
        if (is_special && ALIGNED(pc, ALLOC_UNIT) && (end - pc) >= ALLOC_UNIT) {
            if (shadow_set_special(pc, val))
                pc += ALLOC_UNIT;
            else {
                /* a race and special was replaced w/ non-special: so re-do */
                ASSERT(!shadow_get_special(pc, NULL), "non-special never reverts");
            }
        } else {
            if (!is_special && ALIGNED(pc, SHADOW_GRANULARITY)) {
                app_pc block_end = (app_pc) ALIGN_FORWARD(pc + 1, ALLOC_UNIT);
                if (block_end > start/*overflow*/) {
                    app_pc set_end = (block_end < end ? block_end : end);
                    set_end = (app_pc) ALIGN_BACKWARD(set_end, SHADOW_GRANULARITY);
                    if (set_end > pc) {
                        uint *array_start =
                            &(*block)[BITMAPx2_IDX(((ptr_uint_t)pc) % ALLOC_UNIT)];
                        byte *memset_start = ((byte *)array_start) +
                            (((ptr_uint_t)pc) % BITMAPx2_UNIT) / SHADOW_GRANULARITY;
                        memset(memset_start, val_to_dword[val],
                               (set_end - pc) / SHADOW_GRANULARITY);
                        LOG(3, "\tmemset "PFX"-"PFX"\n", pc, set_end);
                        pc = set_end;
                        continue;
                    }
                }
            }
            shadow_set_byte(pc, val);
            LOG(4, "\tset byte "PFX"\n", pc);
            pc++;
        }
    }
}

/* Copies the values for each byte in the range [old_start, old_start+end) to
 * [new_start, new_start+size).  The two ranges can overlap.
 */
void
shadow_copy_range(app_pc old_start, app_pc new_start, size_t size)
{
    app_pc pc = old_start;
    app_pc new_pc;
    uint val;
    LOG(2, "copy range "PFX"-"PFX" to "PFX"-"PFX"\n",
         old_start, old_start+size, new_start, new_start+size);
    /* We don't check what the current value of the destination is b/c
     * it could be anything: realloc can shrink, grow, overlap, etc.
     */
    while (pc < old_start + size) {
        new_pc = (pc - old_start) + new_start;
        if (ALIGNED(pc, ALLOC_UNIT) && ALIGNED(new_pc, ALLOC_UNIT) &&
            (old_start + size - pc) >= ALLOC_UNIT &&
            shadow_get_special(pc, &val) &&
            shadow_get_special(new_pc, &val)) {
            if (shadow_set_special(new_pc, val))
                pc += ALLOC_UNIT;
            else {
                /* a race and special was replaced w/ non-special: so re-do */
                ASSERT(!shadow_get_special(new_pc, NULL), "non-special never reverts");
            }
        } else {
            /* FIXME optimize: set 4 aligned bytes at a time */
            shadow_set_byte(new_pc, shadow_get_byte(pc));
            pc++;
        }
    }
}

static uint dqword_to_val(uint dqword)
{
    if (dqword == SHADOW_DQWORD_UNADDRESSABLE)
        return SHADOW_UNADDRESSABLE;
    if (dqword == SHADOW_DQWORD_UNDEFINED)
        return SHADOW_UNDEFINED;
    if (dqword == SHADOW_DQWORD_DEFINED)
        return SHADOW_DEFINED;
    if (dqword == SHADOW_DQWORD_BITLEVEL)
        return SHADOW_DEFINED_BITLEVEL;
    return UINT_MAX;
}

const char *
shadow_dqword_name(uint dqword)
{
    if (dqword == SHADOW_DQWORD_UNADDRESSABLE)
        return shadow_name[SHADOW_UNADDRESSABLE];
    if (dqword == SHADOW_DQWORD_UNDEFINED)
        return shadow_name[SHADOW_UNDEFINED];
    if (dqword == SHADOW_DQWORD_DEFINED)
        return shadow_name[SHADOW_DEFINED];
    if (dqword == SHADOW_DQWORD_BITLEVEL)
        return shadow_name[SHADOW_DEFINED_BITLEVEL];
    return "<mixed>";
}

/* Compares every byte in [start, start+size) to expect.
 * Stops and returns the pc of the first non-matching value.
 * If all bytes match, returns start+size.
 * bad_state is a dqword value.
 */
bool
shadow_check_range(app_pc start, size_t size, uint expect,
                   app_pc *bad_start, app_pc *bad_end, uint *bad_state)
{
    app_pc pc = start;
    uint val;
    uint bad_val = 0;
    bool res = true;
    size_t incr;
    ASSERT(expect <= 4, "invalid shadow value");
    ASSERT(start+size > start, "invalid param");
    while (pc < start+size) {
        if (!ALIGNED(pc, 16)) {
            val = shadow_get_byte(pc);
            incr = 1;
        } else if (shadow_get_special(pc, &val)) {
            incr = ALLOC_UNIT - (pc - (app_pc)ALIGN_BACKWARD(pc, ALLOC_UNIT));
        } else {
            shadow_block_t *block = get_shadow_table(TABLE_IDX(pc));
            val = bitmapx2_dword(*block, ((ptr_uint_t)pc) % ALLOC_UNIT);
            val = dqword_to_val(val);
            if (val == UINT_MAX) {
                /* mixed: have to drop to per-byte */
                val = shadow_get_byte(pc);
                incr = 1;
            } else /* all identical */
                incr = 16;
        }
        if (!res) {
            /* we know we have some non-matching bytes, but we want to know
             * the full extent of them (if identical) (if bad_end is non-NULL)
             */
            if (val != bad_val || bad_end == NULL) {
                if (bad_end != NULL)
                    *bad_end = pc;
                break;
            }
        } else if (val != expect) {
            res = false;
            bad_val = val;
            if (bad_start != NULL)
                *bad_start = pc;
            if (bad_state != NULL)
                *bad_state = val;
        }
        pc += incr;
    }
    if (!res && val == bad_val && bad_end != NULL)
        *bad_end = pc;
    return res;
}

/* Walks backward from start comparing each byte to expect.
 * If a non-matching value is reached, stops and returns false with the
 * non-matching addr in bad_addr.
 * If all bytes match when it reaches start-size, returns true.
 * N.B.: if this finds more important uses, should generalize and give
 * all the features of the forward version.
 */
bool
shadow_check_range_backward(app_pc start, size_t size, uint expect, app_pc *bad_addr)
{
    app_pc pc = start;
    uint val;
    bool res = true;
    ASSERT(expect <= 4, "invalid shadow value");
    ASSERT(size < (size_t)start, "invalid param");
    while (pc > start-size) {
        /* For simplicity and since performance is not critical for current
         * callers, walking one byte at a time
         */
        val = shadow_get_byte(pc);
        if (val != expect) {
            res = false;
            if (bad_addr != NULL)
                *bad_addr = pc;
            break;
        }
        pc--;
    }
    return res;
}

/* Finds the next aligned dword, starting at start and stopping at
 * end, whose shadow equals expect expanded to a dword.
 */
app_pc
shadow_next_dword(app_pc start, app_pc end, uint expect)
{
    app_pc pc = start;
    size_t incr;
    uint expect_dword = val_to_dword[expect];
    ASSERT(expect <= 4, "invalid shadow value");
    ASSERT(ALIGNED(start, 4), "invalid start pc");
    while (pc < end) {
        shadow_block_t *block = get_shadow_table(TABLE_IDX(pc));
        LOG(5, "shadow_next_dword: checking "PFX"\n", pc);
        if (block_is_special(block)) {
            uint blockval = shadow_get_byte(pc);
            uint dwordval = val_to_dword[blockval];
            if (dwordval == expect_dword)
                return pc;
        } else {
            byte *base = (byte *)(*block);
            size_t mod = ((ptr_uint_t)pc) % ALLOC_UNIT;
            byte *start_shadow = base + BLOCK_AS_BYTE_ARRAY_IDX(mod);
            byte *shadow = start_shadow;
            while (shadow < base + sizeof(*block) && *shadow != expect_dword)
                shadow++;
            if (shadow < base + sizeof(*block)) {
                pc = pc + ((shadow - start_shadow)*4);
                if (pc < end)
                    return pc;
                else
                    return NULL;
            }
        }
        incr = ALLOC_UNIT - (pc - (app_pc)ALIGN_BACKWARD(pc, ALLOC_UNIT));
        if (POINTER_OVERFLOW_ON_ADD(pc, incr))
            break;
        pc += incr;
    }
    return NULL;
}

/* Finds the previous aligned dword, starting at start and stopping at
 * end (end < start), whose shadow equals expect expanded to a dword.
 */
app_pc
shadow_prev_dword(app_pc start, app_pc end, uint expect)
{
    app_pc pc = start;
    size_t incr;
    uint expect_dword = val_to_dword[expect];
    ASSERT(expect <= 4, "invalid shadow value");
    ASSERT(ALIGNED(start, 4), "invalid start pc");
    ASSERT(end < start, "invalid end pc");
    while (pc > end) {
        shadow_block_t *block = get_shadow_table(TABLE_IDX(pc));
        LOG(5, "shadow_prev_dword: checking "PFX"\n", pc);
        if (block_is_special(block)) {
            uint blockval = shadow_get_byte(pc);
            uint dwordval = val_to_dword[blockval];
            if (dwordval == expect_dword)
                return pc;
        } else {
            byte *base = (byte *)(*block);
            size_t mod = ((ptr_uint_t)pc) % ALLOC_UNIT;
            byte *start_shadow = base + BLOCK_AS_BYTE_ARRAY_IDX(mod);
            byte *shadow = start_shadow;
            while (shadow >= base && *shadow != expect_dword)
                shadow--;
            if (shadow >= base) {
                pc = pc - ((start_shadow - shadow)*4);
                if (pc > end)
                    return pc;
                else
                    return NULL;
            }
        }
        incr = (pc - (app_pc)ALIGN_BACKWARD(pc-1, ALLOC_UNIT));
        if (POINTER_UNDERFLOW_ON_SUB(pc, incr))
            if (pc - incr < pc)
            break;
        ASSERT(incr > 0, "infinite loop");
        pc -= incr;
    }
    return NULL;
}

/* Caller does a lea or equivalent:
 *   0x4d1cd047  8d 8e 84 00 00 00    lea    0x00000084(%esi) -> %ecx
 * And this routine adds:
 *   0x4d1cd04d  8b d1                mov    %ecx -> %edx
 *   0x4d1cd04f  c1 ea 10             shr    $0x00000010 %edx -> %edx
 *   0x4d1cd052  c1 e9 02             shr    $0x00000002 %ecx -> %ecx
 *   0x4d1cd055  03 0c 95 40 26 96 73 add    0x73962640(,%edx,4) %ecx -> %ecx
 * And now the shadow addr is in %ecx.
 */
void
shadow_gen_translation_addr(void *drcontext, instrlist_t *bb, instr_t *inst,
                            reg_id_t addr_reg, reg_id_t scratch_reg)
{
    uint disp;
    /* Shadow table stores displacement so we want copy of whole addr */
    PRE(bb, inst, INSTR_CREATE_mov_ld
        (drcontext, opnd_create_reg(scratch_reg), opnd_create_reg(addr_reg)));
    /* Get top 16 bits into lower half.  We'll do x4 in a scale later, which
     * saves us from having to clear the lower bits here via OP_and or sthg (PR
     * 553724).
     */
    PRE(bb, inst, INSTR_CREATE_shr
        (drcontext, opnd_create_reg(scratch_reg), OPND_CREATE_INT8(16)));

    /* Instead of finding the uint array index we go straight to the single
     * byte (or 2 bytes) that shadows this <4-byte (or 8-byte) read, since aligned.
     * If sub-dword but not aligned we go ahead and get shadow byte for
     * containing dword.
     */
    PRE(bb, inst, INSTR_CREATE_shr
        (drcontext, opnd_create_reg(addr_reg), OPND_CREATE_INT8(2)));

    /* Index into table: no collisions and no tag storage since full size */
    /* Storing displacement, so add table result to app addr */
    ASSERT_TRUNCATE(disp, uint, (ptr_uint_t)shadow_table);
    disp = (uint)(ptr_uint_t)shadow_table;
    PRE(bb, inst, INSTR_CREATE_add
        (drcontext, opnd_create_reg(addr_reg), opnd_create_base_disp
         (REG_NULL, scratch_reg, 4, disp, OPSZ_PTR)));
}

/***************************************************************************
 * TABLES
 *
 * Table lookup is most performant way to do many operations, when we're
 * instrumenting so much code that code size is a bottleneck.
 */

/*
0000
0001
0010
0011
0100
0101
0110
0111
1000
1001
1010
1011
1100
1101
1110
1111
*/

/* If any 2-bit sequence is 01, return 0 */
const byte shadow_dword_is_addressable[256] = {
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* 2 */
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* A */
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* B */
    
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* E */
    1, 0, 1, 1,  0, 0, 0, 0,  1, 0, 1, 1,  1, 0, 1, 1,  /* F */
};

/* If any 2-bit sequence is 01 or 10, return 0 */
const byte shadow_dword_is_addr_not_bit[256] = {
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* F */
};

/* Expand a 2-bit sequence, with the other bits 0 */
const byte shadow_2_to_dword[256] = {
    0x00,0x55,0xaa,0xff,  0x55,0,0,0,  0xaa,0,0,0,  0xff,0,0,0,  /* 0 */
    0x55,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 1 */
    0xaa,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 2 */
    0xff,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 3 */
    
    0x55,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 4 */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 5 */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 6 */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 7 */
    
    0xaa,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 8 */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* 9 */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* A */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* B */
    
    0xff,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* C */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* D */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* E */
    0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  /* F */
};

/* Expand a 4-bit sequence, with the other bits 0, for movsx.
 * The higher 2-bit propagates, the lower stays.
 * The other half being non-0 is an error so we return bitlevel 0xaa.
 */
const byte shadow_4_to_dword[256] = {
    0x00,0x01,0x02,0x03,0x54,0x55,0x56,0x57,0xa8,0xa9,0xaa,0xab,0xfc,0xfd,0xfe,0xff,/*0*/
    0x01,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*1*/
    0x02,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*2*/
    0x03,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*3*/
    
    0x54,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*4*/
    0x55,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*5*/
    0x56,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*6*/
    0x57,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*7*/
    
    0xa8,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*8*/
    0xa9,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*9*/
    0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*A*/
    0xab,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*B*/
    
    0xfc,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*C*/
    0xfd,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*D*/
    0xfe,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*E*/
    0xff,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,/*F*/
};

/* Returns 1 iff the selected 2-bit sequence is 00 */
const byte shadow_byte_defined[4][256] = {
{ /* offs = 0 => bits 0-1 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 0 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 1 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 2 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 3 */
    
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 4 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 5 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 6 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 7 */
    
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 8 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* 9 */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* A */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* B */
    
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* C */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* D */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* E */
    1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  /* F */
},
{ /* offs = 1 => bits 2-3 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 0 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
{ /* offs = 2 => bits 4-5 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
{ /* offs = 3 => bits 6-7 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 0 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 1 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 2 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
};

/* Returns 1 iff the selected 4-bit sequence is 0000.
 * We should already be checking alignment so we expect
 * either 0 or 2 for the offs.
 */
const byte shadow_word_defined[4][256] = {
{ /* offs = 0 => bits 0-3 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 0 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    1, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
{ /* offs = 1 => unaligned */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
{ /* offs = 2 => bits 4-7 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
{ /* offs = 3 => unaligned */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
};

/* Returns 1 iff the selected 2-bit sequence is 00 or 11 */
const byte shadow_byte_addr_not_bit[4][256] = {
{ /* offs = 0 => bits 0-1 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 0 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 1 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 2 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 3 */
    
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 4 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 5 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 6 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 7 */
    
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 8 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* 9 */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* A */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* B */
    
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* C */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* D */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* E */
    1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,  /* F */
},
{ /* offs = 1 => bits 2-3 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 0 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 1 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 2 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 3 */
                                                      
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 4 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 5 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 6 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 7 */
                                                      
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 8 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* 9 */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* A */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* B */
                                                      
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* C */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* D */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* E */
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 1, 1, 1,  /* F */
},
{ /* offs = 2 => bits 4-5 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 3 */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 7 */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* B */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* F */
},
{ /* offs = 3 => bits 6-7 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 0 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 1 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 2 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* C */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* D */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* E */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* F */
},
};

/* Returns 1 iff the selected 4-bit sequence is 0000, 1100, 0011, or 1111.
 * We should already be checking alignment so we expect
 * either 0 or 2 for the offs.
 */
const byte shadow_word_addr_not_bit[4][256] = {
{ /* offs = 0 => bits 0-3 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 0 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 1 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 2 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 3 */
    
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 4 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 5 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 6 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 7 */
    
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 8 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* 9 */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* A */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* B */
    
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* C */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* D */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* E */
    1, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 1,  /* F */
},
{ /* offs = 1 => unaligned */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
{ /* offs = 2 => bits 4-7 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  /* F */
},
{ /* offs = 3 => unaligned */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 0 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 1 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 2 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 3 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 4 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 5 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 6 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 7 */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 8 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* 9 */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* A */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* B */
    
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* C */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* D */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* E */
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  /* F */
},
};

#endif /* TOOL_DR_MEMORY around whole shadow table */

/***************************************************************************
 * SHADOWING THE GPR REGISTERS
 */

/* We keep our shadow register bits in TLS */
typedef struct _shadow_registers_t {
#ifdef TOOL_DR_MEMORY
    /* First 4-byte TLS slot */
    byte eax;
    byte ecx;
    byte edx;
    byte ebx;
    /* Second 4-byte TLS slot */
    byte esp;
    byte ebp;
    byte esi;
    byte edi;
    /* Third 4-byte TLS slot.  We go ahead and write DWORD values here
     * for simplicity in our fastpath even though we treat this as
     * a single tracked value.
     */
    byte eflags;
    /* Used for PR 578892.  Should remain a very small integer so byte is fine. */
    byte in_heap_routine;
    byte padding[2];
#else
    /* Avoid empty struct.  FIXME: this is a waste of a tls slot */
    void *bogus;
#endif
} shadow_registers_t;

#define NUM_SHADOW_TLS_SLOTS (sizeof(shadow_registers_t)/sizeof(reg_t))

static uint tls_shadow_base;

/* we store a pointer for finding shadow regs for other threads */
static int tls_idx_shadow = -1;

#ifdef TOOL_DR_MEMORY
opnd_t
opnd_create_shadow_reg_slot(reg_id_t reg)
{
    reg_id_t r = reg_to_pointer_sized(reg);
    ASSERT(options.shadowing, "incorrectly called");
    ASSERT(reg_is_gpr(reg), "internal shadow reg error");
    return opnd_create_far_base_disp_ex
        (SEG_FS, REG_NULL, REG_NULL, 1, tls_shadow_base + (r - REG_EAX), OPSZ_1,
         /* we do NOT want an addr16 prefix since most likely going to run on
          * Core or Core2, and P4 doesn't care that much */
         false, true, false);
}

opnd_t
opnd_create_shadow_eflags_slot(void)
{
    ASSERT(options.shadowing, "incorrectly called");
    return opnd_create_far_base_disp_ex
        (SEG_FS, REG_NULL, REG_NULL, 1, tls_shadow_base +
         offsetof(shadow_registers_t, eflags), OPSZ_1,
         /* we do NOT want an addr16 prefix since most likely going to run on
          * Core or Core2, and P4 doesn't care that much */
         false, true, false);
}

/* Opnd to acquire in_heap_routine TLS counter. Used for PR 578892. */
opnd_t
opnd_create_shadow_inheap_slot(void)
{
    ASSERT(options.shadowing, "incorrectly called");
    return opnd_create_far_base_disp_ex
        (SEG_FS, REG_NULL, REG_NULL, 1, tls_shadow_base +
         offsetof(shadow_registers_t, in_heap_routine), OPSZ_1,
         /* we do NOT want an addr16 prefix since most likely going to run on
          * Core or Core2, and P4 doesn't care that much */
         false, true, false);
}
#endif /* TOOL_DR_MEMORY */

#if defined(TOOL_DR_MEMORY) || defined(WINDOWS)
static shadow_registers_t *
get_shadow_registers(void)
{
    byte *seg_base;
    ASSERT(options.shadowing, "incorrectly called");
    seg_base = get_own_seg_base();
    return (shadow_registers_t *)(seg_base + tls_shadow_base);
}
#endif /* TOOL_DR_MEMORY || WINDOWS */

static void
shadow_registers_thread_init(void *drcontext)
{
#ifdef TOOL_DR_MEMORY
    static bool first_thread = true;
#endif
    shadow_registers_t *sr;
#ifdef LINUX
    sr = (shadow_registers_t *)
        (dr_get_dr_segment_base(IF_X64_ELSE(SEG_GS, SEG_FS)) + tls_shadow_base);
#else
    sr = get_shadow_registers();
#endif
#ifdef TOOL_DR_MEMORY
    if (first_thread) {
        first_thread = false;
        /* since we're in late, we consider everything defined
         * (if we were in at init APC, only stack pointer would be defined) */
        memset(sr, SHADOW_DWORD_DEFINED, sizeof(*sr));
        sr->eflags = SHADOW_DEFINED;
    } else {
        /* we are in at start for new threads */
        memset(sr, SHADOW_DWORD_UNDEFINED, sizeof(*sr));
        sr->eflags = SHADOW_UNDEFINED;
#ifdef LINUX
        /* PR 426162: post-clone, esp and eax are defined */
        sr->esp = SHADOW_DWORD_DEFINED;
        sr->eax = SHADOW_DWORD_DEFINED;
#else
        /* new thread on Windows has esp defined */
        sr->esp = SHADOW_DWORD_DEFINED;
#endif
    }
    sr->in_heap_routine = 0;
#endif /* TOOL_DR_MEMORY */

    /* store in per-thread data struct so we can access from another thread */
    drmgr_set_tls_field(drcontext, tls_idx_shadow, (void *) sr);
}

static void
shadow_registers_thread_exit(void *drcontext)
{
    drmgr_set_tls_field(drcontext, tls_idx_shadow, NULL);
}

static void
shadow_registers_init(void)
{
    reg_id_t seg;
    /* XXX: could save space by not allocating shadow regs for -no_check_uninitialized */
    IF_DEBUG(bool ok =)
        dr_raw_tls_calloc(&seg, &tls_shadow_base, NUM_SHADOW_TLS_SLOTS, 0);
    tls_idx_shadow = drmgr_register_tls_field();
    ASSERT(tls_idx_shadow > -1, "failed to reserve TLS slot");
    LOG(2, "TLS shadow base: "PIFX"\n", tls_shadow_base);
    ASSERT(ok, "fatal error: unable to reserve tls slots");
    ASSERT(seg == IF_X64_ELSE(SEG_GS, SEG_FS), "unexpected tls segment");
}

static void
shadow_registers_exit(void)
{
    IF_DEBUG(bool ok =)
        dr_raw_tls_cfree(tls_shadow_base, NUM_SHADOW_TLS_SLOTS);
    ASSERT(ok, "WARNING: unable to free tls slots");
    drmgr_unregister_tls_field(tls_idx_shadow);
}

#ifdef TOOL_DR_MEMORY
void
print_shadow_registers(void)
{
    IF_DEBUG(shadow_registers_t *sr = get_shadow_registers());
    ASSERT(options.shadowing, "shouldn't be called");
    LOG(0, "    eax=%02x ecx=%02x edx=%02x ebx=%02x "
        "esp=%02x ebp=%02x esi=%02x edi=%02x efl=%x\n",
        sr->eax, sr->ecx, sr->edx, sr->ebx, sr->esp, sr->ebp,
        sr->esi, sr->edi, sr->eflags);
}

static uint
reg_shadow_offs(reg_id_t reg)
{
    /* REG_NULL means eflags */
    if (reg == REG_NULL)
        return offsetof(shadow_registers_t, eflags);
    else
        return (reg_to_pointer_sized(reg) - REG_EAX);
}

static byte
get_shadow_register_common(shadow_registers_t *sr, reg_id_t reg)
{
    byte val;
    opnd_size_t sz = reg_get_size(reg);
    ASSERT(options.shadowing, "incorrectly called");
    ASSERT(reg_is_gpr(reg), "internal shadow reg error");
    val = *(((byte*)sr) + reg_shadow_offs(reg));
    if (sz == OPSZ_1)
        val &= 0x3;
    else if (sz == OPSZ_2)
        val &= 0xf;
    else
        ASSERT(sz == OPSZ_4, "internal shadow reg error");
    return val;
}

/* Note that any SHADOW_UNADDRESSABLE bit pairs simply mean it's
 * a sub-register
 */
byte
get_shadow_register(reg_id_t reg)
{
    shadow_registers_t *sr = get_shadow_registers();
    ASSERT(options.shadowing, "incorrectly called");
    return get_shadow_register_common(sr, reg);
}

/* Note that any SHADOW_UNADDRESSABLE bit pairs simply mean it's
 * a sub-register
 */
byte
get_thread_shadow_register(void *drcontext, reg_id_t reg)
{
    shadow_registers_t *sr = (shadow_registers_t *)
        drmgr_get_tls_field(drcontext, tls_idx_shadow);
    ASSERT(options.shadowing, "incorrectly called");
    return get_shadow_register_common(sr, reg);
}

void
register_shadow_set_byte(reg_id_t reg, uint bytenum, uint val)
{
    shadow_registers_t *sr = get_shadow_registers();
    uint shift = bytenum*2;
    byte *addr;
    ASSERT(options.shadowing, "incorrectly called");
    ASSERT(reg_is_gpr(reg), "internal shadow reg error");
    addr = ((byte*)sr) + reg_shadow_offs(reg);
    *addr = set_2bits_inline(*addr, val, shift);
}

void
register_shadow_set_dword(reg_id_t reg, uint val)
{
    shadow_registers_t *sr = get_shadow_registers();
    byte *addr;
    ASSERT(options.shadowing, "incorrectly called");
    ASSERT(reg_is_gpr(reg), "internal shadow reg error");
    addr = ((byte*)sr) + reg_shadow_offs(reg);
    *addr = (byte) val;
}

byte
get_shadow_eflags(void)
{
    shadow_registers_t *sr = get_shadow_registers();
    ASSERT(options.shadowing, "incorrectly called");
    return sr->eflags;
}

void
set_shadow_eflags(uint val)
{
    shadow_registers_t *sr = get_shadow_registers();
    ASSERT(options.shadowing, "incorrectly called");
    sr->eflags = (byte) val;
}

byte
get_shadow_inheap(void)
{
    shadow_registers_t *sr;
    ASSERT(options.shadowing, "incorrectly called");
    sr = get_shadow_registers();
    return sr->in_heap_routine;
}

void
set_shadow_inheap(uint val)
{
    shadow_registers_t *sr;
    ASSERT(options.shadowing, "incorrectly called");
    sr = get_shadow_registers();
    sr->in_heap_routine = (byte) val;
}

/* assumes val was obtained from get_shadow_register(),
 * but should never have unaddressable anyway
 */
bool
is_shadow_register_defined(byte val)
{
    return (val == SHADOW_DEFINED ||
            val == SHADOW_WORD_DEFINED ||
            val == SHADOW_DWORD_DEFINED);
}
#endif /* TOOL_DR_MEMORY */

/***************************************************************************/

void
shadow_thread_init(void *drcontext)
{
    shadow_registers_thread_init(drcontext);
}

void
shadow_thread_exit(void *drcontext)
{
    shadow_registers_thread_exit(drcontext);
}

void
shadow_init(void)
{
    ASSERT(options.shadowing, "shadowing disabled");
    shadow_registers_init();
    shadow_table_init();
}

void
shadow_exit(void)
{
    shadow_registers_exit();
    shadow_table_exit();
}

