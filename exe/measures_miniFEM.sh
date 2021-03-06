#!/bin/bash

# Set the parameters
EXE_DIR=$HOME/Dassault/Mini-FEM/exe
EXE_FILE=$EXE_DIR/GASPI_exe_$NB_NODES\_$NB_PROCESS_PER_NODE.sh
MACHINE_FILE=$EXE_DIR/GASPI_machine_file_$NB_NODES\_$NB_PROCESS_PER_NODE
TEST_CASE=EIB
VECTOR_LENGTH=AVX
NB_ITERATIONS=50

# Set the Anselm environment
module load cmake/2.8.11 PrgEnv-intel/14.0.1 gpi2/1.1.1 impi/4.1.1.036
export PATH=$PATH:/apps/libs/gpi2/1.1.1/bin/

# Set the Salomon environment
#module load CMake/3.0.0-intel-2015b
#export PATH=$PATH:$HOME/Programs/GPI-2/bin

# Go to the appropriate directory, exit on failure
cd $EXE_DIR || exit

for VERSION in 'REF_BulkSynchronous' 'DC_BulkSynchronous_TreeCreation' 'DC_MultithreadedComm_TreeCreation'
do
    for DISTRI in 'XMPI' 'GASPI'
    do
        # Multithreaded comm version unavailable for MPI
        if [ $VERSION == 'DC_MultithreadedComm_TreeCreation' ] &&
           [ $DISTRI  == 'XMPI' ]; then
            continue
        fi

        for SHARED in 'CILK' #'OMP'
        do
            # Set the binary name
            BINARY=$EXE_DIR/bin/miniFEM_$VERSION\_$DISTRI\_$SHARED

            for OPERATOR in 'ela'
            do
                for PART_SIZE in 200
                do
                    export elemPerPart=$PART_SIZE

                    for NB_THREADS_PER_PROCESS in 1 4 8 12 16 24
                    do
                        # Set the number of process and threads
                        let "NB_TOTAL_PROCESS=$NB_PROCESS_PER_NODE*$NB_NODES"
                        let "NB_THREADS_PER_NODE=$NB_PROCESS_PER_NODE*\
                                                 $NB_THREADS_PER_PROCESS"
                        if [ "$NB_THREADS_PER_NODE" -gt "$NB_CORES_PER_NODE" ]; then
                            break
                        fi
                        if [ $VERSION == 'REF_BulkSynchronous' ] &&
                           [ $NB_THREADS_PER_PROCESS -gt 1 ]; then
                            break
                        fi
                        if [ $SHARED == "CILK" ]; then
                            export CILK_NWORKERS=$NB_THREADS_PER_PROCESS
                        else
                            export OMP_NUM_THREADS=$NB_THREADS_PER_PROCESS
                        fi

                        # Create the GASPI execution script and machine file
                        if [ $DISTRI == "GASPI" ]; then
                            echo "#!/bin/sh" > $EXE_FILE
                            echo "cd $EXE_DIR" >> $EXE_FILE
                            echo "export LD_LIBRARY_PATH=$LD_LIBRARY_PATH" >> $EXE_FILE
                            if [ $SHARED == "CILK" ]; then
                                echo "export CILK_NWORKERS=$NB_THREADS_PER_PROCESS" \
                                     >> $EXE_FILE
                            else
                                echo "export OMP_NUM_THREADS=$NB_THREADS_PER_PROCESS" \
                                     >> $EXE_FILE
                            fi
                            echo "$BINARY $TEST_CASE $OPERATOR $NB_ITERATIONS" \
                                 >> $EXE_FILE
                            #echo "./maqao lprof -- $BINARY $TEST_CASE $OPERATOR $NB_ITERATIONS" >> $EXE_FILE
                            chmod +x $EXE_FILE
                            cat $PBS_NODEFILE | cut -d'.' -f1 > $MACHINE_FILE
                        fi

                        # Create the output file
                        OUTPUT_FILE=$EXE_DIR/stdout_$TEST_CASE\_$VERSION\_$OPERATOR\_$PART_SIZE\_$DISTRI\_$SHARED\_$NB_NODES\_$NB_PROCESS_PER_NODE\_$NB_THREADS_PER_PROCESS

                        # Launch the job
                        if [ $DISTRI == "XMPI" ]; then
                            mpirun -np $NB_TOTAL_PROCESS $BINARY $TEST_CASE \
                                       $OPERATOR $NB_ITERATIONS > $OUTPUT_FILE
                        elif [ $DISTRI == "GASPI" ]; then
                            gaspi_run -m $MACHINE_FILE $EXE_FILE > $OUTPUT_FILE
                        fi
                    done
                done
            done
        done
    done
done

rm $MACHINE_FILE $EXE_FILE

exit
