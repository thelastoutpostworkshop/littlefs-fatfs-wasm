#ifndef SPIFFS_CONFIG_H_
#define SPIFFS_CONFIG_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int32_t s32_t;
typedef uint32_t u32_t;
typedef int16_t s16_t;
typedef uint16_t u16_t;
typedef int8_t s8_t;
typedef uint8_t u8_t;

#ifndef SPIFFS_DBG
#define SPIFFS_DBG(...)
#endif

#ifndef SPIFFS_GC_DBG
#define SPIFFS_GC_DBG(...)
#endif

#ifndef SPIFFS_CACHE_DBG
#define SPIFFS_CACHE_DBG(...)
#endif

#ifndef SPIFFS_CHECK_DBG
#define SPIFFS_CHECK_DBG(...)
#endif

#ifndef SPIFFS_API_DBG
#define SPIFFS_API_DBG(...)
#endif

#ifndef SPIFFS_BUFFER_HELP
#define SPIFFS_BUFFER_HELP 1
#endif

#ifndef SPIFFS_CACHE
#define SPIFFS_CACHE 1
#endif

#ifndef SPIFFS_CACHE_WR
#define SPIFFS_CACHE_WR 1
#endif

#ifndef SPIFFS_CACHE_STATS
#define SPIFFS_CACHE_STATS 0
#endif

#ifndef SPIFFS_TEMPORAL_FD_CACHE
#define SPIFFS_TEMPORAL_FD_CACHE 1
#endif

#ifndef SPIFFS_TEMPORAL_CACHE_HIT_SCORE
#define SPIFFS_TEMPORAL_CACHE_HIT_SCORE 4
#endif

#ifndef SPIFFS_PAGE_CHECK
#define SPIFFS_PAGE_CHECK 1
#endif

#ifndef SPIFFS_GC_MAX_RUNS
#define SPIFFS_GC_MAX_RUNS 10
#endif

#ifndef SPIFFS_GC_HEUR_W_DELET
#define SPIFFS_GC_HEUR_W_DELET 5
#endif

#ifndef SPIFFS_GC_HEUR_W_USED
#define SPIFFS_GC_HEUR_W_USED (-1)
#endif

#ifndef SPIFFS_GC_HEUR_W_ERASE_AGE
#define SPIFFS_GC_HEUR_W_ERASE_AGE 50
#endif

#ifndef SPIFFS_OBJ_NAME_LEN
#define SPIFFS_OBJ_NAME_LEN 32
#endif

#ifndef SPIFFS_OBJ_META_LEN
#define SPIFFS_OBJ_META_LEN 4
#endif

#ifndef SPIFFS_COPY_BUFFER_STACK
#define SPIFFS_COPY_BUFFER_STACK 256
#endif

#ifndef SPIFFS_USE_MAGIC
#define SPIFFS_USE_MAGIC 1
#endif

#ifndef SPIFFS_USE_MAGIC_LENGTH
#define SPIFFS_USE_MAGIC_LENGTH 1
#endif

#ifndef SPIFFS_HAL_CALLBACK_EXTRA
#define SPIFFS_HAL_CALLBACK_EXTRA 0
#endif

#ifndef SPIFFS_SINGLETON
#define SPIFFS_SINGLETON 0
#endif

#ifndef SPIFFS_FILEHDL_OFFSET
#define SPIFFS_FILEHDL_OFFSET 0
#endif

#ifndef SPIFFS_READ_ONLY
#define SPIFFS_READ_ONLY 0
#endif

#ifndef SPIFFS_IX_MAP
#define SPIFFS_IX_MAP 1
#endif

#ifndef SPIFFS_NO_BLIND_WRITES
#define SPIFFS_NO_BLIND_WRITES 0
#endif

#ifndef SPIFFS_LOCK
#define SPIFFS_LOCK(fs)
#endif

#ifndef SPIFFS_UNLOCK
#define SPIFFS_UNLOCK(fs)
#endif

#ifndef SPIFFS_TEST_VISUALISATION
#define SPIFFS_TEST_VISUALISATION 0
#endif

#ifndef SPIFFS_TYPES_OVERRIDE
typedef u16_t spiffs_block_ix;
typedef u16_t spiffs_page_ix;
typedef u16_t spiffs_obj_id;
typedef u16_t spiffs_span_ix;
#endif

#endif /* SPIFFS_CONFIG_H_ */
