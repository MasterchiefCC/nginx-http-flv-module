
/*
 * Copyright (C) Roman Arutyunyan
 */

#ifndef _NGX_RTMP_BANDWIDTH_H_INCLUDED_
#define _NGX_RTMP_BANDWIDTH_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>

/* Bandwidth update interval in seconds */
#define NGX_RTMP_BANDWIDTH_INTERVAL 10

typedef struct {
  uint64_t bytes;
  uint64_t bandwidth; /* bytes/sec */

  time_t intl_end;
  uint64_t intl_bytes;
} ngx_rtmp_bandwidth_t;

extern ngx_rtmp_bandwidth_t ngx_rtmp_bw_out;
extern ngx_rtmp_bandwidth_t ngx_rtmp_bw_in;

#include "ngx_rtmp.h"

void ngx_rtmp_update_bandwidth(ngx_rtmp_bandwidth_t *bw, uint32_t bytes);

/*in videoframe update interval in seconds */
#define NGX_RTMP_IN_VIDEOFRAME_INTERVAL 5

#define NGX_RTMP_IN_VIDEOFRAME_SCOPE_1 24
#define NGX_RTMP_IN_VIDEOFRAME_MATCH_1 4
#define NGX_RTMP_IN_VIDEOFRAME_LIMIT_1 0.75

#define NGX_RTMP_IN_VIDEOFRAME_SCOPE_2 12
#define NGX_RTMP_IN_VIDEOFRAME_MATCH_2 3
#define NGX_RTMP_IN_VIDEOFRAME_LIMIT_2 0.5

#define NGX_RTMP_IN_VIDEOFRAME_LIMIT_3 0.333
#define NGX_RTMP_IN_VIDEOFRAME_MATCH_3 2
#define NGX_RTMP_IN_JITTER_INTERVAL 10 * 60

typedef struct {
  ngx_uint_t num;
  double percent;
} ngx_rtmp_in_videoframe_element_t;

typedef struct {
  ngx_rtmp_session_t *publish;
  ngx_array_t *elementArray;
  ngx_uint_t isSave;
  ngx_uint_t videoframe_total;
  ngx_uint_t intl_videoframenum;
  ngx_uint_t no_statis_num;
  ngx_uint_t no_data_num;
  ngx_uint_t no_data_framenum;
  ngx_uint_t intl_videoframe_ave;
  ngx_uint_t fps;
  time_t net_jitter_time;
  time_t intl_end;
  ngx_msec_t videostart;
  ngx_msec_t videoend;
} ngx_rtmp_in_videoframe_t;

void ngx_rtmp_update_in_videoframe(ngx_rtmp_in_videoframe_t *vf, ngx_uint_t fps,
                                   ngx_uint_t num);

#endif /* _NGX_RTMP_BANDWIDTH_H_INCLUDED_ */
