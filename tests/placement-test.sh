#!/bin/sh
#
# Basic data shuffling test for shuffle library with proper write placement
# through ch-placement.
# Arguments are optional. If not provided, we run in cwd with 1 MPI process.
#
# Argument 1: directory containing library and test executable
# Argument 2: number of MPI processes to spawn

BUILD_PREFIX="."
if [ ! -z "$1" ]; then
    BUILD_PREFIX="$1"
fi

MPI_PROCS=1
if [ ! -z "$2" ]; then
    MPI_PROCS=$2
fi

rm -Rf /tmp/pdlfs
rm -Rf /tmp/pdlfs-test
mkdir /tmp/pdlfs

#
# XXX: this assumes a SunOS/linux-style ld.so (won't work on macosx)
#
env LD_PRELOAD=$BUILD_PREFIX/src/libdeltafs-preload.so PDLFS_Placement_test=1 \
    mpirun -np $MPI_PROCS -mca btl ^openib $BUILD_PREFIX/tests/placement-test

if [ $? != 0 ]; then
    echo "Placement test failed ($?)"
    exit 1
fi

echo "Placement test successful"
exit 0