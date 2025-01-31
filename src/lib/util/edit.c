/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file src/lib/util/edit.c
 * @brief Functions to edit pair lists, and track undo operations
 *
 * @copyright 2021 Network RADIUS SAS (legal@networkradius.com)
 */

RCSID("$Id$")

#include <freeradius-devel/util/value.h>
#include <freeradius-devel/util/talloc.h>
#include "edit.h"

typedef enum {
	FR_EDIT_INVALID = 0,
	FR_EDIT_DELETE,			//!< delete a VP
	FR_EDIT_VALUE,			//!< edit a VP in place
	FR_EDIT_CLEAR,			//!< clear the children of a structural entry.
	FR_EDIT_INSERT,			//!< insert a VP into a list, after another one.
} fr_edit_op_t;

#if 0
/*
 *	For debugging.
 */
static const char *edit_names[4] = {
	"invalid",
	"delete",
	"value",
	"clear",
	"insert",
};
#endif

/** Track a series of edits.
 *
 */
struct fr_edit_list_s {
	/*
	 *	List of edits to be made, in order.
	 */
	fr_dlist_head_t	list;

	/*
	 *	VPs which were inserted, and then over-written by a
	 *	later edit.
	 */
	fr_pair_list_t	deleted_pairs;
};

/** Track one particular edit.
 */
typedef struct {
	fr_edit_op_t	op;		//!< edit operation to perform
	fr_dlist_t	entry;		//!< linked list of edits

	fr_pair_t	*vp;		//!< pair edited, deleted, or inserted

	union {
		fr_value_box_t	data;	//!< original data
		fr_pair_list_t	children;  //!< original child list, for "clear"

		struct {
			fr_pair_list_t	*list; //!< parent list
			fr_pair_t	*ref;	//!< reference pair for delete, insert before/after
		};
	};
} fr_edit_t;


/** Undo one particular edit.
 */
static int edit_undo(fr_edit_t *e)
{
	fr_pair_t *vp = e->vp;
#ifndef NDEBUG
	int rcode;
#endif

	fr_assert(vp != NULL);
	PAIR_VERIFY(vp);

	switch (e->op) {
	case FR_EDIT_INVALID:
		return -1;

	case FR_EDIT_VALUE:
		fr_assert(fr_type_is_leaf(vp->vp_type));
		if (!fr_type_is_fixed_size(vp->vp_type)) fr_value_box_clear(&vp->data);
		fr_value_box_copy_shallow(NULL, &vp->data, &e->data);
		break;

	case FR_EDIT_CLEAR:
		fr_assert(fr_type_is_structural(vp->vp_type));

		fr_pair_list_free(&vp->vp_group);
		fr_pair_list_append(&vp->vp_group, &e->children);
		break;

	case FR_EDIT_DELETE:
		fr_assert(e->list != NULL);
#ifndef NDEBUG
		rcode =
#endif
		fr_pair_insert_after(e->list, e->ref, vp);
		fr_assert(rcode == 0);
		break;

	case FR_EDIT_INSERT:
		/*
		 *	We can free the VP here, as any edits to its'
		 *	children MUST come after the creation of the
		 *	VP.  And any deletion of VPs after this one
		 *	must come after this VP was created.
		 */
		fr_pair_delete(e->list, vp);
		break;
	}

	return 0;
}

/** Abort the entries in an edit list.
 *
 *  After this call, the input list(s) are unchanged from before any
 *  edits were made.
 *
 *  the caller does not have to call talloc_free(el);
 */
void fr_edit_list_abort(fr_edit_list_t *el)
{
	fr_edit_t *e;

	if (!el) return;

	/*
	 *      All of these pairs are already in the edit list.  They
	 *      have the correct parent, and will be placed back into
	 *      their correct location by edit_undo()
	 */
	fr_pair_list_init(&el->deleted_pairs);

	/*
	 *	Undo edits in reverse order, as later edits depend on
	 *	earlier ones.  We don't have multiple edits of the
	 *	same VP, but we can create a VP, and then later edit
	 *	its children.
	 */
	while ((e = fr_dlist_pop_tail(&el->list)) != NULL) {
		edit_undo(e);
		/*
		 *	Don't free "e", it will be cleaned up when we
		 *	talloc_free(el).  That should be somewhat
		 *	faster than doing it incrementally.
		 */
	};

	talloc_free(el);
}

/** Record one particular edit
 *
 *  For INSERT / DELETE, this function will also insert / delete the
 *  VP.
 *
 *  For VALUE changes, this function must be called BEFORE the value
 *  is changed.  Once this function has been called, it is then safe
 *  to edit the value in place.
 *
 *  Note that VALUE changes for structural types are allowed ONLY when
 *  using T_OP_SET, which over-writes previous values.  For every
 *  other modification to structural types, we MUST instead call
 *  insert / delete on the vp_group.
 */
static int edit_record(fr_edit_list_t *el, fr_edit_op_t op, fr_pair_t *vp, fr_pair_list_t *list, fr_pair_t *ref)
{
	fr_edit_t *e;

	fr_assert(el != NULL);
	fr_assert(vp != NULL);

	/*
	 *	Search for previous edits.
	 *
	 *	@todo - if we're modifying values of a child VP, and
	 *	it's parent is marked as INSERT, then we don't need to
	 *	record FR_EDIT_VALUE changes to the children.  It's
	 *	not yet clear how best to track this.
	 */
	for (e = fr_dlist_head(&el->list);
	     e != NULL;
	     e = fr_dlist_next(&el->list, e)) {
		fr_assert(e->vp != NULL);

		if (e->vp != vp) continue;

		switch (op) {
		case FR_EDIT_INVALID:
			return -1;

			/*
			 *	We're editing a previous edit.
			 *	There's no need to record anything
			 *	new, as we've already recorded the
			 *	original value.
			 *
			 *	Note that we can insert a pair and
			 *	then edit it.  The undo list only
			 *	saves the insert, as the later edit is
			 *	irrelevant.  If we're undoing, we
			 *	simply delete the new attribute which
			 *	was inserted.
			 */
		case FR_EDIT_VALUE:
			/*
			 *	If we delete a pair, we can't later
			 *	edit it.  That indicates serious
			 *	issues with the code.
			 *
			 *      However, if we previously inserted
			 *      this VP, then we don't need to record
			 *      changes to its value.  Similarly, if
			 *      we had previously changed its value,
			 *      we don't need to record that
			 *      information again.
                         */
			fr_assert(e->op != FR_EDIT_DELETE);
			fr_assert(fr_type_is_leaf(vp->vp_type));
			return 0;

			/*
			 *	We're inserting a new pair.
			 *
			 *	We can't have previously edited this
			 *	pair (inserted, deleted, or updated
			 *	the value), as the pair is new!
			 */
		case FR_EDIT_INSERT:
			fr_assert(0);
			return -1;

		case FR_EDIT_CLEAR:
			/*
			 *	If we're clearing it, we MUST have
			 *	previously inserted it.  So just nuke
			 *	it's children, as merging the
			 *	operations of "insert with stuff" and
			 *	then "clear" is just "insert empty
			 *	pair".
			 *
			 *	However, we don't yet delete the
			 *	children, as there may be other edit
			 *	operations which are referring to
			 *	them.
			 */
			fr_assert(e->op == FR_EDIT_INSERT);
			fr_assert(fr_type_is_structural(vp->vp_type));

			fr_pair_list_append(&el->deleted_pairs, &vp->vp_group);
			break;

			/*
			 *	We're being asked to delete something
			 *	we previously inserted, or previously
			 *	edited.
			 */
		case FR_EDIT_DELETE:
			/*
			 *	We can't delete something which was
			 *	already deleted.
			 */
			fr_assert(e->op != FR_EDIT_DELETE);

			/*
			 *	We had previously inserted it.  So
			 *	just delete the insert operation, and
			 *	delete the VP from the list.
			 *
			 *	Other edits may refer to children of
			 *	this pair.  So we don't free the VP
			 *	immediately, but instead reparent it
			 *	to the edit list.  So that when the
			 *	edit list is freed, the VP will be
			 *	freed.
			 */
			if (e->op == FR_EDIT_INSERT) {
				fr_assert(e->list == list);

				fr_pair_remove(list, vp);
				fr_pair_append(&el->deleted_pairs, vp);

				fr_dlist_remove(&el->list, e);
				talloc_free(e);
				return 0;
			}

			/*
			 *	We had previously changed the value,
			 *	but now we're going to delete it.
			 *
			 *	Since it had previously existed, we
			 *	have to reset its value to the
			 *	original one, and then track the
			 *	deletion.
			 */
			edit_undo(e);

			/*
			 *	Rewrite the edit to be delete.
			 *
			 *	And move the deletion to the tail of
			 *	the edit list, because edits between
			 *	"here" and the tail of the list may
			 *	refer to "vp".  If we leave the
			 *	deletion in place, then subsequent
			 *	edit list entries will refer to a VP
			 *	which has been deleted!
			 */
			e->op = FR_EDIT_DELETE;
			fr_dlist_remove(&el->list, e);
			goto delete;
		}
	} /* loop over existing edits */

	/*
	 *	No edit for this pair exists.  Create a new edit entry.
	 */
	e = talloc_zero(el, fr_edit_t);
	if (!e) return -1;

	e->op = op;
	e->vp = vp;

	switch (op) {
	case FR_EDIT_INVALID:
		talloc_free(e);
		return -1;

	case FR_EDIT_VALUE:
		fr_assert(list == NULL);
		fr_assert(ref == NULL);

		fr_assert(fr_type_is_leaf(vp->vp_type));
		fr_value_box_copy_shallow(NULL, &e->data, &vp->data);
		if (!fr_type_is_fixed_size(vp->vp_type)) fr_value_box_memdup_shallow(&vp->data, vp->data.enumv,
										     e->data.vb_octets, e->data.vb_length,
										     e->data.tainted);
		break;

	case FR_EDIT_CLEAR:
		fr_assert(list == NULL);
		fr_assert(ref == NULL);

		fr_assert(fr_type_is_structural(vp->vp_type));
		fr_pair_list_init(&e->children);
		fr_pair_list_append(&e->children, &vp->vp_group);
		break;

	case FR_EDIT_INSERT:
		fr_assert(list != NULL);

		/*
		 *	There's no need to record "prev".  On undo, we
		 *	just delete this pair from the list.
		 */
		e->list = list;
		fr_pair_insert_after(list, ref, vp);
		break;

	case FR_EDIT_DELETE:
	delete:
		fr_assert(list != NULL);
		fr_assert(ref == NULL);

		e->list = list;
		e->ref = fr_pair_list_prev(list, vp);

		fr_pair_remove(list, vp);
		break;
	}

	fr_dlist_insert_tail(&el->list, e);
	return 0;
}


/** Insert a new VP after an existing one.
 *
 *  This function mirrors fr_pair_insert_after().
 *
 *  After this function returns, the new VP has been inserted into the
 *  list.
 */
int fr_edit_list_insert_after(fr_edit_list_t *el, fr_pair_list_t *list, fr_pair_t *pos, fr_pair_t *vp)
{
	if (!el) return 0;

	return edit_record(el, FR_EDIT_INSERT, vp, list, pos);
}


/** Delete a VP
 *
 *  This function mirrors fr_pair_delete()
 *
 *  After this function returns, the VP has been removed from the list.
 */
int fr_edit_list_delete(fr_edit_list_t *el, fr_pair_list_t *list, fr_pair_t *vp)
{
	return edit_record(el, FR_EDIT_DELETE, vp, list, NULL);
}

/** Record the value of a leaf #fr_value_box_t
 *
 *  After this function returns, it's safe to edit the pair.
 */
int fr_edit_list_save_value(fr_edit_list_t *el, fr_pair_t *vp)
{
	if (!el) return 0;

	if (!fr_type_is_leaf(vp->vp_type)) return -1;

	return edit_record(el, FR_EDIT_VALUE, vp, NULL, NULL);
}

/** Write a new value to the #fr_value_box_t
 *
 *  After this function returns, the value has been updated.
 */
int fr_edit_list_replace_value(fr_edit_list_t *el, fr_pair_t *vp, fr_value_box_t *box)
{
	if (!el) return 0;

	if (!fr_type_is_leaf(vp->vp_type)) return -1;

	if (edit_record(el, FR_EDIT_VALUE, vp, NULL, NULL) < 0) return -1;

	if (!fr_type_is_fixed_size(vp->vp_type)) fr_value_box_clear(&vp->data);
	fr_value_box_copy_shallow(NULL, &vp->data, box);
	return 0;
}

/** Replace a pair with another one.
 *
 *  This function mirrors fr_pair_replace().
 *
 *  After this function returns, the new VP has replaced the old one,
 *  and the new one can be edited.
 */
int fr_edit_list_replace(fr_edit_list_t *el, fr_pair_list_t *list, fr_pair_t *to_replace, fr_pair_t *vp)
{
	if (!el) return 0;

	if (to_replace->da != vp->da) return -1;

	/*
	 *	We call edit_record() twice, which involves two
	 *	complete passes over the edit list.  That's fine,
	 *	either the edit list is small, OR we will eventially
	 *	put the VPs to be edited into an RB tree.
	 */
	if (edit_record(el, FR_EDIT_INSERT, vp, list, to_replace) < 0) return -1;

	/*
	 *	If deleting the old entry fails, then the new entry
	 *	above MUST be the last member of the edit list.  If
	 *	it's not the last member, then it means that it
	 *	already existed in the list (either VP list of edit
	 *	list).  The edit_record() function checks for that,
	 *	and errors if so.
	 */
	if (edit_record(el, FR_EDIT_DELETE, to_replace, list, NULL) < 0) {
		fr_edit_t *e = fr_dlist_pop_tail(&el->list);

		fr_assert(e != NULL);
		fr_assert(e->vp == vp);
		talloc_free(e);
		return -1;
	}

	return 0;
}


/** Free children of a structural pair.
 *
 *  This function mirrors fr_pair_replace().
 *
 *  After this function returns, the new VP has replaced the old one,
 *  and the new one can be edited.
 */
int fr_edit_list_free_children(fr_edit_list_t *el, fr_pair_t *vp)
{
	if (!el) return 0;

	if (!fr_type_is_structural(vp->vp_type)) return -1;

	/*
	 *	Record the list, even if it's empty.  That way if we
	 *	later add children to it, the "undo" operation can
	 *	reset the children list to be empty.
	 */
	return edit_record(el, FR_EDIT_CLEAR, vp, NULL, NULL);
}

/** Insert a list after a particular point in another list.
 *
 *  This function mirrors fr_pair_list_append(), but with a bit more
 *  control over where the to_insert list ends up.
 *
 *  There's nothing magical about this function, it's just easier to
 *  have it here than in multiple places in the code.
 */
int fr_edit_list_insert_list_after(fr_edit_list_t *el, fr_pair_list_t *list, fr_pair_t *pos, fr_pair_list_t *to_insert)
{
	fr_pair_t *prev, *vp;

	if (!el) return 0;

	prev = pos;

	/*
	 *	We have to record each individual insert as a separate
	 *	item.  Some later edit may insert pairs in the middle
	 *	of the ones we've added.
	 */
	while ((vp = fr_pair_list_head(to_insert)) != NULL) {
		(void) fr_pair_remove(to_insert, vp);

		if (edit_record(el, FR_EDIT_INSERT, vp, list, prev) < 0) {
			fr_pair_prepend(to_insert, vp); /* don't lose it! */
			return -1;
		}

		prev = vp;
	}

	return 0;
}


/** Finalize the edits when we destroy the edit list.
 *
 *  Which in large part means freeing the VPs which have been deleted,
 *  or saved, and then deleting the edit list.
 */
static int _edit_list_destructor(fr_edit_list_t *el)
{
	fr_edit_t *e;

	if (!el) return 0;

	for (e = fr_dlist_head(&el->list);
	     e != NULL;
	     e = fr_dlist_next(&el->list, e)) {
		switch (e->op) {
		case FR_EDIT_INVALID:
			fr_assert(0);
			break;

		case FR_EDIT_INSERT:
			break;

		case FR_EDIT_DELETE:
			fr_assert(e->vp != NULL);
			talloc_free(e->vp);
			break;

		case FR_EDIT_CLEAR:
			fr_pair_list_free(&e->children);
			break;

		case FR_EDIT_VALUE:
			fr_assert(fr_type_is_leaf(e->vp->vp_type));
			fr_value_box_clear(&e->data);
			break;
		}
	}

	fr_pair_list_free(&el->deleted_pairs);

	talloc_free(el);

	return 0;
}

fr_edit_list_t *fr_edit_list_alloc(TALLOC_CTX *ctx, int hint)
{
	fr_edit_list_t *el;

	el = talloc_zero_pooled_object(ctx, fr_edit_list_t, hint, hint * sizeof(fr_edit_t));
	if (!el) return NULL;

	fr_dlist_init(&el->list, fr_edit_t, entry);

	fr_pair_list_init(&el->deleted_pairs);

	talloc_set_destructor(el, _edit_list_destructor);

	return el;
}

/** Notes
 *
 *  Unlike "update" sections, edits are _not_ hierarchical.  If we're
 *  editing values a list, then the list has to exist.  If we're
 *  inserting pairs in a list, then we find the lowest existing pair,
 *  and add pairs there.
 *
 *  The functions tmpl_extents_find() and tmpl_extents_build_to_leaf()
 *  should help us figure out where the VPs exist or not.
 *
 *  The overall "update" algorithm is now:
 *
 *	alloc(edit list)
 *
 *	foreach entry in the things to do
 *		expand LHS if needed to local TMPL
 *		expand RHS if needed to local box / cursor / TMPL
 *
 *		use LHS/RHS cursors to find VPs
 *		edit VPs, recording edits
 *
 *	free temporary map
 *	commit(edit list)
 */
