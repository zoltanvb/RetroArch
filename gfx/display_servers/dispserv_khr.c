/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2019 - Brad Parker
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

#include <math.h>

#include <compat/strl.h>
#include <string/stdstring.h>

#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif


#include "../video_display_server.h"
#include "../common/vulkan_common.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

typedef struct
{
   int crt_name_id;
   int monitor_index;
   unsigned opacity;
   uint8_t flags;
   char crt_name[16];
   char new_mode[256];
   char old_mode[256];
   char orig_output[256];
} dispserv_khr_t;

static bool khr_display_server_set_resolution(void *data,
      unsigned width, unsigned height, int int_hz, float hz,
      int center, int monitor_index, int xoffset, int padjust)
{
   unsigned curr_width               = 0;
   unsigned curr_height              = 0;
   unsigned curr_bpp                 = 0;
   float curr_refreshrate            = 0;
   bool retval = false;
   int reinit_flags                  = DRIVERS_CMD_ALL;
   dispserv_khr_t *dispserv = (dispserv_khr_t*)data;
   khr_display_ctx_data_t *khr = (khr_display_ctx_data_t*)
      video_driver_display_userdata_get();

   if (!dispserv)
      return false;

   if (khr)
   {
      curr_refreshrate = khr->refresh_rate_x1000 / 1000.0f;
      curr_width       = khr->width;
      curr_height      = khr->height;
      curr_bpp         = 32;
   }

   RARCH_DBG("[DRM]: Display server set resolution - incoming: %d x %d, %f Hz\n",width, height, hz);

   if (width == 0)
      width = curr_width;
   if (height == 0)
      height = curr_height;
   if (curr_bpp == 0)
      curr_bpp = curr_bpp;
   if (hz == 0)
      hz = curr_refreshrate;

   /* set core refresh from hz */
   video_monitor_set_refresh_rate(hz);

   RARCH_DBG("[DRM]: Display server set resolution - actual: %d x %d, %f Hz\n",width, height, hz);

   retval = video_driver_set_video_mode(width, height, true);

   /* Reinitialize drivers. */
   command_event(CMD_EVENT_REINIT, &reinit_flags);

   return retval;
}

/* TODO: move to somewhere common as it is reused from dispserv_win32.c */
/* Display resolution list qsort helper function */
static int resolution_list_qsort_func(
      const video_display_config_t *a, const video_display_config_t *b)
{
   char str_a[64];
   char str_b[64];

   if (!a || !b)
      return 0;

   str_a[0] = str_b[0] = '\0';

   snprintf(str_a, sizeof(str_a), "%04dx%04d (%d Hz)",
         a->width,
         a->height,
         a->refreshrate);

   snprintf(str_b, sizeof(str_b), "%04dx%04d (%d Hz)",
         b->width,
         b->height,
         b->refreshrate);

   return strcasecmp(str_a, str_b);
}

static void *khr_display_server_get_resolution_list(
      void *data, unsigned *len)
{
   struct video_display_config *conf = NULL;
   VkDisplayModePropertiesKHR *modes = NULL;
   unsigned curr_width               = 0;
   unsigned curr_height              = 0;
   unsigned curr_bpp                 = 0;
   float curr_refreshrate            = 0;
   unsigned count = 0;
   uint32_t display_count                    = 0;
   VkDisplayPropertiesKHR *displays          = NULL;
   uint32_t mode_count               = 0;
   unsigned dpy, i, j;
   settings_t *settings           = config_get_ptr();
   unsigned monitor_index   = settings->uints.video_monitor_index;
   khr_display_ctx_data_t *khr = (khr_display_ctx_data_t*)
      video_driver_display_userdata_get();
   gfx_ctx_vulkan_data_t vk = khr->vk;

   if (vkGetPhysicalDeviceDisplayPropertiesKHR(vk.context.gpu, &display_count, NULL) != VK_SUCCESS)
         return NULL;
   RARCH_DBG("[KHR]: Display server get resolution list - display count: %d\n",display_count);

   if (!(displays = (VkDisplayPropertiesKHR*)calloc(display_count, sizeof(*displays))))
         return NULL;
   if (vkGetPhysicalDeviceDisplayPropertiesKHR(vk.context.gpu, &display_count, displays) != VK_SUCCESS)
         return NULL;

   curr_refreshrate = khr->refresh_rate_x1000 / 1000.0f;
   curr_width       = khr->width;
   curr_height      = khr->height;
   curr_bpp         = 32;

   for (dpy = 0; dpy < display_count; dpy++)
   {
      VkDisplayKHR display;
      if (monitor_index != 0 && (monitor_index - 1) != dpy)
         continue;

      display    = displays[dpy].display;

      if (vkGetDisplayModePropertiesKHR(vk.context.gpu,
            display, &mode_count, NULL) != VK_SUCCESS)
         return NULL;

      if (!(modes = (VkDisplayModePropertiesKHR*)calloc(mode_count, sizeof(*modes))))
         return NULL;
      RARCH_DBG("[KHR]: Display server get resolution list - mode count for display %d: %d\n",dpy,mode_count);

      *len = mode_count;
      if (!(conf = (struct video_display_config*)
         calloc(*len, sizeof(struct video_display_config))))
         return NULL;

      if (vkGetDisplayModePropertiesKHR(vk.context.gpu,
            display, &mode_count, modes) != VK_SUCCESS)
         return NULL;

      for (i = 0; i < mode_count; i++)
      {
         const VkDisplayModePropertiesKHR *mode = &modes[i];

         conf[i].width       = mode->parameters.visibleRegion.width;
         conf[i].height      = mode->parameters.visibleRegion.height;
         conf[i].bpp         = 32;
         conf[i].refreshrate = (float) (mode->parameters.refreshRate / 1000.0f);
         conf[i].idx         = i;
         conf[i].current     = false;
         if (  (conf[i].width       == curr_width)
            && (conf[i].height      == curr_height)
            && (conf[i].bpp         == curr_bpp)
            && (conf[i].refreshrate == curr_refreshrate)
         )
            conf[i].current  = true;
      }

      free(modes);
      modes      = NULL;
   }
   free(displays);

   qsort(
         conf, count,
         sizeof(video_display_config_t),
         (int (*)(const void *, const void *))
               resolution_list_qsort_func);
   return conf;
}

/* TODO: screen orientation has support in DRM via planes, although not really exposed via xf86drm */
#if 0
static void khr_display_server_set_screen_orientation(void *data,
      enum rotation rotation)
{
}

static enum rotation khr_display_server_get_screen_orientation(void *data)
{
   int i, j;
   enum rotation     rotation     = ORIENTATION_NORMAL;
   dispserv_khr_t *dispserv       = (dispserv_khr_t*)data;
   return rotation;
}
#endif

static void* khr_display_server_init(void)
{
   dispserv_khr_t *dispserv = (dispserv_khr_t*)calloc(1, sizeof(*dispserv));

   if (dispserv)
      return dispserv;
   return NULL;
}

static void khr_display_server_destroy(void *data)
{
   dispserv_khr_t *dispserv       = (dispserv_khr_t*)data;
   if (dispserv)
      free(dispserv);
}

static bool khr_display_server_set_window_opacity(void *data, unsigned opacity)
{
   return true;
}

static uint32_t khr_display_server_get_flags(void *data)
{
   uint32_t             flags   = 0;
   return flags;
}

const video_display_server_t dispserv_khr = {
   khr_display_server_init,
   khr_display_server_destroy,
   khr_display_server_set_window_opacity,
   NULL, /* set_window_progress */
   NULL, /* set window decorations */
   khr_display_server_set_resolution,
   khr_display_server_get_resolution_list,
   NULL, /* get output options */
   NULL, /* khr_display_server_set_screen_orientation */
   NULL, /* khr_display_server_get_screen_orientation */
   khr_display_server_get_flags,
   "khr"
};
