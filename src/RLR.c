//
//  RLR.c
//  libCacheSim
//
//  Reinforcement Learned Replacement policy
//  Based on "Cost Effective Cache Replacement Policy Using ML"
//
//  Per-line metadata:
//    - 5-bit saturating Age Counter (counts set misses since last access)
//    - 1-bit Hit Register (set on first demand hit after insertion)
//    - 1-bit Type Register (0=prefetch, 1=non-prefetch)
//
//  Per-set state:
//    - Accumulator: sum of preuse distances from demand hits
//    - demand_hit_count: number of demand hits since last RD update
//    - RD: predicted reuse distance = 2 * avg_preuse, updated every 32 hits
//

#include <limits.h>

#include "../../include/libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RLR_AGE_COUNTER_MAX 31    /* 5-bit saturating max */
#define RLR_RD_UPDATE_INTERVAL 32 /* recompute RD every 32 demand hits */
#define RLR_DEFAULT_RD 8          /* initial reuse distance estimate */

// ***********************************************************************
// ****                                                               ****
// ****                   params and per-set state                    ****
// ****                                                               ****
// ***********************************************************************

typedef struct {
  uint64_t accumulator;      /* sum of preuse distances since last RD update */
  uint32_t demand_hit_count; /* demand hits since last RD update */
  uint32_t RD;               /* current predicted reuse distance */
} RLR_set_state_t;

typedef struct {
  cache_obj_t *q_head;    /* MRU end */
  cache_obj_t *q_tail;    /* LRU end — eviction candidate end */
  RLR_set_state_t *sets;  /* per-set state array */
  uint32_t n_sets;        /* total number of sets */
  uint32_t associativity; /* ways per set */
} RLR_params_t;

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************

static void RLR_free(cache_t *cache);
static bool RLR_get(cache_t *cache, const request_t *req);
static cache_obj_t *RLR_find(cache_t *cache, const request_t *req,
                             bool update_cache);
static cache_obj_t *RLR_insert(cache_t *cache, const request_t *req);
static cache_obj_t *RLR_to_evict(cache_t *cache, const request_t *req);
static void RLR_evict(cache_t *cache, const request_t *req);
static bool RLR_remove(cache_t *cache, obj_id_t obj_id);

// ***********************************************************************
// ****                                                               ****
// ****                        helper functions                       ****
// ****                                                               ****
// ***********************************************************************

/* compute the 4-bit priority score for a cache line
 *   P_line = 8 * P_age + P_type + P_hit
 *   lowest score = eviction candidate */
static inline int rlr_priority(const cache_obj_t *obj, uint32_t RD) {
  int P_age = (obj->RLR.age_counter <= RD) ? 8 : 0;
  int P_type = obj->RLR.type_register; /* 1=non-prefetch, 0=prefetch */
  int P_hit = obj->RLR.hit_register;   /* 1=been hit, 0=never hit */
  return 8 * P_age + P_type + P_hit;
}

/* map an object to its set index using obj_id */
static inline uint32_t rlr_get_set(const cache_obj_t *obj, uint32_t n_sets) {
  return (uint32_t)(obj->obj_id % n_sets);
}

/* update the per-set accumulator and recompute RD every 32 demand hits */
static inline void rlr_update_rd(RLR_set_state_t *set,
                                 uint32_t preuse_distance) {
  set->accumulator += preuse_distance;
  set->demand_hit_count++;

  if (set->demand_hit_count >= RLR_RD_UPDATE_INTERVAL) {
    /* RD = 2 * avg_preuse  (right shift 5 = divide by 32, left shift 1 = x2)
     * equivalent to: accumulated >> (5 - 1) = accumulated >> 4 */
    set->RD = (uint32_t)(set->accumulator >> 4);
    if (set->RD == 0) set->RD = 1; /* guard against zero RD */
    set->accumulator = 0;
    set->demand_hit_count = 0;
  }
}

// ***********************************************************************
// ****                                                               ****
// ****                   init and free                               ****
// ****                                                               ****
// ***********************************************************************

cache_t *RLR_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("RLR", ccache_params, cache_specific_params);

  cache->cache_init = RLR_init;
  cache->cache_free = RLR_free;
  cache->get = RLR_get;
  cache->find = RLR_find;
  cache->insert = RLR_insert;
  cache->evict = RLR_evict;
  cache->remove = RLR_remove;
  cache->to_evict = RLR_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = sizeof(RLR_obj_metadata_t);
  } else {
    cache->obj_md_size = 0;
  }

  RLR_params_t *params = malloc(sizeof(RLR_params_t));
  memset(params, 0, sizeof(RLR_params_t));

  params->q_head = NULL;
  params->q_tail = NULL;
  /* use associativity from cache params, default to 16 if not set */
  params->associativity = 16;
  /* compute number of sets: total_ways / associativity */
  uint64_t total_ways = ccache_params.cache_size / 64; /* assume 64B lines */
  params->n_sets = (uint32_t)(total_ways / params->associativity);
  if (params->n_sets == 0) params->n_sets = 1;

  params->sets = malloc(sizeof(RLR_set_state_t) * params->n_sets);
  for (uint32_t i = 0; i < params->n_sets; i++) {
    params->sets[i].accumulator = 0;
    params->sets[i].demand_hit_count = 0;
    params->sets[i].RD = RLR_DEFAULT_RD;
  }

  cache->eviction_params = params;
  return cache;
}

static void RLR_free(cache_t *cache) {
  RLR_params_t *params = (RLR_params_t *)cache->eviction_params;
  free(params->sets);
  free(params);
  cache_struct_free(cache);
}

// ***********************************************************************
// ****                                                               ****
// ****                   core policy hooks                           ****
// ****                                                               ****
// ***********************************************************************

static bool RLR_get(cache_t *cache, const request_t *req) {
  return cache_get_base(cache, req);
}

static cache_obj_t *RLR_find(cache_t *cache, const request_t *req,
                             bool update_cache) {
  RLR_params_t *params = (RLR_params_t *)cache->eviction_params;
  cache_obj_t *obj = cache_find_base(cache, req, update_cache);

  if (obj != NULL && update_cache) {
    /* demand hit: update RD accumulator with this line's preuse distance
     * (age_counter value at hit time = preuse distance) */
    if (true) {
      uint32_t set_idx = rlr_get_set(obj, params->n_sets);
      rlr_update_rd(&params->sets[set_idx], obj->RLR.age_counter);

      /* set hit register */
      obj->RLR.hit_register = 1;
    }

    /* reset age counter on any hit (line is now most recently accessed) */
    obj->RLR.age_counter = 0;

    /* move to MRU end for recency tracking */
    move_obj_to_head(&params->q_head, &params->q_tail, obj);
  }

  return obj;
}

static cache_obj_t *RLR_insert(cache_t *cache, const request_t *req) {
  RLR_params_t *params = (RLR_params_t *)cache->eviction_params;

  cache_obj_t *obj = cache_insert_base(cache, req);

  /* initialise per-line metadata */
  obj->RLR.age_counter = 0;
  obj->RLR.hit_register = 0;
  obj->RLR.type_register = 1;

  /* insert at MRU head */
  prepend_obj_to_head(&params->q_head, &params->q_tail, obj);

  return obj;
}

static cache_obj_t *RLR_to_evict(cache_t *cache, const request_t *req) {
  RLR_params_t *params = (RLR_params_t *)cache->eviction_params;

  cache_obj_t *candidate = NULL;
  int lowest_priority = INT_MAX;

  cache_obj_t *cur = params->q_tail; /* start from LRU end */
  while (cur != NULL) {
    uint32_t set_idx = rlr_get_set(cur, params->n_sets);
    uint32_t RD = params->sets[set_idx].RD;
    int priority = rlr_priority(cur, RD);

    if (priority < lowest_priority) {
      lowest_priority = priority;
      candidate = cur;
    }
    cur = cur->queue.prev;
  }

  cache->to_evict_candidate_gen_vtime = cache->n_req;
  return candidate;
}

static void RLR_evict(cache_t *cache, const request_t *req) {
  RLR_params_t *params = (RLR_params_t *)cache->eviction_params;
  cache_obj_t *to_evict = RLR_to_evict(cache, req);

  DEBUG_ASSERT(to_evict != NULL);

  remove_obj_from_list(&params->q_head, &params->q_tail, to_evict);
  cache_evict_base(cache, to_evict, true);
}

static bool RLR_remove(cache_t *cache, obj_id_t obj_id) {
  RLR_params_t *params = (RLR_params_t *)cache->eviction_params;

  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) return false;

  remove_obj_from_list(&params->q_head, &params->q_tail, obj);
  cache_remove_obj_base(cache, obj, true);
  return true;
}

#ifdef __cplusplus
}
#endif
