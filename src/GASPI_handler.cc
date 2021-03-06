/*  Copyright 2014 - UVSQ, Dassault Aviation
    Authors list: Loïc Thébault, Eric Petit

    This file is part of Mini-FEM.

    Mini-FEM is free software: you can redistribute it and/or modify it under the terms
    of the GNU Lesser General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later version.

    Mini-FEM is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
    PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License along with
    Mini-FEM. If not, see <http://www.gnu.org/licenses/>. */

#ifdef GASPI

#include "globals.h"
#include "GASPI_handler.h"

// Free the destination offset array, flush the GASPI queue & free the segments
void GASPI_finalize (int *intfDestIndex, int nbBlocks, int rank,
                     gaspi_segment_id_t srcDataSegmentID,
                     gaspi_segment_id_t destDataSegmentID,
                     gaspi_segment_id_t srcOffsetSegmentID,
                     gaspi_segment_id_t destOffsetSegmentID,
                     gaspi_queue_id_t queueID)
{
    // If there is only one domain, do nothing
    if (nbBlocks < 2) return;
    delete[] intfDestIndex;

    SUCCESS_OR_DIE (gaspi_wait (queueID, GASPI_BLOCK));
    SUCCESS_OR_DIE (gaspi_barrier (GASPI_GROUP_ALL, GASPI_BLOCK));
    SUCCESS_OR_DIE (gaspi_segment_delete (srcDataSegmentID));
    SUCCESS_OR_DIE (gaspi_segment_delete (destDataSegmentID));
    SUCCESS_OR_DIE (gaspi_segment_delete (srcOffsetSegmentID));
    SUCCESS_OR_DIE (gaspi_segment_delete (destOffsetSegmentID));
    SUCCESS_OR_DIE (gaspi_proc_term (GASPI_BLOCK));
}

// Waits until given queue is empty if it's at least half full
void GASPI_wait_for_queue_half_full (gaspi_queue_id_t queueID)
{
    gaspi_number_t queueSizeMax;
    gaspi_number_t queueSize;

    SUCCESS_OR_DIE (gaspi_queue_size_max (&queueSizeMax));
    SUCCESS_OR_DIE (gaspi_queue_size (queueID, &queueSize));

    if (queueSize >= queueSizeMax/2) {
        SUCCESS_OR_DIE (gaspi_wait (queueID, GASPI_BLOCK));
    }
}

// Get the max number of communications
void GASPI_max_nb_communications (int *nbDCcomm, int *globalMax, int nbIntf,
                                  int nbBlocks, int rank)
{
    // If there is only one domain, do nothing
    if (nbBlocks < 2) return;

    // Get the max number of communications of local process
    int localMax = 0;
    for (int i = 0; i < nbIntf; i++) {
        if (nbDCcomm[i] > localMax) localMax = nbDCcomm[i];
    }

    // Get the max number of communications for all process
    SUCCESS_OR_DIE (gaspi_allreduce (&localMax, globalMax, 1, GASPI_OP_MAX,
                                     GASPI_TYPE_INT, GASPI_GROUP_ALL, GASPI_BLOCK));
}

// Get the number of notifications coming from adjacent domains
void GASPI_nb_notifications_exchange (int *neighborsList, int *nbDCcomm,
                                      int *nbNotifications, int nbIntf, int nbBlocks,
                                      int rank, gaspi_segment_id_t destOffsetSegmentID,
                                      gaspi_queue_id_t queueID)
{
    // If there is only one domain, do nothing
    if (nbBlocks < 2) return;

    // For each interface, send the number of notifications
    for (int i = 0; i < nbIntf; i++) {
        // The +1 is required since a notification value cannot be equal to 0...
        SUCCESS_OR_DIE (gaspi_notify (destOffsetSegmentID, neighborsList[i]-1, rank,
                                      nbDCcomm[i]+1, queueID, GASPI_BLOCK));
    }

    // For each interface, receive the number of notifications coming from adjacent
    // domain
    for (int i = 0; i < nbIntf; i++) {
        gaspi_notification_t notifyValue;
        gaspi_notification_id_t notifyID;
        SUCCESS_OR_DIE (gaspi_notify_waitsome (destOffsetSegmentID, neighborsList[i]-1,
                                               1, &notifyID, GASPI_BLOCK));
        SUCCESS_OR_DIE (gaspi_notify_reset (destOffsetSegmentID, notifyID,
                                            &notifyValue));

        // Remove the +1 of the local offset
        (*nbNotifications) += (notifyValue - 1);
    }

    // Ensure that number of notifications are received by all
    SUCCESS_OR_DIE (gaspi_barrier (GASPI_GROUP_ALL, GASPI_BLOCK));
}

// Get the adjacent domains destination offset
void GASPI_offset_exchange (int *intfDestIndex, int *intfIndex, int *neighborsList,
                            int nbIntf, int nbBlocks, int rank,
                            gaspi_segment_id_t destOffsetSegmentID,
                            gaspi_queue_id_t queueID)
{
    // If there is only one domain, do nothing
    if (nbBlocks < 2) return;

    // For each interface, send local offset to adjacent domain
    for (int i = 0; i < nbIntf; i++) {
        // The +1 is required since a notification value cannot be equal to 0...
        SUCCESS_OR_DIE (gaspi_notify (destOffsetSegmentID, neighborsList[i]-1, rank,
                                      intfIndex[i]+1, queueID, GASPI_BLOCK));
    }

    // For each interface, receive the destination offset from adjacent domain
    for (int i = 0; i < nbIntf; i++) {
        gaspi_notification_t notifyValue;
        gaspi_notification_id_t notifyID;
        SUCCESS_OR_DIE (gaspi_notify_waitsome (destOffsetSegmentID, neighborsList[i]-1,
                                               1, &notifyID, GASPI_BLOCK));
        SUCCESS_OR_DIE (gaspi_notify_reset (destOffsetSegmentID, notifyID,
                                            &notifyValue));

        // Remove the +1 of the local offset
        intfDestIndex[i] = notifyValue - 1;
    }

    // Ensure that offsets are received by all
    SUCCESS_OR_DIE (gaspi_barrier (GASPI_GROUP_ALL, GASPI_BLOCK));
}

// Initialization of the GASPI segments & creation of the segment pointers
void GASPI_init (double **srcDataSegment, double **destDataSegment,
                 int **srcOffsetSegment, int **destOffsetSegment,
                 int **intfDestIndex, int nbIntf, int nbIntfNodes, int nbBlocks,
                 int rank, int operatorDim, gaspi_segment_id_t *srcDataSegmentID,
                 gaspi_segment_id_t *destDataSegmentID,
                 gaspi_segment_id_t *srcOffsetSegmentID,
                 gaspi_segment_id_t *destOffsetSegmentID, gaspi_queue_id_t *queueID)
{
    // If there is only one domain, do nothing
    if (nbBlocks < 2) return;
    *intfDestIndex = new int [nbIntf];

    gaspi_pointer_t srcDataSegmentPtr = NULL, destDataSegmentPtr = NULL,
                  srcOffsetSegmentPtr = NULL, destOffsetSegmentPtr = NULL;
    gaspi_size_t dataSegmentSize = nbIntfNodes * operatorDim * sizeof (double),
               offsetSegmentSize = nbIntfNodes * sizeof (int);

    *srcDataSegmentID    = 0;
    *destDataSegmentID   = 1;
    *srcOffsetSegmentID  = 3;
    *destOffsetSegmentID = 4;
    *queueID             = 0;

    SUCCESS_OR_DIE (gaspi_segment_create (*srcDataSegmentID, dataSegmentSize,
                                          GASPI_GROUP_ALL, GASPI_BLOCK,
                                          GASPI_ALLOC_DEFAULT));
    SUCCESS_OR_DIE (gaspi_segment_create (*destDataSegmentID, dataSegmentSize,
                                          GASPI_GROUP_ALL, GASPI_BLOCK,
                                          GASPI_ALLOC_DEFAULT));
    SUCCESS_OR_DIE (gaspi_segment_create (*srcOffsetSegmentID, offsetSegmentSize,
                                          GASPI_GROUP_ALL, GASPI_BLOCK,
                                          GASPI_ALLOC_DEFAULT));
    SUCCESS_OR_DIE (gaspi_segment_create (*destOffsetSegmentID, offsetSegmentSize,
                                          GASPI_GROUP_ALL, GASPI_BLOCK,
                                          GASPI_ALLOC_DEFAULT));
    SUCCESS_OR_DIE (gaspi_segment_ptr (*srcDataSegmentID, &srcDataSegmentPtr));
    SUCCESS_OR_DIE (gaspi_segment_ptr (*destDataSegmentID, &destDataSegmentPtr));
    SUCCESS_OR_DIE (gaspi_segment_ptr (*srcOffsetSegmentID, &srcOffsetSegmentPtr));
    SUCCESS_OR_DIE (gaspi_segment_ptr (*destOffsetSegmentID, &destOffsetSegmentPtr));

    *srcDataSegment    = (double*)srcDataSegmentPtr;
    *destDataSegment   = (double*)destDataSegmentPtr;
    *srcOffsetSegment  = (int*)srcOffsetSegmentPtr;
    *destOffsetSegment = (int*)destOffsetSegmentPtr;
}

#endif
