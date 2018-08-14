
/*
 * Copyright (C) Roman Arutyunyan
 */

#ifndef _NGX_RTMP_RECORD_H_INCLUDED_
#define _NGX_RTMP_RECORD_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"

#define NGX_RTMP_RECORD_OFF 0x01
#define NGX_RTMP_RECORD_AUDIO 0x02
#define NGX_RTMP_RECORD_VIDEO 0x04
#define NGX_RTMP_RECORD_KEYFRAMES 0x08
#define NGX_RTMP_RECORD_MANUAL 0x10
#define NGX_RTMP_RECORD_METADATA 0x20
#define NGX_RTMP_RECORD_XUE 0x40

typedef struct {
  ngx_str_t id;
  ngx_uint_t flags;
  ngx_str_t path;
  size_t max_size;
  size_t max_frames;
  ngx_msec_t interval;
  ngx_str_t suffix;
  ngx_flag_t unique;
  ngx_flag_t append;
  ngx_flag_t lock_file;
  ngx_flag_t notify;
  ngx_url_t *url;

  void **rec_conf;
  ngx_array_t rec; /* ngx_rtmp_record_app_conf_t * */
} ngx_rtmp_record_app_conf_t;

typedef struct {
  ngx_rtmp_record_app_conf_t *conf;
  ngx_file_t file;
  ngx_file_t metadata_file;
  ngx_uint_t nframes;
  uint32_t epoch, time_shift;
  ngx_time_t last;
  time_t timestamp;
  unsigned failed : 1;
  unsigned initialized : 1;
  unsigned aac_header_sent : 1;
  unsigned avc_header_sent : 1;
  unsigned video_key_sent : 1;
  unsigned audio : 1;
  unsigned video : 1;
  ////////////////////////////////////
  ngx_array_t *actionArray;
  ngx_uint_t isFirstLoadAction;
} ngx_rtmp_record_rec_ctx_t;

typedef struct {
  ngx_array_t rec; /* ngx_rtmp_record_rec_ctx_t */
  u_char name[256];
  u_char args[256];
} ngx_rtmp_record_ctx_t;

ngx_uint_t ngx_rtmp_record_find(ngx_rtmp_record_app_conf_t *racf,
                                ngx_str_t *id);

/* Manual recording control,
 * 'n' is record node index in config array.
 * Note: these functions allocate path in static buffer */

ngx_int_t ngx_rtmp_record_open(ngx_rtmp_session_t *s, ngx_uint_t n,
                               ngx_str_t *path);
ngx_int_t ngx_rtmp_record_close(ngx_rtmp_session_t *s, ngx_uint_t n,
                                ngx_str_t *path);

typedef struct {
  ngx_str_t recorder;
  ngx_str_t path;
} ngx_rtmp_record_done_t;

typedef ngx_int_t (*ngx_rtmp_record_done_pt)(ngx_rtmp_session_t *s,
                                             ngx_rtmp_record_done_t *v);

extern ngx_rtmp_record_done_pt ngx_rtmp_record_done;

extern ngx_module_t ngx_rtmp_record_module;

typedef struct {
  ngx_uint_t action_type;
  char actionid[64];
  char date[64];
  ngx_uint_t begintime;
  ngx_uint_t endtime;
  ngx_uint_t timer;
  char type[64];
  char url[512];
} ngx_rtmp_xueersi_action_t;

void ngx_rtmp_record_xueersi_start(ngx_rtmp_session_t *s);
void ngx_rtmp_record_xueersi_stop(ngx_rtmp_session_t *s);
ngx_int_t ngx_rtmp_record_metadata(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h);
ngx_int_t ngx_rtmp_record_metadata_start(ngx_rtmp_session_t *s,
                                         ngx_rtmp_header_t *h, ngx_chain_t *in);

////////////////////////////////////CJSON////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

/* project version */
#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 7

#include <stddef.h>

/* cJSON Types: */
#define cJSON_Invalid (0)
#define cJSON_False (1 << 0)
#define cJSON_True (1 << 1)
#define cJSON_NULL (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw (1 << 7) /* raw json */

#define cJSON_IsReference 256
#define cJSON_StringIsConst 512

/* The cJSON structure: */
typedef struct cJSON {
  /* next/prev allow you to walk array/object chains. Alternatively, use
   * GetArraySize/GetArrayItem/GetObjectItem */
  struct cJSON *next;
  struct cJSON *prev;
  /* An array or object item will have a child pointer pointing to a chain of
   * the items in the array/object. */
  struct cJSON *child;

  /* The type of the item, as above. */
  int type;

  /* The item's string, if type==cJSON_String  and type == cJSON_Raw */
  char *valuestring;
  /* writing to valueint is DEPRECATED, use cJSON_SetNumberValue instead */
  int valueint;
  /* The item's number, if type==cJSON_Number */
  double valuedouble;

  /* The item's name string, if this item is the child of, or is in the list of
   * subitems of an object. */
  char *string;
} cJSON;

typedef struct cJSON_Hooks {
  void *(*malloc_fn)(size_t sz);
  void (*free_fn)(void *ptr);
} cJSON_Hooks;

typedef int cJSON_bool;

#if !defined(__WINDOWS__) && \
    (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif
#ifdef __WINDOWS__

#if !defined(CJSON_HIDE_SYMBOLS) && !defined(CJSON_IMPORT_SYMBOLS) && \
    !defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_EXPORT_SYMBOLS
#endif

#if defined(CJSON_HIDE_SYMBOLS)
#define CJSON_PUBLIC(type) type __stdcall
#elif defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_PUBLIC(type) __declspec(dllexport) type __stdcall
#elif defined(CJSON_IMPORT_SYMBOLS)
#define CJSON_PUBLIC(type) __declspec(dllimport) type __stdcall
#endif
#else /* !WIN32 */
#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined(__SUNPRO_C)) && \
    defined(CJSON_API_VISIBILITY)
#define CJSON_PUBLIC(type) __attribute__((visibility("default"))) type
#else
#define CJSON_PUBLIC(type) type
#endif
#endif

/* Limits how deeply nested arrays/objects can be before cJSON rejects to parse
 * them. This is to prevent stack overflows. */
#ifndef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 1000
#endif

/* returns the version of cJSON as a string */
CJSON_PUBLIC(const char *) cJSON_Version(void);

/* Supply malloc, realloc and free functions to cJSON */
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks *hooks);

/* Memory Management: the caller is always responsible to free the results from
 * all variants of cJSON_Parse (with cJSON_Delete) and cJSON_Print (with stdlib
 * free, cJSON_Hooks.free_fn, or cJSON_free as appropriate). The exception is
 * cJSON_PrintPreallocated, where the caller has full responsibility of the
 * buffer. */
/* Supply a block of JSON, and this returns a cJSON object you can interrogate.
 */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value);
/* ParseWithOpts allows you to require (and check) that the JSON is null
 * terminated, and to retrieve the pointer to the final byte parsed. */
/* If you supply a ptr in return_parse_end and parsing fails, then
 * return_parse_end will contain a pointer to the error so will match
 * cJSON_GetErrorPtr(). */
CJSON_PUBLIC(cJSON *)
cJSON_ParseWithOpts(const char *value, const char **return_parse_end,
                    cJSON_bool require_null_terminated);

/* Render a cJSON entity to text for transfer/storage. */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item);
/* Render a cJSON entity to text for transfer/storage without any formatting. */
CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item);
/* Render a cJSON entity to text using a buffered strategy. prebuffer is a guess
 * at the final size. guessing well reduces reallocation. fmt=0 gives
 * unformatted, =1 gives formatted */
CJSON_PUBLIC(char *)
cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt);
/* Render a cJSON entity to text using a buffer already allocated in memory with
 * given length. Returns 1 on success and 0 on failure. */
/* NOTE: cJSON is not always 100% accurate in estimating how much memory it will
 * use, so to be safe allocate 5 bytes more than you actually need */
CJSON_PUBLIC(cJSON_bool)
cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length,
                        const cJSON_bool format);
/* Delete a cJSON entity and all subentities. */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *c);

/* Returns the number of items in an array (or object). */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array);
/* Retrieve item number "index" from array "array". Returns NULL if
 * unsuccessful. */
CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index);
/* Get item "string" from object. Case insensitive. */
CJSON_PUBLIC(cJSON *)
cJSON_GetObjectItem(const cJSON *const object, const char *const string);
CJSON_PUBLIC(cJSON *)
cJSON_GetObjectItemCaseSensitive(const cJSON *const object,
                                 const char *const string);
CJSON_PUBLIC(cJSON_bool)
cJSON_HasObjectItem(const cJSON *object, const char *string);
/* For analysing failed parses. This returns a pointer to the parse error.
 * You'll probably need to look a few chars back to make sense of it. Defined
 * when cJSON_Parse() returns 0. 0 when cJSON_Parse() succeeds. */
CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void);

/* Check if the item is a string and return its valuestring */
CJSON_PUBLIC(char *) cJSON_GetStringValue(cJSON *item);

/* These functions check the type of an item */
CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON *const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON *const item);

/* These calls create a cJSON item of the appropriate type. */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void);
CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void);
CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void);
CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean);
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num);
CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string);
/* raw json */
CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw);
CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void);
CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void);

/* Create a string where valuestring references a string so
 * it will not be freed by cJSON_Delete */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string);
/* Create an object/arrray that only references it's elements so
 * they will not be freed by cJSON_Delete */
CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child);
CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child);

/* These utilities create an Array of count items. */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count);
CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count);
CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count);
CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char **strings, int count);

/* Append item to the specified array/object. */
CJSON_PUBLIC(void) cJSON_AddItemToArray(cJSON *array, cJSON *item);
CJSON_PUBLIC(void)
cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
/* Use this when string is definitely const (i.e. a literal, or as good as), and
 * will definitely survive the cJSON object. WARNING: When this function was
 * used, make sure to always check that (item->type & cJSON_StringIsConst) is
 * zero before writing to `item->string` */
CJSON_PUBLIC(void)
cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);
/* Append reference to item to the specified array/object. Use this when you
 * want to add an existing cJSON to a new cJSON, but don't want to corrupt your
 * existing cJSON. */
CJSON_PUBLIC(void) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
CJSON_PUBLIC(void)
cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

/* Remove/Detatch items from Arrays/Objects. */
CJSON_PUBLIC(cJSON *)
cJSON_DetachItemViaPointer(cJSON *parent, cJSON *const item);
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which);
CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which);
CJSON_PUBLIC(cJSON *)
cJSON_DetachItemFromObject(cJSON *object, const char *string);
CJSON_PUBLIC(cJSON *)
cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string);
CJSON_PUBLIC(void)
cJSON_DeleteItemFromObject(cJSON *object, const char *string);
CJSON_PUBLIC(void)
cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string);

/* Update array items. */
CJSON_PUBLIC(void)
cJSON_InsertItemInArray(
    cJSON *array, int which,
    cJSON *newitem); /* Shifts pre-existing items to the right. */
CJSON_PUBLIC(cJSON_bool)
cJSON_ReplaceItemViaPointer(cJSON *const parent, cJSON *const item,
                            cJSON *replacement);
CJSON_PUBLIC(void)
cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
CJSON_PUBLIC(void)
cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);
CJSON_PUBLIC(void)
cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string,
                                       cJSON *newitem);

/* Duplicate a cJSON item */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse);
/* Duplicate will create a new, identical cJSON item to the one you pass, in new
memory that will need to be released. With recurse!=0, it will duplicate any
children connected to the item. The item->next and ->prev pointers are always
zero on return from Duplicate. */
/* Recursively compare two cJSON items for equality. If either a or b is NULL or
 * invalid, they will be considered unequal.
 * case_sensitive determines if object keys are treated case sensitive (1) or
 * case insensitive (0) */
CJSON_PUBLIC(cJSON_bool)
cJSON_Compare(const cJSON *const a, const cJSON *const b,
              const cJSON_bool case_sensitive);

CJSON_PUBLIC(void) cJSON_Minify(char *json);

/* Helper functions for creating and adding items to an object at the same time.
 * They return the added item or NULL on failure. */
CJSON_PUBLIC(cJSON *)
cJSON_AddNullToObject(cJSON *const object, const char *const name);
CJSON_PUBLIC(cJSON *)
cJSON_AddTrueToObject(cJSON *const object, const char *const name);
CJSON_PUBLIC(cJSON *)
cJSON_AddFalseToObject(cJSON *const object, const char *const name);
CJSON_PUBLIC(cJSON *)
cJSON_AddBoolToObject(cJSON *const object, const char *const name,
                      const cJSON_bool boolean);
CJSON_PUBLIC(cJSON *)
cJSON_AddNumberToObject(cJSON *const object, const char *const name,
                        const double number);
CJSON_PUBLIC(cJSON *)
cJSON_AddStringToObject(cJSON *const object, const char *const name,
                        const char *const string);
CJSON_PUBLIC(cJSON *)
cJSON_AddRawToObject(cJSON *const object, const char *const name,
                     const char *const raw);
CJSON_PUBLIC(cJSON *)
cJSON_AddObjectToObject(cJSON *const object, const char *const name);
CJSON_PUBLIC(cJSON *)
cJSON_AddArrayToObject(cJSON *const object, const char *const name);

/* When assigning an integer value, it needs to be propagated to valuedouble
 * too. */
#define cJSON_SetIntValue(object, number) \
  ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
/* helper for the cJSON_SetNumberValue macro */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number);
#define cJSON_SetNumberValue(object, number) \
  ((object != NULL) ? cJSON_SetNumberHelper(object, (double)number) : (number))

/* Macro for iterating over an array or object */
#define cJSON_ArrayForEach(element, array)                                 \
  for (element = (array != NULL) ? (array)->child : NULL; element != NULL; \
       element = element->next)

/* malloc/free objects using the malloc/free functions that have been set with
 * cJSON_InitHooks */
CJSON_PUBLIC(void *) cJSON_malloc(size_t size);
CJSON_PUBLIC(void) cJSON_free(void *object);

#ifdef __cplusplus
}
#endif

#endif /* _NGX_RTMP_RECORD_H_INCLUDED_ */
