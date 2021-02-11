/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#if CONFIG_AVFILTER
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#endif

#include <SDL2/SDL.h>

#include "ffplayer/ffplayer.h"
#include "ffplayer/utils.h"
#include "ffp_player_internal.h"

#if _FLUTTER

#include "ffplayer/flutter.h"
#include "third_party/dart/dart_api_dl.h"

#endif

/* options specified by the user */
static AVInputFormat *file_iformat;


#if CONFIG_AVFILTER
static int opt_add_vfilter(void *optctx, const char *opt, const char *arg) {
    GROW_ARRAY(vfilters_list, nb_vfilters);
    vfilters_list[nb_vfilters - 1] = arg;
    return 0;
}
#endif

#define CHECK_PLAYER_WITH_RETURN(PLAYER, RETURN)                               \
if(!(PLAYER) || !(PLAYER)->is) {                                               \
    av_log(nullptr, AV_LOG_ERROR, "check player failed");                      \
    return RETURN;                                                             \
}                                                                              \

#define CHECK_PLAYER(PLAYER)                                                   \
if (!(PLAYER) || !(PLAYER)->is) {                                              \
    av_log(nullptr, AV_LOG_ERROR, "check player failed");                      \
    return;                                                                    \
}                                                                              \

const AVRational av_time_base_q_ = {1, AV_TIME_BASE};

extern AVPacket *flush_pkt;

static inline int64_t get_valid_channel_layout(int64_t channel_layout, int channels) {
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

static inline void on_buffered_update(CPlayer *player, double position) {
    int64_t mills = position * 1000;
    player->buffered_position = mills;
    ffp_send_msg1(player, FFP_MSG_BUFFERING_TIME_UPDATE, mills);
}


static void change_player_state(CPlayer *player, FFPlayerState state) {
    if (player->state == state) {
        return;
    }
    player->state = state;
    ffp_send_msg1(player, FFP_MSG_PLAYBACK_STATE_CHANGED, state);
}

static int message_loop(void *args) {
    auto *player = static_cast<CPlayer *>(args);
    while (true) {
        FFPlayerMessage msg = {0};
        if (player->msg_queue.Get(&msg, true) < 0) {
            break;
        }
#ifdef _FLUTTER
        if (player->message_send_port) {
            // dart do not support int64_t array yet.
            // thanks https://github.com/dart-lang/sdk/issues/44384#issuecomment-738708448
            // so we pass an uint8_t array to dart isolate.
            int64_t arrays[] = {msg.what, msg.arg1, msg.arg2};
            Dart_CObject dart_args;
            dart_args.type = Dart_CObject_kTypedData;
            dart_args.value.as_typed_data.type = Dart_TypedData_kUint8;
            dart_args.value.as_typed_data.length = 3 * sizeof(int64_t);
            dart_args.value.as_typed_data.values = (uint8_t *) arrays;
            Dart_PostCObject_DL(player->message_send_port, &dart_args);
        }
#else
        if (player->on_message) {
            player->on_message(player, msg.what, msg.arg1, msg.arg2);
        }
#endif
    }

    return 0;
}

static void on_decode_frame_block(void *opacity) {
    auto *player = static_cast<CPlayer *>(opacity);
    change_player_state(player, FFP_STATE_BUFFERING);
}

static void stream_component_close(CPlayer *player, int stream_index) {
    VideoState *is = player->is;
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->auddec.Abort(&is->sampq);
            SDL_CloseAudioDevice(player->audio_dev);
            is->auddec.Destroy();
            swr_free(&is->swr_ctx);
            av_freep(&is->audio_buf1);
            is->audio_buf1_size = 0;
            is->audio_buf = nullptr;

            if (is->rdft) {
                av_rdft_end(is->rdft);
                av_freep(&is->rdft_data);
                is->rdft = nullptr;
                is->rdft_bits = 0;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->viddec.Abort(&is->pictq);
            is->viddec.Destroy();
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subdec.Abort(&is->subpq);
            is->subdec.Destroy();
            break;
        default:
            break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = nullptr;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = nullptr;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = nullptr;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}

static void stream_close(CPlayer *player) {
    VideoState *is = player->is;
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, nullptr);

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(player, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(player, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(player, is->subtitle_stream);

    change_player_state(player, FFP_STATE_IDLE);
    player->msg_queue.Abort();
    if (player->msg_tid) {
        SDL_WaitThread(player->msg_tid, nullptr);
    }

    player->video_render_ctx.Stop(player);

    avformat_close_input(&is->ic);

    is->videoq.Destroy();
    is->audioq.Destroy();
    is->subtitleq.Destroy();
    player->msg_queue.Abort();

    /* free all pictures */
    is->pictq.Destroy();
    is->sampq.Destroy();
    is->subpq.Destroy();
    SDL_DestroyCond(is->continue_read_thread);
    av_free(is->filename);

    av_free(is);
    av_free(player);
}


static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is) {
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = is->vidclk.GetClock();
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = is->audclk.GetClock();
            break;
        default:
            val = is->extclk.GetClock();
            break;
    }
    return val;
}


/* seek in the stream */
static void stream_seek(CPlayer *player, int64_t pos, int64_t rel, int seek_by_bytes) {
    VideoState *is = player->is;
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        player->buffered_position = -1;
        change_player_state(player, FFP_STATE_BUFFERING);
        SDL_CondSignal(is->continue_read_thread);
        if (!ffplayer_is_paused(player)) {
            ffplayer_toggle_pause(player);
        }
    }
}

void ffplayer_seek_to_position(CPlayer *player, double position) {
    if (!player) {
        av_log(nullptr, AV_LOG_ERROR, "ffplayer_seek_to_position: player is not available");
        return;
    }
    if (player->is->ic->start_time != AV_NOPTS_VALUE) {
        double start = player->is->ic->start_time / (double) AV_TIME_BASE;
        if (position < start) {
            position = start;
        }
    }
    if (position < 0) {
        av_log(nullptr, AV_LOG_ERROR, "failed to seek to %0.2f.\n", position);
        return;
    }
    av_log(nullptr, AV_LOG_INFO, "ffplayer_seek_to_position to %0.2f \n", position);
    stream_seek(player, (int64_t) (position * AV_TIME_BASE), 0, 0);
}

double ffplayer_get_current_position(CPlayer *player) {
    if (!player || !player->is) {
        av_log(nullptr, AV_LOG_ERROR, "ffplayer_get_current_position: player is not available.\n");
        return 0;
    }
    double position = get_master_clock(player->is);
    if (isnan(position)) {
        position = (double) player->is->seek_pos / AV_TIME_BASE;
    }
    return position;
}

double ffplayer_get_duration(CPlayer *player) {
    if (!player || !player->is || !player->is->ic) {
        av_log(nullptr, AV_LOG_ERROR, "ffplayer_get_duration: player is not available. %p \n", player);
        return -1;
    }
    return player->is->ic->duration / (double) AV_TIME_BASE;
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is) {
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        is->vidclk.SetClock(is->vidclk.GetClock(), is->vidclk.serial);
    }
    is->extclk.SetClock(is->extclk.GetClock(), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

void ffplayer_toggle_pause(CPlayer *player) {
    stream_toggle_pause(player->is);
    player->is->step = 0;
}

bool ffplayer_is_mute(CPlayer *player) {
    return player->is->muted;
}

void ffplayer_set_mute(CPlayer *player, bool mute) {
    player->is->muted = mute;
}

void ffp_set_volume(CPlayer *player, int volume) {
    CHECK_PLAYER(player);
    volume = av_clip(volume, 0, 100);
    volume = av_clip(SDL_MIX_MAXVOLUME * volume / 100, 0, SDL_MIX_MAXVOLUME);
    player->is->audio_volume = volume;
}

int ffp_get_volume(CPlayer *player) {
    CHECK_PLAYER_WITH_RETURN(player, 0);
    int volume = player->is->audio_volume * 100 / SDL_MIX_MAXVOLUME;
    return av_clip(volume, 0, 100);
}

bool ffplayer_is_paused(CPlayer *player) {
    CHECK_PLAYER_WITH_RETURN(player, false);
    return player->is->paused;
}


static int queue_picture(CPlayer *player, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial) {
    VideoState *is = player->is;
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = is->pictq.PeekWritable()))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    auto render_ctx = &player->video_render_ctx;
    if (!render_ctx->first_video_frame_loaded) {
        render_ctx->first_video_frame_loaded = true;
        // https://forum.videohelp.com/threads/323530-please-explain-SAR-DAR-PAR#post2003533
        render_ctx->frame_width = vp->width;
        render_ctx->frame_height = av_rescale(vp->width, vp->height * vp->sar.den, vp->width * vp->sar.num);
        ffp_send_msg2(player, FFP_MSG_VIDEO_FRAME_LOADED, render_ctx->frame_width, render_ctx->frame_height);
    }

    av_frame_move_ref(vp->frame, src_frame);
    is->pictq.Push();
    return 0;
}

static int get_video_frame(CPlayer *player, AVFrame *frame) {
    VideoState *is = player->is;
    int got_picture;
    if ((got_picture = decoder_decode_frame(&is->viddec, frame, nullptr)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (player->framedrop > 0 || (player->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

#if CONFIG_AVFILTER
static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx) {
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = nullptr, *inputs = nullptr;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = nullptr;

        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, nullptr)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext *, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, nullptr);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame) {
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = nullptr, *filt_out = nullptr, *last_filter = nullptr;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, nullptr);
    AVDictionaryEntry *e = nullptr;
    int nb_pix_fmts = 0;
    int i, j;

    for (i = 0; i < renderer_info.num_texture_formats; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
            if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

    while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, nullptr,
                                            graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", nullptr, nullptr, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg)                                                \
    do {                                                                      \
        AVFilterContext *filt_ctx;                                            \
                                                                              \
        ret = avfilter_graph_create_filter(&filt_ctx,                         \
                                           avfilter_get_by_name(name),        \
                                           "ffplay_" name, arg, nullptr, graph); \
        if (ret < 0)                                                          \
            goto fail;                                                        \
                                                                              \
        ret = avfilter_link(filt_ctx, 0, last_filter, 0);                     \
        if (ret < 0)                                                          \
            goto fail;                                                        \
                                                                              \
        last_filter = filt_ctx;                                               \
    } while (0)

    if (autorotate) {
        double theta = get_rotation(is->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", nullptr);
            INSERT_FILT("vflip", nullptr);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format) {
    static const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE};
    int sample_rates[2] = {0, -1};
    int64_t channel_layouts[2] = {0, -1};
    int channels[2] = {0, -1};
    AVFilterContext *filt_asrc = nullptr, *filt_asink = nullptr;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = nullptr;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = filter_nbthreads;

    while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%" PRIx64, is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, nullptr, is->agraph);
    if (ret < 0)
        goto end;

    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       nullptr, nullptr, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels[0] = is->audio_tgt.channels;
        sample_rates[0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}
#endif /* CONFIG_AVFILTER */


static int video_thread(void *arg) {
    auto *player = static_cast<CPlayer *>(arg);
    VideoState *is = player->is;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, nullptr);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = nullptr;
    AVFilterContext *filt_out = nullptr, *filt_in = nullptr;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
#endif

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(player, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

#if CONFIG_AVFILTER
        if (last_w != frame->width || last_h != frame->height || last_format != frame->format || last_serial != is->viddec.pkt_serial || last_vfilter_idx != is->vfilter_idx) {
            av_log(nullptr, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_nullptr(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_nullptr(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : nullptr, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
        duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        ret = queue_picture(player, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
        av_frame_unref(frame);
#if CONFIG_AVFILTER
        if (is->videoq.serial != is->viddec.pkt_serial)
            break;
    }
#endif

        if (ret < 0)
            goto the_end;
    }
    the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

static int subtitle_thread(void *arg) {
    auto *player = static_cast<CPlayer *>(arg);
    VideoState *is = player->is;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = is->subpq.PeekWritable()))
            return 0;

        if ((got_subtitle = decoder_decode_frame(&is->subdec, nullptr, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double) AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            is->subpq.Push();
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

// TODO audio sample callback.
/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size) {
    int size, len;

    size = samples_size / (int) sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = is->audclk.GetClock() - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int) (diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(nullptr, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}


/* open a given stream. Return 0 if OK */
static int stream_component_open(CPlayer *player, int stream_index) {
    VideoState *is = player->is;
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = nullptr;
    AVDictionary *opts = nullptr;
    AVDictionaryEntry *t = nullptr;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = player->lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(nullptr);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->last_audio_stream = stream_index;
            forced_codec_name = player->audio_codec_name;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->last_subtitle_stream = stream_index;
            forced_codec_name = player->subtitle_codec_name;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->last_video_stream = stream_index;
            forced_codec_name = player->video_codec_name;
            break;
        default:
            break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name)
            av_log(nullptr, AV_LOG_WARNING,
                   "No codec could be found with name '%s'\n", forced_codec_name);
        else
            av_log(nullptr, AV_LOG_WARNING,
                   "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (player->fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (!av_dict_get(opts, "threads", nullptr, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
        av_log(nullptr, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_stream = stream_index;
            is->video_st = ic->streams[stream_index];

//            is->viddec.Init(avctx, &is->videoq, is->continue_read_thread);
            if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", player)) < 0)
                goto out;
            is->queue_attachments_req = 1;

            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_stream = stream_index;
            is->subtitle_st = ic->streams[stream_index];

//            is->subdec.Init(avctx, &is->subtitleq, is->continue_read_thread);
            if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", player)) < 0)
                goto out;
            break;
        default:
            break;
    }
    goto out;

    fail:
    avcodec_free_context(&avctx);
    out:
    av_dict_free(&opts);

    return ret;
}


#define BUFFERING_CHECK_PER_MILLISECONDS                     (500)
#define BUFFERING_CHECK_PER_MILLISECONDS_NO_RENDERING        (20)

static void check_buffering(CPlayer *player) {
    VideoState *is = player->is;
    if (is->eof) {
        double position = ffplayer_get_duration(player);
        if (position <= 0 && is->audioq.last_pkt) {
            position = av_q2d(is->audio_st->time_base) *
                       (int64_t) (is->audioq.last_pkt->pkt.pts + is->audioq.last_pkt->pkt.duration);
        }
        if (position <= 0 && is->videoq.last_pkt) {
            position = av_q2d(is->video_st->time_base) *
                       (int64_t) (is->videoq.last_pkt->pkt.pts + is->videoq.last_pkt->pkt.duration);
        }
        on_buffered_update(player, position);
        change_player_state(player, FFP_STATE_READY);
        return;
    }

    int64_t current_ts = av_gettime_relative() / 1000;
    int step = player->state == FFP_STATE_BUFFERING ? BUFFERING_CHECK_PER_MILLISECONDS_NO_RENDERING
                                                    : BUFFERING_CHECK_PER_MILLISECONDS;
    if (current_ts - player->last_io_buffering_ts < step) {
        return;
    }
    if (player->state == FFP_STATE_END || player->state == FFP_STATE_IDLE) {
        return;
    }
    player->last_io_buffering_ts = current_ts;
    double cached_position = INT_MAX;
    int nb_packets = INT_MAX;
    if (is->audio_st && is->audioq.last_pkt) {
        nb_packets = FFP_MIN(nb_packets, player->is->audioq.nb_packets);
        double audio_position = player->is->audioq.last_pkt->pkt.pts * av_q2d(player->is->audio_st->time_base);
        cached_position = FFMIN(cached_position, audio_position);
        av_log(nullptr, AV_LOG_DEBUG, "audio q cached: %f \n",
               player->is->audioq.duration * av_q2d(player->is->audio_st->time_base));
    }
    if (is->video_st && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        cached_position = FFP_MIN(cached_position,
                                  player->is->videoq.duration * av_q2d(player->is->video_st->time_base));
        nb_packets = FFP_MIN(nb_packets, player->is->videoq.nb_packets);
        av_log(nullptr, AV_LOG_DEBUG, "video q cached: %f \n",
               player->is->videoq.duration * av_q2d(player->is->video_st->time_base));
    }
    if (is->subtitle_st) {
        cached_position = FFP_MIN(cached_position,
                                  player->is->subtitleq.duration * av_q2d(player->is->subtitle_st->time_base));
        nb_packets = FFP_MIN(nb_packets, player->is->subtitleq.nb_packets);
        av_log(nullptr, AV_LOG_DEBUG, "subtitle q cached: %f \n",
               player->is->subtitleq.duration * av_q2d(player->is->audio_st->time_base));
    }
    if (cached_position == INT_MAX) {
        av_log(nullptr, AV_LOG_ERROR, "check_buffering failed\n");
        return;
    }
    on_buffered_update(player, cached_position);

    bool ready = false;
    if (is->video_st) {

    }
    if ((player->is->videoq.nb_packets > CACHE_THRESHOLD_MIN_FRAMES || player->is->video_stream < 0 ||
         player->is->videoq.abort_request)
        && (player->is->audioq.nb_packets > CACHE_THRESHOLD_MIN_FRAMES || player->is->audio_stream < 0 ||
            player->is->audioq.abort_request)
        && (player->is->subtitleq.nb_packets > CACHE_THRESHOLD_MIN_FRAMES || player->is->subtitle_stream < 0 ||
            player->is->subtitleq.abort_request)) {
        change_player_state(player, FFP_STATE_READY);
    }

}


static void stream_open(CPlayer *player, const char *filename, AVInputFormat *iformat) {
    change_player_state(player, FFP_STATE_BUFFERING);

    VideoState *is = player->is;

    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->iformat = iformat;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

//    is->read_tid = SDL_CreateThread(read_thread, "ReadThread", player);
    if (!is->read_tid) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        goto fail;
    }

    player->msg_queue.Start();
    player->msg_tid = SDL_CreateThread(message_loop, "message_loop", player);
    if (!player->msg_tid) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        goto fail;
    }
    return;
    fail:
    stream_close(player);
    player->is = nullptr;
}

int ffplayer_get_chapter_count(CPlayer *player) {
    if (!player || !player->is->ic) {
        return -1;
    }
    return (int) player->is->ic->nb_chapters;
}

int ffplayer_get_current_chapter(CPlayer *player) {
    if (!player || !player->is->ic) {
        return -1;
    }
    int64_t pos = get_master_clock(player->is) * AV_TIME_BASE;

    if (!player->is->ic->nb_chapters) {
        return -1;
    }
    for (int i = 0; i < player->is->ic->nb_chapters; i++) {
        AVChapter *ch = player->is->ic->chapters[i];
        if (av_compare_ts(pos, av_time_base_q_, ch->start, ch->time_base) < 0) {
            i--;
            return i;
        }
    }
    return -1;
}

void ffplayer_seek_to_chapter(CPlayer *player, int chapter) {
    if (!player || !player->is->ic) {
        av_log(nullptr, AV_LOG_ERROR, "player not prepared");
        return;
    }
    if (!player->is->ic->nb_chapters) {
        av_log(nullptr, AV_LOG_ERROR, "this video do not contain chapters");
        return;
    }
    if (chapter < 0 || chapter >= player->is->ic->nb_chapters) {
        av_log(nullptr, AV_LOG_ERROR, "chapter out of range: %d", chapter);
        return;
    }
    AVChapter *ac = player->is->ic->chapters[chapter];
    stream_seek(player, av_rescale_q(ac->start, ac->time_base, av_time_base_q_), 0, 0);
}

static VideoState *alloc_video_state() {
    auto *is = static_cast<VideoState *>(av_mallocz(sizeof(VideoState)));
    if (!is)
        return nullptr;
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;

    /* start video display */
    if (is->pictq.Init(&is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (is->subpq.Init(&is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (is->sampq.Init(&is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (is->videoq.Init() < 0 || is->audioq.Init() < 0 || is->subtitleq.Init() < 0)
        goto fail;

    is->vidclk.Init(&is->videoq.serial);
    is->audclk.Init(&is->audioq.serial);
    is->extclk.Init(&is->extclk.serial);
    is->audio_clock_serial = -1;
    is->audio_volume = SDL_MIX_MAXVOLUME;
    is->muted = 0;
    is->av_sync_type = AV_SYNC_AUDIO_MASTER;
    return is;
    fail:
    if (is) {
        av_free(is);
    }
    return nullptr;
}

static CPlayer *ffplayer_alloc_player() {
    auto *player = static_cast<CPlayer *>(av_mallocz(sizeof(CPlayer)));
    if (!player) {
        return nullptr;
    }
    memset(player->wanted_stream_spec, 0, sizeof player->wanted_stream_spec);
    player->audio_disable = 0;
    player->video_disable = 0;
    player->subtitle_disable = 0;

    player->seek_by_bytes = -1;

    player->show_status = -1;
    player->start_time = AV_NOPTS_VALUE;
    player->duration = AV_NOPTS_VALUE;
    player->fast = 0;
    player->genpts = 0;
    player->lowres = 0;
    player->decoder_reorder_pts = -1;

    player->loop = 1;
    player->framedrop = -1;
    player->infinite_buffer = -1;
    player->show_mode = SHOW_MODE_NONE;

    player->audio_codec_name = nullptr;
    player->subtitle_codec_name = nullptr;
    player->video_codec_name = nullptr;

    player->rdftspeed = 0.02;

    player->autorotate = 1;
    player->find_stream_info = 1;
    player->filter_nbthreads = 0;

    player->audio_callback_time = 0;

    player->audio_dev = 0;

    player->on_load_metadata = nullptr;
    player->on_message = nullptr;

    player->buffered_position = -1;
    player->state = FFP_STATE_IDLE;
    player->last_io_buffering_ts = -1;

    player->is = alloc_video_state();

    player->msg_queue.Init();

    ffplayer_toggle_pause(player);

    if (!player->is) {
        av_free(player);
        return nullptr;
    }
#ifdef _FLUTTER
    flutter_on_post_player_created(player);
#endif
    av_log(nullptr, AV_LOG_INFO, "malloc player, %p", player);
    return player;
}

int ffplayer_open_file(CPlayer *player, const char *filename) {
//    stream_open(player, filename, file_iformat);
    if (player->dataSource) {
        av_log(nullptr, AV_LOG_ERROR, "can not open file multi-times.\n");
        return -1;
    }
    player->dataSource = new DataSource(filename, file_iformat);
    player->dataSource->Open(player);
    player->dataSource->audio_queue = &player->is->audioq;
    player->dataSource->video_queue = &player->is->videoq;
    player->dataSource->subtitle_queue = &player->is->subtitleq;
    player->dataSource->ext_clock = &player->is->extclk;

    auto decoder_ctx = new DecoderContext;
    player->dataSource->decoder_ctx = decoder_ctx;


    auto audio_render = new AudioRender;
    audio_render->audio_clock = &player->is->audclk;
    audio_render->ext_clock = &player->is->extclk;
    audio_render->sample_queue = new FrameQueue;
    decoder_ctx->audio_decoder = new Decoder;
    decoder_ctx->audio_render = audio_render;
    audio_render->sample_queue->Init(&player->is->audioq, SAMPLE_QUEUE_SIZE, 1);

    return 0;
}

void ffplayer_free_player(CPlayer *player) {
#ifdef _FLUTTER
    flutter_on_pre_player_free(player);
    ffp_detach_video_render_flutter(player);
#endif
    av_log(nullptr, AV_LOG_INFO, "free play, close stream %p \n", player);
    stream_close(player);
}

void ffplayer_global_init(void *arg) {
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_INFO);
    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        av_log(nullptr, AV_LOG_ERROR, "SDL fails to initialize audio subsystem!\n%s", SDL_GetError());
    else
        av_log(nullptr, AV_LOG_DEBUG, "SDL Audio was initialized fine!\n");

    flush_pkt = new AVPacket;
    av_init_packet(flush_pkt);
    flush_pkt->data = (uint8_t *) &flush_pkt;

#ifdef _FLUTTER
    assert(arg);
    Dart_InitializeApiDL(arg);
    flutter_free_all_player([](CPlayer *player) {
        av_log(nullptr, AV_LOG_INFO, "free play, close stream %p by flutter global \n", player);
        stream_close(player);
    });
#endif
}


void ffp_set_message_callback(CPlayer *player, void (*callback)(CPlayer *, int32_t, int64_t, int64_t)) {
    player->on_message = callback;
}

CPlayer *ffp_create_player(FFPlayerConfiguration *config) {
    CPlayer *player = ffplayer_alloc_player();
    if (!player) {
        return nullptr;
    }
    player->audio_disable = config->audio_disable;
    player->video_disable = config->video_disable;
    player->subtitle_disable = config->subtitle_disable;
    player->seek_by_bytes = config->seek_by_bytes;
    player->show_status = config->show_status;
    player->start_time = config->start_time;
    player->loop = config->loop;

    return player;
}

void ffp_refresh_texture(CPlayer *player, void(*on_locked)(FFP_VideoRenderContext *video_render_ctx)) {
    CHECK_PLAYER(player);
    auto render_ctx = &player->video_render_ctx;
    if (!render_ctx->render_attached) {
        return;
    }
    render_ctx->DrawFrame(player);
}

void ffp_attach_video_render(CPlayer *player, FFP_VideoRenderCallback *render_callback) {
    CHECK_PLAYER(player);
    if (player->video_render_ctx.render_attached) {
        av_log(nullptr, AV_LOG_FATAL, "video_render_already attached.\n");
        return;
    }
    if (render_callback == nullptr) {
        av_log(nullptr, AV_LOG_ERROR, "can not attach null render_callback.\n");
        return;
    }
    auto render_ctx = &player->video_render_ctx;
    render_ctx->render_callback_ = render_callback;
    if (render_ctx->Start(player)) {
        render_ctx->render_attached = true;
    }
}

double ffp_get_video_aspect_ratio(CPlayer *player) {
    CHECK_PLAYER_WITH_RETURN(player, -1);
    auto render_ctx = &player->video_render_ctx;
    if (!render_ctx->first_video_frame_loaded) {
        return 0;
    }
    if (render_ctx->frame_height == 0) {
        return 0;
    }
    return ((double) render_ctx->frame_width) / render_ctx->frame_height;
}


#ifdef _FLUTTER

int64_t ffp_attach_video_render_flutter(CPlayer *player) {
    int64_t texture_id = flutter_attach_video_render(player);
    auto render_ctx = &player->video_render_ctx;
    if (render_ctx->render_callback_ && !render_ctx->render_thread_) {
        start_video_render(player);
    }
    return texture_id;
}

void ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port) {
    player->message_send_port = send_port;
}

void ffp_detach_video_render_flutter(CPlayer *player) {
    CHECK_PLAYER(player);
    flutter_detach_video_render(player);
}
#endif // _FLUTTER