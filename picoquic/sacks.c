/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include <stdlib.h>
#include <string.h>

/*
* Packet sequence recording prepares the next ACK:
*
* Maintain largest acknowledged number & the timestamp of that
* arrival used to calculate the ACK delay.
*
* Maintain the list of ACK
*/

/* Procedures to manage the list of ack ranges as a splay
 */
static void* picoquic_sack_node_value(picosplay_node_t* node)
{
    return (void*)((char*)node - offsetof(struct st_picoquic_sack_item_t, node));
}

static picoquic_sack_item_t* picoquic_sack_item_value(picosplay_node_t* node)
{
    return (node == NULL) ? NULL : (picoquic_sack_item_t*)picoquic_sack_node_value(node);
}

static int64_t picoquic_sack_item_compare(void* l, void* r) {
    int64_t delta = ((picoquic_sack_item_t*)l)->start_of_sack_range - ((picoquic_sack_item_t*)r)->start_of_sack_range;
    return delta;
}

static picosplay_node_t* picoquic_sack_node_create(void* value)
{
    return &((picoquic_sack_item_t*)value)->node;
}

static void picoquic_sack_node_delete(void* tree, picosplay_node_t* node)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(tree);
#endif
    free(picoquic_sack_node_value(node));
}

/* Return the first ACK item in the list */
picoquic_sack_item_t* picoquic_sack_first_item(picoquic_sack_list_t* sack_list)
{
    return picoquic_sack_item_value(picosplay_first(&sack_list->ack_tree));
}

picoquic_sack_item_t* picoquic_sack_last_item(picoquic_sack_list_t* sack_list)
{
    return picoquic_sack_item_value(picosplay_last(&sack_list->ack_tree));
}

picoquic_sack_item_t* picoquic_sack_next_item(picoquic_sack_item_t* sack)
{
    return picoquic_sack_item_value(picosplay_next(&sack->node));
}

picoquic_sack_item_t* picoquic_sack_previous_item(picoquic_sack_item_t* sack)
{
    return picoquic_sack_item_value(picosplay_previous(&sack->node));
}

int picoquic_sack_insert_item(picoquic_sack_list_t* sack_list, uint64_t range_min, uint64_t range_max)
{
    int ret = 0;
    picoquic_sack_item_t* sack_new = (picoquic_sack_item_t*)malloc(sizeof(picoquic_sack_item_t));
    if (sack_new == NULL) {
        ret = -1;
    }
    else
    {
        memset(sack_new, 0, sizeof(picoquic_sack_item_t));
        sack_new->start_of_sack_range = range_min;
        sack_new->end_of_sack_range = range_max;
        (void)picosplay_insert(&sack_list->ack_tree, sack_new);
    }

    return ret;
}
void picoquic_sack_delete_item(picoquic_sack_list_t* sack_list, picoquic_sack_item_t* sack)
{
    /* TODO: accounting of deleted values */
    /* Delete the item in the splay */
    picosplay_delete_hint(&sack_list->ack_tree, &sack->node);
}

/* Check whether the sack list is empty
 */
int picoquic_sack_list_is_empty(picoquic_sack_list_t* sack_list)
{
    return (sack_list->ack_tree.size == 0);
}

/* Find the sack list for the context
 */
picoquic_sack_list_t* picoquic_sack_list_from_cnx_context(picoquic_cnx_t* cnx,
    picoquic_packet_context_enum pc, picoquic_local_cnxid_t* l_cid) {
    picoquic_sack_list_t* sack_list = (pc == picoquic_packet_context_application && cnx->is_multipath_enabled) ?
        ((l_cid == NULL) ? &cnx->path[0]->p_local_cnxid->ack_ctx.sack_list :
            &l_cid->ack_ctx.sack_list) : &cnx->ack_ctx[pc].sack_list;
    return sack_list;
}

/* Find the closest range below an optional specified sack item
 */
picoquic_sack_item_t* picoquic_sack_find_range_below_number(picoquic_sack_list_t* sack_list, picoquic_sack_item_t* previous,
    uint64_t pn64)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(previous);
#endif
    picoquic_sack_item_t v = { 0 };
    v.start_of_sack_range = pn64;
    v.end_of_sack_range = pn64;
    return(picoquic_sack_item_value(picosplay_find_previous(&sack_list->ack_tree, &v)));
}
/*
 * Check whether the packet was already received.
 */
int picoquic_is_pn_already_received(picoquic_cnx_t* cnx, 
    picoquic_packet_context_enum pc, picoquic_local_cnxid_t * l_cid, uint64_t pn64)
{
    picoquic_sack_list_t* sack_list = picoquic_sack_list_from_cnx_context(cnx, pc, l_cid);
    picoquic_sack_item_t* sack_found = picoquic_sack_find_range_below_number(sack_list, NULL, pn64);
    int is_received = (sack_found != NULL && pn64 <= sack_found->end_of_sack_range);
    return is_received;
}

/*
 * Packet was already received and checksum, etc. was properly verified.
 * Record it in the chain. We do "in place" changes carefully,
 * in order to not mess up the ordered list.
 */

int picoquic_update_sack_list(picoquic_sack_list_t* sack_list,
    uint64_t pn64_min, uint64_t pn64_max)
{
    int ret = 1; /* duplicate by default, reset to 0 if update found */
    picoquic_sack_item_t* previous = picoquic_sack_find_range_below_number(sack_list, NULL, pn64_min);

    if (previous == NULL || previous->end_of_sack_range + 1 < pn64_min) {
        /* No overlap with a range below */
        picoquic_sack_item_t* next = (previous == NULL) ?
            picoquic_sack_first_item(sack_list) : picoquic_sack_next_item(previous);
        if (next == NULL || next->start_of_sack_range - 1 > pn64_max) {
            /* create a new item in the list */
            ret = picoquic_sack_insert_item(sack_list, pn64_min, pn64_max);
            /* set previous to null to bypass the next block */
            previous = NULL;
        }
        else {
            /* extend the existing item towards the min. */
            next->start_of_sack_range = pn64_min;
            /* record that this item was modified. */
            next->nb_times_sent = 0;
            ret = 0;
            /* set previous to next and do the extension part */
            previous = next;
        }
    }
    while (previous != NULL && previous->end_of_sack_range < pn64_max) {
        /* we found or created an item that includes the beginning
         * of the acked range. Check the next one */
        picoquic_sack_item_t* next = picoquic_sack_next_item(previous);
        if (next == NULL || next->start_of_sack_range - 1 > pn64_max) {
            /* No overlap. Extend the previous item up to the max of the range */
            previous->end_of_sack_range = pn64_max;
            /* record that this item was modified. */
            previous->nb_times_sent = 0;
            ret = 0;
        }
        else {
            /* Overlap. */
            /* Extend the range of the previous item to include the next one. */
            previous->end_of_sack_range = next->end_of_sack_range;
            /* record that this item was modified. */
            previous->nb_times_sent = 0;
            ret = 0;
            /* Delete the next item, accounting of ack times, etc. */
            picoquic_sack_delete_item(sack_list, next);
        }
    }

    return ret;
}

int picoquic_record_pn_received(picoquic_cnx_t* cnx,
    picoquic_packet_context_enum pc,
    picoquic_local_cnxid_t * l_cid,
    uint64_t pn64, uint64_t current_microsec)
{
    int ret = 0;
    picoquic_sack_list_t* sack_list = picoquic_sack_list_from_cnx_context(cnx, pc, l_cid);

    if (picoquic_sack_list_is_empty(sack_list)) {
        /* This is the first packet ever received.. */
        cnx->ack_ctx[pc].time_stamp_largest_received = current_microsec;
    }
    else {
        uint64_t pn_last = picoquic_sack_list_last(sack_list);
        if (pn64 > pn_last) {
            if (pn64 > pn_last + 1) {
                cnx->ack_ctx[pc].out_of_order_received = 1;
            }
            cnx->ack_ctx[pc].time_stamp_largest_received = current_microsec;
        }
        else if (cnx->ack_ctx[pc].ack_needed && pn64 < cnx->ack_ctx[pc].highest_ack_sent) {
            cnx->ack_ctx[pc].out_of_order_received = 1;
        }
    }

    ret = picoquic_update_sack_list(sack_list, pn64, pn64);
    return ret;
}

/*
 * Check whether the data fills a hole. returns 0 if it does, -1 otherwise.
 */
int picoquic_check_sack_list(picoquic_sack_list_t* sack_list,
    uint64_t pn64_min, uint64_t pn64_max)
{
    int ret = 0;
    picoquic_sack_item_t* sack = picoquic_sack_find_range_below_number(sack_list, NULL, pn64_min);

    if (sack != NULL) {
        if (pn64_max <= sack->end_of_sack_range) {
            ret = -1;
        }
    }
    return ret;
}

/* Process acknowledgement of an acknowledgement. Mark the corresponding
 * ranges as "already acknowledged" so they do not need to be resent.
 * We request complete overlap to register a match.
 */

picoquic_sack_item_t* picoquic_process_ack_of_ack_range(picoquic_sack_list_t* sack_list, picoquic_sack_item_t* previous,
    uint64_t start_of_range, uint64_t end_of_range)
{
    /* Find if the range is inside the tree */
    previous = picoquic_sack_find_range_below_number(sack_list, NULL, start_of_range);

    if (previous != NULL && previous->start_of_sack_range == start_of_range){
        picoquic_sack_item_t* next = picoquic_sack_item_value(picosplay_next(&previous->node));
        if (next == NULL) {
            /* Matching the highest range, which shall not be deleted */
            if (end_of_range < previous->end_of_sack_range) {
                previous->start_of_sack_range = end_of_range + 1;
            }
            else {
                previous->start_of_sack_range = previous->end_of_sack_range;
            }
        }
        else if (previous->end_of_sack_range == end_of_range) {
            /* Matching ACK */
            picoquic_sack_delete_item(sack_list, previous);
        }
    }

    return previous;
}

/* Return the first element of a sack list */
uint64_t picoquic_sack_list_first(picoquic_sack_list_t* sack_list)
{
    picoquic_sack_item_t* first = picoquic_sack_first_item(sack_list);
    return (first == NULL)? UINT64_MAX:first->start_of_sack_range;
}

/* Return the last element in a sack list, or UINT64_MAX if the list is empty.
 */
uint64_t picoquic_sack_list_last(picoquic_sack_list_t* sack_list)
{
    picoquic_sack_item_t* last = picoquic_sack_last_item(sack_list);
    return (last == NULL) ? 0 : last->end_of_sack_range;
}

/* Return the first range in the sack list
 */
picoquic_sack_item_t * picoquic_sack_list_first_range(picoquic_sack_list_t* sack_list)
{
    picoquic_sack_item_t* first = picoquic_sack_first_item(sack_list);
    return(first == NULL) ? NULL : picoquic_sack_item_value(picosplay_next(&first->node));
}

/* Initialize a sack list
 */
int picoquic_sack_list_init(picoquic_sack_list_t* sack_list)
{
    int ret = 0;

    memset(sack_list, 0, sizeof(picoquic_sack_list_t));
    picosplay_init_tree(&sack_list->ack_tree, picoquic_sack_item_compare,
        picoquic_sack_node_create, picoquic_sack_node_delete, picoquic_sack_node_value);

    return ret;
}

/* Reset a SACK list to single range
 */
int picoquic_sack_list_reset(picoquic_sack_list_t* sack_list, uint64_t range_min, uint64_t range_max)
{
    int ret = 0;
    picoquic_sack_list_free(sack_list);
    ret = picoquic_sack_insert_item(sack_list, range_min, range_max);
    return ret;
}

/* Free the elements of a sack list 
 */
void picoquic_sack_list_free(picoquic_sack_list_t* sack_list)
{
    picosplay_empty_tree(&sack_list->ack_tree);
}

/* Access to the elements in sack item
 */
uint64_t picoquic_sack_item_range_start(picoquic_sack_item_t* sack_item)
{
    return sack_item->start_of_sack_range;
}

uint64_t picoquic_sack_item_range_end(picoquic_sack_item_t* sack_item)
{
    return sack_item->end_of_sack_range;
}

int picoquic_sack_item_nb_times_sent(picoquic_sack_item_t* sack_item)
{
    return sack_item->nb_times_sent;
}

void picoquic_sack_item_record_sent(picoquic_sack_item_t* sack_item)
{
    sack_item->nb_times_sent++;
}

size_t picoquic_sack_list_size(picoquic_sack_list_t* sack_list)
{
    return (size_t)sack_list->ack_tree.size;
}