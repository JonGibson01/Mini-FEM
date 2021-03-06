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

#ifdef XMPI
    #include <mpi.h>
#elif GASPI
    #include <GASPI.h>
    #include "GASPI_handler.h"
#endif
#include <iostream>
#include <iomanip>
#include <DC.h>

#include "globals.h"
#include "FEM.h"
#include "matrix.h"
#include "coloring.h"
#include "IO.h"

// External Fortran functions
extern "C" {
    void dqmrd4_ (int *nbNodes, int *boundNodesCode, int *nbBoundNodes,
                  int *boundNodesList, int *error);
    void e_essbcm_(int *dimNode, int *nbNodes, int *nbBoundNodes, int *boundNodesList,
                   int *boundNodesCode, int *checkBounds);
}

// Global variables
string meshName, operatorName;
int *colorToElem = nullptr;
int nbTotalColors;
//int MAX_ELEM_PER_PART = strtol (getenv ("elemPerPart"), nullptr, 0);

// Help message
void help () {
	cerr << "Please specify:\n"
		 << " 1. The test case: LM6, EIB or FGN.\n"
		 << " 2. The operator: lap or ela.\n"
		 << " 3. The number of iterations.\n";
}

// Check arguments (test case, operator & number of iterations)
void check_args (int argCount, char **argValue, int *nbIter, int rank)
{
    if (argCount < 4) {
        if (rank == 0) help ();
        exit (EXIT_FAILURE);
    }
    meshName = argValue[1];
    if (meshName.compare ("LM6") && meshName.compare ("EIB") &&
        meshName.compare ("FGN")) {
        if (rank == 0) {
		    cerr << "Incorrect argument \"" << meshName << "\".\n";
		    help ();
        }
		exit (EXIT_FAILURE);
	}
	operatorName = argValue[2];
	if (operatorName.compare ("lap") && operatorName.compare ("ela")) {
        if (rank == 0) {
		    cerr << "Incorrect argument \"" << operatorName << "\".\n";
		    help ();
        }
		exit (EXIT_FAILURE);
	}
	*nbIter = strtol (argValue[3], nullptr, 0);
	if (*nbIter < 1) {
        if (rank == 0) cerr << "Number of iterations must be at least 1.\n";
		exit (EXIT_FAILURE);
	}

    if (rank == 0) {
        cout << "\t\t* Mini-FEM *\n\n"
            << "Test case              : \"" << meshName << "\"\n"
            << "Operator               : \"" << operatorName << "\"\n"
            << "Elements per partition :  "  << MAX_ELEM_PER_PART << "\n"
            << "Iterations             :  "  << *nbIter << "\n\n"
            << scientific << setprecision (1);
    }
}

int main (int argCount, char **argValue)
{
    // Process initialization
    int nbBlocks = 0, rank = 0;
    #ifdef XMPI
        MPI_Init (&argCount, &argValue);
        MPI_Comm_size (MPI_COMM_WORLD, &nbBlocks);
        MPI_Comm_rank (MPI_COMM_WORLD, &rank);
    #elif GASPI
        SUCCESS_OR_DIE (gaspi_proc_init (GASPI_BLOCK));
        SUCCESS_OR_DIE (gaspi_proc_num ((gaspi_rank_t*)&nbBlocks));
        SUCCESS_OR_DIE (gaspi_proc_rank ((gaspi_rank_t*)&rank));
    #endif

    // Declarations
    DC_timer timer;
    index_t nodeToElem;
    double *coord = nullptr, *nodeToNodeValue = nullptr, *prec = nullptr;
    int *nodeToNodeRow = nullptr, *nodeToNodeColumn = nullptr, *elemToNode = nullptr,
        *intfIndex = nullptr, *intfNodes = nullptr, *intfDestIndex = nullptr,
        *neighborsList = nullptr, *boundNodesCode = nullptr, *boundNodesList = nullptr,
        *checkBounds = nullptr, *elemToEdge = nullptr;
    int nbElem, nbNodes, nbEdges, nbIntf, nbIntfNodes, nbDispNodes,
        nbBoundNodes, operatorDim, operatorID, nbIter, error, nbNotifications = 0,
        nbMaxComm = 0;

    // Arguments initialization
    check_args (argCount, argValue, &nbIter, rank);

    // Set the operator dimension & ID
    if (!operatorName.compare ("lap")) {
        operatorDim = 1;
        operatorID  = 0;
    }
    else {
        operatorDim = DIM_NODE * DIM_NODE;
        operatorID  = 1;
    }

    // Get the input data from DefMesh
    if (rank == 0) {
        cout << "Reading input data...                ";
        timer.start_time ();
    }
    read_input_data (&coord, &elemToNode, &neighborsList, &intfIndex, &intfNodes,
                     &boundNodesCode, &nbElem, &nbNodes, &nbEdges, &nbIntf,
                     &nbIntfNodes, &nbDispNodes, &nbBoundNodes, nbBlocks, rank);
    if (rank == 0) {
        timer.stop_time ();
        cout << "done  (" << timer.get_avg_time () << " seconds)\n";
        timer.reset_time ();
    }

    // D&C versions
    #if defined (DC) || defined (DC_VEC)

        // Set the path to the D&C tree and permutations
        string treePath = (string)DATA_PATH + "/" + meshName + "/DC_tree/"
                          + to_string ((long long)MAX_ELEM_PER_PART) + "_"
                          + to_string ((long long)nbBlocks) + "_"
                          + to_string ((long long)rank);

        // Creation of the D&C tree and permutations
        #ifdef TREE_CREATION
            if (rank == 0) {
                cout << "Creation of the D&C tree...          ";
                timer.start_time ();
            }
            DC_create_tree (elemToNode, nbElem, DIM_ELEM, nbNodes);
            if (rank == 0) {
                timer.stop_time ();
            	cout << "done  (" << timer.get_avg_time () << " seconds)\n";
                timer.reset_time ();
            }

        // Reading of the D&C tree and permutations
        #else
            if (rank == 0) {
                cout << "Reading the D&C tree...              ";
                timer.start_time ();
            }
            DC_read_tree (treePath, nbElem, nbNodes, nbIntf, &nbNotifications,
                          &nbMaxComm);
            if (rank == 0) {
                timer.stop_time ();
            	cout << "done  (" << timer.get_avg_time () << " seconds)\n";
                timer.reset_time ();
            }
        #endif

        // Apply permutations
        if (rank == 0) {
            cout << "Applying permutation...              ";
            timer.start_time ();
        }
        DC_permute_double_2d_array (coord, nbNodes, DIM_NODE);
        #ifndef TREE_CREATION
            DC_permute_int_2d_array (elemToNode, nullptr, nbElem, DIM_ELEM, 0);
        #endif
        DC_renumber_int_array (elemToNode, nbElem * DIM_ELEM, true);
        DC_renumber_int_array (intfNodes, nbIntfNodes, true);
        DC_permute_int_1d_array (boundNodesCode, nbNodes);
        if (rank == 0) {
            timer.stop_time ();
            cout << "done  (" << timer.get_avg_time () << " seconds)\n";
            timer.reset_time ();
        }

    // Mesh coloring version
    #elif COLORING

        // Create the coloring
        if (rank == 0) {
            cout << "Coloring of the mesh...              ";
            timer.start_time ();
        }
        int *colorPerm = new int [nbElem];
        coloring_creation (elemToNode, colorPerm, nbElem, nbNodes);
        if (rank == 0) {
            timer.stop_time ();
            cout << "done  (" << timer.get_avg_time () << " seconds)\n";
            timer.reset_time ();
        }

        // Apply the element permutation
        if (rank == 0) {
            cout << "Applying permutation...              ";
            timer.start_time ();
        }
        DC_permute_int_2d_array (elemToNode, colorPerm, nbElem, DIM_ELEM, 0);
        delete[] colorPerm;
        if (rank == 0) {
            timer.stop_time ();
            cout << "done  (" << timer.get_avg_time () << " seconds)\n";
            timer.reset_time ();
        }
    #endif

    // Create the CSR matrix
    if (rank == 0) {
        cout << "Creating CSR matrix...               ";
        timer.start_time ();
    }
    nodeToElem.index = new int [nbNodes + 1];
    nodeToElem.value = new int [nbElem * DIM_ELEM];
    nodeToNodeRow    = new int [nbNodes + 1];
    nodeToNodeColumn = new int [nbEdges];
    DC_create_nodeToElem (nodeToElem, elemToNode, nbElem, DIM_ELEM, nbNodes);
    create_nodeToNode (nodeToNodeRow, nodeToNodeColumn, nodeToElem, elemToNode,
                       nbNodes);
    delete[] nodeToElem.value, delete[] nodeToElem.index;
    if (rank == 0) {
        timer.stop_time ();
    	cout << "done  (" << timer.get_avg_time () << " seconds)\n";
        timer.reset_time ();
    }

    // Initialization of the GASPI library
    #ifdef GASPI
        if (rank == 0) {
            cout << "Initializing GASPI lib...            ";
            timer.start_time ();
        }
        double *srcDataSegment = nullptr,   *destDataSegment = nullptr;
        int  *srcOffsetSegment = nullptr, *destOffsetSegment = nullptr;
        gaspi_segment_id_t srcDataSegmentID, destDataSegmentID,
                         srcOffsetSegmentID, destOffsetSegmentID;
        gaspi_queue_id_t queueID;
        GASPI_init (&srcDataSegment, &destDataSegment, &srcOffsetSegment,
                    &destOffsetSegment, &intfDestIndex, nbIntf, nbIntfNodes,
                    nbBlocks, rank, operatorDim, &srcDataSegmentID, &destDataSegmentID,
                    &srcOffsetSegmentID, &destOffsetSegmentID, &queueID);
        GASPI_offset_exchange (intfDestIndex, intfIndex, neighborsList, nbIntf,
                               nbBlocks, rank, destOffsetSegmentID, queueID);
        if (rank == 0) {
            timer.stop_time ();
            cout << "done  (" << timer.get_avg_time () << " seconds)\n";
            timer.reset_time ();
        }
    #endif

    // Finalize and store the D&C tree
    #if (defined (DC) || defined (DC_VEC)) && defined (TREE_CREATION)
        if (rank == 0) {
            cout << "Finalizing the D&C tree...           ";
            timer.start_time ();
        }
        int *nbDCcomm = nullptr;
        #ifdef MULTITHREADED_COMM
            nbDCcomm = new int [nbIntf] ();
        #endif
        DC_finalize_tree (nodeToNodeRow, elemToNode, intfIndex, intfNodes,
                          intfDestIndex, nbDCcomm, nbElem, DIM_ELEM, nbBlocks,
                          nbIntf, rank);
        #ifdef MULTITHREADED_COMM
            GASPI_nb_notifications_exchange (neighborsList, nbDCcomm, &nbNotifications,
                                             nbIntf, nbBlocks, rank,
                                             destOffsetSegmentID, queueID);
            GASPI_max_nb_communications (nbDCcomm, &nbMaxComm, nbIntf, nbBlocks, rank);
            delete[] nbDCcomm;
        #endif
        if (rank == 0) {
            timer.stop_time ();
            cout << "done  (" << timer.get_avg_time () << " seconds)\n";
            timer.reset_time ();
            cout << "Storing the D&C tree...              ";
            timer.start_time ();
        }
        DC_store_tree (treePath, nbElem, nbNodes, nbIntf, nbNotifications, nbMaxComm);
        if (rank == 0) {
            timer.stop_time ();
            cout << "done  (" << timer.get_avg_time () << " seconds)\n";
            timer.reset_time ();
        }
    #endif

    // Compute the index of each edge of each element
    #ifdef OPTIMIZED
        if (rank == 0) {
            cout << "Computing edges index...             ";
            timer.start_time ();
        }
        elemToEdge = new int [nbElem * VALUES_PER_ELEM];
        create_elemToEdge (nodeToNodeRow, nodeToNodeColumn, elemToNode, elemToEdge,
                          nbElem);
        if (rank == 0) {
            timer.stop_time ();
    	    cout << "done  (" << timer.get_avg_time () << " seconds)\n";
            timer.reset_time ();
        }
    #endif

    // Compute the boundary conditions
    if (rank == 0) {
        cout << "Computing boundary conditions...     ";
        timer.start_time ();
    }
    int dimNode = DIM_NODE;
    boundNodesList = new int [nbBoundNodes];
    checkBounds    = new int [nbNodes * DIM_NODE];
    dqmrd4_ (&nbNodes, boundNodesCode, &nbBoundNodes, boundNodesList, &error);
    e_essbcm_ (&dimNode, &nbNodes, &nbBoundNodes, boundNodesList, boundNodesCode,
               checkBounds);
    delete[] boundNodesList, delete[] boundNodesCode;
    if (rank == 0) {
        timer.stop_time ();
        cout << "done  (" << timer.get_avg_time () << " seconds)\n";
        timer.reset_time ();
    }

    // Main loop with assembly, solver & update
    if (rank == 0) cout << "\nMain FEM loop\n";
    nodeToNodeValue = new double [nbEdges * operatorDim];
    prec            = new double [nbNodes * operatorDim];
    FEM_loop (prec, coord, nodeToNodeValue, nodeToNodeRow, nodeToNodeColumn,
              elemToNode, elemToEdge, intfIndex, intfNodes, neighborsList, checkBounds,
              nbElem, nbNodes, nbEdges, nbIntf, nbIntfNodes, nbIter, nbBlocks, rank,
    #ifdef XMPI
              operatorDim, operatorID);
    #elif GASPI
              operatorDim, operatorID, nbMaxComm, nbNotifications, srcDataSegment,
              destDataSegment, srcOffsetSegment, destOffsetSegment, intfDestIndex,
              srcDataSegmentID, destDataSegmentID, srcOffsetSegmentID,
              destOffsetSegmentID, queueID);
    #endif
    delete[] checkBounds, delete[] nodeToNodeColumn, delete[] nodeToNodeRow;
    delete[] intfNodes, delete[] intfIndex, delete[] neighborsList, delete[] coord;
    delete[] elemToNode;
    #ifdef OPTIMIZED
        delete[] elemToEdge; 
    #endif

    // Check matrix & prec arraysValue & prec arrays
    check_results (prec, nodeToNodeValue, nbEdges, nbNodes, operatorDim, nbBlocks,
                   rank);
    delete[] prec, delete[] nodeToNodeValue;

    #ifdef XMPI
        MPI_Finalize ();
    #elif GASPI
        GASPI_finalize (intfDestIndex, nbBlocks, rank, srcDataSegmentID,
                        destDataSegmentID, srcOffsetSegmentID, destOffsetSegmentID,
                        queueID);
    #endif

	return EXIT_SUCCESS;
}
