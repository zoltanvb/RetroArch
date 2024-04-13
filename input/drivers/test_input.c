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

#include <stdint.h>
#include <stdlib.h>

#include <retro_miscellaneous.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <streams/file_stream.h>
#include <formats/rjson.h>

#include "../input_driver.h"
#include "../input_keymaps.h"
#include "../../verbosity.h"
#include "../../gfx/video_driver.h"

#define MAX_TEST_STEPS 200

#define INPUT_TEST_COMMAND_PRESS_KEY          1
#define INPUT_TEST_COMMAND_RELEASE_KEY        2

/* TODO/FIXME - static globals */
static uint16_t test_key_state[DEFAULT_MAX_PADS+1][RETROK_LAST];


typedef struct
{
   unsigned frame;
   unsigned action;
   unsigned param_num;
   char param_str[255];
   bool handled;
} input_test_step_t;

static input_test_step_t input_test_steps[MAX_TEST_STEPS];

static unsigned current_frame         = 0;
static unsigned next_teststep_frame   = 0;
static unsigned current_test_step     = 0;
static unsigned last_test_step        = MAX_TEST_STEPS + 1;
static uint32_t input_state_validated = 0;
static uint32_t combo_state_validated = 0;
static bool     dump_state_blocked    = false;

/************************************/
/* JSON Helpers for test input file */
/************************************/

typedef struct
{
   unsigned *current_entry_uint_val;
   char **current_entry_str_val;
   unsigned frame;
   unsigned action;
   unsigned param_num;
   char *param_str;
} KTifJSONContext;

static bool KTifJSONObjectEndHandler(void* context)
{
   KTifJSONContext *pCtx = (KTifJSONContext*)context;

   /* Too long input is handled elsewhere, it should not lead to parse error */
   if (current_test_step >= MAX_TEST_STEPS)
      return true;

   /* Copy values read from JSON file + fill defaults */
   if (pCtx->frame == 0xffff)
      input_test_steps[current_test_step].frame  = input_test_steps[current_test_step-1].frame + 60;
   else
      input_test_steps[current_test_step].frame  = pCtx->frame;
   
   input_test_steps[current_test_step].action    = pCtx->action;
   input_test_steps[current_test_step].param_num = pCtx->param_num;
   input_test_steps[current_test_step].handled   = false;

   if (!string_is_empty(pCtx->param_str))
      strlcpy(
            input_test_steps[current_test_step].param_str, pCtx->param_str,
            sizeof(input_test_steps[current_test_step].param_str));
   else
      input_test_steps[current_test_step].param_str[0] = '\0';
   
   current_test_step++;
   last_test_step = current_test_step;
   pCtx->frame = 0xffff;
   return true;
}

static bool KTifJSONObjectMemberHandler(void* context, const char *pValue, size_t length)
{
   KTifJSONContext *pCtx = (KTifJSONContext*)context;

   /* something went wrong */
   if (pCtx->current_entry_str_val)
      return false;

   if (length)
   {
      if (string_is_equal(pValue, "frame"))
         pCtx->current_entry_uint_val = &pCtx->frame;
      else if (string_is_equal(pValue, "action"))
         pCtx->current_entry_uint_val = &pCtx->action;
      else if (string_is_equal(pValue, "param_num"))
         pCtx->current_entry_uint_val = &pCtx->param_num;
      else if (string_is_equal(pValue, "param_str"))
         pCtx->current_entry_str_val = &pCtx->param_str;
      /* ignore unknown members */
   }

   return true;
}

static bool KTifJSONNumberHandler(void* context, const char *pValue, size_t length)
{
   KTifJSONContext *pCtx = (KTifJSONContext*)context;

   if (pCtx->current_entry_uint_val && length && !string_is_empty(pValue))
      *pCtx->current_entry_uint_val = string_to_unsigned(pValue);
   /* ignore unknown members */

   pCtx->current_entry_uint_val = NULL;

   return true;
}

static bool KTifJSONStringHandler(void* context, const char *pValue, size_t length)
{
   KTifJSONContext *pCtx = (KTifJSONContext*)context;

   if (pCtx->current_entry_str_val && length && !string_is_empty(pValue))
   {
      if (*pCtx->current_entry_str_val)
         free(*pCtx->current_entry_str_val);

      *pCtx->current_entry_str_val = strdup(pValue);
   }
   /* ignore unknown members */

   pCtx->current_entry_str_val = NULL;

   return true;
}

/* Parses test input file referenced by file_path.
 * Does nothing if test input file does not exist. */
static bool input_test_file_read(const char* file_path)
{
   bool success            = false;
   KTifJSONContext context = {0};
   RFILE *file             = NULL;
   rjson_t* parser;

   /* Sanity check */
   if (    string_is_empty(file_path)
       || !path_is_valid(file_path)
      )
   {
      RARCH_DBG("[Test input driver]: No test input file supplied.\n");
      return false;
   }
      
   /* Attempt to open test input file */
   file = filestream_open(
         file_path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!file)
   {
      RARCH_ERR("[Test input driver]: Failed to open test input file: \"%s\".\n",
            file_path);
      return false;
   }

   /* Initialise JSON parser */
   if (!(parser = rjson_open_rfile(file)))
   {
      RARCH_ERR("[Test input driver]: Failed to create JSON parser.\n");
      goto end;
   }

   /* Configure parser */
   rjson_set_options(parser, RJSON_OPTION_ALLOW_UTF8BOM);

   /* Read file */
   if (rjson_parse(parser, &context,
         KTifJSONObjectMemberHandler,
         KTifJSONStringHandler,
         KTifJSONNumberHandler,
         NULL, KTifJSONObjectEndHandler, NULL, NULL, /* object/array handlers */
         NULL, NULL) /* unused boolean/null handlers */
         != RJSON_DONE)
   {
      if (rjson_get_source_context_len(parser))
      {
         RARCH_ERR(
               "[Test input driver]: Error parsing chunk of test input file: %s\n---snip---\n%.*s\n---snip---\n",
               file_path,
               rjson_get_source_context_len(parser),
               rjson_get_source_context_buf(parser));
      }
      RARCH_WARN(
            "[Test input driver]: Error parsing test input file: %s\n",
            file_path);
      RARCH_ERR(
            "[Test input driver]: Error: Invalid JSON at line %d, column %d - %s.\n",
            (int)rjson_get_source_line(parser),
            (int)rjson_get_source_column(parser),
            (*rjson_get_error(parser) ? rjson_get_error(parser) : "format error"));
   }

   /* Free parser */
   rjson_free(parser);

   success = true;
end:
   /* Clean up leftover strings */
   if (context.param_str)
      free(context.param_str);

   /* Close log file */
   filestream_close(file);

   if (last_test_step >= MAX_TEST_STEPS)
   {
      RARCH_WARN("[Test input driver]: too long test input json, maximum size: %d\n",MAX_TEST_STEPS);
   }
   for (current_test_step = 0; current_test_step < last_test_step; current_test_step++)
   {
      RARCH_DBG(
         "[Test input driver]: test step %02d read from file: frame %d, action %x, num %x, str %s\n",
         current_test_step,
         input_test_steps[current_test_step].frame,
         input_test_steps[current_test_step].action,
         input_test_steps[current_test_step].param_num,
         input_test_steps[current_test_step].param_str);
   }
   current_test_step = 0;
   return success;
}

/********************************/
/* Test input file handling end */
/********************************/


/*uint16_t *test_keyboard_state_get(unsigned port)
{
   return test_key_state[port];
}*/

static void test_keyboard_free(void)
{
   unsigned i, j;

   for (i = 0; i < DEFAULT_MAX_PADS; i++)
      for (j = 0; j < RETROK_LAST; j++)
         test_key_state[i][j] = 0;
}

static int16_t test_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      bool keyboard_mapping_blocked,
      unsigned port,
      unsigned device,
      unsigned idx,
      unsigned id)
{
   if (port <= 0)
   {
      switch (device)
      {
         case RETRO_DEVICE_JOYPAD:
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            {
               unsigned i;
               int16_t ret = 0;

               for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
               {
                  if (binds[port][i].valid)
                  {
                     if (id < RARCH_BIND_LIST_END)
                        if (test_key_state[DEFAULT_MAX_PADS]
                              [rarch_keysym_lut[binds[port][i].key]])
                           ret |= (1 << i);
                  }
               }

               return ret;
            }

            if (binds[port][id].valid)
            {
               if (
                     (id < RARCH_BIND_LIST_END
                      && test_key_state[DEFAULT_MAX_PADS]
                      [rarch_keysym_lut[binds[port][id].key]])
                  )
                  return 1;
            }
            break;
         case RETRO_DEVICE_KEYBOARD:
            if (id < RARCH_BIND_LIST_END)
               return (test_key_state[DEFAULT_MAX_PADS]
                     [rarch_keysym_lut[binds[port][id].key]]);
            break;
      }
   }

   return 0;
}

static void test_input_free_input(void *data)
{
   test_keyboard_free();
}

static void* test_input_init(const char *joypad_driver)
{
   settings_t *settings = config_get_ptr();
   unsigned i;

   RARCH_DBG("[Test input driver]: start\n");

   input_test_file_read(settings->paths.test_input_file_general);
   if (last_test_step > MAX_TEST_STEPS)
      last_test_step = 0;
      
   input_keymaps_init_keyboard_lut(rarch_key_map_test);
   return (void*)-1;
}

static void test_input_poll(void *data)
{
   video_driver_state_t *video_st = video_state_get_ptr();
   uint64_t curr_frame            = video_st->frame_count;
   unsigned i;
   
   for (i=0; i<last_test_step; i++)
   {
      if (!input_test_steps[i].handled && curr_frame > input_test_steps[i].frame)
      {
         if( input_test_steps[i].action == INPUT_TEST_COMMAND_PRESS_KEY)
         {
            if(input_test_steps[i].param_num < RETROK_LAST)
               test_key_state[DEFAULT_MAX_PADS][input_test_steps[i].param_num] = 1;
            input_test_steps[i].handled = true;
            RARCH_DBG(
               "[Test input driver]: Pressing keyboard button %d at frame %d\n",
               input_test_steps[i].param_num, curr_frame);
         }
         else if( input_test_steps[i].action == INPUT_TEST_COMMAND_RELEASE_KEY)
         {
            if(input_test_steps[i].param_num < RETROK_LAST)
               test_key_state[DEFAULT_MAX_PADS][input_test_steps[i].param_num] = 0;
            input_test_steps[i].handled = true;
            RARCH_DBG(
               "[Test input driver]: Releasing keyboard button %d at frame %d\n",
               input_test_steps[i].param_num, curr_frame);
         }
         else
         {
            input_test_steps[i].handled = true;
            RARCH_WARN(
               "[Test joypad driver]: Unrecognized action %d in step %d, skipping\n",
               input_test_steps[i].action,i);
         }

      }
   }
}


static uint64_t test_input_get_capabilities(void *data)
{
   return UINT64_C(1) << RETRO_DEVICE_JOYPAD;
}

input_driver_t input_test = {
   test_input_init,
   test_input_poll,
   test_input_state,
   test_input_free_input,
   NULL,
   NULL,
   test_input_get_capabilities,
   "test",
   NULL,                         /* grab_mouse */
   NULL,
   NULL
};
