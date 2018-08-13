
/*
 * Copyright (C) Roman Arutyunyan
 */

#include "ngx_rtmp_notify_module.h"
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_md5.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_netcall_module.h"
#include "ngx_rtmp_record_module.h"
#include "ngx_rtmp_relay_module.h"

static ngx_rtmp_connect_pt next_connect;
static ngx_rtmp_disconnect_pt next_disconnect;
static ngx_rtmp_publish_pt next_publish;
static ngx_rtmp_play_pt next_play;
static ngx_rtmp_close_stream_pt next_close_stream;
static ngx_rtmp_record_done_pt next_record_done;

static char *ngx_rtmp_notify_on_srv_event(ngx_conf_t *cf, ngx_command_t *cmd,
                                          void *conf);
static char *ngx_rtmp_notify_on_app_event(ngx_conf_t *cf, ngx_command_t *cmd,
                                          void *conf);
static char *ngx_rtmp_notify_method(ngx_conf_t *cf, ngx_command_t *cmd,
                                    void *conf);
static ngx_int_t ngx_rtmp_notify_postconfiguration(ngx_conf_t *cf);
static void *ngx_rtmp_notify_create_app_conf(ngx_conf_t *cf);
static char *ngx_rtmp_notify_merge_app_conf(ngx_conf_t *cf, void *parent,
                                            void *child);
static void *ngx_rtmp_notify_create_srv_conf(ngx_conf_t *cf);
static char *ngx_rtmp_notify_merge_srv_conf(ngx_conf_t *cf, void *parent,
                                            void *child);
static ngx_int_t ngx_rtmp_notify_done(ngx_rtmp_session_t *s, char *cbname,
                                      ngx_uint_t url_idx);
static void ngx_rtmp_notify_reconnect_evt_handler(ngx_event_t *rec_evt);
static ngx_int_t ngx_rtmp_notify_copy_str(ngx_pool_t *pool, ngx_str_t *dst,
                                          ngx_str_t *src);

ngx_str_t ngx_rtmp_notify_urlencoded =
    ngx_string("application/x-www-form-urlencoded");

#define NGX_RTMP_NOTIFY_PUBLISHING 0x01
#define NGX_RTMP_NOTIFY_PLAYING 0x02

typedef struct {
  u_char *cbname;
  ngx_uint_t url_idx;
} ngx_rtmp_notify_done_t;

static ngx_command_t ngx_rtmp_notify_commands[] = {

    {ngx_string("on_connect"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_srv_event, NGX_RTMP_SRV_CONF_OFFSET, 0, NULL},

    {ngx_string("on_disconnect"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_srv_event, NGX_RTMP_SRV_CONF_OFFSET, 0, NULL},

    {ngx_string("on_publish"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_app_event, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("on_play"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_app_event, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("on_publish_done"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_app_event, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("on_play_done"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_app_event, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("on_done"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_app_event, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("on_record_done"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_RTMP_REC_CONF | NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_app_event, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("on_update"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_rtmp_notify_on_app_event, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("notify_method"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_rtmp_notify_method, NGX_RTMP_APP_CONF_OFFSET, 0, NULL},

    {ngx_string("notify_update_timeout"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_msec_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_notify_app_conf_t, update_timeout), NULL},

    {ngx_string("notify_update_strict"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_flag_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_notify_app_conf_t, update_strict), NULL},

    {ngx_string("notify_relay_redirect"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_flag_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_notify_app_conf_t, relay_redirect), NULL},

    {ngx_string("reconnect_time_gap"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_msec_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_notify_app_conf_t, reconnect_timegap), NULL},

    {ngx_string("reconnect_timeout"),
     NGX_RTMP_MAIN_CONF | NGX_RTMP_SRV_CONF | NGX_RTMP_APP_CONF |
         NGX_CONF_TAKE1,
     ngx_conf_set_msec_slot, NGX_RTMP_APP_CONF_OFFSET,
     offsetof(ngx_rtmp_notify_app_conf_t, reconnect_timeout), NULL},

    ngx_null_command};

static ngx_rtmp_module_t ngx_rtmp_notify_module_ctx = {
    NULL,                              /* preconfiguration */
    ngx_rtmp_notify_postconfiguration, /* postconfiguration */
    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */
    ngx_rtmp_notify_create_srv_conf,   /* create server configuration */
    ngx_rtmp_notify_merge_srv_conf,    /* merge server configuration */
    ngx_rtmp_notify_create_app_conf,   /* create app configuration */
    ngx_rtmp_notify_merge_app_conf     /* merge app configuration */
};

ngx_module_t ngx_rtmp_notify_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_notify_module_ctx, /* module context */
    ngx_rtmp_notify_commands,    /* module directives */
    NGX_RTMP_MODULE,             /* module type */
    NULL,                        /* init master */
    NULL,                        /* init module */
    NULL,                        /* init process */
    NULL,                        /* init thread */
    NULL,                        /* exit thread */
    NULL,                        /* exit process */
    NULL,                        /* exit master */
    NGX_MODULE_V1_PADDING};

static void *ngx_rtmp_notify_create_app_conf(ngx_conf_t *cf) {
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_uint_t n;

  nacf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_notify_app_conf_t));
  if (nacf == NULL) {
    return NULL;
  }

  for (n = 0; n < NGX_RTMP_NOTIFY_APP_MAX; ++n) {
    nacf->url[n] = NGX_CONF_UNSET_PTR;
  }

  nacf->method = NGX_CONF_UNSET_UINT;
  nacf->update_timeout = NGX_CONF_UNSET_MSEC;
  nacf->reconnect_timegap = NGX_CONF_UNSET_MSEC;
  nacf->reconnect_timeout = NGX_CONF_UNSET_MSEC;
  nacf->update_strict = NGX_CONF_UNSET;
  nacf->relay_redirect = NGX_CONF_UNSET;

  return nacf;
}

static char *ngx_rtmp_notify_merge_app_conf(ngx_conf_t *cf, void *parent,
                                            void *child) {
  ngx_rtmp_notify_app_conf_t *prev = parent;
  ngx_rtmp_notify_app_conf_t *conf = child;
  ngx_uint_t n;

  for (n = 0; n < NGX_RTMP_NOTIFY_APP_MAX; ++n) {
    ngx_conf_merge_ptr_value(conf->url[n], prev->url[n], NULL);
    if (conf->url[n]) {
      conf->active = 1;
    }
  }

  if (conf->active) {
    prev->active = 1;
  }

  ngx_conf_merge_uint_value(conf->method, prev->method,
                            NGX_RTMP_NETCALL_HTTP_POST);
  ngx_conf_merge_msec_value(conf->update_timeout, prev->update_timeout, 30000);
  ngx_conf_merge_msec_value(conf->reconnect_timegap, prev->reconnect_timegap,
                            3000);  // 3s
  ngx_conf_merge_msec_value(conf->reconnect_timeout, prev->reconnect_timeout,
                            300000);  // 5m
  ngx_conf_merge_value(conf->update_strict, prev->update_strict, 0);
  ngx_conf_merge_value(conf->relay_redirect, prev->relay_redirect, 0);
  conf->loop_times = conf->reconnect_timeout / conf->reconnect_timegap;

  return NGX_CONF_OK;
}

static void *ngx_rtmp_notify_create_srv_conf(ngx_conf_t *cf) {
  ngx_rtmp_notify_srv_conf_t *nscf;
  ngx_uint_t n;

  nscf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_notify_srv_conf_t));
  if (nscf == NULL) {
    return NULL;
  }

  for (n = 0; n < NGX_RTMP_NOTIFY_SRV_MAX; ++n) {
    nscf->url[n] = NGX_CONF_UNSET_PTR;
  }

  nscf->method = NGX_CONF_UNSET_UINT;

  return nscf;
}

static char *ngx_rtmp_notify_merge_srv_conf(ngx_conf_t *cf, void *parent,
                                            void *child) {
  ngx_rtmp_notify_srv_conf_t *prev = parent;
  ngx_rtmp_notify_srv_conf_t *conf = child;
  ngx_uint_t n;

  for (n = 0; n < NGX_RTMP_NOTIFY_SRV_MAX; ++n) {
    ngx_conf_merge_ptr_value(conf->url[n], prev->url[n], NULL);
  }

  ngx_conf_merge_uint_value(conf->method, prev->method,
                            NGX_RTMP_NETCALL_HTTP_POST);

  return NGX_CONF_OK;
}

static ngx_int_t ngx_rtmp_notify_copy_str(ngx_pool_t *pool, ngx_str_t *dst,
                                          ngx_str_t *src) {
  if (src->len == 0) {
    return NGX_OK;
  }
  dst->len = src->len;
  dst->data = ngx_palloc(pool, src->len);
  if (dst->data == NULL) {
    return NGX_ERROR;
  }
  ngx_memcpy(dst->data, src->data, src->len);
  return NGX_OK;
}

static ngx_chain_t *ngx_rtmp_notify_create_request(ngx_rtmp_session_t *s,
                                                   ngx_pool_t *pool,
                                                   ngx_uint_t url_idx,
                                                   ngx_chain_t *args) {
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_chain_t *al, *bl, *cl;
  ngx_url_t *url;

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

  url = nacf->url[url_idx];

  al = ngx_rtmp_netcall_http_format_session(s, pool);
  if (al == NULL) {
    return NULL;
  }

  al->next = args;

  bl = NULL;

  if (nacf->method == NGX_RTMP_NETCALL_HTTP_POST) {
    cl = al;
    al = bl;
    bl = cl;
  }

  return ngx_rtmp_netcall_http_format_request(nacf->method, &url->host,
                                              &url->uri, al, bl, pool,
                                              &ngx_rtmp_notify_urlencoded);
}

static ngx_chain_t *ngx_rtmp_notify_connect_create(ngx_rtmp_session_t *s,
                                                   void *arg,
                                                   ngx_pool_t *pool) {
  ngx_rtmp_connect_t *v = arg;

  ngx_rtmp_notify_srv_conf_t *nscf;
  ngx_url_t *url;
  ngx_chain_t *al, *bl;
  ngx_buf_t *b;
  ngx_str_t *addr_text;
  size_t app_len, args_len, flashver_len, swf_url_len, tc_url_len, page_url_len;

  nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

  al = ngx_alloc_chain_link(pool);
  if (al == NULL) {
    return NULL;
  }

  /* these values are still missing in session
   * so we have to construct the request from
   * connection struct */

  app_len = ngx_strlen(v->app);
  args_len = ngx_strlen(v->args);
  flashver_len = ngx_strlen(v->flashver);
  swf_url_len = ngx_strlen(v->swf_url);
  tc_url_len = ngx_strlen(v->tc_url);
  page_url_len = ngx_strlen(v->page_url);

  addr_text = &s->connection->addr_text;

  b = ngx_create_temp_buf(
      pool, sizeof("call=connect") - 1 + sizeof("&app=") - 1 + app_len * 3 +
                sizeof("&flashver=") - 1 + flashver_len * 3 +
                sizeof("&swfurl=") - 1 + swf_url_len * 3 + sizeof("&tcurl=") -
                1 + tc_url_len * 3 + sizeof("&pageurl=") - 1 +
                page_url_len * 3 + sizeof("&addr=") - 1 + addr_text->len * 3 +
                sizeof("&epoch=") - 1 + NGX_INT32_LEN + 1 + args_len);

  if (b == NULL) {
    return NULL;
  }

  al->buf = b;
  al->next = NULL;

  b->last = ngx_cpymem(b->last, (u_char *)"app=", sizeof("app=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, v->app, app_len, NGX_ESCAPE_ARGS);

  b->last =
      ngx_cpymem(b->last, (u_char *)"&flashver=", sizeof("&flashver=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, v->flashver, flashver_len,
                                     NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&swfurl=", sizeof("&swfurl=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, v->swf_url, swf_url_len,
                                     NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&tcurl=", sizeof("&tcurl=") - 1);
  b->last =
      (u_char *)ngx_escape_uri(b->last, v->tc_url, tc_url_len, NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&pageurl=", sizeof("&pageurl=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, v->page_url, page_url_len,
                                     NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&addr=", sizeof("&addr=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, addr_text->data, addr_text->len,
                                     NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&epoch=", sizeof("&epoch=") - 1);
  b->last = ngx_sprintf(b->last, "%uD", (uint32_t)s->epoch);

  b->last = ngx_cpymem(b->last, (u_char *)"&call=connect",
                       sizeof("&call=connect") - 1);

  if (args_len) {
    *b->last++ = '&';
    b->last = (u_char *)ngx_cpymem(b->last, v->args, args_len);
  }

  url = nscf->url[NGX_RTMP_NOTIFY_CONNECT];

  bl = NULL;

  if (nscf->method == NGX_RTMP_NETCALL_HTTP_POST) {
    bl = al;
    al = NULL;
  }

  return ngx_rtmp_netcall_http_format_request(nscf->method, &url->host,
                                              &url->uri, al, bl, pool,
                                              &ngx_rtmp_notify_urlencoded);
}

static ngx_chain_t *ngx_rtmp_notify_disconnect_create(ngx_rtmp_session_t *s,
                                                      void *arg,
                                                      ngx_pool_t *pool) {
  ngx_rtmp_notify_srv_conf_t *nscf;
  ngx_url_t *url;
  ngx_chain_t *al, *bl, *pl;
  ngx_buf_t *b;

  nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

  pl = ngx_alloc_chain_link(pool);
  if (pl == NULL) {
    return NULL;
  }

  b = ngx_create_temp_buf(pool, sizeof("&call=disconnect") + sizeof("&app=") +
                                    s->app.len * 3 + 1 + s->args.len);
  if (b == NULL) {
    return NULL;
  }

  pl->buf = b;
  pl->next = NULL;

  b->last = ngx_cpymem(b->last, (u_char *)"&call=disconnect",
                       sizeof("&call=disconnect") - 1);

  b->last = ngx_cpymem(b->last, (u_char *)"&app=", sizeof("&app=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, s->app.data, s->app.len,
                                     NGX_ESCAPE_ARGS);

  if (s->args.len) {
    *b->last++ = '&';
    b->last = (u_char *)ngx_cpymem(b->last, s->args.data, s->args.len);
  }

  url = nscf->url[NGX_RTMP_NOTIFY_DISCONNECT];

  al = ngx_rtmp_netcall_http_format_session(s, pool);
  if (al == NULL) {
    return NULL;
  }

  al->next = pl;

  bl = NULL;

  if (nscf->method == NGX_RTMP_NETCALL_HTTP_POST) {
    bl = al;
    al = NULL;
  }

  return ngx_rtmp_netcall_http_format_request(nscf->method, &url->host,
                                              &url->uri, al, bl, pool,
                                              &ngx_rtmp_notify_urlencoded);
}

static ngx_chain_t *ngx_rtmp_notify_publish_create(ngx_rtmp_session_t *s,
                                                   void *arg,
                                                   ngx_pool_t *pool) {
  ngx_rtmp_publish_t *v = arg;

  ngx_chain_t *pl;
  ngx_buf_t *b;
  size_t name_len, type_len, args_len;

  pl = ngx_alloc_chain_link(pool);
  if (pl == NULL) {
    return NULL;
  }

  name_len = ngx_strlen(v->name);
  type_len = ngx_strlen(v->type);
  args_len = ngx_strlen(v->args);

  b = ngx_create_temp_buf(pool, sizeof("&call=publish") + sizeof("&name=") +
                                    name_len * 3 + sizeof("&type=") +
                                    type_len * 3 + 1 + args_len);
  if (b == NULL) {
    return NULL;
  }

  pl->buf = b;
  pl->next = NULL;

  b->last = ngx_cpymem(b->last, (u_char *)"&call=publish",
                       sizeof("&call=publish") - 1);

  b->last = ngx_cpymem(b->last, (u_char *)"&name=", sizeof("&name=") - 1);
  b->last =
      (u_char *)ngx_escape_uri(b->last, v->name, name_len, NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&type=", sizeof("&type=") - 1);
  b->last =
      (u_char *)ngx_escape_uri(b->last, v->type, type_len, NGX_ESCAPE_ARGS);

  if (args_len) {
    *b->last++ = '&';
    b->last = (u_char *)ngx_cpymem(b->last, v->args, args_len);
  }

  return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_PUBLISH, pl);
}

static ngx_chain_t *ngx_rtmp_notify_play_create(ngx_rtmp_session_t *s,
                                                void *arg, ngx_pool_t *pool) {
  ngx_rtmp_play_t *v = arg;

  ngx_chain_t *pl;
  ngx_buf_t *b;
  size_t name_len, args_len;

  pl = ngx_alloc_chain_link(pool);
  if (pl == NULL) {
    return NULL;
  }

  name_len = ngx_strlen(v->name);
  args_len = ngx_strlen(v->args);

  b = ngx_create_temp_buf(pool, sizeof("&call=play") + sizeof("&name=") +
                                    name_len * 3 +
                                    sizeof("&start=&duration=&reset=") +
                                    NGX_INT32_LEN * 3 + 1 + args_len);
  if (b == NULL) {
    return NULL;
  }

  pl->buf = b;
  pl->next = NULL;

  b->last =
      ngx_cpymem(b->last, (u_char *)"&call=play", sizeof("&call=play") - 1);

  b->last = ngx_cpymem(b->last, (u_char *)"&name=", sizeof("&name=") - 1);
  b->last =
      (u_char *)ngx_escape_uri(b->last, v->name, name_len, NGX_ESCAPE_ARGS);

  b->last = ngx_snprintf(b->last, b->end - b->last,
                         "&start=%uD&duration=%uD&reset=%d", (uint32_t)v->start,
                         (uint32_t)v->duration, v->reset & 1);

  if (args_len) {
    *b->last++ = '&';
    b->last = (u_char *)ngx_cpymem(b->last, v->args, args_len);
  }

  return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_PLAY, pl);
}

static ngx_chain_t *ngx_rtmp_notify_done_create(ngx_rtmp_session_t *s,
                                                void *arg, ngx_pool_t *pool) {
  ngx_rtmp_notify_done_t *ds = arg;

  ngx_chain_t *pl;
  ngx_buf_t *b;
  size_t cbname_len, name_len, args_len;
  ngx_rtmp_notify_ctx_t *ctx;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  pl = ngx_alloc_chain_link(pool);
  if (pl == NULL) {
    return NULL;
  }

  cbname_len = ngx_strlen(ds->cbname);
  name_len = ctx ? ngx_strlen(ctx->name) : 0;
  args_len = ctx ? ngx_strlen(ctx->args) : 0;

  b = ngx_create_temp_buf(pool, sizeof("&call=") + cbname_len +
                                    sizeof("&name=") + name_len * 3 + 1 +
                                    args_len);
  if (b == NULL) {
    return NULL;
  }

  pl->buf = b;
  pl->next = NULL;

  b->last = ngx_cpymem(b->last, (u_char *)"&call=", sizeof("&call=") - 1);
  b->last = ngx_cpymem(b->last, ds->cbname, cbname_len);

  if (name_len) {
    b->last = ngx_cpymem(b->last, (u_char *)"&name=", sizeof("&name=") - 1);
    b->last =
        (u_char *)ngx_escape_uri(b->last, ctx->name, name_len, NGX_ESCAPE_ARGS);
  }

  if (args_len) {
    *b->last++ = '&';
    b->last = (u_char *)ngx_cpymem(b->last, ctx->args, args_len);
  }

  return ngx_rtmp_notify_create_request(s, pool, ds->url_idx, pl);
}

static ngx_chain_t *ngx_rtmp_notify_update_create(ngx_rtmp_session_t *s,
                                                  void *arg, ngx_pool_t *pool) {
  ngx_chain_t *pl;
  ngx_buf_t *b;
  size_t name_len, args_len;
  ngx_rtmp_notify_ctx_t *ctx;
  ngx_str_t sfx;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  pl = ngx_alloc_chain_link(pool);
  if (pl == NULL) {
    return NULL;
  }

  if (ctx->flags & NGX_RTMP_NOTIFY_PUBLISHING) {
    ngx_str_set(&sfx, "_publish");
  } else if (ctx->flags & NGX_RTMP_NOTIFY_PLAYING) {
    ngx_str_set(&sfx, "_play");
  } else {
    ngx_str_null(&sfx);
  }

  name_len = ctx ? ngx_strlen(ctx->name) : 0;
  args_len = ctx ? ngx_strlen(ctx->args) : 0;

  b = ngx_create_temp_buf(
      pool, sizeof("&call=update") + sfx.len + sizeof("&time=") +
                NGX_TIME_T_LEN + sizeof("&timestamp=") + NGX_INT32_LEN +
                sizeof("&name=") + name_len * 3 + 1 + args_len);
  if (b == NULL) {
    return NULL;
  }

  pl->buf = b;
  pl->next = NULL;

  b->last =
      ngx_cpymem(b->last, (u_char *)"&call=update", sizeof("&call=update") - 1);
  b->last = ngx_cpymem(b->last, sfx.data, sfx.len);

  b->last = ngx_cpymem(b->last, (u_char *)"&time=", sizeof("&time=") - 1);
  b->last = ngx_sprintf(b->last, "%T", ngx_cached_time->sec - ctx->start);

  b->last =
      ngx_cpymem(b->last, (u_char *)"&timestamp=", sizeof("&timestamp=") - 1);
  b->last = ngx_sprintf(b->last, "%D", s->current_time);

  if (name_len) {
    b->last = ngx_cpymem(b->last, (u_char *)"&name=", sizeof("&name=") - 1);
    b->last =
        (u_char *)ngx_escape_uri(b->last, ctx->name, name_len, NGX_ESCAPE_ARGS);
  }

  if (args_len) {
    *b->last++ = '&';
    b->last = (u_char *)ngx_cpymem(b->last, ctx->args, args_len);
  }

  return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_UPDATE, pl);
}

static ngx_chain_t *ngx_rtmp_notify_record_done_create(ngx_rtmp_session_t *s,
                                                       void *arg,
                                                       ngx_pool_t *pool) {
  ngx_rtmp_record_done_t *v = arg;

  ngx_rtmp_notify_ctx_t *ctx;
  ngx_chain_t *pl;
  ngx_buf_t *b;
  size_t name_len, args_len;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  pl = ngx_alloc_chain_link(pool);
  if (pl == NULL) {
    return NULL;
  }

  name_len = ngx_strlen(ctx->name);
  args_len = ngx_strlen(ctx->args);

  b = ngx_create_temp_buf(
      pool, sizeof("&call=record_done") + sizeof("&recorder=") +
                v->recorder.len + sizeof("&name=") + name_len * 3 +
                sizeof("&path=") + v->path.len * 3 + 1 + args_len);
  if (b == NULL) {
    return NULL;
  }

  pl->buf = b;
  pl->next = NULL;

  b->last = ngx_cpymem(b->last, (u_char *)"&call=record_done",
                       sizeof("&call=record_done") - 1);

  b->last =
      ngx_cpymem(b->last, (u_char *)"&recorder=", sizeof("&recorder=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, v->recorder.data, v->recorder.len,
                                     NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&name=", sizeof("&name=") - 1);
  b->last =
      (u_char *)ngx_escape_uri(b->last, ctx->name, name_len, NGX_ESCAPE_ARGS);

  b->last = ngx_cpymem(b->last, (u_char *)"&path=", sizeof("&path=") - 1);
  b->last = (u_char *)ngx_escape_uri(b->last, v->path.data, v->path.len,
                                     NGX_ESCAPE_ARGS);

  if (args_len) {
    *b->last++ = '&';
    b->last = (u_char *)ngx_cpymem(b->last, ctx->args, args_len);
  }

  return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_RECORD_DONE,
                                        pl);
}

static ngx_int_t ngx_rtmp_notify_parse_http_retcode(ngx_rtmp_session_t *s,
                                                    ngx_chain_t *in) {
  ngx_buf_t *b;
  ngx_int_t n;
  u_char c;

  /* find 10th character */

  n = 9;
  while (in) {
    b = in->buf;
    if (b->last - b->pos > n) {
      c = b->pos[n];
      if (c >= (u_char)'0' && c <= (u_char)'9') {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "notify: HTTP retcode: %dxx", (int)(c - '0'));
        switch (c) {
          case (u_char)'2':
            return NGX_OK;
          case (u_char)'3':
            return NGX_AGAIN;
          default:
            return NGX_ERROR;
        }
      }

      ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                    "notify: invalid HTTP retcode: %d..", (int)c);

      return NGX_ERROR;
    }
    n -= (b->last - b->pos);
    in = in->next;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                "notify: empty or broken HTTP response");

  /*
   * not enough data;
   * it can happen in case of empty or broken reply
   */

  return NGX_ERROR;
}

static ngx_int_t ngx_rtmp_notify_parse_http_header(ngx_rtmp_session_t *s,
                                                   ngx_chain_t *in,
                                                   ngx_str_t *name,
                                                   u_char *data, size_t len) {
  ngx_buf_t *b;
  ngx_int_t matched;
  u_char *p, c;
  ngx_uint_t n;

  enum {
    parse_name,
    parse_space,
    parse_value,
    parse_value_newline
  } state = parse_name;

  n = 0;
  matched = 0;

  while (in) {
    b = in->buf;

    for (p = b->pos; p != b->last; ++p) {
      c = *p;

      if (c == '\r') {
        continue;
      }

      switch (state) {
        case parse_value_newline:
          if (c == ' ' || c == '\t') {
            state = parse_space;
            break;
          }

          if (matched) {
            return n;
          }

          if (c == '\n') {
            return NGX_OK;
          }

          n = 0;
          state = parse_name;

          /* fall through */

        case parse_name:
          switch (c) {
            case ':':
              matched = (n == name->len);
              n = 0;
              state = parse_space;
              break;
            case '\n':
              n = 0;
              break;
            default:
              if (n < name->len &&
                  ngx_tolower(c) == ngx_tolower(name->data[n])) {
                ++n;
                break;
              }
              n = name->len + 1;
          }
          break;

        case parse_space:
          if (c == ' ' || c == '\t') {
            break;
          }
          state = parse_value;

          /* fall through */

        case parse_value:
          if (c == '\n') {
            state = parse_value_newline;
            break;
          }

          if (matched && n + 1 < len) {
            data[n++] = c;
          }

          break;
      }
    }

    in = in->next;
  }

  return NGX_OK;
}

static void ngx_rtmp_notify_clear_flag(ngx_rtmp_session_t *s, ngx_uint_t flag) {
  ngx_rtmp_notify_ctx_t *ctx;

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  ctx->flags &= ~flag;
}

static ngx_int_t ngx_rtmp_notify_connect_handle(ngx_rtmp_session_t *s,
                                                void *arg, ngx_chain_t *in) {
  ngx_rtmp_connect_t *v = arg;
  ngx_int_t rc;
  u_char app[NGX_RTMP_MAX_NAME];

  static ngx_str_t location = ngx_string("location");

  rc = ngx_rtmp_notify_parse_http_retcode(s, in);
  if (rc == NGX_ERROR) {
    return NGX_ERROR;
  }

  if (rc == NGX_AGAIN) {
    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "notify: connect redirect received");

    rc = ngx_rtmp_notify_parse_http_header(s, in, &location, app,
                                           sizeof(app) - 1);
    if (rc > 0) {
      *ngx_cpymem(v->app, app, rc) = 0;
      ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                    "notify: connect redirect to '%s'", v->app);
    }
  }

  return next_connect(s, v);
}

static void ngx_rtmp_notify_set_name(u_char *dst, size_t dst_len, u_char *src,
                                     size_t src_len) {
  u_char result[16], *p;
  ngx_md5_t md5;

  ngx_md5_init(&md5);
  ngx_md5_update(&md5, src, src_len);
  ngx_md5_final(result, &md5);

  p = ngx_hex_dump(dst, result, ngx_min((dst_len - 1) / 2, 16));
  *p = '\0';
}

static ngx_int_t ngx_rtmp_notify_publish_handle(ngx_rtmp_session_t *s,
                                                void *arg, ngx_chain_t *in) {
  ngx_rtmp_publish_t *v = arg;
  ngx_int_t rc;
  ngx_str_t local_name;
  ngx_rtmp_relay_target_t target;
  ngx_url_t *u;
  ngx_rtmp_notify_app_conf_t *nacf;
  u_char name[NGX_RTMP_MAX_NAME];

  static ngx_str_t location = ngx_string("location");

  rc = ngx_rtmp_notify_parse_http_retcode(s, in);
  if (rc == NGX_ERROR) {
    ngx_rtmp_notify_clear_flag(s, NGX_RTMP_NOTIFY_PUBLISHING);
    return NGX_ERROR;
  }

  if (rc != NGX_AGAIN) {
    goto next;
  }

  /* HTTP 3xx */

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "notify: publish redirect received");

  rc = ngx_rtmp_notify_parse_http_header(s, in, &location, name,
                                         sizeof(name) - 1);
  if (rc <= 0) {
    goto next;
  }

  if (ngx_strncasecmp(name, (u_char *)"rtmp://", 7)) {
    *ngx_cpymem(v->name, name, rc) = 0;
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: publish redirect to '%s'", v->name);
    goto next;
  }

  /* push */

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
  if (nacf->relay_redirect) {
    ngx_rtmp_notify_set_name(v->name, NGX_RTMP_MAX_NAME, name, (size_t)rc);
  }

  ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                "notify: push '%s' to '%*s'", v->name, rc, name);

  local_name.data = v->name;
  local_name.len = ngx_strlen(v->name);

  ngx_memzero(&target, sizeof(target));

  u = &target.url;
  u->url = local_name;
  u->url.data = name + 7;
  u->url.len = rc - 7;
  u->default_port = 1935;
  u->uri_part = 1;
  u->no_resolve = 1; /* want ip here */

  if (ngx_parse_url(s->connection->pool, u) != NGX_OK) {
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: push failed '%V'", &local_name);
    return NGX_ERROR;
  }

  ngx_rtmp_relay_push(s, &local_name, &target);

next:

  return next_publish(s, v);
}

static ngx_int_t ngx_rtmp_notify_parse_http_json_body(ngx_rtmp_session_t *s,
                                                      ngx_chain_t *in) {
  ngx_uint_t i;
  json_char *json;
  ngx_chain_t *cur;
  ngx_array_t *body;
  ngx_buf_t *buffer;
  u_char *pos, c, *temp, has_body = 0;
  json_value *value, *temp_ret;
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_rtmp_notify_ctx_t *ctx;
  ngx_str_t json_state = ngx_string("state"),
            target_name[3] = {ngx_string("content"), ngx_string("addrs"),
                              ngx_string("addr")};

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  if (ctx == NULL) return NGX_ERROR;

  enum { in_header, next_header } state = in_header;

  body =
      ngx_array_create(s->connection->pool, NGX_RTMP_MAX_NAME, sizeof(u_char));
  if (body == NULL) {
    return NGX_ERROR;
  }

  cur = in;
  while (cur != NULL) {
    // skip header
    buffer = cur->buf;
    for (pos = buffer->pos; pos != buffer->last; ++pos) {
      c = *pos;
      if (has_body) {
        temp = ngx_array_push(body);
        *temp = c;
      } else if (c == '\r')
        continue;
      else if (state == next_header && c == '\n') {
        has_body = 1;
      } else if (c == '\n')
        state = next_header;
      else
        state = in_header;
    }
    cur = cur->next;
  }

  if (has_body == 0) {
    ngx_array_destroy(body);
    return NGX_DECLINED;
  }

  json = body->elts;

  value = json_parse(json, body->nelts / body->size);

  if (value != NULL &&
      (temp_ret = ngx_find_json_name(value, &json_state)) != NULL &&
      temp_ret->type == json_integer && temp_ret->u.integer == 0) {
    temp_ret = value;
    for (i = 0; i < 2; ++i) {
      temp_ret = ngx_find_json_name(temp_ret, &target_name[i]);
      if (temp_ret == NULL) break;
    }
    if (i != 2) {
      goto error;
    }
    ctx->addrs_buffer =
        ngx_array_create(s->connection->pool, 1, sizeof(ngx_str_t));
    if (ctx->addrs_buffer == NULL) {
      goto error;
    }

    ngx_match_json_name(temp_ret, &target_name[2], s->connection->pool,
                        ctx->addrs_buffer);
    if (ctx->addrs_buffer->nelts <= 0) {
      ngx_array_destroy(ctx->addrs_buffer);
      ctx->addrs_buffer = NULL;
      goto error;
    }
  }
  if (value != NULL) json_value_free(value);
  ngx_array_destroy(body);
  return NGX_OK;

error:
  ngx_array_destroy(body);
  if (value != NULL) json_value_free(value);
  return NGX_ERROR;
}

static ngx_int_t ngx_rtmp_notify_play_handle(ngx_rtmp_session_t *s, void *arg,
                                             ngx_chain_t *in) {
  ngx_rtmp_play_t *v = arg;
  ngx_int_t rc, length = -1;
  ngx_str_t *temp_addr, temp;
  ngx_rtmp_notify_ctx_t *ctx;
  ngx_rtmp_notify_app_conf_t *nacf;
  u_char name[NGX_RTMP_MAX_NAME];

  static ngx_str_t location = ngx_string("location");

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  rc = ngx_rtmp_notify_parse_http_retcode(s, in);

  if (rc == NGX_ERROR) {
    ngx_rtmp_notify_clear_flag(s, NGX_RTMP_NOTIFY_PLAYING);
    return NGX_ERROR;
  }

  ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "notify: play redirect received");
  /* HTTP 3xx */
  if (rc == NGX_AGAIN) {
    ctx->addrs_buffer =
        ngx_array_create(s->connection->pool, 1, sizeof(ngx_str_t));

    if (ctx->addrs_buffer == NULL) return next_play(s, v);

    length = ngx_rtmp_notify_parse_http_header(s, in, &location, name,
                                               sizeof(name) - 1);
  } else {
    /* HTTP 2xx rc==-2==NGX_OK*/
    if (ngx_rtmp_notify_parse_http_json_body(s, in) != NGX_OK) {
      return next_play(s, v);
    }
  }

  if (rc == NGX_AGAIN) {
    temp_addr = ngx_array_push(ctx->addrs_buffer);
    temp_addr->len = length;
    temp_addr->data = ngx_pcalloc(s->connection->pool, length);
    ngx_memcpy(temp_addr->data, name, length);
  } else {
    length = ctx->addrs_buffer->nelts;
  }

  if (length <= 0) {
    return next_play(s, v);
  }

  ctx->v_play = ngx_palloc(s->connection->pool, sizeof(*v));
  ngx_memcpy(ctx->v_play, v, sizeof(*v));
  ctx->cnt = 0;
  ctx->reconnect_times = 0;
  temp.data = v->name;
  temp.len = (size_t)ngx_strlen(v->name);
  ctx->stream_name = ngx_palloc(s->connection->pool, sizeof(ngx_str_t));
  ngx_rtmp_notify_copy_str(s->connection->pool, ctx->stream_name, &temp);
  ///////////////////////////

  return ngx_rtmp_peer_connect_from_addrs(s);
}

static void ngx_rtmp_notify_reconnect_evt_handler(ngx_event_t *rec_evt) {
  ngx_connection_t *c = rec_evt->data;
  ngx_rtmp_session_t *s = c->data;
  ngx_rtmp_peer_connect_from_addrs(s);
}

ngx_int_t ngx_rtmp_peer_connect_from_addrs(ngx_rtmp_session_t *s) {
  u_char name[NGX_RTMP_MAX_NAME];
  ngx_uint_t length, addrs_len;
  ngx_str_t local_name, *cur_addr, *start_addr;
  ngx_rtmp_relay_target_t target;
  ngx_url_t *u;
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_rtmp_live_ctx_t *lctx;
  ngx_rtmp_notify_ctx_t *ctx;
  ngx_array_t *buffer;

  ngx_memzero(name, sizeof(name));

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);
  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

  if (ctx == NULL || ctx->addrs_buffer == NULL) return NGX_ERROR;

  buffer = ctx->addrs_buffer;
  addrs_len = buffer->nelts;

  if (ctx->cnt >= addrs_len) {
    goto reconnect;
  }

  start_addr = buffer->elts;

  for (; ctx->cnt < addrs_len; ++ctx->cnt) {
    cur_addr = start_addr + ctx->cnt;
    length = cur_addr->len;
    ngx_memcpy(name, cur_addr->data, length);
    if (ngx_strncasecmp(name, (u_char *)"rtmp://", 7) == 0) {
      break;
    }
  }

  if (ctx->cnt >= addrs_len) {
    goto reconnect;
  }

  ++ctx->cnt;
  lctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
  if (lctx != NULL) {
    lctx->stream = NULL;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                "notify: pull '%s' from '%*s'", ctx->v_play->name, length,
                name);

  ngx_memzero(&target, sizeof(target));

  u = &target.url;  // tar.url==u
  u->url = local_name;
  u->url.data = name + 7;
  u->url.len = length - 7;
  u->default_port = 1935;
  u->uri_part = 1;
  u->no_resolve = 1; /* want ip here */

  if (ngx_parse_url(s->connection->pool, u) != NGX_OK) {
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: pull failed '%V'", &local_name);
    return NGX_ERROR;
  }

  if (target.app.len == 0 || target.play_path.len == 0) {
    ngx_str_t *uri, temp;
    u_char *first, *last, *p;
    uri = &u->uri;
    first = uri->data;
    last = uri->data + uri->len;
    if (first != last && *first == '/') {
      ++first;
    }

    if (first != last) {
      p = ngx_strlchr(first, last, '/');
      if (p == NULL) {
        p = last;
      }
      if (target.app.len == 0 && first != p) {
        temp.data = first;
        temp.len = p - first;
        if (ngx_rtmp_notify_copy_str(s->connection->pool, &target.app, &temp) !=
            NGX_OK) {
          goto destory;
        }
      }

      if (p != last) {
        ++p;
        if (target.play_path.len == 0 && p != last) {
          temp.data = p;
          temp.len = last - p;
          if (ngx_rtmp_notify_copy_str(s->connection->pool, &target.play_path,
                                       &temp) != NGX_OK) {
            goto destory;
          }
        }
      } else {
        if (ngx_rtmp_notify_copy_str(s->connection->pool, &target.play_path,
                                     ctx->stream_name) != NGX_OK) {
          goto destory;
        }
        name[length] = '/';
        ngx_memcpy(name + length + 1, target.play_path.data,
                   target.play_path.len);
      }
    }
  }

  if (nacf->relay_redirect) {
    ngx_rtmp_notify_set_name(ctx->v_play->name, NGX_RTMP_MAX_NAME, name,
                             (size_t)length);
  }

  local_name.data = ctx->v_play->name;
  local_name.len = ngx_strlen(ctx->v_play->name);
  ngx_rtmp_relay_pull(s, &local_name, &target);

  return next_play(s, ctx->v_play);

reconnect:
  ++ctx->reconnect_times;
  if (ctx->reconnect_times >= nacf->loop_times) goto destory;
  ctx->cnt = 0;

  ctx->reconnect_evt.data = s->connection;
  ctx->reconnect_evt.log = s->connection->log;
  ctx->reconnect_evt.handler = ngx_rtmp_notify_reconnect_evt_handler;
  ngx_add_timer(&ctx->reconnect_evt, nacf->reconnect_timegap);

  return NGX_OK;

destory:
  ngx_array_destroy(buffer);
  ctx->addrs_buffer = NULL;
  ctx->v_play = NULL;
  ctx->cnt = 0;
  return NGX_ERROR;
}

static ngx_int_t ngx_rtmp_notify_update_handle(ngx_rtmp_session_t *s, void *arg,
                                               ngx_chain_t *in) {
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_rtmp_notify_ctx_t *ctx;
  ngx_int_t rc;

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

  rc = ngx_rtmp_notify_parse_http_retcode(s, in);

  if ((!nacf->update_strict && rc == NGX_ERROR) ||
      (nacf->update_strict && rc != NGX_OK)) {
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "notify: update failed");

    return NGX_ERROR;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "notify: schedule update %Mms", nacf->update_timeout);

  ngx_add_timer(&ctx->update_evt, nacf->update_timeout);

  return NGX_OK;
}

static void ngx_rtmp_notify_update(ngx_event_t *e) {
  ngx_connection_t *c;
  ngx_rtmp_session_t *s;
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_rtmp_netcall_init_t ci;
  ngx_url_t *url;

  c = e->data;
  s = c->data;

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

  url = nacf->url[NGX_RTMP_NOTIFY_UPDATE];

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "notify: update '%V'",
                &url->url);

  ngx_memzero(&ci, sizeof(ci));

  ci.url = url;
  ci.create = ngx_rtmp_notify_update_create;
  ci.handle = ngx_rtmp_notify_update_handle;

  if (ngx_rtmp_netcall_create(s, &ci) == NGX_OK) {
    return;
  }

  /* schedule next update on connection error */

  ngx_rtmp_notify_update_handle(s, NULL, NULL);
}

static void ngx_rtmp_notify_init(ngx_rtmp_session_t *s,
                                 u_char name[NGX_RTMP_MAX_NAME],
                                 u_char args[NGX_RTMP_MAX_ARGS],
                                 ngx_uint_t flags) {
  ngx_rtmp_notify_ctx_t *ctx;
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_event_t *e;

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
  if (!nacf->active) {
    return;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  if (ctx == NULL) {
    ctx = ngx_pcalloc(s->connection->pool, sizeof(ngx_rtmp_notify_ctx_t));
    if (ctx == NULL) {
      return;
    }

    ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_notify_module);
  }

  ngx_memcpy(ctx->name, name, NGX_RTMP_MAX_NAME);
  ngx_memcpy(ctx->args, args, NGX_RTMP_MAX_ARGS);

  ctx->flags |= flags;
  ctx->cnt = 0;
  ctx->addrs_buffer = NULL;
  ctx->v_play = NULL;

  if (nacf->url[NGX_RTMP_NOTIFY_UPDATE] == NULL || nacf->update_timeout == 0) {
    return;
  }

  if (ctx->update_evt.timer_set) {
    return;
  }

  ctx->start = ngx_cached_time->sec;

  e = &ctx->update_evt;

  e->data = s->connection;
  e->log = s->connection->log;
  e->handler = ngx_rtmp_notify_update;

  ngx_add_timer(e, nacf->update_timeout);

  ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                 "notify: schedule initial update %Mms", nacf->update_timeout);
}

static ngx_int_t ngx_rtmp_notify_connect(ngx_rtmp_session_t *s,
                                         ngx_rtmp_connect_t *v) {
  ngx_rtmp_notify_srv_conf_t *nscf;
  ngx_rtmp_netcall_init_t ci;
  ngx_url_t *url;

  if (s->auto_pushed || s->relay) {
    goto next;
  }

  nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

  url = nscf->url[NGX_RTMP_NOTIFY_CONNECT];
  if (url == NULL) {
    goto next;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "notify: connect '%V'",
                &url->url);

  ngx_memzero(&ci, sizeof(ci));

  ci.url = url;
  ci.create = ngx_rtmp_notify_connect_create;
  ci.handle = ngx_rtmp_notify_connect_handle;
  ci.arg = v;
  ci.argsize = sizeof(*v);

  return ngx_rtmp_netcall_create(s, &ci);

next:
  return next_connect(s, v);
}

static ngx_int_t ngx_rtmp_notify_disconnect(ngx_rtmp_session_t *s) {
  ngx_rtmp_notify_srv_conf_t *nscf;
  ngx_rtmp_netcall_init_t ci;
  ngx_url_t *url;

  if (s->auto_pushed || s->relay) {
    goto next;
  }

  nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

  url = nscf->url[NGX_RTMP_NOTIFY_DISCONNECT];
  if (url == NULL) {
    goto next;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "notify: disconnect '%V'",
                &url->url);

  ngx_memzero(&ci, sizeof(ci));

  ci.url = url;
  ci.create = ngx_rtmp_notify_disconnect_create;

  ngx_rtmp_netcall_create(s, &ci);

next:
  return next_disconnect(s);
}

static ngx_int_t ngx_rtmp_notify_publish(ngx_rtmp_session_t *s,
                                         ngx_rtmp_publish_t *v) {
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_rtmp_netcall_init_t ci;
  ngx_url_t *url;

  if (s->auto_pushed) {
    goto next;
  }

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
  if (nacf == NULL) {
    goto next;
  }

  url = nacf->url[NGX_RTMP_NOTIFY_PUBLISH];

  ngx_rtmp_notify_init(s, v->name, v->args, NGX_RTMP_NOTIFY_PUBLISHING);

  if (url == NULL) {
    goto next;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "notify: publish '%V'",
                &url->url);

  ngx_memzero(&ci, sizeof(ci));

  ci.url = url;
  ci.create = ngx_rtmp_notify_publish_create;
  ci.handle = ngx_rtmp_notify_publish_handle;
  ci.arg = v;
  ci.argsize = sizeof(*v);

  return ngx_rtmp_netcall_create(s, &ci);

next:
  return next_publish(s, v);
}

static ngx_int_t ngx_rtmp_notify_play(ngx_rtmp_session_t *s,
                                      ngx_rtmp_play_t *v) {
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_rtmp_netcall_init_t ci;
  ngx_url_t *url;

  if (s->auto_pushed || v->silent) {
    goto next;
  }

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
  if (nacf == NULL) {
    goto next;
  }

  url = nacf->url[NGX_RTMP_NOTIFY_PLAY];

  ngx_rtmp_notify_init(s, v->name, v->args, NGX_RTMP_NOTIFY_PLAYING);

  if (url == NULL) {
    goto next;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "notify: play '%V'",
                &url->url);

  ngx_memzero(&ci, sizeof(ci));

  ci.url = url;
  ci.create = ngx_rtmp_notify_play_create;
  ci.handle = ngx_rtmp_notify_play_handle;
  ci.arg = v;
  ci.argsize = sizeof(*v);

  return ngx_rtmp_netcall_create(s, &ci);

next:
  return next_play(s, v);
}

static ngx_int_t ngx_rtmp_notify_close_stream(ngx_rtmp_session_t *s,
                                              ngx_rtmp_close_stream_t *v) {
  ngx_rtmp_notify_ctx_t *ctx;
  ngx_rtmp_notify_app_conf_t *nacf;

  if (s->auto_pushed) {
    goto next;
  }

  ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

  if (ctx == NULL) {
    goto next;
  }

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

  if (nacf == NULL) {
    goto next;
  }

  if (ctx->flags & NGX_RTMP_NOTIFY_PUBLISHING) {
    ngx_rtmp_notify_done(s, "publish_done", NGX_RTMP_NOTIFY_PUBLISH_DONE);
  }

  if (ctx->flags & NGX_RTMP_NOTIFY_PLAYING) {
    ngx_rtmp_notify_done(s, "play_done", NGX_RTMP_NOTIFY_PLAY_DONE);
  }

  if (ctx->flags) {
    ngx_rtmp_notify_done(s, "done", NGX_RTMP_NOTIFY_DONE);
  }

  if (ctx->update_evt.timer_set) {
    ngx_del_timer(&ctx->update_evt);
  }

  ctx->flags = 0;

next:
  return next_close_stream(s, v);
}

static ngx_int_t ngx_rtmp_notify_record_done(ngx_rtmp_session_t *s,
                                             ngx_rtmp_record_done_t *v) {
  ngx_rtmp_netcall_init_t ci;
  ngx_rtmp_notify_app_conf_t *nacf;

  if (s->auto_pushed) {
    goto next;
  }

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
  if (nacf == NULL || nacf->url[NGX_RTMP_NOTIFY_RECORD_DONE] == NULL) {
    goto next;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                "notify: record_done recorder=%V path='%V' url='%V'",
                &v->recorder, &v->path,
                &nacf->url[NGX_RTMP_NOTIFY_RECORD_DONE]->url);

  ngx_memzero(&ci, sizeof(ci));

  ci.url = nacf->url[NGX_RTMP_NOTIFY_RECORD_DONE];
  ci.create = ngx_rtmp_notify_record_done_create;
  ci.arg = v;

  ngx_rtmp_netcall_create(s, &ci);

next:
  return next_record_done(s, v);
}

static ngx_int_t ngx_rtmp_notify_done(ngx_rtmp_session_t *s, char *cbname,
                                      ngx_uint_t url_idx) {
  ngx_rtmp_netcall_init_t ci;
  ngx_rtmp_notify_done_t ds;
  ngx_rtmp_notify_app_conf_t *nacf;
  ngx_url_t *url;

  nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

  url = nacf->url[url_idx];
  if (url == NULL) {
    return NGX_OK;
  }

  ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "notify: %s '%V'", cbname,
                &url->url);

  ds.cbname = (u_char *)cbname;
  ds.url_idx = url_idx;

  ngx_memzero(&ci, sizeof(ci));

  ci.url = url;
  ci.arg = &ds;
  ci.create = ngx_rtmp_notify_done_create;

  return ngx_rtmp_netcall_create(s, &ci);
}

static ngx_url_t *ngx_rtmp_notify_parse_url(ngx_conf_t *cf, ngx_str_t *url) {
  ngx_url_t *u;
  size_t add;

  add = 0;

  u = ngx_pcalloc(cf->pool, sizeof(ngx_url_t));
  if (u == NULL) {
    return NULL;
  }

  if (ngx_strncasecmp(url->data, (u_char *)"http://", 7) == 0) {
    add = 7;
  }

  u->url.len = url->len - add;
  u->url.data = url->data + add;
  u->default_port = 80;
  u->uri_part = 1;

  if (ngx_parse_url(cf->pool, u) != NGX_OK) {
    if (u->err) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%s in url \"%V\"", u->err,
                         &u->url);
    }
    return NULL;
  }

  return u;
}

static char *ngx_rtmp_notify_on_srv_event(ngx_conf_t *cf, ngx_command_t *cmd,
                                          void *conf) {
  ngx_rtmp_notify_srv_conf_t *nscf = conf;

  ngx_str_t *name, *value;
  ngx_url_t *u;
  ngx_uint_t n;

  value = cf->args->elts;

  u = ngx_rtmp_notify_parse_url(cf, &value[1]);
  if (u == NULL) {
    return NGX_CONF_ERROR;
  }

  name = &value[0];

  n = 0;

  switch (name->len) {
    case sizeof("on_connect") - 1:
      n = NGX_RTMP_NOTIFY_CONNECT;
      break;

    case sizeof("on_disconnect") - 1:
      n = NGX_RTMP_NOTIFY_DISCONNECT;
      break;
  }

  nscf->url[n] = u;

  return NGX_CONF_OK;
}

static char *ngx_rtmp_notify_on_app_event(ngx_conf_t *cf, ngx_command_t *cmd,
                                          void *conf) {
  ngx_rtmp_notify_app_conf_t *nacf = conf;

  ngx_str_t *name, *value;
  ngx_url_t *u;
  ngx_uint_t n;

  value = cf->args->elts;

  u = ngx_rtmp_notify_parse_url(cf, &value[1]);
  if (u == NULL) {
    return NGX_CONF_ERROR;
  }

  name = &value[0];

  n = 0;

  switch (name->len) {
    case sizeof("on_done") - 1: /* and on_play */
      if (name->data[3] == 'd') {
        n = NGX_RTMP_NOTIFY_DONE;
      } else {
        n = NGX_RTMP_NOTIFY_PLAY;
      }
      break;

    case sizeof("on_update") - 1:
      n = NGX_RTMP_NOTIFY_UPDATE;
      break;

    case sizeof("on_publish") - 1:
      n = NGX_RTMP_NOTIFY_PUBLISH;
      break;

    case sizeof("on_play_done") - 1:
      n = NGX_RTMP_NOTIFY_PLAY_DONE;
      break;

    case sizeof("on_record_done") - 1:
      n = NGX_RTMP_NOTIFY_RECORD_DONE;
      break;

    case sizeof("on_publish_done") - 1:
      n = NGX_RTMP_NOTIFY_PUBLISH_DONE;
      break;
  }

  nacf->url[n] = u;

  return NGX_CONF_OK;
}

static char *ngx_rtmp_notify_method(ngx_conf_t *cf, ngx_command_t *cmd,
                                    void *conf) {
  ngx_rtmp_notify_app_conf_t *nacf = conf;

  ngx_rtmp_notify_srv_conf_t *nscf;
  ngx_str_t *value;

  value = cf->args->elts;
  value++;

  if (value->len == sizeof("get") - 1 &&
      ngx_strncasecmp(value->data, (u_char *)"get", value->len) == 0) {
    nacf->method = NGX_RTMP_NETCALL_HTTP_GET;

  } else if (value->len == sizeof("post") - 1 &&
             ngx_strncasecmp(value->data, (u_char *)"post", value->len) == 0) {
    nacf->method = NGX_RTMP_NETCALL_HTTP_POST;

  } else {
    return "got unexpected method";
  }

  nscf = ngx_rtmp_conf_get_module_srv_conf(cf, ngx_rtmp_notify_module);
  nscf->method = nacf->method;

  return NGX_CONF_OK;
}

static ngx_int_t ngx_rtmp_notify_postconfiguration(ngx_conf_t *cf) {
  next_connect = ngx_rtmp_connect;
  ngx_rtmp_connect = ngx_rtmp_notify_connect;

  next_disconnect = ngx_rtmp_disconnect;
  ngx_rtmp_disconnect = ngx_rtmp_notify_disconnect;

  next_publish = ngx_rtmp_publish;
  ngx_rtmp_publish = ngx_rtmp_notify_publish;

  next_play = ngx_rtmp_play;
  ngx_rtmp_play = ngx_rtmp_notify_play;

  next_close_stream = ngx_rtmp_close_stream;
  ngx_rtmp_close_stream = ngx_rtmp_notify_close_stream;

  next_record_done = ngx_rtmp_record_done;
  ngx_rtmp_record_done = ngx_rtmp_notify_record_done;

  return NGX_OK;
}

//////////////////////Json parse//////////////////////

#ifdef _MSC_VER
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

const struct _json_value json_value_none;

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int json_uchar;

static unsigned char hex_value(json_char c) {
  if (isdigit(c)) return c - '0';

  switch (c) {
    case 'a':
    case 'A':
      return 0x0A;
    case 'b':
    case 'B':
      return 0x0B;
    case 'c':
    case 'C':
      return 0x0C;
    case 'd':
    case 'D':
      return 0x0D;
    case 'e':
    case 'E':
      return 0x0E;
    case 'f':
    case 'F':
      return 0x0F;
    default:
      return 0xFF;
  }
}

typedef struct {
  unsigned long used_memory;

  unsigned int uint_max;
  unsigned long ulong_max;

  json_settings settings;
  int first_pass;

  const json_char *ptr;
  unsigned int cur_line, cur_col;

} json_state;

static void *default_alloc(size_t size, int zero, void *user_data) {
  return zero ? calloc(1, size) : malloc(size);
}

static void default_free(void *ptr, void *user_data) { free(ptr); }

static void *json_alloc(json_state *state, unsigned long size, int zero) {
  if ((state->ulong_max - state->used_memory) < size) return 0;

  if (state->settings.max_memory &&
      (state->used_memory += size) > state->settings.max_memory) {
    return 0;
  }

  return state->settings.mem_alloc(size, zero, state->settings.user_data);
}

double pow(double base, double exponent) {
  double ret = 1;
  unsigned int i;

  for (i = 0; i < exponent; ++i) {
    ret *= base;
  }

  return ret;
}

static int new_value(json_state *state, json_value **top, json_value **root,
                     json_value **alloc, json_type type) {
  json_value *value;
  int values_size;

  if (!state->first_pass) {
    value = *top = *alloc;
    *alloc = (*alloc)->_reserved.next_alloc;

    if (!*root) *root = value;

    switch (value->type) {
      case json_array:

        if (value->u.array.length == 0) break;

        if (!(value->u.array.values = (json_value **)json_alloc(
                  state, value->u.array.length * sizeof(json_value *), 0))) {
          return 0;
        }

        value->u.array.length = 0;
        break;

      case json_object:

        if (value->u.object.length == 0) break;

        values_size = sizeof(*value->u.object.values) * value->u.object.length;

        if (!(value->u.object.values = (json_object_entry *)json_alloc(
                  state, values_size + ((unsigned long)value->u.object.values),
                  0))) {
          return 0;
        }

        value->_reserved.object_mem =
            (*(char **)&value->u.object.values) + values_size;

        value->u.object.length = 0;
        break;

      case json_string:

        if (!(value->u.string.ptr = (json_char *)json_alloc(
                  state, (value->u.string.length + 1) * sizeof(json_char),
                  0))) {
          return 0;
        }

        value->u.string.length = 0;
        break;

      default:
        break;
    };

    return 1;
  }

  if (!(value = (json_value *)json_alloc(
            state, sizeof(json_value) + state->settings.value_extra, 1))) {
    return 0;
  }

  if (!*root) *root = value;

  value->type = type;
  value->parent = *top;

#ifdef JSON_TRACK_SOURCE
  value->line = state->cur_line;
  value->col = state->cur_col;
#endif

  if (*alloc) (*alloc)->_reserved.next_alloc = value;

  *alloc = *top = value;

  return 1;
}

#define whitespace     \
  case '\n':           \
    ++state.cur_line;  \
    state.cur_col = 0; \
  case ' ':            \
  case '\t':           \
  case '\r'

#define string_add(b)                                 \
  do {                                                \
    if (!state.first_pass) string[string_length] = b; \
    ++string_length;                                  \
  } while (0);

#define line_and_col state.cur_line, state.cur_col

static const long flag_next = 1 << 0, flag_reproc = 1 << 1,
                  flag_need_comma = 1 << 2, flag_seek_value = 1 << 3,
                  flag_escaped = 1 << 4, flag_string = 1 << 5,
                  flag_need_colon = 1 << 6, flag_done = 1 << 7,
                  flag_num_negative = 1 << 8, flag_num_zero = 1 << 9,
                  flag_num_e = 1 << 10, flag_num_e_got_sign = 1 << 11,
                  flag_num_e_negative = 1 << 12, flag_line_comment = 1 << 13,
                  flag_block_comment = 1 << 14;

json_value *json_parse_ex(json_settings *settings, const json_char *json,
                          size_t length, char *error_buf) {
  json_char error[json_error_max];
  const json_char *end;
  json_value *top, *root, *alloc = 0;
  //全部包装到state类
  json_state state;
  long flags;
  long num_digits = 0, num_e = 0;
  json_int_t num_fraction = 0;

  ngx_memzero(&state, sizeof(json_state));
  /* Skip UTF-8 BOM
   */
  if (length >= 3 && ((unsigned char)json[0]) == 0xEF &&
      ((unsigned char)json[1]) == 0xBB && ((unsigned char)json[2]) == 0xBF) {
    json += 3;
    length -= 3;
  }
  //设置
  error[0] = '\0';
  end = (json + length);

  memcpy(&state.settings, settings, sizeof(json_settings));

  if (!state.settings.mem_alloc) state.settings.mem_alloc = default_alloc;

  if (!state.settings.mem_free) state.settings.mem_free = default_free;

  memset(&state.uint_max, 0xFF, sizeof(state.uint_max));
  memset(&state.ulong_max, 0xFF, sizeof(state.ulong_max));

  state.uint_max -= 8; /* limit of how much can be added before next check */
  state.ulong_max -= 8;
  //设置结束

  //解析
  for (state.first_pass = 1; state.first_pass >= 0; --state.first_pass) {
    json_uchar uchar;
    unsigned char uc_b1, uc_b2, uc_b3, uc_b4;
    json_char *string = 0;
    unsigned int string_length = 0;

    top = root = 0;
    flags = flag_seek_value;

    state.cur_line = 1;

    for (state.ptr = json;; ++state.ptr) {
      json_char b = (state.ptr == end ? 0 : *state.ptr);

      if (flags & flag_string) {
        if (!b) {
          sprintf(error, "Unexpected EOF in string (at %d:%d)", line_and_col);
          goto e_failed;
        }

        if (string_length > state.uint_max) goto e_overflow;

        if (flags & flag_escaped) {
          flags &= ~flag_escaped;

          switch (b) {
            case 'b':
              string_add('\b');
              break;
            case 'f':
              string_add('\f');
              break;
            case 'n':
              string_add('\n');
              break;
            case 'r':
              string_add('\r');
              break;
            case 't':
              string_add('\t');
              break;
            case 'u':

              if (end - state.ptr <= 4 ||
                  (uc_b1 = hex_value(*++state.ptr)) == 0xFF ||
                  (uc_b2 = hex_value(*++state.ptr)) == 0xFF ||
                  (uc_b3 = hex_value(*++state.ptr)) == 0xFF ||
                  (uc_b4 = hex_value(*++state.ptr)) == 0xFF) {
                sprintf(error, "Invalid character value `%c` (at %d:%d)", b,
                        line_and_col);
                goto e_failed;
              }

              uc_b1 = (uc_b1 << 4) | uc_b2;
              uc_b2 = (uc_b3 << 4) | uc_b4;
              uchar = (uc_b1 << 8) | uc_b2;

              if ((uchar & 0xF800) == 0xD800) {
                json_uchar uchar2;

                if (end - state.ptr <= 6 || (*++state.ptr) != '\\' ||
                    (*++state.ptr) != 'u' ||
                    (uc_b1 = hex_value(*++state.ptr)) == 0xFF ||
                    (uc_b2 = hex_value(*++state.ptr)) == 0xFF ||
                    (uc_b3 = hex_value(*++state.ptr)) == 0xFF ||
                    (uc_b4 = hex_value(*++state.ptr)) == 0xFF) {
                  sprintf(error, "Invalid character value `%c` (at %d:%d)", b,
                          line_and_col);
                  goto e_failed;
                }

                uc_b1 = (uc_b1 << 4) | uc_b2;
                uc_b2 = (uc_b3 << 4) | uc_b4;
                uchar2 = (uc_b1 << 8) | uc_b2;

                uchar = 0x010000 | ((uchar & 0x3FF) << 10) | (uchar2 & 0x3FF);
              }

              if (sizeof(json_char) >= sizeof(json_uchar) || (uchar <= 0x7F)) {
                string_add((json_char)uchar);
                break;
              }

              if (uchar <= 0x7FF) {
                if (state.first_pass)
                  string_length += 2;
                else {
                  string[string_length++] = 0xC0 | (uchar >> 6);
                  string[string_length++] = 0x80 | (uchar & 0x3F);
                }

                break;
              }

              if (uchar <= 0xFFFF) {
                if (state.first_pass)
                  string_length += 3;
                else {
                  string[string_length++] = 0xE0 | (uchar >> 12);
                  string[string_length++] = 0x80 | ((uchar >> 6) & 0x3F);
                  string[string_length++] = 0x80 | (uchar & 0x3F);
                }

                break;
              }

              if (state.first_pass)
                string_length += 4;
              else {
                string[string_length++] = 0xF0 | (uchar >> 18);
                string[string_length++] = 0x80 | ((uchar >> 12) & 0x3F);
                string[string_length++] = 0x80 | ((uchar >> 6) & 0x3F);
                string[string_length++] = 0x80 | (uchar & 0x3F);
              }

              break;

            default:
              string_add(b);
          };

          continue;
        }

        if (b == '\\') {
          flags |= flag_escaped;
          continue;
        }

        if (b == '"') {
          if (!state.first_pass) string[string_length] = 0;

          flags &= ~flag_string;
          string = 0;

          switch (top->type) {
            case json_string:

              top->u.string.length = string_length;
              flags |= flag_next;

              break;

            case json_object:

              if (state.first_pass)
                (*(json_char **)&top->u.object.values) += string_length + 1;
              else {
                top->u.object.values[top->u.object.length].name =
                    (json_char *)top->_reserved.object_mem;

                top->u.object.values[top->u.object.length].name_length =
                    string_length;

                (*(json_char **)&top->_reserved.object_mem) +=
                    string_length + 1;
              }

              flags |= flag_seek_value | flag_need_colon;
              continue;

            default:
              break;
          };
        } else {
          string_add(b);
          continue;
        }
      }

      if (state.settings.settings & json_enable_comments) {
        if (flags & (flag_line_comment | flag_block_comment)) {
          if (flags & flag_line_comment) {
            if (b == '\r' || b == '\n' || !b) {
              flags &= ~flag_line_comment;
              --state.ptr; /* so null can be reproc'd */
            }

            continue;
          }

          if (flags & flag_block_comment) {
            if (!b) {
              sprintf(error, "%d:%d: Unexpected EOF in block comment",
                      line_and_col);
              goto e_failed;
            }

            if (b == '*' && state.ptr < (end - 1) && state.ptr[1] == '/') {
              flags &= ~flag_block_comment;
              ++state.ptr; /* skip closing sequence */
            }

            continue;
          }
        } else if (b == '/') {
          if (!(flags & (flag_seek_value | flag_done)) &&
              top->type != json_object) {
            sprintf(error, "%d:%d: Comment not allowed here", line_and_col);
            goto e_failed;
          }

          if (++state.ptr == end) {
            sprintf(error, "%d:%d: EOF unexpected", line_and_col);
            goto e_failed;
          }

          switch (b = *state.ptr) {
            case '/':
              flags |= flag_line_comment;
              continue;

            case '*':
              flags |= flag_block_comment;
              continue;

            default:
              sprintf(error,
                      "%d:%d: Unexpected `%c` in comment opening sequence",
                      line_and_col, b);
              goto e_failed;
          };
        }
      }

      if (flags & flag_done) {
        if (!b) break;

        switch (b) {
        whitespace:
          continue;

          default:

            sprintf(error, "%d:%d: Trailing garbage: `%c`", state.cur_line,
                    state.cur_col, b);

            goto e_failed;
        };
      }

      if (flags & flag_seek_value) {
        switch (b) {
        whitespace:
          continue;

          case ']':

            if (top && top->type == json_array)
              flags =
                  (flags & ~(flag_need_comma | flag_seek_value)) | flag_next;
            else {
              sprintf(error, "%d:%d: Unexpected ]", line_and_col);
              goto e_failed;
            }

            break;

          default:

            if (flags & flag_need_comma) {
              if (b == ',') {
                flags &= ~flag_need_comma;
                continue;
              } else {
                sprintf(error, "%d:%d: Expected , before %c", state.cur_line,
                        state.cur_col, b);

                goto e_failed;
              }
            }

            if (flags & flag_need_colon) {
              if (b == ':') {
                flags &= ~flag_need_colon;
                continue;
              } else {
                sprintf(error, "%d:%d: Expected : before %c", state.cur_line,
                        state.cur_col, b);

                goto e_failed;
              }
            }

            flags &= ~flag_seek_value;

            switch (b) {
              case '{':

                if (!new_value(&state, &top, &root, &alloc, json_object))
                  goto e_alloc_failure;

                continue;

              case '[':

                if (!new_value(&state, &top, &root, &alloc, json_array))
                  goto e_alloc_failure;

                flags |= flag_seek_value;
                continue;

              case '"':

                if (!new_value(&state, &top, &root, &alloc, json_string))
                  goto e_alloc_failure;

                flags |= flag_string;

                string = top->u.string.ptr;
                string_length = 0;

                continue;

              case 't':

                if ((end - state.ptr) < 3 || *(++state.ptr) != 'r' ||
                    *(++state.ptr) != 'u' || *(++state.ptr) != 'e') {
                  goto e_unknown_value;
                }

                if (!new_value(&state, &top, &root, &alloc, json_boolean))
                  goto e_alloc_failure;

                top->u.boolean = 1;

                flags |= flag_next;
                break;

              case 'f':

                if ((end - state.ptr) < 4 || *(++state.ptr) != 'a' ||
                    *(++state.ptr) != 'l' || *(++state.ptr) != 's' ||
                    *(++state.ptr) != 'e') {
                  goto e_unknown_value;
                }

                if (!new_value(&state, &top, &root, &alloc, json_boolean))
                  goto e_alloc_failure;

                flags |= flag_next;
                break;

              case 'n':

                if ((end - state.ptr) < 3 || *(++state.ptr) != 'u' ||
                    *(++state.ptr) != 'l' || *(++state.ptr) != 'l') {
                  goto e_unknown_value;
                }

                if (!new_value(&state, &top, &root, &alloc, json_null))
                  goto e_alloc_failure;

                flags |= flag_next;
                break;

              default:

                if (isdigit(b) || b == '-') {
                  if (!new_value(&state, &top, &root, &alloc, json_integer))
                    goto e_alloc_failure;

                  if (!state.first_pass) {
                    while (isdigit(b) || b == '+' || b == '-' || b == 'e' ||
                           b == 'E' || b == '.') {
                      if ((++state.ptr) == end) {
                        b = 0;
                        break;
                      }

                      b = *state.ptr;
                    }

                    flags |= flag_next | flag_reproc;
                    break;
                  }

                  flags &=
                      ~(flag_num_negative | flag_num_e | flag_num_e_got_sign |
                        flag_num_e_negative | flag_num_zero);

                  num_digits = 0;
                  num_fraction = 0;
                  num_e = 0;

                  if (b != '-') {
                    flags |= flag_reproc;
                    break;
                  }

                  flags |= flag_num_negative;
                  continue;
                } else {
                  sprintf(error, "%d:%d: Unexpected %c when seeking value",
                          line_and_col, b);
                  goto e_failed;
                }
            };
        };
      } else {
        switch (top->type) {
          case json_object:

            switch (b) {
            whitespace:
              continue;

              case '"':

                if (flags & flag_need_comma) {
                  sprintf(error, "%d:%d: Expected , before \"", line_and_col);
                  goto e_failed;
                }

                flags |= flag_string;

                string = (json_char *)top->_reserved.object_mem;
                string_length = 0;

                break;

              case '}':

                flags = (flags & ~flag_need_comma) | flag_next;
                break;

              case ',':

                if (flags & flag_need_comma) {
                  flags &= ~flag_need_comma;
                  break;
                }

              default:
                sprintf(error, "%d:%d: Unexpected `%c` in object", line_and_col,
                        b);
                goto e_failed;
            };

            break;

          case json_integer:
          case json_double:

            if (isdigit(b)) {
              ++num_digits;

              if (top->type == json_integer || flags & flag_num_e) {
                if (!(flags & flag_num_e)) {
                  if (flags & flag_num_zero) {
                    sprintf(error, "%d:%d: Unexpected `0` before `%c`",
                            line_and_col, b);
                    goto e_failed;
                  }

                  if (num_digits == 1 && b == '0') flags |= flag_num_zero;
                } else {
                  flags |= flag_num_e_got_sign;
                  num_e = (num_e * 10) + (b - '0');
                  continue;
                }

                top->u.integer = (top->u.integer * 10) + (b - '0');
                continue;
              }

              num_fraction = (num_fraction * 10) + (b - '0');
              continue;
            }

            if (b == '+' || b == '-') {
              if ((flags & flag_num_e) && !(flags & flag_num_e_got_sign)) {
                flags |= flag_num_e_got_sign;

                if (b == '-') flags |= flag_num_e_negative;

                continue;
              }
            } else if (b == '.' && top->type == json_integer) {
              if (!num_digits) {
                sprintf(error, "%d:%d: Expected digit before `.`",
                        line_and_col);
                goto e_failed;
              }

              top->type = json_double;
              top->u.dbl = (double)top->u.integer;

              num_digits = 0;
              continue;
            }

            if (!(flags & flag_num_e)) {
              if (top->type == json_double) {
                if (!num_digits) {
                  sprintf(error, "%d:%d: Expected digit after `.`",
                          line_and_col);
                  goto e_failed;
                }

                top->u.dbl +=
                    ((double)num_fraction) / (pow(10.0, (double)num_digits));
              }

              if (b == 'e' || b == 'E') {
                flags |= flag_num_e;

                if (top->type == json_integer) {
                  top->type = json_double;
                  top->u.dbl = (double)top->u.integer;
                }

                num_digits = 0;
                flags &= ~flag_num_zero;

                continue;
              }
            } else {
              if (!num_digits) {
                sprintf(error, "%d:%d: Expected digit after `e`", line_and_col);
                goto e_failed;
              }

              top->u.dbl *= pow(
                  10.0, (double)(flags & flag_num_e_negative ? -num_e : num_e));
            }

            if (flags & flag_num_negative) {
              if (top->type == json_integer)
                top->u.integer = -top->u.integer;
              else
                top->u.dbl = -top->u.dbl;
            }

            flags |= flag_next | flag_reproc;
            break;

          default:
            break;
        };
      }

      if (flags & flag_reproc) {
        flags &= ~flag_reproc;
        --state.ptr;
      }

      if (flags & flag_next) {
        flags = (flags & ~flag_next) | flag_need_comma;

        if (!top->parent) {
          /* root value done */

          flags |= flag_done;
          continue;
        }

        if (top->parent->type == json_array) flags |= flag_seek_value;

        if (!state.first_pass) {
          json_value *parent = top->parent;

          switch (parent->type) {
            case json_object:

              parent->u.object.values[parent->u.object.length].value = top;

              break;

            case json_array:

              parent->u.array.values[parent->u.array.length] = top;

              break;

            default:
              break;
          };
        }

        if ((++top->parent->u.array.length) > state.uint_max) goto e_overflow;

        top = top->parent;

        continue;
      }
    }

    alloc = root;
  }

  return root;

e_unknown_value:

  sprintf(error, "%d:%d: Unknown value", line_and_col);
  goto e_failed;

e_alloc_failure:

  strcpy(error, "Memory allocation failure");
  goto e_failed;

e_overflow:

  sprintf(error, "%d:%d: Too long (caught overflow)", line_and_col);
  goto e_failed;

e_failed:

  if (error_buf) {
    if (*error)
      strcpy(error_buf, error);
    else
      strcpy(error_buf, "Unknown error");
  }

  if (state.first_pass) alloc = root;

  while (alloc) {
    top = alloc->_reserved.next_alloc;
    state.settings.mem_free(alloc, state.settings.user_data);
    alloc = top;
  }

  if (!state.first_pass) json_value_free_ex(&state.settings, root);

  return 0;
}

// length 为大小
json_value *json_parse(const json_char *json, size_t length) {
  json_settings settings;
  ngx_memzero(&settings, sizeof(json_settings));
  return json_parse_ex(&settings, json, length, 0);
}

void json_value_free_ex(json_settings *settings, json_value *value) {
  json_value *cur_value;

  if (!value) return;

  value->parent = 0;
  while (value) {
    switch (value->type) {
      case json_array:

        if (!value->u.array.length) {
          settings->mem_free(value->u.array.values, settings->user_data);
          break;
        }

        value = value->u.array.values[--value->u.array.length];
        continue;

      case json_object:

        if (!value->u.object.length) {
          settings->mem_free(value->u.object.values, settings->user_data);
          break;
        }

        value = value->u.object.values[--value->u.object.length].value;
        continue;

      case json_string:

        settings->mem_free(value->u.string.ptr, settings->user_data);
        break;

      default:
        break;
    };

    cur_value = value;
    value = value->parent;
    settings->mem_free(cur_value, settings->user_data);
  }
}

void json_value_free(json_value *value) {
  json_settings settings;
  ngx_memzero(&settings, sizeof(json_settings));
  settings.mem_free = default_free;
  json_value_free_ex(&settings, value);
}

json_value *ngx_find_json_name(json_value *root, ngx_str_t *target) {
  if (root == NULL || target == NULL) return NULL;
  switch (root->type) {
    case json_object:
      return ngx_find_json_name_in_object(root, target);
    case json_array:
      return ngx_find_json_name_in_array(root, target);
    default:
      return NULL;
  }
  return NULL;
}

json_value *ngx_find_json_name_in_object(json_value *root, ngx_str_t *target) {
  int length, i;
  json_object_entry temp;
  json_value *ret;
  if (root == NULL || target == NULL || root->type != json_object) return NULL;

  length = root->u.object.length;
  // root->u.object.values[x].name||value;

  for (i = 0; i < length; ++i) {
    temp = root->u.object.values[i];
    if (ngx_strncasecmp((u_char *)temp.name, target->data, target->len) == 0) {
      return temp.value;
    }

    switch (temp.value->type) {
      case json_object:
        ret = ngx_find_json_name_in_object(temp.value, target);
        if (ret != NULL) return ret;
        break;
      case json_array:
        ret = ngx_find_json_name_in_array(temp.value, target);
        if (ret != NULL) return ret;
        break;
      default:
        break;
    }
  }

  return NULL;
}

json_value *ngx_find_json_name_in_array(json_value *root, ngx_str_t *target) {
  int length, i;
  struct _json_value *temp;
  json_value *ret;
  if (root == NULL || target == NULL || root->type != json_array) return NULL;

  length = root->u.array.length;

  for (i = 0; i < length; ++i) {
    temp = root->u.array.values[i];
    if (temp->type != json_object) continue;

    ret = ngx_find_json_name_in_object(temp, target);
    if (ret != NULL) return ret;
  }
  return NULL;
}

void ngx_match_json_name(json_value *root, ngx_str_t *target, ngx_pool_t *pool,
                         ngx_array_t *arr) {
  if (root == NULL || target == NULL || pool == NULL || arr == NULL) return;

  switch (root->type) {
    case json_object:
      ngx_match_json_name_in_object(root, target, pool, arr);
    case json_array:
      ngx_match_json_name_in_array(root, target, pool, arr);
    default:
      return;
  }
}

void ngx_match_json_name_in_object(json_value *root, ngx_str_t *target,
                                   ngx_pool_t *pool, ngx_array_t *arr) {
  int length, i;
  json_object_entry temp;
  json_value *to_copy;
  ngx_str_t *string;
  if (root == NULL || target == NULL || pool == NULL || arr == NULL ||
      root->type != json_object)
    return;

  length = root->u.object.length;
  // root->u.object.values[x].name||value;

  for (i = 0; i < length; ++i) {
    temp = root->u.object.values[i];
    if (ngx_strncasecmp((u_char *)temp.name, target->data, target->len) == 0 &&
        temp.value->type == json_string && temp.value->u.string.length >= 7) {
      to_copy = temp.value;
      string = ngx_array_push(arr);
      string->len = to_copy->u.string.length;
      string->data = ngx_pcalloc(pool, to_copy->u.string.length);

      ngx_memcpy(string->data, to_copy->u.string.ptr, to_copy->u.string.length);
      continue;
    }

    switch (temp.value->type) {
      case json_object:
        ngx_match_json_name_in_object(temp.value, target, pool, arr);
        break;
      case json_array:
        ngx_match_json_name_in_array(temp.value, target, pool, arr);
      default:
        break;
    }
  }
}

void ngx_match_json_name_in_array(json_value *root, ngx_str_t *target,
                                  ngx_pool_t *pool, ngx_array_t *arr) {
  int length, i;
  struct _json_value *temp;
  if (root == NULL || target == NULL || pool == NULL || arr == NULL ||
      root->type != json_array)
    return;

  length = root->u.array.length;

  for (i = 0; i < length; ++i) {
    temp = root->u.array.values[i];
    if (temp->type != json_object) continue;

    ngx_match_json_name_in_object(temp, target, pool, arr);
  }
  return;
}
