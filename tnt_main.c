
/*--------------------------------------------------------------------*/
/*--- Taintgrind: The taint analysis Valgrind tool.        tnt_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Taintgrind, the taint analysis Valgrind tool.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"

#include "pub_tool_vki.h"           // keeps libcproc.h happy, syscall nums
#include "pub_tool_aspacemgr.h"     // VG_(am_shadow_alloc)
#include "pub_tool_debuginfo.h"     // VG_(get_fnname_w_offset), VG_(get_fnname)
#include "pub_tool_hashtable.h"     // For tnt_include.h, VgHashtable
#include "pub_tool_libcassert.h"    // tl_assert
#include "pub_tool_libcbase.h"      // VG_STREQN
#include "pub_tool_libcprint.h"     // VG_(message)
#include "pub_tool_libcproc.h"      // VG_(getenv)
#include "pub_tool_replacemalloc.h" // VG_(replacement_malloc_process_cmd_line_option)
#include "pub_tool_machine.h"       // VG_(get_IP)
#include "pub_tool_mallocfree.h"    // VG_(out_of_memory_NORETURN)
#include "pub_tool_options.h"       // VG_STR/BHEX/BINT_CLO
#include "pub_tool_oset.h"          // OSet operations
#include "pub_tool_threadstate.h"   // VG_(get_running_tid)
#include "pub_tool_xarray.h"        // VG_(*XA)
#include "pub_tool_stacktrace.h"    // VG_(get_and_pp_StackTrace)
#include "pub_tool_libcfile.h"      // VG_(readlink)
#include "pub_tool_addrinfo.h"      // VG_(describe_addr)

#include "tnt_include.h"
#include "tnt_strings.h"
#include "tnt_structs.h"


/*------------------------------------------------------------*/
/*--- Fast-case knobs                                      ---*/
/*------------------------------------------------------------*/

// Comment these out to disable the fast cases (don't just set them to zero).

#define PERF_FAST_LOADV    1
#define PERF_FAST_STOREV   1

#define PERF_FAST_SARP     1

/*---------- Taintgrind DEBUG statements---------*/
//#define DBG_MEM
//#define DBG_LOAD
//#define DBG_STORE
//#define DBG_COPY_ADDR_RANGE_STATE

/* --------------- Basic configuration --------------- */

/* Only change this.  N_PRIMARY_MAP *must* be a power of 2. */

#if VG_WORDSIZE == 4

/* cover the entire address space */
#  define N_PRIMARY_BITS  16

#else

/* Just handle the first 32G fast and the rest via auxiliary
   primaries.  If you change this, Memcheck will assert at startup.
   See the definition of UNALIGNED_OR_HIGH for extensive comments. */
#  define N_PRIMARY_BITS  19

#endif


/* Do not change this. */
#define N_PRIMARY_MAP  ( ((UWord)1) << N_PRIMARY_BITS)

/* Do not change this. */
#define MAX_PRIMARY_ADDRESS (Addr)((((Addr)65536) * N_PRIMARY_MAP)-1)

// Taintgrind: UNDEFINED -> TAINTED, DEFINED -> UNTAINTED,
//             PARTDEFINED -> PARTUNTAINTED
// These represent eight bits of memory.
#define VA_BITS2_NOACCESS      0x0      // 00b
#define VA_BITS2_TAINTED       0x1      // 01b
#define VA_BITS2_UNTAINTED     0x2      // 10b
#define VA_BITS2_PARTUNTAINTED 0x3      // 11b

// These represent 16 bits of memory.
#define VA_BITS4_NOACCESS     0x0      // 00_00b
#define VA_BITS4_TAINTED      0x5      // 01_01b
#define VA_BITS4_UNTAINTED    0xa      // 10_10b

// These represent 32 bits of memory.
#define VA_BITS8_NOACCESS     0x00     // 00_00_00_00b
#define VA_BITS8_TAINTED      0x55     // 01_01_01_01b
#define VA_BITS8_UNTAINTED    0xaa     // 10_10_10_10b

// These represent 64 bits of memory.
#define VA_BITS16_NOACCESS    0x0000   // 00_00_00_00b x 2
#define VA_BITS16_TAINTED     0x5555   // 01_01_01_01b x 2
#define VA_BITS16_UNTAINTED   0xaaaa   // 10_10_10_10b x 2

#define SM_CHUNKS             16384
#define SM_OFF(aaa)           (((aaa) & 0xffff) >> 2)
#define SM_OFF_16(aaa)        (((aaa) & 0xffff) >> 3)

// Paranoia:  it's critical for performance that the requested inlining
// occurs.  So try extra hard.
#define INLINE    inline __attribute__((always_inline))

static INLINE Addr start_of_this_sm ( Addr a ) {
   return (a & (~SM_MASK));
}
static INLINE Bool is_start_of_sm ( Addr a ) {
   return (start_of_this_sm(a) == a);
}

typedef
   struct {
      UChar vabits8[SM_CHUNKS];
   }
   SecMap;

// 3 distinguished secondary maps, one for no-access, one for
// accessible but undefined, and one for accessible and defined.
// Distinguished secondaries may never be modified.
#define SM_DIST_NOACCESS   0
#define SM_DIST_TAINTED    1
#define SM_DIST_UNTAINTED  2

static SecMap sm_distinguished[3];

static INLINE Bool is_distinguished_sm ( SecMap* sm ) {
   return sm >= &sm_distinguished[0] && sm <= &sm_distinguished[2];
}

// -Start- Forward declarations for Taintgrind
Int  ctoi( HChar c );
Int  ctoi_test( HChar c );
Int  atoi( HChar *s );
Int get_and_check_reg( HChar *reg );
Int get_and_check_tvar( HChar *tmp );
void infer_client_binary_name(UInt pc);
// -End- Forward declarations for Taintgrind

static void update_SM_counts(SecMap* oldSM, SecMap* newSM); //285

/* dist_sm points to one of our three distinguished secondaries.  Make
   a copy of it so that we can write to it.
*/
static SecMap* copy_for_writing ( SecMap* dist_sm )
{
   SecMap* new_sm;
   tl_assert(dist_sm == &sm_distinguished[0]
          || dist_sm == &sm_distinguished[1]
          || dist_sm == &sm_distinguished[2]);

   new_sm = VG_(am_shadow_alloc)(sizeof(SecMap));
   if (new_sm == NULL)
      VG_(out_of_memory_NORETURN)( "memcheck:allocate new SecMap",
                                   sizeof(SecMap) );
   VG_(memcpy)(new_sm, dist_sm, sizeof(SecMap));
   update_SM_counts(dist_sm, new_sm);
   return new_sm;
}

/* --------------- Stats --------------- */

static Int   n_issued_SMs      = 0;
static Int   n_deissued_SMs    = 0;
static Int   n_noaccess_SMs    = N_PRIMARY_MAP; // start with many noaccess DSMs
static Int   n_undefined_SMs   = 0;
static Int   n_defined_SMs     = 0;
static Int   n_non_DSM_SMs     = 0;
static Int   max_noaccess_SMs  = 0;
static Int   max_undefined_SMs = 0;
static Int   max_defined_SMs   = 0;
static Int   max_non_DSM_SMs   = 0;

/* # searches initiated in auxmap_L1, and # base cmps required */
static ULong n_auxmap_L1_searches  = 0;
static ULong n_auxmap_L1_cmps      = 0;
/* # of searches that missed in auxmap_L1 and therefore had to
   be handed to auxmap_L2. And the number of nodes inserted. */
static ULong n_auxmap_L2_searches  = 0;
static ULong n_auxmap_L2_nodes     = 0;

//static Int   n_sanity_cheap     = 0;
//static Int   n_sanity_expensive = 0;

static Int   n_secVBit_nodes   = 0;
static Int   max_secVBit_nodes = 0;

static void update_SM_counts(SecMap* oldSM, SecMap* newSM)
{
   if      (oldSM == &sm_distinguished[SM_DIST_NOACCESS ]) n_noaccess_SMs --;
   else if (oldSM == &sm_distinguished[SM_DIST_TAINTED]) n_undefined_SMs--;
   else if (oldSM == &sm_distinguished[SM_DIST_UNTAINTED  ]) n_defined_SMs  --;
   else                                                  { n_non_DSM_SMs  --;
                                                           n_deissued_SMs ++; }

   if      (newSM == &sm_distinguished[SM_DIST_NOACCESS ]) n_noaccess_SMs ++;
   else if (newSM == &sm_distinguished[SM_DIST_TAINTED]) n_undefined_SMs++;
   else if (newSM == &sm_distinguished[SM_DIST_UNTAINTED  ]) n_defined_SMs  ++;
   else                                                  { n_non_DSM_SMs  ++;
                                                           n_issued_SMs   ++; }

   if (n_noaccess_SMs  > max_noaccess_SMs ) max_noaccess_SMs  = n_noaccess_SMs;
   if (n_undefined_SMs > max_undefined_SMs) max_undefined_SMs = n_undefined_SMs;
   if (n_defined_SMs   > max_defined_SMs  ) max_defined_SMs   = n_defined_SMs;
   if (n_non_DSM_SMs   > max_non_DSM_SMs  ) max_non_DSM_SMs   = n_non_DSM_SMs;
}

/* --------------- Primary maps --------------- */

/* The main primary map.  This covers some initial part of the address
   space, addresses 0 .. (N_PRIMARY_MAP << 16)-1.  The rest of it is
   handled using the auxiliary primary map.
*/
static SecMap* primary_map[N_PRIMARY_MAP];

/* An entry in the auxiliary primary map.  base must be a 64k-aligned
   value, and sm points at the relevant secondary map.  As with the
   main primary map, the secondary may be either a real secondary, or
   one of the three distinguished secondaries.  DO NOT CHANGE THIS
   LAYOUT: the first word has to be the key for OSet fast lookups.
*/
typedef
   struct {
      Addr    base;
      SecMap* sm;
   }
   AuxMapEnt;

/* Tunable parameter: How big is the L1 queue? */
#define N_AUXMAP_L1 24

/* Tunable parameter: How far along the L1 queue to insert
   entries resulting from L2 lookups? */
#define AUXMAP_L1_INSERT_IX 12

static struct {
          Addr       base;
          AuxMapEnt* ent; // pointer to the matching auxmap_L2 node
       }
       auxmap_L1[N_AUXMAP_L1];

static OSet* auxmap_L2 = NULL;
static void init_auxmap_L1_L2 ( void )
{
   Int i;
   for (i = 0; i < N_AUXMAP_L1; i++) {
      auxmap_L1[i].base = 0;
      auxmap_L1[i].ent  = NULL;
   }

   tl_assert(0 == offsetof(AuxMapEnt,base));
   tl_assert(sizeof(Addr) == sizeof(void*));
   auxmap_L2 = VG_(OSetGen_Create)( /*keyOff*/  offsetof(AuxMapEnt,base),
                                    /*fastCmp*/ NULL,
                                    VG_(malloc), "mc.iaLL.1", VG_(free) );
}

/* Check representation invariants; if OK return NULL; else a
   descriptive bit of text.  Also return the number of
   non-distinguished secondary maps referred to from the auxiliary
   primary maps. */

//static HChar* check_auxmap_L1_L2_sanity ( Word* n_secmaps_found )
//{
//   Word i, j;
   /* On a 32-bit platform, the L2 and L1 tables should
      both remain empty forever.

      On a 64-bit platform:
      In the L2 table:
       all .base & 0xFFFF == 0
       all .base > MAX_PRIMARY_ADDRESS
      In the L1 table:
       all .base & 0xFFFF == 0
       all (.base > MAX_PRIMARY_ADDRESS
            .base & 0xFFFF == 0
            and .ent points to an AuxMapEnt with the same .base)
           or
           (.base == 0 and .ent == NULL)
   */
//   *n_secmaps_found = 0;
//   if (sizeof(void*) == 4) {
      /* 32-bit platform */
//      if (VG_(OSetGen_Size)(auxmap_L2) != 0)
//         return "32-bit: auxmap_L2 is non-empty";
//      for (i = 0; i < N_AUXMAP_L1; i++)
//        if (auxmap_L1[i].base != 0 || auxmap_L1[i].ent != NULL)
//      return "32-bit: auxmap_L1 is non-empty";
//   } else {
      /* 64-bit platform */
//      UWord elems_seen = 0;
//      AuxMapEnt *elem, *res;
//      AuxMapEnt key;
      /* L2 table */
//      VG_(OSetGen_ResetIter)(auxmap_L2);
//      while ( (elem = VG_(OSetGen_Next)(auxmap_L2)) ) {
//         elems_seen++;
//         if (0 != (elem->base & (Addr)0xFFFF))
//            return "64-bit: nonzero .base & 0xFFFF in auxmap_L2";
//         if (elem->base <= MAX_PRIMARY_ADDRESS)
//            return "64-bit: .base <= MAX_PRIMARY_ADDRESS in auxmap_L2";
//         if (elem->sm == NULL)
//            return "64-bit: .sm in _L2 is NULL";
//         if (!is_distinguished_sm(elem->sm))
//            (*n_secmaps_found)++;
//      }
//      if (elems_seen != n_auxmap_L2_nodes)
//         return "64-bit: disagreement on number of elems in _L2";
      /* Check L1-L2 correspondence */
/*      for (i = 0; i < N_AUXMAP_L1; i++) {
         if (auxmap_L1[i].base == 0 && auxmap_L1[i].ent == NULL)
            continue;
         if (0 != (auxmap_L1[i].base & (Addr)0xFFFF))
            return "64-bit: nonzero .base & 0xFFFF in auxmap_L1";
         if (auxmap_L1[i].base <= MAX_PRIMARY_ADDRESS)
            return "64-bit: .base <= MAX_PRIMARY_ADDRESS in auxmap_L1";
         if (auxmap_L1[i].ent == NULL)
            return "64-bit: .ent is NULL in auxmap_L1";
         if (auxmap_L1[i].ent->base != auxmap_L1[i].base)
            return "64-bit: _L1 and _L2 bases are inconsistent";*/
         /* Look it up in auxmap_L2. */
/*         key.base = auxmap_L1[i].base;
         key.sm   = 0;
         res = VG_(OSetGen_Lookup)(auxmap_L2, &key);
         if (res == NULL)
            return "64-bit: _L1 .base not found in _L2";
         if (res != auxmap_L1[i].ent)
            return "64-bit: _L1 .ent disagrees with _L2 entry";
      }*/
      /* Check L1 contains no duplicates */
/*      for (i = 0; i < N_AUXMAP_L1; i++) {
         if (auxmap_L1[i].base == 0)
            continue;
         for (j = i+1; j < N_AUXMAP_L1; j++) {
            if (auxmap_L1[j].base == 0)
               continue;
            if (auxmap_L1[j].base == auxmap_L1[i].base)
               return "64-bit: duplicate _L1 .base entries";
         }
      }
   }
   return NULL;*/ /* ok */
//}

static void insert_into_auxmap_L1_at ( Word rank, AuxMapEnt* ent )
{
   Word i;
   tl_assert(ent);
   tl_assert(rank >= 0 && rank < N_AUXMAP_L1);
   for (i = N_AUXMAP_L1-1; i > rank; i--)
      auxmap_L1[i] = auxmap_L1[i-1];
   auxmap_L1[rank].base = ent->base;
   auxmap_L1[rank].ent  = ent;
}

static INLINE AuxMapEnt* maybe_find_in_auxmap ( Addr a )
{
   AuxMapEnt  key;
   AuxMapEnt* res;
   Word       i;

   tl_assert(a > MAX_PRIMARY_ADDRESS);
   a &= ~(Addr)0xFFFF;

   /* First search the front-cache, which is a self-organising
      list containing the most popular entries. */

   if (LIKELY(auxmap_L1[0].base == a))
      return auxmap_L1[0].ent;
   if (LIKELY(auxmap_L1[1].base == a)) {
      Addr       t_base = auxmap_L1[0].base;
      AuxMapEnt* t_ent  = auxmap_L1[0].ent;
      auxmap_L1[0].base = auxmap_L1[1].base;
      auxmap_L1[0].ent  = auxmap_L1[1].ent;
      auxmap_L1[1].base = t_base;
      auxmap_L1[1].ent  = t_ent;
      return auxmap_L1[0].ent;
   }

   n_auxmap_L1_searches++;

   for (i = 0; i < N_AUXMAP_L1; i++) {
      if (auxmap_L1[i].base == a) {
         break;
      }
   }
   tl_assert(i >= 0 && i <= N_AUXMAP_L1);

   n_auxmap_L1_cmps += (ULong)(i+1);

   if (i < N_AUXMAP_L1) {
      if (i > 0) {
         Addr       t_base = auxmap_L1[i-1].base;
         AuxMapEnt* t_ent  = auxmap_L1[i-1].ent;
         auxmap_L1[i-1].base = auxmap_L1[i-0].base;
         auxmap_L1[i-1].ent  = auxmap_L1[i-0].ent;
         auxmap_L1[i-0].base = t_base;
         auxmap_L1[i-0].ent  = t_ent;
         i--;
      }
      return auxmap_L1[i].ent;
   }

   n_auxmap_L2_searches++;

   /* First see if we already have it. */
   key.base = a;
   key.sm   = 0;

   res = VG_(OSetGen_Lookup)(auxmap_L2, &key);
   if (res)
      insert_into_auxmap_L1_at( AUXMAP_L1_INSERT_IX, res );
   return res;
}

static AuxMapEnt* find_or_alloc_in_auxmap ( Addr a )
{
   AuxMapEnt *nyu, *res;

   /* First see if we already have it. */
   res = maybe_find_in_auxmap( a );
   if (LIKELY(res))
      return res;

   /* Ok, there's no entry in the secondary map, so we'll have
      to allocate one. */
   a &= ~(Addr)0xFFFF;
   nyu = (AuxMapEnt*) VG_(OSetGen_AllocNode)( auxmap_L2, sizeof(AuxMapEnt) );
   tl_assert(nyu);
   nyu->base = a;
   nyu->sm   = &sm_distinguished[SM_DIST_NOACCESS];
   VG_(OSetGen_Insert)( auxmap_L2, nyu );
   insert_into_auxmap_L1_at( AUXMAP_L1_INSERT_IX, nyu );
   n_auxmap_L2_nodes++;
   return nyu;
}

/* --------------- SecMap fundamentals --------------- */ //586

// In all these, 'low' means it's definitely in the main primary map,
// 'high' means it's definitely in the auxiliary table.

static INLINE SecMap** get_secmap_low_ptr ( Addr a )
{
   UWord pm_off = a >> 16;
//#  if VG_DEBUG_MEMORY >= 1
   tl_assert(pm_off < N_PRIMARY_MAP);
//#  endif
   return &primary_map[ pm_off ];
}

static INLINE SecMap** get_secmap_high_ptr ( Addr a )
{
   AuxMapEnt* am = find_or_alloc_in_auxmap(a);
   return &am->sm;
}

static SecMap** get_secmap_ptr ( Addr a )
{
   return ( a <= MAX_PRIMARY_ADDRESS
          ? get_secmap_low_ptr(a)
          : get_secmap_high_ptr(a));
}

static INLINE SecMap* get_secmap_for_reading_low ( Addr a )
{
   return *get_secmap_low_ptr(a);
}

static INLINE SecMap* get_secmap_for_reading_high ( Addr a )
{
   return *get_secmap_high_ptr(a);
}

static INLINE SecMap* get_secmap_for_writing_low(Addr a)
{
   SecMap** p = get_secmap_low_ptr(a);
   if (UNLIKELY(is_distinguished_sm(*p)))
      *p = copy_for_writing(*p);
   return *p;
}

static INLINE SecMap* get_secmap_for_writing_high ( Addr a )
{
   SecMap** p = get_secmap_high_ptr(a);
   if (UNLIKELY(is_distinguished_sm(*p)))
      *p = copy_for_writing(*p);
   return *p;
}

/* Produce the secmap for 'a', either from the primary map or by
   ensuring there is an entry for it in the aux primary map.  The
   secmap may be a distinguished one as the caller will only want to
   be able to read it.
*/
static INLINE SecMap* get_secmap_for_reading ( Addr a )
{
   return ( a <= MAX_PRIMARY_ADDRESS
          ? get_secmap_for_reading_low (a)
          : get_secmap_for_reading_high(a) );
}

/* Produce the secmap for 'a', either from the primary map or by
   ensuring there is an entry for it in the aux primary map.  The
   secmap may not be a distinguished one, since the caller will want
   to be able to write it.  If it is a distinguished secondary, make a
   writable copy of it, install it, and return the copy instead.  (COW
   semantics).
*/
static SecMap* get_secmap_for_writing ( Addr a )
{
   return ( a <= MAX_PRIMARY_ADDRESS
          ? get_secmap_for_writing_low (a)
          : get_secmap_for_writing_high(a) );
}

/* If 'a' has a SecMap, produce it.  Else produce NULL.  But don't
   allocate one if one doesn't already exist.  This is used by the
   leak checker.
*/
/*static SecMap* maybe_get_secmap_for ( Addr a )
{
   if (a <= MAX_PRIMARY_ADDRESS) {
      return get_secmap_for_reading_low(a);
   } else {
      AuxMapEnt* am = maybe_find_in_auxmap(a);
      return am ? am->sm : NULL;
   }
}*/

/* --------------- Fundamental functions --------------- */

static INLINE
void insert_vabits2_into_vabits8 ( Addr a, UChar vabits2, UChar* vabits8 ) //682
{
   UInt shift =  (a & 3)  << 1;        // shift by 0, 2, 4, or 6
   *vabits8  &= ~(0x3     << shift);   // mask out the two old bits
   *vabits8  |=  (vabits2 << shift);   // mask  in the two new bits
}

static INLINE
void insert_vabits4_into_vabits8 ( Addr a, UChar vabits4, UChar* vabits8 )
{
   UInt shift;
   tl_assert(VG_IS_2_ALIGNED(a));      // Must be 2-aligned
   shift     =  (a & 2)   << 1;        // shift by 0 or 4
   *vabits8 &= ~(0xf      << shift);   // mask out the four old bits
   *vabits8 |=  (vabits4 << shift);    // mask  in the four new bits
}

static INLINE
UChar extract_vabits2_from_vabits8 ( Addr a, UChar vabits8 )
{
   UInt shift = (a & 3) << 1;          // shift by 0, 2, 4, or 6
   vabits8 >>= shift;                  // shift the two bits to the bottom
   return 0x3 & vabits8;               // mask out the rest
}

static INLINE
UChar extract_vabits4_from_vabits8 ( Addr a, UChar vabits8 )
{
   UInt shift;
   tl_assert(VG_IS_2_ALIGNED(a));      // Must be 2-aligned
   shift = (a & 2) << 1;               // shift by 0 or 4
   vabits8 >>= shift;                  // shift the four bits to the bottom
   return 0xf & vabits8;               // mask out the rest
}

// Note that these four are only used in slow cases.  The fast cases do
// clever things like combine the auxmap check (in
// get_secmap_{read,writ}able) with alignment checks.

// *** WARNING! ***
// Any time this function is called, if it is possible that vabits2
// is equal to VA_BITS2_PARTUNTAINTED, then the corresponding entry in the
// sec-V-bits table must also be set!
static INLINE
void set_vabits2 ( Addr a, UChar vabits2 )
{
   SecMap* sm       = get_secmap_for_writing(a); // Taintgrind: only handle 32-bits
   UWord   sm_off   = SM_OFF(a);

#ifdef DBG_MEM
   // Taintgrind
//   if (vabits2 == VA_BITS2_TAINTED)
      VG_(printf)("set_vabits2 a:0x%08lx vabits2:0x%x sm->vabit8[sm_off]:0x%08x\n",
                  a, vabits2, (Int)&(sm->vabits8[sm_off]));
#endif

   insert_vabits2_into_vabits8( a, vabits2, &(sm->vabits8[sm_off]) );
}

// Needed by TNT_(instrument)
//static INLINE
UChar get_vabits2 ( Addr a )
{
   SecMap* sm       = get_secmap_for_reading(a); // Taintgrind: only handle 32-bits
   UWord   sm_off   = SM_OFF(a);
   UChar   vabits8  = sm->vabits8[sm_off];

#ifdef DBG_MEM
   // Taintgrind
   UChar result = extract_vabits2_from_vabits8(a, vabits8);
//   if (vabits2 == VA_BITS2_TAINTED)
      VG_(printf)("get_vabits2 a:0x%08lx vabits2:0x%x sm->vabit8[sm_off]:0x%08x\n",
                  a, result, (Int)&vabits8);
   return result;
#endif
   return extract_vabits2_from_vabits8(a, vabits8);
}

// *** WARNING! ***
// Any time this function is called, if it is possible that any of the
// 4 2-bit fields in vabits8 are equal to VA_BITS2_PARTUNTAINTED, then the
// corresponding entry(s) in the sec-V-bits table must also be set!
static INLINE
UChar get_vabits8_for_aligned_word32 ( Addr a )
{
   SecMap* sm       = get_secmap_for_reading(a);
   UWord   sm_off   = SM_OFF(a);
   UChar   vabits8  = sm->vabits8[sm_off];
   return vabits8;
}

static INLINE
void set_vabits8_for_aligned_word32 ( Addr a, UChar vabits8 )
{
   SecMap* sm       = get_secmap_for_writing(a);
   UWord   sm_off   = SM_OFF(a);
   sm->vabits8[sm_off] = vabits8;
}


// Forward declarations
static UWord get_sec_vbits8(Addr a);
static void  set_sec_vbits8(Addr a, UWord vbits8);

// Returns False if there was an addressability error.
// Taintgrind: skip addressability check
static INLINE
Bool set_vbits8 ( Addr a, UChar vbits8 )
{
   Bool  ok      = True;
   UChar vabits2 = get_vabits2(a);
   //if ( VA_BITS2_NOACCESS != vabits2 ) {
      // Addressable.  Convert in-register format to in-memory format.
      // Also remove any existing sec V bit entry for the byte if no
      // longer necessary.
      if      ( V_BITS8_UNTAINTED == vbits8 ) { vabits2 = VA_BITS2_UNTAINTED; }
      else if ( V_BITS8_TAINTED   == vbits8 ) { vabits2 = VA_BITS2_TAINTED;   }
      else                                    { vabits2 = VA_BITS2_PARTUNTAINTED;
                                                set_sec_vbits8(a, vbits8);  }
      set_vabits2(a, vabits2);

   //} else {
   //   // Unaddressable!  Do nothing -- when writing to unaddressable
   //   // memory it acts as a black hole, and the V bits can never be seen
   //   // again.  So we don't have to write them at all.
   //   ok = False;
   //}
   return ok;
}

// Returns False if there was an addressability error.  In that case, we put
// all defined bits into vbits8.
static INLINE
Bool get_vbits8 ( Addr a, UChar* vbits8 )
{
   Bool  ok      = True;
   UChar vabits2 = get_vabits2(a);

   // Convert the in-memory format to in-register format.
   if      ( VA_BITS2_UNTAINTED == vabits2 ) { *vbits8 = V_BITS8_UNTAINTED; }
   else if ( VA_BITS2_TAINTED   == vabits2 ) { *vbits8 = V_BITS8_TAINTED;   }
   else if ( VA_BITS2_NOACCESS  == vabits2 ) {
      *vbits8 = V_BITS8_UNTAINTED;    // Make V bits defined!
      ok = False;
   } else {
      tl_assert( VA_BITS2_PARTUNTAINTED == vabits2 );
      *vbits8 = get_sec_vbits8(a);
   }
   return ok;
}

/* --------------- Secondary V bit table ------------ */
static OSet* secVBitTable;

// Stats
static ULong sec_vbits_new_nodes = 0;
static ULong sec_vbits_updates   = 0;

// This must be a power of two;  this is checked in tnt_pre_clo_init().
// The size chosen here is a trade-off:  if the nodes are bigger (ie. cover
// a larger address range) they take more space but we can get multiple
// partially-defined bytes in one if they are close to each other, reducing
// the number of total nodes.  In practice sometimes they are clustered (eg.
// perf/bz2 repeatedly writes then reads more than 20,000 in a contiguous
// row), but often not.  So we choose something intermediate.
#define BYTES_PER_SEC_VBIT_NODE     16

// We make the table bigger if more than this many nodes survive a GC.
#define MAX_SURVIVOR_PROPORTION  0.5

// Each time we make the table bigger, we increase it by this much.
#define TABLE_GROWTH_FACTOR      2

// This defines "sufficiently stale" -- any node that hasn't been touched in
// this many GCs will be removed.
#define MAX_STALE_AGE            2

// We GC the table when it gets this many nodes in it, ie. it's effectively
// the table size.  It can change.
static Int  secVBitLimit = 1024;

// The number of GCs done, used to age sec-V-bit nodes for eviction.
// Because it's unsigned, wrapping doesn't matter -- the right answer will
// come out anyway.
static UInt GCs_done = 0;

typedef
   struct {
      Addr  a;
      UChar vbits8[BYTES_PER_SEC_VBIT_NODE];
      UInt  last_touched;
   }
   SecVBitNode;

static OSet* createSecVBitTable(void)
{
   return VG_(OSetGen_Create)( offsetof(SecVBitNode, a),
                               NULL, // use fast comparisons
                               VG_(malloc), "mc.cSVT.1 (sec VBit table)",
                               VG_(free) );
}

static void gcSecVBitTable(void)
{
   OSet*        secVBitTable2;
   SecVBitNode* n;
   Int          i, n_nodes = 0, n_survivors = 0;

   GCs_done++;

   // Create the new table.
   secVBitTable2 = createSecVBitTable();

   // Traverse the table, moving fresh nodes into the new table.
   VG_(OSetGen_ResetIter)(secVBitTable);
   while ( (n = VG_(OSetGen_Next)(secVBitTable)) ) {
      Bool keep = False;
      if ( (GCs_done - n->last_touched) <= MAX_STALE_AGE ) {
         // Keep node if it's been touched recently enough (regardless of
         // freshness/staleness).
         keep = True;
      } else {
         // Keep node if any of its bytes are non-stale.  Using
         // get_vabits2() for the lookup is not very efficient, but I don't
         // think it matters.
         for (i = 0; i < BYTES_PER_SEC_VBIT_NODE; i++) {
            if (VA_BITS2_PARTUNTAINTED == get_vabits2(n->a + i)) {
               keep = True;      // Found a non-stale byte, so keep
               break;
            }
         }
      }

      if ( keep ) {
         // Insert a copy of the node into the new table.
         SecVBitNode* n2 =
            VG_(OSetGen_AllocNode)(secVBitTable2, sizeof(SecVBitNode));
         *n2 = *n;
         VG_(OSetGen_Insert)(secVBitTable2, n2);
      }
   }

   // Get the before and after sizes.
   n_nodes     = VG_(OSetGen_Size)(secVBitTable);
   n_survivors = VG_(OSetGen_Size)(secVBitTable2);

   // Destroy the old table, and put the new one in its place.
   VG_(OSetGen_Destroy)(secVBitTable);
   secVBitTable = secVBitTable2;

   if (VG_(clo_verbosity) > 1) {
      HChar percbuf[6];
      VG_(percentify)(n_survivors, n_nodes, 1, 6, percbuf);
      VG_(message)(Vg_DebugMsg, "tnt_main.c: GC: %d nodes, %d survivors (%s)\n",
                   n_nodes, n_survivors, percbuf);
   }

   // Increase table size if necessary.
   if (n_survivors > (secVBitLimit * MAX_SURVIVOR_PROPORTION)) {
      secVBitLimit *= TABLE_GROWTH_FACTOR;
      if (VG_(clo_verbosity) > 1)
         VG_(message)(Vg_DebugMsg, "tnt_main.c: GC: increase table size to %d\n",
                      secVBitLimit);
   }
}

static UWord get_sec_vbits8(Addr a)
{
   Addr         aAligned = VG_ROUNDDN(a, BYTES_PER_SEC_VBIT_NODE);
   Int          amod     = a % BYTES_PER_SEC_VBIT_NODE;
   SecVBitNode* n        = VG_(OSetGen_Lookup)(secVBitTable, &aAligned);
   UChar        vbits8;
   tl_assert2(n, "get_sec_vbits8: no node for address %p (%p)\n", aAligned, a);
   // Shouldn't be fully defined or fully undefined -- those cases shouldn't
   // make it to the secondary V bits table.
   vbits8 = n->vbits8[amod];
   tl_assert(V_BITS8_UNTAINTED != vbits8 && V_BITS8_TAINTED != vbits8);
   return vbits8;
}

static void set_sec_vbits8(Addr a, UWord vbits8)
{
   Addr         aAligned = VG_ROUNDDN(a, BYTES_PER_SEC_VBIT_NODE);
   Int          i, amod  = a % BYTES_PER_SEC_VBIT_NODE;
   SecVBitNode* n        = VG_(OSetGen_Lookup)(secVBitTable, &aAligned);
   // Shouldn't be fully defined or fully undefined -- those cases shouldn't
   // make it to the secondary V bits table.
   tl_assert(V_BITS8_UNTAINTED != vbits8 && V_BITS8_TAINTED != vbits8);
   if (n) {
      n->vbits8[amod] = vbits8;     // update
      n->last_touched = GCs_done;
      sec_vbits_updates++;
   } else {
      // New node:  assign the specific byte, make the rest invalid (they
      // should never be read as-is, but be cautious).
      n = VG_(OSetGen_AllocNode)(secVBitTable, sizeof(SecVBitNode));
      n->a            = aAligned;
      for (i = 0; i < BYTES_PER_SEC_VBIT_NODE; i++) {
         n->vbits8[i] = V_BITS8_TAINTED;
      }
      n->vbits8[amod] = vbits8;
      n->last_touched = GCs_done;

      // Do a table GC if necessary.  Nb: do this before inserting the new
      // node, to avoid erroneously GC'ing the new node.
      if (secVBitLimit == VG_(OSetGen_Size)(secVBitTable)) {
         gcSecVBitTable();
      }

      // Insert the new node.
      VG_(OSetGen_Insert)(secVBitTable, n);
      sec_vbits_new_nodes++;

      n_secVBit_nodes = VG_(OSetGen_Size)(secVBitTable);
      if (n_secVBit_nodes > max_secVBit_nodes)
         max_secVBit_nodes = n_secVBit_nodes;
   }
}

/* --------------- Endianness helpers --------------- */

/* Returns the offset in memory of the byteno-th most significant byte
   in a wordszB-sized word, given the specified endianness. */
static INLINE UWord byte_offset_w ( UWord wordszB, Bool bigendian,
                                    UWord byteno ) {
   return bigendian ? (wordszB-1-byteno) : byteno;
}


/* --------------- Load/store slow cases. --------------- */
static
__attribute__((noinline))
void tnt_LOADV_128_or_256_slow ( /*OUT*/ULong* res,
                                Addr a, SizeT nBits, Bool bigendian )
{
   ULong  pessim[4];     /* only used when p-l-ok=yes */
   SSizeT szB            = nBits / 8;
   SSizeT szL            = szB / 8;  /* Size in Longs (64-bit units) */
   SSizeT i, j;          /* Must be signed. */
   SizeT  n_addrs_bad = 0;
   Addr   ai;
   UChar  vbits8;
   Bool   ok;

   /* Code below assumes load size is a power of two and at least 64
      bits. */
   tl_assert((szB & (szB-1)) == 0 && szL > 0);

   /* If this triggers, you probably just need to increase the size of
      the pessim array. */
   tl_assert(szL <= sizeof(pessim) / sizeof(pessim[0]));

   for (j = 0; j < szL; j++) {
      pessim[j] = V_BITS64_UNTAINTED;
      res[j] = V_BITS64_TAINTED;
   }

   /* Make up a result V word, which contains the loaded data for
      valid addresses and Defined for invalid addresses.  Iterate over
      the bytes in the word, from the most significant down to the
      least.  The vbits to return are calculated into vbits128.  Also
      compute the pessimising value to be used when
      --partial-loads-ok=yes.  n_addrs_bad is redundant (the relevant
      info can be gleaned from the pessim array) but is used as a
      cross-check. */
   for (j = szL-1; j >= 0; j--) {
      ULong vbits64    = V_BITS64_TAINTED;
      ULong pessim64   = V_BITS64_UNTAINTED;
      UWord long_index = byte_offset_w(szL, bigendian, j);
      for (i = 8-1; i >= 0; i--) {
         PROF_EVENT(31, "tnt_LOADV_128_or_256_slow(loop)");
         ai = a + 8*long_index + byte_offset_w(8, bigendian, i);
         ok = get_vbits8(ai, &vbits8);
         vbits64 <<= 8;
         // Taintgrind: Optimistically untainted?
         //vbits64 |= vbits8;
         vbits64 &= vbits8;
         if (!ok) n_addrs_bad++;
         pessim64 <<= 8;
         // Taintgrind: Optimistically untainted?
         //pessim64 |= (ok ? V_BITS8_UNTAINTED : V_BITS8_TAINTED);
         pessim64 &= (ok ? V_BITS8_UNTAINTED : V_BITS8_TAINTED);
      }
      res[long_index] = vbits64;
      pessim[long_index] = pessim64;
   }

   /* In the common case, all the addresses involved are valid, so we
      just return the computed V bits and have done. */
   if (LIKELY(n_addrs_bad == 0))
      return;

   /* If there's no possibility of getting a partial-loads-ok
      exemption, report the error and quit. */
   //if (!MC_(clo_partial_loads_ok)) {
   //   MC_(record_address_error)( VG_(get_running_tid)(), a, szB, False );
   //   return;
   //}

   /* The partial-loads-ok excemption might apply.  Find out if it
      does.  If so, don't report an addressing error, but do return
      Undefined for the bytes that are out of range, so as to avoid
      false negatives.  If it doesn't apply, just report an addressing
      error in the usual way. */

   /* Some code steps along byte strings in aligned chunks
      even when there is only a partially defined word at the end (eg,
      optimised strlen).  This is allowed by the memory model of
      modern machines, since an aligned load cannot span two pages and
      thus cannot "partially fault".

      Therefore, a load from a partially-addressible place is allowed
      if all of the following hold:
      - the command-line flag is set [by default, it isn't]
      - it's an aligned load
      - at least one of the addresses in the word *is* valid

      Since this suppresses the addressing error, we avoid false
      negatives by marking bytes undefined when they come from an
      invalid address.
   */

   /* "at least one of the addresses is invalid" */
   ok = False;
   for (j = 0; j < szL; j++)
      ok |= pessim[j] != V_BITS8_UNTAINTED;
   // Taintgrind: We're ok with ok not being ok
   //tl_assert(ok);

   if (0 == (a & (szB - 1)) && n_addrs_bad < szB) {
      /* Exemption applies.  Use the previously computed pessimising
         value and return the combined result, but don't flag an
         addressing error.  The pessimising value is Defined for valid
         addresses and Undefined for invalid addresses. */
      /* for assumption that doing bitwise or implements UifU */
      tl_assert(V_BIT_TAINTED == 1 && V_BIT_UNTAINTED == 0);
      /* (really need "UifU" here...)
         vbits[j] UifU= pessim[j]  (is pessimised by it, iow) */
      for (j = szL-1; j >= 0; j--)
         res[j] |= pessim[j];
      return;
   }

   /* Exemption doesn't apply.  Flag an addressing error in the normal
      way. */
   //MC_(record_address_error)( VG_(get_running_tid)(), a, szB, False );
}

static
#ifndef PERF_FAST_LOADV
INLINE
#endif
ULong tnt_LOADVn_slow ( Addr a, SizeT nBits, Bool bigendian )
{
   /* Make up a 64-bit result V word, which contains the loaded data for
      valid addresses and Defined for invalid addresses.  Iterate over
      the bytes in the word, from the most significant down to the
      least. */
   ULong vbits64     = V_BITS64_TAINTED;
   SizeT szB         = nBits / 8;
   SSizeT i;                        // Must be signed.
   SizeT n_addrs_bad = 0;
   Addr  ai;
   //Bool  partial_load_exemption_applies;
   UChar vbits8;
   Bool  ok;

   PROF_EVENT(30, "tnt_LOADVn_slow");

   /* ------------ BEGIN semi-fast cases ------------ */
   /* These deal quickly-ish with the common auxiliary primary map
      cases on 64-bit platforms.  Are merely a speedup hack; can be
      omitted without loss of correctness/functionality.  Note that in
      both cases the "sizeof(void*) == 8" causes these cases to be
      folded out by compilers on 32-bit platforms.  These are derived
      from LOADV64 and LOADV32.
   */
   if (LIKELY(sizeof(void*) == 8
                      && nBits == 64 && VG_IS_8_ALIGNED(a))) {
      SecMap* sm       = get_secmap_for_reading(a);
      UWord   sm_off16 = SM_OFF_16(a);
      UWord   vabits16 = ((UShort*)(sm->vabits8))[sm_off16];
#ifdef DBG_LOAD
      VG_(printf)("tnt_LOADn_slow fully t/ut 0x%lx 0x%x\n", a, vabits16);
#endif
      if (LIKELY(vabits16 == VA_BITS16_UNTAINTED))
         return V_BITS64_UNTAINTED;
      if (LIKELY(vabits16 == VA_BITS16_TAINTED))
         return V_BITS64_TAINTED;
      /* else fall into the slow case */
   }
   if (LIKELY(sizeof(void*) == 8
                      && nBits == 32 && VG_IS_4_ALIGNED(a))) {
      SecMap* sm = get_secmap_for_reading(a);
      UWord sm_off = SM_OFF(a);
      UWord vabits8 = sm->vabits8[sm_off];
      if (LIKELY(vabits8 == VA_BITS8_UNTAINTED))
         return ((UWord)0xFFFFFFFF00000000ULL | (UWord)V_BITS32_UNTAINTED);
      if (LIKELY(vabits8 == VA_BITS8_TAINTED))
         return ((UWord)0xFFFFFFFF00000000ULL | (UWord)V_BITS32_TAINTED);
      /* else fall into slow case */
   }
   /* ------------ END semi-fast cases ------------ */

   tl_assert(nBits == 64 || nBits == 32 || nBits == 16 || nBits == 8);

   for (i = szB-1; i >= 0; i--) {
      PROF_EVENT(31, "tnt_LOADVn_slow(loop)");
      ai = a + byte_offset_w(szB, bigendian, i);
      ok = get_vbits8(ai, &vbits8);
      if (!ok) n_addrs_bad++;
      vbits64 <<= 8;
      vbits64 |= vbits8;
#ifdef DBG_LOAD
      VG_(printf)("tnt_LOADn_slow loop 0x%lx 0x%x\n", ai, vbits8);
#endif
   }

   /* This is a hack which avoids producing errors for code which
      insists in stepping along byte strings in aligned word-sized
      chunks, and there is a partially defined word at the end.  (eg,
      optimised strlen).  Such code is basically broken at least WRT
      semantics of ANSI C, but sometimes users don't have the option
      to fix it, and so this option is provided.  Note it is now
      defaulted to not-engaged.

      A load from a partially-addressible place is allowed if:
      - the command-line flag is set
      - it's a word-sized, word-aligned load
      - at least one of the addresses in the word *is* valid
   */
   //partial_load_exemption_applies
      //= /*TNT_(clo_partial_loads_ok)*/ 0 && szB == VG_WORDSIZE
      //                             && VG_IS_WORD_ALIGNED(a)
      //                             && n_addrs_bad < VG_WORDSIZE;

//   Taintgrind: TODO
//   if (n_addrs_bad > 0 && !partial_load_exemption_applies)
//      TNT_(record_address_error)( VG_(get_running_tid)(), a, szB, False );

#ifdef DBG_LOAD
   if( nBits == 8 &&
       vbits64
//       || (a & 0x80000000)
       )
      VG_(printf)("tnt_LOADn_slow 0x%08lx 0x%llx\n", a, vbits64);
//      VG_(printf)("tnt_LOADn_slow 0x%08lx\n", a);
#endif

   return vbits64;
}


static
#ifndef PERF_FAST_STOREV
INLINE
#endif
void tnt_STOREVn_slow ( Addr a, SizeT nBits, ULong vbytes, Bool bigendian )
{
   SizeT szB = nBits / 8;
   SizeT i, n_addrs_bad = 0;
   UChar vbits8;
   Addr  ai;
   Bool  ok;

   PROF_EVENT(35, "tnt_STOREVn_slow");


   /* ------------ BEGIN semi-fast cases ------------ */
   /* These deal quickly-ish with the common auxiliary primary map
      cases on 64-bit platforms.  Are merely a speedup hack; can be
      omitted without loss of correctness/functionality.  Note that in
      both cases the "sizeof(void*) == 8" causes these cases to be
      folded out by compilers on 32-bit platforms.  These are derived
      from STOREV64 and STOREV32.
   */
   if (LIKELY(sizeof(void*) == 8
                      && nBits == 64 && VG_IS_8_ALIGNED(a))) {
      SecMap* sm       = get_secmap_for_reading(a);
      UWord   sm_off16 = SM_OFF_16(a);
      UWord   vabits16 = ((UShort*)(sm->vabits8))[sm_off16];
      if (LIKELY( !is_distinguished_sm(sm) &&
                          (VA_BITS16_UNTAINTED == vabits16 ||
                           VA_BITS16_TAINTED   == vabits16) )) {
         /* Handle common case quickly: a is suitably aligned, */
         /* is mapped, and is addressible. */
         // Convert full V-bits in register to compact 2-bit form.
         if (LIKELY(V_BITS64_UNTAINTED == vbytes)) {
#ifdef DBG_STORE
            VG_(printf)("tnt_STOREVn_slow likely untainted 0x%llx 0x%llx\n", a, nBits);
#endif
            ((UShort*)(sm->vabits8))[sm_off16] = (UShort)VA_BITS16_UNTAINTED;
            return;
         } else if (V_BITS64_TAINTED == vbytes) {
#ifdef DBG_STORE
            VG_(printf)("tnt_STOREVn_slow tainted 0x%llx 0x%llx\n", a, nBits);
#endif
            ((UShort*)(sm->vabits8))[sm_off16] = (UShort)VA_BITS16_TAINTED;
            return;
         }
         /* else fall into the slow case */
      }
      /* else fall into the slow case */
   }
   if (LIKELY(sizeof(void*) == 8
                      && nBits == 32 && VG_IS_4_ALIGNED(a))) {
      SecMap* sm      = get_secmap_for_reading(a);
      UWord   sm_off  = SM_OFF(a);
      UWord   vabits8 = sm->vabits8[sm_off];
      if (LIKELY( !is_distinguished_sm(sm) &&
                          (VA_BITS8_UNTAINTED   == vabits8 ||
                           VA_BITS8_TAINTED == vabits8) )) {
         /* Handle common case quickly: a is suitably aligned, */
         /* is mapped, and is addressible. */
         // Convert full V-bits in register to compact 2-bit form.
         if (LIKELY(V_BITS32_UNTAINTED == (vbytes & 0xFFFFFFFF))) {
            sm->vabits8[sm_off] = VA_BITS8_UNTAINTED;
            return;
         } else if (V_BITS32_TAINTED == (vbytes & 0xFFFFFFFF)) {
#ifdef DBG_STORE
            VG_(printf)("tnt_STOREVn_slow tainted ffffffff 0x%lx 0x%llx\n", a, nBits);
#endif
            sm->vabits8[sm_off] = VA_BITS8_TAINTED;
            return;
         }
         /* else fall into the slow case */
      }
      /* else fall into the slow case */
   }
   /* ------------ END semi-fast cases ------------ */

   tl_assert(nBits == 64 || nBits == 32 || nBits == 16 || nBits == 8);

   /* Dump vbytes in memory, iterating from least to most significant
      byte.  At the same time establish addressibility of the location. */
   for (i = 0; i < szB; i++) {
      PROF_EVENT(36, "tnt_STOREVn_slow(loop)");
      ai     = a + byte_offset_w(szB, bigendian, i);
      vbits8 = vbytes & 0xff;
      ok     = set_vbits8(ai, vbits8);
      if (!ok) n_addrs_bad++;
      vbytes >>= 8;
#ifdef DBG_STORE
      VG_(printf)("tnt_STOREVn_slow loop 0x%lx 0x%x ok %d\n", ai, vbits8, ok);
#endif
   }

   /* If an address error has happened, report it. */
   // Taintgrind: TODO
//   if (n_addrs_bad > 0)
//      TNT_(record_address_error)( VG_(get_running_tid)(), a, szB, True );
}
                                                                 

/*------------------------------------------------------------*/
/*--- Setting permissions over address ranges.             ---*/
/*------------------------------------------------------------*/

static void set_address_range_perms ( Addr a, SizeT lenT, UWord vabits16,
                                      UWord dsm_num )
{
   UWord    sm_off, sm_off16;
   UWord    vabits2 = vabits16 & 0x3;
   SizeT    lenA, lenB, len_to_next_secmap;
   Addr     aNext;
   SecMap*  sm;
   SecMap** sm_ptr;
   SecMap*  example_dsm;

   PROF_EVENT(150, "set_address_range_perms");

   /* Check the V+A bits make sense. */
   tl_assert(VA_BITS16_NOACCESS  == vabits16 ||
             VA_BITS16_TAINTED   == vabits16 ||
             VA_BITS16_UNTAINTED == vabits16);

   // This code should never write PDBs;  ensure this.  (See comment above
   // set_vabits2().)
   tl_assert(VA_BITS2_PARTUNTAINTED != vabits2);

   if (lenT == 0)
      return;

   if (lenT > 256 * 1024 * 1024) {
      if (VG_(clo_verbosity) > 0 && !VG_(clo_xml)) {
         const HChar* s = "unknown???";
         if (vabits16 == VA_BITS16_NOACCESS ) s = "noaccess";
         if (vabits16 == VA_BITS16_TAINTED  ) s = "tainted";
         if (vabits16 == VA_BITS16_UNTAINTED) s = "untainted";
         VG_(message)(Vg_UserMsg, "Warning: set address range perms: "
                                  "large range [0x%lx, 0x%lx) (%s)\n",
                                  a, a + lenT, s);
      }
   }

#ifndef PERF_FAST_SARP
   /*------------------ debug-only case ------------------ */
   {
      // Endianness doesn't matter here because all bytes are being set to
      // the same value.
      // Nb: We don't have to worry about updating the sec-V-bits table
      // after these set_vabits2() calls because this code never writes
      // VA_BITS2_PARTUNTAINTED values.
      SizeT i;
      for (i = 0; i < lenT; i++) {
         set_vabits2(a + i, vabits2);
      }
      return;
   }
#endif

   /*------------------ standard handling ------------------ */

   /* Get the distinguished secondary that we might want
      to use (part of the space-compression scheme). */
   example_dsm = &sm_distinguished[dsm_num];

   // Break up total length (lenT) into two parts:  length in the first
   // sec-map (lenA), and the rest (lenB);   lenT == lenA + lenB.
   aNext = start_of_this_sm(a) + SM_SIZE;
   len_to_next_secmap = aNext - a;
   if ( lenT <= len_to_next_secmap ) {
      // Range entirely within one sec-map.  Covers almost all cases.
      PROF_EVENT(151, "set_address_range_perms-single-secmap");
      lenA = lenT;
      lenB = 0;
   } else if (is_start_of_sm(a)) {
      // Range spans at least one whole sec-map, and starts at the beginning
      // of a sec-map; skip to Part 2.
      PROF_EVENT(152, "set_address_range_perms-startof-secmap");
      lenA = 0;
      lenB = lenT;
      goto part2;
   } else {
      // Range spans two or more sec-maps, first one is partial.
      PROF_EVENT(153, "set_address_range_perms-multiple-secmaps");
      lenA = len_to_next_secmap;
      lenB = lenT - lenA;
   }

#ifdef DBG_MEM
   // Taintgrind
   VG_(printf)("set_address_range_perms(0) lenA:0x%x lenB:0x%x\n", (Int)lenA, (Int)lenB);
#endif

   //------------------------------------------------------------------------
   // Part 1: Deal with the first sec_map.  Most of the time the range will be
   // entirely within a sec_map and this part alone will suffice.  Also,
   // doing it this way lets us avoid repeatedly testing for the crossing of
   // a sec-map boundary within these loops.
   //------------------------------------------------------------------------

   // If it's distinguished, make it undistinguished if necessary.
   sm_ptr = get_secmap_ptr(a);
   if (is_distinguished_sm(*sm_ptr)) {
      if (*sm_ptr == example_dsm) {
         // Sec-map already has the V+A bits that we want, so skip.
         PROF_EVENT(154, "set_address_range_perms-dist-sm1-quick");
         a    = aNext;
         lenA = 0;
      } else {
         PROF_EVENT(155, "set_address_range_perms-dist-sm1");
         *sm_ptr = copy_for_writing(*sm_ptr);
      }
   }
   sm = *sm_ptr;

   // 1 byte steps
   while (True) {
      if (VG_IS_8_ALIGNED(a)) break;
      if (lenA < 1)           break;
      PROF_EVENT(156, "set_address_range_perms-loop1a");
      sm_off = SM_OFF(a);

#ifdef DBG_MEM
      // Taintgrind
      VG_(printf)("set_address_range_perms(1.1) a:0x%08lx vabits2:0x%lx sm->vabit8[sm_off]:0x%08x\n",
                  a, vabits2, (Int)&(sm->vabits8[sm_off]));
#endif
      insert_vabits2_into_vabits8( a, vabits2, &(sm->vabits8[sm_off]) );
      a    += 1;
      lenA -= 1;
   }
   // 8-aligned, 8 byte steps
   while (True) {
      if (lenA < 8) break;
      PROF_EVENT(157, "set_address_range_perms-loop8a");
      sm_off16 = SM_OFF_16(a);

#ifdef DBG_MEM
      // Taintgrind
      VG_(printf)("set_address_range_perms(1.2) sm->vabits8:0x%08x sm_off16:0x%lx vabits16:0x%08lx\n",
                 (Int) ((UShort*)(sm->vabits8)), sm_off16, vabits16);
#endif
      ((UShort*)(sm->vabits8))[sm_off16] = vabits16;
      a    += 8;
      lenA -= 8;
   }
   // 1 byte steps
   while (True) {
      if (lenA < 1) break;
      PROF_EVENT(158, "set_address_range_perms-loop1b");
      sm_off = SM_OFF(a);

#ifdef DBG_MEM
      // Taintgrind
      VG_(printf)("set_address_range_perms(1.3) a:0x%08lx vabits2:0x%lx sm->vabits8[sm_off]:0x%08x\n",
                  a, vabits2, (Int)&(sm->vabits8[sm_off]));
#endif
      insert_vabits2_into_vabits8( a, vabits2, &(sm->vabits8[sm_off]) );
      a    += 1;
      lenA -= 1;
   }

   // We've finished the first sec-map.  Is that it?
   if (lenB == 0)
      return;

   //------------------------------------------------------------------------
   // Part 2: Fast-set entire sec-maps at a time.
   //------------------------------------------------------------------------
  part2:
   // 64KB-aligned, 64KB steps.
   // Nb: we can reach here with lenB < SM_SIZE
   tl_assert(0 == lenA);
   while (True) {
      if (lenB < SM_SIZE) break;
      tl_assert(is_start_of_sm(a));
      PROF_EVENT(159, "set_address_range_perms-loop64K");
      sm_ptr = get_secmap_ptr(a);
      if (!is_distinguished_sm(*sm_ptr)) {
         PROF_EVENT(160, "set_address_range_perms-loop64K-free-dist-sm");
         // Free the non-distinguished sec-map that we're replacing.  This
         // case happens moderately often, enough to be worthwhile.
         VG_(am_munmap_valgrind)((Addr)*sm_ptr, sizeof(SecMap));
      }
      update_SM_counts(*sm_ptr, example_dsm);
      // Make the sec-map entry point to the example DSM
      *sm_ptr = example_dsm;
      lenB -= SM_SIZE;
      a    += SM_SIZE;
   }

   // We've finished the whole sec-maps.  Is that it?
   if (lenB == 0)
      return;

   //------------------------------------------------------------------------
   // Part 3: Finish off the final partial sec-map, if necessary.
   //------------------------------------------------------------------------

   tl_assert(is_start_of_sm(a) && lenB < SM_SIZE);

   // If it's distinguished, make it undistinguished if necessary.
   sm_ptr = get_secmap_ptr(a);
   if (is_distinguished_sm(*sm_ptr)) {
      if (*sm_ptr == example_dsm) {
         // Sec-map already has the V+A bits that we want, so stop.
         PROF_EVENT(161, "set_address_range_perms-dist-sm2-quick");
         return;
      } else {
         PROF_EVENT(162, "set_address_range_perms-dist-sm2");
         *sm_ptr = copy_for_writing(*sm_ptr);
      }
   }
   sm = *sm_ptr;

   // 8-aligned, 8 byte steps
   while (True) {
      if (lenB < 8) break;
      PROF_EVENT(163, "set_address_range_perms-loop8b");
      sm_off16 = SM_OFF_16(a);
      ((UShort*)(sm->vabits8))[sm_off16] = vabits16;
      a    += 8;
      lenB -= 8;
   }
   // 1 byte steps
   while (True) {
      if (lenB < 1) return;
      PROF_EVENT(164, "set_address_range_perms-loop1c");
      sm_off = SM_OFF(a);
      insert_vabits2_into_vabits8( a, vabits2, &(sm->vabits8[sm_off]) );
      a    += 1;
      lenB -= 1;
   }
}


/* --- Set permissions for arbitrary address ranges --- */

void TNT_(make_mem_noaccess) ( Addr a, SizeT len )
{
   PROF_EVENT(40, "TNT_(make_mem_noaccess)");
//   DEBUG("TNT_(make_mem_noaccess)(%p, %lu)\n", a, len);
   set_address_range_perms ( a, len, VA_BITS16_NOACCESS, SM_DIST_NOACCESS );
//   if (UNLIKELY( TNT_(clo_tnt_level) == 3 ))
//      ocache_sarp_Clear_Origins ( a, len );
}

void TNT_(make_mem_tainted) ( Addr a, SizeT len )//1608
{
   PROF_EVENT(42, "TNT_(make_mem_undefined)");
//   DEBUG("TNT_(make_mem_undefined)(%p, %lu)\n", a, len);
   set_address_range_perms ( a, len, VA_BITS16_TAINTED, SM_DIST_TAINTED );
//   if (UNLIKELY( TNT_(clo_tnt_level) == 3 ))
//      ocache_sarp_Clear_Origins ( a, len );

   // SMT2
   if ( TNT_(clo_smt2) )
   {
      UInt i = 0;
      for ( ; i<len; i++ )
      {
         VG_(printf)("(declare-fun byte%d () (_ BitVec 8))\n", i);
         VG_(printf)("(declare-fun a%lx () (_ BitVec 8))\n", a+i);
         VG_(printf)("(assert (= byte%d a%lx))\n", i, a+i);
      }
   }
}

void TNT_(make_mem_untainted) ( Addr a, SizeT len )
{
   PROF_EVENT(42, "TNT_(make_mem_untainted)");
//   DEBUG("TNT_(make_mem_untainted)(%p, %lu)\n", a, len);
   set_address_range_perms ( a, len, VA_BITS16_UNTAINTED, SM_DIST_UNTAINTED );
//   if (UNLIKELY( TNT_(clo_tnt_level) == 3 ))
//      ocache_sarp_Clear_Origins ( a, len );
}


/* --- Block-copy permissions (needed for implementing realloc() and
       sys_mremap). --- */

void TNT_(copy_address_range_state) ( Addr src, Addr dst, SizeT len )
{
   SizeT i, j;
   UChar vabits2, vabits8;
   Bool  aligned, nooverlap;

#ifdef DBG_COPY_ADDR_RANGE_STATE
   VG_(printf)( "copy_addr_range_state 0x%x 0x%x 0x%x\n", (Int)src, (Int)dst, (Int)len );
#endif
//   DEBUG("TNT_(copy_address_range_state)\n");
   PROF_EVENT(50, "TNT_(copy_address_range_state)");

   if (len == 0 || src == dst)
      return;

   aligned   = VG_IS_4_ALIGNED(src) && VG_IS_4_ALIGNED(dst);
   nooverlap = src+len <= dst || dst+len <= src;

   if (nooverlap && aligned) {

      /* Vectorised fast case, when no overlap and suitably aligned */
      /* vector loop */
      i = 0;
      while (len >= 4) {
         vabits8 = get_vabits8_for_aligned_word32( src+i );
         set_vabits8_for_aligned_word32( dst+i, vabits8 );
         if (LIKELY(VA_BITS8_UNTAINTED == vabits8
                            || VA_BITS8_TAINTED == vabits8
                            || VA_BITS8_NOACCESS == vabits8)) {
            /* do nothing */
         } else {
            /* have to copy secondary map info */
            if (VA_BITS2_PARTUNTAINTED == get_vabits2( src+i+0 ))
               set_sec_vbits8( dst+i+0, get_sec_vbits8( src+i+0 ) );
            if (VA_BITS2_PARTUNTAINTED == get_vabits2( src+i+1 ))
               set_sec_vbits8( dst+i+1, get_sec_vbits8( src+i+1 ) );
            if (VA_BITS2_PARTUNTAINTED == get_vabits2( src+i+2 ))
               set_sec_vbits8( dst+i+2, get_sec_vbits8( src+i+2 ) );
            if (VA_BITS2_PARTUNTAINTED == get_vabits2( src+i+3 ))
               set_sec_vbits8( dst+i+3, get_sec_vbits8( src+i+3 ) );
         }
         i += 4;
         len -= 4;
      }
      /* fixup loop */
      while (len >= 1) {
         vabits2 = get_vabits2( src+i );
         set_vabits2( dst+i, vabits2 );
         if (VA_BITS2_PARTUNTAINTED == vabits2) {
            set_sec_vbits8( dst+i, get_sec_vbits8( src+i ) );
         }
         i++;
         len--;
      }

   } else {

      /* We have to do things the slow way */
      if (src < dst) {
         for (i = 0, j = len-1; i < len; i++, j--) {
            PROF_EVENT(51, "TNT_(copy_address_range_state)(loop)");
            vabits2 = get_vabits2( src+j );
            set_vabits2( dst+j, vabits2 );
            if (VA_BITS2_PARTUNTAINTED == vabits2) {
               set_sec_vbits8( dst+j, get_sec_vbits8( src+j ) );
            }
         }
      }

      if (src > dst) {
         for (i = 0; i < len; i++) {
            PROF_EVENT(52, "TNT_(copy_address_range_state)(loop)");
            vabits2 = get_vabits2( src+i );
            set_vabits2( dst+i, vabits2 );
            if (VA_BITS2_PARTUNTAINTED == vabits2) {
               set_sec_vbits8( dst+i, get_sec_vbits8( src+i ) );
            }
         }
      }
   }

}

/*static
void tnt_new_mem_mmap ( Addr a, SizeT len, Bool rr, Bool ww, Bool xx,
                       ULong di_handle )
{
   if (rr || ww || xx)
      TNT_(make_mem_defined)(a, len);
   else
      TNT_(make_mem_noaccess)(a, len);
}*/


//void TNT_(helperc_MAKE_STACK_UNINIT) ( Addr base, UWord len, Addr nia )
//{
//   //UInt otag;
//   tl_assert(sizeof(UWord) == sizeof(SizeT));
//   if (0)
//      VG_(printf)("helperc_MAKE_STACK_UNINIT (%#lx,%lu,nia=%#lx)\n",
//                  base, len, nia );
//
///*   if (UNLIKELY( MC_(clo_mc_level) == 3 )) {
//      UInt ecu = convert_nia_to_ecu ( nia );
//      tl_assert(VG_(is_plausible_ECU)(ecu));
//      otag = ecu | MC_OKIND_STACK;
//   } else {*/
//      tl_assert(nia == 0);
//      //otag = 0;
///*   }*/
//
//   /* Idea is: go fast when
//         * 8-aligned and length is 128
//         * the sm is available in the main primary map
//         * the address range falls entirely with a single secondary map
//      If all those conditions hold, just update the V+A bits by writing
//      directly into the vabits array.  (If the sm was distinguished, this
//      will make a copy and then write to it.)
//   */
//
//   if (LIKELY( len == 128 && VG_IS_8_ALIGNED(base) )) {
//      /* Now we know the address range is suitably sized and aligned. */
//      UWord a_lo = (UWord)(base);
//      UWord a_hi = (UWord)(base + 128 - 1);
//      tl_assert(a_lo < a_hi);             // paranoia: detect overflow
//      if (a_hi <= MAX_PRIMARY_ADDRESS) {
//         // Now we know the entire range is within the main primary map.
//         SecMap* sm    = get_secmap_for_writing_low(a_lo);
//         SecMap* sm_hi = get_secmap_for_writing_low(a_hi);
//         /* Now we know that the entire address range falls within a
//            single secondary map, and that that secondary 'lives' in
//            the main primary map. */
//         if (LIKELY(sm == sm_hi)) {
//            // Finally, we know that the range is entirely within one secmap.
//            UWord   v_off = SM_OFF(a_lo);
//            UShort* p     = (UShort*)(&sm->vabits8[v_off]);
//            p[ 0] = VA_BITS16_TAINTED;
//            p[ 1] = VA_BITS16_TAINTED;
//            p[ 2] = VA_BITS16_TAINTED;
//            p[ 3] = VA_BITS16_TAINTED;
//            p[ 4] = VA_BITS16_TAINTED;
//            p[ 5] = VA_BITS16_TAINTED;
//            p[ 6] = VA_BITS16_TAINTED;
//            p[ 7] = VA_BITS16_TAINTED;
//            p[ 8] = VA_BITS16_TAINTED;
//            p[ 9] = VA_BITS16_TAINTED;
//            p[10] = VA_BITS16_TAINTED;
//            p[11] = VA_BITS16_TAINTED;
//            p[12] = VA_BITS16_TAINTED;
//            p[13] = VA_BITS16_TAINTED;
//            p[14] = VA_BITS16_TAINTED;
//            p[15] = VA_BITS16_TAINTED;
//            return;
//         }
//      }
//   }
//
//   /* 288 bytes (36 ULongs) is the magic value for ELF ppc64. */
//   if (LIKELY( len == 288 && VG_IS_8_ALIGNED(base) )) {
//      /* Now we know the address range is suitably sized and aligned. */
//      UWord a_lo = (UWord)(base);
//      UWord a_hi = (UWord)(base + 288 - 1);
//      tl_assert(a_lo < a_hi);             // paranoia: detect overflow
//      if (a_hi <= MAX_PRIMARY_ADDRESS) {
//         // Now we know the entire range is within the main primary map.
//         SecMap* sm    = get_secmap_for_writing_low(a_lo);
//         SecMap* sm_hi = get_secmap_for_writing_low(a_hi);
//         /* Now we know that the entire address range falls within a
//            single secondary map, and that that secondary 'lives' in
//            the main primary map. */
//         if (LIKELY(sm == sm_hi)) {
//            // Finally, we know that the range is entirely within one secmap.
//            UWord   v_off = SM_OFF(a_lo);
//            UShort* p     = (UShort*)(&sm->vabits8[v_off]);
//            p[ 0] = VA_BITS16_TAINTED;
//            p[ 1] = VA_BITS16_TAINTED;
//            p[ 2] = VA_BITS16_TAINTED;
//            p[ 3] = VA_BITS16_TAINTED;
//            p[ 4] = VA_BITS16_TAINTED;
//            p[ 5] = VA_BITS16_TAINTED;
//            p[ 6] = VA_BITS16_TAINTED;
//            p[ 7] = VA_BITS16_TAINTED;
//            p[ 8] = VA_BITS16_TAINTED;
//            p[ 9] = VA_BITS16_TAINTED;
//            p[10] = VA_BITS16_TAINTED;
//            p[11] = VA_BITS16_TAINTED;
//            p[12] = VA_BITS16_TAINTED;
//            p[13] = VA_BITS16_TAINTED;
//            p[14] = VA_BITS16_TAINTED;
//            p[15] = VA_BITS16_TAINTED;
//            p[16] = VA_BITS16_TAINTED;
//            p[17] = VA_BITS16_TAINTED;
//            p[18] = VA_BITS16_TAINTED;
//            p[19] = VA_BITS16_TAINTED;
//            p[20] = VA_BITS16_TAINTED;
//            p[21] = VA_BITS16_TAINTED;
//            p[22] = VA_BITS16_TAINTED;
//            p[23] = VA_BITS16_TAINTED;
//            p[24] = VA_BITS16_TAINTED;
//            p[25] = VA_BITS16_TAINTED;
//            p[26] = VA_BITS16_TAINTED;
//            p[27] = VA_BITS16_TAINTED;
//            p[28] = VA_BITS16_TAINTED;
//            p[29] = VA_BITS16_TAINTED;
//            p[30] = VA_BITS16_TAINTED;
//            p[31] = VA_BITS16_TAINTED;
//            p[32] = VA_BITS16_TAINTED;
//            p[33] = VA_BITS16_TAINTED;
//            p[34] = VA_BITS16_TAINTED;
//            p[35] = VA_BITS16_TAINTED;
//            return;
//         }
//      }
//   }
//
//   /* else fall into slow case */
////   TNT_(make_mem_undefined_w_otag)(base, len, otag);
//   TNT_(make_mem_tainted)(base, len);
//}


/*------------------------------------------------------------*/
/*--- Functions called directly from generated code:       ---*/
/*--- Load/store handlers.                                 ---*/
/*------------------------------------------------------------*/

/* If any part of '_a' indicated by the mask is 1, either '_a' is not
   naturally '_sz/8'-aligned, or it exceeds the range covered by the
   primary map.  This is all very tricky (and important!), so let's
   work through the maths by hand (below), *and* assert for these
   values at startup. */
#define MASK(_szInBytes) \
   ( ~((0x10000UL-(_szInBytes)) | ((N_PRIMARY_MAP-1) << 16)) )

/* MASK only exists so as to define this macro. */
#define UNALIGNED_OR_HIGH(_a,_szInBits) \
   ((_a) & MASK((_szInBits>>3)))

/* ------------------------ Size = 16 ------------------------ */

static INLINE
void tnt_LOADV_128_or_256 ( /*OUT*/ULong* res,
                           Addr a, SizeT nBits, Bool isBigEndian )
{
   PROF_EVENT(200, "tnt_LOADV_128_or_256");

#ifndef PERF_FAST_LOADV
   tnt_LOADV_128_or_256_slow( res, a, nBits, isBigEndian );
   return;
#else
   {
      UWord   sm_off16, vabits16, j;
      UWord   nBytes  = nBits / 8;
      UWord   nULongs = nBytes / 8;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,nBits) )) {
         PROF_EVENT(201, "tnt_LOADV_128_or_256-slow1");
         tnt_LOADV_128_or_256_slow( res, a, nBits, isBigEndian );
         return;
      }

      /* Handle common cases quickly: a (and a+8 and a+16 etc.) is
         suitably aligned, is mapped, and addressible. */
      for (j = 0; j < nULongs; j++) {
         sm       = get_secmap_for_reading_low(a + 8*j);
         sm_off16 = SM_OFF_16(a + 8*j);
         vabits16 = ((UShort*)(sm->vabits8))[sm_off16];

         // Convert V bits from compact memory form to expanded
         // register form.
         if (LIKELY(vabits16 == VA_BITS16_UNTAINTED)) {
            res[j] = V_BITS64_UNTAINTED;
         } else if (LIKELY(vabits16 == VA_BITS16_TAINTED)) {
            res[j] = V_BITS64_TAINTED;
         } else {
            /* Slow case: some block of 8 bytes are not all-defined or
               all-undefined. */
            PROF_EVENT(202, "tnt_LOADV_128_or_256-slow2");
            tnt_LOADV_128_or_256_slow( res, a, nBits, isBigEndian );
            return;
         }
      }
      return;
   }
#endif
}

VG_REGPARM(2) void TNT_(helperc_LOADV256be) ( /*OUT*/V256* res, Addr a )
{
   tnt_LOADV_128_or_256(&res->w64[0], a, 256, True);
}
VG_REGPARM(2) void TNT_(helperc_LOADV256le) ( /*OUT*/V256* res, Addr a )
{
   tnt_LOADV_128_or_256(&res->w64[0], a, 256, False);
}

VG_REGPARM(2) void TNT_(helperc_LOADV128be) ( /*OUT*/V128* res, Addr a )
{
   tnt_LOADV_128_or_256(&res->w64[0], a, 128, True);
}
VG_REGPARM(2) void TNT_(helperc_LOADV128le) ( /*OUT*/V128* res, Addr a )
{
   tnt_LOADV_128_or_256(&res->w64[0], a, 128, False);
}

/* ------------------------ Size = 8 ------------------------ */

static INLINE
ULong tnt_LOADV64 ( Addr a, Bool isBigEndian )
{
   PROF_EVENT(200, "tnt_LOADV64");
#ifdef DBG_LOAD
   VG_(printf)("tnt_LOADV64 0x%lx\n", a);
#endif

#ifndef PERF_FAST_LOADV
   return tnt_LOADVn_slow( a, 64, isBigEndian );
#else
   {
      UWord   sm_off16, vabits16;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,64) )) {
         PROF_EVENT(201, "tnt_LOADV64-slow1");
         return (ULong)tnt_LOADVn_slow( a, 64, isBigEndian );
      }

      sm       = get_secmap_for_reading_low(a);
      sm_off16 = SM_OFF_16(a);
      vabits16 = ((UShort*)(sm->vabits8))[sm_off16];

      // Handle common case quickly: a is suitably aligned, is mapped, and
      // addressible.
      // Convert V bits from compact memory form to expanded register form.
      if (LIKELY(vabits16 == VA_BITS16_UNTAINTED)) {
         return V_BITS64_UNTAINTED;
      } else if (LIKELY(vabits16 == VA_BITS16_TAINTED)) {
         return V_BITS64_TAINTED;
      } else {
         /* Slow case: the 8 bytes are not all-defined or all-undefined. */
         PROF_EVENT(202, "tnt_LOADV64-slow2");
         return tnt_LOADVn_slow( a, 64, isBigEndian );
      }
   }
#endif
}

VG_REGPARM(1) ULong TNT_(helperc_LOADV64be) ( Addr a )
{
   return tnt_LOADV64(a, True);
}
VG_REGPARM(1) ULong TNT_(helperc_LOADV64le) ( Addr a )
{
   ULong result = tnt_LOADV64(a, False);

#ifdef DBG_LOAD
   // Taintgrind
   if( result
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_LOADV64le) 0x%08lx 0x%llx\n", a, result);
//      VG_(printf)("TNT_(helperc_LOADV64le) 64 0x%08lx\n", a);
#endif

   return result;
//   return tnt_LOADV64(a, False);
}


static INLINE
void tnt_STOREV64 ( Addr a, ULong vbits64, Bool isBigEndian )
{
   PROF_EVENT(210, "tnt_STOREV64");

#ifndef PERF_FAST_STOREV
   // XXX: this slow case seems to be marginally faster than the fast case!
   // Investigate further.
   tnt_STOREVn_slow( a, 64, vbits64, isBigEndian );
#else
   {
      UWord   sm_off16, vabits16;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,64) )) {
         PROF_EVENT(211, "tnt_STOREV64-slow1");
#ifdef DBG_STORE
         VG_(printf)("tnt_STOREV64 unlikely 0x%lx 0x%llx\n", a, vbits64);
#endif
         tnt_STOREVn_slow( a, 64, vbits64, isBigEndian );
         return;
      }
#ifdef DBG_STORE
      VG_(printf)("tnt_STOREV64 0x%08lx 0x%llx\n", a, vbits64);
#endif

      sm       = get_secmap_for_reading_low(a);
      sm_off16 = SM_OFF_16(a);
      vabits16 = ((UShort*)(sm->vabits8))[sm_off16];

      if (LIKELY( !is_distinguished_sm(sm) &&
                          (VA_BITS16_UNTAINTED   == vabits16 ||
                           VA_BITS16_TAINTED == vabits16) ))
      {
         /* Handle common case quickly: a is suitably aligned, */
         /* is mapped, and is addressible. */
         // Convert full V-bits in register to compact 2-bit form.
         if (V_BITS64_UNTAINTED == vbits64) {
            ((UShort*)(sm->vabits8))[sm_off16] = (UShort)VA_BITS16_UNTAINTED;
         } else if (V_BITS64_TAINTED == vbits64) {
            ((UShort*)(sm->vabits8))[sm_off16] = (UShort)VA_BITS16_TAINTED;
#ifdef DBG_STORE
            VG_(printf)("tnt_STOREV64 V_BITS64_TAINTED\n");
#endif
         } else {
            /* Slow but general case -- writing partially defined bytes. */
            PROF_EVENT(212, "tnt_STOREV64-slow2");
            tnt_STOREVn_slow( a, 64, vbits64, isBigEndian );
         }
      } else {
         /* Slow but general case. */
         PROF_EVENT(213, "tnt_STOREV64-slow3");
         tnt_STOREVn_slow( a, 64, vbits64, isBigEndian );
      }
   }
#endif
}

VG_REGPARM(1) void TNT_(helperc_STOREV64be) ( Addr a, ULong vbits64 )
{
#ifdef DBG_STORE
   // Taintgrind
   if( vbits64
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_STOREV64be) 0x%08lx 0x%llx\n", a, vbits64);
//      VG_(printf)("TNT_(helperc_STOREV64be) 64 0x%08lx\n", a);
#endif

   tnt_STOREV64(a, vbits64, True);
}
VG_REGPARM(1) void TNT_(helperc_STOREV64le) ( Addr a, ULong vbits64 )
{
#ifdef DBG_STORE
   // Taintgrind
   if( vbits64
//      || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_STOREV64le) 0x%08lx 0x%llx\n", a, vbits64);
//      VG_(printf)("TNT_(helperc_STOREV64le) 64 0x%08lx\n", a);
#endif

   tnt_STOREV64(a, vbits64, False);
}

/* ------------------------ Size = 4 ------------------------ */

static INLINE
UWord tnt_LOADV32 ( Addr a, Bool isBigEndian )
{
   PROF_EVENT(220, "tnt_LOADV32");

#ifndef PERF_FAST_LOADV
   return (UWord)tnt_LOADVn_slow( a, 32, isBigEndian );
#else
   {
      UWord   sm_off, vabits8;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,32) )) {
         PROF_EVENT(221, "tnt_LOADV32-slow1");
         return (UWord)tnt_LOADVn_slow( a, 32, isBigEndian );
      }

      sm      = get_secmap_for_reading_low(a);
      sm_off  = SM_OFF(a);
      vabits8 = sm->vabits8[sm_off];

      // Handle common case quickly: a is suitably aligned, is mapped, and the
      // entire word32 it lives in is addressible.
      // Convert V bits from compact memory form to expanded register form.
      // For 64-bit platforms, set the high 32 bits of retval to 1 (undefined).
      // Almost certainly not necessary, but be paranoid.
      if (LIKELY(vabits8 == VA_BITS8_UNTAINTED)) {
         return ((UWord)0xFFFFFFFF00000000ULL | (UWord)V_BITS32_UNTAINTED);
      } else if (LIKELY(vabits8 == VA_BITS8_TAINTED)) {
         return ((UWord)0xFFFFFFFF00000000ULL | (UWord)V_BITS32_TAINTED);
      } else {
         /* Slow case: the 4 bytes are not all-defined or all-undefined. */
         PROF_EVENT(222, "tnt_LOADV32-slow2");
         return (UWord)tnt_LOADVn_slow( a, 32, isBigEndian );
      }
   }
#endif
}

VG_REGPARM(1) UWord TNT_(helperc_LOADV32be) ( Addr a )
{
   UWord result = tnt_LOADV32(a, True);

#ifdef DBG_LOAD
   // Taintgrind
   if( result
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_LOADV32be) 0x%08lx 0x%lx\n", a, result);
//      VG_(printf)("TNT_(helperc_LOADV32be) 32 0x%08lx\n", a);
#endif

   return result;
//   return tnt_LOADV32(a, True);
}

VG_REGPARM(1) UWord TNT_(helperc_LOADV32le) ( Addr a )
{
   UWord result = tnt_LOADV32(a, False);

#ifdef DBG_LOAD
   // Taintgrind
   if( result
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_LOADV32le) 0x%08lx 0x%lx\n", a, result);
//      VG_(printf)("TNT_(helperc_LOADV32le) 32 0x%08lx\n", a);
#endif

   return result;
//   return tnt_LOADV32(a, False);
}


static INLINE
void tnt_STOREV32 ( Addr a, UWord vbits32, Bool isBigEndian )
{
   PROF_EVENT(230, "tnt_STOREV32");

#ifndef PERF_FAST_STOREV
   tnt_STOREVn_slow( a, 32, (ULong)vbits32, isBigEndian );
#else
   {
      UWord   sm_off, vabits8;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,32) )) {
         PROF_EVENT(231, "tnt_STOREV32-slow1");
         tnt_STOREVn_slow( a, 32, (ULong)vbits32, isBigEndian );
         return;
      }

      sm      = get_secmap_for_reading_low(a);
      sm_off  = SM_OFF(a);
      vabits8 = sm->vabits8[sm_off];

      // Cleverness:  sometimes we don't have to write the shadow memory at
      // all, if we can tell that what we want to write is the same as what is
      // already there.  The 64/16/8 bit cases also have cleverness at this
      // point, but it works a little differently to the code below.
      if (V_BITS32_UNTAINTED == vbits32) {
         if (vabits8 == (UInt)VA_BITS8_UNTAINTED) {
            return;
         } else if (!is_distinguished_sm(sm) && VA_BITS8_TAINTED == vabits8) {
            sm->vabits8[sm_off] = (UInt)VA_BITS8_UNTAINTED;
         } else {
            // not defined/undefined, or distinguished and changing state
            PROF_EVENT(232, "tnt_STOREV32-slow2");
            tnt_STOREVn_slow( a, 32, (ULong)vbits32, isBigEndian );
         }
      } else if (V_BITS32_TAINTED == vbits32) {
         if (vabits8 == (UInt)VA_BITS8_TAINTED) {
            return;
         } else if (!is_distinguished_sm(sm) && VA_BITS8_UNTAINTED == vabits8) {
            sm->vabits8[sm_off] = (UInt)VA_BITS8_TAINTED;
         } else {
            // not defined/undefined, or distinguished and changing state
            PROF_EVENT(233, "tnt_STOREV32-slow3");
            tnt_STOREVn_slow( a, 32, (ULong)vbits32, isBigEndian );
         }
      } else {
         // Partially defined word
         PROF_EVENT(234, "tnt_STOREV32-slow4");
         tnt_STOREVn_slow( a, 32, (ULong)vbits32, isBigEndian );
      }
   }
#endif
}

VG_REGPARM(2) void TNT_(helperc_STOREV32be) ( Addr a, UWord vbits32 )
{
#ifdef DBG_STORE
   // Taintgrind
   if( vbits32
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_STOREV32be) 0x%08lx 0x%lx\n", a, vbits32);
//      VG_(printf)("TNT_(helperc_STOREV32be) 32 0x%08lx\n", a);
#endif

   tnt_STOREV32(a, vbits32, True);
}


VG_REGPARM(2) void TNT_(helperc_STOREV32le) ( Addr a, UWord vbits32 )
{
#ifdef DBG_STORE
   // Taintgrind
   if( vbits32
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_STOREV32le) 0x%08lx 0x%lx\n", a, vbits32);
//      VG_(printf)("TNT_(helperc_STOREV32le) 32 0x%08lx\n", a);
#endif

   tnt_STOREV32(a, vbits32, False);
}


/* ------------------------ Size = 2 ------------------------ */

static INLINE
UWord tnt_LOADV16 ( Addr a, Bool isBigEndian )
{
   PROF_EVENT(240, "tnt_LOADV16");

#ifndef PERF_FAST_LOADV
   return (UWord)tnt_LOADVn_slow( a, 16, isBigEndian );
#else
   {
      UWord   sm_off, vabits8;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,16) )) {
         PROF_EVENT(241, "tnt_LOADV16-slow1");
         return (UWord)tnt_LOADVn_slow( a, 16, isBigEndian );
      }

      sm      = get_secmap_for_reading_low(a);
      sm_off  = SM_OFF(a);
      vabits8 = sm->vabits8[sm_off];
      // Handle common case quickly: a is suitably aligned, is mapped, and is
      // addressible.
      // Convert V bits from compact memory form to expanded register form
      if      (vabits8 == VA_BITS8_UNTAINTED  ) { return V_BITS16_UNTAINTED;   }
      else if (vabits8 == VA_BITS8_TAINTED) { return V_BITS16_TAINTED; }
      else {
         // The 4 (yes, 4) bytes are not all-defined or all-undefined, check
         // the two sub-bytes.
         UChar vabits4 = extract_vabits4_from_vabits8(a, vabits8);
         if      (vabits4 == VA_BITS4_UNTAINTED  ) { return V_BITS16_UNTAINTED;   }
         else if (vabits4 == VA_BITS4_TAINTED) { return V_BITS16_TAINTED; }
         else {
            /* Slow case: the two bytes are not all-defined or all-undefined. */
            PROF_EVENT(242, "tnt_LOADV16-slow2");
            return (UWord)tnt_LOADVn_slow( a, 16, isBigEndian );
         }
      }
   }
#endif
}

VG_REGPARM(1) UWord TNT_(helperc_LOADV16be) ( Addr a )
{
   return tnt_LOADV16(a, True);
}
VG_REGPARM(1) UWord TNT_(helperc_LOADV16le) ( Addr a )
{
   UWord result = tnt_LOADV16(a, False);

#ifdef DBG_LOAD
   // Taintgrind
   if( result
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_LOADV16le) 0x%08lx 0x%lx\n", a, result);
//      VG_(printf)("TNT_(helperc_LOADV16le) 16 0x%08lx\n", a);
#endif

   return result;
//   return tnt_LOADV16(a, False);
}


static INLINE
void tnt_STOREV16 ( Addr a, UWord vbits16, Bool isBigEndian )
{
   PROF_EVENT(250, "tnt_STOREV16");

#ifndef PERF_FAST_STOREV
   tnt_STOREVn_slow( a, 16, (ULong)vbits16, isBigEndian );
#else
   {
      UWord   sm_off, vabits8;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,16) )) {
         PROF_EVENT(251, "tnt_STOREV16-slow1");
         tnt_STOREVn_slow( a, 16, (ULong)vbits16, isBigEndian );
         return;
      }

      sm      = get_secmap_for_reading_low(a);
      sm_off  = SM_OFF(a);
      vabits8 = sm->vabits8[sm_off];
      if (LIKELY( !is_distinguished_sm(sm) &&
                          (VA_BITS8_UNTAINTED   == vabits8 ||
                           VA_BITS8_TAINTED == vabits8) ))
      {
         /* Handle common case quickly: a is suitably aligned, */
         /* is mapped, and is addressible. */
         // Convert full V-bits in register to compact 2-bit form.
         if (V_BITS16_UNTAINTED == vbits16) {
            insert_vabits4_into_vabits8( a, VA_BITS4_UNTAINTED ,
                                         &(sm->vabits8[sm_off]) );
         } else if (V_BITS16_TAINTED == vbits16) {
            insert_vabits4_into_vabits8( a, VA_BITS4_TAINTED,
                                         &(sm->vabits8[sm_off]) );
         } else {
            /* Slow but general case -- writing partially defined bytes. */
            PROF_EVENT(252, "tnt_STOREV16-slow2");
            tnt_STOREVn_slow( a, 16, (ULong)vbits16, isBigEndian );
         }
      } else {
         /* Slow but general case. */
         PROF_EVENT(253, "tnt_STOREV16-slow3");
         tnt_STOREVn_slow( a, 16, (ULong)vbits16, isBigEndian );
      }
   }
#endif
}

VG_REGPARM(2) void TNT_(helperc_STOREV16be) ( Addr a, UWord vbits16 )
{
#ifdef DBG_STORE
   // Taintgrind
   if( vbits16
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_STOREV16be) 0x%08lx 0x%lx\n", a, vbits16);
//      VG_(printf)("TNT_(helperc_STOREV16be) 16 0x%08lx\n", a);
#endif

   tnt_STOREV16(a, vbits16, True);
}
VG_REGPARM(2) void TNT_(helperc_STOREV16le) ( Addr a, UWord vbits16 )
{
#ifdef DBG_STORE
   // Taintgrind
   if( vbits16
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_STOREV16le) 0x%08lx 0x%lx\n", a, vbits16);
//      VG_(printf)("TNT_(helperc_STOREV16le) 16 0x%08lx\n", a);
#endif

   tnt_STOREV16(a, vbits16, False);
}


/* ------------------------ Size = 1 ------------------------ */
/* Note: endianness is irrelevant for size == 1 */

VG_REGPARM(1)
UWord TNT_(helperc_LOADV8) ( Addr a )
{
   PROF_EVENT(260, "tnt_LOADV8");

#ifndef PERF_FAST_LOADV
   return (UWord)tnt_LOADVn_slow( a, 8, False/*irrelevant*/ );
#else
   {
      UWord   sm_off, vabits8;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,8) )) {
         PROF_EVENT(261, "tnt_LOADV8-slow1");
         return (UWord)tnt_LOADVn_slow( a, 8, False/*irrelevant*/ );
      }

      sm      = get_secmap_for_reading_low(a);
      sm_off  = SM_OFF(a);
      vabits8 = sm->vabits8[sm_off];
      // Convert V bits from compact memory form to expanded register form
      // Handle common case quickly: a is mapped, and the entire
      // word32 it lives in is addressible.
      if      (vabits8 == VA_BITS8_UNTAINTED  ) { return V_BITS8_UNTAINTED;   }
      else if (vabits8 == VA_BITS8_TAINTED) {

#ifdef DBG_LOAD
         // Taintgrind
         VG_(printf)("TNT_(helperc_LOADV8) 0x%08lx 0x%x\n", a, V_BITS8_TAINTED);
         //VG_(printf)("TNT_(helperc_LOADV8) 8 0x%08lx\n", a);
#endif

         return V_BITS8_TAINTED; }
      else {
         // The 4 (yes, 4) bytes are not all-defined or all-undefined, check
         // the single byte.
         UChar vabits2 = extract_vabits2_from_vabits8(a, vabits8);
         if      (vabits2 == VA_BITS2_UNTAINTED  ) { return V_BITS8_UNTAINTED;   }
         else if (vabits2 == VA_BITS2_TAINTED) {

#ifdef DBG_LOAD
         // Taintgrind
         VG_(printf)("TNT_(helperc_LOADV8) 0x%08lx 0x%x\n", a, V_BITS8_TAINTED);
         //VG_(printf)("TNT_(helperc_LOADV8) 8 0x%08lx\n", a);
#endif

         return V_BITS8_TAINTED; }
         else {
            /* Slow case: the byte is not all-defined or all-undefined. */
            PROF_EVENT(262, "tnt_LOADV8-slow2");
            return (UWord)tnt_LOADVn_slow( a, 8, False/*irrelevant*/ );
         }
      }
   }
#endif
}


VG_REGPARM(2)
void TNT_(helperc_STOREV8) ( Addr a, UWord vbits8 )
{
#ifdef DBG_STORE
   // Taintgrind
   if( vbits8
//       || a & 0x80000000
     )
      VG_(printf)("TNT_(helperc_STOREV8) 0x%08lx 0x%lx\n", a, vbits8);
//      VG_(printf)("TNT_(helperc_STOREV8) 8 0x%08lx\n", a);
#endif

   PROF_EVENT(270, "tnt_STOREV8");

#ifndef PERF_FAST_STOREV
   tnt_STOREVn_slow( a, 8, (ULong)vbits8, False/*irrelevant*/ );
#else
   {
      UWord   sm_off, vabits8;
      SecMap* sm;

      if (UNLIKELY( UNALIGNED_OR_HIGH(a,8) )) {
         PROF_EVENT(271, "tnt_STOREV8-slow1");
         tnt_STOREVn_slow( a, 8, (ULong)vbits8, False/*irrelevant*/ );
         return;
      }

      sm      = get_secmap_for_reading_low(a);
      sm_off  = SM_OFF(a);
      vabits8 = sm->vabits8[sm_off];
      if (LIKELY
            ( !is_distinguished_sm(sm) &&
              ( (VA_BITS8_UNTAINTED == vabits8 || VA_BITS8_TAINTED == vabits8)
             || (VA_BITS2_NOACCESS != extract_vabits2_from_vabits8(a, vabits8))
              )
            )
         )
      {
         /* Handle common case quickly: a is mapped, the entire word32 it
            lives in is addressible. */
         // Convert full V-bits in register to compact 2-bit form.
         if (V_BITS8_UNTAINTED == vbits8) {
            insert_vabits2_into_vabits8( a, VA_BITS2_UNTAINTED,
                                          &(sm->vabits8[sm_off]) );
         } else if (V_BITS8_TAINTED == vbits8) {
            insert_vabits2_into_vabits8( a, VA_BITS2_TAINTED,
                                          &(sm->vabits8[sm_off]) );
         } else {
            /* Slow but general case -- writing partially defined bytes. */
            PROF_EVENT(272, "tnt_STOREV8-slow2");
            tnt_STOREVn_slow( a, 8, (ULong)vbits8, False/*irrelevant*/ );
         }
      } else {
         /* Slow but general case. */
         PROF_EVENT(273, "tnt_STOREV8-slow3");
         tnt_STOREVn_slow( a, 8, (ULong)vbits8, False/*irrelevant*/ );
      }
   }
#endif
}

Int ctoi_test( HChar c ){
   switch(c){
   case '0':
   case '1':
   case '2':
   case '3':
   case '4':
   case '5':
   case '6':
   case '7':
   case '8':
   case '9':
   case 'a':
   case 'A':
   case 'b':
   case 'B':
   case 'c':
   case 'C':
   case 'd':
   case 'D':
   case 'e':
   case 'E':
   case 'f':
   case 'F':
      return 1;
   default:
      return 0;
   }
}

Int ctoi( HChar c ){
   tl_assert( ctoi_test(c) );

   switch(c){
   case '0':
      return 0;
   case '1':
      return 1;
   case '2':
      return 2;
   case '3':
      return 3;
   case '4':
      return 4;
   case '5':
      return 5;
   case '6':
      return 6;
   case '7':
      return 7;
   case '8':
      return 8;
   case '9':
      return 9;
   case 'a':
   case 'A':
      return 0xa;
   case 'b':
   case 'B':
      return 0xb;
   case 'c':
   case 'C':
      return 0xc;
   case 'd':
   case 'D':
      return 0xd;
   case 'e':
   case 'E':
      return 0xe;
   case 'f':
   case 'F':
      return 0xf;
   default: {
      tl_assert(0);
      break;
   }
   }
   return -1; // unreachable
}

Int atoi( HChar *s ){
   Int result = 0;
   Int multiplier = 1;
   Int i;

   for( i = VG_(strlen)(s)-1; i>=0; i-- ){
      tl_assert( ctoi_test( s[i] ) );
      result += multiplier * ctoi(s[i]);
      // Assume decimal
      multiplier *= 10;
   }

   return result;
}

/*-----------------------------------------------
   Helper functions for taint information flows
-------------------------------------------------*/

// tmp variables go from t0, t1, t2,..., t255
// reg variables go from r0, r4, r8,..., r320
// see libvex_guest_amd64.h
//#define TI_MAX 440
//#define RI_MAX 740 
// These arrays are initialised to 0 in TNT_(clo_post_init)
// Tmp variable indices; the MSB indicates whether it's tainted (1) or not (0)
UInt  ti[TI_MAX];
// Tmp variable values
ULong tv[TI_MAX];
// Reg variable indices; values are obtained in real-time
UInt  ri[RI_MAX];

struct   myStringArray lvar_s;
int      lvar_i[STACK_SIZE];

////////////////////////////////
// Start of SOAAP-related data
////////////////////////////////
HChar* client_binary_name = NULL;

UInt shared_fds[FD_MAX];
UInt persistent_sandbox_nesting_depth = 0;
UInt ephemeral_sandbox_nesting_depth = 0;
Bool have_created_sandbox = False;

struct myStringArray shared_vars;
UInt shared_vars_perms[VAR_MAX];
HChar* next_shared_variable_to_update = NULL;

Bool allowed_syscalls[SYSCALLS_MAX];

UInt callgate_nesting_depth = 0;
////////////////////////////////
// End of SOAAP-related data
////////////////////////////////

Int get_and_check_reg( HChar *reg ){

   Int regnum = atoi( reg );
//   if( regnum % 4 ){
//      VG_(printf)("get_and_check_tvar: regnum %d mod 4 != 0\n", regnum );
//      tl_assert( !( regnum % 4 ) );
//   }
   if( regnum >= RI_MAX ){
      VG_(printf)("get_and_check_reg: regnum %d >= %d\n", regnum, RI_MAX );
      tl_assert( regnum < RI_MAX );
   }

   return regnum;
}

Int get_and_check_tvar( HChar *tmp ){

   Int tmpnum = atoi( tmp );
   tl_assert( tmpnum < TI_MAX );
   return tmpnum;
}

void infer_client_binary_name(UInt pc) {

   if (client_binary_name == NULL) {
      DebugInfo* di = VG_(find_DebugInfo)(pc);
      if (di && VG_(strcmp)(VG_(DebugInfo_get_soname)(di), "NONE") == 0) {
         //VG_(printf)("client_binary_name: %s\n", VG_(DebugInfo_get_filename)(di));
         client_binary_name = (HChar*)VG_(malloc)("client_binary_name",sizeof(HChar)*(VG_(strlen)(VG_(DebugInfo_get_filename)(di)+1)));
         VG_(strcpy)(client_binary_name, VG_(DebugInfo_get_filename)(di));
      }  
   }

}

// If stdout is not a tty, don't highlight text
int istty = 0;

/**** 32-bit helpers ****/

// macros
// Defined in tnt_include.h
//#define _ti(ltmp) ti[ltmp] & 0x7fffffff
#define is_tainted(ltmp) (ti[ltmp] >> 31)
#define KRED "\e[31m"
#define KMAG "\e[35m"
#define KNRM "\e[0m"

#define H32_PC \
   UInt  pc = VG_(get_IP)( VG_(get_running_tid)() ); \
   HChar fnname[FNNAME_MAX]; \
   HChar aTmp[128]; \
   infer_client_binary_name(pc); \
   VG_(describe_IP) ( pc, fnname, FNNAME_MAX, NULL );

#define H64_PC \
   ULong pc = VG_(get_IP)( VG_(get_running_tid)() ); \
   HChar fnname[FNNAME_MAX]; \
   HChar aTmp[128]; \
   infer_client_binary_name(pc); \
   VG_(describe_IP) ( pc, fnname, FNNAME_MAX, NULL );

#define H_VAR \
   HChar varname[256]; \
   ThreadId tid = VG_(get_running_tid()); \
   VG_(memset)( varname, 0, 255 ); \
   enum VariableType type = 0; \
   enum VariableLocation var_loc; \
   TNT_(describe_data)(address, varname, 255, &type, &var_loc); \
   TNT_(check_var_access)(tid, varname, VAR_WRITE, type, var_loc);

#define H_EXIT_EARLY \
   if(!TNT_(do_print) && taint)  TNT_(do_print) = 1; \
   if(!TNT_(do_print))  return; \
   if(!(TNT_(clo_tainted_ins_only) && taint) && TNT_(clo_tainted_ins_only)) return;

#define H_EXIT_EARLY_LDST \
   if(!TNT_(do_print) && taint)  TNT_(do_print) = 1; \
   if(!TNT_(do_print))  return; \
   if(!(TNT_(clo_tainted_ins_only) && (taint | is_tainted(atmp))) && TNT_(clo_tainted_ins_only)) return;

#define H_WRTMP_BOOKKEEPING \
   UInt ltmp = clone->Ist.WrTmp.tmp; \
   if ( ltmp >= TI_MAX ) \
      VG_(printf)("ltmp %d\n", ltmp); \
   tl_assert( ltmp < TI_MAX ); \
   ti[ltmp]++; \
   if ( taint ) \
      ti[ltmp] |= 0x80000000; \
   else \
      ti[ltmp] &= 0x7fffffff; \
   tv[ltmp] = value;

#define H32_PRINT \
   VG_(printf)("%s | %s | 0x%x | 0x%x | ", fnname, aTmp, value, taint);

#define H32_PRINTC \
   VG_(printf)("%s%s%s | %s | 0x%x | 0x%x | ", KMAG, fnname, KNRM, aTmp, value, taint);

#define H64_PRINT \
   VG_(printf)("%s | %s | 0x%llx | 0x%llx | ", fnname, aTmp, value, taint);

#define H64_PRINTC \
   VG_(printf)("%s%s%s | %s | 0x%llx | 0x%llx | ", KMAG, fnname, KNRM, aTmp, value, taint);

#define H_SMT2( fn ) \
   if ( TNT_(clo_smt2) ) \
   { \
      TNT_(fn)(clone); \
      return; \
   }


// if <gtmp> goto <jk> dst
VG_REGPARM(3)
void TNT_(h32_exit_t) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_EXIT_EARLY
   H_SMT2(smt2_exit);
   H32_PC

   IRExpr *guard = clone->Ist.Exit.guard;
   UInt gtmp     = guard->Iex.RdTmp.tmp;
   IRConst *dst  = clone->Ist.Exit.dst;
   ULong addr    = extract_IRConst64(dst);

   tl_assert( gtmp < TI_MAX );

   if ( istty && is_tainted(gtmp) )
   {
      VG_(sprintf)( aTmp, "IF %st%d_%d%s GOTO 0x%llx",
                               KRED,
                               gtmp, _ti(gtmp),
                               KNRM,
                               addr );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "IF t%d_%d GOTO 0x%llx", gtmp, _ti(gtmp), addr );
      H32_PRINT
   }

   if ( is_tainted(gtmp) )
      VG_(printf)( "t%d_%d\n", gtmp, _ti(gtmp) );
   else
      VG_(printf)("\n");
}

// if <const> goto <jk> dst
VG_REGPARM(3)
void TNT_(h32_exit_c) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   // End of BB
}

// JMP tmp
VG_REGPARM(3)
void TNT_(h32_next_t) (
   IRExpr *clone, 
   UInt value, 
   UInt taint ) {

   H_EXIT_EARLY
   H32_PC

   UInt next = clone->Iex.RdTmp.tmp;

   tl_assert( next < TI_MAX );

   if ( istty && is_tainted(next) )
   {
      VG_(sprintf)( aTmp, "JMP %st%d_%d%s", KRED, next, ti[next], KNRM );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "JMP t%d_%d", next, ti[next] );
      H32_PRINT
   }

   if ( is_tainted(next) )
      VG_(printf)( "t%d_%d\n", next, ti[next] );
   else
      VG_(printf)("\n");
}

// JMP const 
VG_REGPARM(3)
void TNT_(h32_next_c) (
   IRExpr *clone, 
   UInt value, 
   UInt taint ) {

   // End of BB
}

// STORE <end> atmp = dtmp
VG_REGPARM(3)
void TNT_(h32_store_tt) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   IRExpr *addr = clone->Ist.Store.addr;
   IRExpr *data = clone->Ist.Store.data;
   UInt atmp = addr->Iex.RdTmp.tmp;
   UInt dtmp = data->Iex.RdTmp.tmp;

   tl_assert( atmp < TI_MAX );
   tl_assert( dtmp < TI_MAX );

   H_EXIT_EARLY_LDST
   H_SMT2(smt2_store_tt);
   H32_PC

   UInt address = tv[atmp];
   H_VAR

   if ( istty && is_tainted(dtmp) )
   {
      VG_(sprintf)( aTmp, "STORE t%d_%d = %st%d_%d%s",
                                  atmp, _ti(atmp),
                                  KRED,
                                  dtmp, _ti(dtmp),
                                  KNRM );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "STORE t%d_%d = t%d_%d",
                                  atmp, _ti(atmp),
                                  dtmp, _ti(dtmp) );
      H32_PRINT
   }

   // Information flow
   // Check if it hasn't been seen before
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(dtmp) && is_tainted(atmp) ) {
      VG_(printf)( "%s_%d <- t%d_%d", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], dtmp, _ti(dtmp) );
      VG_(printf)( "; %s_%d <*- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], atmp, _ti(atmp) );
   } else if ( is_tainted(dtmp) )
      VG_(printf)( "%s_%d <- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], dtmp, _ti(dtmp) );
   else if ( is_tainted(atmp) )
      VG_(printf)( "%s_%d <*- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], atmp, _ti(atmp) );
   else
      VG_(printf)("\n");
}

// STORE atmp = const
VG_REGPARM(3)
void TNT_(h32_store_tc) (
   IRStmt *clone,
   UInt value, 
   UInt taint ) {

   IRExpr *addr = clone->Ist.Store.addr;
   IRExpr *data = clone->Ist.Store.data;
   UInt atmp    = addr->Iex.RdTmp.tmp;
   UInt c       = extract_IRConst(data->Iex.Const.con);

   tl_assert( atmp < TI_MAX );

   H_EXIT_EARLY_LDST
   H32_PC

   UInt address = tv[atmp];
   H_VAR

   VG_(sprintf)( aTmp, "STORE t%d_%d = 0x%x", atmp, _ti(atmp), c );
   H32_PRINT

   // Information flow
   // Check if it hasn't been seen before
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(atmp) )
      VG_(printf)( "%s_%d <-*- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], atmp, _ti(atmp) );
   else
      VG_(printf)("\n");
}

// STORE const = dtmp
VG_REGPARM(3)
void TNT_(h32_store_ct) (
   IRStmt *clone,
   UInt value, 
   UInt taint ) {

   H_EXIT_EARLY
   H32_PC

   IRExpr *addr = clone->Ist.Store.addr;
   IRExpr *data = clone->Ist.Store.data;
   UInt c       = extract_IRConst(addr->Iex.Const.con);
   UInt dtmp    = data->Iex.RdTmp.tmp;

   tl_assert( dtmp < TI_MAX );

   UInt address = c;
   H_VAR

   if ( istty && is_tainted(dtmp) )
   {
      VG_(sprintf)( aTmp, "STORE 0x%x = %st%d_%d%s",
                    c, KRED, dtmp, _ti(dtmp), KNRM );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "STORE 0x%x = t%d_%d", c, dtmp, _ti(dtmp) );
      H32_PRINT
   }

   // Information flow
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(dtmp) )
      VG_(printf)( "%s_%d <- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], dtmp, _ti(dtmp) );
   else
      VG_(printf)("\n");
}

// ltmp = LOAD <ty> atmp
VG_REGPARM(3)
void TNT_(h32_load_t) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   UInt ty      = clone->Ist.WrTmp.data->Iex.Load.ty - Ity_INVALID;
   IRExpr* addr = clone->Ist.WrTmp.data->Iex.Load.addr;
   UInt atmp    = addr->Iex.RdTmp.tmp;

   tl_assert( atmp < TI_MAX );

   H_EXIT_EARLY_LDST
   H_SMT2(smt2_load_t);
   H32_PC

   UInt address = tv[atmp];
   H_VAR

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "t%d_%d = LOAD %s t%d_%d", ltmp, _ti(ltmp),
                                  IRType_string[ty], atmp, _ti(atmp) );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = LOAD %s t%d_%d", ltmp, _ti(ltmp),
                                  IRType_string[ty], atmp, _ti(atmp) );
      H32_PRINT
   }

   // Information flow
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   //lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(ltmp) && is_tainted(atmp) ) {
      VG_(printf)( "t%d_%d <- %s_%d", ltmp, _ti(ltmp), varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ] );
      VG_(printf)( "; t%d_%d <*- t%d_%d\n", ltmp, _ti(ltmp), atmp, _ti(atmp) );
   } else if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- %s_%d\n", ltmp, _ti(ltmp), varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ] );
   else if ( is_tainted(atmp) )
      VG_(printf)( "t%d_%d <*- t%d_%d\n", ltmp, _ti(ltmp), atmp, _ti(atmp) );
   else
      VG_(printf)("\n");
}

// ltmp = LOAD <ty> c
VG_REGPARM(3)
void TNT_(h32_load_c) (
   IRStmt *clone,
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   H_EXIT_EARLY
   H32_PC

   UInt ty      = clone->Ist.WrTmp.data->Iex.Load.ty - Ity_INVALID;
   IRExpr* addr = clone->Ist.WrTmp.data->Iex.Load.addr;
   UInt c       = extract_IRConst(addr->Iex.Const.con);

   UInt address = c;
   H_VAR

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = LOAD %s 0x%x",
                                                 KRED,
                                      ltmp, _ti(ltmp),
                                                 KNRM,
                                IRType_string[ty], c );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = LOAD %s 0x%x", ltmp, _ti(ltmp),
                                            IRType_string[ty], c );
      H32_PRINT
   }

   // Information flow
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   //lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- %s_%d\n", ltmp, _ti(ltmp), varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ] );
   else
      VG_(printf)("\n");
}

VG_REGPARM(3)
void TNT_(h32_get) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_get);
   H32_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ty      = data->Iex.Get.ty - Ity_INVALID;
   UInt reg     = data->Iex.Get.offset;

   tl_assert( reg < RI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)(aTmp, "%st%d_%d%s = r%d_%d %s",
                   KRED,
                   ltmp, _ti(ltmp),
                   KNRM,
                   reg, ri[reg], IRType_string[ty&0xff] );
      H32_PRINTC
   } else {
      VG_(sprintf)(aTmp, "t%d_%d = r%d_%d %s",
                   ltmp, _ti(ltmp),
                   reg, ri[reg], IRType_string[ty&0xff] );
      H32_PRINT
   }

   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- r%d_%d\n", ltmp, _ti(ltmp), reg, ri[reg] );
   else
      VG_(printf)("\n");
}

VG_REGPARM(3)
void TNT_(h32_geti) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING
}

// reg = tmp
VG_REGPARM(3)
void TNT_(h32_put_t) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {
   // Reg book-keeping
   UInt reg     = clone->Ist.Put.offset;
   tl_assert( reg < RI_MAX );
   ri[reg]++;

   if ( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_put_t);
   H32_PC

   IRExpr *data = clone->Ist.Put.data;
   UInt tmp     = data->Iex.RdTmp.tmp;

   tl_assert( tmp < TI_MAX );

   if ( istty && is_tainted(tmp) )
   {
      VG_(sprintf)(aTmp, "r%d_%d = %st%d_%d%s",
                   reg, ri[reg],
                   KRED,
                   tmp, _ti(tmp),
                   KNRM );
      H32_PRINTC
   } else {
      VG_(sprintf)(aTmp, "r%d_%d = t%d_%d", reg, ri[reg],
                                            tmp, _ti(tmp) );
      H32_PRINT
   }

   if ( is_tainted(tmp) )
      VG_(printf)("r%d_%d <- t%d_%d\n", reg, ri[reg], tmp, _ti(tmp));
   else
      VG_(printf)("\n");
}

// reg = const 
VG_REGPARM(3)
void TNT_(h32_put_c) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {
   // Reg bookkeeping
   UInt reg     = clone->Ist.Put.offset;
   tl_assert( reg < RI_MAX );
   ri[reg]++;

   if ( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   IRExpr *data = clone->Ist.Put.data;
   UInt c       = extract_IRConst(data->Iex.Const.con);

   VG_(sprintf)(aTmp, "r%d_%d = 0x%x", reg, ri[reg], c);
   H32_PRINT

   VG_(printf)("\n");
}

VG_REGPARM(3)
void TNT_(h32_puti) (
   UInt tt1, 
   UInt tt2, 
   UInt value, 
   UInt taint ) {

   if ( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   UInt elemTy = (tt1 >> 16) & 0xff;
   UInt ix = tt1 & 0xffff;
   UInt bias = (tt2 >> 16) & 0xffff;
   UInt tmp = tt2 & 0xffff;

   if ( istty && is_tainted(tmp) )
   {
      VG_(sprintf)(aTmp, "PUTI<%s>[%x,%x] = %st%d%s",
                IRType_string[elemTy], ix, bias,
                KRED, tmp, KNRM );
      H32_PRINTC
   } else {
      VG_(sprintf)(aTmp, "PUTI<%s>[%x,%x] = t%d", IRType_string[elemTy], ix, bias, tmp);
      H32_PRINT
   }

   // TODO: Info flow
   //tl_assert( reg < RI_MAX );
   //tl_assert( tmp < TI_MAX );
   //ri[reg]++;

   //VG_(printf)("r%d_%d <- t%d_%d\n", reg, ri[reg], tmp, ti[tmp]);
   VG_(printf)("\n");
}

// ltmp = <op> const 
VG_REGPARM(3)
void TNT_(h32_wrtmp_c) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING
   VG_(printf)("%x %x\n", value, taint);
}

// ltmp = <op> rtmp
VG_REGPARM(3)
void TNT_(h32_unop_t) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_unop_t);
   H32_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Unop.op - Iop_INVALID;
   IRExpr* arg = clone->Ist.WrTmp.data->Iex.Unop.arg;
   UInt rtmp = arg->Iex.RdTmp.tmp;

   tl_assert( rtmp < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op],
                    rtmp, _ti(rtmp) );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s t%d_%d",
                    ltmp, _ti(ltmp), IROp_string[op],
                    rtmp, _ti(rtmp) );
      H32_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp, _ti(rtmp) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> const
VG_REGPARM(3)
void TNT_(h32_unop_c) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Unop.op - Iop_INVALID;
   IRExpr* arg = clone->Ist.WrTmp.data->Iex.Unop.arg;
   UInt c = extract_IRConst( arg->Iex.Const.con );

   VG_(sprintf)( aTmp, "t%d_%d = %s 0x%x",
                 ltmp, _ti(ltmp), IROp_string[op], c );
   H32_PRINT

   // No information flow
   VG_(printf)("\n");
}

// ltmp = <op> rtmp1, const
VG_REGPARM(3)
void TNT_(h32_binop_tc) (
   IRStmt *clone,
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_binop_tc);
   H32_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   UInt rtmp1 = arg1->Iex.RdTmp.tmp;
   UInt c = extract_IRConst( arg2->Iex.Const.con );

   tl_assert( rtmp1 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s t%d_%d 0x%x",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op], rtmp1, _ti(rtmp1), c );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s t%d_%d 0x%x",
                    ltmp, _ti(ltmp),
                    IROp_string[op], rtmp1, _ti(rtmp1), c );
      H32_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> Const rtmp1
VG_REGPARM(3)
void TNT_(h32_binop_ct) (
   IRStmt *clone,
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   UInt c = extract_IRConst( arg1->Iex.Const.con );
   UInt rtmp2 = arg2->Iex.RdTmp.tmp;

   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s 0x%x t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op], c, rtmp2, _ti(rtmp2) );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s 0x%x t%d_%d",
                    ltmp, _ti(ltmp),
                    IROp_string[op], c, rtmp2, _ti(rtmp2) );
      H32_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> rtmp1 rtmp2
VG_REGPARM(3)
void TNT_(h32_binop_tt) (
   IRStmt *clone,
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   UInt rtmp1 = arg1->Iex.RdTmp.tmp;
   UInt rtmp2 = arg2->Iex.RdTmp.tmp;

   tl_assert( rtmp1 < TI_MAX );
   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s t%d_%d t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op],
                    rtmp1, _ti(rtmp1),
                    rtmp2, _ti(rtmp2) );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s t%d_%d t%d_%d",
                    ltmp, _ti(ltmp),
                    IROp_string[op],
                    rtmp1, _ti(rtmp1),
                    rtmp2, _ti(rtmp2) );
      H32_PRINT
   }

   // Information flow
   if ( is_tainted(rtmp1) && !is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1) );
   else if ( !is_tainted(rtmp1) && is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp2, _ti(rtmp2) );
   else if ( is_tainted(rtmp1) && is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d, t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> const1 const2
VG_REGPARM(3)
void TNT_(h32_binop_cc) (
   IRStmt *clone,
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   UInt c1 = extract_IRConst( arg1->Iex.Const.con );
   UInt c2 = extract_IRConst( arg2->Iex.Const.con );

   VG_(sprintf)( aTmp, "t%d_%d = %s 0x%x 0x%x",
                 ltmp, _ti(ltmp),
                 IROp_string[op], c1, c2 );
   H32_PRINT

   // No information flow
   VG_(printf)("\n");
}

// ltmp = <op> rtmp1, rtmp2, rtmp3
VG_REGPARM(3)
void TNT_(h32_triop) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING
}

// ltmp = <op> rtmp1, rtmp2, rtmp3, rtmp4
VG_REGPARM(3)
void TNT_(h32_qop) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING
}

// ltmp = rtmp
VG_REGPARM(3)
void TNT_(h32_rdtmp) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_rdtmp);
   H32_PC

   UInt rtmp = clone->Ist.WrTmp.data->Iex.RdTmp.tmp;

   tl_assert( rtmp < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    rtmp, _ti(rtmp) );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d", ltmp, _ti(ltmp),
                                          rtmp, _ti(rtmp) );
      H32_PRINT
   }

   if ( is_tainted(ltmp) )
      VG_(printf)("t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp),
                                        rtmp, _ti(rtmp));
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? rtmp1 : const
VG_REGPARM(3)
void TNT_(h32_ite_tc) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   UInt rtmp1   = data->Iex.ITE.iftrue->Iex.RdTmp.tmp;
   UInt c       = extract_IRConst(data->Iex.ITE.iffalse->Iex.Const.con);

   tl_assert( ctmp  < TI_MAX );
   tl_assert( rtmp1 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d ? t%d_%d : 0x%x",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    ctmp, _ti(ctmp), rtmp1, _ti(rtmp1), c );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? t%d_%d : 0x%x",
                    ltmp, _ti(ltmp), ctmp, _ti(ctmp), rtmp1, _ti(rtmp1), c );
      H32_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1) );
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? const : rtmp2
VG_REGPARM(3)
void TNT_(h32_ite_ct) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   UInt c       = extract_IRConst(data->Iex.ITE.iftrue->Iex.Const.con);
   UInt rtmp2   = data->Iex.ITE.iffalse->Iex.RdTmp.tmp;

   tl_assert( ctmp  < TI_MAX );
   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d ? 0x%x : t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    ctmp, _ti(ctmp), c, rtmp2, _ti(rtmp2) );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? 0x%x : t%d_%d",
                    ltmp, _ti(ltmp), ctmp, _ti(ctmp), c, rtmp2, _ti(rtmp2) );
      H32_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? rtmp1 : rtmp2
VG_REGPARM(3)
void TNT_(h32_ite_tt) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   UInt rtmp1   = data->Iex.ITE.iftrue->Iex.RdTmp.tmp;
   UInt rtmp2   = data->Iex.ITE.iffalse->Iex.RdTmp.tmp;

   tl_assert( ctmp  < TI_MAX );
   tl_assert( rtmp1 < TI_MAX );
   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d ? t%d_%d : t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    ctmp, _ti(ctmp),
                    rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
      H32_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? t%d_%d : t%d_%d",
                    ltmp, _ti(ltmp), ctmp, _ti(ctmp),
                    rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
      H32_PRINT
   }

   // Information flow
   if ( is_tainted(rtmp1) && is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d, t%d_%d\n", ltmp, _ti(ltmp),
                          rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
   else if ( is_tainted(rtmp1) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp),
                                         rtmp1, _ti(rtmp1) );
   else if ( is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp),
                                         rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? const1 : const2
VG_REGPARM(3)
void TNT_(h32_ite_cc) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   UInt c1      = extract_IRConst(data->Iex.ITE.iftrue->Iex.Const.con);
   UInt c2      = extract_IRConst(data->Iex.ITE.iffalse->Iex.Const.con);

   tl_assert( ctmp  < TI_MAX );
   if ( (ti[ctmp] & 0x80000000) == 0 ) return;

   VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? 0x%x : 0x%x",
                 ltmp, _ti(ltmp), ctmp, _ti(ctmp), c1, c2 );
   H32_PRINT

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp),
                                         ctmp, _ti(ctmp) );
   else
      VG_(printf)("\n");
}

// ltmp = callee( arg[0], ... )
VG_REGPARM(3)
void TNT_(h32_ccall) (
   IRStmt *clone, 
   UInt value, 
   UInt taint ) {

   H_WRTMP_BOOKKEEPING
}

// No decoding necessary. Just print the string
VG_REGPARM(3)
void TNT_(h32_none) ( 
   HChar *str, 
   UInt value, 
   UInt taint ) {

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   VG_(sprintf)( aTmp, "%s", str);
   H32_PRINT
   // No information flow info
   VG_(printf)("\n");
}

/**** 64-bit helpers ****/

// IF <gtmp> GOTO <jk> addr
VG_REGPARM(3)
void TNT_(h64_exit_t) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_EXIT_EARLY
   H64_PC

   IRExpr *guard = clone->Ist.Exit.guard;
   UInt gtmp     = guard->Iex.RdTmp.tmp;
   IRConst *dst  = clone->Ist.Exit.dst;
   ULong addr    = extract_IRConst64(dst);

   tl_assert( gtmp < TI_MAX );

   if ( istty && is_tainted(gtmp) )
   {
      VG_(sprintf)( aTmp, "IF %st%d_%d%s GOTO 0x%llx", KRED, gtmp, _ti(gtmp), KNRM, addr );
      H64_PRINTC 
   } else {
      VG_(sprintf)( aTmp, "IF t%d_%d GOTO 0x%llx", gtmp, _ti(gtmp), addr );
      H64_PRINT 
   }


   if ( is_tainted(gtmp) )
      VG_(printf)( "t%d_%d\n", gtmp, _ti(gtmp) );
   else
      VG_(printf)("\n");
}

// IF <gtmp> GOTO <jk> addr
VG_REGPARM(3)
void TNT_(h64_exit_c) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   // End of BB ??
}

// JMP tmp
VG_REGPARM(3)
void TNT_(h64_next_t) (
   IRExpr *clone, 
   ULong value, 
   ULong taint ) {

   H_EXIT_EARLY
   H64_PC

   UInt next = clone->Iex.RdTmp.tmp;

   tl_assert( next < TI_MAX );

   if ( istty && is_tainted(next) )
   {
      VG_(sprintf)( aTmp, "JMP %st%d_%d%s", KRED, next, ti[next], KNRM );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "JMP t%d_%d", next, ti[next] );
      H64_PRINT
   }


   if ( is_tainted(next) )
      VG_(printf)( "t%d_%d\n", next, ti[next] );
   else
      VG_(printf)("\n");
}

// JMP const 
VG_REGPARM(3)
void TNT_(h64_next_c) (
   IRExpr *clone, 
   ULong value, 
   ULong taint ) {

   H_EXIT_EARLY
   H64_PC
   VG_(sprintf)( aTmp, "JMP 0x%llx", value );
   H64_PRINT
   VG_(printf)("\n");
   // End of BB
}

// STORE atmp = dtmp
VG_REGPARM(3)
void TNT_(h64_store_tt) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   IRExpr *addr = clone->Ist.Store.addr;
   IRExpr *data = clone->Ist.Store.data;
   UInt atmp = addr->Iex.RdTmp.tmp;
   UInt dtmp = data->Iex.RdTmp.tmp;

   tl_assert( atmp < TI_MAX );
   tl_assert( dtmp < TI_MAX );

   H_EXIT_EARLY_LDST
   H_SMT2(smt2_store_tt);
   H64_PC

   ULong address = tv[atmp];
   H_VAR

   if ( istty && is_tainted(dtmp) )
   {
      VG_(sprintf)( aTmp, "STORE t%d_%d = %st%d_%d%s",
                                  atmp, _ti(atmp),
                                  KRED,
                                  dtmp, _ti(dtmp),
                                  KNRM );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "STORE t%d_%d = t%d_%d",
                                  atmp, _ti(atmp),
                                  dtmp, _ti(dtmp) );
      H64_PRINT
   }

   // Information flow
   // Check if it hasn't been seen before
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(dtmp) && is_tainted(atmp) ) {
      VG_(printf)( "%s_%d <- t%d_%d", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], dtmp, _ti(dtmp) );
      VG_(printf)( "; %s_%d <*- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], atmp, _ti(atmp) );
   } else if ( is_tainted(dtmp) )
      VG_(printf)( "%s_%d <- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], dtmp, _ti(dtmp) );
   else if ( is_tainted(atmp) )
      VG_(printf)( "%s_%d <*- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], atmp, _ti(atmp) );
   else
      VG_(printf)("\n");
}

// STORE atmp = c
VG_REGPARM(3)
void TNT_(h64_store_tc) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   IRExpr *addr = clone->Ist.Store.addr;
   IRExpr *data = clone->Ist.Store.data;
   UInt atmp    = addr->Iex.RdTmp.tmp;
   ULong c      = extract_IRConst64(data->Iex.Const.con);

   tl_assert( atmp < TI_MAX );

   H_EXIT_EARLY_LDST
   H64_PC

   ULong address = c;
   H_VAR

   VG_(sprintf)( aTmp, "STORE t%d_%d = 0x%llx", atmp, _ti(atmp), c );
   H64_PRINT

   // Information flow
   // Check if it hasn't been seen before
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(atmp) )
      VG_(printf)( "%s_%d <-*- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], atmp, _ti(atmp) );
   else
      VG_(printf)("\n");
}

// STORE c = dtmp
VG_REGPARM(3)
void TNT_(h64_store_ct) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_EXIT_EARLY
   H64_PC

   IRExpr *addr = clone->Ist.Store.addr;
   IRExpr *data = clone->Ist.Store.data;
   ULong c      = extract_IRConst64(addr->Iex.Const.con);
   UInt dtmp    = data->Iex.RdTmp.tmp;

   tl_assert( dtmp < TI_MAX );

   ULong address = c;
   H_VAR

   if ( istty && is_tainted(dtmp) )
   {
      VG_(sprintf)( aTmp, "STORE 0x%llx = %st%d_%d%s", c, KRED, dtmp, _ti(dtmp), KNRM );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "STORE 0x%llx = t%d_%d", c, dtmp, _ti(dtmp) );
      H64_PRINT
   }

   // Information flow
   // Check if it hasn't been seen before
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(dtmp) )
      VG_(printf)( "%s_%d <- t%d_%d\n", varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ], dtmp, _ti(dtmp) );
   else
      VG_(printf)("\n");
}

// ltmp = LOAD <ty> atmp
VG_REGPARM(3)
void TNT_(h64_load_t) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   UInt ty      = clone->Ist.WrTmp.data->Iex.Load.ty - Ity_INVALID;
   IRExpr* addr = clone->Ist.WrTmp.data->Iex.Load.addr;
   UInt atmp    = addr->Iex.RdTmp.tmp;

   tl_assert( atmp < TI_MAX );

   H_EXIT_EARLY_LDST
   H_SMT2(smt2_load_t);
   H64_PC

   ULong address = tv[atmp];
   H_VAR

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = LOAD %s t%d_%d", KRED, ltmp, _ti(ltmp), KNRM, IRType_string[ty], atmp, _ti(atmp) );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = LOAD %s t%d_%d", ltmp, _ti(ltmp), IRType_string[ty], atmp, _ti(atmp) );
      H64_PRINT
   }

   // Information flow
   // Check if it hasn't been seen before
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   //lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(ltmp) && is_tainted(atmp) ) {
      VG_(printf)( "t%d_%d <- %s_%d", ltmp, _ti(ltmp), varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ] );
      VG_(printf)( "; t%d_%d <*- t%d_%d\n", ltmp, _ti(ltmp), atmp, _ti(atmp) );
   } else if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- %s_%d\n", ltmp, _ti(ltmp), varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ] );
   else if ( is_tainted(atmp) )
      VG_(printf)( "t%d_%d <*- t%d_%d\n", ltmp, _ti(ltmp), atmp, _ti(atmp) );
   else
      VG_(printf)("\n");
}

// ltmp = LOAD <ty> const
VG_REGPARM(3)
void TNT_(h64_load_c) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   H_EXIT_EARLY
   H64_PC

   UInt ty      = clone->Ist.WrTmp.data->Iex.Load.ty - Ity_INVALID;
   IRExpr* addr = clone->Ist.WrTmp.data->Iex.Load.addr;
   ULong c      = extract_IRConst64(addr->Iex.Const.con);

   ULong address = c;
   H_VAR

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = LOAD %s 0x%llx", KRED, ltmp, _ti(ltmp), KNRM, IRType_string[ty], c );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = LOAD %s 0x%llx", ltmp, _ti(ltmp), IRType_string[ty], c );
      H64_PRINT
   }

   // Information flow
   if( myStringArray_getIndex( &lvar_s, varname ) == -1 ){
      myStringArray_push( &lvar_s, varname );
   }
   //lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ]++;

   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- %s_%d\n", ltmp, _ti(ltmp), varname, lvar_i[ myStringArray_getIndex( &lvar_s, varname ) ] );
   else
      VG_(printf)("\n");
}

VG_REGPARM(3)
void TNT_(h64_get) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC
   //H_VAR

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ty      = data->Iex.Get.ty - Ity_INVALID;
   UInt reg     = data->Iex.Get.offset;

   tl_assert( reg < RI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)(aTmp, "%st%d_%d%s = r%d_%d %s",
                           KRED,
                           ltmp, _ti(ltmp),
                           KNRM,
                           reg, ri[reg], IRType_string[ty&0xff] );
      H64_PRINTC
   } else {
      VG_(sprintf)(aTmp, "t%d_%d = r%d_%d %s",
                           ltmp, _ti(ltmp),
                           reg, ri[reg], IRType_string[ty&0xff] );
      H64_PRINT
   }

   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- r%d_%d\n", ltmp, _ti(ltmp), reg, ri[reg] );
   else
      VG_(printf)("\n");
}

VG_REGPARM(3)
void TNT_(h64_geti) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING
}

// reg = tmp
VG_REGPARM(3)
void TNT_(h64_put_t) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {
   // Reg book-keeping
   UInt reg     = clone->Ist.Put.offset;
   tl_assert( reg < RI_MAX );
   ri[reg]++;

   if ( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_put_t);
   H64_PC
   //H_VAR

   IRExpr *data = clone->Ist.Put.data;
   UInt tmp     = data->Iex.RdTmp.tmp;

   tl_assert( tmp < TI_MAX );

   if ( istty && is_tainted(tmp) )
   {
      VG_(sprintf)(aTmp, "r%d_%d = %st%d_%d%s", reg, ri[reg],
                                            KRED,
                                            tmp, _ti(tmp),
                                            KNRM );
      H64_PRINTC
   } else {
      VG_(sprintf)(aTmp, "r%d_%d = t%d_%d", reg, ri[reg],
                                            tmp, _ti(tmp) );
      H64_PRINT
   }

   if ( is_tainted(tmp) )
      VG_(printf)("r%d_%d <- t%d_%d\n", reg, ri[reg], tmp, _ti(tmp));
   else
      VG_(printf)("\n");
}

// reg = const
VG_REGPARM(3)
void TNT_(h64_put_c) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {
   // Reg bookkeeping
   UInt reg     = clone->Ist.Put.offset;
   tl_assert( reg < RI_MAX );
   ri[reg]++;

   if ( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC
   //H_VAR

   IRExpr *data = clone->Ist.Put.data;
   ULong c      = extract_IRConst(data->Iex.Const.con);

   VG_(sprintf)(aTmp, "r%d_%d = 0x%llx", reg, ri[reg], c);
   H64_PRINT

   VG_(printf)("\n");
}


VG_REGPARM(3)
void TNT_(h64_puti) (
   ULong tt1, 
   ULong tt2, 
   ULong value, 
   ULong taint ) {

   if ( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC
   //H_VAR

   UInt base = (tt1 >> 32) & 0xffffffff;
   UInt elemTy = (tt1 >> 16) & 0xff;
   UInt nElems = tt1 & 0xffff;
   UInt ix = (tt2 >> 32) & 0xffffffff;
   UInt bias = (tt2 >> 16) & 0xffff;
   UInt tmp = tt2 & 0xffff;

   if ( istty && is_tainted(tmp) )
   {
      VG_(sprintf)(aTmp, "PUTI<%d:%s:%d>[%x,%x] = %st%d%s", base, IRType_string[elemTy], nElems, ix, bias, KRED, tmp, KNRM);
      H64_PRINTC
   } else {
      VG_(sprintf)(aTmp, "PUTI<%d:%s:%d>[%x,%x] = t%d", base, IRType_string[elemTy], nElems, ix, bias, tmp);
      H64_PRINT
   }

   // TODO: Info flow
   //tl_assert( reg < RI_MAX );
   //tl_assert( tmp < TI_MAX );
   //ri[reg]++;

   //VG_(printf)("r%d_%d <- t%d_%d\n", reg, ri[reg], tmp, ti[tmp]);
   VG_(printf)("\n");
}

// ltmp = <op> ...
VG_REGPARM(3)
void TNT_(h64_wrtmp_c) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   VG_(printf)("%llx %llx\n", value, taint);
}

// ltmp = <op> rtmp
VG_REGPARM(3)
void TNT_(h64_unop_t) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_unop_t);
   H64_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Unop.op - Iop_INVALID;
   IRExpr* arg = clone->Ist.WrTmp.data->Iex.Unop.arg;
   UInt rtmp = arg->Iex.RdTmp.tmp;

   tl_assert( rtmp < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op],
                    rtmp, _ti(rtmp) );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s t%d_%d",
                    ltmp, _ti(ltmp), IROp_string[op],
                    rtmp, _ti(rtmp) );
      H64_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp, _ti(rtmp) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> const
VG_REGPARM(3)
void TNT_(h64_unop_c) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Unop.op - Iop_INVALID;
   IRExpr* arg = clone->Ist.WrTmp.data->Iex.Unop.arg;
   ULong c = extract_IRConst( arg->Iex.Const.con );

   VG_(sprintf)( aTmp, "t%d_%d = %s 0x%llx",
                 ltmp, _ti(ltmp), IROp_string[op], c );
   H64_PRINT

   // No information flow
   VG_(printf)("\n");
}

// ltmp = <op> rtmp1 const
VG_REGPARM(3)
void TNT_(h64_binop_tc) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   UInt rtmp1 = arg1->Iex.RdTmp.tmp;
   
   ULong c = extract_IRConst64(arg2->Iex.Const.con);
   
   tl_assert( rtmp1 < TI_MAX );
   
   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s t%d_%d 0x%llx",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op], rtmp1, _ti(rtmp1), c );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s t%d_%d 0x%llx",
                    ltmp, _ti(ltmp),
                    IROp_string[op], rtmp1, _ti(rtmp1), c );
      H64_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> const rtmp2
VG_REGPARM(3)
void TNT_(h64_binop_ct) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   ULong c = extract_IRConst64(arg1->Iex.Const.con);
   UInt rtmp2 = arg2->Iex.RdTmp.tmp;

   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s 0x%llx t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op], c, rtmp2, _ti(rtmp2) );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s 0x%llx t%d_%d",
                    ltmp, _ti(ltmp),
                    IROp_string[op], c, rtmp2, _ti(rtmp2) );
      H64_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> rtmp1 rtmp2
VG_REGPARM(3)
void TNT_(h64_binop_tt) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;
   
   H_EXIT_EARLY
   H64_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   UInt rtmp1 = arg1->Iex.RdTmp.tmp;
   UInt rtmp2 = arg2->Iex.RdTmp.tmp;
   
   tl_assert( rtmp1 < TI_MAX );
   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) ) 
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = %s t%d_%d t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    IROp_string[op],
                    rtmp1, _ti(rtmp1),
                    rtmp2, _ti(rtmp2) );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = %s t%d_%d t%d_%d",
                    ltmp, _ti(ltmp),
                    IROp_string[op],
                    rtmp1, _ti(rtmp1),
                    rtmp2, _ti(rtmp2) );
      H64_PRINT
   }

   // Information flow
   if ( is_tainted(rtmp1) && !is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1) );
   else if ( !is_tainted(rtmp1) && is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp2, _ti(rtmp2) );
   else if ( is_tainted(rtmp1) && is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d, t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = <op> const1 const2
VG_REGPARM(3)
void TNT_(h64_binop_cc) (
   IRStmt *clone,
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   UInt op = clone->Ist.WrTmp.data->Iex.Binop.op - Iop_INVALID;
   IRExpr* arg1 = clone->Ist.WrTmp.data->Iex.Binop.arg1;
   IRExpr* arg2 = clone->Ist.WrTmp.data->Iex.Binop.arg2;
   ULong c1 = extract_IRConst( arg1->Iex.Const.con );
   ULong c2 = extract_IRConst( arg2->Iex.Const.con );

   VG_(sprintf)( aTmp, "t%d_%d = %s 0x%llx 0x%llx",
                 ltmp, _ti(ltmp),
                 IROp_string[op], c1, c2 );
   H64_PRINT

   // No information flow
   VG_(printf)("\n");
}

// ltmp = <op> rtmp1, rtmp2, rtmp3
VG_REGPARM(3)
void TNT_(h64_triop) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING
}

// ltmp = <op> rtmp1, rtmp2, rtmp3, rtmp4
VG_REGPARM(3)
void TNT_(h64_qop) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING
}

VG_REGPARM(3)
void TNT_(h64_rdtmp) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H_SMT2(smt2_rdtmp);
   H64_PC
   //H_VAR

   UInt rtmp = clone->Ist.WrTmp.data->Iex.RdTmp.tmp;

   tl_assert( rtmp < TI_MAX );

   // Sanity check for the WrTmp book-keeping,
   // since RdTmp is essentially a no-op
   if ( value != tv[rtmp] )
      VG_(printf)("value 0x%llx != tv[rtmp] 0x%llx\n", value, tv[rtmp] );
   tl_assert( value == tv[rtmp] );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d",
                                KRED,
                                ltmp, _ti(ltmp),
                                KNRM,
                                rtmp, _ti(rtmp) );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d", ltmp, _ti(ltmp),
                                             rtmp, _ti(rtmp) );
      H64_PRINT
   }

   if ( is_tainted(ltmp) )
      VG_(printf)("t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp, _ti(rtmp));
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? rtmp1 : const
VG_REGPARM(3)
void TNT_(h64_ite_tc) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   UInt rtmp1   = data->Iex.ITE.iftrue->Iex.RdTmp.tmp;
   ULong c      = extract_IRConst64(data->Iex.ITE.iffalse->Iex.Const.con);

   tl_assert( ctmp  < TI_MAX );
   tl_assert( rtmp1 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d ? t%d_%d : 0x%llx",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    ctmp, _ti(ctmp), rtmp1, _ti(rtmp1), c );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? t%d_%d : 0x%llx",
                    ltmp, _ti(ltmp), ctmp, _ti(ctmp), rtmp1, _ti(rtmp1), c );
      H64_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1) );
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? const : rtmp2
VG_REGPARM(3)
void TNT_(h64_ite_ct) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H32_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   ULong c      = extract_IRConst64(data->Iex.ITE.iftrue->Iex.Const.con);
   UInt rtmp2   = data->Iex.ITE.iffalse->Iex.RdTmp.tmp;

   tl_assert( ctmp  < TI_MAX );
   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d ? 0x%llx : t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    ctmp, _ti(ctmp), c, rtmp2, _ti(rtmp2) );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? 0x%llx : t%d_%d",
                    ltmp, _ti(ltmp), ctmp, _ti(ctmp), c, rtmp2, _ti(rtmp2) );
      H64_PRINT
   }

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? rtmp1 : rtmp2
VG_REGPARM(3)
void TNT_(h64_ite_tt) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   UInt rtmp1   = data->Iex.ITE.iftrue->Iex.RdTmp.tmp;
   UInt rtmp2   = data->Iex.ITE.iffalse->Iex.RdTmp.tmp;

   tl_assert( ltmp  < TI_MAX );
   tl_assert( rtmp1 < TI_MAX );
   tl_assert( rtmp2 < TI_MAX );

   if ( istty && is_tainted(ltmp) )
   {
      VG_(sprintf)( aTmp, "%st%d_%d%s = t%d_%d ? t%d_%d : t%d_%d",
                    KRED,
                    ltmp, _ti(ltmp),
                    KNRM,
                    ctmp, _ti(ctmp),
                    rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
      H64_PRINTC
   } else {
      VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? t%d_%d : t%d_%d",
                    ltmp, _ti(ltmp), ctmp, _ti(ctmp),
                    rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
      H64_PRINT
   }

   // Information flow
   if ( is_tainted(rtmp1) && is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d, t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1), rtmp2, _ti(rtmp2) );
   else if ( is_tainted(rtmp1) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp1, _ti(rtmp1) );
   else if ( is_tainted(rtmp2) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp), rtmp2, _ti(rtmp2) );
   else
      VG_(printf)("\n");
}

// ltmp = ctmp? const1 : const2
VG_REGPARM(3)
void TNT_(h64_ite_cc) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   IRExpr *data = clone->Ist.WrTmp.data;
   UInt ctmp    = data->Iex.ITE.cond->Iex.RdTmp.tmp;
   ULong c1     = extract_IRConst64(data->Iex.ITE.iftrue->Iex.Const.con);
   ULong c2     = extract_IRConst64(data->Iex.ITE.iffalse->Iex.Const.con);

   tl_assert( ctmp  < TI_MAX );
   if ( (ti[ctmp] & 0x80000000) == 0 ) return;

   VG_(sprintf)( aTmp, "t%d_%d = t%d_%d ? 0x%llx : 0x%llx",
                 ltmp, _ti(ltmp), ctmp, _ti(ctmp), c1, c2 );
   H64_PRINT

   // Information flow
   if ( is_tainted(ltmp) )
      VG_(printf)( "t%d_%d <- t%d_%d\n", ltmp, _ti(ltmp),
                                         ctmp, _ti(ctmp) );
   else
      VG_(printf)("\n");
}

// ltmp = callee( arg[0], ... )
VG_REGPARM(3)
void TNT_(h64_ccall) (
   IRStmt *clone, 
   ULong value, 
   ULong taint ) {

   H_WRTMP_BOOKKEEPING
}

// No decoding necessary. Just print the string
VG_REGPARM(3)
void TNT_(h64_none) ( 
   HChar *str, 
   ULong value, 
   ULong taint ) {

   if( TNT_(clo_critical_ins_only) ) return;

   H_EXIT_EARLY
   H64_PC

   VG_(sprintf)( aTmp, "%s", str);
   H64_PRINT

   // No information flow info
   VG_(printf)("\n");
}
/*-- End of 64-bit helpers --*/

/*------------------------------------------------------------*/
/*--- utility function for finding local/global variable   ---*/
/*--- name from data address, using debug symbol tables.   ---*/
/*------------------------------------------------------------*/

void TNT_(describe_data)(Addr addr, HChar* varnamebuf, UInt bufsize, enum VariableType* type, enum VariableLocation* loc) {


	// first try to see if it is a global var
	PtrdiffT pdt;
	VG_(get_datasym_and_offset)( addr, varnamebuf, bufsize, &pdt );

	// Seems to get exe name?
	if ( VG_(get_objname)(addr, varnamebuf, bufsize) )
	{
	   //VG_(printf)("varname %s\n", varnamebuf);
	   return;
	}

        AddrInfo ai; ai.tag = Addr_Undescribed;
        VG_(describe_addr)(addr, &ai);
        //VG_(pp_addrinfo)(addr, &ai);
        //VG_(printf)("ai->tag %x\n", ai.tag);

        if ( ai.tag == Addr_DataSym )
        {
           VG_(strncpy)(varnamebuf, ai.Addr.DataSym.name, bufsize);
           return;
        } else if ( ai.tag == Addr_Variable )
        {
           //VG_(printf)("descr1 %s\n", VG_(indexXA)(ai.Addr.Variable.descr1,0) );
           //VG_(printf)("descr2 %s\n", VG_(indexXA)(ai.Addr.Variable.descr2,0) );
           //VG_(strncpy)(varnamebuf, VG_(indexXA)(ai.Addr.Variable.descr1,0), bufsize );
           return;
        }

	if( varnamebuf[0] == '\0' ){
		// now let's try for local var
		XArray* descr1
		      = VG_(newXA)( VG_(malloc), "tnt.da.descr1",
		                    VG_(free), sizeof(HChar) );
		XArray* descr2
		      = VG_(newXA)( VG_(malloc), "tnt.da.descr2",
		                    VG_(free), sizeof(HChar) );

		   (void) VG_(get_data_description)( descr1, descr2, addr );
		   /* If there's nothing in descr1/2, free them.  Why is it safe to to
		      VG_(indexXA) at zero here?  Because VG_(get_data_description)
		      guarantees to zero terminate descr1/2 regardless of the outcome
		      of the call.  So there's always at least one element in each XA
		      after the call.
		   */
		   if (0 == VG_(strlen)( VG_(indexXA)( descr1, 0 ))) {
		      VG_(deleteXA)( descr1 );
		      descr1 = NULL;
		   }

		   if (0 == VG_(strlen)( VG_(indexXA)( descr2, 0 ))) {
		      VG_(deleteXA)( descr2 );
		      descr2 = NULL;
		   }

		   /* Assume (assert) that VG_(get_data_description) fills in descr1
		      before it fills in descr2 */
		   if (descr1 == NULL)
		      tl_assert(descr2 == NULL);

		   /* If we could not obtain the variable name, then just use "unknownobj" */
		   if (descr1 == NULL) {
			   VG_(sprintf)( varnamebuf, "%lx_unknownobj", addr );
		   }
		   else {

//			   VG_(printf)("descr1: %s descr2: %s\n", (HChar*)VG_(indexXA)(descr1,0), (HChar*)VG_(indexXA)(descr2,0));

			   // descr1 will either be of the form:
			   // (1) Location 0xbef29644 is 0 bytes inside local var "n"
			   // or
			   // (2) Location 0xbed42644 is 0 bytes inside n[1],
			   // or
			   // (3) Location 0xbebb842c is 0 bytes inside args.str,
			   // or
			   // (4) Location 0xbebb842c is 0 bytes inside args[1].str,
			   // or
			   // (5) Location 0xbebb842c is 0 bytes inside args.str[0],
			   //
			   // So, the terminator for a variable name is either '"' or ','

			   HChar* descr1str =  (HChar*)VG_(indexXA)(descr1, 0);
			   const char* commonVarPrefix = "bytes inside ";
			   char* varPrefixPtr = VG_(strstr)(descr1str, commonVarPrefix);

			   tl_assert(varPrefixPtr != NULL);

			   // fast forward to start of var name
			   varPrefixPtr += (VG_(strlen)(commonVarPrefix)*sizeof(HChar));

			   // disambiguate between local var or others
			   const char* localVarPrefix = "local var ";
			   char* varStart = VG_(strstr)(varPrefixPtr, localVarPrefix);
			   HChar* varEnd;
			   int varNameLen = 0;

			   if (varStart == NULL) {
				   // case 2, 3, 4 or 5
				   varStart = varPrefixPtr;
				   varEnd = VG_(strchr)(varStart, ',');
//				   VG_(printf)("varStart: %s, varEnd: %s, descr1: %s, descr2: %s\n", varStart, varEnd, descr1str, (HChar*)VG_(indexXA)(descr2,0));
				   tl_assert(varEnd != NULL);
			   }
			   else {
				   // case 1: local variable
				   varStart += ((VG_(strlen)(localVarPrefix)+1)*sizeof(HChar)); // +1 to skip first "
				   varEnd = VG_(strchr)(varStart, '"');
			   }

			   tl_assert(varStart != NULL);
			   tl_assert(varEnd != NULL);

//			   VG_(printf)("varStart: %s, varEnd: %s, descr1: %s, descr2: %s\n", varStart, varEnd, descr1str, (HChar*)VG_(indexXA)(descr2,0));
//			   VG_(printf)("varStart: %s, varEnd: %s\n", varStart, varEnd);

			   varNameLen = VG_(strlen)(varStart) - VG_(strlen)(varEnd);
			   if (varNameLen >= bufsize) {
				   varNameLen = bufsize-1;
			   }
//						   VG_(printf)("first: %s, second: %s, varnamelen: %d\n", first, second, varnamelen);
			   VG_(strncpy)(varnamebuf, varStart, varNameLen);
			   varnamebuf[varNameLen] = '\0';

////			   VG_(printf)("Addr: %x, Var: %s\n", addr, varnamebuf);
		   }

		   if (descr1 != NULL) {
			   VG_(deleteXA)( descr1 );
		   }

		   if (descr2 != NULL) {
			   VG_(deleteXA)( descr2 );
		   }

		   *type = Local;
	}
	else {
		// it's a global variable
		*type = Global;

		if (have_created_sandbox || IN_SANDBOX) {
			tl_assert(client_binary_name != NULL);

			// let's determine it's location:
			// It is external from this application if the soname 
      // field in its DebugInfo is non-empty
      /*VG_(printf)("var: %s\n", varnamebuf);
      DebugInfo* di = NULL;
      while (di = VG_(next_DebugInfo)(di)) {
        VG_(printf)("  soname: %s, filename: %s, handle: %d\n", VG_(DebugInfo_get_soname)(di), VG_(DebugInfo_get_filename)(di), VG_(DebugInfo_get_handle)(di));
        XArray* gbs = VG_(di_get_global_blocks_from_dihandle)(VG_(DebugInfo_get_handle)(di), True);
        //tl_assert(gbs);
        int i, n = VG_(sizeXA)( gbs );
        VG_(printf)("  n: %d\n", n);
        for (i = 0; i < n; i++) {
          GlobalBlock* gbp;
          GlobalBlock* gb = VG_(indexXA)( gbs, i );
          if (0) VG_(printf)("   new Global size %2lu at %#lx:  %s %s\n",
                             gb->szB, gb->addr, gb->soname, gb->name );
        }
      }*/
      //DebugInfo* di = VG_(find_DebugInfo)(addr);
      //VG_(printf)("var: %s, di: %d\n", varnamebuf, di);
      
			UInt pc = VG_(get_IP)(VG_(get_running_tid)());
			HChar binarynamebuf[1024];
			VG_(get_objname)(pc, binarynamebuf, 1024);
      //VG_(printf)("var: %s, declaring binary: %s, client binary: %s\n", varnamebuf, binarynamebuf, client_binary_name);
			*loc = (VG_(strcmp)(binarynamebuf, client_binary_name) == 0 && VG_(strstr)(varnamebuf, "@@") == NULL) ? GlobalFromApplication : GlobalFromElsewhere;
      //*loc = GlobalFromElsewhere;
		}
	}
}



/*------------------------------------------------------------*/
/*--- Initialisation                                       ---*/
/*------------------------------------------------------------*/

static void init_shadow_memory ( void )
{
   Int     i;
   SecMap* sm;

   tl_assert(V_BIT_TAINTED   == 1);
   tl_assert(V_BIT_UNTAINTED     == 0);
   tl_assert(V_BITS8_TAINTED == 0xFF);
   tl_assert(V_BITS8_UNTAINTED   == 0);

   /* Build the 3 distinguished secondaries */
   sm = &sm_distinguished[SM_DIST_NOACCESS];
   for (i = 0; i < SM_CHUNKS; i++) sm->vabits8[i] = VA_BITS8_NOACCESS;

   sm = &sm_distinguished[SM_DIST_TAINTED];
   for (i = 0; i < SM_CHUNKS; i++) sm->vabits8[i] = VA_BITS8_TAINTED;

   sm = &sm_distinguished[SM_DIST_UNTAINTED];
   for (i = 0; i < SM_CHUNKS; i++) sm->vabits8[i] = VA_BITS8_UNTAINTED;

   /* Set up the primary map. */
   /* These entries gradually get overwritten as the used address
      space expands. */
   // Taintgrind: Initialise all memory as untainted
   for (i = 0; i < N_PRIMARY_MAP; i++)
      primary_map[i] = &sm_distinguished[SM_DIST_UNTAINTED];
//      primary_map[i] = &sm_distinguished[SM_DIST_NOACCESS];

   /* Auxiliary primary maps */
   init_auxmap_L1_L2();

   /* auxmap_size = auxmap_used = 0;
      no ... these are statically initialised */

   /* Secondary V bit table */
   secVBitTable = createSecVBitTable();

#if 0
   // Taintgrind: Solely for testing
   TNT_(make_mem_tainted)(0xbe000000, 0x1000000);
#endif
}

//static void read_allowed_syscalls() {
//	char* filename = TNT_(clo_allowed_syscalls);
//	int fd = VG_(fd_open)(filename, VKI_O_RDONLY, 0);
//	if (fd != -1) {
//		Bool finished = False;
//		char c;
//		int syscallno = 0;
//		int i=0;
//		while (VG_(read)(fd, &c, 1)) {
//			if (c != '\n') {
//				syscallno = 10*syscallno + ctoi(c);
//			}
//			else {
//				// end of line
//				VG_(printf)("allowed_syscall: %s (%d)\n", syscallnames[syscallno], syscallno);
//				allowed_syscalls[syscallno] = True;
//				syscallno = 0;
//			}
//		}
//		VG_(close)(fd);
//	}
//	else {
//		VG_(printf)("Error reading allowed syscalls file: %s\n", filename);
//	}
//}


/*------------------------------------------------------------*/
/*--- Syscall event handlers                               ---*/
/*------------------------------------------------------------*/

static
void tnt_pre_syscall(ThreadId tid, UInt syscallno,
                           UWord* args, UInt nArgs)
{
}

static
void tnt_post_syscall(ThreadId tid, UInt syscallno,
                            UWord* args, UInt nArgs, SysRes res)
{
	TNT_(syscall_allowed_check)(tid, syscallno);

	switch ((int)syscallno) {
#if defined VGO_freebsd
    case 3: //__NR_read:
      TNT_(syscall_read)(tid, args, nArgs, res);
      break;
    case 4: // __NR_write
      TNT_(syscall_write)(tid, args, nArgs, res);
      break;
    case 5: //__NR_open:
      TNT_(syscall_open)(tid, args, nArgs, res);
      break;
    case 6: //__NR_close:
      TNT_(syscall_close)(tid, args, nArgs, res);
      break;
    case 475: //__NR_pread64:
      TNT_(syscall_pread)(tid, args, nArgs, res);
      break;
    case 478: //__NR_lseek:
      TNT_(syscall_llseek)(tid, args, nArgs, res);
      break;
#else
    // Should be defined by respective vki/vki-arch-os.h
    case __NR_read:
      TNT_(syscall_read)(tid, args, nArgs, res);
      break;
    case __NR_write:
      TNT_(syscall_write)(tid, args, nArgs, res);
      break;
    case __NR_open:
    case __NR_openat:
      TNT_(syscall_open)(tid, args, nArgs, res);
      break;
    case __NR_close:
      TNT_(syscall_close)(tid, args, nArgs, res);
      break;
    case __NR_lseek:
#ifdef __NR_llseek
    case __NR_llseek:
#endif
      TNT_(syscall_llseek)(tid, args, nArgs, res);
      break;
    case __NR_pread64:
      TNT_(syscall_pread)(tid, args, nArgs, res);
      break;
#ifdef __NR_recv
    case __NR_recv:
      TNT_(syscall_recv)(tid, args, nArgs, res);
      break;
#endif
#ifdef __NR_recvfrom
    case __NR_recvfrom:
      TNT_(syscall_recvfrom)(tid, args, nArgs, res);
      break;
#endif
#endif // VGO_freebsd
  }
}

Bool TNT_(handle_client_requests) ( ThreadId tid, UWord* arg, UWord* ret ) {
	switch (arg[0]) {
		case VG_USERREQ__TAINTGRIND_ENTER_PERSISTENT_SANDBOX: {
			persistent_sandbox_nesting_depth++;
			break;
		}
		case VG_USERREQ__TAINTGRIND_EXIT_PERSISTENT_SANDBOX: {
			persistent_sandbox_nesting_depth--;
			break;
		}
		case VG_USERREQ__TAINTGRIND_ENTER_EPHEMERAL_SANDBOX: {
			ephemeral_sandbox_nesting_depth++;
			break;
		}
		case VG_USERREQ__TAINTGRIND_EXIT_EPHEMERAL_SANDBOX: {
			ephemeral_sandbox_nesting_depth--;
			break;
		}
		case VG_USERREQ__TAINTGRIND_CREATE_SANDBOX: {
			have_created_sandbox = 1;
			break;
		}
		case VG_USERREQ__TAINTGRIND_SHARED_FD: {
			Int fd = arg[1];
			Int perm = arg[2];
			if (fd >= 0) {
				FD_SET_PERMISSION(fd, perm);
			}
			break;
		}
		case VG_USERREQ__TAINTGRIND_SHARED_VAR: {
			HChar* var = (HChar*)arg[1];
			Int perm = arg[2];
			Int var_idx = myStringArray_push(&shared_vars, var);
			VAR_SET_PERMISSION(var_idx, perm);
			break;
		}
		case VG_USERREQ__TAINTGRIND_UPDATE_SHARED_VAR: {
			// record next shared var to be updated so that we can
			// check that the user has annotated a global variable write
			next_shared_variable_to_update = (HChar*)arg[1];
			break;
		}
		case VG_USERREQ__TAINTGRIND_ALLOW_SYSCALL: {
			int syscallno = arg[1];
			allowed_syscalls[syscallno] = True;
			break;
		}
		case VG_USERREQ__TAINTGRIND_ENTER_CALLGATE: {
			callgate_nesting_depth++;
			break;
		}
		case VG_USERREQ__TAINTGRIND_EXIT_CALLGATE: {
			callgate_nesting_depth--;
			break;
		}
		case VG_USERREQ__TAINTGRIND_MAKE_MEM_TAINTED: {
			TNT_(make_mem_tainted)(arg[1], arg[2]);
			break;
		}
		case VG_USERREQ__TAINTGRIND_MAKE_MEM_UNTAINTED: {
			TNT_(make_mem_untainted)(arg[1], arg[2]);
			break;
		}
		case VG_USERREQ__TAINTGRIND_START_PRINT: {
			TNT_(do_print) = 1;
			TNT_(clo_tainted_ins_only) = False;
			TNT_(clo_critical_ins_only) = False;

                        if ( TNT_(clo_smt2) )
                        {
                           TNT_(do_print) = 0;
			   TNT_(clo_tainted_ins_only) = True;
			   TNT_(clo_critical_ins_only) = False;
                        }
			break;
		}
		case VG_USERREQ__TAINTGRIND_STOP_PRINT: {
			TNT_(do_print) = 0;
			break;
		}
	}
	return True;
}

/*
   Taintgrind args
*/

// Defined in tnt_include.h
//#define MAX_PATH 256
//static Char   TNT_(default_file_filter)[]      = "";
HChar         TNT_(clo_file_filter)[MAX_PATH]  ;
Int           TNT_(clo_taint_start)            = 0;
Int           TNT_(clo_taint_len)              = 0x800000;
Bool          TNT_(clo_taint_all)              = False;
//Int           TNT_(clo_after_kbb)              = 0;
//Int           TNT_(clo_before_kbb)             = -1;
Bool          TNT_(clo_tainted_ins_only)       = True;
Bool          TNT_(clo_critical_ins_only)      = False;
Int           TNT_(do_print)                   = 0;
//Char*         TNT_(clo_allowed_syscalls)       = "";
//Bool          TNT_(read_syscalls_file)         = False;
Bool          TNT_(clo_smt2)                   = False;

void init_soaap_data(void);

static Bool tnt_process_cmd_line_options(const HChar* arg) {
   const HChar* tmp_str;

   if VG_STR_CLO(arg, "--file-filter", tmp_str) {
      VG_(strncpy)(TNT_(clo_file_filter), tmp_str, MAX_PATH);
      TNT_(do_print) = 0;
   }
   else if VG_BHEX_CLO(arg, "--taint-start", TNT_(clo_taint_start), 0x0000, 0x8000000) {}
   else if VG_BHEX_CLO(arg, "--taint-len", TNT_(clo_taint_len), 0x0000, 0x800000) {}
   else if VG_BOOL_CLO(arg, "--taint-all", TNT_(clo_taint_all)) {}
   //else if VG_BINT_CLO(arg, "--after-kbb", TNT_(clo_after_kbb), 0, 1000000) {}
   //else if VG_BINT_CLO(arg, "--before-kbb", TNT_(clo_before_kbb), 0, 1000000) {}
   else if VG_BOOL_CLO(arg, "--tainted-ins-only", TNT_(clo_tainted_ins_only)) {}
   else if VG_BOOL_CLO(arg, "--critical-ins-only", TNT_(clo_critical_ins_only)) {}
//   else if VG_STR_CLO(arg, "--allowed-syscalls", TNT_(clo_allowed_syscalls)) {
//	   TNT_(read_syscalls_file) = True;
//   }
   else if VG_BOOL_CLO(arg, "--smt2", TNT_(clo_smt2)) {}
   else
      return VG_(replacement_malloc_process_cmd_line_option)(arg);

   return True;
}

static void tnt_print_usage(void) {
   VG_(printf)(
"    --file-filter=<full_path>   full path of file to taint [\"\"]\n"
"    --taint-start=[0,800000]    starting byte to taint (in hex) [0]\n"
"    --taint-len=[0,800000]      number of bytes to taint from taint-start (in hex)[800000]\n"
"    --taint-all= no|yes         taint all bytes of all files read. warning: slow! [no]\n"
//"    --after-kbb=[0,1000000]     start instrumentation after # of BBs, in thousands [0]\n"
//"    --before-kbb=[0,1000000]    stop instrumentation after # of BBs, in thousands [-1]\n"
"    --tainted-ins-only= no|yes  print tainted instructions only [yes]\n"
"    --critical-ins-only= no|yes print critical instructions only [no]\n"
"    --smt2= no|yes              output SMT-LIBv2 format [no]\n"
   );
}

static void tnt_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}
                                   

/*
   Valgrind core functions
*/                                                     

static int tnt_isatty(void)
{
   HChar buf[256], dev2[11];
   const HChar dev[] = "/dev/pts/";
   int i;

   // 2: stderr
   VG_(readlink)("/proc/self/fd/2", buf, 255);
   //VG_(printf)("isatty: %s\n", buf);
   // If stderr goes to terminal, buf should be /dev/pts/[0-9]
   for ( i=0; i<10; i++ )
   {
      VG_(sprintf)(dev2, "%s%d", dev, i);
      if ( VG_(strncmp)(buf, dev2, 10) == 0 ) return 1;
   }
   return 0;
}

static void tnt_post_clo_init(void)
{
   if(*TNT_(clo_file_filter) == '\0'){

      if( !TNT_(clo_tainted_ins_only) || !TNT_(clo_critical_ins_only) )
         TNT_(do_print) = 1;

   }else if(*TNT_(clo_file_filter) != '/') { // Not absolute path
      if (*TNT_(clo_file_filter) == '~') {
         HChar* home    = VG_(getenv)("HOME");

         if (home) {
            HChar tmp[MAX_PATH+1];
            VG_(snprintf)( tmp, MAX_PATH, "%s%s", home, TNT_(clo_file_filter)+1 );
            VG_(snprintf)( TNT_(clo_file_filter), MAX_PATH, "%s", tmp );
            //VG_(printf)("%s\n", TNT_(clo_file_filter) );
         }else{
            VG_(printf)("*** Please use absolute path for --file-filter\n");
            VG_(exit)(1);
         }
      }else if (*TNT_(clo_file_filter) == '*') {
         // Wildcard
      }else{
         VG_(printf)("*** Please use absolute path for --file-filter\n");
         VG_(exit)(1);
      }
   }

   if( TNT_(clo_critical_ins_only) )
      TNT_(clo_tainted_ins_only) = True;

   // Initialise temporary variables/reg SSA index array
   Int i;
   for( i=0; i< TI_MAX; i++ ) {
      ti[i] = 0;
      tv[i] = 0;
      tt[i] = 0;
   }
   for( i=0; i< RI_MAX; i++ )
      ri[i] = 0;
   for( i=0; i< STACK_SIZE; i++ )
      lvar_i[i] = 0;
   lvar_s.size = 0;

//   if (TNT_(read_syscalls_file)) {
//	   read_allowed_syscalls();
//   }

   // DEBUG
   //tnt_read = 0;

   // If stdout is not a tty, don't highlight text
   istty = tnt_isatty();

   // Print SMT2 preamble if output is smt2
   if ( TNT_(clo_smt2) )
   {
      TNT_(smt2_preamble)();
      TNT_(clo_tainted_ins_only) = True;
      TNT_(clo_critical_ins_only) = False;
   }
}

static void tnt_fini(Int exitcode)
{
}

static void tnt_pre_clo_init(void)
{
   VG_(details_name)            ("Taintgrind");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("the taint analysis tool");
   VG_(details_copyright_author)(
      "Copyright (C) 2010-2014, and GNU GPL'd, by Wei Ming Khoo.");
   VG_(details_bug_reports_to)  ("weimzz@gmail.com");

   VG_(basic_tool_funcs)        (tnt_post_clo_init,
                                 TNT_(instrument),
                                 tnt_fini);

   /* Track syscalls for tainting purposes */
   // TODO: will this conflict?
   VG_(needs_syscall_wrapper)     ( tnt_pre_syscall,
                                    tnt_post_syscall );

   init_shadow_memory();

   init_soaap_data();

   VG_(needs_command_line_options)(tnt_process_cmd_line_options,
                                   tnt_print_usage,
                                   tnt_print_debug_usage);

   VG_(needs_malloc_replacement)  (TNT_(malloc),
                                   TNT_(__builtin_new),
                                   TNT_(__builtin_vec_new),
                                   TNT_(memalign),
                                   TNT_(calloc),
                                   TNT_(free),
                                   TNT_(__builtin_delete),
                                   TNT_(__builtin_vec_delete),
                                   TNT_(realloc),
                                   TNT_(malloc_usable_size),
                                   TNT_MALLOC_REDZONE_SZB );

   VG_(needs_client_requests)  (TNT_(handle_client_requests));

   // Taintgrind: Needed for tnt_malloc_wrappers.c
   TNT_(malloc_list)  = VG_(HT_construct)( "TNT_(malloc_list)" );
//   TNT_(mempool_list) = VG_(HT_construct)( "TNT_(mempool_list)" );

//   VG_(track_new_mem_mmap)        ( tnt_new_mem_mmap );
   VG_(track_copy_mem_remap)      ( TNT_(copy_address_range_state) );
   VG_(track_die_mem_stack_signal)( TNT_(make_mem_untainted) );
   VG_(track_die_mem_brk)         ( TNT_(make_mem_untainted) );
   VG_(track_die_mem_munmap)      ( TNT_(make_mem_untainted) );
}

VG_DETERMINE_INTERFACE_VERSION(tnt_pre_clo_init)

void TNT_(check_var_access)(ThreadId tid, HChar* varname, Int var_request, enum VariableType type, enum VariableLocation var_loc) {
	if (type == Global && var_loc == GlobalFromApplication) {
		HChar fnname[FNNAME_MAX];
		TNT_(get_fnname)(tid, fnname, FNNAME_MAX);
		Int var_idx = myStringArray_getIndex(&shared_vars, varname);
		// first check if this access is allowed
		Bool allowed = var_idx != -1 && (shared_vars_perms[var_idx] & var_request);
		if (IN_SANDBOX && !allowed) {
			const HChar* access_str;
			switch (var_request) {
				case VAR_READ: {
					access_str = "read";
					break;
				}
				case VAR_WRITE: {
					access_str = "wrote to";
					break;
				}
				default: {
					tl_assert(0);
					break;
				}
			}
			VG_(printf)("*** Sandbox %s global variable \"%s\" in method %s, but it is not allowed to. ***\n", access_str, varname, fnname);
			VG_(get_and_pp_StackTrace)(tid, STACK_TRACE_SIZE);
			VG_(printf)("\n");
		}
		// check for unnannotated writes to global vars both inside and outside
		// sandboxes
		if (var_request == VAR_WRITE) {
			if (next_shared_variable_to_update == NULL || VG_(strcmp)(next_shared_variable_to_update, varname) != 0) {
				if (IN_SANDBOX) {
					if (allowed) {
						VG_(printf)("*** Sandbox is allowed to write to global variable \"%s\" in method %s, but you have not explicitly declared this. ***\n", varname, fnname);
						VG_(get_and_pp_StackTrace)(tid, STACK_TRACE_SIZE);
						VG_(printf)("\n");
					}
				}
				else if (have_created_sandbox) {
					// only output this error if the sandbox is allowed at least read access
					Bool allowed_read = var_idx != -1 && (shared_vars_perms[var_idx] & VAR_READ);
					if (allowed_read) {
						VG_(printf)("*** Global variable \"%s\" is being written to in method %s after a sandbox has been created and so the sandbox will not see this new value. ***\n", varname, fnname);
						VG_(get_and_pp_StackTrace)(tid, STACK_TRACE_SIZE);
						VG_(printf)("\n");
					}
				}
			}
			else {
				next_shared_variable_to_update = NULL;
			}
		}

	}
}

void init_soaap_data() {
	persistent_sandbox_nesting_depth = 0;
	ephemeral_sandbox_nesting_depth = 0;
	have_created_sandbox = False;

	VG_(memset)(shared_vars_perms, 0, sizeof(Int)*VAR_MAX);
	VG_(memset)(shared_fds, 0, sizeof(Int)*FD_MAX);
	VG_(memset)(allowed_syscalls, 0, sizeof(Bool)*SYSCALLS_MAX);

	next_shared_variable_to_update = NULL;
	client_binary_name = NULL;
}

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/
