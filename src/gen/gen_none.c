/* gen_none.c - an empty block table, used by build.cmd when src/gen/gen_all.c has not been
 * generated (python tools/ughrec.py, from your own copy of the game).  The game then runs in the
 * interpreter, which is slower but gives the same result. */
#include "gen.h"
#include <stddef.h>

const GenBlock g_gen_blocks[1] = {{0u, 0u, NULL}};
const unsigned g_gen_block_count = 0u;
const GenRange g_gen_ranges[1] = {{0u, 0u, 0u}};
const unsigned g_gen_range_count = 0u;
const uint16_t g_gen_entry_cs = REL(0x0000u), g_gen_entry_ip = 0x0000u;
