#include "external_media_texture.h"
#include "external_video_renderer_sink.h"

#include "base/logging.h"

extern void register_external_texture_factory(FlutterTextureAdapterFactory factory) {
  DCHECK(factory) << "can not register flutter texture factory with nullptr";
  DCHECK(!media::ExternalVideoRendererSink::factory_) << "can not register more than once";
  media::ExternalVideoRendererSink::factory_ = factory;
}

