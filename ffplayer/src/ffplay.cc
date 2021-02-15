﻿// ffplayer.cpp: 定义应用程序的入口点。
//

#include <csignal>
#include <iostream>
#include <cstdint>
#include <utility>

#include "ffp_utils.h"
#include "ffp_player.h"
#include "ffp_frame_queue.h"

extern "C" {
#include "SDL2/SDL.h"
#include "libavutil/avutil.h"
#include "libavutil/time.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/avstring.h"
}

using namespace std;

#define FF_DRAW_EVENT    (SDL_USEREVENT + 2)
#define FF_MSG_EVENT    (SDL_USEREVENT + 3)
/* Step size for volume control*/
#define SDL_VOLUME_STEP (10)

#define CURSOR_HIDE_DELAY 1000000

static SDL_Window *window;

static float seek_interval = 10;
static int exit_on_keydown;
static int exit_on_mousedown;
static int borderless;
static int alwaysontop;
static int64_t cursor_last_shown;
static int cursor_hidden;

static int is_full_screen;

static const char *window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

static unsigned sws_flags = SWS_BICUBIC;

static const struct TextureFormatEntry {
  enum AVPixelFormat format;
  int texture_fmt;
} sdl_texture_format_map[] = {
    {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
    {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
    {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
    {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
    {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
    {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
    {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
    {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
    {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888},
    {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888},
    {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
    {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
    {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
    {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
    {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
    {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
    {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
    {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
    {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
    {AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN},
};

struct VideoRenderData {
  std::shared_ptr<SDL_Renderer> renderer;
  SDL_Texture *texture = nullptr;
  SDL_Texture *sub_texture = nullptr;
  struct SwsContext *img_convert_ctx = nullptr;
 public:

  explicit VideoRenderData(std::shared_ptr<SDL_Renderer> renderer_) : renderer(std::move(renderer_)) {

  }

  ~VideoRenderData() {
    if (texture) {
      SDL_DestroyTexture(texture);
    }
    if (sub_texture) {
      SDL_DestroyTexture(sub_texture);
    }
    sws_freeContext(img_convert_ctx);
  }

};

struct MessageData {
  CPlayer *player = nullptr;
  int32_t what = 0;
  int64_t arg1 = 0;
  int64_t arg2 = 0;

};

static void on_message(CPlayer *player, int what, int64_t arg1, int64_t arg2);

static void sigterm_handler(int sig) {
  exit(123);
}

static void do_exit(CPlayer *player) {
  delete player;
  if (window)
    SDL_DestroyWindow(window);

  SDL_Quit();
  av_log(nullptr, AV_LOG_QUIET, "%s", "");
  exit(0);
}

static void toggle_full_screen() {
  is_full_screen = !is_full_screen;
  SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void step_to_next_frame(CPlayer *player) {
  /* if the stream is paused unpause it, then step */
  if (player->IsPaused()) {
    player->TogglePause();
  }
//    player->is->step = 1;
}

static void refresh_loop_wait_event(SDL_Event *event) {
  SDL_PumpEvents();
  while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden = 1;
    }
    SDL_PumpEvents();
  }
}

/* handle an event sent by the GUI */
static void event_loop(CPlayer *player) {
  SDL_Event event;
  double incr;

  for (;;) {
    double x;
    refresh_loop_wait_event(&event);
#if WIN32
    player->DrawFrame();
#endif
    switch (event.type) {
      case SDL_KEYDOWN:
        if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
          do_exit(player);
          break;
        }
        // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
        if (false) // TODO check if player render start
          continue;
        switch (event.key.keysym.sym) {
          case SDLK_f:toggle_full_screen();
//                        is->force_refresh = 1;
            break;
          case SDLK_p:
          case SDLK_SPACE:player->TogglePause();
            break;
          case SDLK_m:player->SetMute(!player->IsMuted());
            break;
          case SDLK_KP_MULTIPLY:
          case SDLK_0:player->SetVolume(player->GetVolume() + SDL_VOLUME_STEP);
            break;
          case SDLK_KP_DIVIDE:
          case SDLK_9:player->SetVolume(player->GetVolume() - SDL_VOLUME_STEP);
            break;
          case SDLK_s:  // S: Step to next frame
            step_to_next_frame(player);
            break;
          case SDLK_a:break;
          case SDLK_v:break;
          case SDLK_c:break;
          case SDLK_t:break;
          case SDLK_w:break;
          case SDLK_PAGEUP:
            if (player->GetChapterCount() <= 1) {
              incr = 600.0;
              goto do_seek;
            }
            player->SeekToChapter(player->GetChapterCount() + 1);
            break;
          case SDLK_PAGEDOWN:
            if (player->GetChapterCount() <= 1) {
              incr = -600.0;
              goto do_seek;
            }
            player->SeekToChapter(player->GetChapterCount() - 1);
            break;
          case SDLK_LEFT:incr = -seek_interval;
            goto do_seek;
          case SDLK_RIGHT:incr = seek_interval;
            goto do_seek;
          case SDLK_UP:incr = 60.0;
            goto do_seek;
          case SDLK_DOWN:incr = -60.0;
          do_seek:
            printf("ffplayer_seek_to_position from: %0.2f , to: %0.2f .\n",
                   player->GetCurrentPosition(), player->GetCurrentPosition() + incr);
            player->Seek(player->GetCurrentPosition() + incr);
            break;
          default:break;
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (exit_on_mousedown) {
          do_exit(player);
          break;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
          static int64_t last_mouse_left_click = 0;
          if (av_gettime_relative() - last_mouse_left_click <= 500000) {
            toggle_full_screen();
//                        is->force_refresh = 1;
            last_mouse_left_click = 0;
          } else {
            last_mouse_left_click = av_gettime_relative();
          }
        }
      case SDL_MOUSEMOTION:
        if (cursor_hidden) {
          SDL_ShowCursor(1);
          cursor_hidden = 0;
        }
        cursor_last_shown = av_gettime_relative();
        if (event.type == SDL_MOUSEBUTTONDOWN) {
          if (event.button.button != SDL_BUTTON_RIGHT)
            break;
          x = event.button.x;
        } else {
          if (!(event.motion.state & SDL_BUTTON_RMASK))
            break;
          x = event.motion.x;
        }
        {
          double dest = (x / screen_width) * player->GetDuration();
          player->Seek(dest);
        }
        break;
      case SDL_WINDOWEVENT:
        switch (event.window.event) {
          case SDL_WINDOWEVENT_SIZE_CHANGED: {
            screen_width = event.window.data1;
            screen_height = event.window.data2;
            printf("SDL_WINDOWEVENT_SIZE_CHANGED: %d, %d \n", event.window.data1, event.window.data2);
//                        [](VideoRender *context) {
//                            auto render_data = static_cast<VideoRenderData *>(context->render_callback_->opacity);
//                            SDL_DestroyTexture(render_data->texture);
//                            render_data->texture = nullptr;
//                        }
            player->DrawFrame();
          }
          case SDL_WINDOWEVENT_EXPOSED:
//                        is->force_refresh = 1;
            break;
        }
        break;
      case SDL_QUIT:do_exit(player);
        return;
      case FF_DRAW_EVENT: {
//                auto *video_render_ctx = static_cast<VideoRenderData *>(event.user.data1);
//                SDL_RenderPresent(video_render_ctx->renderer);
//                cout << "draw delay time: "
//                     << (SDL_GetTicks() - event.user.timestamp) << " milliseconds"
//                     << endl;
      }
        break;
      case FF_MSG_EVENT: {
        auto *msg = static_cast<MessageData *>(event.user.data1);
        on_message(msg->player, msg->what, msg->arg1, msg->arg2);
        delete msg;
      }
        break;
      default:break;
    }
  }
}

static void set_default_window_size(int width, int height) {
  SDL_Rect rect;
  int max_width = screen_width ? screen_width : INT_MAX;
  int max_height = screen_height ? screen_height : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX)
    max_height = height;
  AVRational rational = {1, 1};
  calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, rational);
  default_width = rect.w;
  default_height = rect.h;
}

static void on_message(CPlayer *player, int what, int64_t arg1, int64_t arg2) {
//    av_log(nullptr, AV_LOG_INFO, "on msg(%d): arg1 = %ld, arg2 = %ld \n", what, arg1, arg2);
  switch (what) {
    case FFP_MSG_VIDEO_FRAME_LOADED:set_default_window_size(arg1, arg2);
      int w, h;
      w = screen_width ? screen_width : default_width;
      h = screen_height ? screen_height : default_height;

      if (!window_title)
        window_title = player->GetUrl();
      SDL_SetWindowTitle(window, window_title);

      printf("set_default_window_size : %d , %d \n", w, h);
      SDL_SetWindowSize(window, w, h);
      SDL_SetWindowPosition(window, screen_left, screen_top);
      if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
      SDL_ShowWindow(window);
      break;
    case FFP_MSG_PLAYBACK_STATE_CHANGED:printf("FFP_MSG_PLAYBACK_STATE_CHANGED : %ld \n", arg1);
      break;
    case FFP_MSG_BUFFERING_TIME_UPDATE:
      printf("FFP_MSG_BUFFERING_TIME_UPDATE: %f.  %f:%f \n", arg1 / 1000.0, player->GetCurrentPosition(),
             player->GetDuration());
      break;
    case FFP_MSG_AV_METADATA_LOADED: {
      const char *title = player->GetMetadataDict("title");
      if (!window_title && title)
        window_title = av_asprintf("%s - %s", title, player->GetUrl());
      break;
    }
    default:break;
  }
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame) {
#if SDL_VERSION_ATLEAST(2, 0, 8)
  SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
  if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 ||
      frame->format == AV_PIX_FMT_UYVY422)) {
    if (frame->color_range == AVCOL_RANGE_JPEG)
      mode = SDL_YUV_CONVERSION_JPEG;
    else if (frame->colorspace == AVCOL_SPC_BT709)
      mode = SDL_YUV_CONVERSION_BT709;
    else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
        frame->colorspace == AVCOL_SPC_SMPTE240M)
      mode = SDL_YUV_CONVERSION_BT601;
  }
  SDL_SetYUVConversionMode(mode);
#endif
}

static int upload_texture(VideoRenderData *render_data, SDL_Texture **tex, AVFrame *frame,
                          struct SwsContext **img_convert_ctx);

int main(int argc, char *argv[]) {
  char *input_file = argv[1];
  if (!input_file) {
#ifdef _WIN32
    static const char *input_filename = "C:/Users/boyan/Desktop/mojito.mp4";
#elif __LINUX__
    static const char *input_filename = "/home/boyan/mojito.mp4";
#else
    static const char *input_filename = nullptr;
#endif
    input_file = (char *) input_filename;
  }
  if (!input_file) {
    av_log(nullptr, AV_LOG_FATAL, "An input file must be specified\n");
    exit(1);
  }

//    ffplayer_global_init(nullptr);
  CPlayer::GlobalInit();

  signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
  signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  PlayerConfiguration config;

  auto *player = new CPlayer;
  player->start_configuration = config;
  player->SetVolume(100);

  uint32_t flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (config.audio_disable)
    flags &= ~SDL_INIT_AUDIO;
  else {
    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
      SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
  }
  if (config.video_disable)
    flags &= ~SDL_INIT_VIDEO;
  if (SDL_Init(flags)) {
    av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
    av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
    exit(1);
  }
  SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

  std::shared_ptr<SDL_Renderer> renderer;

  if (!config.video_disable) {
    SDL_RendererInfo renderer_info = {nullptr};
    uint32_t window_flags = SDL_WINDOW_HIDDEN;
    if (alwaysontop)
#if SDL_VERSION_ATLEAST(2, 0, 5)
      window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
    av_log(nullptr, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
    if (borderless)
      window_flags |= SDL_WINDOW_BORDERLESS;
    else
      window_flags |= SDL_WINDOW_RESIZABLE;
    window = SDL_CreateWindow("ffplay", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                              default_height, window_flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
      renderer = std::shared_ptr<SDL_Renderer>(
          SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
          [](SDL_Renderer *ptr) {
            SDL_DestroyRenderer(ptr);
          });
      if (!renderer) {
        av_log(nullptr, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
               SDL_GetError());
        renderer = std::shared_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, 0), [](SDL_Renderer *ptr) {
          SDL_DestroyRenderer(ptr);
        });
      }
      if (renderer) {
        if (!SDL_GetRendererInfo(renderer.get(), &renderer_info))
          av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
      }
    }
    if (!window || !renderer || !renderer_info.num_texture_formats) {
      av_log(nullptr, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
      do_exit(nullptr);
    }

    auto render_data = new VideoRenderData(renderer);
    player->SetVideoRender([&render_data](Frame *vp) {
      SDL_SetRenderDrawColor(render_data->renderer.get(), 0, 0, 0, 255);
      SDL_RenderClear(render_data->renderer.get());
      SDL_Rect rect{};
      calculate_display_rect(&rect, 0, 0, screen_width,
                             screen_height, vp->width, vp->height, vp->sar);
      if (!vp->uploaded) {
        if (upload_texture(render_data, &render_data->texture, vp->frame,
                           &render_data->img_convert_ctx) < 0)
          return;
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
      }
      set_sdl_yuv_conversion_mode(vp->frame);
      SDL_RenderCopyEx(render_data->renderer.get(), render_data->texture, nullptr, &rect, 0, nullptr,
                       vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
      set_sdl_yuv_conversion_mode(nullptr);

      {
//                SDL_Event event;
//                event.type = FF_DRAW_EVENT;
//                event.user.data1 = video_render_ctx->render_callback_->opacity;
//                {
//                    // clean existed scheduled frame.
//                    SDL_FilterEvents([](void *data, SDL_Event *event) -> int {
//                        if (event->type == FF_DRAW_EVENT) {
//                            return 0;
//                        } else {
//                            return 1;
//                        }
//                    }, nullptr);
//                }
//                SDL_PushEvent(&event);
        SDL_RenderPresent(render_data->renderer.get());
//                show_status(player);
      }

    });
  }
  player->SetMessageHandleCallback([player](int32_t what, int64_t arg1, int64_t arg2) {
    SDL_Event event;
    event.type = FF_MSG_EVENT;
    auto *data = new MessageData;
    event.user.data1 = data;
    data->player = player;
    data->what = what;
    data->arg1 = arg1;
    data->arg2 = arg2;
    SDL_PushEvent(&event);
  });

  if (player->OpenDataSource(input_file) < 0) {
    av_log(nullptr, AV_LOG_FATAL, "failed to open file\n");
    do_exit(nullptr);
  }

  if (player->IsPaused()) {
    // perform play when start.
    player->TogglePause();
  }
  event_loop(player);

  return 0;
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
  int i;
  *sdl_blendmode = SDL_BLENDMODE_NONE;
  *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
  if (format == AV_PIX_FMT_RGB32 ||
      format == AV_PIX_FMT_RGB32_1 ||
      format == AV_PIX_FMT_BGR32 ||
      format == AV_PIX_FMT_BGR32_1)
    *sdl_blendmode = SDL_BLENDMODE_BLEND;
  for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
    if (format == sdl_texture_format_map[i].format) {
      *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
      return;
    }
  }
}

static int
realloc_texture(SDL_Renderer *renderer, SDL_Texture **texture, Uint32 new_format, int new_width, int new_height,
                SDL_BlendMode blendmode, int init_texture) {
  Uint32 format;
  int access, w, h;
  if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h ||
      new_format != format) {
    void *pixels;
    int pitch;
    if (*texture)
      SDL_DestroyTexture(*texture);
    if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width,
                                       new_height)))
      return -1;
    if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
      return -1;
    if (init_texture) {
      if (SDL_LockTexture(*texture, nullptr, &pixels, &pitch) < 0)
        return -1;
      memset(pixels, 0, pitch * new_height);
      SDL_UnlockTexture(*texture);
    }
    av_log(nullptr, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height,
           SDL_GetPixelFormatName(new_format));
  }
  return 0;
}

static int upload_texture(VideoRenderData *render_data, SDL_Texture **tex, AVFrame *frame,
                          struct SwsContext **img_convert_ctx) {
  int ret = 0;
  Uint32 sdl_pix_fmt;
  SDL_BlendMode sdl_blendmode;
  get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
  if (realloc_texture(render_data->renderer.get(), tex,
                      sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,
                      frame->width, frame->height, sdl_blendmode, 0) < 0)
    return -1;
  switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_UNKNOWN:
      /* This should only happen if we are not using avfilter... */
      *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                                              frame->width, frame->height, AVPixelFormat(frame->format),
                                              frame->width, frame->height, AVPixelFormat::AV_PIX_FMT_BGRA,
                                              sws_flags, nullptr, nullptr, nullptr);

      if (*img_convert_ctx != nullptr) {
        uint8_t *pixels[4];
        int pitch[4];
        if (!SDL_LockTexture(*tex, nullptr, (void **) pixels, pitch)) {
          sws_scale(*img_convert_ctx, (const uint8_t *const *) frame->data, frame->linesize,
                    0, frame->height, pixels, pitch);
          SDL_UnlockTexture(*tex);
        }
      } else {
        av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
        ret = -1;
      }
      break;
    case SDL_PIXELFORMAT_IYUV:
      if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
        ret = SDL_UpdateYUVTexture(*tex, nullptr, frame->data[0], frame->linesize[0],
                                   frame->data[1], frame->linesize[1],
                                   frame->data[2], frame->linesize[2]);
      } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
        ret = SDL_UpdateYUVTexture(*tex, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                   -frame->linesize[0],
                                   frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                   -frame->linesize[1],
                                   frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                   -frame->linesize[2]);
      } else {
        av_log(nullptr, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
        return -1;
      }
      break;
    default:
      if (frame->linesize[0] < 0) {
        ret = SDL_UpdateTexture(*tex, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                -frame->linesize[0]);
      } else {
        ret = SDL_UpdateTexture(*tex, nullptr, frame->data[0], frame->linesize[0]);
      }
      break;
  }
  return ret;
}
