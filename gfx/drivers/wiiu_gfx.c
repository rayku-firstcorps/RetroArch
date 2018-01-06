/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2014-2017 - Ali Bouhlel
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <wiiu/os.h>
#include <wiiu/gx2.h>
#include <formats/image.h>

#include "../../driver.h"
#include "../../configuration.h"
#include "../../verbosity.h"

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "gfx/common/gx2_common.h"
#include "gfx/video_shader_parse.h"
#include "gfx/drivers_shader/slang_preprocess.h"
#include "system/memory.h"

#include "wiiu_dbg.h"

#include "../font_driver.h"


static const wiiu_render_mode_t wiiu_render_mode_map[] =
{
   {0},                                         /* GX2_TV_SCAN_MODE_NONE  */
   {854,  480,  GX2_TV_RENDER_MODE_WIDE_480P},  /* GX2_TV_SCAN_MODE_576I  */
   {854,  480,  GX2_TV_RENDER_MODE_WIDE_480P},  /* GX2_TV_SCAN_MODE_480I  */
   {854,  480,  GX2_TV_RENDER_MODE_WIDE_480P},  /* GX2_TV_SCAN_MODE_480P  */
   {1280, 720,  GX2_TV_RENDER_MODE_WIDE_720P},  /* GX2_TV_SCAN_MODE_720P  */
   {0},                                         /* GX2_TV_SCAN_MODE_unk   */
   {1920, 1080, GX2_TV_RENDER_MODE_WIDE_1080P}, /* GX2_TV_SCAN_MODE_1080I */
   {1920, 1080, GX2_TV_RENDER_MODE_WIDE_1080P}  /* GX2_TV_SCAN_MODE_1080P */
};

static void wiiu_set_tex_coords(frame_vertex_t *v, GX2Texture *texture, float u0, float v0, float u1, float v1,
                                unsigned rotation)
{
   v[0].coord.u = u0 / texture->surface.width;
   v[0].coord.v = v0 / texture->surface.height;
   v[1].coord.u = u1 / texture->surface.width;
   v[1].coord.v = v0 / texture->surface.height;
   v[2].coord.u = u1 / texture->surface.width;
   v[2].coord.v = v1 / texture->surface.height;
   v[3].coord.u = u0 / texture->surface.width;
   v[3].coord.v = v1 / texture->surface.height;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, v, 4 * sizeof(*v));
}

static void wiiu_set_projection(wiiu_video_t *wiiu)
{
   math_matrix_4x4 proj, rot;
   matrix_4x4_ortho(proj, 0, 1, 1, 0, -1, 1);
   matrix_4x4_rotate_z(rot, wiiu->rotation * -M_PI_2);
   matrix_4x4_multiply((*wiiu->ubo_mvp), rot, proj);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_UNIFORM_BLOCK, wiiu->ubo_mvp, sizeof(*wiiu->ubo_mvp));
}

static void wiiu_gfx_update_viewport(wiiu_video_t *wiiu)
{
   unsigned width, height;
   int x                    = 0;
   int y                    = 0;
   unsigned viewport_width  = wiiu->color_buffer.surface.width;
   unsigned viewport_height = wiiu->color_buffer.surface.height;
   float device_aspect      = (float)viewport_width / viewport_height;
   settings_t *settings     = config_get_ptr();

   video_driver_get_size(&width, &height);

   if (settings->bools.video_scale_integer)
   {
      video_viewport_get_scaled_integer(&wiiu->vp,
                                        viewport_width, viewport_height,
                                        video_driver_get_aspect_ratio(), wiiu->keep_aspect);
      viewport_width  = wiiu->vp.width;
      viewport_height = wiiu->vp.height;
   }
   else if (wiiu->keep_aspect)
   {
      float desired_aspect = video_driver_get_aspect_ratio();

#if defined(HAVE_MENU)

      if (settings->uints.video_aspect_ratio_idx == ASPECT_RATIO_CUSTOM)
      {
         const struct video_viewport *custom = video_viewport_get_custom();

         /* Vulkan has top-left origin viewport. */
         x               = custom->x;
         y               = custom->y;
         viewport_width  = custom->width;
         viewport_height = custom->height;
      }
      else
#endif
      {
         float delta;

         if (fabsf(device_aspect - desired_aspect) < 0.0001f)
         {
            /* If the aspect ratios of screen and desired aspect
             * ratio are sufficiently equal (floating point stuff),
             * assume they are actually equal.
             */
         }
         else if (device_aspect > desired_aspect)
         {
            delta          = (desired_aspect / device_aspect - 1.0f)
                             / 2.0f + 0.5f;
            x              = (int)roundf(viewport_width * (0.5f - delta));
            viewport_width = (unsigned)roundf(2.0f * viewport_width * delta);
         }
         else
         {
            delta           = (device_aspect / desired_aspect - 1.0f)
                              / 2.0f + 0.5f;
            y               = (int)roundf(viewport_height * (0.5f - delta));
            viewport_height = (unsigned)roundf(2.0f * viewport_height * delta);
         }
      }

      wiiu->vp.x      = x;
      wiiu->vp.y      = y;
      wiiu->vp.width  = viewport_width;
      wiiu->vp.height = viewport_height;
   }
   else
   {
      wiiu->vp.x      = 0;
      wiiu->vp.y      = 0;
      wiiu->vp.width  = viewport_width;
      wiiu->vp.height = viewport_height;
   }

   wiiu_set_projection(wiiu);
}

static void wiiu_gfx_set_aspect_ratio(void *data, unsigned aspect_ratio_idx)
{
   wiiu_video_t *wiiu = (wiiu_video_t *)data;

   switch (aspect_ratio_idx)
   {
   case ASPECT_RATIO_SQUARE:
      video_driver_set_viewport_square_pixel();
      break;

   case ASPECT_RATIO_CORE:
      video_driver_set_viewport_core();
      break;

   case ASPECT_RATIO_CONFIG:
      video_driver_set_viewport_config();
      break;

   default:
      break;
   }

   video_driver_set_aspect_ratio_value(aspectratio_lut[aspect_ratio_idx].value);

   if (!wiiu)
      return;

   wiiu->keep_aspect = true;
   wiiu->should_resize = true;
}

static void *wiiu_gfx_init(const video_info_t *video,
                           const input_driver_t **input, void **input_data)
{
   float refresh_rate = 60.0f / 1.001f;
   u32 size           = 0;
   u32 tmp            = 0;
   void *wiiuinput    = NULL;
   wiiu_video_t *wiiu = calloc(1, sizeof(*wiiu));

   if (!wiiu)
      return NULL;

   *input             = NULL;
   *input_data        = NULL;

   if (input && input_data)
   {
      settings_t *settings = config_get_ptr();
      wiiuinput            = input_wiiu.init(settings->arrays.input_joypad_driver);
      *input               = wiiuinput ? &input_wiiu : NULL;
      *input_data          = wiiuinput;
   }

   /* video initialize */
   wiiu->cmd_buffer = MEM2_alloc(0x400000, 0x40);
   u32 init_attributes[] =
   {
      GX2_INIT_CMD_BUF_BASE, (u32)wiiu->cmd_buffer,
      GX2_INIT_CMD_BUF_POOL_SIZE, 0x400000,
      GX2_INIT_ARGC, 0,
      GX2_INIT_ARGV, 0,
      GX2_INIT_END
   };
   GX2Init(init_attributes);

   /* setup scanbuffers */
   wiiu->render_mode = wiiu_render_mode_map[GX2GetSystemTVScanMode()];
//   wiiu->render_mode = wiiu_render_mode_map[GX2_TV_SCAN_MODE_480P];
   GX2CalcTVSize(wiiu->render_mode.mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                 GX2_BUFFERING_MODE_DOUBLE, &size, &tmp);

   wiiu->tv_scan_buffer = MEMBucket_alloc(size, GX2_SCAN_BUFFER_ALIGNMENT);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU, wiiu->tv_scan_buffer, size);
   GX2SetTVBuffer(wiiu->tv_scan_buffer, size, wiiu->render_mode.mode,
                  GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                  GX2_BUFFERING_MODE_DOUBLE);

   GX2CalcDRCSize(GX2_DRC_RENDER_MODE_SINGLE, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                  GX2_BUFFERING_MODE_DOUBLE, &size,
                  &tmp);

   wiiu->drc_scan_buffer = MEMBucket_alloc(size, GX2_SCAN_BUFFER_ALIGNMENT);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU, wiiu->drc_scan_buffer, size);
   GX2SetDRCBuffer(wiiu->drc_scan_buffer, size, GX2_DRC_RENDER_MODE_SINGLE,
                   GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                   GX2_BUFFERING_MODE_DOUBLE);

   memset(&wiiu->color_buffer, 0, sizeof(GX2ColorBuffer));

   wiiu->color_buffer.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
   wiiu->color_buffer.surface.width     = wiiu->render_mode.width;
   wiiu->color_buffer.surface.height    = wiiu->render_mode.height;
   wiiu->color_buffer.surface.depth     = 1;
   wiiu->color_buffer.surface.mipLevels = 1;
   wiiu->color_buffer.surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   wiiu->color_buffer.surface.use       = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
   wiiu->color_buffer.viewNumSlices     = 1;

   GX2CalcSurfaceSizeAndAlignment(&wiiu->color_buffer.surface);
   GX2InitColorBufferRegs(&wiiu->color_buffer);

   wiiu->color_buffer.surface.image = MEM1_alloc(wiiu->color_buffer.surface.imageSize,
                                      wiiu->color_buffer.surface.alignment);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU, wiiu->color_buffer.surface.image,
                 wiiu->color_buffer.surface.imageSize);

   wiiu->ctx_state = (GX2ContextState *)MEM2_alloc(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
   GX2SetupContextStateEx(wiiu->ctx_state, GX2_TRUE);

   GX2SetContextState(wiiu->ctx_state);
   GX2SetColorBuffer(&wiiu->color_buffer, GX2_RENDER_TARGET_0);
   GX2SetViewport(0.0f, 0.0f, wiiu->color_buffer.surface.width, wiiu->color_buffer.surface.height, 0.0f, 1.0f);
   GX2SetScissor(0, 0, wiiu->color_buffer.surface.width, wiiu->color_buffer.surface.height);
   GX2SetDepthOnlyControl(GX2_DISABLE, GX2_DISABLE, GX2_COMPARE_FUNC_ALWAYS);
   GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, GX2_DISABLE, GX2_ENABLE);
   GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                      GX2_BLEND_COMBINE_MODE_ADD,
                      GX2_ENABLE,          GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD);
   GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, GX2_DISABLE, GX2_DISABLE);

   GX2InitShader(&frame_shader);
   GX2InitShader(&sprite_shader);
   GX2SetShader(&frame_shader);

   wiiu->ubo_vp  = MEM1_alloc(sizeof(*wiiu->ubo_vp), GX2_UNIFORM_BLOCK_ALIGNMENT);
   wiiu->ubo_vp->width = wiiu->color_buffer.surface.width;
   wiiu->ubo_vp->height = wiiu->color_buffer.surface.height;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_UNIFORM_BLOCK, wiiu->ubo_vp, sizeof(*wiiu->ubo_vp));

   wiiu->ubo_tex = MEM1_alloc(sizeof(*wiiu->ubo_tex), GX2_UNIFORM_BLOCK_ALIGNMENT);
   wiiu->ubo_tex->width = 1.0;
   wiiu->ubo_tex->height = 1.0;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_UNIFORM_BLOCK, wiiu->ubo_tex, sizeof(*wiiu->ubo_tex));

   wiiu->ubo_mvp = MEM1_alloc(sizeof(*wiiu->ubo_mvp), GX2_UNIFORM_BLOCK_ALIGNMENT);
   wiiu_set_projection(wiiu);

   wiiu->input_ring_buffer_size = GX2CalcGeometryShaderInputRingBufferSize(sprite_shader.vs.ringItemSize);
   wiiu->output_ring_buffer_size = GX2CalcGeometryShaderOutputRingBufferSize(sprite_shader.gs.ringItemSize);
   wiiu->input_ring_buffer = MEM1_alloc(wiiu->input_ring_buffer_size, 0x1000);
   wiiu->output_ring_buffer = MEM1_alloc(wiiu->output_ring_buffer_size, 0x1000);

   /* init menu texture */
   memset(&wiiu->menu.texture, 0, sizeof(GX2Texture));
   wiiu->menu.texture.surface.width    = 512;
   wiiu->menu.texture.surface.height   = 512;
   wiiu->menu.texture.surface.depth    = 1;
   wiiu->menu.texture.surface.dim      = GX2_SURFACE_DIM_TEXTURE_2D;
   wiiu->menu.texture.surface.format   = GX2_SURFACE_FORMAT_UNORM_R4_G4_B4_A4;
   wiiu->menu.texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
   wiiu->menu.texture.viewNumSlices    = 1;
   wiiu->menu.texture.compMap          = GX2_COMP_SEL(_A, _R, _G, _B);
   GX2CalcSurfaceSizeAndAlignment(&wiiu->menu.texture.surface);
   GX2InitTextureRegs(&wiiu->menu.texture);

   wiiu->menu.texture.surface.image = MEM2_alloc(wiiu->menu.texture.surface.imageSize,
                                      wiiu->menu.texture.surface.alignment);

   memset(wiiu->menu.texture.surface.image, 0x0, wiiu->menu.texture.surface.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, wiiu->menu.texture.surface.image,
                 wiiu->menu.texture.surface.imageSize);

   wiiu->v = MEM2_alloc(4 * sizeof(*wiiu->v), GX2_VERTEX_BUFFER_ALIGNMENT);

   wiiu->v[0].pos.x = 0.0f;
   wiiu->v[0].pos.y = 0.0f;
   wiiu->v[1].pos.x = 1.0f;
   wiiu->v[1].pos.y = 0.0f;
   wiiu->v[2].pos.x = 1.0f;
   wiiu->v[2].pos.y = 1.0f;
   wiiu->v[3].pos.x = 0.0f;
   wiiu->v[3].pos.y = 1.0f;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, wiiu->v, 4 * sizeof(*wiiu->v));

   wiiu_set_tex_coords(wiiu->v, &wiiu->texture, 0, 0,
                       wiiu->texture.surface.width, wiiu->texture.surface.height, wiiu->rotation);

   GX2SetAttribBuffer(0, 4 * sizeof(*wiiu->v), sizeof(*wiiu->v), wiiu->v);

   wiiu->menu.v = MEM2_alloc(4 * sizeof(*wiiu->menu.v), GX2_VERTEX_BUFFER_ALIGNMENT);
   wiiu->menu.v->pos.x = 0.0f;
   wiiu->menu.v->pos.y = 0.0f;
   wiiu->menu.v->pos.width = wiiu->color_buffer.surface.width;
   wiiu->menu.v->pos.height = wiiu->color_buffer.surface.height;
   wiiu->menu.v->coord.u = 0.0f;
   wiiu->menu.v->coord.v = 0.0f;
   wiiu->menu.v->coord.width = 1.0f;
   wiiu->menu.v->coord.height = 1.0f;
   wiiu->menu.v->color = 0xFFFFFF80;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, wiiu->menu.v, 4 * sizeof(*wiiu->menu.v));

   wiiu->vertex_cache.size       = 0x1000;
   wiiu->vertex_cache.current    = 0;
   wiiu->vertex_cache.v  = MEM2_alloc(wiiu->vertex_cache.size
                                      * sizeof(*wiiu->vertex_cache.v), GX2_VERTEX_BUFFER_ALIGNMENT);

   /* Initialize samplers */
   GX2InitSampler(&wiiu->sampler_nearest, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
   GX2InitSampler(&wiiu->sampler_linear, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

   /* set Texture and Sampler */
   GX2SetPixelTexture(&wiiu->texture, frame_shader.ps.samplerVars[0].location);
   GX2SetPixelSampler(&wiiu->sampler_linear, frame_shader.ps.samplerVars[0].location);

   /* clear leftover image */
   GX2ClearColor(&wiiu->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
   GX2CopyColorBufferToScanBuffer(&wiiu->color_buffer, GX2_SCAN_TARGET_DRC);
   GX2CopyColorBufferToScanBuffer(&wiiu->color_buffer, GX2_SCAN_TARGET_TV);

   GX2SwapScanBuffers();
   GX2Flush();
   GX2WaitForVsync();

   GX2SetTVEnable(GX2_ENABLE);
   GX2SetDRCEnable(GX2_ENABLE);

   wiiu->keep_aspect    = true;
   wiiu->should_resize  = true;
   wiiu->smooth         = video->smooth;
   wiiu->vsync          = video->vsync;
   GX2SetSwapInterval(!!video->vsync);

   wiiu->vp.x           = 0;
   wiiu->vp.y           = 0;
   wiiu->vp.width       = wiiu->render_mode.width;
   wiiu->vp.height      = wiiu->render_mode.height;
   wiiu->vp.full_width  = wiiu->render_mode.width;
   wiiu->vp.full_height = wiiu->render_mode.height;
   video_driver_set_size(&wiiu->vp.width, &wiiu->vp.height);

   driver_ctl(RARCH_DRIVER_CTL_SET_REFRESH_RATE, &refresh_rate);

   font_driver_init_osd(wiiu, false,
                        video->is_threaded,
                        FONT_DRIVER_RENDER_WIIU);

   return wiiu;
}

#ifdef HAVE_OVERLAY
static void gx2_overlay_tex_geom(void *data, unsigned image,
                                 float x, float y, float w, float h)
{
   wiiu_video_t            *gx2 = (wiiu_video_t *)data;
   struct gx2_overlay_data *o = NULL;

   if (gx2)
      o = (struct gx2_overlay_data *)&gx2->overlay[image];

   if (!o)
      return;

   o->v.coord.u = x;
   o->v.coord.v = y;
   o->v.coord.width = w;
   o->v.coord.height = h;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &o->v, sizeof(o->v));
}

static void gx2_overlay_vertex_geom(void *data, unsigned image,
                                    float x, float y, float w, float h)
{
   wiiu_video_t            *gx2 = (wiiu_video_t *)data;
   struct gx2_overlay_data *o = NULL;


   if (gx2)
      o = (struct gx2_overlay_data *)&gx2->overlay[image];

   if (!o)
      return;

   o->v.pos.x = x * gx2->color_buffer.surface.width;
   o->v.pos.y = y * gx2->color_buffer.surface.height;
   o->v.pos.width = w * gx2->color_buffer.surface.width;
   o->v.pos.height = h * gx2->color_buffer.surface.height;

   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &o->v, sizeof(o->v));
}

static void gx2_free_overlay(wiiu_video_t *gx2)
{
   unsigned i;

   for (i = 0; i < gx2->overlays; i++)
      MEM2_free(gx2->overlay[i].tex.surface.image);

   free(gx2->overlay);
   gx2->overlay = NULL;
   gx2->overlays = 0;

}

static bool gx2_overlay_load(void *data,
                             const void *image_data, unsigned num_images)
{
   unsigned i, j;
   wiiu_video_t *gx2 = (wiiu_video_t *)data;
   const struct texture_image *images = (const struct texture_image *)image_data;

   gx2_free_overlay(gx2);
   gx2->overlay = (struct gx2_overlay_data *)calloc(num_images, sizeof(*gx2->overlay));

   if (!gx2->overlay)
      return false;

   gx2->overlays = num_images;

   for (i = 0; i < num_images; i++)
   {
      struct gx2_overlay_data *o = (struct gx2_overlay_data *)&gx2->overlay[i];

      //GX2Texture* o->tex = calloc(1, sizeof(GX2Texture));

      memset(&o->tex, 0, sizeof(GX2Texture));
      o->tex.surface.width    = images[i].width;
      o->tex.surface.height   = images[i].height;
      o->tex.surface.depth    = 1;
      o->tex.surface.dim      = GX2_SURFACE_DIM_TEXTURE_2D;
      o->tex.surface.format   = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
      o->tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
      o->tex.viewNumSlices    = 1;
      o->tex.compMap          = GX2_COMP_SEL(_G, _B, _A, _R);
      GX2CalcSurfaceSizeAndAlignment(&o->tex.surface);
      GX2InitTextureRegs(&o->tex);

      o->tex.surface.image = MEM2_alloc(o->tex.surface.imageSize,
                                        o->tex.surface.alignment);

      for (j = 0; (j < images[i].height) && (j < o->tex.surface.height); j++)
         memcpy((uint32_t *)o->tex.surface.image + (j * o->tex.surface.pitch),
                images[i].pixels + (j * images[i].width), images[i].width * sizeof(images[i].pixels));

      GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,  o->tex.surface.image,  o->tex.surface.imageSize);

      /* Default. Stretch to whole screen. */
      gx2_overlay_tex_geom(gx2, i, 0, 0, 1, 1);
      gx2_overlay_vertex_geom(gx2, i, 0, 0, 1, 1);
      gx2->overlay[i].alpha_mod = 1.0f;
      gx2->overlay[i].v.color = 0xFFFFFFFF;

      GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &o->v, sizeof(o->v));

   }

   return true;
}

static void gx2_overlay_enable(void *data, bool state)
{

   wiiu_video_t *gx2 = (wiiu_video_t *)data;
   gx2->overlay_enable = state;
}

static void gx2_overlay_full_screen(void *data, bool enable)
{
   wiiu_video_t *gx2 = (wiiu_video_t *)data;
   gx2->overlay_full_screen = enable;
}

static void gx2_overlay_set_alpha(void *data, unsigned image, float mod)
{
   wiiu_video_t *gx2 = (wiiu_video_t *)data;

   if (gx2)
   {
      gx2->overlay[image].alpha_mod = mod;
      gx2->overlay[image].v.color = COLOR_RGBA(0xFF, 0xFF, 0xFF, 0xFF * gx2->overlay[image].alpha_mod);
      GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &gx2->overlay[image].v, sizeof(gx2->overlay[image].v));
   }
}

static void gx2_render_overlay(void *data)
{
   unsigned i;

   wiiu_video_t *gx2 = (wiiu_video_t *)data;

   for (i = 0; i < gx2->overlays; i++)
   {

      GX2SetAttribBuffer(0, sizeof(gx2->overlay[i].v), sizeof(gx2->overlay[i].v), &gx2->overlay[i].v);

      GX2SetPixelTexture(&gx2->overlay[i].tex, sprite_shader.ps.samplerVars[0].location);
      GX2SetPixelSampler(&gx2->sampler_linear, sprite_shader.ps.samplerVars[0].location);

      GX2DrawEx(GX2_PRIMITIVE_MODE_POINTS, 1, 0, 1);

   }

}

static const video_overlay_interface_t gx2_overlay_interface =
{
   gx2_overlay_enable,
   gx2_overlay_load,
   gx2_overlay_tex_geom,
   gx2_overlay_vertex_geom,
   gx2_overlay_full_screen,
   gx2_overlay_set_alpha,
};

static void gx2_get_overlay_interface(void *data,
                                      const video_overlay_interface_t **iface)
{
   (void)data;
   *iface = &gx2_overlay_interface;
}
#endif

static void wiiu_free_shader_preset(wiiu_video_t *wiiu)
{
   if (!wiiu->shader_preset)
      return;

   for (int i = 0; i < wiiu->shader_preset->passes; i++)
   {
      gfd_free(wiiu->pass[i].gfd);
      MEM2_free(wiiu->pass[i].vs_ubo);
      MEM2_free(wiiu->pass[i].ps_ubo);
      MEM1_free(wiiu->pass[i].texture.surface.image);
   }

   memset(wiiu->pass, 0, sizeof(wiiu->pass));

   free(wiiu->shader_preset);
   wiiu->shader_preset = NULL;
}

static void wiiu_gfx_free(void *data)
{
   wiiu_video_t *wiiu = (wiiu_video_t *) data;

   if (!wiiu)
      return;


   /* clear leftover image */
   GX2ClearColor(&wiiu->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
   GX2CopyColorBufferToScanBuffer(&wiiu->color_buffer, GX2_SCAN_TARGET_DRC);
   GX2CopyColorBufferToScanBuffer(&wiiu->color_buffer, GX2_SCAN_TARGET_TV);

   GX2SwapScanBuffers();
   GX2Flush();
   GX2DrawDone();
   GX2WaitForVsync();
   GX2Shutdown();

   GX2SetTVEnable(GX2_DISABLE);
   GX2SetDRCEnable(GX2_DISABLE);

   GX2DestroyShader(&frame_shader);
   GX2DestroyShader(&sprite_shader);
   wiiu_free_shader_preset(wiiu);

#ifdef HAVE_OVERLAY
   gx2_free_overlay(wiiu);
#endif

   MEM2_free(wiiu->ctx_state);
   MEM2_free(wiiu->cmd_buffer);
   MEM2_free(wiiu->texture.surface.image);
   MEM2_free(wiiu->menu.texture.surface.image);
   MEM2_free(wiiu->v);
   MEM2_free(wiiu->menu.v);
   MEM2_free(wiiu->vertex_cache.v);

   MEM1_free(wiiu->color_buffer.surface.image);
   MEM1_free(wiiu->ubo_vp);
   MEM1_free(wiiu->ubo_tex);
   MEM1_free(wiiu->ubo_mvp);
   MEM1_free(wiiu->input_ring_buffer);
   MEM1_free(wiiu->output_ring_buffer);

   MEMBucket_free(wiiu->tv_scan_buffer);
   MEMBucket_free(wiiu->drc_scan_buffer);

   free(wiiu);
}

static bool wiiu_init_frame_textures(wiiu_video_t *wiiu, unsigned width, unsigned height)
{
   MEM2_free(wiiu->texture.surface.image);
   if(wiiu->shader_preset)
   {
      for (int i = 0; i < wiiu->shader_preset->passes; i++)
         MEM1_free(wiiu->pass[i].texture.surface.image);
   }

   /* Initialize frame texture */
   memset(&wiiu->texture, 0, sizeof(GX2Texture));
   wiiu->texture.surface.width       = width;
   wiiu->texture.surface.height      = height;
   wiiu->texture.surface.depth       = 1;
   wiiu->texture.surface.dim         = GX2_SURFACE_DIM_TEXTURE_2D;
   wiiu->texture.surface.tileMode    = GX2_TILE_MODE_LINEAR_ALIGNED;
   wiiu->texture.viewNumSlices       = 1;
   wiiu->rgb32                       = wiiu->rgb32;

   if (wiiu->rgb32)
   {
      wiiu->texture.surface.format   = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
      wiiu->texture.compMap          = GX2_COMP_SEL(_G, _B, _A, _1);
   }
   else
   {
      wiiu->texture.surface.format   = GX2_SURFACE_FORMAT_UNORM_R5_G6_B5;
      wiiu->texture.compMap          = GX2_COMP_SEL(_B, _G, _R, _1);
   }

   GX2CalcSurfaceSizeAndAlignment(&wiiu->texture.surface);
   GX2InitTextureRegs(&wiiu->texture);

   wiiu->texture.surface.image = MEM2_alloc(wiiu->texture.surface.imageSize,
                                 wiiu->texture.surface.alignment);
   memset(wiiu->texture.surface.image, 0x0, wiiu->texture.surface.imageSize);
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, wiiu->texture.surface.image,
                 wiiu->texture.surface.imageSize);

   if(wiiu->shader_preset)
   {
      for (int i = 0; i < wiiu->shader_preset->passes; i++)
      {
         struct video_shader_pass *pass = &wiiu->shader_preset->pass[i];

         switch (pass->fbo.type_x)
         {
         case RARCH_SCALE_INPUT:
            width *= pass->fbo.scale_x;
            break;

         case RARCH_SCALE_VIEWPORT:
            width = wiiu->vp.width * pass->fbo.scale_x;
            break;

         case RARCH_SCALE_ABSOLUTE:
            width = pass->fbo.abs_x;
            break;

         default:
            break;
         }

         switch (pass->fbo.type_y)
         {
         case RARCH_SCALE_INPUT:
            height *= pass->fbo.scale_y;
            break;

         case RARCH_SCALE_VIEWPORT:
            height = wiiu->vp.height * pass->fbo.scale_y;
            break;

         case RARCH_SCALE_ABSOLUTE:
            height = pass->fbo.abs_y;
            break;

         default:
            break;
         }

         if (!width)
            width = wiiu->color_buffer.surface.width;

         if (!height)
            height = wiiu->color_buffer.surface.height;



         memset(&wiiu->pass[i].texture, 0, sizeof(wiiu->pass[i].texture));
         wiiu->pass[i].texture.surface.dim         = GX2_SURFACE_DIM_TEXTURE_2D;
         wiiu->pass[i].texture.surface.width       = width;
         wiiu->pass[i].texture.surface.height      = height;
         wiiu->pass[i].texture.surface.depth       = 1;
   //      wiiu->pass[i].texture.surface.mipLevels   = 1;
         wiiu->pass[i].texture.surface.format      = pass->fbo.fp_fbo? GX2_SURFACE_FORMAT_FLOAT_R32_G32_B32_A32 :
                                                                       pass->fbo.srgb_fbo? GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8 :
                                                                                           GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
         wiiu->pass[i].texture.surface.use         = (GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
         wiiu->pass[i].texture.viewNumSlices       = 1;
         wiiu->pass[i].texture.compMap             = GX2_COMP_SEL(_R, _G, _B, _A);

         GX2CalcSurfaceSizeAndAlignment(&wiiu->pass[i].texture.surface);
         GX2InitTextureRegs(&wiiu->pass[i].texture);


         if((i != (wiiu->shader_preset->passes - 1)) || (width != wiiu->vp.width) || (height != wiiu->vp.height))
         {
            wiiu->pass[i].texture.surface.image = MEM1_alloc(wiiu->pass[i].texture.surface.imageSize,
                                                  wiiu->pass[i].texture.surface.alignment);

            if (!wiiu->pass[i].texture.surface.image)
            {
               printf("failed to allocate Render target memory from MEM1. falling back to stock.\n");
               wiiu_free_shader_preset(wiiu);
               return false;
            }

            memset(wiiu->pass[i].texture.surface.image, 0x00, wiiu->pass[i].texture.surface.imageSize);
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, wiiu->pass[i].texture.surface.image,
                          wiiu->pass[i].texture.surface.imageSize);
         }

         memset(&wiiu->pass[i].color_buffer, 0, sizeof(wiiu->pass[i].color_buffer));
         wiiu->pass[i].color_buffer.surface = wiiu->pass[i].texture.surface;
         GX2InitColorBufferRegs(&wiiu->pass[i].color_buffer);
      }
   }

   return true;
}


static bool wiiu_gfx_frame(void *data, const void *frame,
                           unsigned width, unsigned height, uint64_t frame_count,
                           unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
#if 0
   static float fps;
   static u32 frames;
   static u32 lastTick, currentTick;
   u32 diff;
#endif
   uint32_t i;
   wiiu_video_t *wiiu = (wiiu_video_t *) data;

   (void)msg;

   if (wiiu->vsync)
   {
      uint32_t swap_count;
      uint32_t flip_count;
      OSTime last_flip;
      OSTime last_vsync;

      GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);

      if (wiiu->last_vsync >= last_vsync)
      {
         GX2WaitForVsync();
         wiiu->last_vsync = last_vsync + ms_to_ticks(17);
      }
      else
         wiiu->last_vsync = last_vsync;
   }

   GX2WaitForFlip();

   if (!width || !height)
      return true;

#if 0
   currentTick = OSGetSystemTick();
   diff        = currentTick - lastTick;

   frames++;

   if (diff > wiiu_timer_clock)
   {
      fps = (float)frames * ((float) wiiu_timer_clock / (float) diff);
      lastTick = currentTick;
      frames = 0;
   }

   static u32 last_frame_tick;

   if (!(wiiu->menu.enable))
      printf("frame time : %10.6f ms            \r",
             (float)(currentTick - last_frame_tick) * 1000.0f / (float)wiiu_timer_clock);

   last_frame_tick = currentTick;
   printf("fps: %8.8f frames : %5i\r", fps, wiiu->frames++);
   fflush(stdout);
#endif

   if (wiiu->should_resize)
      wiiu_gfx_update_viewport(wiiu);

   GX2ClearColor(&wiiu->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
   /* can't call GX2ClearColor after GX2SetContextState for whatever reason */
   GX2SetContextState(wiiu->ctx_state);

   if (frame)
   {
      if((width != wiiu->texture.surface.width) ||
         (height != wiiu->texture.surface.height))
         wiiu_init_frame_textures(wiiu, width, height);

      wiiu->width  = width;
      wiiu->height = height;

      if (wiiu->rgb32)
      {
         const uint32_t *src = frame;
         uint32_t *dst = (uint32_t *)wiiu->texture.surface.image;

         for (i = 0; i < height; i++)
         {
            uint32_t j;

            for (j = 0; j < width; j++)
               dst[j] = src[j];

            dst += wiiu->texture.surface.pitch;
            src += pitch / 4;
         }
      }
      else
      {
         const uint16_t *src = frame;
         uint16_t       *dst = (uint16_t *)wiiu->texture.surface.image;

         for (i = 0; i < height; i++)
         {
            unsigned j;

            for (j = 0; j < width; j++)
               dst[j] = __builtin_bswap16(src[j]);

            dst += wiiu->texture.surface.pitch;
            src += pitch / 2;
         }
      }


      GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, wiiu->texture.surface.image,
                    wiiu->texture.surface.imageSize);

      wiiu_set_tex_coords(wiiu->v, &wiiu->texture, 0, 0, width, height, wiiu->rotation);
   }

   GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);

   GX2SetFetchShader(&frame_shader.fs);
   GX2SetVertexUniformBlock(frame_shader.vs.uniformBlocks[0].offset, frame_shader.vs.uniformBlocks[0].size, wiiu->ubo_mvp);
   GX2SetAttribBuffer(0, 4 * sizeof(*wiiu->v), sizeof(*wiiu->v), wiiu->v);

   GX2Texture *texture = &wiiu->texture;

   if (wiiu->shader_preset)
   {
      if(!wiiu->pass[0].texture.surface.image)
         wiiu_init_frame_textures(wiiu, width, height);

      for (int i = 0; i < wiiu->shader_preset->passes; i++)
      {

         GX2SetVertexShader(wiiu->pass[i].gfd->vs);
         GX2SetPixelShader(wiiu->pass[i].gfd->ps);

//         if (wiiu->pass[i].gfd->vs->uniformBlockCount > 0)
//            GX2SetVertexUniformBlock(wiiu->pass[i].gfd->vs->uniformBlocks[0].offset, wiiu->pass[i].gfd->vs->uniformBlocks[0].size, wiiu->ubo_mvp);

//         GX2SetAttribBuffer(0, 4 * sizeof(*wiiu->v), sizeof(*wiiu->v), wiiu->v);

//         if(wiiu->pass[i].vs_ubo)
//            GX2SetVertexUniformBlock(wiiu->pass[i].gfd->vs->uniformBlocks[1].offset,
//                  wiiu->pass[i].gfd->vs->uniformBlocks[1].size,
//                  wiiu->pass[i].vs_ubo);

         if (wiiu->pass[i].ps_ubo)
         {
            for (int j = 0; j < wiiu->pass[i].gfd->ps->uniformVarCount; j++)
            {
               if (wiiu->pass[i].gfd->ps->uniformVars[j].block != 0)
                  continue;

               const char *id = strrchr(wiiu->pass[i].gfd->ps->uniformVars[j].name, '.');

               if (!id)
                  continue;

               id++;

               float *dst = wiiu->pass[i].ps_ubo + wiiu->pass[i].gfd->ps->uniformVars[j].offset;

               if (!strcmp(id, "OutputSize"))
               {
                  ((GX2_vec4 *)dst)->x = wiiu->pass[i].color_buffer.surface.width;
                  ((GX2_vec4 *)dst)->y = wiiu->pass[i].color_buffer.surface.height;
                  ((GX2_vec4 *)dst)->z = 1.0f / wiiu->pass[i].color_buffer.surface.width;
                  ((GX2_vec4 *)dst)->w = 1.0f / wiiu->pass[i].color_buffer.surface.height;
                  continue;
               }

               if (!strcmp(id, "OriginalSize"))
               {
                  ((GX2_vec4 *)dst)->x = wiiu->texture.surface.width;
                  ((GX2_vec4 *)dst)->y = wiiu->texture.surface.height;
                  ((GX2_vec4 *)dst)->z = 1.0f / wiiu->texture.surface.width;
                  ((GX2_vec4 *)dst)->w = 1.0f / wiiu->texture.surface.height;
                  continue;
               }

               if (!strcmp(id, "SourceSize"))
               {
                  ((GX2_vec4 *)dst)->x = texture->surface.width;
                  ((GX2_vec4 *)dst)->y = texture->surface.height;
                  ((GX2_vec4 *)dst)->z = 1.0f / texture->surface.width;
                  ((GX2_vec4 *)dst)->w = 1.0f / texture->surface.height;
                  continue;
               }

               for (int k = 0; k < wiiu->shader_preset->num_parameters; k++)
               {
                  if (!strcmp(id, wiiu->shader_preset->parameters[k].id))
                  {
                     *dst = wiiu->shader_preset->parameters[k].current;
                     *(u32 *)dst = __builtin_bswap32(*(u32 *)dst);
                     break;
                  }
               }

            }

            GX2Invalidate(GX2_INVALIDATE_MODE_CPU_UNIFORM_BLOCK, wiiu->pass[i].ps_ubo,
                          wiiu->pass[i].gfd->ps->uniformBlocks[0].size);

            GX2SetPixelUniformBlock(wiiu->pass[i].gfd->ps->uniformBlocks[0].offset,
                                    wiiu->pass[i].gfd->ps->uniformBlocks[0].size, wiiu->pass[i].ps_ubo);
         }


         GX2SetPixelTexture(texture, wiiu->pass[i].gfd->ps->samplerVars[0].location);
         GX2SetPixelSampler(wiiu->shader_preset->pass[i].filter ? &wiiu->sampler_linear : &wiiu->sampler_nearest,
                            wiiu->pass[i].gfd->ps->samplerVars[0].location);

         if(wiiu->pass[i].color_buffer.surface.image)
         {
            GX2SetColorBuffer(&wiiu->pass[i].color_buffer, GX2_RENDER_TARGET_0);
            GX2SetViewport(0.0f, 0.0f, wiiu->pass[i].color_buffer.surface.width, wiiu->pass[i].color_buffer.surface.height, 0.0f,
                           1.0f);
            GX2SetScissor(0, 0, wiiu->pass[i].color_buffer.surface.width, wiiu->pass[i].color_buffer.surface.height);
            GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);

            GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, wiiu->pass[i].texture.surface.image,
                          wiiu->pass[i].texture.surface.imageSize);
            texture = &wiiu->pass[i].texture;
         }
         else
         {
            texture = NULL;
            break;
         }
      }

      GX2SetColorBuffer(&wiiu->color_buffer, GX2_RENDER_TARGET_0);
   }

   if(texture)
   {
      GX2SetVertexShader(&frame_shader.vs);
      GX2SetPixelShader(&frame_shader.ps);
      GX2SetPixelTexture(texture, frame_shader.ps.samplerVars[0].location);
      GX2SetPixelSampler(wiiu->smooth ? &wiiu->sampler_linear : &wiiu->sampler_nearest,
                         frame_shader.ps.samplerVars[0].location);
   }

   GX2SetViewport(wiiu->vp.x, wiiu->vp.y, wiiu->vp.width, wiiu->vp.height, 0.0f, 1.0f);
   GX2SetScissor(wiiu->vp.x, wiiu->vp.y, wiiu->vp.width, wiiu->vp.height);
   GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);


   GX2SetShaderMode(GX2_SHADER_MODE_GEOMETRY_SHADER);
   GX2SetShader(&sprite_shader);
   GX2SetGeometryShaderInputRingBuffer(wiiu->input_ring_buffer, wiiu->input_ring_buffer_size);
   GX2SetGeometryShaderOutputRingBuffer(wiiu->output_ring_buffer, wiiu->output_ring_buffer_size);
   GX2SetVertexUniformBlock(sprite_shader.vs.uniformBlocks[0].offset, sprite_shader.vs.uniformBlocks[0].size,
                            wiiu->ubo_vp);
   GX2SetVertexUniformBlock(sprite_shader.vs.uniformBlocks[1].offset, sprite_shader.vs.uniformBlocks[1].size,
                            wiiu->ubo_tex);
   GX2SetViewport(0.0f, 0.0f, wiiu->color_buffer.surface.width, wiiu->color_buffer.surface.height, 0.0f, 1.0f);
   GX2SetScissor(0, 0, wiiu->color_buffer.surface.width, wiiu->color_buffer.surface.height);

#ifdef HAVE_OVERLAY

   if (wiiu->overlay_enable)
      gx2_render_overlay(wiiu);

#endif

   if (wiiu->menu.enable)
   {
      GX2SetAttribBuffer(0, 4 * sizeof(*wiiu->menu.v), sizeof(*wiiu->menu.v), wiiu->menu.v);

      GX2SetPixelTexture(&wiiu->menu.texture, sprite_shader.ps.samplerVars[0].location);
      GX2SetPixelSampler(&wiiu->sampler_linear, sprite_shader.ps.samplerVars[0].location);

      GX2DrawEx(GX2_PRIMITIVE_MODE_POINTS, 1, 0, 1);
   }

   wiiu->vertex_cache.current = 0;
   GX2SetAttribBuffer(0, wiiu->vertex_cache.size * sizeof(*wiiu->vertex_cache.v),
                      sizeof(*wiiu->vertex_cache.v), wiiu->vertex_cache.v);
   GX2SetPixelSampler(&wiiu->sampler_linear, sprite_shader.ps.samplerVars[0].location);

   wiiu->render_msg_enabled = true;

   if (wiiu->menu.enable)
      menu_driver_frame(video_info);

   if (msg)
      font_driver_render_msg(video_info, NULL, msg, NULL);

   wiiu->render_msg_enabled = false;

   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER,
                 wiiu->vertex_cache.v, wiiu->vertex_cache.current * sizeof(*wiiu->vertex_cache.v));

   if (wiiu->menu.enable)
      GX2DrawDone();

   GX2CopyColorBufferToScanBuffer(&wiiu->color_buffer, GX2_SCAN_TARGET_DRC);
   GX2CopyColorBufferToScanBuffer(&wiiu->color_buffer, GX2_SCAN_TARGET_TV);

   GX2SwapScanBuffers();
   GX2Flush();

   return true;
}

static void wiiu_gfx_set_nonblock_state(void *data, bool toggle)
{
   wiiu_video_t *wiiu = (wiiu_video_t *) data;

   if (!wiiu)
      return;

   wiiu->vsync = !toggle;
   GX2SetSwapInterval(!toggle);  /* do we need this ? */
}

static bool wiiu_gfx_alive(void *data)
{
   (void)data;
   return true;
}

static bool wiiu_gfx_focus(void *data)
{
   (void)data;
   return true;
}

static bool wiiu_gfx_suppress_screensaver(void *data, bool enable)
{
   (void)data;
   (void)enable;
   return false;
}

static bool wiiu_gfx_set_shader(void *data,
                                enum rarch_shader_type type, const char *path)
{
   wiiu_video_t *wiiu = (wiiu_video_t *)data;

   if (!wiiu)
      return false;

   GX2DrawDone();
   wiiu_free_shader_preset(wiiu);

   if (type != RARCH_SHADER_SLANG && path)
   {
      RARCH_WARN("Only .slang or .slangp shaders are supported. Falling back to stock.\n");
      path = NULL;
   }

   if (!path)
      return true;

   config_file_t *conf = config_file_new(path);

   if (!conf)
      return false;

   wiiu->shader_preset = calloc(1, sizeof(*wiiu->shader_preset));

   if (!video_shader_read_conf_cgp(conf, wiiu->shader_preset))
   {
      free(wiiu->shader_preset);
      wiiu->shader_preset = NULL;
      return false;
   }

   config_file_free(conf);

   video_shader_resolve_relative(wiiu->shader_preset, path);

//   video_shader_resolve_parameters(conf, &shader);
   for (int i = 0; i < wiiu->shader_preset->passes; i++)
      slang_preprocess_parse_parameters(wiiu->shader_preset->pass[i].source.path, wiiu->shader_preset);

   for (int i = 0; i < wiiu->shader_preset->passes; i++)
   {
      char gfdpath[PATH_MAX_LENGTH];
      struct video_shader_pass *pass = &wiiu->shader_preset->pass[i];

      strncpy(gfdpath, pass->source.path, PATH_MAX_LENGTH);

      char *ptr = strrchr(gfdpath, '.');

      if (!ptr)
         ptr = gfdpath + strlen(gfdpath);

      *ptr++ = '.';
      *ptr++ = 'g';
      *ptr++ = 's';
      *ptr++ = 'h';
      *ptr++ = '\0';

      wiiu->pass[i].gfd = gfd_open(gfdpath);

      if (!wiiu->pass[i].gfd)
      {
         wiiu_free_shader_preset(wiiu);
         return false;
      }

      if (wiiu->pass[i].gfd->vs->uniformBlockCount > 1)
      {
         wiiu->pass[i].vs_ubo = MEM2_alloc(wiiu->pass[i].gfd->vs->uniformBlocks[1].size, GX2_UNIFORM_BLOCK_ALIGNMENT);
         memset(wiiu->pass[i].vs_ubo, 0, wiiu->pass[i].gfd->vs->uniformBlocks[1].size);
         GX2Invalidate(GX2_INVALIDATE_MODE_CPU_UNIFORM_BLOCK, wiiu->pass[i].vs_ubo,
                       wiiu->pass[i].gfd->vs->uniformBlocks[1].size);
      }

      if (wiiu->pass[i].gfd->ps->uniformBlockCount > 0)
      {
         wiiu->pass[i].ps_ubo = MEM2_alloc(wiiu->pass[i].gfd->ps->uniformBlocks[0].size, GX2_UNIFORM_BLOCK_ALIGNMENT);
         memset(wiiu->pass[i].ps_ubo, 0, wiiu->pass[i].gfd->ps->uniformBlocks[0].size);
         GX2Invalidate(GX2_INVALIDATE_MODE_CPU_UNIFORM_BLOCK, wiiu->pass[i].ps_ubo,
                       wiiu->pass[i].gfd->ps->uniformBlocks[0].size);
      }
   }

   return true;

}

static struct video_shader *wiiu_gfx_get_current_shader(void *data)
{
   wiiu_video_t *wiiu = (wiiu_video_t *)data;

   if (!wiiu)
      return NULL;

   return wiiu->shader_preset;
}


static void wiiu_gfx_set_rotation(void *data,
                                  unsigned rotation)
{
   wiiu_video_t *wiiu = (wiiu_video_t *) data;

   if (wiiu)
   {
      wiiu->rotation = rotation;
      wiiu_set_projection(wiiu);
      wiiu->should_resize = true;
   }
}

static void wiiu_gfx_viewport_info(void *data,
                                   struct video_viewport *vp)
{
   wiiu_video_t *wiiu = (wiiu_video_t *) data;

   if (wiiu)
      *vp = wiiu->vp;
}

static bool wiiu_gfx_read_viewport(void *data, uint8_t *buffer, bool is_idle)
{
   (void)data;
   (void)buffer;

   return true;
}

static uintptr_t wiiu_gfx_load_texture(void *video_data, void *data,
                                       bool threaded, enum texture_filter_type filter_type)
{
   uint32_t i;
   wiiu_video_t *wiiu = (wiiu_video_t *) video_data;
   struct texture_image *image = (struct texture_image *)data;

   if (!wiiu)
      return 0;

   GX2Texture *texture = calloc(1, sizeof(GX2Texture));

   texture->surface.width       = image->width;
   texture->surface.height      = image->height;
   texture->surface.depth       = 1;
   texture->surface.dim         = GX2_SURFACE_DIM_TEXTURE_2D;
   texture->surface.tileMode    = GX2_TILE_MODE_LINEAR_ALIGNED;
   texture->viewNumSlices       = 1;

   texture->surface.format      = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
   texture->compMap             = GX2_COMP_SEL(_G, _B, _A, _R);

   GX2CalcSurfaceSizeAndAlignment(&texture->surface);
   GX2InitTextureRegs(texture);
   texture->surface.image = MEM2_alloc(texture->surface.imageSize, texture->surface.alignment);

   for (i = 0; (i < image->height) && (i < texture->surface.height); i++)
      memcpy((uint32_t *)texture->surface.image + (i * texture->surface.pitch),
             image->pixels + (i * image->width), image->width * sizeof(image->pixels));

   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);

   return (uintptr_t)texture;
}
static void wiiu_gfx_unload_texture(void *data, uintptr_t handle)
{
   GX2Texture *texture = (GX2Texture *)handle;

   if (!texture)
      return;

   MEM2_free(texture->surface.image);
   free(texture);
}
static void wiiu_gfx_set_filtering(void *data, unsigned index, bool smooth)
{
   wiiu_video_t *wiiu = (wiiu_video_t *) data;

   if (wiiu)
      wiiu->smooth = smooth;
}


static void wiiu_gfx_apply_state_changes(void *data)
{
   wiiu_video_t *wiiu = (wiiu_video_t *)data;

   if (wiiu)
      wiiu->should_resize = true;
}

static void wiiu_gfx_set_texture_frame(void *data, const void *frame, bool rgb32,
                                       unsigned width, unsigned height, float alpha)
{
   uint32_t i;
   const uint16_t *src = NULL;
   uint16_t *dst       = NULL;
   wiiu_video_t *wiiu  = (wiiu_video_t *) data;

   if (!wiiu)
      return;

   if (!frame || !width || !height)
      return;

   if (width > wiiu->menu.texture.surface.width)
      width = wiiu->menu.texture.surface.width;

   if (height > wiiu->menu.texture.surface.height)
      height = wiiu->menu.texture.surface.height;

   wiiu->menu.width  = width;
   wiiu->menu.height = height;

   src               = frame;
   dst               = (uint16_t *)wiiu->menu.texture.surface.image;

   for (i = 0; i < height; i++)
   {
      memcpy(dst, src, width * sizeof(uint16_t));
      dst += wiiu->menu.texture.surface.pitch;
      src += width;
   }

   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, wiiu->menu.texture.surface.image,
                 wiiu->menu.texture.surface.imageSize);

   wiiu->menu.v->pos.x = 0.0f;
   wiiu->menu.v->pos.y = 0.0f;
   wiiu->menu.v->pos.width = width;
   wiiu->menu.v->pos.height = height;
   wiiu->menu.v->coord.u = 0.0f;
   wiiu->menu.v->coord.v = 0.0f;
   wiiu->menu.v->coord.width = (float)width / wiiu->texture.surface.width;
   wiiu->menu.v->coord.height = (float)height / wiiu->texture.surface.height;
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, wiiu->menu.v, 4 * sizeof(*wiiu->menu.v));

}

static void wiiu_gfx_set_texture_enable(void *data, bool state, bool full_screen)
{
   (void) full_screen;
   wiiu_video_t *wiiu = (wiiu_video_t *) data;

   if (wiiu)
      wiiu->menu.enable = state;

}

static void wiiu_gfx_set_osd_msg(void *data,
                                 video_frame_info_t *video_info,
                                 const char *msg,
                                 const void *params, void *font)
{
   wiiu_video_t *wiiu = (wiiu_video_t *)data;

   if (wiiu)
   {
      if (wiiu->render_msg_enabled)
         font_driver_render_msg(video_info, font, msg, params);
      else
         printf("OSD msg: %s\n", msg);
   }

}

static const video_poke_interface_t wiiu_poke_interface =
{
   NULL,                      /* set_coords */
   NULL,                      /* set_mvp */
   wiiu_gfx_load_texture,
   wiiu_gfx_unload_texture,
   NULL, /* set_video_mode */
   wiiu_gfx_set_filtering,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   wiiu_gfx_set_aspect_ratio,
   wiiu_gfx_apply_state_changes,
   wiiu_gfx_set_texture_frame,
   wiiu_gfx_set_texture_enable,
   wiiu_gfx_set_osd_msg,
   NULL, /* show_mouse */
   NULL, /* grab_mouse_toggle */
   wiiu_gfx_get_current_shader,
   NULL, /* get_current_software_framebuffer */
   NULL, /* get_hw_render_interface */
};

static void wiiu_gfx_get_poke_interface(void *data,
                                        const video_poke_interface_t **iface)
{
   (void)data;
   *iface = &wiiu_poke_interface;
}

video_driver_t video_wiiu =
{
   wiiu_gfx_init,
   wiiu_gfx_frame,
   wiiu_gfx_set_nonblock_state,
   wiiu_gfx_alive,
   wiiu_gfx_focus,
   wiiu_gfx_suppress_screensaver,
   NULL, /* has_windowed */
   wiiu_gfx_set_shader,
   wiiu_gfx_free,
   "gx2",
   NULL, /* set_viewport */
   wiiu_gfx_set_rotation,
   wiiu_gfx_viewport_info,
   wiiu_gfx_read_viewport,
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   gx2_get_overlay_interface, /* overlay_interface */
#endif
   wiiu_gfx_get_poke_interface,
   NULL, /* wrap_type_to_enum */
};
