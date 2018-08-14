
/*
 * Copyright (C) Roman Arutyunyan
 */

#include "ngx_rtmp_bandwidth.h"
#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_live_module.h"

void ngx_rtmp_update_bandwidth(ngx_rtmp_bandwidth_t *bw, uint32_t bytes) {
  if (ngx_cached_time->sec > bw->intl_end) {
    bw->bandwidth =
        ngx_cached_time->sec > bw->intl_end + NGX_RTMP_BANDWIDTH_INTERVAL
            ? 0
            : bw->intl_bytes / NGX_RTMP_BANDWIDTH_INTERVAL;
    bw->intl_bytes = 0;
    bw->intl_end = ngx_cached_time->sec + NGX_RTMP_BANDWIDTH_INTERVAL;
  }

  bw->bytes += bytes;
  bw->intl_bytes += bytes;
}

void ngx_rtmp_update_in_videoframe(ngx_rtmp_in_videoframe_t *vf, ngx_uint_t fps,
                                   ngx_uint_t num) {
  ngx_rtmp_live_app_conf_t *lacf;

  if (ngx_cached_time->sec >= vf->intl_end) {
    ngx_rtmp_session_t *s = vf->publish;

    lacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_live_module);

    if (vf->intl_videoframenum != 0) {
      if (vf->isSave) {
        ngx_rtmp_in_videoframe_element_t *element;
        ngx_uint_t iscloseconnect = 0;

        if (ngx_cached_time->sec >=
            vf->intl_end + NGX_RTMP_IN_VIDEOFRAME_INTERVAL) {
          ngx_uint_t elenum = (ngx_cached_time->sec - vf->intl_end) /
                              NGX_RTMP_IN_VIDEOFRAME_INTERVAL;
          while (elenum--) {
            element = ngx_array_push(vf->elementArray);
            element->num = 0;
            element->percent = 0;
            vf->no_statis_num++;
          }
          element = ngx_array_push(vf->elementArray);
          element->num = vf->intl_videoframenum;
          element->percent = 0;
          vf->no_statis_num++;
          vf->no_data_num++;
          vf->no_data_framenum += vf->intl_videoframenum;
          ngx_log_error(
              NGX_LOG_ERR, s->connection->log, 0,
              "live  jitter "
              "name=%s,duration=%i,framenum=%ui,pos=%ui,nodataseq=%ui",
              s->streamname, ngx_cached_time->sec > vf->intl_end, element->num,
              vf->elementArray->nelts, vf->no_data_num);
        } else {
          element = ngx_array_push(vf->elementArray);
          element->num = vf->intl_videoframenum;
          if (fps)
            element->percent =
                (double)element->num / (fps * NGX_RTMP_IN_VIDEOFRAME_INTERVAL);
          else if (vf->intl_videoframe_ave)
            element->percent = (double)element->num / vf->intl_videoframe_ave;
          else
            element->percent = 1;

          if (fps) {
            ngx_log_error(
                NGX_LOG_INFO, s->connection->log, 0,
                "live "
                "name=%s,framenum=%ui,percent=%.3f,ave=%ui,fps=%ui,count=%ui",
                s->streamname, element->num, element->percent,
                vf->intl_videoframe_ave, fps, vf->elementArray->nelts);
          } else {
            ngx_log_error(
                NGX_LOG_INFO, s->connection->log, 0,
                "live "
                "name=%s,framenum=%ui,percent=%.3f,ave=%ui,fps=%ui,count=%ui",
                s->streamname, element->num, element->percent,
                vf->intl_videoframe_ave, vf->fps, vf->elementArray->nelts);
          }

          if (vf->elementArray->nelts - vf->no_statis_num >=
              NGX_RTMP_IN_VIDEOFRAME_SCOPE_1) {
            if (element->percent <= NGX_RTMP_IN_VIDEOFRAME_LIMIT_1) {
              ngx_rtmp_in_videoframe_element_t *firstElement =
                  vf->elementArray->elts;
              ngx_uint_t n = 1;
              ngx_uint_t i;
              for (i = 0; i < NGX_RTMP_IN_VIDEOFRAME_SCOPE_1 - 1; i++) {
                if (firstElement[vf->elementArray->nelts - 2 - i].num &&
                    firstElement[vf->elementArray->nelts - 2 - i].percent <=
                        NGX_RTMP_IN_VIDEOFRAME_LIMIT_1 &&
                    ++n >= NGX_RTMP_IN_VIDEOFRAME_MATCH_1) {
                  ngx_log_error(
                      NGX_LOG_ERR, s->connection->log, 0,
                      "drop publish because live jitter name=%s,condition=%.3f",
                      s->streamname, NGX_RTMP_IN_VIDEOFRAME_LIMIT_1);
                  if (lacf && lacf->jitter) {
                    ngx_rtmp_finalize_session(s);
                  }
                  iscloseconnect = 1;
                  break;
                }
              }
              if (!iscloseconnect &&
                  element->percent <= NGX_RTMP_IN_VIDEOFRAME_LIMIT_2) {
                n = 1;
                for (i = 0; i < NGX_RTMP_IN_VIDEOFRAME_SCOPE_2 - 1; i++) {
                  if (firstElement[vf->elementArray->nelts - 2 - i].num &&
                      firstElement[vf->elementArray->nelts - 2 - i].percent <=
                          NGX_RTMP_IN_VIDEOFRAME_LIMIT_2 &&
                      ++n >= NGX_RTMP_IN_VIDEOFRAME_MATCH_2) {
                    ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                                  "drop publish because live jitter "
                                  "name=%s,condition=%.3f",
                                  s->streamname,
                                  NGX_RTMP_IN_VIDEOFRAME_LIMIT_2);
                    if (lacf && lacf->jitter) {
                      ngx_rtmp_finalize_session(s);
                    }
                    iscloseconnect = 1;
                    break;
                  }
                }
              }
            }
            vf->intl_videoframe_ave =
                (vf->videoframe_total - vf->no_data_framenum) /
                (vf->elementArray->nelts - vf->no_statis_num);
            vf->fps = vf->intl_videoframe_ave / NGX_RTMP_IN_VIDEOFRAME_INTERVAL;
          }
        }

        if (vf->elementArray->nelts - vf->no_statis_num >=
                NGX_RTMP_IN_VIDEOFRAME_SCOPE_1 ||
            element->percent < 0.0001) {
          if (element->percent <= NGX_RTMP_IN_VIDEOFRAME_LIMIT_3) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "live jitter name=%s,duration=%ui,framenum=%ui",
                          s->streamname,
                          ngx_cached_time->sec - vf->intl_end +
                              NGX_RTMP_IN_VIDEOFRAME_INTERVAL,
                          element->num);

            if (vf->net_jitter_time) {
              ngx_log_error(
                  NGX_LOG_ERR, s->connection->log, 0,
                  "drop publish because live jitter name=%s,condition=%.3f",
                  s->streamname, NGX_RTMP_IN_VIDEOFRAME_LIMIT_3);
              if (lacf && lacf->jitter) {
                ngx_rtmp_finalize_session(s);
              }
            }

            vf->net_jitter_time = ngx_cached_time->sec;
          }

          if (vf->net_jitter_time &&
              ngx_cached_time->sec >
                  vf->net_jitter_time + NGX_RTMP_IN_JITTER_INTERVAL) {
            ngx_log_error(
                NGX_LOG_ERR, s->connection->log, 0,
                "live jitter name=%s havn't happened exceed %ui seconds ",
                s->streamname, ngx_cached_time->sec - vf->net_jitter_time);
            vf->net_jitter_time = 0;
          }
        }
      } else {
        vf->videoframe_total = 0;
      }
      vf->isSave = 1;
      vf->intl_videoframenum = 0;
    }

    if (vf->videoframe_total || num > 0)
      vf->intl_end = ngx_cached_time->sec + NGX_RTMP_IN_VIDEOFRAME_INTERVAL;
  }

  vf->videoframe_total += num;
  vf->intl_videoframenum += num;

  if (vf->videoframe_total == 1 && num != 0) vf->videostart = ngx_current_msec;

  if (num != 0) vf->videoend = ngx_current_msec;
}
