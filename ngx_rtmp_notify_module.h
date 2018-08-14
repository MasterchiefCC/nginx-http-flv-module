#ifndef _NGX_RTMP_NOTIFY_H_INCLUDED_
#define _NGX_RTMP_NOTIFY_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_cmd_module.h"

#ifndef json_char
#define json_char char
#endif

#ifndef json_int_t
#ifndef _MSC_VER
#include <inttypes.h>
#define json_int_t int64_t
#else
#define json_int_t __int64
#endif
#endif

#include <stdlib.h>
enum {
  NGX_RTMP_NOTIFY_CONNECT,
  NGX_RTMP_NOTIFY_DISCONNECT,
  NGX_RTMP_NOTIFY_SRV_MAX
};

enum {
  NGX_RTMP_NOTIFY_PLAY,
  NGX_RTMP_NOTIFY_PUBLISH,
  NGX_RTMP_NOTIFY_PLAY_DONE,
  NGX_RTMP_NOTIFY_PUBLISH_DONE,
  NGX_RTMP_NOTIFY_DONE,
  NGX_RTMP_NOTIFY_RECORD_DONE,
  NGX_RTMP_NOTIFY_UPDATE,
  NGX_RTMP_NOTIFY_APP_MAX
};

typedef struct {
  ngx_url_t* url[NGX_RTMP_NOTIFY_APP_MAX];
  ngx_flag_t active;
  ngx_uint_t method, loop_times;
  ngx_msec_t update_timeout, reconnect_timegap, reconnect_timeout;
  ngx_flag_t update_strict;
  ngx_flag_t relay_redirect;
} ngx_rtmp_notify_app_conf_t;

typedef struct {
  ngx_url_t* url[NGX_RTMP_NOTIFY_SRV_MAX];
  ngx_uint_t method;
} ngx_rtmp_notify_srv_conf_t;

typedef struct {
  ngx_uint_t flags, cnt, reconnect_times;
  u_char name[256];
  u_char args[256];
  ngx_event_t update_evt;
  time_t start;
  ngx_rtmp_play_t* v_play;
  ngx_array_t* addrs_buffer;
  ngx_event_t reconnect_evt;
  ngx_str_t* stream_name;
} ngx_rtmp_notify_ctx_t;

typedef struct {
  unsigned long max_memory;
  int settings;

  /* Custom allocator support (leave null to use malloc/free)
   */

  void* (*mem_alloc)(size_t, int zero, void* user_data);
  void (*mem_free)(void*, void* user_data);

  void* user_data; /* will be passed to mem_alloc and mem_free */

  size_t value_extra; /* how much extra space to allocate for values? */

} json_settings;

#define json_enable_comments 0x01

typedef enum {
  json_none,
  json_object,
  json_array,
  json_integer,
  json_double,
  json_string,
  json_boolean,
  json_null

} json_type;

extern const struct _json_value json_value_none;

typedef struct _json_object_entry {
  json_char* name;
  unsigned int name_length;

  struct _json_value* value;

} json_object_entry;

typedef struct _json_value {
  struct _json_value* parent;

  json_type type;

  union {
    int boolean;
    json_int_t integer;
    double dbl;

    struct {
      unsigned int length;
      json_char* ptr; /* null terminated */

    } string;

    struct {
      unsigned int length;

      json_object_entry* values;
    } object;

    struct {
      unsigned int length;
      struct _json_value** values;
    } array;

  } u;

  union {
    struct _json_value* next_alloc;
    void* object_mem;

  } _reserved;
} json_value;

json_value* json_parse(const json_char* json, size_t length);

#define json_error_max 128

json_value* json_parse_ex(json_settings* settings, const json_char* json,
                          size_t length, char* error);

void json_value_free(json_value*);

/* Not usually necessary, unless you used a custom mem_alloc and now want to
 * use a custom mem_free.
 */

void json_value_free_ex(json_settings* settings, json_value*);
json_value* ngx_find_json_name(json_value* root, ngx_str_t* target);
json_value* ngx_find_json_name_in_object(json_value* root, ngx_str_t* target);
json_value* ngx_find_json_name_in_array(json_value* root, ngx_str_t* target);

void ngx_match_json_name(json_value* root, ngx_str_t* target, ngx_pool_t* pool,
                         ngx_array_t* arr);
void ngx_match_json_name_in_object(json_value* root, ngx_str_t* target,
                                   ngx_pool_t* pool, ngx_array_t* arr);
void ngx_match_json_name_in_array(json_value* root, ngx_str_t* target,
                                  ngx_pool_t* pool, ngx_array_t* arr);

extern ngx_module_t ngx_rtmp_notify_module;

#endif
