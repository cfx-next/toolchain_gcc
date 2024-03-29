/* Callgraph construction.
   Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010
   Free Software Foundation, Inc.
   Contributed by Jan Hubicka

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "tree-flow.h"
#include "langhooks.h"
#include "pointer-set.h"
#include "cgraph.h"
#include "intl.h"
#include "gimple.h"
#include "toplev.h"
#include "gcov-io.h"
#include "coverage.h"
#include "tree-pass.h"
#include "ipa-utils.h"
#include "except.h"
#include "l-ipo.h"

/* Context of record_reference.  */
struct record_reference_ctx
{
  bool only_vars;
  struct varpool_node *varpool_node;
};

/* Walk tree and record all calls and references to functions/variables.
   Called via walk_tree: TP is pointer to tree to be examined.
   When DATA is non-null, record references to callgraph.
   */

static tree
record_reference (tree *tp, int *walk_subtrees, void *data)
{
  tree t = *tp;
  tree decl;
  struct record_reference_ctx *ctx = (struct record_reference_ctx *)data;

  switch (TREE_CODE (t))
    {
    case VAR_DECL:
    case FUNCTION_DECL:
      gcc_unreachable ();
      break;

    case FDESC_EXPR:
    case ADDR_EXPR:
      /* Record dereferences to the functions.  This makes the
	 functions reachable unconditionally.  */
      decl = get_base_var (*tp);
      if (TREE_CODE (decl) == FUNCTION_DECL)
	{
	  if (!ctx->only_vars)
	  cgraph_mark_address_taken_node (cgraph_node (decl));
	  ipa_record_reference (NULL, ctx->varpool_node,
			        cgraph_node (decl), NULL,
			        IPA_REF_ADDR, NULL);
	}

      if (TREE_CODE (decl) == VAR_DECL)
	{
	  struct varpool_node *vnode = varpool_node (decl);
	  if (lang_hooks.callgraph.analyze_expr)
	    lang_hooks.callgraph.analyze_expr (&decl, walk_subtrees);
	  varpool_mark_needed_node (vnode);
	  if (vnode->alias && vnode->extra_name)
	    vnode = vnode->extra_name;
	  ipa_record_reference (NULL, ctx->varpool_node,
				NULL, vnode,
				IPA_REF_ADDR, NULL);
	}
      *walk_subtrees = 0;
      break;

    default:
      /* Save some cycles by not walking types and declaration as we
	 won't find anything useful there anyway.  */
      if (IS_TYPE_OR_DECL_P (*tp))
	{
	  *walk_subtrees = 0;
	  break;
	}

      if ((unsigned int) TREE_CODE (t) >= LAST_AND_UNUSED_TREE_CODE)
	return lang_hooks.callgraph.analyze_expr (tp, walk_subtrees);
      break;
    }

  return NULL_TREE;
}

/* Record references to typeinfos in the type list LIST.  */

static void
record_type_list (struct cgraph_node *node, tree list)
{
  for (; list; list = TREE_CHAIN (list))
    {
      tree type = TREE_VALUE (list);
      
      if (TYPE_P (type))
	type = lookup_type_for_runtime (type);
      STRIP_NOPS (type);
      if (TREE_CODE (type) == ADDR_EXPR)
	{
	  type = TREE_OPERAND (type, 0);
	  if (TREE_CODE (type) == VAR_DECL)
	    {
	      struct varpool_node *vnode = varpool_node (type);
	      varpool_mark_needed_node (vnode);
	      ipa_record_reference (node, NULL,
				    NULL, vnode,
				    IPA_REF_ADDR, NULL);
	    }
	}
    }
}

/* Record all references we will introduce by producing EH tables
   for NODE.  */

static void
record_eh_tables (struct cgraph_node *node, struct function *fun)
{
  eh_region i;

  if (DECL_FUNCTION_PERSONALITY (node->decl))
    ipa_record_reference (node, NULL,
			  cgraph_node (DECL_FUNCTION_PERSONALITY (node->decl)),
			  NULL, IPA_REF_ADDR, NULL);

  i = fun->eh->region_tree;
  if (!i)
    return;

  while (1)
    {
      switch (i->type)
	{
	case ERT_CLEANUP:
	case ERT_MUST_NOT_THROW:
	  break;

	case ERT_TRY:
	  {
	    eh_catch c;
	    for (c = i->u.eh_try.first_catch; c; c = c->next_catch)
	      record_type_list (node, c->type_list);
	  }
	  break;

	case ERT_ALLOWED_EXCEPTIONS:
	  record_type_list (node, i->u.allowed.type_list);
	  break;
	}
      /* If there are sub-regions, process them.  */
      if (i->inner)
	i = i->inner;
      /* If there are peers, process them.  */
      else if (i->next_peer)
	i = i->next_peer;
      /* Otherwise, step back up the tree to the next peer.  */
      else
	{
	  do
	    {
	      i = i->outer;
	      if (i == NULL)
		return;
	    }
	  while (i->next_peer == NULL);
	  i = i->next_peer;
	}
    }
}

/* Reset inlining information of all incoming call edges of NODE.  */

void
reset_inline_failed (struct cgraph_node *node)
{
  struct cgraph_edge *e;

  for (e = node->callers; e; e = e->next_caller)
    {
      e->callee->global.inlined_to = NULL;
      if (!node->analyzed)
	e->inline_failed = CIF_BODY_NOT_AVAILABLE;
      else if (node->local.redefined_extern_inline)
	e->inline_failed = CIF_REDEFINED_EXTERN_INLINE;
      else if (!node->local.inlinable)
	e->inline_failed = CIF_FUNCTION_NOT_INLINABLE;
      else if (e->call_stmt_cannot_inline_p)
	e->inline_failed = CIF_MISMATCHED_ARGUMENTS;
      else
	e->inline_failed = CIF_FUNCTION_NOT_CONSIDERED;
    }
}

/* Computes the frequency of the call statement so that it can be stored in
   cgraph_edge.  BB is the basic block of the call statement.  */
int
compute_call_stmt_bb_frequency (tree decl, basic_block bb)
{
  int entry_freq = ENTRY_BLOCK_PTR_FOR_FUNCTION
  		     (DECL_STRUCT_FUNCTION (decl))->frequency;
  int freq = bb->frequency;

  if (profile_status_for_function (DECL_STRUCT_FUNCTION (decl)) == PROFILE_ABSENT)
    return CGRAPH_FREQ_BASE;

  if (!entry_freq)
    entry_freq = 1, freq++;

  freq = freq * CGRAPH_FREQ_BASE / entry_freq;
  if (freq > CGRAPH_FREQ_MAX)
    freq = CGRAPH_FREQ_MAX;

  return freq;
}


bool cgraph_pre_profiling_inlining_done = false;

/* Return true if E is a fake indirect call edge.  */

bool
cgraph_is_fake_indirect_call_edge (struct cgraph_edge *e)
{
  return !e->call_stmt;
}


/* Add fake cgraph edges from NODE to its indirect call callees
   using profile data.  */

static void
add_fake_indirect_call_edges (struct cgraph_node *node)
{
  unsigned n_counts, i;
  gcov_type *ic_counts;

  /* Enable this only for LIPO for now.  */
  if (!L_IPO_COMP_MODE)
    return;

  if (cgraph_pre_profiling_inlining_done)
    return;

  ic_counts
      = get_coverage_counts_no_warn (DECL_STRUCT_FUNCTION (node->decl),
                                     GCOV_COUNTER_ICALL_TOPNV, &n_counts);

  if (!ic_counts)
    return;

  gcc_assert ((n_counts % GCOV_ICALL_TOPN_NCOUNTS) == 0);

/* After the early_inline_1 before value profile transformation,
   functions that are indirect call targets may have their bodies
   removed (extern inline functions or functions from aux modules,
   functions in comdat etc) if all direct callsites are inlined. This
   will lead to missing inline opportunities after profile based
   indirect call promotion. The solution is to add fake edges to
   indirect call targets. Note that such edges are not associated
   with actual indirect call sites because it is not possible to
   reliably match pre-early-inline indirect callsites with indirect
   call profile counters which are from post-early inline function body.  */

  for (i = 0; i < n_counts;
       i += GCOV_ICALL_TOPN_NCOUNTS, ic_counts += GCOV_ICALL_TOPN_NCOUNTS)
    {
      gcov_type val1, val2, count1, count2;
      struct cgraph_node *direct_call1 = 0, *direct_call2 = 0;

      val1 = ic_counts[1];
      count1 = ic_counts[2];
      val2 = ic_counts[3];
      count2 = ic_counts[4];

      if (val1 == 0 || count1 == 0)
        continue;

      direct_call1 = find_func_by_global_id (val1);
      if (direct_call1)
        {
          tree decl = direct_call1->decl;
          cgraph_create_edge (node,
	                      cgraph_node (decl),
			      NULL,
                              count1, 0, 0);
        }

      if (val2 == 0 || count2 == 0)
        continue;
      direct_call2 = find_func_by_global_id (val2);
      if (direct_call2)
        {
          tree decl = direct_call2->decl;
          cgraph_create_edge (node,
	                      cgraph_node (decl),
                              NULL,
                              count2, 0, 0);
        }
    }
}


/* This can be implemented as an IPA pass that must be first one 
   before any unreachable node elimination. */

void
cgraph_add_fake_indirect_call_edges (void)
{
  struct cgraph_node *node;

  /* Enable this only for LIPO for now.  */
  if (!L_IPO_COMP_MODE)
    return;

  for (node = cgraph_nodes; node; node = node->next)
    {
      if (node->analyzed && (node->needed || node->reachable))
        add_fake_indirect_call_edges (node);
    }
}

/* Remove zero count fake edges added for the purpose of ensuring
   the right processing order.  This should be called after all
   small ipa passes.  */
void
cgraph_remove_zero_count_fake_edges (void)
{
  struct cgraph_node *node;

  /* Enable this only for LIPO for now.  */
  if (!L_IPO_COMP_MODE)
    return;

  for (node = cgraph_nodes; node; node = node->next)
    {
      if (node->analyzed && (node->needed || node->reachable))
        {
          struct cgraph_edge *e, *f;
	  for (e = node->callees; e; e = f)
	    {
	      f = e->next_callee;
	      if (!e->call_stmt && !e->count && !e->loop_nest
	          && !e->frequency)
                cgraph_remove_edge (e);
            }
	}
    }
}

/* Mark address taken in STMT.  */

static bool
mark_address (gimple stmt, tree addr, void *data)
{
  addr = get_base_address (addr);
  if (TREE_CODE (addr) == FUNCTION_DECL)
    {
      struct cgraph_node *node = cgraph_node (addr);
      cgraph_mark_address_taken_node (node);
      ipa_record_reference ((struct cgraph_node *)data, NULL,
			    node, NULL,
			    IPA_REF_ADDR, stmt);
    }
  else if (addr && TREE_CODE (addr) == VAR_DECL
	   && (TREE_STATIC (addr) || DECL_EXTERNAL (addr)))
    {
      struct varpool_node *vnode = varpool_node (addr);
      int walk_subtrees;

      if (lang_hooks.callgraph.analyze_expr)
	lang_hooks.callgraph.analyze_expr (&addr, &walk_subtrees);
      varpool_mark_needed_node (vnode);
      if (vnode->alias && vnode->extra_name)
	vnode = vnode->extra_name;
      ipa_record_reference ((struct cgraph_node *)data, NULL,
			    NULL, vnode,
			    IPA_REF_ADDR, stmt);
    }

  return false;
}

/* Mark load of T.  */

static bool
mark_load (gimple stmt, tree t, void *data)
{
  t = get_base_address (t);
  if (t && TREE_CODE (t) == FUNCTION_DECL)
    {
      /* ??? This can happen on platforms with descriptors when these are
	 directly manipulated in the code.  Pretend that it's an address.  */
      struct cgraph_node *node = cgraph_node (t);
      cgraph_mark_address_taken_node (node);
      ipa_record_reference ((struct cgraph_node *)data, NULL,
			    node, NULL,
			    IPA_REF_ADDR, stmt);
    }
  else if (t && TREE_CODE (t) == VAR_DECL
	   && (TREE_STATIC (t) || DECL_EXTERNAL (t)))
    {
      struct varpool_node *vnode = varpool_node (t);
      int walk_subtrees;

      if (lang_hooks.callgraph.analyze_expr)
	lang_hooks.callgraph.analyze_expr (&t, &walk_subtrees);
      varpool_mark_needed_node (vnode);
      if (vnode->alias && vnode->extra_name)
	vnode = vnode->extra_name;
      ipa_record_reference ((struct cgraph_node *)data, NULL,
			    NULL, vnode,
			    IPA_REF_LOAD, stmt);
    }
  return false;
}

/* Mark store of T.  */

static bool
mark_store (gimple stmt, tree t, void *data)
{
  t = get_base_address (t);
  if (t && TREE_CODE (t) == VAR_DECL
      && (TREE_STATIC (t) || DECL_EXTERNAL (t)))
    {
      struct varpool_node *vnode = varpool_node (t);
      int walk_subtrees;

      if (lang_hooks.callgraph.analyze_expr)
	lang_hooks.callgraph.analyze_expr (&t, &walk_subtrees);
      varpool_mark_needed_node (vnode);
      if (vnode->alias && vnode->extra_name)
	vnode = vnode->extra_name;
      ipa_record_reference ((struct cgraph_node *)data, NULL,
			    NULL, vnode,
			    IPA_REF_STORE, stmt);
     }
  return false;
}

/* Create cgraph edges for function calls.
   Also look for functions and variables having addresses taken.  */

static unsigned int
build_cgraph_edges (void)
{
  basic_block bb;
  struct cgraph_node *node = cgraph_node (current_function_decl);
  struct pointer_set_t *visited_nodes = pointer_set_create ();
  gimple_stmt_iterator gsi;
  tree decl;
  unsigned ix;

  /* Create the callgraph edges and record the nodes referenced by the function.
     body.  */
  FOR_EACH_BB (bb)
    {
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple stmt = gsi_stmt (gsi);
	  tree decl;

	  if (is_gimple_call (stmt))
	    {
	      int freq = compute_call_stmt_bb_frequency (current_function_decl,
							 bb);
	      decl = gimple_call_fndecl (stmt);
	      if (decl)
		cgraph_create_edge (node, cgraph_node (decl), stmt,
				    bb->count, freq,
				    bb->loop_depth);
	      else
		cgraph_create_indirect_edge (node, stmt,
					     gimple_call_flags (stmt),
					     bb->count, freq,
					     bb->loop_depth);
	    }
	  walk_stmt_load_store_addr_ops (stmt, node, mark_load,
					 mark_store, mark_address);
	  if (gimple_code (stmt) == GIMPLE_OMP_PARALLEL
	      && gimple_omp_parallel_child_fn (stmt))
	    {
	      tree fn = gimple_omp_parallel_child_fn (stmt);
	      ipa_record_reference (node, NULL, cgraph_node (fn),
				    NULL, IPA_REF_ADDR, stmt);
	    }
	  if (gimple_code (stmt) == GIMPLE_OMP_TASK)
	    {
	      tree fn = gimple_omp_task_child_fn (stmt);
	      if (fn)
		ipa_record_reference (node, NULL, cgraph_node (fn),
				      NULL, IPA_REF_ADDR, stmt);
	      fn = gimple_omp_task_copy_fn (stmt);
	      if (fn)
		ipa_record_reference (node, NULL, cgraph_node (fn),
				      NULL, IPA_REF_ADDR, stmt);
	    }
	}
      for (gsi = gsi_start (phi_nodes (bb)); !gsi_end_p (gsi); gsi_next (&gsi))
	walk_stmt_load_store_addr_ops (gsi_stmt (gsi), node,
				       mark_load, mark_store, mark_address);
   }


  /* Look for initializers of constant variables and private statics.  */
  FOR_EACH_LOCAL_DECL (cfun, ix, decl)
    if (TREE_CODE (decl) == VAR_DECL
	&& (TREE_STATIC (decl) && !DECL_EXTERNAL (decl)))
      varpool_finalize_decl (decl);
  record_eh_tables (node, cfun);

  pointer_set_destroy (visited_nodes);
  return 0;
}

struct gimple_opt_pass pass_build_cgraph_edges =
{
 {
  GIMPLE_PASS,
  "*build_cgraph_edges",			/* name */
  NULL,					/* gate */
  build_cgraph_edges,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_NONE,				/* tv_id */
  PROP_cfg,				/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0					/* todo_flags_finish */
 }
};

/* Record references to functions and other variables present in the
   initial value of DECL, a variable.
   When ONLY_VARS is true, we mark needed only variables, not functions.  */

void
record_references_in_initializer (tree decl, bool only_vars)
{
  struct pointer_set_t *visited_nodes = pointer_set_create ();
  struct varpool_node *node = varpool_node (decl);
  struct record_reference_ctx ctx = {false, NULL};

  ctx.varpool_node = node;
  ctx.only_vars = only_vars;
  walk_tree (&DECL_INITIAL (decl), record_reference,
             &ctx, visited_nodes);
  pointer_set_destroy (visited_nodes);
}

/* Rebuild cgraph edges for current function node.  This needs to be run after
   passes that don't update the cgraph.  */

unsigned int
rebuild_cgraph_edges (void)
{
  basic_block bb;
  struct cgraph_node *node = cgraph_node (current_function_decl);
  gimple_stmt_iterator gsi;

  cgraph_node_remove_callees (node);
  ipa_remove_all_references (&node->ref_list);

  node->count = ENTRY_BLOCK_PTR->count;
  node->max_bb_count = 0;

  FOR_EACH_BB (bb)
    {
      if (bb->count > node->max_bb_count)
	node->max_bb_count = bb->count;
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple stmt = gsi_stmt (gsi);
	  tree decl;

	  if (is_gimple_call (stmt))
	    {
	      int freq = compute_call_stmt_bb_frequency (current_function_decl,
							 bb);
	      decl = gimple_call_fndecl (stmt);
	      if (decl)
	        {
		  struct cgraph_node *callee;
		  /* In LIPO mode, before tree_profiling, the call graph edge
		     needs to be built with the original target node to make
		     sure consistent early inline decisions between profile generate
		     and profile use. After tree-profiling, the target needs to be
		     set to the resolved node so that ipa-inline sees the definitions.  */
		  if (L_IPO_COMP_MODE && cgraph_pre_profiling_inlining_done)
		    callee = cgraph_lipo_get_resolved_node (decl);
                  else
		    callee = cgraph_node (decl);
                  cgraph_create_edge (node, callee, stmt,
                                      bb->count, freq,
                                      bb->loop_depth);
                  if (L_IPO_COMP_MODE && cgraph_pre_profiling_inlining_done
		      && decl != callee->decl)
		    gimple_call_set_fndecl (stmt, callee->decl);
                }
	      else
		cgraph_create_indirect_edge (node, stmt,
					     gimple_call_flags (stmt),
					     bb->count, freq,
					     bb->loop_depth);
	    }
	  walk_stmt_load_store_addr_ops (stmt, node, mark_load,
					 mark_store, mark_address);

	}
      for (gsi = gsi_start (phi_nodes (bb)); !gsi_end_p (gsi); gsi_next (&gsi))
	walk_stmt_load_store_addr_ops (gsi_stmt (gsi), node,
				       mark_load, mark_store, mark_address);
    }
  add_fake_indirect_call_edges (node);
  record_eh_tables (node, cfun);
  gcc_assert (!node->global.inlined_to);

  return 0;
}

/* Rebuild cgraph edges for current function node.  This needs to be run after
   passes that don't update the cgraph.  */

void
cgraph_rebuild_references (void)
{
  basic_block bb;
  struct cgraph_node *node = cgraph_node (current_function_decl);
  gimple_stmt_iterator gsi;

  ipa_remove_all_references (&node->ref_list);

  node->count = ENTRY_BLOCK_PTR->count;

  FOR_EACH_BB (bb)
    {
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple stmt = gsi_stmt (gsi);

	  walk_stmt_load_store_addr_ops (stmt, node, mark_load,
					 mark_store, mark_address);

	}
      for (gsi = gsi_start (phi_nodes (bb)); !gsi_end_p (gsi); gsi_next (&gsi))
	walk_stmt_load_store_addr_ops (gsi_stmt (gsi), node,
				       mark_load, mark_store, mark_address);
    }
  record_eh_tables (node, cfun);
}

struct gimple_opt_pass pass_rebuild_cgraph_edges =
{
 {
  GIMPLE_PASS,
  "*rebuild_cgraph_edges",		/* name */
  NULL,					/* gate */
  rebuild_cgraph_edges,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_CGRAPH,				/* tv_id */
  PROP_cfg,				/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0,					/* todo_flags_finish */
 }
};

/* Defined in tree-optimize.c  */
extern bool cgraph_callee_edges_final_cleanup; 

static unsigned int
remove_cgraph_callee_edges (void)
{
  /* The -fcallgraph-profiles-sections flag needs the call-graph preserved
     till pass_final.  */
  if (cgraph_callee_edges_final_cleanup
      && flag_callgraph_profiles_sections)
      return 0;

  cgraph_node_remove_callees (cgraph_node (current_function_decl));
  return 0;
}

struct gimple_opt_pass pass_remove_cgraph_callee_edges =
{
 {
  GIMPLE_PASS,
  "*remove_cgraph_callee_edges",		/* name */
  NULL,					/* gate */
  remove_cgraph_callee_edges,		/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_NONE,				/* tv_id */
  0,					/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0,					/* todo_flags_finish */
 }
};
