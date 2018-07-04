#!/bin/bash
echo "===================================================================="
echo " BUILDING DTB ..."
echo "===================================================================="
#sh build_dtb.sh
sh build_dtb.sh
echo "===================================================================="
echo " BUILDING KERNEL ..."
echo "===================================================================="
sh build_kernel.sh
ROOTDIR=$(pwd)
cp $ROOTDIR/out_zImage/temp/ToxicKernel.zip $ROOTDIR
echo "===================================================================="
echo " BUILDING COMPLETE, ToxicKernel.zip should be in root folder now "
echo "===================================================================="

