
/*
 * Copyright (C) Roman Arutyunyan
 */

#include "ngx_rtmp_record_module.h"
#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_codec_module.h"
#include "ngx_rtmp_netcall_module.h"

#if !defined(_CRT_SECURE_NO_DEPRECATE) && defined(_MSC_VER)
#define _CRT_SECURE_NO_DEPRECATE
#endif

#ifdef __GNUC__
#pragma GCC visibility push(default)
#endif
#if defined(_MSC_VER)
#pragma warning(push)
/* disable warning about single line comments in system headers */
#pragma warning(disable : 4001)
#endif

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_LOCALES
#include <locale.h>
#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#ifdef __GNUC__
#pragma GCC visibility pop
#endif


ngx_rtmp_record_done_pt ngx_rtmp_record_done;

static ngx_rtmp_publish_pt next_publish;
static ngx_rtmp_close_stream_pt next_close_stream;
static ngx_rtmp_stream_begin_pt next_stream_begin;
static ngx_rtmp_stream_eof_pt next_stream_eof;

static char *ngx_rtmp_record_recorder(ngx_conf_t *cf, ngx_command_t *cmd,
                                      void *conf);
static ngx_int_t ngx_rtmp_record_postconfiguration(ngx_conf_t *cf);
static void *ngx_rtmp_record_create_app_conf(ngx_conf_t *cf);
static char *ngx_rtmp_record_merge_app_conf(ngx_conf_t *cf, void *parent,
                                            void *child);
static ngx_int_t ngx_rtmp_record_write_frame(ngx_rtmp_session_t *s,
                                             ngx_rtmp_record_rec_ctx_t *rctx,
                                             ngx_rtmp_header_t *h,
                                             ngx_chain_t *in,
                                             ngx_int_t inc_nframes);
static ngx_int_t ngx_rtmp_record_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                                    ngx_chain_t *in);
static ngx_int_t ngx_rtmp_record_node_av(ngx_rtmp_session_t *s,
                                         ngx_rtmp_record_rec_ctx_t *rctx,
                                         ngx_rtmp_header_t *h, ngx_chain_t *in);
static ngx_int_t ngx_rtmp_record_node_open(ngx_rtmp_session_t *s,
                                           ngx_rtmp_record_rec_ctx_t *rctx);
static ngx_int_t ngx_rtmp_record_node_close(ngx_rtmp_session_t *s,
                                            ngx_rtmp_record_rec_ctx_t *rctx);
static void ngx_rtmp_record_make_path(ngx_rtmp_session_t *s,
                                      ngx_rtmp_record_rec_ctx_t *rctx,
                                      ngx_str_t *path);
static ngx_int_t ngx_rtmp_record_init(ngx_rtmp_session_t *s);

//////////////////////////////////////////////////////////////////

static ngx_int_t ngx_rtmp_record_metadata_open(ngx_rtmp_session_t *s,
                                               ngx_rtmp_record_rec_ctx_t *rctx);
static ngx_int_t ngx_rtmp_record_metadata_close(
    ngx_rtmp_session_t *s, ngx_rtmp_record_rec_ctx_t *rctx);
static void ngx_rtmp_record_make_metadata_path(ngx_rtmp_session_t *s,
                                               ngx_rtmp_record_rec_ctx_t *rctx,
                                               ngx_str_t *path);

static ngx_int_t ngx_rtmp_record_write_metadata(ngx_rtmp_session_t *s,
                                                ngx_rtmp_record_rec_ctx_t *rctx,
                                                ngx_rtmp_header_t *h);

static ngx_int_t ngx_rtmp_record_node_metadata(ngx_rtmp_session_t *s,
                                               ngx_rtmp_record_rec_ctx_t *rctx,
                                               ngx_rtmp_header_t *h,
                                               ngx_chain_t *in);

static ngx_conf_bitmask_t ngx_rtmp_record_mask[] = {
    {ngx_string("off"), NGX_RTMP_RECORD_OFF},
    {ngx_string("all"), NGX_RTMP_RECORD_AUDIO | NGX_RTMP_RECORD_VIDEO},
    {ngx_string("audio"), NGX_RTMP_RECORD_AUDIO},
    {ngx_string("video"), NGX_RTMP_RECORD_VIDEO},
    {ngx_string("keyframes"), NGX_RTMP_RECORD_KEYFRAMES},
    {ngx_string("manual"), NGX_RTMP_RECORD_MANUAL},
    {ngx_string("metadata"), NGX_RTMP_RECORD_METADATA},
    {ngx_string("record_xes"), NGX_RTMP_RECORD_XUE},
    {ngx_null_string, 0}};

static ngx_command_t ngx_rtmp_record_commands[] = {

    {ngx_string("record"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_1MORE,
     ngx_conf_set_bitmask_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, flags), ngx_rtmp_record_mask},

    {ngx_string("record_path"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, path), NULL},

    {ngx_string("record_suffix"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, suffix), NULL},

    {ngx_string("record_unique"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_flag_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, unique), NULL},

    {ngx_string("record_append"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_flag_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, append), NULL},

    {ngx_string("record_lock"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_flag_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, lock_file), NULL},

    {ngx_string("record_max_size"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_size_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, max_size), NULL},

    {ngx_string("record_max_frames"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_size_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, max_frames), NULL},

    {ngx_string("record_interval"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_msec_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, interval), NULL},

    {ngx_string("record_notify"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_flag_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_record_app_conf_t, notify), NULL},

    {ngx_string("recorder"),
     NGX_RTMP_APP_CONF | NGX_CONF_BLOCK | NGX_CONF_TAKE1,
     ngx_rtmp_record_recorder, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    ngx_null_command};

static ngx_rtmp_module_t ngx_rtmp_record_module_ctx = {
    NULL,                              /* preconfiguration */
    ngx_rtmp_record_postconfiguration, /* postconfiguration */
    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */
    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */
    ngx_rtmp_record_create_app_conf,   /* create app configuration */
    ngx_rtmp_record_merge_app_conf     /* merge app configuration */
};

ngx_module_t ngx_rtmp_record_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_record_module_ctx, /* module context */
    ngx_rtmp_record_commands,    /* module directives */
    NGX_RTMP_MODULE,             /* module type */
    NULL,                        /* init master */
    NULL,                        /* init module */
    NULL,                        /* init process */
    NULL,                        /* init thread */
    NULL,                        /* exit thread */
    NULL,                        /* exit process */
    NULL,                        /* exit master */
    NGX_MODULE_V1_PADDING};

static void *ngx_rtmp_record_create_app_conf(ngx_conf_t *cf) {
  ngx_rtmp_record_app_conf_t *racf;

  racf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_record_app_conf_t));

  if (racf == NULL) {
    return NULL;
  }

  racf->max_size = NGX_CONF_UNSET_SIZE;
  racf->max_frames = NGX_CONF_UNSET_SIZE;
  racf->interval = NGX_CONF_UNSET_MSEC;
  racf->unique = NGX_CONF_UNSET;
  racf->append = NGX_CONF_UNSET;
  racf->lock_file = NGX_CONF_UNSET;
  racf->notify = NGX_CONF_UNSET;
  racf->url = NGX_CONF_UNSET_PTR;

  if (ngx_array_init(&racf->rec, cf->pool, 1, sizeof(void *)) != NGX_OK) {
    return NULL;
  }

  return racf;
}

static char *ngx_rtmp_record_merge_app_conf(ngx_conf_t *cf, void *parent,
                                            void *child) {
  ngx_rtmp_record_app_conf_t *prev = parent;
  ngx_rtmp_record_app_conf_t *conf = child;
  ngx_rtmp_record_app_conf_t **rracf;

  ngx_conf_merge_str_value(conf->path, prev->path, "");
  ngx_conf_merge_str_value(conf->suffix, prev->suffix, ".flv");
  ngx_conf_merge_size_value(conf->max_size, prev->max_size, 0);
  ngx_conf_merge_size_value(conf->max_frames, prev->max_frames, 0);
  ngx_conf_merge_value(conf->unique, prev->unique, 0);
  ngx_conf_merge_value(conf->append, prev->append, 0);
  ngx_conf_merge_value(conf->lock_file, prev->lock_file, 0);
  ngx_conf_merge_value(conf->notify, prev->notify, 0);
  ngx_conf_merge_msec_value(conf->interval, prev->interval,
                            (ngx_msec_t)NGX_CONF_UNSET);
  ngx_conf_merge_bitmask_value(conf->flags, prev->flags, 0);
  ngx_conf_merge_ptr_value(conf->url, prev->url, NULL);

  if (conf->flags) {
    rracf = ngx_array_push(&conf->rec);
    if (rracf == NULL) {
      return NGX_CONF_ERROR;
    }

    *rracf = conf;
  }

  return NGX_CONF_OK;
}

static ngx_int_t ngx_rtmp_record_write_header(ngx_file_t *file) {
  static u_char flv_header[] = {
      0x46,                   /* 'F' */
      0x4c,                   /* 'L' */
      0x56,                   /* 'V' */
      0x01,                   /* version = 1 */
      0x05,                   /* 00000 1 0 1 = has audio & video */
      0x00, 0x00, 0x00, 0x09, /* header size */
      0x00, 0x00, 0x00, 0x00  /* PreviousTagSize0 (not actually a header) */
  };

  return ngx_write_file(file, flv_header, sizeof(flv_header), 0) == NGX_ERROR
             ? NGX_ERROR
             : NGX_OK;
}

static ngx_rtmp_record_rec_ctx_t *ngx_rtmp_record_get_node_ctx(
    ngx_rtmp_session_t *s, ngx_uint_t n) {
  ngx_rtmp_record_ctx_t *ctx;
  ngx_rtmp_record_rec_ctx_t *rctx;

  if (ngx_rtmp_record_init(s) != NGX_OK) {
    return NULL;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);

  if (n >= ctx->rec.nelts) {
    return NULL;
  }

  rctx = ctx->rec.elts;

  return &rctx[n];
}

ngx_int_t ngx_rtmp_record_open(ngx_rtmp_session_t *s, ngx_uint_t n,
                               ngx_str_t *path) {
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_int_t rc;

  ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: #%ui manual open", n);

  rctx = ngx_rtmp_record_get_node_ctx(s, n);

  if (rctx == NULL) {
    return NGX_ERROR;
  }

  rc = ngx_rtmp_record_node_open(s, rctx);
  if (rc != NGX_OK) {
    return rc;
  }

  if (path) {
    ngx_rtmp_record_make_path(s, rctx, path);
  }

  return NGX_OK;
}

ngx_int_t ngx_rtmp_record_close(ngx_rtmp_session_t *s, ngx_uint_t n,
                                ngx_str_t *path) {
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_int_t rc;

  ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: #%ui manual close", n);

  rctx = ngx_rtmp_record_get_node_ctx(s, n);

  if (rctx == NULL) {
    return NGX_ERROR;
  }

  rc = ngx_rtmp_record_node_close(s, rctx);
  if (rc != NGX_OK) {
    return rc;
  }

  if (path) {
    ngx_rtmp_record_make_path(s, rctx, path);
  }

  return NGX_OK;
}

ngx_uint_t ngx_rtmp_record_find(ngx_rtmp_record_app_conf_t *racf,
                                ngx_str_t *id) {
  ngx_rtmp_record_app_conf_t **pracf, *rracf;
  ngx_uint_t n;

  pracf = racf->rec.elts;

  for (n = 0; n < racf->rec.nelts; ++n, ++pracf) {
    rracf = *pracf;

    if (rracf->id.len == id->len &&
        ngx_strncmp(rracf->id.data, id->data, id->len) == 0) {
      return n;
    }
  }

  return NGX_CONF_UNSET_UINT;
}

void ngx_create_my_path(u_char *dir, ngx_uint_t access, ngx_uint_t start) {
  u_char *p, ch;

  p = dir + start;

  for (/* void */; *p; p++) {
    ch = *p;

    if (ch != '/') {
      continue;
    }

    *p = '\0';

    ngx_create_dir(dir, access);

    *p = '/';
  }

  return;
}

//把path从设置文件设置出来
//生成文件名
/* This funcion returns pointer to a static buffer */
static void ngx_rtmp_record_make_path(ngx_rtmp_session_t *s,
                                      ngx_rtmp_record_rec_ctx_t *rctx,
                                      ngx_str_t *path) {
  ngx_rtmp_record_ctx_t *ctx;
  ngx_rtmp_record_app_conf_t *rracf;
  ngx_rtmp_codec_ctx_t *codec_ctx;

  u_char *p, *l;
  struct tm tm;

  static u_char buf[NGX_TIME_T_LEN + 1];
  static u_char pbuf[NGX_MAX_PATH + 1];

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);
  codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

  rracf = rctx->conf;

  /* create file path */
  p = pbuf;
  l = pbuf + sizeof(pbuf) - 1;

  // path就是设置文件设置的path
  p = ngx_cpymem(p, rracf->path.data,
                 ngx_min(rracf->path.len, (size_t)(l - p - 1)));

  if (codec_ctx != NULL && *codec_ctx->record_path != '\0') {
    p = ngx_cpymem(p, codec_ctx->record_path,
                   ngx_min(strlen((const char *)codec_ctx->record_path),
                           (size_t)(l - p - 1)));
  }
  *p++ = '/';

  if (codec_ctx != NULL && *codec_ctx->record_filename != '\0' &&
      ((rctx->conf->flags & NGX_RTMP_RECORD_XUE) &&
       (rctx->conf->flags & NGX_RTMP_RECORD_MANUAL))) {
    p = ngx_cpymem(p, codec_ctx->record_filename,
                   ngx_min(strlen((const char *)codec_ctx->record_filename),
                           (size_t)(l - p - 1)));
  } else {
    p = (u_char *)ngx_escape_uri(
        p, ctx->name, ngx_min(ngx_strlen(ctx->name), (size_t)(l - p)),
        NGX_ESCAPE_URI_COMPONENT);
  }

  /* append timestamp */
  //设置了unique
  if (rracf->unique) {
    p = ngx_cpymem(
        p, buf, ngx_min(ngx_sprintf(buf, "-%T", rctx->timestamp) - buf, l - p));
  }

  if (ngx_strchr(rracf->suffix.data, '%')) {
    ngx_libc_localtime(rctx->timestamp, &tm);
    p += strftime((char *)p, l - p, (char *)rracf->suffix.data, &tm);
  } else {
    if ((rctx->conf->flags & NGX_RTMP_RECORD_XUE) &&
        (rctx->conf->flags & NGX_RTMP_RECORD_MANUAL)) {
      p = ngx_cpymem(p, "#", ngx_min(strlen("#"), (size_t)(l - p)));
    } else {
      p = ngx_cpymem(p, rracf->suffix.data,
                     ngx_min(rracf->suffix.len, (size_t)(l - p)));
    }
  }

  *p = 0;
  path->data = pbuf;
  path->len = p - pbuf;

  ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: %V path: '%V'", &rracf->id, path);
}

static void ngx_rtmp_record_make_metadata_path(ngx_rtmp_session_t *s,
                                               ngx_rtmp_record_rec_ctx_t *rctx,
                                               ngx_str_t *path) {
  ngx_rtmp_record_ctx_t *ctx;
  ngx_rtmp_record_app_conf_t *rracf;
  u_char *p, *l;
  struct tm tm;
  static u_char buf[NGX_TIME_T_LEN + 1];
  static u_char pbuf[NGX_MAX_PATH + 1];
  ngx_rtmp_codec_ctx_t *codec_ctx;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);

  rracf = rctx->conf;

  /* create file path */
  p = pbuf;
  l = pbuf + sizeof(pbuf) - 1;

  codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

  p = ngx_cpymem(p, rracf->path.data,
                 ngx_min(rracf->path.len, (size_t)(l - p - 1)));

  if (codec_ctx != NULL && *codec_ctx->record_path != '\0') {
    ngx_uint_t pos = p - pbuf + 1;
    p = ngx_cpymem(p, codec_ctx->record_path,
                   ngx_min(strlen((const char *)codec_ctx->record_path),
                           (size_t)(l - p - 1)));
    *p = 0;
    ngx_create_my_path(pbuf, 0777, pos);
  }

  *p++ = '/';

  if (codec_ctx != NULL && *codec_ctx->record_filename != '\0' &&
      ((rctx->conf->flags & NGX_RTMP_RECORD_XUE) &&
       (rctx->conf->flags & NGX_RTMP_RECORD_MANUAL))) {
    p = ngx_cpymem(p, codec_ctx->record_filename,
                   ngx_min(strlen((const char *)codec_ctx->record_filename),
                           (size_t)(l - p - 1)));
  } else {
    p = (u_char *)ngx_escape_uri(
        p, ctx->name, ngx_min(ngx_strlen(ctx->name), (size_t)(l - p)),
        NGX_ESCAPE_URI_COMPONENT);
  }

  /* append timestamp */
  if (rracf->unique) {
    p = ngx_cpymem(
        p, buf, ngx_min(ngx_sprintf(buf, "-%T", rctx->timestamp) - buf, l - p));
  }

  if (ngx_strchr(rracf->suffix.data, '%')) {
    ngx_libc_localtime(rctx->timestamp, &tm);
    p += strftime((char *)p, l - p, (char *)rracf->suffix.data, &tm);
  } else {
    p = ngx_cpymem(p, ".metadata",
                   ngx_min(strlen(".metadata"), (size_t)(l - p)));
  }

  *p = 0;
  path->data = pbuf;
  path->len = p - pbuf;

  ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                "record metadata: %V path: '%V'", &rracf->id, path);
}

static void ngx_rtmp_record_notify_error(ngx_rtmp_session_t *s,
                                         ngx_rtmp_record_rec_ctx_t *rctx) {
  ngx_rtmp_record_app_conf_t *rracf = rctx->conf;

  rctx->failed = 1;

  if (!rracf->notify) {
    return;
  }

  ngx_rtmp_send_status(s, "NetStream.Record.Failed", "error",
                       rracf->id.data ? (char *)rracf->id.data : "");
}

//设置rctx基础配置
static ngx_int_t ngx_rtmp_record_node_open(ngx_rtmp_session_t *s,
                                           ngx_rtmp_record_rec_ctx_t *rctx) {
  ngx_rtmp_record_app_conf_t *rracf;
  ngx_err_t err;
  ngx_str_t path;
  ngx_int_t mode, create_mode;
  u_char buf[8], *p;
  off_t file_size;
  uint32_t tag_size, mlen, timestamp;
  ngx_file_t metadatafile;
  ngx_array_t *actionArray;

  if (rctx->conf->flags & (NGX_RTMP_RECORD_XUE)) {
    ngx_rtmp_record_metadata_open(s, rctx);
  }

  rracf = rctx->conf;
  metadatafile = rctx->metadata_file;
  actionArray = rctx->actionArray;
  tag_size = 0;

  if (rctx->file.fd != NGX_INVALID_FILE) {
    return NGX_AGAIN;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: %V opening", &rracf->id);

  ngx_memzero(rctx, sizeof(*rctx));
  rctx->conf = rracf;
  rctx->last = *ngx_cached_time;
  rctx->timestamp = ngx_cached_time->sec;
  rctx->metadata_file = metadatafile;
  rctx->actionArray = actionArray;

  ngx_rtmp_record_make_path(s, rctx, &path);

  mode = rracf->append ? NGX_FILE_RDWR : NGX_FILE_WRONLY;
  create_mode = rracf->append ? NGX_FILE_CREATE_OR_OPEN : NGX_FILE_TRUNCATE;

  ngx_memzero(&rctx->file, sizeof(rctx->file));
  rctx->file.offset = 0;
  rctx->file.log = s->connection->log;
  //在这里open
  rctx->file.fd =
      ngx_open_file(path.data, mode, create_mode, NGX_FILE_DEFAULT_ACCESS);
  ngx_str_set(&rctx->file.name, "recorded");

  if (rctx->file.fd == NGX_INVALID_FILE) {
    err = ngx_errno;

    if (err != NGX_ENOENT) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, err,
                    "record: %V failed to open file '%V'", &rracf->id, &path);
    }

    ngx_rtmp_record_notify_error(s, rctx);

    return NGX_OK;
  }

#if !(NGX_WIN32)
  // record_lock
  if (rracf->lock_file) {
    err = ngx_lock_fd(rctx->file.fd);
    if (err) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, err,
                    "record: %V lock failed", &rracf->id);
    }
  }
#endif

  ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: %V opened '%V'", &rracf->id, &path);

  if (rracf->notify) {
    ngx_rtmp_send_status(s, "NetStream.Record.Start", "status",
                         rracf->id.data ? (char *)rracf->id.data : "");
  }

  if (rracf->append) {
    file_size = 0;
    timestamp = 0;

#if (NGX_WIN32)
    {
      LONG lo, hi;

      lo = 0;
      hi = 0;
      lo = SetFilePointer(rctx->file.fd, lo, &hi, FILE_END);
      file_size =
          (lo == INVALID_SET_FILE_POINTER ? (off_t)-1
                                          : (off_t)hi << 32 | (off_t)lo);
    }
#else
    file_size = lseek(rctx->file.fd, 0, SEEK_END);
#endif
    if (file_size == (off_t)-1) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, ngx_errno,
                    "record: %V seek failed", &rracf->id);
      goto done;
    }

    //会在这里goto done
    if (file_size < 4) {
      goto done;
    }

    if (ngx_read_file(&rctx->file, buf, 4, file_size - 4) != 4) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, ngx_errno,
                    "record: %V tag size read failed", &rracf->id);
      goto done;
    }

    p = (u_char *)&tag_size;
    p[0] = buf[3];
    p[1] = buf[2];
    p[2] = buf[1];
    p[3] = buf[0];

    if (tag_size == 0 || tag_size + 4 > file_size) {
      file_size = 0;
      goto done;
    }

    if (ngx_read_file(&rctx->file, buf, 8, file_size - tag_size - 4) != 8) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, ngx_errno,
                    "record: %V tag read failed", &rracf->id);
      goto done;
    }

    p = (u_char *)&mlen;
    p[0] = buf[3];
    p[1] = buf[2];
    p[2] = buf[1];
    p[3] = 0;

    if (tag_size != mlen + 11) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, ngx_errno,
                    "record: %V tag size mismatch: "
                    "tag_size=%uD, mlen=%uD",
                    &rracf->id, tag_size, mlen);
      goto done;
    }

    p = (u_char *)&timestamp;
    p[3] = buf[7];
    p[0] = buf[6];
    p[1] = buf[5];
    p[2] = buf[4];

  done:
    rctx->file.offset = file_size;
    rctx->time_shift = timestamp;

    ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "record: append offset=%O, time=%uD, tag_size=%uD",
                   file_size, timestamp, tag_size);
  }

  return NGX_OK;
}

static ngx_int_t ngx_rtmp_record_metadata_open(
    ngx_rtmp_session_t *s, ngx_rtmp_record_rec_ctx_t *rctx) {
  ngx_rtmp_record_app_conf_t *rracf;
  ngx_err_t err;
  ngx_str_t path;
  ngx_int_t mode, create_mode;
  off_t file_size;

  rracf = rctx->conf;

  if (rctx->metadata_file.fd != NGX_INVALID_FILE) {
    return NGX_AGAIN;
  }

  ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                "record metadata: %V opening", &rracf->id);

  ngx_rtmp_record_make_metadata_path(s, rctx, &path);

  mode = rracf->append ? NGX_FILE_RDWR : NGX_FILE_WRONLY;
  create_mode = rracf->append ? NGX_FILE_CREATE_OR_OPEN : NGX_FILE_TRUNCATE;

  ngx_memzero(&rctx->metadata_file, sizeof(rctx->metadata_file));
  rctx->metadata_file.offset = 0;
  rctx->metadata_file.log = s->connection->log;
  rctx->metadata_file.fd =
      ngx_open_file(path.data, mode, create_mode, NGX_FILE_DEFAULT_ACCESS);
  ngx_str_set(&rctx->metadata_file.name, "recorded_metadata");

  if (rctx->metadata_file.fd == NGX_INVALID_FILE) {
    err = ngx_errno;

    if (err != NGX_ENOENT) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, err,
                    "record metadata: %V failed to open file '%V'", &rracf->id,
                    &path);
    }

    return NGX_OK;
  }

  if (rctx->actionArray == NULL) {
    rctx->actionArray = ngx_array_create(s->connection->pool, 32,
                                         sizeof(ngx_rtmp_xueersi_action_t));
  }

  ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                "record metadata: %V opened '%V'", &rracf->id, &path);

  if (rracf->append) {
    file_size = 0;

#if (NGX_WIN32)
    {
      LONG lo, hi;

      lo = 0;
      hi = 0;
      lo = SetFilePointer(rctx->metadata_file.fd, lo, &hi, FILE_END);
      file_size =
          (lo == INVALID_SET_FILE_POINTER ? (off_t)-1
                                          : (off_t)hi << 32 | (off_t)lo);
    }
#else
    file_size = lseek(rctx->metadata_file.fd, 0, SEEK_END);
#endif
    if (file_size == (off_t)-1) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, ngx_errno,
                    "record metadata: %V seek failed", &rracf->id);
      file_size = 0;
    }

    ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                  "record metadata: append offset=%O, time=%uD", file_size,
                  rctx->time_shift);

    if (file_size) {
      u_char *filebuffer;
      cJSON *root, *item, *element;
      if (rctx->isFirstLoadAction == 1) {
        return NGX_OK;
      }
      rctx->isFirstLoadAction = 1;

      filebuffer = (u_char *)ngx_pcalloc(s->connection->pool, file_size + 1);
      ngx_read_file(&rctx->metadata_file, filebuffer, file_size, 0);
      root = cJSON_Parse((const char *)filebuffer);

      if (root && root->type == cJSON_Array) {
        ngx_rtmp_xueersi_action_t *action;
        ngx_int_t elenum = 0;
        while ((element = cJSON_DetachItemFromArray(root, 0))) {
          elenum++;
          action = ngx_array_push(rctx->actionArray);

          ngx_memzero(action, sizeof(ngx_rtmp_xueersi_action_t));

          item = cJSON_DetachItemFromObject(element, "category");
          if (item) {
            action->action_type = item->valueint;
            cJSON_Delete(item);
          }

          item = cJSON_DetachItemFromObject(element, "id");
          if (item) {
            strcpy(action->actionid, item->valuestring);
            cJSON_Delete(item);
          }

          item = cJSON_DetachItemFromObject(element, "type");
          if (item && item->valuestring) {
            strcpy(action->type, item->valuestring);
            cJSON_Delete(item);
          }

          item = cJSON_DetachItemFromObject(element, "url");
          if (item) {
            strcpy(action->url, item->valuestring);
            cJSON_Delete(item);
          }

          item = cJSON_DetachItemFromObject(element, "begintime");
          if (item) {
            action->begintime = item->valueint;
            cJSON_Delete(item);
          }

          item = cJSON_DetachItemFromObject(element, "endtime");
          if (item) {
            action->endtime = item->valueint;
            cJSON_Delete(item);
          }

          item = cJSON_DetachItemFromObject(element, "date");
          if (item) {
            strcpy(action->date, item->valuestring);
            cJSON_Delete(item);
          }

          item = cJSON_DetachItemFromObject(element, "timer");
          if (item) {
            action->timer = item->valueint;
            cJSON_Delete(item);
          }
          cJSON_Delete(element);
        };
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "file_size=%d,metadate num=%d", file_size, elenum);
      } else {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "metadate file '%V' format error or isn't array", &path);
      }

      ngx_pfree(s->connection->pool, filebuffer);
      cJSON_Delete(root);
    }
  }

  return NGX_OK;
}

static ngx_int_t ngx_rtmp_record_init(ngx_rtmp_session_t *s) {
  ngx_rtmp_record_app_conf_t *racf, **rracf;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_rtmp_record_ctx_t *ctx;
  ngx_uint_t n;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);

  if (ctx) {
    return NGX_OK;
  }

  racf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_record_module);

  if (racf == NULL || racf->rec.nelts == 0) {
    return NGX_OK;
  }

  ctx = ngx_pcalloc(s->connection->pool, sizeof(ngx_rtmp_record_ctx_t));

  if (ctx == NULL) {
    return NGX_ERROR;
  }

  ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_record_module);

  if (ngx_array_init(&ctx->rec, s->connection->pool, racf->rec.nelts,
                     sizeof(ngx_rtmp_record_rec_ctx_t)) != NGX_OK) {
    return NGX_ERROR;
  }

  rracf = racf->rec.elts;

  rctx = ngx_array_push_n(&ctx->rec, racf->rec.nelts);

  if (rctx == NULL) {
    return NGX_ERROR;
  }

  for (n = 0; n < racf->rec.nelts; ++n, ++rracf, ++rctx) {
    ngx_memzero(rctx, sizeof(*rctx));

    rctx->conf = *rracf;
    rctx->file.fd = NGX_INVALID_FILE;
    rctx->metadata_file.fd = NGX_INVALID_FILE;
  }

  return NGX_OK;
}

static void ngx_rtmp_record_start(ngx_rtmp_session_t *s) {
  ngx_rtmp_record_app_conf_t *racf;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_rtmp_record_ctx_t *ctx;
  ngx_uint_t n;

  racf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_record_module);
  if (racf == NULL || racf->rec.nelts == 0) {
    return;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);
  if (ctx == NULL) {
    return;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0, "record: start");

  rctx = ctx->rec.elts;
  for (n = 0; n < ctx->rec.nelts; ++n, ++rctx) {
    if (rctx->conf->flags & (NGX_RTMP_RECORD_OFF | NGX_RTMP_RECORD_MANUAL)) {
      continue;
    }
    ngx_rtmp_record_node_open(s, rctx);
  }
}

void ngx_rtmp_record_xueersi_start(ngx_rtmp_session_t *s) {
  ngx_rtmp_record_app_conf_t *racf;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_rtmp_record_ctx_t *ctx;
  ngx_uint_t n;

  racf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_record_module);
  if (racf == NULL || racf->rec.nelts == 0) {
    return;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);
  if (ctx == NULL) {
    return;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: xueersi_start");

  rctx = ctx->rec.elts;
  for (n = 0; n < ctx->rec.nelts; ++n, ++rctx) {
    if (rctx->conf->flags & (NGX_RTMP_RECORD_OFF)) {
      continue;
    }

    if ((rctx->conf->flags & NGX_RTMP_RECORD_XUE) &&
        (rctx->conf->flags & NGX_RTMP_RECORD_MANUAL)) {
      ngx_rtmp_record_node_open(s, rctx);
    }
  }
}

void ngx_rtmp_record_xueersi_stop(ngx_rtmp_session_t *s) {
  ngx_rtmp_record_app_conf_t *racf;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_rtmp_record_ctx_t *ctx;
  ngx_uint_t n;

  racf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_record_module);
  if (racf == NULL || racf->rec.nelts == 0) {
    return;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);
  if (ctx == NULL) {
    return;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: xueersi_stop");

  rctx = ctx->rec.elts;
  for (n = 0; n < ctx->rec.nelts; ++n, ++rctx) {
    if ((rctx->conf->flags & NGX_RTMP_RECORD_XUE) &&
        (rctx->conf->flags & NGX_RTMP_RECORD_MANUAL)) {
      ngx_rtmp_record_node_close(s, rctx);
    }
  }
}

static void ngx_rtmp_record_stop(ngx_rtmp_session_t *s) {
  ngx_rtmp_record_app_conf_t *racf;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_rtmp_record_ctx_t *ctx;
  ngx_uint_t n;

  racf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_record_module);
  if (racf == NULL || racf->rec.nelts == 0) {
    return;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);
  if (ctx == NULL) {
    return;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0, "record: stop");

  rctx = ctx->rec.elts;
  for (n = 0; n < ctx->rec.nelts; ++n, ++rctx) {
    ngx_rtmp_record_node_close(s, rctx);
  }
}

static ngx_int_t ngx_rtmp_record_publish(ngx_rtmp_session_t *s,
                                         ngx_rtmp_publish_t *v) {
  ngx_rtmp_record_app_conf_t *racf;
  ngx_rtmp_record_ctx_t *ctx;
  u_char *p;

  if (s->auto_pushed) {
    goto next;
  }

  racf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_record_module);

  if (racf == NULL || racf->rec.nelts == 0) {
    goto next;
  }
  //创建ctx
  if (ngx_rtmp_record_init(s) != NGX_OK) {
    return NGX_ERROR;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: publish %ui nodes", racf->rec.nelts);

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);

  ngx_memcpy(ctx->name, v->name, sizeof(ctx->name));
  ngx_memcpy(ctx->args, v->args, sizeof(ctx->args));

  /* terminate name on /../ */
  for (p = ctx->name; *p; ++p) {
    if (ngx_path_separator(p[0]) && p[1] == '.' && p[2] == '.' &&
        ngx_path_separator(p[3])) {
      *p = 0;
      break;
    }
  }

  ngx_rtmp_record_start(s);

next:
  return next_publish(s, v);
}

static ngx_int_t ngx_rtmp_record_stream_begin(ngx_rtmp_session_t *s,
                                              ngx_rtmp_stream_begin_t *v) {
  if (s->auto_pushed) {
    goto next;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: stream_begin");

  ngx_rtmp_record_start(s);

next:
  return next_stream_begin(s, v);
}

static ngx_int_t ngx_rtmp_record_stream_eof(ngx_rtmp_session_t *s,
                                            ngx_rtmp_stream_begin_t *v) {
  if (s->auto_pushed) {
    goto next;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: stream_eof");

  ngx_rtmp_record_stop(s);

next:
  return next_stream_eof(s, v);
}

static ngx_int_t ngx_rtmp_record_metadata_close(
    ngx_rtmp_session_t *s, ngx_rtmp_record_rec_ctx_t *rctx) {
  ngx_rtmp_record_app_conf_t *rracf;
  ngx_err_t err;
  ngx_rtmp_codec_ctx_t *ctx;

  rracf = rctx->conf;

  if (rctx->metadata_file.fd == NGX_INVALID_FILE) {
    return NGX_AGAIN;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
  if (ctx) {
    ctx->action_type = 7;
    ngx_rtmp_record_write_metadata(s, rctx, NULL);
  }

  if (ngx_close_file(rctx->metadata_file.fd) == NGX_FILE_ERROR) {
    err = ngx_errno;
    ngx_log_error(NGX_LOG_CRIT, s->connection->log, err,
                  "record metadata: %V error closing file", &rracf->id);
  }

  rctx->metadata_file.fd = NGX_INVALID_FILE;

  ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                "record metadata: %V closed", &rracf->id);

  return err;
}

static ngx_int_t ngx_rtmp_record_node_close(ngx_rtmp_session_t *s,
                                            ngx_rtmp_record_rec_ctx_t *rctx) {
  ngx_rtmp_record_app_conf_t *rracf;
  ngx_err_t err;
  void **app_conf;
  ngx_int_t rc;
  ngx_rtmp_record_done_t v;
  u_char av;

  ngx_rtmp_record_metadata_close(s, rctx);

  rracf = rctx->conf;

  if (rctx->file.fd == NGX_INVALID_FILE) {
    return NGX_AGAIN;
  }

  if (rctx->initialized) {
    av = 0;

    if (rctx->video) {
      av |= 0x01;
    }

    if (rctx->audio) {
      av |= 0x04;
    }

    if (ngx_write_file(&rctx->file, &av, 1, 4) == NGX_ERROR) {
      ngx_log_error(NGX_LOG_CRIT, s->connection->log, ngx_errno,
                    "record: %V error writing av mask", &rracf->id);
    }
  }

  if (ngx_close_file(rctx->file.fd) == NGX_FILE_ERROR) {
    err = ngx_errno;
    ngx_log_error(NGX_LOG_CRIT, s->connection->log, err,
                  "record: %V error closing file", &rracf->id);

    ngx_rtmp_record_notify_error(s, rctx);
  }

  rctx->file.fd = NGX_INVALID_FILE;

  ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0, "record: %V closed",
                 &rracf->id);

  if (rctx->actionArray) {
    ngx_array_destroy(rctx->actionArray);
    rctx->actionArray = NULL;
  }

  if (rracf->notify) {
    ngx_rtmp_send_status(s, "NetStream.Record.Stop", "status",
                         rracf->id.data ? (char *)rracf->id.data : "");
  }

  app_conf = s->app_conf;

  if (rracf->rec_conf) {
    s->app_conf = rracf->rec_conf;
  }

  v.recorder = rracf->id;
  ngx_rtmp_record_make_path(s, rctx, &v.path);

  rc = ngx_rtmp_record_done(s, &v);

  s->app_conf = app_conf;

  return rc;
}

static ngx_int_t ngx_rtmp_record_close_stream(ngx_rtmp_session_t *s,
                                              ngx_rtmp_close_stream_t *v) {
  if (s->auto_pushed) {
    goto next;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: close_stream");

  ngx_rtmp_record_stop(s);

next:
  return next_close_stream(s, v);
}

static ngx_int_t ngx_rtmp_record_write_frame(ngx_rtmp_session_t *s,
                                             ngx_rtmp_record_rec_ctx_t *rctx,
                                             ngx_rtmp_header_t *h,
                                             ngx_chain_t *in,
                                             ngx_int_t inc_nframes) {
  u_char hdr[11], *p, *ph;
  uint32_t timestamp, tag_size;
  ngx_rtmp_record_app_conf_t *rracf;

  rracf = rctx->conf;

  ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "record: %V frame: mlen=%uD", &rracf->id, h->mlen);

  if (h->type == NGX_RTMP_MSG_VIDEO) {
    rctx->video = 1;
  } else {
    rctx->audio = 1;
  }

  timestamp = h->timestamp - rctx->epoch;

  if ((int32_t)timestamp < 0) {
    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "record: %V cut timestamp=%D", &rracf->id, timestamp);

    timestamp = 0;
  }

  /* write tag header */
  ph = hdr;

  *ph++ = (u_char)h->type;

  p = (u_char *)&h->mlen;
  *ph++ = p[2];
  *ph++ = p[1];
  *ph++ = p[0];

  p = (u_char *)&timestamp;
  *ph++ = p[2];
  *ph++ = p[1];
  *ph++ = p[0];
  *ph++ = p[3];

  *ph++ = 0;
  *ph++ = 0;
  *ph++ = 0;

  tag_size = (ph - hdr) + h->mlen;

  if (ngx_write_file(&rctx->file, hdr, ph - hdr, rctx->file.offset) ==
      NGX_ERROR) {
    ngx_rtmp_record_notify_error(s, rctx);

    ngx_close_file(rctx->file.fd);

    return NGX_ERROR;
  }

  /* write tag body
   * FIXME: NGINX
   * ngx_write_chain seems to fit best
   * but it suffers from uncontrollable
   * allocations.
   * we're left with plain writing */
  for (; in; in = in->next) {
    if (in->buf->pos == in->buf->last) {
      continue;
    }

    if (ngx_write_file(&rctx->file, in->buf->pos, in->buf->last - in->buf->pos,
                       rctx->file.offset) == NGX_ERROR) {
      return NGX_ERROR;
    }
  }

  /* write tag size */
  ph = hdr;
  p = (u_char *)&tag_size;

  *ph++ = p[3];
  *ph++ = p[2];
  *ph++ = p[1];
  *ph++ = p[0];

  if (ngx_write_file(&rctx->file, hdr, ph - hdr, rctx->file.offset) ==
      NGX_ERROR) {
    return NGX_ERROR;
  }

  rctx->nframes += inc_nframes;

  /* watch max size */
  if ((rracf->max_size && rctx->file.offset >= (ngx_int_t)rracf->max_size) ||
      (rracf->max_frames && rctx->nframes >= rracf->max_frames)) {
    ngx_rtmp_record_node_close(s, rctx);
  }

  return NGX_OK;
}

static ngx_int_t ngx_rtmp_record_write_metadata(ngx_rtmp_session_t *s,
                                                ngx_rtmp_record_rec_ctx_t *rctx,
                                                ngx_rtmp_header_t *h) {
  uint32_t timestamp;
  ngx_rtmp_record_app_conf_t *rracf;
  ngx_rtmp_xueersi_action_t *action;
  ngx_rtmp_xueersi_action_t *first_action;
  ngx_int_t i;
  ngx_uint_t isAction = 0;
  ngx_rtmp_codec_ctx_t *ctx;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
  if (ctx == NULL || !rctx->actionArray) {
    return NGX_OK;
  }

  if (rctx->metadata_file.fd == NGX_INVALID_FILE) {
    return NGX_OK;
  }

  rracf = rctx->conf;

  if (!rctx->initialized) {
    ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                  "record metadata: %V fail not initialized", &rracf->id);
    return NGX_OK;
  }

  ngx_log_error(NGX_LOG_ERR, s->connection->log, 0, "record metadata: %V frame",
                &rracf->id);

  if (ctx->action_type != 6 && ctx->action_type != 7) {
    timestamp = (h->timestamp - rctx->epoch) / 1000;

    if ((int32_t)timestamp < 0) {
      ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                     "record: %V cut timestamp=%D", &rracf->id, timestamp);

      timestamp = 0;
    } else {
      ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                     "record:oldtimestamp=%D,timestamp=%D,epoch=%d",
                     h->timestamp, timestamp, (int32_t)rctx->epoch);
    }
  } else {
    timestamp = ngx_current_msec / 1000;
  }

  if (ctx->action_type == 6 || ctx->action_type == 7 ||
      ((ctx->action_type == 1 || ctx->action_type == 2 ||
        ctx->action_type == 4 || ctx->action_type == 8 ||
        ctx->action_type == 10 || ctx->action_type >= MIN_NEW_CATEGORY) &&
       (rracf->flags & NGX_RTMP_RECORD_MANUAL))) {
    if (ctx->action_type == 1 || ctx->action_type == 4 ||
        ctx->action_type == 8 || ctx->action_type == 10) {
      first_action = rctx->actionArray->elts;
      for (i = rctx->actionArray->nelts - 1; i >= 0; i--) {
        if (ctx->action_type == first_action[i].action_type &&
            ngx_strcasecmp((u_char *)ctx->actionid,
                           (u_char *)first_action[i].actionid) == 0 &&
            ngx_strcasecmp((u_char *)ctx->questionType,
                           (u_char *)first_action[i].type) == 0) {
          if (first_action[i].endtime) {
            break;
          } else {
            return NGX_OK;
          }
        }
      }
    }
    isAction = 1;
    action = ngx_array_push(rctx->actionArray);
    action->action_type = ctx->action_type;
    strcpy(action->actionid, ctx->actionid);
    strcpy(action->date, ctx->date);
    action->begintime = timestamp;
    action->endtime = 0;
    action->timer = ctx->timer;
    strcpy(action->type, ctx->questionType);
    strcpy(action->url, ctx->url);
  } else if ((ctx->action_type == 3 || ctx->action_type == 5 ||
              ctx->action_type == 9 || ctx->action_type == 11) &&
             (rracf->flags & NGX_RTMP_RECORD_MANUAL)) {
    first_action = rctx->actionArray->elts;
    for (i = rctx->actionArray->nelts - 1; i >= 0; i--) {
      if ((ctx->action_type == 3 && first_action[i].action_type == 1) ||
          (ctx->action_type == 5 && first_action[i].action_type == 4) ||
          (ctx->action_type == 9 && first_action[i].action_type == 8) ||
          (ctx->action_type == 11 && first_action[i].action_type == 10)) {
        action = &(first_action[i]);
        if ((ngx_strlen(ctx->actionid) == 0 ||
             (ngx_strcasecmp((u_char *)ctx->actionid,
                             (u_char *)action->actionid) == 0 &&
              ngx_strcasecmp((u_char *)ctx->questionType,
                             (u_char *)action->type) == 0)) &&
            action->endtime == 0) {
          isAction = 1;
          action->endtime = timestamp;
          break;
        }
      }
    }
  }

  if (isAction) {
    if (rracf->flags & NGX_RTMP_RECORD_MANUAL) {
      ngx_log_error(
          NGX_LOG_ERR, s->connection->log, 0,
          "manualwritefile stream_name=%s,category=%ui,begintime=%ui,num=%ui",
          s->stream_name, ctx->action_type, (ngx_uint_t)timestamp,
          rctx->actionArray->nelts);
    } else {
      ngx_log_error(
          NGX_LOG_ERR, s->connection->log, 0,
          "writefile stream_name=%s,category=%ui,begintime=%ui,num=%ui",
          s->stream_name, ctx->action_type, (ngx_uint_t)timestamp,
          rctx->actionArray->nelts);
    }

    u_char *filebuffer;
    cJSON *root, *element;
    root = cJSON_CreateArray();
    first_action = rctx->actionArray->elts;
    for (i = 0; i < (ngx_int_t)rctx->actionArray->nelts; i++) {
      cJSON_AddItemToArray(root, element = cJSON_CreateObject());
      cJSON_AddNumberToObject(element, "category", first_action[i].action_type);
      cJSON_AddNumberToObject(element, "begintime", first_action[i].begintime);

      if (first_action[i].action_type != 6 &&
          first_action[i].action_type != 7) {
        if (first_action[i].endtime > 0) {
          cJSON_AddNumberToObject(element, "endtime", first_action[i].endtime);
        }

        if (ngx_strlen(first_action[i].actionid)) {
          cJSON_AddStringToObject(element, "id", first_action[i].actionid);
        }

        if (ngx_strlen(first_action[i].type)) {
          cJSON_AddStringToObject(element, "type", first_action[i].type);
        }

        if (ngx_strlen(first_action[i].url)) {
          cJSON_AddStringToObject(element, "url", first_action[i].url);
        }

        if (ngx_strlen(first_action[i].date)) {
          cJSON_AddStringToObject(element, "date", first_action[i].date);
        }

        if (first_action[i].timer > 0) {
          cJSON_AddNumberToObject(element, "timer", first_action[i].timer);
        }
      }
    }

    filebuffer = (u_char *)cJSON_Print(root);
    cJSON_Delete(root);

    if (ngx_write_file(&rctx->metadata_file, filebuffer, ngx_strlen(filebuffer),
                       0) == NGX_ERROR) {
      ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                    "stream_name=%s record %V write json file error ",
                    s->stream_name, &rracf->id);
      ngx_close_file(rctx->metadata_file.fd);
      free(filebuffer);
      return NGX_ERROR;
    }
    free(filebuffer);
  }

  return NGX_OK;
}

static size_t ngx_rtmp_record_get_chain_mlen(ngx_chain_t *in) {
  size_t ret;

  for (ret = 0; in; in = in->next) {
    ret += (in->buf->last - in->buf->pos);
  }

  return ret;
}

ngx_int_t ngx_rtmp_record_metadata_start(ngx_rtmp_session_t *s,
                                         ngx_rtmp_header_t *h,
                                         ngx_chain_t *in) {
  ngx_rtmp_record_ctx_t *ctx;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_uint_t n;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);

  if (ctx == NULL) {
    return NGX_OK;
  }

  rctx = ctx->rec.elts;

  for (n = 0; n < ctx->rec.nelts; ++n, ++rctx) {
    if (rctx->conf->flags & (NGX_RTMP_RECORD_METADATA)) {
      ngx_rtmp_record_node_metadata(s, rctx, h, in);
    }
  }

  return NGX_OK;
}

static ngx_int_t ngx_rtmp_record_node_metadata(ngx_rtmp_session_t *s,
                                               ngx_rtmp_record_rec_ctx_t *rctx,
                                               ngx_rtmp_header_t *h,
                                               ngx_chain_t *in) {
  if (rctx->initialized == 1) {
    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "record: metadate: mlen=%uD", h->mlen);
    ngx_rtmp_record_write_frame(s, rctx, h, in, 0);
  }
  return NGX_OK;
}

ngx_int_t ngx_rtmp_record_metadata(ngx_rtmp_session_t *s,
                                   ngx_rtmp_header_t *h) {
  ngx_rtmp_record_ctx_t *ctx;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_uint_t n;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);

  if (ctx == NULL) {
    return NGX_OK;
  }

  rctx = ctx->rec.elts;

  for (n = 0; n < ctx->rec.nelts; ++n, ++rctx) {
    ngx_rtmp_record_write_metadata(s, rctx, h);
  }

  return NGX_OK;
}

static ngx_int_t ngx_rtmp_record_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                                    ngx_chain_t *in) {
  ngx_rtmp_record_ctx_t *ctx;
  ngx_rtmp_record_rec_ctx_t *rctx;
  ngx_uint_t n;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_record_module);

  if (ctx == NULL) {
    return NGX_OK;
  }

  rctx = ctx->rec.elts;

  for (n = 0; n < ctx->rec.nelts; ++n, ++rctx) {
    ngx_rtmp_record_node_av(s, rctx, h, in);
  }

  return NGX_OK;
}

static ngx_int_t ngx_rtmp_record_node_av(ngx_rtmp_session_t *s,
                                         ngx_rtmp_record_rec_ctx_t *rctx,
                                         ngx_rtmp_header_t *h,
                                         ngx_chain_t *in) {
  ngx_time_t next;
  ngx_rtmp_header_t ch;
  ngx_rtmp_codec_ctx_t *codec_ctx;
  ngx_int_t keyframe, brkframe;
  ngx_rtmp_record_app_conf_t *rracf;

  rracf = rctx->conf;

  if (rracf->flags & NGX_RTMP_RECORD_OFF) {
    ngx_rtmp_record_node_close(s, rctx);
    return NGX_OK;
  }

  keyframe =
      (h->type == NGX_RTMP_MSG_VIDEO)
          ? (ngx_rtmp_get_video_frame_type(in) == NGX_RTMP_VIDEO_KEY_FRAME)
          : 0;

  brkframe = (h->type == NGX_RTMP_MSG_VIDEO)
                 ? keyframe
                 : (rracf->flags & NGX_RTMP_RECORD_VIDEO) == 0;

  if (brkframe && (rracf->flags & NGX_RTMP_RECORD_MANUAL) == 0) {
    if (rracf->interval != (ngx_msec_t)NGX_CONF_UNSET) {
      next = rctx->last;
      next.msec += rracf->interval;
      next.sec += (next.msec / 1000);
      next.msec %= 1000;

      if (ngx_cached_time->sec > next.sec ||
          (ngx_cached_time->sec == next.sec &&
           ngx_cached_time->msec > next.msec)) {
        ngx_rtmp_record_node_close(s, rctx);
        ngx_rtmp_record_node_open(s, rctx);
      }

    } else if (!rctx->failed) {
      ngx_rtmp_record_node_open(s, rctx);
    }
  }

  if ((rracf->flags & NGX_RTMP_RECORD_MANUAL) && !brkframe &&
      rctx->nframes == 0) {
    return NGX_OK;
  }

  //没有fd
  if (rctx->file.fd == NGX_INVALID_FILE) {
    return NGX_OK;
  }

  //不录声音
  if (h->type == NGX_RTMP_MSG_AUDIO &&
      (rracf->flags & NGX_RTMP_RECORD_AUDIO) == 0) {
    return NGX_OK;
  }

  if (h->type == NGX_RTMP_MSG_VIDEO &&
      (rracf->flags & NGX_RTMP_RECORD_VIDEO) == 0 &&
      ((rracf->flags & NGX_RTMP_RECORD_KEYFRAMES) == 0 || !keyframe)) {
    return NGX_OK;
  }

  codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

  if (!rctx->initialized) {
    rctx->initialized = 1;
    rctx->epoch = h->timestamp - rctx->time_shift;
    //写入头部
    if (rctx->file.offset == 0 &&
        ngx_rtmp_record_write_header(&rctx->file) != NGX_OK) {
      ngx_rtmp_record_node_close(s, rctx);
      return NGX_OK;
    }
  }

  codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
  if (codec_ctx) {
    ch = *h;

    /* AAC header */
    if (!rctx->aac_header_sent && codec_ctx->aac_header &&
        (rracf->flags & NGX_RTMP_RECORD_AUDIO)) {
      ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                     "record: %V writing AAC header", &rracf->id);

      ch.type = NGX_RTMP_MSG_AUDIO;
      ch.mlen = ngx_rtmp_record_get_chain_mlen(codec_ctx->aac_header);

      if (ngx_rtmp_record_write_frame(s, rctx, &ch, codec_ctx->aac_header, 0) !=
          NGX_OK) {
        return NGX_OK;
      }

      if (codec_ctx) {
        codec_ctx->action_type = 6;
        ngx_rtmp_record_write_metadata(s, rctx, NULL);
      }

      rctx->aac_header_sent = 1;
    }

    /* AVC header */
    if (!rctx->avc_header_sent && codec_ctx->avc_header &&
        (rracf->flags & (NGX_RTMP_RECORD_VIDEO | NGX_RTMP_RECORD_KEYFRAMES))) {
      ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                     "record: %V writing AVC header", &rracf->id);

      ch.type = NGX_RTMP_MSG_VIDEO;
      ch.mlen = ngx_rtmp_record_get_chain_mlen(codec_ctx->avc_header);

      if (ngx_rtmp_record_write_frame(s, rctx, &ch, codec_ctx->avc_header, 0) !=
          NGX_OK) {
        return NGX_OK;
      }

      rctx->avc_header_sent = 1;
    }
  }

  if (h->type == NGX_RTMP_MSG_VIDEO) {
    if (codec_ctx && codec_ctx->video_codec_id == NGX_RTMP_VIDEO_H264 &&
        !rctx->avc_header_sent) {
      ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                     "record: %V skipping until H264 header", &rracf->id);
      return NGX_OK;
    }

    if (ngx_rtmp_get_video_frame_type(in) == NGX_RTMP_VIDEO_KEY_FRAME &&
        ((codec_ctx && codec_ctx->video_codec_id != NGX_RTMP_VIDEO_H264) ||
         !ngx_rtmp_is_codec_header(in))) {
      rctx->video_key_sent = 1;
    }

    if (!rctx->video_key_sent) {
      ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                     "record: %V skipping until keyframe", &rracf->id);
      return NGX_OK;
    }

  } else {
    if (codec_ctx && codec_ctx->audio_codec_id == NGX_RTMP_AUDIO_AAC &&
        !rctx->aac_header_sent) {
      ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                     "record: %V skipping until AAC header", &rracf->id);
      return NGX_OK;
    }
  }

  return ngx_rtmp_record_write_frame(s, rctx, h, in, 1);
}

static ngx_int_t ngx_rtmp_record_done_init(ngx_rtmp_session_t *s,
                                           ngx_rtmp_record_done_t *v) {
  return NGX_OK;
}

static char *ngx_rtmp_record_recorder(ngx_conf_t *cf, ngx_command_t *cmd,
                                      void *conf) {
  char *rv;
  ngx_int_t i;
  ngx_str_t *value;
  ngx_conf_t save;
  ngx_module_t **modules;
  ngx_rtmp_module_t *module;
  ngx_rtmp_core_app_conf_t *cacf, **pcacf, *rcacf;
  ngx_rtmp_record_app_conf_t *racf, **pracf, *rracf;
  ngx_rtmp_conf_ctx_t *ctx, *pctx;

  value = cf->args->elts;

  cacf = ngx_rtmp_conf_get_module_app_conf(cf, ngx_rtmp_core_module);

  racf = ngx_rtmp_conf_get_module_app_conf(cf, ngx_rtmp_record_module);

  ctx = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_conf_ctx_t));
  if (ctx == NULL) {
    return NGX_CONF_ERROR;
  }

  pctx = cf->ctx;

  ctx->main_conf = pctx->main_conf;
  ctx->srv_conf = pctx->srv_conf;

  ctx->app_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_rtmp_max_module);
  if (ctx->app_conf == NULL) {
    return NGX_CONF_ERROR;
  }

#if (nginx_version >= 1009011)
  modules = cf->cycle->modules;
#else
  modules = ngx_modules;
#endif

  for (i = 0; modules[i]; i++) {
    if (modules[i]->type != NGX_RTMP_MODULE) {
      continue;
    }

    module = modules[i]->ctx;

    if (module->create_app_conf) {
      ctx->app_conf[modules[i]->ctx_index] = module->create_app_conf(cf);
      if (ctx->app_conf[modules[i]->ctx_index] == NULL) {
        return NGX_CONF_ERROR;
      }
    }
  }

  /* add to sub-applications */
  rcacf = ctx->app_conf[ngx_rtmp_core_module.ctx_index];
  rcacf->app_conf = ctx->app_conf;
  pcacf = ngx_array_push(&cacf->applications);
  if (pcacf == NULL) {
    return NGX_CONF_ERROR;
  }
  *pcacf = rcacf;

  /* add to recorders */
  rracf = ctx->app_conf[ngx_rtmp_record_module.ctx_index];
  rracf->rec_conf = ctx->app_conf;
  pracf = ngx_array_push(&racf->rec);
  if (pracf == NULL) {
    return NGX_CONF_ERROR;
  }
  *pracf = rracf;

  rracf->id = value[1];

  save = *cf;
  cf->ctx = ctx;
  cf->cmd_type = NGX_RTMP_REC_CONF;

  rv = ngx_conf_parse(cf, NULL);
  *cf = save;

  return rv;
}

static ngx_int_t ngx_rtmp_record_postconfiguration(ngx_conf_t *cf) {
  ngx_rtmp_core_main_conf_t *cmcf;
  ngx_rtmp_handler_pt *h;

  ngx_rtmp_record_done = ngx_rtmp_record_done_init;

  cmcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_core_module);

  h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_AUDIO]);
  *h = ngx_rtmp_record_av;

  h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_VIDEO]);
  *h = ngx_rtmp_record_av;

  next_publish = ngx_rtmp_publish;
  ngx_rtmp_publish = ngx_rtmp_record_publish;

  next_close_stream = ngx_rtmp_close_stream;
  ngx_rtmp_close_stream = ngx_rtmp_record_close_stream;

  next_stream_begin = ngx_rtmp_stream_begin;
  ngx_rtmp_stream_begin = ngx_rtmp_record_stream_begin;

  next_stream_eof = ngx_rtmp_stream_eof;
  ngx_rtmp_stream_eof = ngx_rtmp_record_stream_eof;

  return NGX_OK;
}
/////////////////////////////////////////cjson/////////////////////////////////////////

#define true ((cJSON_bool)1)
#define false ((cJSON_bool)0)

typedef struct {
  const unsigned char *json;
  size_t position;
} error;
static error global_error = {NULL, 0};

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void) {
  return (const char *)(global_error.json + global_error.position);
}

CJSON_PUBLIC(char *) cJSON_GetStringValue(cJSON *item) {
  if (!cJSON_IsString(item)) {
    return NULL;
  }

  return item->valuestring;
}

/* This is a safeguard to prevent copy-pasters from using incompatible C and
 * header files */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 7) || \
    (CJSON_VERSION_PATCH != 7)
#error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

CJSON_PUBLIC(const char *) cJSON_Version(void) {
  static char version[15];
  sprintf(version, "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR,
          CJSON_VERSION_PATCH);

  return version;
}

/* Case insensitive string comparison, doesn't consider two NULL pointers equal
 * though */
static int case_insensitive_strcmp(const unsigned char *string1,
                                   const unsigned char *string2) {
  if ((string1 == NULL) || (string2 == NULL)) {
    return 1;
  }

  if (string1 == string2) {
    return 0;
  }

  for (; tolower(*string1) == tolower(*string2); (void)string1++, string2++) {
    if (*string1 == '\0') {
      return 0;
    }
  }

  return tolower(*string1) - tolower(*string2);
}

typedef struct internal_hooks {
  void *(*allocate)(size_t size);
  void (*deallocate)(void *pointer);
  void *(*reallocate)(void *pointer, size_t size);
} internal_hooks;

#if defined(_MSC_VER)
/* work around MSVC error C2322: '...' address of dillimport '...' is not static
 */
static void *internal_malloc(size_t size) { return malloc(size); }
static void internal_free(void *pointer) { free(pointer); }
static void *internal_realloc(void *pointer, size_t size) {
  return realloc(pointer, size);
}
#else
#define internal_malloc malloc
#define internal_free free
#define internal_realloc realloc
#endif

static internal_hooks global_hooks = {internal_malloc, internal_free,
                                      internal_realloc};

static unsigned char *cJSON_strdup(const unsigned char *string,
                                   const internal_hooks *const hooks) {
  size_t length = 0;
  unsigned char *copy = NULL;

  if (string == NULL) {
    return NULL;
  }

  length = strlen((const char *)string) + sizeof("");
  copy = (unsigned char *)hooks->allocate(length);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, string, length);

  return copy;
}

CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks *hooks) {
  if (hooks == NULL) {
    /* Reset hooks */
    global_hooks.allocate = malloc;
    global_hooks.deallocate = free;
    global_hooks.reallocate = realloc;
    return;
  }

  global_hooks.allocate = malloc;
  if (hooks->malloc_fn != NULL) {
    global_hooks.allocate = hooks->malloc_fn;
  }

  global_hooks.deallocate = free;
  if (hooks->free_fn != NULL) {
    global_hooks.deallocate = hooks->free_fn;
  }

  /* use realloc only if both free and malloc are used */
  global_hooks.reallocate = NULL;
  if ((global_hooks.allocate == malloc) && (global_hooks.deallocate == free)) {
    global_hooks.reallocate = realloc;
  }
}

/* Internal constructor. */
static cJSON *cJSON_New_Item(const internal_hooks *const hooks) {
  cJSON *node = (cJSON *)hooks->allocate(sizeof(cJSON));
  if (node) {
    memset(node, '\0', sizeof(cJSON));
  }

  return node;
}

/* Delete a cJSON structure. */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item) {
  cJSON *next = NULL;
  while (item != NULL) {
    next = item->next;
    if (!(item->type & cJSON_IsReference) && (item->child != NULL)) {
      cJSON_Delete(item->child);
    }
    if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL)) {
      global_hooks.deallocate(item->valuestring);
    }
    if (!(item->type & cJSON_StringIsConst) && (item->string != NULL)) {
      global_hooks.deallocate(item->string);
    }
    global_hooks.deallocate(item);
    item = next;
  }
}

/* get the decimal point character of the current locale */
static unsigned char get_decimal_point(void) {
#ifdef ENABLE_LOCALES
  struct lconv *lconv = localeconv();
  return (unsigned char)lconv->decimal_point[0];
#else
  return '.';
#endif
}

typedef struct {
  const unsigned char *content;
  size_t length;
  size_t offset;
  size_t depth; /* How deeply nested (in arrays/objects) is the input at the
                   current offset. */
  internal_hooks hooks;
} parse_buffer;

/* check if the given size is left to read in a given parse buffer (starting
 * with 1) */
#define can_read(buffer, size) \
  ((buffer != NULL) && (((buffer)->offset + size) <= (buffer)->length))
/* check if the buffer can be accessed at the given index (starting with 0) */
#define can_access_at_index(buffer, index) \
  ((buffer != NULL) && (((buffer)->offset + index) < (buffer)->length))
#define cannot_access_at_index(buffer, index) \
  (!can_access_at_index(buffer, index))
/* get a pointer to the buffer at the position */
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/* Parse the input text to generate a number, and populate the result into item.
 */
static cJSON_bool parse_number(cJSON *const item,
                               parse_buffer *const input_buffer) {
  double number = 0;
  unsigned char *after_end = NULL;
  unsigned char number_c_string[64];
  unsigned char decimal_point = get_decimal_point();
  size_t i = 0;

  if ((input_buffer == NULL) || (input_buffer->content == NULL)) {
    return false;
  }

  /* copy the number into a temporary buffer and replace '.' with the decimal
   * point of the current locale (for strtod) This also takes care of '\0' not
   * necessarily being available for marking the end of the input */
  for (i = 0; (i < (sizeof(number_c_string) - 1)) &&
              can_access_at_index(input_buffer, i);
       i++) {
    switch (buffer_at_offset(input_buffer)[i]) {
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
      case '+':
      case '-':
      case 'e':
      case 'E':
        number_c_string[i] = buffer_at_offset(input_buffer)[i];
        break;

      case '.':
        number_c_string[i] = decimal_point;
        break;

      default:
        goto loop_end;
    }
  }
loop_end:
  number_c_string[i] = '\0';

  number = strtod((const char *)number_c_string, (char **)&after_end);
  if (number_c_string == after_end) {
    return false; /* parse_error */
  }

  item->valuedouble = number;

  /* use saturation in case of overflow */
  if (number >= INT_MAX) {
    item->valueint = INT_MAX;
  } else if (number <= INT_MIN) {
    item->valueint = INT_MIN;
  } else {
    item->valueint = (int)number;
  }

  item->type = cJSON_Number;

  input_buffer->offset += (size_t)(after_end - number_c_string);
  return true;
}

/* don't ask me, but the original cJSON_SetNumberValue returns an integer or
 * double */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number) {
  if (number >= INT_MAX) {
    object->valueint = INT_MAX;
  } else if (number <= INT_MIN) {
    object->valueint = INT_MIN;
  } else {
    object->valueint = (int)number;
  }

  return object->valuedouble = number;
}

typedef struct {
  unsigned char *buffer;
  size_t length;
  size_t offset;
  size_t depth; /* current nesting depth (for formatted printing) */
  cJSON_bool noalloc;
  cJSON_bool format; /* is this print a formatted print */
  internal_hooks hooks;
} printbuffer;

/* realloc printbuffer if necessary to have at least "needed" bytes more */
static unsigned char *ensure(printbuffer *const p, size_t needed) {
  unsigned char *newbuffer = NULL;
  size_t newsize = 0;

  if ((p == NULL) || (p->buffer == NULL)) {
    return NULL;
  }

  if ((p->length > 0) && (p->offset >= p->length)) {
    /* make sure that offset is valid */
    return NULL;
  }

  if (needed > INT_MAX) {
    /* sizes bigger than INT_MAX are currently not supported */
    return NULL;
  }

  needed += p->offset + 1;
  if (needed <= p->length) {
    return p->buffer + p->offset;
  }

  if (p->noalloc) {
    return NULL;
  }

  /* calculate new buffer size */
  if (needed > (INT_MAX / 2)) {
    /* overflow of int, use INT_MAX if possible */
    if (needed <= INT_MAX) {
      newsize = INT_MAX;
    } else {
      return NULL;
    }
  } else {
    newsize = needed * 2;
  }

  if (p->hooks.reallocate != NULL) {
    /* reallocate with realloc if available */
    newbuffer = (unsigned char *)p->hooks.reallocate(p->buffer, newsize);
    if (newbuffer == NULL) {
      p->hooks.deallocate(p->buffer);
      p->length = 0;
      p->buffer = NULL;

      return NULL;
    }
  } else {
    /* otherwise reallocate manually */
    newbuffer = (unsigned char *)p->hooks.allocate(newsize);
    if (!newbuffer) {
      p->hooks.deallocate(p->buffer);
      p->length = 0;
      p->buffer = NULL;

      return NULL;
    }
    if (newbuffer) {
      memcpy(newbuffer, p->buffer, p->offset + 1);
    }
    p->hooks.deallocate(p->buffer);
  }
  p->length = newsize;
  p->buffer = newbuffer;

  return newbuffer + p->offset;
}

/* calculate the new length of the string in a printbuffer and update the offset
 */
static void update_offset(printbuffer *const buffer) {
  const unsigned char *buffer_pointer = NULL;
  if ((buffer == NULL) || (buffer->buffer == NULL)) {
    return;
  }
  buffer_pointer = buffer->buffer + buffer->offset;

  buffer->offset += strlen((const char *)buffer_pointer);
}

/* Render the number nicely from the given item into a string. */
static cJSON_bool print_number(const cJSON *const item,
                               printbuffer *const output_buffer) {
  unsigned char *output_pointer = NULL;
  double d = item->valuedouble;
  int length = 0;
  size_t i = 0;
  unsigned char
      number_buffer[26]; /* temporary buffer to print the number into */
  unsigned char decimal_point = get_decimal_point();
  double test;

  if (output_buffer == NULL) {
    return false;
  }

  /* This checks for NaN and Infinity */
  if ((d * 0) != 0) {
    length = sprintf((char *)number_buffer, "null");
  } else {
    /* Try 15 decimal places of precision to avoid nonsignificant nonzero digits
     */
    length = sprintf((char *)number_buffer, "%1.15g", d);

    /* Check whether the original double can be recovered */
    if ((sscanf((char *)number_buffer, "%lg", &test) != 1) ||
        ((double)test != d)) {
      /* If not, print with 17 decimal places of precision */
      length = sprintf((char *)number_buffer, "%1.17g", d);
    }
  }

  /* sprintf failed or buffer overrun occured */
  if ((length < 0) || (length > (int)(sizeof(number_buffer) - 1))) {
    return false;
  }

  /* reserve appropriate space in the output */
  output_pointer = ensure(output_buffer, (size_t)length + sizeof(""));
  if (output_pointer == NULL) {
    return false;
  }

  /* copy the printed number to the output and replace locale
   * dependent decimal point with '.' */
  for (i = 0; i < ((size_t)length); i++) {
    if (number_buffer[i] == decimal_point) {
      output_pointer[i] = '.';
      continue;
    }

    output_pointer[i] = number_buffer[i];
  }
  output_pointer[i] = '\0';

  output_buffer->offset += (size_t)length;

  return true;
}

/* parse 4 digit hexadecimal number */
static unsigned parse_hex4(const unsigned char *const input) {
  unsigned int h = 0;
  size_t i = 0;

  for (i = 0; i < 4; i++) {
    /* parse digit */
    if ((input[i] >= '0') && (input[i] <= '9')) {
      h += (unsigned int)input[i] - '0';
    } else if ((input[i] >= 'A') && (input[i] <= 'F')) {
      h += (unsigned int)10 + input[i] - 'A';
    } else if ((input[i] >= 'a') && (input[i] <= 'f')) {
      h += (unsigned int)10 + input[i] - 'a';
    } else /* invalid */
    {
      return 0;
    }

    if (i < 3) {
      /* shift left to make place for the next nibble */
      h = h << 4;
    }
  }

  return h;
}

/* converts a UTF-16 literal to UTF-8
 * A literal can be one or two sequences of the form \uXXXX */
static unsigned char utf16_literal_to_utf8(
    const unsigned char *const input_pointer,
    const unsigned char *const input_end, unsigned char **output_pointer) {
  long unsigned int codepoint = 0;
  unsigned int first_code = 0;
  const unsigned char *first_sequence = input_pointer;
  unsigned char utf8_length = 0;
  unsigned char utf8_position = 0;
  unsigned char sequence_length = 0;
  unsigned char first_byte_mark = 0;

  if ((input_end - first_sequence) < 6) {
    /* input ends unexpectedly */
    goto fail;
  }

  /* get the first utf16 sequence */
  first_code = parse_hex4(first_sequence + 2);

  /* check that the code is valid */
  if (((first_code >= 0xDC00) && (first_code <= 0xDFFF))) {
    goto fail;
  }

  /* UTF16 surrogate pair */
  if ((first_code >= 0xD800) && (first_code <= 0xDBFF)) {
    const unsigned char *second_sequence = first_sequence + 6;
    unsigned int second_code = 0;
    sequence_length = 12; /* \uXXXX\uXXXX */

    if ((input_end - second_sequence) < 6) {
      /* input ends unexpectedly */
      goto fail;
    }

    if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u')) {
      /* missing second half of the surrogate pair */
      goto fail;
    }

    /* get the second utf16 sequence */
    second_code = parse_hex4(second_sequence + 2);
    /* check that the code is valid */
    if ((second_code < 0xDC00) || (second_code > 0xDFFF)) {
      /* invalid second half of the surrogate pair */
      goto fail;
    }

    /* calculate the unicode codepoint from the surrogate pair */
    codepoint =
        0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
  } else {
    sequence_length = 6; /* \uXXXX */
    codepoint = first_code;
  }

  /* encode as UTF-8
   * takes at maximum 4 bytes to encode:
   * 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
  if (codepoint < 0x80) {
    /* normal ascii, encoding 0xxxxxxx */
    utf8_length = 1;
  } else if (codepoint < 0x800) {
    /* two bytes, encoding 110xxxxx 10xxxxxx */
    utf8_length = 2;
    first_byte_mark = 0xC0; /* 11000000 */
  } else if (codepoint < 0x10000) {
    /* three bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx */
    utf8_length = 3;
    first_byte_mark = 0xE0; /* 11100000 */
  } else if (codepoint <= 0x10FFFF) {
    /* four bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx 10xxxxxx */
    utf8_length = 4;
    first_byte_mark = 0xF0; /* 11110000 */
  } else {
    /* invalid unicode codepoint */
    goto fail;
  }

  /* encode as utf8 */
  for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0;
       utf8_position--) {
    /* 10xxxxxx */
    (*output_pointer)[utf8_position] =
        (unsigned char)((codepoint | 0x80) & 0xBF);
    codepoint >>= 6;
  }
  /* encode first byte */
  if (utf8_length > 1) {
    (*output_pointer)[0] =
        (unsigned char)((codepoint | first_byte_mark) & 0xFF);
  } else {
    (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F);
  }

  *output_pointer += utf8_length;

  return sequence_length;

fail:
  return 0;
}

/* Parse the input text into an unescaped cinput, and populate item. */
static cJSON_bool parse_string(cJSON *const item,
                               parse_buffer *const input_buffer) {
  const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;
  const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;
  unsigned char *output_pointer = NULL;
  unsigned char *output = NULL;

  /* not a string */
  if (buffer_at_offset(input_buffer)[0] != '\"') {
    goto fail;
  }

  {
    /* calculate approximate size of the output (overestimate) */
    size_t allocation_length = 0;
    size_t skipped_bytes = 0;
    while (
        ((size_t)(input_end - input_buffer->content) < input_buffer->length) &&
        (*input_end != '\"')) {
      /* is escape sequence */
      if (input_end[0] == '\\') {
        if ((size_t)(input_end + 1 - input_buffer->content) >=
            input_buffer->length) {
          /* prevent buffer overflow when last input character is a backslash */
          goto fail;
        }
        skipped_bytes++;
        input_end++;
      }
      input_end++;
    }
    if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) ||
        (*input_end != '\"')) {
      goto fail; /* string ended unexpectedly */
    }

    /* This is at most how much we need for the output */
    allocation_length =
        (size_t)(input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
    output = (unsigned char *)input_buffer->hooks.allocate(allocation_length +
                                                           sizeof(""));
    if (output == NULL) {
      goto fail; /* allocation failure */
    }
  }

  output_pointer = output;
  /* loop through the string literal */
  while (input_pointer < input_end) {
    if (*input_pointer != '\\') {
      *output_pointer++ = *input_pointer++;
    }
    /* escape sequence */
    else {
      unsigned char sequence_length = 2;
      if ((input_end - input_pointer) < 1) {
        goto fail;
      }

      switch (input_pointer[1]) {
        case 'b':
          *output_pointer++ = '\b';
          break;
        case 'f':
          *output_pointer++ = '\f';
          break;
        case 'n':
          *output_pointer++ = '\n';
          break;
        case 'r':
          *output_pointer++ = '\r';
          break;
        case 't':
          *output_pointer++ = '\t';
          break;
        case '\"':
        case '\\':
        case '/':
          *output_pointer++ = input_pointer[1];
          break;

        /* UTF-16 literal */
        case 'u':
          sequence_length =
              utf16_literal_to_utf8(input_pointer, input_end, &output_pointer);
          if (sequence_length == 0) {
            /* failed to convert UTF16-literal to UTF-8 */
            goto fail;
          }
          break;

        default:
          goto fail;
      }
      input_pointer += sequence_length;
    }
  }

  /* zero terminate the output */
  *output_pointer = '\0';

  item->type = cJSON_String;
  item->valuestring = (char *)output;

  input_buffer->offset = (size_t)(input_end - input_buffer->content);
  input_buffer->offset++;

  return true;

fail:
  if (output != NULL) {
    input_buffer->hooks.deallocate(output);
  }

  if (input_pointer != NULL) {
    input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
  }

  return false;
}

/* Render the cstring provided to an escaped version that can be printed. */
static cJSON_bool print_string_ptr(const unsigned char *const input,
                                   printbuffer *const output_buffer) {
  const unsigned char *input_pointer = NULL;
  unsigned char *output = NULL;
  unsigned char *output_pointer = NULL;
  size_t output_length = 0;
  /* numbers of additional characters needed for escaping */
  size_t escape_characters = 0;

  if (output_buffer == NULL) {
    return false;
  }

  /* empty string */
  if (input == NULL) {
    output = ensure(output_buffer, sizeof("\"\""));
    if (output == NULL) {
      return false;
    }
    strcpy((char *)output, "\"\"");

    return true;
  }

  /* set "flag" to 1 if something needs to be escaped */
  for (input_pointer = input; *input_pointer; input_pointer++) {
    switch (*input_pointer) {
      case '\"':
      case '\\':
      case '\b':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
        /* one character escape sequence */
        escape_characters++;
        break;
      default:
        if (*input_pointer < 32) {
          /* UTF-16 escape sequence uXXXX */
          escape_characters += 5;
        }
        break;
    }
  }
  output_length = (size_t)(input_pointer - input) + escape_characters;

  output = ensure(output_buffer, output_length + sizeof("\"\""));
  if (output == NULL) {
    return false;
  }

  /* no characters have to be escaped */
  if (escape_characters == 0) {
    output[0] = '\"';
    memcpy(output + 1, input, output_length);
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
  }

  output[0] = '\"';
  output_pointer = output + 1;
  /* copy the string */
  for (input_pointer = input; *input_pointer != '\0';
       (void)input_pointer++, output_pointer++) {
    if ((*input_pointer > 31) && (*input_pointer != '\"') &&
        (*input_pointer != '\\')) {
      /* normal character, copy */
      *output_pointer = *input_pointer;
    } else {
      /* character needs to be escaped */
      *output_pointer++ = '\\';
      switch (*input_pointer) {
        case '\\':
          *output_pointer = '\\';
          break;
        case '\"':
          *output_pointer = '\"';
          break;
        case '\b':
          *output_pointer = 'b';
          break;
        case '\f':
          *output_pointer = 'f';
          break;
        case '\n':
          *output_pointer = 'n';
          break;
        case '\r':
          *output_pointer = 'r';
          break;
        case '\t':
          *output_pointer = 't';
          break;
        default:
          /* escape and print as unicode codepoint */
          sprintf((char *)output_pointer, "u%04x", *input_pointer);
          output_pointer += 4;
          break;
      }
    }
  }
  output[output_length + 1] = '\"';
  output[output_length + 2] = '\0';

  return true;
}

/* Invoke print_string_ptr (which is useful) on an item. */
static cJSON_bool print_string(const cJSON *const item, printbuffer *const p) {
  return print_string_ptr((unsigned char *)item->valuestring, p);
}

/* Predeclare these prototypes. */
static cJSON_bool parse_value(cJSON *const item,
                              parse_buffer *const input_buffer);
static cJSON_bool print_value(const cJSON *const item,
                              printbuffer *const output_buffer);
static cJSON_bool parse_array(cJSON *const item,
                              parse_buffer *const input_buffer);
static cJSON_bool print_array(const cJSON *const item,
                              printbuffer *const output_buffer);
static cJSON_bool parse_object(cJSON *const item,
                               parse_buffer *const input_buffer);
static cJSON_bool print_object(const cJSON *const item,
                               printbuffer *const output_buffer);

/* Utility to jump whitespace and cr/lf */
static parse_buffer *buffer_skip_whitespace(parse_buffer *const buffer) {
  if ((buffer == NULL) || (buffer->content == NULL)) {
    return NULL;
  }

  while (can_access_at_index(buffer, 0) &&
         (buffer_at_offset(buffer)[0] <= 32)) {
    buffer->offset++;
  }

  if (buffer->offset == buffer->length) {
    buffer->offset--;
  }

  return buffer;
}

/* skip the UTF-8 BOM (byte order mark) if it is at the beginning of a buffer */
static parse_buffer *skip_utf8_bom(parse_buffer *const buffer) {
  if ((buffer == NULL) || (buffer->content == NULL) || (buffer->offset != 0)) {
    return NULL;
  }

  if (can_access_at_index(buffer, 4) &&
      (strncmp((const char *)buffer_at_offset(buffer), "\xEF\xBB\xBF", 3) ==
       0)) {
    buffer->offset += 3;
  }

  return buffer;
}

/* Parse an object - create a new root, and populate. */
CJSON_PUBLIC(cJSON *)
cJSON_ParseWithOpts(const char *value, const char **return_parse_end,
                    cJSON_bool require_null_terminated) {
  parse_buffer buffer = {0, 0, 0, 0, {0, 0, 0}};
  cJSON *item = NULL;

  /* reset error position */
  global_error.json = NULL;
  global_error.position = 0;

  if (value == NULL) {
    goto fail;
  }

  buffer.content = (const unsigned char *)value;
  buffer.length = strlen((const char *)value) + sizeof("");
  buffer.offset = 0;
  buffer.hooks = global_hooks;

  item = cJSON_New_Item(&global_hooks);
  if (item == NULL) /* memory fail */
  {
    goto fail;
  }

  if (!parse_value(item, buffer_skip_whitespace(skip_utf8_bom(&buffer)))) {
    /* parse failure. ep is set. */
    goto fail;
  }

  /* if we require null-terminated JSON without appended garbage, skip and then
   * check for a null terminator */
  if (require_null_terminated) {
    buffer_skip_whitespace(&buffer);
    if ((buffer.offset >= buffer.length) ||
        buffer_at_offset(&buffer)[0] != '\0') {
      goto fail;
    }
  }
  if (return_parse_end) {
    *return_parse_end = (const char *)buffer_at_offset(&buffer);
  }

  return item;

fail:
  if (item != NULL) {
    cJSON_Delete(item);
  }

  if (value != NULL) {
    error local_error;
    local_error.json = (const unsigned char *)value;
    local_error.position = 0;

    if (buffer.offset < buffer.length) {
      local_error.position = buffer.offset;
    } else if (buffer.length > 0) {
      local_error.position = buffer.length - 1;
    }

    if (return_parse_end != NULL) {
      *return_parse_end = (const char *)local_error.json + local_error.position;
    }

    global_error = local_error;
  }

  return NULL;
}

/* Default options for cJSON_Parse */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value) {
  return cJSON_ParseWithOpts(value, 0, 0);
}

#define cjson_min(a, b) ((a < b) ? a : b)

static unsigned char *print(const cJSON *const item, cJSON_bool format,
                            const internal_hooks *const hooks) {
  static const size_t default_buffer_size = 256;
  printbuffer buffer[1];
  unsigned char *printed = NULL;

  memset(buffer, 0, sizeof(buffer));

  /* create buffer */
  buffer->buffer = (unsigned char *)hooks->allocate(default_buffer_size);
  buffer->length = default_buffer_size;
  buffer->format = format;
  buffer->hooks = *hooks;
  if (buffer->buffer == NULL) {
    goto fail;
  }

  /* print the value */
  if (!print_value(item, buffer)) {
    goto fail;
  }
  update_offset(buffer);

  /* check if reallocate is available */
  if (hooks->reallocate != NULL) {
    printed =
        (unsigned char *)hooks->reallocate(buffer->buffer, buffer->offset + 1);
    if (printed == NULL) {
      goto fail;
    }
    buffer->buffer = NULL;
  } else /* otherwise copy the JSON over to a new buffer */
  {
    printed = (unsigned char *)hooks->allocate(buffer->offset + 1);
    if (printed == NULL) {
      goto fail;
    }
    memcpy(printed, buffer->buffer,
           cjson_min(buffer->length, buffer->offset + 1));
    printed[buffer->offset] = '\0'; /* just to be sure */

    /* free the buffer */
    hooks->deallocate(buffer->buffer);
  }

  return printed;

fail:
  if (buffer->buffer != NULL) {
    hooks->deallocate(buffer->buffer);
  }

  if (printed != NULL) {
    hooks->deallocate(printed);
  }

  return NULL;
}

/* Render a cJSON item/entity/structure to text. */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item) {
  return (char *)print(item, true, &global_hooks);
}

CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item) {
  return (char *)print(item, false, &global_hooks);
}

CJSON_PUBLIC(char *)
cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt) {
  printbuffer p = {0, 0, 0, 0, 0, 0, {0, 0, 0}};

  if (prebuffer < 0) {
    return NULL;
  }

  p.buffer = (unsigned char *)global_hooks.allocate((size_t)prebuffer);
  if (!p.buffer) {
    return NULL;
  }

  p.length = (size_t)prebuffer;
  p.offset = 0;
  p.noalloc = false;
  p.format = fmt;
  p.hooks = global_hooks;

  if (!print_value(item, &p)) {
    global_hooks.deallocate(p.buffer);
    return NULL;
  }

  return (char *)p.buffer;
}

CJSON_PUBLIC(cJSON_bool)
cJSON_PrintPreallocated(cJSON *item, char *buf, const int len,
                        const cJSON_bool fmt) {
  printbuffer p = {0, 0, 0, 0, 0, 0, {0, 0, 0}};

  if ((len < 0) || (buf == NULL)) {
    return false;
  }

  p.buffer = (unsigned char *)buf;
  p.length = (size_t)len;
  p.offset = 0;
  p.noalloc = true;
  p.format = fmt;
  p.hooks = global_hooks;

  return print_value(item, &p);
}

/* Parser core - when encountering text, process appropriately. */
static cJSON_bool parse_value(cJSON *const item,
                              parse_buffer *const input_buffer) {
  if ((input_buffer == NULL) || (input_buffer->content == NULL)) {
    return false; /* no input */
  }

  /* parse the different types of values */
  /* null */
  if (can_read(input_buffer, 4) &&
      (strncmp((const char *)buffer_at_offset(input_buffer), "null", 4) == 0)) {
    item->type = cJSON_NULL;
    input_buffer->offset += 4;
    return true;
  }
  /* false */
  if (can_read(input_buffer, 5) &&
      (strncmp((const char *)buffer_at_offset(input_buffer), "false", 5) ==
       0)) {
    item->type = cJSON_False;
    input_buffer->offset += 5;
    return true;
  }
  /* true */
  if (can_read(input_buffer, 4) &&
      (strncmp((const char *)buffer_at_offset(input_buffer), "true", 4) == 0)) {
    item->type = cJSON_True;
    item->valueint = 1;
    input_buffer->offset += 4;
    return true;
  }
  /* string */
  if (can_access_at_index(input_buffer, 0) &&
      (buffer_at_offset(input_buffer)[0] == '\"')) {
    return parse_string(item, input_buffer);
  }
  /* number */
  if (can_access_at_index(input_buffer, 0) &&
      ((buffer_at_offset(input_buffer)[0] == '-') ||
       ((buffer_at_offset(input_buffer)[0] >= '0') &&
        (buffer_at_offset(input_buffer)[0] <= '9')))) {
    return parse_number(item, input_buffer);
  }
  /* array */
  if (can_access_at_index(input_buffer, 0) &&
      (buffer_at_offset(input_buffer)[0] == '[')) {
    return parse_array(item, input_buffer);
  }
  /* object */
  if (can_access_at_index(input_buffer, 0) &&
      (buffer_at_offset(input_buffer)[0] == '{')) {
    return parse_object(item, input_buffer);
  }

  return false;
}

/* Render a value to text. */
static cJSON_bool print_value(const cJSON *const item,
                              printbuffer *const output_buffer) {
  unsigned char *output = NULL;

  if ((item == NULL) || (output_buffer == NULL)) {
    return false;
  }

  switch ((item->type) & 0xFF) {
    case cJSON_NULL:
      output = ensure(output_buffer, 5);
      if (output == NULL) {
        return false;
      }
      strcpy((char *)output, "null");
      return true;

    case cJSON_False:
      output = ensure(output_buffer, 6);
      if (output == NULL) {
        return false;
      }
      strcpy((char *)output, "false");
      return true;

    case cJSON_True:
      output = ensure(output_buffer, 5);
      if (output == NULL) {
        return false;
      }
      strcpy((char *)output, "true");
      return true;

    case cJSON_Number:
      return print_number(item, output_buffer);

    case cJSON_Raw: {
      size_t raw_length = 0;
      if (item->valuestring == NULL) {
        return false;
      }

      raw_length = strlen(item->valuestring) + sizeof("");
      output = ensure(output_buffer, raw_length);
      if (output == NULL) {
        return false;
      }
      memcpy(output, item->valuestring, raw_length);
      return true;
    }

    case cJSON_String:
      return print_string(item, output_buffer);

    case cJSON_Array:
      return print_array(item, output_buffer);

    case cJSON_Object:
      return print_object(item, output_buffer);

    default:
      return false;
  }
}

/* Build an array from input text. */
static cJSON_bool parse_array(cJSON *const item,
                              parse_buffer *const input_buffer) {
  cJSON *head = NULL; /* head of the linked list */
  cJSON *current_item = NULL;

  if (input_buffer->depth >= CJSON_NESTING_LIMIT) {
    return false; /* to deeply nested */
  }
  input_buffer->depth++;

  if (buffer_at_offset(input_buffer)[0] != '[') {
    /* not an array */
    goto fail;
  }

  input_buffer->offset++;
  buffer_skip_whitespace(input_buffer);
  if (can_access_at_index(input_buffer, 0) &&
      (buffer_at_offset(input_buffer)[0] == ']')) {
    /* empty array */
    goto success;
  }

  /* check if we skipped to the end of the buffer */
  if (cannot_access_at_index(input_buffer, 0)) {
    input_buffer->offset--;
    goto fail;
  }

  /* step back to character in front of the first element */
  input_buffer->offset--;
  /* loop through the comma separated array elements */
  do {
    /* allocate next item */
    cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
    if (new_item == NULL) {
      goto fail; /* allocation failure */
    }

    /* attach next item to list */
    if (head == NULL) {
      /* start the linked list */
      current_item = head = new_item;
    } else {
      /* add to the end and advance */
      current_item->next = new_item;
      new_item->prev = current_item;
      current_item = new_item;
    }

    /* parse next value */
    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (!parse_value(current_item, input_buffer)) {
      goto fail; /* failed to parse value */
    }
    buffer_skip_whitespace(input_buffer);
  } while (can_access_at_index(input_buffer, 0) &&
           (buffer_at_offset(input_buffer)[0] == ','));

  if (cannot_access_at_index(input_buffer, 0) ||
      buffer_at_offset(input_buffer)[0] != ']') {
    goto fail; /* expected end of array */
  }

success:
  input_buffer->depth--;

  item->type = cJSON_Array;
  item->child = head;

  input_buffer->offset++;

  return true;

fail:
  if (head != NULL) {
    cJSON_Delete(head);
  }

  return false;
}

/* Render an array to text */
static cJSON_bool print_array(const cJSON *const item,
                              printbuffer *const output_buffer) {
  unsigned char *output_pointer = NULL;
  size_t length = 0;
  cJSON *current_element = item->child;

  if (output_buffer == NULL) {
    return false;
  }

  /* Compose the output array. */
  /* opening square bracket */
  output_pointer = ensure(output_buffer, 1);
  if (output_pointer == NULL) {
    return false;
  }

  *output_pointer = '[';
  output_buffer->offset++;
  output_buffer->depth++;

  while (current_element != NULL) {
    if (!print_value(current_element, output_buffer)) {
      return false;
    }
    update_offset(output_buffer);
    if (current_element->next) {
      length = (size_t)(output_buffer->format ? 2 : 1);
      output_pointer = ensure(output_buffer, length + 1);
      if (output_pointer == NULL) {
        return false;
      }
      *output_pointer++ = ',';
      if (output_buffer->format) {
        *output_pointer++ = ' ';
      }
      *output_pointer = '\0';
      output_buffer->offset += length;
    }
    current_element = current_element->next;
  }

  output_pointer = ensure(output_buffer, 2);
  if (output_pointer == NULL) {
    return false;
  }
  *output_pointer++ = ']';
  *output_pointer = '\0';
  output_buffer->depth--;

  return true;
}

/* Build an object from the text. */
static cJSON_bool parse_object(cJSON *const item,
                               parse_buffer *const input_buffer) {
  cJSON *head = NULL; /* linked list head */
  cJSON *current_item = NULL;

  if (input_buffer->depth >= CJSON_NESTING_LIMIT) {
    return false; /* to deeply nested */
  }
  input_buffer->depth++;

  if (cannot_access_at_index(input_buffer, 0) ||
      (buffer_at_offset(input_buffer)[0] != '{')) {
    goto fail; /* not an object */
  }

  input_buffer->offset++;
  buffer_skip_whitespace(input_buffer);
  if (can_access_at_index(input_buffer, 0) &&
      (buffer_at_offset(input_buffer)[0] == '}')) {
    goto success; /* empty object */
  }

  /* check if we skipped to the end of the buffer */
  if (cannot_access_at_index(input_buffer, 0)) {
    input_buffer->offset--;
    goto fail;
  }

  /* step back to character in front of the first element */
  input_buffer->offset--;
  /* loop through the comma separated array elements */
  do {
    /* allocate next item */
    cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
    if (new_item == NULL) {
      goto fail; /* allocation failure */
    }

    /* attach next item to list */
    if (head == NULL) {
      /* start the linked list */
      current_item = head = new_item;
    } else {
      /* add to the end and advance */
      current_item->next = new_item;
      new_item->prev = current_item;
      current_item = new_item;
    }

    /* parse the name of the child */
    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (!parse_string(current_item, input_buffer)) {
      goto fail; /* faile to parse name */
    }
    buffer_skip_whitespace(input_buffer);

    /* swap valuestring and string, because we parsed the name */
    current_item->string = current_item->valuestring;
    current_item->valuestring = NULL;

    if (cannot_access_at_index(input_buffer, 0) ||
        (buffer_at_offset(input_buffer)[0] != ':')) {
      goto fail; /* invalid object */
    }

    /* parse the value */
    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (!parse_value(current_item, input_buffer)) {
      goto fail; /* failed to parse value */
    }
    buffer_skip_whitespace(input_buffer);
  } while (can_access_at_index(input_buffer, 0) &&
           (buffer_at_offset(input_buffer)[0] == ','));

  if (cannot_access_at_index(input_buffer, 0) ||
      (buffer_at_offset(input_buffer)[0] != '}')) {
    goto fail; /* expected end of object */
  }

success:
  input_buffer->depth--;

  item->type = cJSON_Object;
  item->child = head;

  input_buffer->offset++;
  return true;

fail:
  if (head != NULL) {
    cJSON_Delete(head);
  }

  return false;
}

/* Render an object to text. */
static cJSON_bool print_object(const cJSON *const item,
                               printbuffer *const output_buffer) {
  unsigned char *output_pointer = NULL;
  size_t length = 0;
  cJSON *current_item = item->child;

  if (output_buffer == NULL) {
    return false;
  }

  /* Compose the output: */
  length = (size_t)(output_buffer->format ? 2 : 1); /* fmt: {\n */
  output_pointer = ensure(output_buffer, length + 1);
  if (output_pointer == NULL) {
    return false;
  }

  *output_pointer++ = '{';
  output_buffer->depth++;
  if (output_buffer->format) {
    *output_pointer++ = '\n';
  }
  output_buffer->offset += length;

  while (current_item) {
    if (output_buffer->format) {
      size_t i;
      output_pointer = ensure(output_buffer, output_buffer->depth);
      if (output_pointer == NULL) {
        return false;
      }
      for (i = 0; i < output_buffer->depth; i++) {
        *output_pointer++ = '\t';
      }
      output_buffer->offset += output_buffer->depth;
    }

    /* print key */
    if (!print_string_ptr((unsigned char *)current_item->string,
                          output_buffer)) {
      return false;
    }
    update_offset(output_buffer);

    length = (size_t)(output_buffer->format ? 2 : 1);
    output_pointer = ensure(output_buffer, length);
    if (output_pointer == NULL) {
      return false;
    }
    *output_pointer++ = ':';
    if (output_buffer->format) {
      *output_pointer++ = '\t';
    }
    output_buffer->offset += length;

    /* print value */
    if (!print_value(current_item, output_buffer)) {
      return false;
    }
    update_offset(output_buffer);

    /* print comma if not last */
    length = (size_t)((output_buffer->format ? 1 : 0) +
                      (current_item->next ? 1 : 0));
    output_pointer = ensure(output_buffer, length + 1);
    if (output_pointer == NULL) {
      return false;
    }
    if (current_item->next) {
      *output_pointer++ = ',';
    }

    if (output_buffer->format) {
      *output_pointer++ = '\n';
    }
    *output_pointer = '\0';
    output_buffer->offset += length;

    current_item = current_item->next;
  }

  output_pointer = ensure(
      output_buffer, output_buffer->format ? (output_buffer->depth + 1) : 2);
  if (output_pointer == NULL) {
    return false;
  }
  if (output_buffer->format) {
    size_t i;
    for (i = 0; i < (output_buffer->depth - 1); i++) {
      *output_pointer++ = '\t';
    }
  }
  *output_pointer++ = '}';
  *output_pointer = '\0';
  output_buffer->depth--;

  return true;
}

/* Get Array size/item / object item. */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array) {
  cJSON *child = NULL;
  size_t size = 0;

  if (array == NULL) {
    return 0;
  }

  child = array->child;

  while (child != NULL) {
    size++;
    child = child->next;
  }

  /* FIXME: Can overflow here. Cannot be fixed without breaking the API */

  return (int)size;
}

static cJSON *get_array_item(const cJSON *array, size_t index) {
  cJSON *current_child = NULL;

  if (array == NULL) {
    return NULL;
  }

  current_child = array->child;
  while ((current_child != NULL) && (index > 0)) {
    index--;
    current_child = current_child->next;
  }

  return current_child;
}

CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index) {
  if (index < 0) {
    return NULL;
  }

  return get_array_item(array, (size_t)index);
}

static cJSON *get_object_item(const cJSON *const object, const char *const name,
                              const cJSON_bool case_sensitive) {
  cJSON *current_element = NULL;

  if ((object == NULL) || (name == NULL)) {
    return NULL;
  }

  current_element = object->child;
  if (case_sensitive) {
    while ((current_element != NULL) &&
           (strcmp(name, current_element->string) != 0)) {
      current_element = current_element->next;
    }
  } else {
    while ((current_element != NULL) &&
           (case_insensitive_strcmp(
                (const unsigned char *)name,
                (const unsigned char *)(current_element->string)) != 0)) {
      current_element = current_element->next;
    }
  }

  return current_element;
}

CJSON_PUBLIC(cJSON *)
cJSON_GetObjectItem(const cJSON *const object, const char *const string) {
  return get_object_item(object, string, false);
}

CJSON_PUBLIC(cJSON *)
cJSON_GetObjectItemCaseSensitive(const cJSON *const object,
                                 const char *const string) {
  return get_object_item(object, string, true);
}

CJSON_PUBLIC(cJSON_bool)
cJSON_HasObjectItem(const cJSON *object, const char *string) {
  return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* Utility for array list handling. */
static void suffix_object(cJSON *prev, cJSON *item) {
  prev->next = item;
  item->prev = prev;
}

/* Utility for handling references. */
static cJSON *create_reference(const cJSON *item,
                               const internal_hooks *const hooks) {
  cJSON *reference = NULL;
  if (item == NULL) {
    return NULL;
  }

  reference = cJSON_New_Item(hooks);
  if (reference == NULL) {
    return NULL;
  }

  memcpy(reference, item, sizeof(cJSON));
  reference->string = NULL;
  reference->type |= cJSON_IsReference;
  reference->next = reference->prev = NULL;
  return reference;
}

static cJSON_bool add_item_to_array(cJSON *array, cJSON *item) {
  cJSON *child = NULL;

  if ((item == NULL) || (array == NULL)) {
    return false;
  }

  child = array->child;

  if (child == NULL) {
    /* list is empty, start new one */
    array->child = item;
  } else {
    /* append to the end */
    while (child->next) {
      child = child->next;
    }
    suffix_object(child, item);
  }

  return true;
}

/* Add item to array/object. */
CJSON_PUBLIC(void) cJSON_AddItemToArray(cJSON *array, cJSON *item) {
  add_item_to_array(array, item);
}

#if defined(__clang__) || \
    (defined(__GNUC__) && \
     ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
#pragma GCC diagnostic push
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/* helper function to cast away const */
static void *cast_away_const(const void *string) { return (void *)string; }
#if defined(__clang__) || \
    (defined(__GNUC__) && \
     ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
#pragma GCC diagnostic pop
#endif

static cJSON_bool add_item_to_object(cJSON *const object,
                                     const char *const string,
                                     cJSON *const item,
                                     const internal_hooks *const hooks,
                                     const cJSON_bool constant_key) {
  char *new_key = NULL;
  int new_type = cJSON_Invalid;

  if ((object == NULL) || (string == NULL) || (item == NULL)) {
    return false;
  }

  if (constant_key) {
    new_key = (char *)cast_away_const(string);
    new_type = item->type | cJSON_StringIsConst;
  } else {
    new_key = (char *)cJSON_strdup((const unsigned char *)string, hooks);
    if (new_key == NULL) {
      return false;
    }

    new_type = item->type & ~cJSON_StringIsConst;
  }

  if (!(item->type & cJSON_StringIsConst) && (item->string != NULL)) {
    hooks->deallocate(item->string);
  }

  item->string = new_key;
  item->type = new_type;

  return add_item_to_array(object, item);
}

CJSON_PUBLIC(void)
cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item) {
  add_item_to_object(object, string, item, &global_hooks, false);
}

/* Add an item to an object with constant string as key */
CJSON_PUBLIC(void)
cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item) {
  add_item_to_object(object, string, item, &global_hooks, true);
}

CJSON_PUBLIC(void) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item) {
  if (array == NULL) {
    return;
  }

  add_item_to_array(array, create_reference(item, &global_hooks));
}

CJSON_PUBLIC(void)
cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item) {
  if ((object == NULL) || (string == NULL)) {
    return;
  }

  add_item_to_object(object, string, create_reference(item, &global_hooks),
                     &global_hooks, false);
}

CJSON_PUBLIC(cJSON *)
cJSON_AddNullToObject(cJSON *const object, const char *const name) {
  cJSON *null = cJSON_CreateNull();
  if (add_item_to_object(object, name, null, &global_hooks, false)) {
    return null;
  }

  cJSON_Delete(null);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddTrueToObject(cJSON *const object, const char *const name) {
  cJSON *true_item = cJSON_CreateTrue();
  if (add_item_to_object(object, name, true_item, &global_hooks, false)) {
    return true_item;
  }

  cJSON_Delete(true_item);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddFalseToObject(cJSON *const object, const char *const name) {
  cJSON *false_item = cJSON_CreateFalse();
  if (add_item_to_object(object, name, false_item, &global_hooks, false)) {
    return false_item;
  }

  cJSON_Delete(false_item);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddBoolToObject(cJSON *const object, const char *const name,
                      const cJSON_bool boolean) {
  cJSON *bool_item = cJSON_CreateBool(boolean);
  if (add_item_to_object(object, name, bool_item, &global_hooks, false)) {
    return bool_item;
  }

  cJSON_Delete(bool_item);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddNumberToObject(cJSON *const object, const char *const name,
                        const double number) {
  cJSON *number_item = cJSON_CreateNumber(number);
  if (add_item_to_object(object, name, number_item, &global_hooks, false)) {
    return number_item;
  }

  cJSON_Delete(number_item);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddStringToObject(cJSON *const object, const char *const name,
                        const char *const string) {
  cJSON *string_item = cJSON_CreateString(string);
  if (add_item_to_object(object, name, string_item, &global_hooks, false)) {
    return string_item;
  }

  cJSON_Delete(string_item);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddRawToObject(cJSON *const object, const char *const name,
                     const char *const raw) {
  cJSON *raw_item = cJSON_CreateRaw(raw);
  if (add_item_to_object(object, name, raw_item, &global_hooks, false)) {
    return raw_item;
  }

  cJSON_Delete(raw_item);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddObjectToObject(cJSON *const object, const char *const name) {
  cJSON *object_item = cJSON_CreateObject();
  if (add_item_to_object(object, name, object_item, &global_hooks, false)) {
    return object_item;
  }

  cJSON_Delete(object_item);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddArrayToObject(cJSON *const object, const char *const name) {
  cJSON *array = cJSON_CreateArray();
  if (add_item_to_object(object, name, array, &global_hooks, false)) {
    return array;
  }

  cJSON_Delete(array);
  return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_DetachItemViaPointer(cJSON *parent, cJSON *const item) {
  if ((parent == NULL) || (item == NULL)) {
    return NULL;
  }

  if (item->prev != NULL) {
    /* not the first element */
    item->prev->next = item->next;
  }
  if (item->next != NULL) {
    /* not the last element */
    item->next->prev = item->prev;
  }

  if (item == parent->child) {
    /* first element */
    parent->child = item->next;
  }
  /* make sure the detached item doesn't point anywhere anymore */
  item->prev = NULL;
  item->next = NULL;

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which) {
  if (which < 0) {
    return NULL;
  }

  return cJSON_DetachItemViaPointer(array,
                                    get_array_item(array, (size_t)which));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which) {
  cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

CJSON_PUBLIC(cJSON *)
cJSON_DetachItemFromObject(cJSON *object, const char *string) {
  cJSON *to_detach = cJSON_GetObjectItem(object, string);

  return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(cJSON *)
cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string) {
  cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);

  return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(void)
cJSON_DeleteItemFromObject(cJSON *object, const char *string) {
  cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

CJSON_PUBLIC(void)
cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string) {
  cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/* Replace array/object items with new ones. */
CJSON_PUBLIC(void)
cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem) {
  cJSON *after_inserted = NULL;

  if (which < 0) {
    return;
  }

  after_inserted = get_array_item(array, (size_t)which);
  if (after_inserted == NULL) {
    add_item_to_array(array, newitem);
    return;
  }

  newitem->next = after_inserted;
  newitem->prev = after_inserted->prev;
  after_inserted->prev = newitem;
  if (after_inserted == array->child) {
    array->child = newitem;
  } else {
    newitem->prev->next = newitem;
  }
}

CJSON_PUBLIC(cJSON_bool)
cJSON_ReplaceItemViaPointer(cJSON *const parent, cJSON *const item,
                            cJSON *replacement) {
  if ((parent == NULL) || (replacement == NULL) || (item == NULL)) {
    return false;
  }

  if (replacement == item) {
    return true;
  }

  replacement->next = item->next;
  replacement->prev = item->prev;

  if (replacement->next != NULL) {
    replacement->next->prev = replacement;
  }
  if (replacement->prev != NULL) {
    replacement->prev->next = replacement;
  }
  if (parent->child == item) {
    parent->child = replacement;
  }

  item->next = NULL;
  item->prev = NULL;
  cJSON_Delete(item);

  return true;
}

CJSON_PUBLIC(void)
cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem) {
  if (which < 0) {
    return;
  }

  cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which),
                              newitem);
}

static cJSON_bool replace_item_in_object(cJSON *object, const char *string,
                                         cJSON *replacement,
                                         cJSON_bool case_sensitive) {
  if ((replacement == NULL) || (string == NULL)) {
    return false;
  }

  /* replace the name in the replacement */
  if (!(replacement->type & cJSON_StringIsConst) &&
      (replacement->string != NULL)) {
    cJSON_free(replacement->string);
  }
  replacement->string =
      (char *)cJSON_strdup((const unsigned char *)string, &global_hooks);
  replacement->type &= ~cJSON_StringIsConst;

  cJSON_ReplaceItemViaPointer(
      object, get_object_item(object, string, case_sensitive), replacement);

  return true;
}

CJSON_PUBLIC(void)
cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem) {
  replace_item_in_object(object, string, newitem, false);
}

CJSON_PUBLIC(void)
cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string,
                                       cJSON *newitem) {
  replace_item_in_object(object, string, newitem, true);
}

/* Create basic types: */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_NULL;
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_True;
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_False;
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool b) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = b ? cJSON_True : cJSON_False;
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_Number;
    item->valuedouble = num;

    /* use saturation in case of overflow */
    if (num >= INT_MAX) {
      item->valueint = INT_MAX;
    } else if (num <= INT_MIN) {
      item->valueint = INT_MIN;
    } else {
      item->valueint = (int)num;
    }
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_String;
    item->valuestring =
        (char *)cJSON_strdup((const unsigned char *)string, &global_hooks);
    if (!item->valuestring) {
      cJSON_Delete(item);
      return NULL;
    }
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item != NULL) {
    item->type = cJSON_String | cJSON_IsReference;
    item->valuestring = (char *)cast_away_const(string);
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item != NULL) {
    item->type = cJSON_Object | cJSON_IsReference;
    item->child = (cJSON *)cast_away_const(child);
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item != NULL) {
    item->type = cJSON_Array | cJSON_IsReference;
    item->child = (cJSON *)cast_away_const(child);
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_Raw;
    item->valuestring =
        (char *)cJSON_strdup((const unsigned char *)raw, &global_hooks);
    if (!item->valuestring) {
      cJSON_Delete(item);
      return NULL;
    }
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_Array;
  }

  return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void) {
  cJSON *item = cJSON_New_Item(&global_hooks);
  if (item) {
    item->type = cJSON_Object;
  }

  return item;
}

/* Create Arrays: */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count) {
  size_t i = 0;
  cJSON *n = NULL;
  cJSON *p = NULL;
  cJSON *a = NULL;

  if ((count < 0) || (numbers == NULL)) {
    return NULL;
  }

  a = cJSON_CreateArray();
  for (i = 0; a && (i < (size_t)count); i++) {
    n = cJSON_CreateNumber(numbers[i]);
    if (!n) {
      cJSON_Delete(a);
      return NULL;
    }
    if (!i) {
      a->child = n;
    } else {
      suffix_object(p, n);
    }
    p = n;
  }

  return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count) {
  size_t i = 0;
  cJSON *n = NULL;
  cJSON *p = NULL;
  cJSON *a = NULL;

  if ((count < 0) || (numbers == NULL)) {
    return NULL;
  }

  a = cJSON_CreateArray();

  for (i = 0; a && (i < (size_t)count); i++) {
    n = cJSON_CreateNumber((double)numbers[i]);
    if (!n) {
      cJSON_Delete(a);
      return NULL;
    }
    if (!i) {
      a->child = n;
    } else {
      suffix_object(p, n);
    }
    p = n;
  }

  return a;
}

CJSON_PUBLIC(cJSON *)
cJSON_CreateDoubleArray(const double *numbers, int count) {
  size_t i = 0;
  cJSON *n = NULL;
  cJSON *p = NULL;
  cJSON *a = NULL;

  if ((count < 0) || (numbers == NULL)) {
    return NULL;
  }

  a = cJSON_CreateArray();

  for (i = 0; a && (i < (size_t)count); i++) {
    n = cJSON_CreateNumber(numbers[i]);
    if (!n) {
      cJSON_Delete(a);
      return NULL;
    }
    if (!i) {
      a->child = n;
    } else {
      suffix_object(p, n);
    }
    p = n;
  }

  return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char **strings, int count) {
  size_t i = 0;
  cJSON *n = NULL;
  cJSON *p = NULL;
  cJSON *a = NULL;

  if ((count < 0) || (strings == NULL)) {
    return NULL;
  }

  a = cJSON_CreateArray();

  for (i = 0; a && (i < (size_t)count); i++) {
    n = cJSON_CreateString(strings[i]);
    if (!n) {
      cJSON_Delete(a);
      return NULL;
    }
    if (!i) {
      a->child = n;
    } else {
      suffix_object(p, n);
    }
    p = n;
  }

  return a;
}

/* Duplication */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse) {
  cJSON *newitem = NULL;
  cJSON *child = NULL;
  cJSON *next = NULL;
  cJSON *newchild = NULL;

  /* Bail on bad ptr */
  if (!item) {
    goto fail;
  }
  /* Create new item */
  newitem = cJSON_New_Item(&global_hooks);
  if (!newitem) {
    goto fail;
  }
  /* Copy over all vars */
  newitem->type = item->type & (~cJSON_IsReference);
  newitem->valueint = item->valueint;
  newitem->valuedouble = item->valuedouble;
  if (item->valuestring) {
    newitem->valuestring =
        (char *)cJSON_strdup((unsigned char *)item->valuestring, &global_hooks);
    if (!newitem->valuestring) {
      goto fail;
    }
  }
  if (item->string) {
    newitem->string = (item->type & cJSON_StringIsConst)
                          ? item->string
                          : (char *)cJSON_strdup((unsigned char *)item->string,
                                                 &global_hooks);
    if (!newitem->string) {
      goto fail;
    }
  }
  /* If non-recursive, then we're done! */
  if (!recurse) {
    return newitem;
  }
  /* Walk the ->next chain for the child. */
  child = item->child;
  while (child != NULL) {
    newchild = cJSON_Duplicate(
        child,
        true); /* Duplicate (with recurse) each item in the ->next chain */
    if (!newchild) {
      goto fail;
    }
    if (next != NULL) {
      /* If newitem->child already set, then crosswire ->prev and ->next and
       * move on */
      next->next = newchild;
      newchild->prev = next;
      next = newchild;
    } else {
      /* Set newitem->child and move to it */
      newitem->child = newchild;
      next = newchild;
    }
    child = child->next;
  }

  return newitem;

fail:
  if (newitem != NULL) {
    cJSON_Delete(newitem);
  }

  return NULL;
}

CJSON_PUBLIC(void) cJSON_Minify(char *json) {
  unsigned char *into = (unsigned char *)json;

  if (json == NULL) {
    return;
  }

  while (*json) {
    if (*json == ' ') {
      json++;
    } else if (*json == '\t') {
      /* Whitespace characters. */
      json++;
    } else if (*json == '\r') {
      json++;
    } else if (*json == '\n') {
      json++;
    } else if ((*json == '/') && (json[1] == '/')) {
      /* double-slash comments, to end of line. */
      while (*json && (*json != '\n')) {
        json++;
      }
    } else if ((*json == '/') && (json[1] == '*')) {
      /* multiline comments. */
      while (*json && !((*json == '*') && (json[1] == '/'))) {
        json++;
      }
      json += 2;
    } else if (*json == '\"') {
      /* string literals, which are \" sensitive. */
      *into++ = (unsigned char)*json++;
      while (*json && (*json != '\"')) {
        if (*json == '\\') {
          *into++ = (unsigned char)*json++;
        }
        *into++ = (unsigned char)*json++;
      }
      *into++ = (unsigned char)*json++;
    } else {
      /* All other characters. */
      *into++ = (unsigned char)*json++;
    }
  }

  /* and null-terminate. */
  *into = '\0';
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_Invalid;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_False;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xff) == cJSON_True;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & (cJSON_True | cJSON_False)) != 0;
}
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_NULL;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_Number;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_String;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_Array;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_Object;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON *const item) {
  if (item == NULL) {
    return false;
  }

  return (item->type & 0xFF) == cJSON_Raw;
}

CJSON_PUBLIC(cJSON_bool)
cJSON_Compare(const cJSON *const a, const cJSON *const b,
              const cJSON_bool case_sensitive) {
  if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF)) ||
      cJSON_IsInvalid(a)) {
    return false;
  }

  /* check if type is valid */
  switch (a->type & 0xFF) {
    case cJSON_False:
    case cJSON_True:
    case cJSON_NULL:
    case cJSON_Number:
    case cJSON_String:
    case cJSON_Raw:
    case cJSON_Array:
    case cJSON_Object:
      break;

    default:
      return false;
  }

  /* identical objects are equal */
  if (a == b) {
    return true;
  }

  switch (a->type & 0xFF) {
    /* in these cases and equal type is enough */
    case cJSON_False:
    case cJSON_True:
    case cJSON_NULL:
      return true;

    case cJSON_Number:
      if (a->valuedouble == b->valuedouble) {
        return true;
      }
      return false;

    case cJSON_String:
    case cJSON_Raw:
      if ((a->valuestring == NULL) || (b->valuestring == NULL)) {
        return false;
      }
      if (strcmp(a->valuestring, b->valuestring) == 0) {
        return true;
      }

      return false;

    case cJSON_Array: {
      cJSON *a_element = a->child;
      cJSON *b_element = b->child;

      for (; (a_element != NULL) && (b_element != NULL);) {
        if (!cJSON_Compare(a_element, b_element, case_sensitive)) {
          return false;
        }

        a_element = a_element->next;
        b_element = b_element->next;
      }

      /* one of the arrays is longer than the other */
      if (a_element != b_element) {
        return false;
      }

      return true;
    }

    case cJSON_Object: {
      cJSON *a_element = NULL;
      cJSON *b_element = NULL;
      cJSON_ArrayForEach(a_element, a) {
        /* TODO This has O(n^2) runtime, which is horrible! */
        b_element = get_object_item(b, a_element->string, case_sensitive);
        if (b_element == NULL) {
          return false;
        }

        if (!cJSON_Compare(a_element, b_element, case_sensitive)) {
          return false;
        }
      }

      /* doing this twice, once on a and b to prevent true comparison if a
       * subset of b
       * TODO: Do this the proper way, this is just a fix for now */
      cJSON_ArrayForEach(b_element, b) {
        a_element = get_object_item(a, b_element->string, case_sensitive);
        if (a_element == NULL) {
          return false;
        }

        if (!cJSON_Compare(b_element, a_element, case_sensitive)) {
          return false;
        }
      }

      return true;
    }

    default:
      return false;
  }
}

CJSON_PUBLIC(void *) cJSON_malloc(size_t size) {
  return global_hooks.allocate(size);
}

CJSON_PUBLIC(void) cJSON_free(void *object) { global_hooks.deallocate(object); }