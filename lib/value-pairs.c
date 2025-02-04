/*
 * Copyright (c) 2011-2014 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 2011-2014 Gergely Nagy <algernon@balabit.hu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "value-pairs.h"
#include "vptransform.h"
#include "logmsg.h"
#include "template/templates.h"
#include "template/macros.h"
#include "type-hinting.h"
#include "cfg-parser.h"
#include "misc.h"
#include "scratch-buffers.h"
#include "cfg.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct
{
  GPatternSpec *pattern;
  gboolean include;
} VPPatternSpec;

typedef struct
{
  gchar *name;
  LogTemplate *template;
} VPPairConf;

struct _ValuePairs
{
  GAtomicCounter ref_cnt;
  VPPatternSpec **patterns;
  GPtrArray *vpairs;
  GList *transforms;

  /* guint32 as CfgFlagHandler only supports 32 bit integers */
  guint32 scopes;
  guint32 patterns_size;
};

typedef enum
{
  VPS_NV_PAIRS        = 0x01,
  VPS_DOT_NV_PAIRS    = 0x02,
  VPS_RFC3164         = 0x04,
  VPS_RFC5424         = 0x08,
  VPS_ALL_MACROS      = 0x10,
  VPS_SELECTED_MACROS = 0x20,
  VPS_SDATA           = 0x40,
  VPS_EVERYTHING      = 0x7f,
} ValuePairScope;

enum
{
  VPT_MACRO,
  VPT_NVPAIR,
};

typedef struct
{
  gchar *name;
  gchar *alt_name;
  gint type;
  gint id;
} ValuePairSpec;

static ValuePairSpec rfc3164[] =
{
  /* there's one macro named DATE that'll be expanded specially */
  { "FACILITY" },
  { "PRIORITY" },
  { "HOST"     },
  { "PROGRAM"  },
  { "PID"      },
  { "MESSAGE"  },
  { "DATE" },
  { 0 },
};

static ValuePairSpec rfc5424[] =
{
  { "MSGID", },
  { 0 },
};

static ValuePairSpec selected_macros[] =
{
  { "TAGS" },
  { "SOURCEIP" },
  { "SEQNUM" },
  { 0 },
};

static ValuePairSpec *all_macros;
static gboolean value_pair_sets_initialized;

static CfgFlagHandler value_pair_scope[] =
{
  { "nv-pairs",           CFH_SET, offsetof(ValuePairs, scopes), VPS_NV_PAIRS },
  { "dot-nv-pairs",       CFH_SET, offsetof(ValuePairs, scopes), VPS_DOT_NV_PAIRS},
  { "all-nv-pairs",       CFH_SET, offsetof(ValuePairs, scopes), VPS_NV_PAIRS | VPS_DOT_NV_PAIRS },
  { "rfc3164",            CFH_SET, offsetof(ValuePairs, scopes), VPS_RFC3164 },
  { "core",               CFH_SET, offsetof(ValuePairs, scopes), VPS_RFC3164 },
  { "base",               CFH_SET, offsetof(ValuePairs, scopes), VPS_RFC3164 },
  { "rfc5424",            CFH_SET, offsetof(ValuePairs, scopes), VPS_RFC5424 },
  { "syslog-proto",       CFH_SET, offsetof(ValuePairs, scopes), VPS_RFC5424 },
  { "all-macros",         CFH_SET, offsetof(ValuePairs, scopes), VPS_ALL_MACROS },
  { "selected-macros",    CFH_SET, offsetof(ValuePairs, scopes), VPS_SELECTED_MACROS },
  { "sdata",              CFH_SET, offsetof(ValuePairs, scopes), VPS_SDATA },
  { "everything",         CFH_SET, offsetof(ValuePairs, scopes), VPS_EVERYTHING },
  { NULL,                 0,       0,                            0},
};

static gboolean
vp_pattern_spec_eval(VPPatternSpec *self, const gchar *input)
{
  return g_pattern_match_string(self->pattern, input);
}

static void
vp_pattern_spec_free(VPPatternSpec *self)
{
  g_pattern_spec_free(self->pattern);
  g_free(self);
}

static VPPatternSpec *
vp_pattern_spec_new(const gchar *pattern, gboolean include)
{
  VPPatternSpec *self = g_new0(VPPatternSpec, 1);

  self->pattern = g_pattern_spec_new(pattern);
  self->include = include;
  return self;
}

gboolean
value_pairs_add_scope(ValuePairs *vp, const gchar *scope)
{
  return cfg_process_flag(value_pair_scope, vp, scope);
}

void
value_pairs_add_glob_pattern(ValuePairs *vp, const gchar *pattern,
                             gboolean include)
{
  gint i;

  i = vp->patterns_size++;
  vp->patterns = g_renew(VPPatternSpec *, vp->patterns, vp->patterns_size);
  vp->patterns[i] = vp_pattern_spec_new(pattern, include);;
}

void
value_pairs_add_glob_patterns(ValuePairs *vp, GList *patterns, gboolean include)
{
  GList *l = patterns;

  while (l)
    {
      value_pairs_add_glob_pattern(vp, (gchar *)l->data, include);
      l = g_list_next (l);
    }
  string_list_free(patterns);
}

gboolean
value_pairs_add_pair(ValuePairs *vp, const gchar *key, LogTemplate *value)
{
  VPPairConf *p = g_new(VPPairConf, 1);

  p->name = g_strdup(key);
  p->template = log_template_ref(value);
  g_ptr_array_add(vp->vpairs, p);
  return TRUE;
}

static gchar *
vp_transform_apply (ValuePairs *vp, gchar *key)
{
  GList *l;
  gchar *ckey, *okey = g_strdup(key);

  if (!vp->transforms)
    return okey;

  l = vp->transforms;
  while (l)
    {
      ValuePairsTransformSet *t = (ValuePairsTransformSet *)l->data;
      ckey = value_pairs_transform_set_apply(t, okey);
      g_free(okey);
      okey = ckey;
      l = g_list_next (l);
    }

  return ckey;
}

/* runs over the name-value pairs requested by the user (e.g. with value_pairs_add_pair) */
static void
vp_pairs_foreach(gpointer data, gpointer user_data)
{
  ValuePairs *vp = ((gpointer *)user_data)[0];
  LogMessage *msg = ((gpointer *)user_data)[2];
  gint32 seq_num = GPOINTER_TO_INT (((gpointer *)user_data)[3]);
  GTree *scope_set = ((gpointer *)user_data)[5];
  const LogTemplateOptions *template_options = ((gpointer *)user_data)[6];
  SBTHGString *sb = sb_th_gstring_acquire();
  VPPairConf *vpc = (VPPairConf *)data;
  gint time_zone_mode = GPOINTER_TO_INT (((gpointer *)user_data)[7]);

  sb->type_hint = vpc->template->type_hint;
  log_template_append_format((LogTemplate *)vpc->template, msg,
                             template_options,
                             time_zone_mode, seq_num, NULL, sb_th_gstring_string(sb));

  if (sb_th_gstring_string(sb)->len == 0)
    {
      sb_th_gstring_release(sb);
      return;
    }

  g_tree_insert(scope_set, vp_transform_apply(vp, vpc->name), sb);
}

/* runs over the LogMessage nv-pairs, and inserts them unless excluded */
static gboolean
vp_msg_nvpairs_foreach(NVHandle handle, gchar *name,
                       const gchar *value, gssize value_len,
                       gpointer user_data)
{
  ValuePairs *vp = ((gpointer *)user_data)[0];
  GTree *scope_set = ((gpointer *)user_data)[5];
  guint j;
  gboolean inc;
  SBTHGString *sb;

  if (value_len == 0)
    return FALSE;

  inc = (name[0] == '.' && (vp->scopes & VPS_DOT_NV_PAIRS)) ||
        (name[0] != '.' && (vp->scopes & VPS_NV_PAIRS)) ||
        (log_msg_is_handle_sdata(handle) && (vp->scopes & (VPS_SDATA + VPS_RFC5424)));

  for (j = 0; j < vp->patterns_size; j++)
    {
      if (vp_pattern_spec_eval(vp->patterns[j], name))
        inc = vp->patterns[j]->include;
    }

  if (!inc)
    return FALSE;

  sb = sb_th_gstring_acquire();

  g_string_append_len(sb_th_gstring_string(sb), value, value_len);
  sb->type_hint = TYPE_HINT_STRING;
  g_tree_insert(scope_set, vp_transform_apply(vp, name), sb);

  return FALSE;
}

static gboolean
vp_find_in_set(ValuePairs *vp, gchar *name, gboolean exclude)
{
  guint j;
  gboolean included = exclude;

  for (j = 0; j < vp->patterns_size; j++)
    {
      if (vp_pattern_spec_eval(vp->patterns[j], name))
        included = vp->patterns[j]->include;
    }

  return included;
}

static void
vp_merge_other_set(ValuePairs *vp, LogMessage *msg, gint32 seq_num, gint time_zone_mode, ValuePairSpec *set, GTree *dest, const LogTemplateOptions *template_options, gboolean exclude)
{
  gint i;
  SBTHGString *sb;

  for (i = 0; set[i].name; i++)
    {
      if (!vp_find_in_set(vp, set[i].name, exclude))
        continue;

      sb = sb_th_gstring_acquire();

      switch (set[i].type)
        {
        case VPT_MACRO:
          log_macro_expand(sb_th_gstring_string(sb), set[i].id, FALSE,
                           template_options, time_zone_mode, seq_num, NULL, msg);
          break;
        case VPT_NVPAIR:
          {
            const gchar *nv;
            gssize len;

            nv = log_msg_get_value(msg, (NVHandle) set[i].id, &len);
            g_string_append_len(sb_th_gstring_string(sb), nv, len);
            break;
          }
        default:
          g_assert_not_reached();
        }

      if (sb_th_gstring_string(sb)->len == 0)
        {
          sb_th_gstring_release(sb);
          continue;
        }

      g_tree_insert(dest, vp_transform_apply(vp, set[i].name), sb);
    }
}

/* runs over the all macros and merges the selected ones by the pattern into the value-pair set */
static void
vp_merge_macros(ValuePairs *vp, LogMessage *msg, gint32 seq_num, gint time_zone_mode, GTree *dest, const LogTemplateOptions *template_options)
{
  vp_merge_other_set(vp, msg, seq_num, time_zone_mode, all_macros, dest, template_options, FALSE);
}

/* runs over a set of ValuePairSpec structs and merges them into the value-pair set */
static void
vp_merge_set(ValuePairs *vp, LogMessage *msg, gint32 seq_num, gint time_zone_mode, ValuePairSpec *set, GTree *dest, const LogTemplateOptions *template_options)
{
  vp_merge_other_set(vp, msg, seq_num, time_zone_mode, set, dest, template_options, TRUE);
}

static gboolean
vp_foreach_helper (const gchar *name, const SBTHGString *hinted_value,
                   gpointer data)
{
  VPForeachFunc func = ((gpointer *)data)[0];
  gpointer user_data = ((gpointer *)data)[1];
  gboolean *r = ((gpointer *)data)[2];

  *r &= !func(name, hinted_value->type_hint,
              sb_th_gstring_string(hinted_value)->str, user_data);
  return !*r;
}

static void
vp_data_free (SBTHGString *s)
{
  sb_th_gstring_release (s);
}

gboolean
value_pairs_foreach_sorted (ValuePairs *vp, VPForeachFunc func,
                            GCompareDataFunc compare_func,
                            LogMessage *msg, gint32 seq_num, gint time_zone_mode,
                            const LogTemplateOptions *template_options,
                            gpointer user_data)
{
  gpointer args[] = { vp, func, msg, GINT_TO_POINTER (seq_num), user_data, NULL,
                      /* remove constness, we are not using that pointer non-const anyway */
                      (LogTemplateOptions *) template_options, GINT_TO_POINTER(time_zone_mode)
                    };
  gboolean result = TRUE;
  gpointer helper_args[] = { func, user_data, &result };
  GTree *scope_set;

  scope_set = g_tree_new_full((GCompareDataFunc)compare_func, NULL,
                              (GDestroyNotify)g_free,
                              (GDestroyNotify)vp_data_free);
  args[5] = scope_set;

  /*
   * Build up the base set
   */
  if (vp->scopes & (VPS_NV_PAIRS + VPS_DOT_NV_PAIRS + VPS_SDATA + VPS_RFC5424) ||
      vp->patterns_size > 0)
    nv_table_foreach(msg->payload, logmsg_registry,
                     (NVTableForeachFunc) vp_msg_nvpairs_foreach, args);

  if (vp->patterns_size > 0)
    vp_merge_macros(vp, msg, seq_num, time_zone_mode, scope_set, template_options);

  if (vp->scopes & (VPS_RFC3164 + VPS_RFC5424 + VPS_SELECTED_MACROS))
    vp_merge_set(vp, msg, seq_num, time_zone_mode, rfc3164, scope_set, template_options);

  if (vp->scopes & VPS_RFC5424)
    vp_merge_set(vp, msg, seq_num, time_zone_mode, rfc5424, scope_set, template_options);

  if (vp->scopes & VPS_SELECTED_MACROS)
    vp_merge_set(vp, msg, seq_num, time_zone_mode, selected_macros, scope_set, template_options);

  if (vp->scopes & VPS_ALL_MACROS)
    vp_merge_set(vp, msg, seq_num, time_zone_mode, all_macros, scope_set, template_options);

  /* Merge the explicit key-value pairs too */
  g_ptr_array_foreach(vp->vpairs, (GFunc)vp_pairs_foreach, args);

  /* Aaand we run it through the callback! */
  g_tree_foreach(scope_set, (GTraverseFunc)vp_foreach_helper, helper_args);

  g_tree_destroy(scope_set);

  return result;
}

gboolean
value_pairs_foreach(ValuePairs *vp, VPForeachFunc func,
                    LogMessage *msg, gint32 seq_num, gint time_zone_mode,
                    const LogTemplateOptions *template_options,
                    gpointer user_data)
{
  return value_pairs_foreach_sorted(vp, func, (GCompareDataFunc) strcmp,
                                    msg, seq_num, time_zone_mode, template_options, user_data);
}

static gboolean
value_pairs_debug_append (const gchar *name, TypeHint type, const gchar *value, gpointer user_data)
{
  GString *text = (GString *) user_data;
  g_string_append_printf(text, "%s=%s ",name, value);
  return FALSE;
};

EVTTAG *
evt_tag_value_pairs(const char* key, ValuePairs *vp, LogMessage *msg, gint32 seq_num, gint time_zone_mode, LogTemplateOptions *template_options)
{
   GString *debug_text = g_string_new("");
   EVTTAG* result;

   value_pairs_foreach(vp, value_pairs_debug_append, msg, seq_num, time_zone_mode, template_options, debug_text);

   result = evt_tag_str(key, debug_text->str);

   g_string_free(debug_text, TRUE);
   return result;
};

/*******************************************************************************
 * vp_stack (represented by vp_stack_t)
 *
 * A not very generic stack implementation used by vp_walker.
 *******************************************************************************/

#define VP_STACK_INITIAL_SIZE 16

typedef struct
{
  GPtrArray *elems;
} vp_stack_t;

static void
vp_stack_init(vp_stack_t *stack)
{
  stack->elems = g_ptr_array_sized_new(VP_STACK_INITIAL_SIZE);
}

static void
vp_stack_destroy(vp_stack_t *stack)
{
  g_ptr_array_free(stack->elems, TRUE);
}

static void
vp_stack_push(vp_stack_t *stack, gpointer data)
{
  g_ptr_array_add(stack->elems, data);
}

static gpointer
vp_stack_peek(vp_stack_t *stack)
{
  if (stack->elems->len == 0)
    return NULL;

  return g_ptr_array_index(stack->elems, stack->elems->len - 1);
}

static gpointer
vp_stack_pop(vp_stack_t *stack)
{
  gpointer data = NULL;

  data = vp_stack_peek(stack);
  if (data)
    g_ptr_array_remove_index(stack->elems, stack->elems->len - 1);
  return data;
}

static guint
vp_stack_height(vp_stack_t *stack)
{
  return stack->elems->len;
}

/*******************************************************************************
 * vp_walker (represented by vp_walk_state_t),
 *
 * The stuff that translates name-value pairs to a tree with SAX like
 * callbacks. (start/value/end)
 *******************************************************************************/

typedef struct
{
  gchar *key;
  gchar *prefix;
  gint prefix_len;

  gpointer data;
} vp_walk_stack_data_t;

typedef struct
{
  VPWalkCallbackFunc obj_start;
  VPWalkCallbackFunc obj_end;
  VPWalkValueCallbackFunc process_value;

  gpointer user_data;
  vp_stack_t stack;
} vp_walk_state_t;

static vp_walk_stack_data_t *
vp_walker_stack_push (vp_stack_t *stack,
                      gchar *key, gchar *prefix)
{
  vp_walk_stack_data_t *nt = g_new(vp_walk_stack_data_t, 1);

  nt->key = key;
  nt->prefix = prefix;
  nt->prefix_len = strlen(nt->prefix);
  nt->data = NULL;

  vp_stack_push(stack, nt);
  return nt;
}

static vp_walk_stack_data_t *
vp_walker_stack_peek(vp_stack_t *stack)
{
  return (vp_walk_stack_data_t *) vp_stack_peek(stack);
}

static vp_walk_stack_data_t *
vp_walker_stack_pop(vp_stack_t *stack)
{
  return (vp_walk_stack_data_t *) vp_stack_pop(stack);
}

static void
vp_walker_free_stack_data(vp_walk_stack_data_t *t)
{
  g_free(t->key);
  g_free(t->prefix);
  g_free(t);
}

static void
vp_walker_stack_unwind_containers_until(vp_walk_state_t *state,
                                        const gchar *name)
{
  vp_walk_stack_data_t *t;

  while ((t = vp_walker_stack_pop(&state->stack)) != NULL)
    {
      vp_walk_stack_data_t *p;

      if (name && strncmp(name, t->prefix, t->prefix_len) == 0)
        {
          /* This one matched, put it back, PUT IT BACK! */
          vp_stack_push(&state->stack, t);
          break;
        }

      p = vp_walker_stack_peek(&state->stack);

      if (p)
        state->obj_end(t->key, t->prefix, &t->data,
                       p->prefix, &p->data,
                       state->user_data);
      else
        state->obj_end(t->key, t->prefix, &t->data,
                       NULL, NULL,
                       state->user_data);
      vp_walker_free_stack_data(t);
    }
}

static void
vp_walker_stack_unwind_all_containers(vp_walk_state_t *state)
{
  vp_walker_stack_unwind_containers_until(state, NULL);
}

static gint
vp_walker_skip_sdata_enterprise_id(const gchar *name, gint pos)
{
  /* parse .SDATA.foo@1234.56.678 format, starting with the '@'
     character. Assume that any numbers + dots form part of the
     "foo@1234.56.678" key, even if they contain dots */
  do
    {
      /* skip @ or . */
      pos++;
      pos += strspn(&name[pos], "0123456789");
    }
  while (name[pos] == '.' && isdigit(name[pos + 1]));
  return pos;
}

static GPtrArray *
vp_walker_split_name_to_tokens(vp_walk_state_t *state, const gchar *name)
{
  gint pos, token_start = 0, name_len = strlen(name);
  GPtrArray *array = g_ptr_array_sized_new(VP_STACK_INITIAL_SIZE);

  pos = 0;
  do
    {
      token_start = pos;
      pos += strcspn(&name[pos], "@.");
      if (name[pos] == '@')
        pos = vp_walker_skip_sdata_enterprise_id(name, pos);

      if (name[pos] == '.' || pos == name_len)
        {
          g_ptr_array_add(array, g_strndup(&name[token_start], pos - token_start));
          pos++;
        }
    }
  while (pos < name_len);

  if (array->len == 0)
    {
      g_ptr_array_free(array, TRUE);
      return NULL;
    }

  return array;
}

static gchar *
vp_walker_name_combine_prefix(GPtrArray *tokens, gint until)
{
  SBGString *s = sb_gstring_acquire();
  gchar *str;
  gint i;

  for (i = 0; i < until; i++)
    {
      g_string_append(sb_gstring_string(s), g_ptr_array_index(tokens, i));
      g_string_append_c(sb_gstring_string(s), '.');
    }
  g_string_append(sb_gstring_string(s), g_ptr_array_index(tokens, until));

  str = g_strdup(sb_gstring_string(s)->str);

  sb_gstring_release(s);

  return str;
}

static gchar *
vp_walker_start_containers_for_name(vp_walk_state_t *state,
                                    const gchar *name)
{
  GPtrArray *tokens;
  gchar *key = NULL;
  guint i, start;

  tokens = vp_walker_split_name_to_tokens(state, name);

  start = vp_stack_height(&state->stack);
  for (i = start; i < tokens->len - 1; i++)
    {
      vp_walk_stack_data_t *p, *nt;

      p = vp_walker_stack_peek(&state->stack);
      nt = vp_walker_stack_push(&state->stack,
                                g_strdup(g_ptr_array_index(tokens, i)),
                                vp_walker_name_combine_prefix(tokens, i));

      if (p)
        state->obj_start(nt->key, nt->prefix, &nt->data,
                         p->prefix, &p->data,
                         state->user_data);
      else
        state->obj_start(nt->key, nt->prefix, &nt->data,
                         NULL, NULL, state->user_data);
    }

  /* The last token is the key (well, second to last, last being
     NULL), so treat that normally. */
  key = g_ptr_array_index(tokens, tokens->len - 1);
  g_ptr_array_index(tokens, tokens->len - 1) = NULL;

  g_ptr_array_foreach(tokens, (GFunc)g_free, NULL);
  g_ptr_array_free(tokens, TRUE);

  return key;
}

static gboolean
value_pairs_walker(const gchar *name, TypeHint type, const gchar *value,
                   gpointer user_data)
{
  vp_walk_state_t *state = (vp_walk_state_t *)user_data;
  vp_walk_stack_data_t *data;
  gchar *key;
  gboolean result;

  vp_walker_stack_unwind_containers_until(state, name);
  key = vp_walker_start_containers_for_name(state, name);
  data = vp_walker_stack_peek(&state->stack);

  if (data != NULL)
    result = state->process_value(key, data->prefix,
                                  type, value,
                                  &data->data,
                                  state->user_data);
  else
    result = state->process_value(key, NULL,
                                  type, value,
                                  NULL,
                                  state->user_data);

  g_free(key);

  return result;
}

static gint
vp_walk_cmp(const gchar *s1, const gchar *s2)
{
  return strcmp(s2, s1);
}

/*******************************************************************************
 * Public API
 *******************************************************************************/

gboolean
value_pairs_walk(ValuePairs *vp,
                 VPWalkCallbackFunc obj_start_func,
                 VPWalkValueCallbackFunc process_value_func,
                 VPWalkCallbackFunc obj_end_func,
                 LogMessage *msg, gint32 seq_num, gint time_zone_mode,
                 const LogTemplateOptions *template_options,
                 gpointer user_data)
{
  vp_walk_state_t state;
  gboolean result;

  state.user_data = user_data;
  state.obj_start = obj_start_func;
  state.obj_end = obj_end_func;
  state.process_value = process_value_func;
  vp_stack_init(&state.stack);

  state.obj_start(NULL, NULL, NULL, NULL, NULL, user_data);
  result = value_pairs_foreach_sorted(vp, value_pairs_walker,
                                      (GCompareDataFunc)vp_walk_cmp, msg,
                                      seq_num, time_zone_mode, template_options, &state);
  vp_walker_stack_unwind_all_containers(&state);
  state.obj_end(NULL, NULL, NULL, NULL, NULL, user_data);
  vp_stack_destroy(&state.stack);

  return result;
}

static void
value_pairs_init_set(ValuePairSpec *set)
{
  gint i;

  for (i = 0; set[i].name; i++)
    {
      guint id;
      gchar *name;

      name = set[i].alt_name ? set[i].alt_name : set[i].name;

      if ((id = log_macro_lookup(name, strlen(name))))
        {
          set[i].type = VPT_MACRO;
          set[i].id = id;
        }
      else
        {
          set[i].type = VPT_NVPAIR;
          set[i].id = log_msg_get_value_handle(name);
        }
    }
}

static void
vp_free_pair(VPPairConf *vpc)
{
  log_template_unref(vpc->template);
  g_free(vpc->name);
  g_free(vpc);
}

ValuePairs *
value_pairs_ref(ValuePairs *self)
{
  g_assert(!self || g_atomic_counter_get(&self->ref_cnt) > 0);

  if (self)
    {
      g_atomic_counter_inc(&self->ref_cnt);
    }
  return self;
}

void
value_pairs_unref(ValuePairs *self)
{
  g_assert(!self || g_atomic_counter_get(&self->ref_cnt));

  if (g_atomic_counter_dec_and_test(&self->ref_cnt))
    {
       value_pairs_free(self);
    }
}

ValuePairs *
value_pairs_new(void)
{
  ValuePairs *vp;
  gint i = 0;
  GArray *a;

  vp = g_new0(ValuePairs, 1);
   g_atomic_counter_set(&vp->ref_cnt, 1);
  vp->vpairs = g_ptr_array_sized_new(8);

  if (!value_pair_sets_initialized)
    {

      /* NOTE: that we're being only called during config parsing,
       * thus this code is never threaded. And we only want to perform
       * it once anyway. If it would be threaded, we'd need to convert
       * this to a value_pairs_init() function called before anything
       * else. */

      value_pairs_init_set(rfc3164);
      value_pairs_init_set(rfc5424);
      value_pairs_init_set(selected_macros);

      a = g_array_new(TRUE, TRUE, sizeof(ValuePairSpec));
      for (i = 0; macros[i].name; i++)
        {
          ValuePairSpec pair;

          pair.name = macros[i].name;
          pair.type = VPT_MACRO;
          pair.id = macros[i].id;
          g_array_append_val(a, pair);
        }
      all_macros = (ValuePairSpec *) g_array_free(a, FALSE);

      value_pair_sets_initialized = TRUE;
    }

  return vp;
}

ValuePairs *
value_pairs_new_default(GlobalConfig *cfg)
{
  ValuePairs *vp = value_pairs_new();

  value_pairs_add_scope(vp, "selected-macros");
  value_pairs_add_scope(vp, "nv-pairs");
  value_pairs_add_scope(vp, "sdata");
  return vp;
}

void
value_pairs_free (ValuePairs *vp)
{
  guint i;
  GList *l;

  for (i = 0; i < vp->vpairs->len; i++)
    vp_free_pair(g_ptr_array_index(vp->vpairs, i));

  g_ptr_array_free(vp->vpairs, TRUE);

  for (i = 0; i < vp->patterns_size; i++)
    {
      vp_pattern_spec_free(vp->patterns[i]);
    }
  g_free(vp->patterns);

  l = vp->transforms;
  while (l)
    {
      value_pairs_transform_set_free((ValuePairsTransformSet *)l->data);

      l = g_list_delete_link (l, l);
    }

  g_free(vp);
}

void
value_pairs_add_transforms(ValuePairs *vp, gpointer vpts)
{
  vp->transforms = g_list_append(vp->transforms, vpts);
}

/*******************************************************************************
 * Command line parser
 *******************************************************************************/

static void
vp_cmdline_parse_rekey_finish (gpointer data)
{
  gpointer *args = (gpointer *) data;
  ValuePairs *vp = (ValuePairs *) args[1];
  ValuePairsTransformSet *vpts = (ValuePairsTransformSet *) args[2];

  if (vpts)
    value_pairs_add_transforms (vp, args[2]);
  args[2] = NULL;
  g_free(args[3]);
  args[3] = NULL;
}

/* parse a value-pair specification from a command-line like environment */
static gboolean
vp_cmdline_parse_scope(const gchar *option_name, const gchar *value,
                       gpointer data, GError **error)
{
  gpointer *args = (gpointer *) data;
  ValuePairs *vp = (ValuePairs *) args[1];
  gchar **scopes;
  gint i;

  vp_cmdline_parse_rekey_finish (data);

  scopes = g_strsplit (value, ",", -1);
  for (i = 0; scopes[i] != NULL; i++)
    {
      if (!value_pairs_add_scope (vp, scopes[i]))
        {
          g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                       "Error parsing value-pairs: unknown scope %s", scopes[i]);
          g_strfreev (scopes);
          return FALSE;
        }
    }
  g_strfreev (scopes);

  return TRUE;
}

static gboolean
vp_cmdline_parse_exclude(const gchar *option_name, const gchar *value,
                         gpointer data, GError **error)
{
  gpointer *args = (gpointer *) data;
  ValuePairs *vp = (ValuePairs *) args[1];
  gchar **excludes;
  gint i;

  vp_cmdline_parse_rekey_finish (data);

  excludes = g_strsplit(value, ",", -1);
  for (i = 0; excludes[i] != NULL; i++)
    value_pairs_add_glob_pattern(vp, excludes[i], FALSE);
  g_strfreev(excludes);

  return TRUE;
}

static void
vp_cmdline_start_key(gpointer data, const gchar *key)
{
  gpointer *args = (gpointer *) data;

  vp_cmdline_parse_rekey_finish (data);
  args[3] = g_strdup(key);
}

static gboolean
vp_cmdline_parse_key(const gchar *option_name, const gchar *value,
		      gpointer data, GError **error)
{
  gpointer *args = (gpointer *) data;
  ValuePairs *vp = (ValuePairs *) args[1];
  gchar **keys;
  gint i;

  vp_cmdline_start_key(data, value);

  keys = g_strsplit(value, ",", -1);
  for (i = 0; keys[i] != NULL; i++)
    value_pairs_add_glob_pattern(vp, keys[i], TRUE);
  g_strfreev(keys);

  return TRUE;
}

static gboolean
vp_cmdline_parse_rekey(const gchar *option_name, const gchar *value,
                       gpointer data, GError **error)
{
  vp_cmdline_start_key(data, value);
  return TRUE;
}

static void
value_pairs_parse_type(gchar *spec, gchar **value, gchar **type)
{
  char *sp, *ep;

  *type = NULL;
  sp = spec;

  while (g_ascii_isalnum(*sp) || (*sp) == '_')
    sp++;

  while (*sp == ' ' || *sp == '\t')
    sp++;

  if (*sp != '(' ||
      !((g_ascii_toupper(spec[0]) >= 'A' &&
         g_ascii_toupper(spec[0]) <= 'Z') ||
        spec[0] == '_'))
    {
      *value = spec;
      return;
    }

  ep = strchr(sp, ')');
  if (ep == NULL || ep[1] != '\0')
    {
      *value = spec;
      return;
    }

  *value = sp + 1;
  *type = spec;
  sp[0] = '\0';
  ep[0] = '\0';
}

static gboolean
vp_cmdline_parse_pair (const gchar *option_name, const gchar *value,
		       gpointer data, GError **error)
{
  gpointer *args = (gpointer *) data;
  ValuePairs *vp = (ValuePairs *) args[1];
  GlobalConfig *cfg = (GlobalConfig *) args[0];
  gchar **kv, *v, *t;
  gboolean res = FALSE;
  LogTemplate *template;

  vp_cmdline_parse_rekey_finish (data);

  if (strchr(value, '=') == NULL)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		   "Error parsing value-pairs: expected an equal sign in key=value pair");
      return FALSE;
    }

  kv = g_strsplit(value, "=", 2);
  value_pairs_parse_type(kv[1], &v, &t);

  template = log_template_new(cfg, NULL);
  if (!log_template_compile(template, v, error))
    goto error;
  if (!log_template_set_type_hint(template, t, error))
    goto error;

  value_pairs_add_pair(vp, kv[0], template);

  res = TRUE;
 error:
  log_template_unref(template);
  g_strfreev(kv);

  return res;
}

static gboolean
vp_cmdline_parse_pair_or_key (const gchar *option_name, const gchar *value,
		              gpointer data, GError **error)
{
  if (strchr(value, '=') == NULL)
    return vp_cmdline_parse_key(option_name, value, data, error);
  else
    return vp_cmdline_parse_pair(option_name, value, data, error);
}

static ValuePairsTransformSet *
vp_cmdline_rekey_verify (gchar *key, ValuePairsTransformSet *vpts,
                         gpointer data)
{
  gpointer *args = (gpointer *)data;

  if (!vpts)
    {
      if (!key)
        return NULL;
      vpts = value_pairs_transform_set_new (key);
      vp_cmdline_parse_rekey_finish (data);
      args[2] = vpts;
      return vpts;
    }
  return vpts;
}


static gboolean
vp_cmdline_parse_rekey_replace_prefix (const gchar *option_name, const gchar *value,
                                       gpointer data, GError **error)
{
  gpointer *args = (gpointer *) data;
  ValuePairsTransformSet *vpts = (ValuePairsTransformSet *) args[2];
  gchar *key = (gchar *) args[3];
  gchar **kv;

  vpts = vp_cmdline_rekey_verify (key, vpts, data);
  if (!vpts)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   "Error parsing value-pairs: --replace-prefix used without --key or --rekey");
      return FALSE;
    }

  if (!g_strstr_len (value, strlen (value), "="))
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "Error parsing value-pairs: rekey replace-prefix construct should be in the format string=replacement");
      return FALSE;
    }

  kv = g_strsplit(value, "=", 2);
  value_pairs_transform_set_add_func
    (vpts, value_pairs_new_transform_replace_prefix (kv[0], kv[1]));

  g_free (kv[0]);
  g_free (kv[1]);
  g_free (kv);

  return TRUE;
}

static gboolean
vp_cmdline_parse_rekey_add_prefix (const gchar *option_name, const gchar *value,
                                   gpointer data, GError **error)
{
  gpointer *args = (gpointer *) data;
  ValuePairsTransformSet *vpts = (ValuePairsTransformSet *) args[2];
  gchar *key = (gchar *) args[3];

  vpts = vp_cmdline_rekey_verify (key, vpts, data);
  if (!vpts)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   "Error parsing value-pairs: --add-prefix used without --key or --rekey");
      return FALSE;
    }

  value_pairs_transform_set_add_func
    (vpts, value_pairs_new_transform_add_prefix (value));
  return TRUE;
}

static gboolean
vp_cmdline_parse_rekey_shift (const gchar *option_name, const gchar *value,
                              gpointer data, GError **error)
{
  gpointer *args = (gpointer *) data;
  ValuePairsTransformSet *vpts = (ValuePairsTransformSet *) args[2];
  gchar *key = (gchar *) args[3];

  vpts = vp_cmdline_rekey_verify (key, vpts, data);
  if (!vpts)
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   "Error parsing value-pairs: --shift used without --key or --rekey");
      return FALSE;
    }

  value_pairs_transform_set_add_func
    (vpts, value_pairs_new_transform_shift (atoi (value)));
  return TRUE;
}

ValuePairs *
value_pairs_new_from_cmdline (GlobalConfig *cfg,
			      gint argc, gchar **argv,
			      GError **error)
{
  ValuePairs *vp;
  GOptionContext *ctx;

  GOptionEntry vp_options[] = {
    { "scope", 's', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_scope,
      NULL, NULL },
    { "exclude", 'x', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_exclude,
      NULL, NULL },
    { "key", 'k', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_key,
      NULL, NULL },
    { "rekey", 'r', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_rekey,
      NULL, NULL },
    { "pair", 'p', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_pair,
      NULL, NULL },
    { "shift", 'S', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_rekey_shift,
      NULL, NULL },
    { "add-prefix", 'A', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_rekey_add_prefix,
      NULL, NULL },
    { "replace-prefix", 'R', 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_rekey_replace_prefix,
      NULL, NULL },
    { "replace", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_CALLBACK,
      vp_cmdline_parse_rekey_replace_prefix, NULL, NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_CALLBACK, vp_cmdline_parse_pair_or_key,
      NULL, NULL },
    { NULL }
  };
  GOptionGroup *og;
  gpointer user_data_args[4];
  gboolean success;

  vp = value_pairs_new();
  user_data_args[0] = cfg;
  user_data_args[1] = vp;
  user_data_args[2] = NULL;
  user_data_args[3] = NULL;

  ctx = g_option_context_new ("value-pairs");
  og = g_option_group_new (NULL, NULL, NULL, user_data_args, NULL);
  g_option_group_add_entries (og, vp_options);
  g_option_context_set_main_group (ctx, og);

  success = g_option_context_parse (ctx, &argc, &argv, error);
  vp_cmdline_parse_rekey_finish (user_data_args);
  g_option_context_free (ctx);

  if (!success)
    {
      value_pairs_unref (vp);
      vp = NULL;
    }

  return vp;
}
